/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#define _GNU_SOURCE
#include "config_reader.h"
#include "tx_app_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>

/* -------------------------------------------------------------------------
 * Minimal JSON helpers — operate on a bounded region [start, end).
 * All searches are confined to the region so nested objects with identical
 * key names don't bleed across object boundaries.
 * -------------------------------------------------------------------------*/

/* Extract a JSON string value for key within [start, end).
 * Returns pointer to buffer on success, NULL if key not found. */
static char* extract_json_string(const char* start, const char* end,
                                 const char* key,
                                 char* buffer, size_t buffer_size) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    size_t klen = strlen(search_key);

    const char* pos = start;
    while (pos < end) {
        pos = (const char*)memmem(pos, end - pos, search_key, klen);
        if (!pos) return NULL;
        pos += klen;
        /* skip whitespace then ':' */
        while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
        if (pos >= end || *pos != ':') continue;
        pos++;
        while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
        if (pos >= end || *pos != '"') return NULL;
        pos++; /* skip opening quote */
        const char* vend = (const char*)memchr(pos, '"', end - pos);
        if (!vend) return NULL;
        size_t vlen = vend - pos;
        if (vlen >= buffer_size) vlen = buffer_size - 1;
        memcpy(buffer, pos, vlen);
        buffer[vlen] = '\0';
        return buffer;
    }
    return NULL;
}

/* Extract a JSON integer value for key within [start, end). Returns -1 if
 * not found (callers must treat -1 as "use default"). */
static int extract_json_int(const char* start, const char* end, const char* key) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    size_t klen = strlen(search_key);

    const char* pos = start;
    while (pos < end) {
        pos = (const char*)memmem(pos, end - pos, search_key, klen);
        if (!pos) return -1;
        pos += klen;
        while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
        if (pos >= end || *pos != ':') continue;
        pos++;
        while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
        return atoi(pos);
    }
    return -1;
}

/* Find the opening '{' of the object that follows key within [start, end).
 * Returns pointer to '{', or NULL. */
static const char* find_object(const char* start, const char* end, const char* key) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    size_t klen = strlen(search_key);

    const char* pos = (const char*)memmem(start, end - start, search_key, klen);
    if (!pos) return NULL;
    pos += klen;
    while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' || *pos == ':')) pos++;
    if (pos >= end || *pos != '{') return NULL;
    return pos;
}

/* Find the closing '}' that matches the opening '{' at obj_start.
 * Returns pointer to '}', or NULL. */
static const char* find_object_end(const char* obj_start, const char* buf_end) {
    if (!obj_start || *obj_start != '{') return NULL;
    int depth = 0;
    const char* p = obj_start;
    while (p < buf_end) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) return p; }
        p++;
    }
    return NULL;
}

/* Find opening '[' of the array that follows key within [start, end). */
static const char* find_array(const char* start, const char* end, const char* key) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    size_t klen = strlen(search_key);

    const char* pos = (const char*)memmem(start, end - start, search_key, klen);
    if (!pos) return NULL;
    pos += klen;
    while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' || *pos == ':')) pos++;
    if (pos >= end || *pos != '[') return NULL;
    return pos;
}

