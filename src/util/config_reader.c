/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#define _GNU_SOURCE
#include "util/config_reader.h"
#include "app_context.h"
#include "util/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <regex.h>
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
        if (pos == NULL) return NULL;
        pos += klen;
        /* skip whitespace then ':' */
        while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
        if (pos >= end || *pos != ':') continue;
        pos++;
        while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')) pos++;
        if (pos >= end || *pos != '"') return NULL;
        pos++; /* skip opening quote */

        /* Walk the string value handling escape sequences (\\, \", \/, \n, etc.)
         * and rejecting bare control characters (U+0000..U+001F). */
        size_t out = 0;
        while (pos < end && *pos != '"') {
            if ((unsigned char)*pos < 0x20) {
                /* Bare control character in JSON string — skip it */
                pos++;
                continue;
            }
            if (*pos == '\\' && pos + 1 < end) {
                pos++; /* consume backslash */
                char esc = '\0';
                switch (*pos) {
                    case '"':  esc = '"';  break;
                    case '\\': esc = '\\'; break;
                    case '/':  esc = '/';  break;
                    case 'n':  esc = '\n'; break;
                    case 'r':  esc = '\r'; break;
                    case 't':  esc = '\t'; break;
                    case 'b':  esc = '\b'; break;
                    case 'f':  esc = '\f'; break;
                    default:   esc = *pos; break; /* unknown escape — keep literal */
                }
                if (out < buffer_size - 1) buffer[out++] = esc;
                pos++;
                continue;
            }
            if (out < buffer_size - 1) buffer[out++] = *pos;
            pos++;
        }
        buffer[out] = '\0';
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
        if (pos == NULL) return -1;
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
    if (obj_start == NULL || *obj_start != '{') return NULL;
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
    if (pos == NULL) return NULL;
    pos += klen;
    while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' || *pos == ':')) pos++;
    if (pos >= end || *pos != '[') return NULL;
    return pos;
}