/* Find the closing ']' that matches the opening '[' at arr_start. */
static const char* find_array_end(const char* arr_start, const char* buf_end) {
    if (!arr_start || *arr_start != '[') return NULL;
    int depth = 0;
    const char* p = arr_start;
    while (p < buf_end) {
        if (*p == '[') depth++;
        else if (*p == ']') { depth--; if (depth == 0) return p; }
        p++;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * parse_tx_config — parse the JSON config file into tx_app_config.
 *
 * Expected JSON structure:
 *   {
 *     "interfaces": [ { "name": "...", "sip": "...", "dip": "..." } ],
 *     "video": { "width": N, "height": N, "fps": N, "fmt": "...", "tx_url": "..." },
 *     "tx_sessions": [
 *       { "udp_port": N, "payload_type": N, "crop": { "x":N, "y":N, "w":N, "h":N } },
 *       ...
 *     ]
 *   }
 * -------------------------------------------------------------------------*/
int parse_tx_config(const char* config_file, struct tx_app_config* config) {
    FILE* fp = fopen(config_file, "r");
    if (!fp) {
        printf("Error: Cannot open config file %s\n", config_file);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size <= 0) { fclose(fp); return -1; }

    char* json = malloc(file_size + 1);
    if (!json) { fclose(fp); return -1; }

    fread(json, 1, file_size, fp);
    json[file_size] = '\0';
    fclose(fp);

    const char* buf_end = json + file_size;
    memset(config, 0, sizeof(*config));

    /* --- interfaces[0] --- */
    const char* ifaces_arr = find_array(json, buf_end, "interfaces");
    if (ifaces_arr) {
        const char* ifaces_end = find_array_end(ifaces_arr, buf_end);
        if (ifaces_end) {
            const char* first_brace = ifaces_arr + 1;
            while (first_brace < ifaces_end && *first_brace != '{') first_brace++;
            if (first_brace < ifaces_end) {
                const char* iface_obj = first_brace;
                const char* iface_end = find_object_end(iface_obj, ifaces_end);
                if (!iface_end) {
                    printf("Warning: interfaces[0] object not properly closed; "
                           "truncating parse at array end\n");
                    iface_end = ifaces_end;
                }
                extract_json_string(iface_obj, iface_end, "name", config->interface_name, sizeof(config->interface_name));
                extract_json_string(iface_obj, iface_end, "sip",  config->interface_sip,  sizeof(config->interface_sip));
                extract_json_string(iface_obj, iface_end, "dip",  config->interface_dip,  sizeof(config->interface_dip));
            }
        }
    }

    /* --- video block --- */
    const char* video_obj = find_object(json, buf_end, "video");
    if (video_obj) {
        const char* video_end = find_object_end(video_obj, buf_end);
        if (!video_end) video_end = buf_end;
        int v;
        v = extract_json_int(video_obj, video_end, "width");  config->width  = (v > 0) ? v : 1920;
        v = extract_json_int(video_obj, video_end, "height"); config->height = (v > 0) ? v : 1080;
        v = extract_json_int(video_obj, video_end, "fps");    config->fps    = (v > 0) ? v : 25;
        extract_json_string(video_obj, video_end, "fmt",    config->fmt,    sizeof(config->fmt));
        extract_json_string(video_obj, video_end, "tx_url", config->tx_url, sizeof(config->tx_url));
        if (config->fmt[0] == '\0') strncpy(config->fmt, "yuv422p10le", sizeof(config->fmt) - 1);
    } else {
        config->width = 1920; config->height = 1080; config->fps = 25;
        strncpy(config->fmt, "yuv422p10le", sizeof(config->fmt) - 1);
    }

    /* --- tx_sessions array --- */
    const char* sessions_arr = find_array(json, buf_end, "tx_sessions");
    if (!sessions_arr) {
        printf("Error: 'tx_sessions' not found in config\n");
        free(json);
        return -1;
    }
    const char* sessions_end = find_array_end(sessions_arr, buf_end);
    if (!sessions_end) sessions_end = buf_end;

    /* Walk the array, extracting each '{...}' object */
    const char* cursor = sessions_arr + 1; /* skip '[' */
    config->session_count = 0;

    while (cursor < sessions_end && config->session_count < MAX_TX_SESSIONS) {
        /* advance to next '{' */
        while (cursor < sessions_end && *cursor != '{') cursor++;
        if (cursor >= sessions_end) break;

        const char* sess_obj = cursor;
        const char* sess_end = find_object_end(sess_obj, sessions_end);
        if (!sess_end) break;

        struct tx_session_config* s = &config->sessions[config->session_count];
        int v;

        v = extract_json_int(sess_obj, sess_end, "udp_port");
        if (v > 0 && v <= 65535) {
            s->udp_port = (uint16_t)v;
        } else {
            if (v <= 0)
                printf("Warning: session %d: udp_port not set or invalid (%d); using default 20000\n",
                       config->session_count, v);
            else
                printf("Warning: session %d: udp_port %d exceeds 65535; using default 20000\n",
                       config->session_count, v);
            s->udp_port = 20000;
        }

        v = extract_json_int(sess_obj, sess_end, "payload_type");
        if (v > 0 && v <= 255) {
            s->payload_type = (uint8_t)v;
        } else {
            if (v <= 0)
                printf("Warning: session %d: payload_type not set or invalid (%d); using default 96\n",
                       config->session_count, v);
            else
                printf("Warning: session %d: payload_type %d exceeds 255; using default 96\n",
                       config->session_count, v);
            s->payload_type = 96;
        }

        /* crop sub-object */
        const char* crop_obj = find_object(sess_obj, sess_end, "crop");
        if (crop_obj) {
            const char* crop_end = find_object_end(crop_obj, sess_end);
            if (!crop_end) crop_end = sess_end;
            v = extract_json_int(crop_obj, crop_end, "x"); s->crop_x = (v >= 0) ? v : 0;
            v = extract_json_int(crop_obj, crop_end, "y"); s->crop_y = (v >= 0) ? v : 0;
            v = extract_json_int(crop_obj, crop_end, "w"); s->crop_w = (v > 0)  ? v : (int)config->width;
            v = extract_json_int(crop_obj, crop_end, "h"); s->crop_h = (v > 0)  ? v : (int)config->height;
        } else {
            /* No "crop" object: default to full frame.
             * For multi-session configs, each session should provide an explicit crop. */
            s->crop_x = 0; s->crop_y = 0;
            s->crop_w = config->width; s->crop_h = config->height;
        }

        config->session_count++;
        cursor = sess_end + 1;
    }

    if (config->session_count == 0) {
        printf("Error: No tx_sessions found in config\n");
        free(json);
        return -1;
    }

    free(json);
    return 0;
}

int validate_tx_config(const struct tx_app_config* config) {
    /* Interface validation */
    if (config->interface_name[0] == '\0') {
        printf("Error: interfaces[0].name is required\n");
        return -1;
    }
    if (config->interface_dip[0] == '\0') {
        printf("Error: interfaces[0].dip is required\n");
        return -1;
    }

    /* Validate IP address format (basic dotted-quad check) */
    if (config->interface_sip[0] != '\0') {
        struct in_addr tmp;
        if (inet_pton(AF_INET, config->interface_sip, &tmp) != 1) {
            printf("Error: Invalid source IP address '%s'\n", config->interface_sip);
            return -1;
        }
    }
    {
        struct in_addr tmp;
        if (inet_pton(AF_INET, config->interface_dip, &tmp) != 1) {
            printf("Error: Invalid destination IP address '%s'\n", config->interface_dip);
            return -1;
        }
    }

    /* Video resolution validation */
    if (config->width == 0 || config->height == 0) {
        printf("Error: video width/height must be non-zero\n");
        return -1;
    }
    if (config->width > 7680 || config->height > 4320) {
        printf("Error: video resolution %dx%d exceeds maximum 7680x4320\n",
               config->width, config->height);
        return -1;
    }
    if (config->width % 2 != 0) {
        printf("Error: video width %d must be even for YUV formats\n", config->width);
        return -1;
    }

    /* FPS validation */
    if (config->fps != 25 && config->fps != 30 &&
        config->fps != 50 && config->fps != 60) {
        printf("Error: unsupported fps %d (supported: 25, 30, 50, 60)\n", config->fps);
        return -1;
    }

    /* Pixel format validation */
    if (config->fmt[0] != '\0' &&
        strcmp(config->fmt, "yuv422p10le") != 0 &&
        strcmp(config->fmt, "yuv420p") != 0 &&
        strcmp(config->fmt, "yuv422p12le") != 0 &&
        strcmp(config->fmt, "yuv444p10le") != 0 &&
        strcmp(config->fmt, "yuv444p12le") != 0 &&
        strcmp(config->fmt, "gbrp10le") != 0 &&
        strcmp(config->fmt, "gbrp12le") != 0) {
        printf("Error: unsupported pixel format '%s'\n", config->fmt);
        printf("  Supported: yuv422p10le, yuv420p, yuv422p12le, yuv444p10le, "
               "yuv444p12le, gbrp10le, gbrp12le\n");
        return -1;
    }

    /* Video source file validation */
    if (config->tx_url[0] != '\0') {
        FILE* f = fopen(config->tx_url, "r");
        if (!f) {
            printf("Error: video source file not found: %s\n", config->tx_url);
            return -1;
        }
        fclose(f);
    }

    /* Session validation */
    if (config->session_count == 0) {
        printf("Error: tx_sessions array is empty\n");
        return -1;
    }

    for (int i = 0; i < config->session_count; i++) {
        const struct tx_session_config* s = &config->sessions[i];

        /* UDP port range */
        if (s->udp_port == 0) {
            printf("Error: session %d: udp_port must be non-zero\n", i);
            return -1;
        }

        /* Payload type range (RFC 3551: dynamic range 96-127) */
        if (s->payload_type < 96 || s->payload_type > 127) {
            printf("Error: session %d: payload_type %d out of range 96-127\n",
                   i, s->payload_type);
            return -1;
        }

        /* Crop bounds: must fit within the source video */
        if (s->crop_x < 0 || s->crop_y < 0 || s->crop_w <= 0 || s->crop_h <= 0) {
            printf("Error: session %d: crop values must be positive "
                   "(x=%d y=%d w=%d h=%d)\n", i, s->crop_x, s->crop_y,
                   s->crop_w, s->crop_h);
            return -1;
        }
        if ((uint32_t)(s->crop_x + s->crop_w) > config->width) {
            printf("Error: session %d: crop x=%d + w=%d = %d exceeds video width %d\n",
                   i, s->crop_x, s->crop_w, s->crop_x + s->crop_w, config->width);
            return -1;
        }
        if ((uint32_t)(s->crop_y + s->crop_h) > config->height) {
            printf("Error: session %d: crop y=%d + h=%d = %d exceeds video height %d\n",
                   i, s->crop_y, s->crop_h, s->crop_y + s->crop_h, config->height);
            return -1;
        }
        if (s->crop_w % 2 != 0) {
            printf("Error: session %d: crop width %d must be even for YUV formats\n",
                   i, s->crop_w);
            return -1;
        }

        /* Validate crop_x/y alignment for chroma-subsampled formats.
         * Use the pixel format descriptor (if available) to determine
         * the required alignment from log2_chroma_w/h. */
        {
            enum AVPixelFormat pix_fmt = av_get_pix_fmt(
                config->fmt[0] ? config->fmt : "yuv422p10le");
            const AVPixFmtDescriptor* desc =
                (pix_fmt != AV_PIX_FMT_NONE) ? av_pix_fmt_desc_get(pix_fmt) : NULL;
            int x_align = desc ? (1 << desc->log2_chroma_w) : 2;
            int y_align = desc ? (1 << desc->log2_chroma_h) : 1;

            if (x_align > 1 && s->crop_x % x_align != 0) {
                printf("Error: session %d: crop_x %d must be a multiple of %d "
                       "for pixel format '%s'\n", i, s->crop_x, x_align, config->fmt);
                return -1;
            }
            if (x_align > 1 && s->crop_w % x_align != 0) {
                printf("Error: session %d: crop_w %d must be a multiple of %d "
                       "for pixel format '%s'\n", i, s->crop_w, x_align, config->fmt);
                return -1;
            }
            if (y_align > 1 && s->crop_y % y_align != 0) {
                printf("Error: session %d: crop_y %d must be a multiple of %d "
                       "for pixel format '%s'\n", i, s->crop_y, y_align, config->fmt);
                return -1;
            }
            if (y_align > 1 && s->crop_h % y_align != 0) {
                printf("Error: session %d: crop_h %d must be a multiple of %d "
                       "for pixel format '%s'\n", i, s->crop_h, y_align, config->fmt);
                return -1;
            }
        }

        /* Check for duplicate UDP ports */
        for (int j = 0; j < i; j++) {
            if (config->sessions[j].udp_port == s->udp_port) {
                printf("Error: session %d and %d have duplicate udp_port %d\n",
                       j, i, s->udp_port);
                return -1;
            }
        }
    }

    return 0;
}

int load_and_apply_config(struct tx_app_context* app, const char* config_file) {
    if (!app || !config_file || config_file[0] == '\0')
        return 0; /* no config file — keep CLI defaults */

    struct tx_app_config config;
    if (parse_tx_config(config_file, &config) != 0) {
        printf("Warning: Failed to parse config file %s\n", config_file);
        return -1;
    }
    if (validate_tx_config(&config) != 0) {
        printf("Warning: Invalid config file %s\n", config_file);
        return -1;
    }

    /* Interface */
    strncpy(app->port, config.interface_name, sizeof(app->port) - 1);
    app->port[sizeof(app->port) - 1] = '\0';

    if (config.interface_sip[0] != '\0') {
        strncpy(app->sip_addr_str, config.interface_sip, sizeof(app->sip_addr_str) - 1);
        app->sip_addr_str[sizeof(app->sip_addr_str) - 1] = '\0';
    }

    strncpy(app->dip_addr_str, config.interface_dip, sizeof(app->dip_addr_str) - 1);
    app->dip_addr_str[sizeof(app->dip_addr_str) - 1] = '\0';

    /* Video */
    app->width  = config.width;
    app->height = config.height;
    app->fps    = config.fps;

    if (strcmp(config.fmt, "yuv422p10le") == 0)       app->fmt = AV_PIX_FMT_YUV422P10LE;
    else if (strcmp(config.fmt, "yuv420p") == 0)      app->fmt = AV_PIX_FMT_YUV420P;
    else if (strcmp(config.fmt, "yuv422p12le") == 0)  app->fmt = AV_PIX_FMT_YUV422P12LE;
    else if (strcmp(config.fmt, "yuv444p10le") == 0)  app->fmt = AV_PIX_FMT_YUV444P10LE;
    else if (strcmp(config.fmt, "yuv444p12le") == 0)  app->fmt = AV_PIX_FMT_YUV444P12LE;
    else if (strcmp(config.fmt, "gbrp10le") == 0)     app->fmt = AV_PIX_FMT_GBRP10LE;
    else if (strcmp(config.fmt, "gbrp12le") == 0)     app->fmt = AV_PIX_FMT_GBRP12LE;
    else {
        printf("Warning: Unknown fmt '%s', defaulting to yuv422p10le\n", config.fmt);
        app->fmt = AV_PIX_FMT_YUV422P10LE;
    }

    if (config.tx_url[0] != '\0') {
        strncpy(app->tx_url, config.tx_url, sizeof(app->tx_url) - 1);
        app->tx_url[sizeof(app->tx_url) - 1] = '\0';
    }

    /* Sessions — count drives how many TX sessions are created */
    app->st20p_sessions = config.session_count;

    /* Copy per-session network + crop into app->session_net[] */
    for (int i = 0; i < config.session_count; i++) {
        app->session_net[i].udp_port     = config.sessions[i].udp_port;
        app->session_net[i].payload_type = config.sessions[i].payload_type;
        app->session_net[i].crop_x       = config.sessions[i].crop_x;
        app->session_net[i].crop_y       = config.sessions[i].crop_y;
        app->session_net[i].crop_w       = config.sessions[i].crop_w;
        app->session_net[i].crop_h       = config.sessions[i].crop_h;
    }

    /* Use first session's udp_port as the legacy app->udp_port
     * (used only for the status print in tx_app_main.c) */
    app->udp_port     = config.sessions[0].udp_port;
    app->payload_type = config.sessions[0].payload_type;

    printf("Config loaded: %s (interface=%s sip=%s dip=%s)\n",
           config_file, config.interface_name,
           config.interface_sip[0] ? config.interface_sip : "dhcp",
           config.interface_dip);
    printf("Video: %dx%d %dfps %s  tx_url=%s\n",
           config.width, config.height, config.fps, config.fmt,
           config.tx_url[0] ? config.tx_url : "<none>");
    for (int i = 0; i < config.session_count; i++)
        printf("  Session %d: udp_port=%u pt=%u crop=[%d,%d %dx%d]\n", i,
               config.sessions[i].udp_port, config.sessions[i].payload_type,
               config.sessions[i].crop_x, config.sessions[i].crop_y,
               config.sessions[i].crop_w, config.sessions[i].crop_h);

    return 0;
}