/* Find the closing ']' that matches the opening '[' at arr_start. */
static const char* find_array_end(const char* arr_start, const char* buf_end) {
    if (arr_start == NULL || *arr_start != '[') return NULL;
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
 * peek_config_log_file — quickly extract only the top-level "log_file" value.
 * Returns 0 and fills out_buf on success, -1 if not found or on error.
 * Intentionally lightweight so the logger can be redirected to file BEFORE
 * the full parse (and its log output) runs.
 * -------------------------------------------------------------------------*/
int peek_config_log_file(const char* config_file, char* out_buf, size_t out_size) {
    if (config_file == NULL || out_buf == NULL) return -1;

    FILE* fp = fopen(config_file, "r");
    if (fp == NULL) return -1;

    fseek(fp, 0, SEEK_END);
    long raw_file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (raw_file_size <= 0) { fclose(fp); return -1; }
    size_t file_size = (size_t)raw_file_size;

    char* json = malloc(file_size + 1);
    if (json == NULL) { fclose(fp); return -1; }

    size_t nread = fread(json, 1, file_size, fp);
    if (nread == 0) { free(json); fclose(fp); return -1; }
    json[nread] = '\0';
    fclose(fp);

    char* result = extract_json_string(json, json + nread, "log_file", out_buf, out_size);
    free(json);
    return result ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * parse_tx_config — parse the JSON config file into dvledtx_config.
 *
 * Expected JSON structure:
 *   {
 *     "interfaces": [ { "name": "...", "sip": "...", "dip": "..." } ],
 *     "video": { "width": N, "height": N, "fps": N, "fmt": "...", "tx_url": "..." },
 *     "log_file": "/path/to/dvledtx.log",  (optional — omit for console-only logging)
 *     "tx_sessions": [
 *       { "udp_port": N, "payload_type": N, "crop": { "x":N, "y":N, "w":N, "h":N } },
 *       ...
 *     ]
 *   }
 * -------------------------------------------------------------------------*/
int parse_tx_config(const char* config_file, struct dvledtx_config* config) {
    FILE* fp = fopen(config_file, "r");
    if (fp == NULL) {
        LOG_ERROR("Cannot open config file %s", config_file);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long raw_file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (raw_file_size <= 0) { fclose(fp); return -1; }
    size_t file_size = (size_t)raw_file_size;

    char* json = malloc(file_size + 1);
    if (json == NULL) { fclose(fp); return -1; }

    size_t nread = fread(json, 1, file_size, fp);
    if (nread == 0) { free(json); fclose(fp); return -1; }
    json[nread] = '\0';
    fclose(fp);

    const char* buf_end = json + nread;
    memset(config, 0, sizeof(*config));

    /* --- interfaces[0] --- */
    const char* ifaces_arr = find_array(json, buf_end, "interfaces");
    if (ifaces_arr != NULL) {
        const char* ifaces_end = find_array_end(ifaces_arr, buf_end);
        if (ifaces_end != NULL) {
            const char* first_brace = ifaces_arr + 1;
            while (first_brace < ifaces_end && *first_brace != '{') first_brace++;
            if (first_brace < ifaces_end) {
                const char* iface_obj = first_brace;
                const char* iface_end = find_object_end(iface_obj, ifaces_end);
                if (iface_end == NULL) {
                    LOG_WARN("interfaces[0] object not properly closed; "
                           "truncating parse at array end");
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
    if (video_obj != NULL) {
        const char* video_end = find_object_end(video_obj, buf_end);
        if (video_end == NULL) video_end = buf_end;
        int v;
        v = extract_json_int(video_obj, video_end, "width");  if (v > 0) config->width  = v;
        v = extract_json_int(video_obj, video_end, "height"); if (v > 0) config->height = v;
        v = extract_json_int(video_obj, video_end, "fps");    if (v > 0) config->fps    = v;
        extract_json_string(video_obj, video_end, "fmt",    config->fmt,    sizeof(config->fmt));
        extract_json_string(video_obj, video_end, "tx_url", config->tx_url, sizeof(config->tx_url));
    }

    /* --- optional top-level log_file --- */
    extract_json_string(json, buf_end, "log_file", config->log_file, sizeof(config->log_file));

    /* --- tx_sessions array --- */
    const char* sessions_arr = find_array(json, buf_end, "tx_sessions");
    if (sessions_arr == NULL) {
        LOG_ERROR("'tx_sessions' not found in config");
        free(json);
        return -1;
    }
    const char* sessions_end = find_array_end(sessions_arr, buf_end);
    if (sessions_end == NULL) sessions_end = buf_end;

    /* Walk the array, extracting each '{...}' object */
    const char* cursor = sessions_arr + 1; /* skip '[' */
    config->session_count = 0;

    while (cursor < sessions_end && config->session_count < MAX_TX_SESSIONS) {
        /* advance to next '{' */
        while (cursor < sessions_end && *cursor != '{') cursor++;
        if (cursor >= sessions_end) break;

        const char* sess_obj = cursor;
        const char* sess_end = find_object_end(sess_obj, sessions_end);
        if (sess_end == NULL) break;

        struct tx_session_config* s = &config->sessions[config->session_count];
        int v;

        v = extract_json_int(sess_obj, sess_end, "udp_port");
        if (v > 0 && v <= 65535) {
            s->udp_port = (uint16_t)v;
        } else {
            LOG_ERROR("session %d: udp_port not set or invalid (%d)",
                   config->session_count, v);
            free(json);
            return -1;
        }

        v = extract_json_int(sess_obj, sess_end, "payload_type");
        if (v > 0 && v <= 255) {
            s->payload_type = (uint8_t)v;
        } else {
            LOG_ERROR("session %d: payload_type not set or invalid (%d)",
                   config->session_count, v);
            free(json);
            return -1;
        }

        /* crop sub-object */
        const char* crop_obj = find_object(sess_obj, sess_end, "crop");
        if (crop_obj == NULL) {
            LOG_ERROR("session %d: 'crop' object is required", config->session_count);
            free(json);
            return -1;
        }
        {
            const char* crop_end = find_object_end(crop_obj, sess_end);
            if (crop_end == NULL) crop_end = sess_end;
            v = extract_json_int(crop_obj, crop_end, "x");
            if (v < 0) { LOG_ERROR("session %d: crop 'x' is required", config->session_count); free(json); return -1; }
            s->crop_x = v;
            v = extract_json_int(crop_obj, crop_end, "y");
            if (v < 0) { LOG_ERROR("session %d: crop 'y' is required", config->session_count); free(json); return -1; }
            s->crop_y = v;
            v = extract_json_int(crop_obj, crop_end, "w");
            if (v <= 0) { LOG_ERROR("session %d: crop 'w' is required", config->session_count); free(json); return -1; }
            s->crop_w = v;
            v = extract_json_int(crop_obj, crop_end, "h");
            if (v <= 0) { LOG_ERROR("session %d: crop 'h' is required", config->session_count); free(json); return -1; }
            s->crop_h = v;
        }

        config->session_count++;
        cursor = sess_end + 1;
    }

    if (config->session_count == 0) {
        LOG_ERROR("No tx_sessions found in config");
        free(json);
        return -1;
    }

    free(json);
    return 0;
}

int validate_tx_config(const struct dvledtx_config* config) {
    /* Interface validation */
    if (config->interface_name[0] == '\0') {
        LOG_ERROR("interfaces[0].name is required");
        return -1;
    }
    if (config->interface_dip[0] == '\0') {
        LOG_ERROR("interfaces[0].dip is required");
        return -1;
    }

    /* Validate IP address format (basic dotted-quad check) */
    if (config->interface_sip[0] != '\0') {
        struct in_addr tmp;
        if (inet_pton(AF_INET, config->interface_sip, &tmp) != 1) {
            LOG_ERROR("Invalid source IP address '%s'", config->interface_sip);
            return -1;
        }
    }
    {
        struct in_addr tmp;
        if (inet_pton(AF_INET, config->interface_dip, &tmp) != 1) {
            LOG_ERROR("Invalid destination IP address '%s'", config->interface_dip);
            return -1;
        }
        /* D-3: Validate DIP is within multicast range (224.0.0.0/4) */
        uint32_t dip_host = ntohl(tmp.s_addr);
        if ((dip_host & 0xF0000000U) != 0xE0000000U) {
            LOG_ERROR("Destination IP '%s' is not a valid multicast address "
                   "(must be in 224.0.0.0/4)", config->interface_dip);
            return -1;
        }
        /* Warn if not in administratively-scoped range 239.0.0.0/8 */
        if ((dip_host >> 24) != 239) {
            LOG_WARN("Destination IP '%s' is outside the administratively-scoped "
                   "multicast range (239.0.0.0/8)", config->interface_dip);
        }
    }

    /* S-2: Validate PCI BDF format (e.g. "0000:06:00.0") */
    if (config->interface_name[0] != '\0') {
        regex_t regex;
        int reti = regcomp(&regex,
            "^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\\.[0-9]$",
            REG_EXTENDED | REG_NOSUB);
        if (reti == 0) {
            reti = regexec(&regex, config->interface_name, 0, NULL, 0);
            regfree(&regex);
            if (reti != 0) {
                LOG_ERROR("Invalid PCI BDF format '%s' "
                       "(expected DDDD:DD:DD.D hex pattern)",
                       config->interface_name);
                return -1;
            }
        }
    }

    /* Video resolution validation */
    if (config->width == 0 || config->height == 0) {
        LOG_ERROR("video width/height must be non-zero");
        return -1;
    }
    if (config->width > 7680 || config->height > 4320) {
        LOG_ERROR("video resolution %dx%d exceeds maximum 7680x4320",
               config->width, config->height);
        return -1;
    }
    if (config->width % 2 != 0) {
        LOG_ERROR("video width %d must be even for YUV formats", config->width);
        return -1;
    }

    /* FPS validation */
    if (config->fps != 25 && config->fps != 30 &&
        config->fps != 50 && config->fps != 60) {
        LOG_ERROR("unsupported fps %d (supported: 25, 30, 50, 60)", config->fps);
        return -1;
    }

    /* Pixel format validation */
    if (config->fmt[0] != '\0' &&
        strcmp(config->fmt, "yuv422p10le") != 0 &&
        strcmp(config->fmt, "yuv420") != 0 &&
        strcmp(config->fmt, "yuv444p10le") != 0 &&
        strcmp(config->fmt, "gbrp10le") != 0) {
        LOG_ERROR("unsupported pixel format '%s'", config->fmt);
        LOG_ERROR("  Supported: yuv422p10le, yuv420, yuv444p10le, gbrp10le");
        return -1;
    }

    /* Video source file validation */
    if (config->tx_url[0] != '\0') {
        FILE* f = fopen(config->tx_url, "r");
        if (!f) {
            LOG_ERROR("video source file not found: %s", config->tx_url);
            return -1;
        }
        fclose(f);
    }

    /* Session validation */
    if (config->session_count == 0) {
        LOG_ERROR("tx_sessions array is empty");
        return -1;
    }

    for (int i = 0; i < config->session_count; i++) {
        const struct tx_session_config* s = &config->sessions[i];

        /* UDP port range */
        if (s->udp_port == 0) {
            LOG_ERROR("session %d: udp_port must be non-zero", i);
            return -1;
        }
        /* D-3: Enforce UDP port in safe range (above well-known ports) */
        if (s->udp_port < 1024) {
            LOG_ERROR("session %d: udp_port %d is in the privileged range "
                   "(must be >= 1024)", i, s->udp_port);
            return -1;
        }

        /* Payload type range (RFC 3551: dynamic range 96-127) */
        if (s->payload_type < 96 || s->payload_type > 127) {
            LOG_ERROR("session %d: payload_type %d out of range 96-127",
                   i, s->payload_type);
            return -1;
        }

        /* Crop bounds: must fit within the source video */
        if (s->crop_x < 0 || s->crop_y < 0 || s->crop_w <= 0 || s->crop_h <= 0) {
            LOG_ERROR("session %d: crop values must be positive "
                   "(x=%d y=%d w=%d h=%d)", i, s->crop_x, s->crop_y,
                   s->crop_w, s->crop_h);
            return -1;
        }
        if ((uint32_t)s->crop_x + (uint32_t)s->crop_w > config->width) {
            LOG_ERROR("session %d: crop x=%d + w=%d = %u exceeds video width %u",
                   i, s->crop_x, s->crop_w,
                   (uint32_t)s->crop_x + (uint32_t)s->crop_w, config->width);
            return -1;
        }
        if ((uint32_t)s->crop_y + (uint32_t)s->crop_h > config->height) {
            LOG_ERROR("session %d: crop y=%d + h=%d = %u exceeds video height %u",
                   i, s->crop_y, s->crop_h,
                   (uint32_t)s->crop_y + (uint32_t)s->crop_h, config->height);
            return -1;
        }
        if (s->crop_w % 2 != 0) {
            LOG_ERROR("session %d: crop width %d must be even for YUV formats",
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
                LOG_ERROR("session %d: crop_x %d must be a multiple of %d "
                       "for pixel format '%s'", i, s->crop_x, x_align, config->fmt);
                return -1;
            }
            if (x_align > 1 && s->crop_w % x_align != 0) {
                LOG_ERROR("session %d: crop_w %d must be a multiple of %d "
                       "for pixel format '%s'", i, s->crop_w, x_align, config->fmt);
                return -1;
            }
            if (y_align > 1 && s->crop_y % y_align != 0) {
                LOG_ERROR("session %d: crop_y %d must be a multiple of %d "
                       "for pixel format '%s'", i, s->crop_y, y_align, config->fmt);
                return -1;
            }
            if (y_align > 1 && s->crop_h % y_align != 0) {
                LOG_ERROR("session %d: crop_h %d must be a multiple of %d "
                       "for pixel format '%s'", i, s->crop_h, y_align, config->fmt);
                return -1;
            }
        }

        /* Check for duplicate UDP ports */
        for (int j = 0; j < i; j++) {
            if (config->sessions[j].udp_port == s->udp_port) {
                LOG_ERROR("session %d and %d have duplicate udp_port %d",
                       j, i, s->udp_port);
                return -1;
            }
        }
    }

    return 0;
}

int load_and_apply_config(struct dvledtx_context* app, const char* config_file) {
    if (app == NULL || config_file == NULL || config_file[0] == '\0')
        return 0; /* no config file — keep CLI defaults */

    struct dvledtx_config config;
    if (parse_tx_config(config_file, &config) != 0) {
        LOG_WARN("Failed to parse config file %s", config_file);
        return -1;
    }
    if (validate_tx_config(&config) != 0) {
        LOG_WARN("Invalid config file %s", config_file);
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
    else if (strcmp(config.fmt, "yuv420") == 0)        app->fmt = AV_PIX_FMT_YUV420P;
    else if (strcmp(config.fmt, "yuv444p10le") == 0)  app->fmt = AV_PIX_FMT_YUV444P10LE;
    else if (strcmp(config.fmt, "gbrp10le") == 0)     app->fmt = AV_PIX_FMT_GBRP10LE;
    else {
        LOG_ERROR("Unsupported pixel format '%s'", config.fmt);
        return -1;
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
     * (used only for the status print in dvledtx_main.c) */
    app->udp_port     = config.sessions[0].udp_port;
    app->payload_type = config.sessions[0].payload_type;

    /* Optional log file from config */
    if (config.log_file[0] != '\0') {
        strncpy(app->log_file, config.log_file, sizeof(app->log_file) - 1);
        app->log_file[sizeof(app->log_file) - 1] = '\0';
    }

    LOG_INFO("Config loaded: %s (interface=%s sip=%s dip=%s)",
           config_file, config.interface_name,
           config.interface_sip[0] ? config.interface_sip : "dhcp",
           config.interface_dip);
    LOG_INFO("Video: %dx%d %dfps %s  tx_url=%s",
           config.width, config.height, config.fps, config.fmt,
           config.tx_url[0] ? config.tx_url : "<none>");
    for (int i = 0; i < config.session_count; i++)
        LOG_INFO("  Session %d: udp_port=%u pt=%u crop=[%d,%d %dx%d]", i,
               config.sessions[i].udp_port, config.sessions[i].payload_type,
               config.sessions[i].crop_x, config.sessions[i].crop_y,
               config.sessions[i].crop_w, config.sessions[i].crop_h);

    return 0;
}

/* -------------------------------------------------------------------------
 * resolve_ip_addrs
 *
 * Convert the human-readable sip_addr_str / dip_addr_str fields of the app
 * context into packed binary (struct in_addr) after the configuration has
 * been loaded.  Must be called once, after load_and_apply_config().
 *
 * sip_addr_str is optional (empty → DHCP mode, binary left zeroed).
 * dip_addr_str is mandatory — the multicast destination IP must be valid.
 * -------------------------------------------------------------------------*/
int resolve_ip_addrs(struct dvledtx_context* ctx) {
    if (ctx->sip_addr_str[0] != '\0') {
        if (inet_pton(AF_INET, ctx->sip_addr_str, ctx->sip_addr) != 1) {
            LOG_ERROR("Invalid source IP address %s", ctx->sip_addr_str);
            return -1;
        }
    } else {
        LOG_INFO("No source IP provided, DHCP mode");
    }
    if (inet_pton(AF_INET, ctx->dip_addr_str, ctx->dip_addr) != 1) {
        LOG_ERROR("Invalid destination IP address %s", ctx->dip_addr_str);
        return -1;
    }
    return 0;
}
