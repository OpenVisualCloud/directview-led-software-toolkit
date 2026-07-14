/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#define _GNU_SOURCE
#include "util/config_reader.h"
#include "app_context.h"
#include "util/logger.h"
#include <errno.h>
#include <limits.h>
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
        char *endptr = NULL;
        errno = 0;
        long val = strtol(pos, &endptr, 10);
        if (endptr == pos || errno == ERANGE || val < INT_MIN || val > INT_MAX)
            return -1;
        return (int)val;
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

    const char* result = extract_json_string(json, json + nread, "log_file", out_buf, out_size);
    free(json);
    return result ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * parse_tx_config — parse the JSON config file into dvledtx_config.
 *
 * Expected JSON structure:
 *   {
 *     "interfaces": [ { "name": "...", "sip": "...", "dip": "..." } ],
 *     "video": { "width": N, "height": N, "tx_url": "..." },
 *     "tx_video": { "scale_width": N, "scale_height": N, "fps": N, "fmt": "..." },
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
    strncpy(config->input_mode, "file", sizeof(config->input_mode) - 1);

    /* --- interfaces[] — parse all entries, growing arrays as needed --- */
    const char* ifaces_arr = find_array(json, buf_end, "interfaces");
    if (ifaces_arr != NULL) {
        const char* ifaces_end = find_array_end(ifaces_arr, buf_end);
        if (ifaces_end != NULL) {
            const char* icursor = ifaces_arr + 1;
            config->nic_count = 0;
            config->nic_cap   = 0;
            while (icursor < ifaces_end) {
                while (icursor < ifaces_end && *icursor != '{') icursor++;
                if (icursor >= ifaces_end) break;
                const char* iface_obj = icursor;
                const char* iface_end = find_object_end(iface_obj, ifaces_end);
                if (iface_end == NULL) {
                    LOG_WARN("interfaces[%d]: object not properly closed; stopping parse",
                             config->nic_count);
                    break;
                }
                /* Grow arrays if needed.  Each realloc() is assigned back
                 * to config-> immediately after it succeeds, rather than
                 * only after all three succeed.  Otherwise, if e.g. the
                 * first realloc() succeeds (freeing/moving the old buffer)
                 * but a later one fails, the successfully-reallocated
                 * buffer would never be stored in config and would leak,
                 * while config's stale copy of that pointer would be a
                 * dangling reference that dvledtx_config_free() would then
                 * double-free. */
                if (config->nic_count >= config->nic_cap) {
                    int old_cap = config->nic_cap;
                    int new_cap = old_cap == 0 ? 4 : old_cap * 2;

                    char (*nn)[64] = realloc(config->interface_name, (size_t)new_cap * sizeof(*nn));
                    if (!nn) {
                        LOG_ERROR("realloc failed for interface_name array");
                        free(json); dvledtx_config_free(config); return -1;
                    }
                    config->interface_name = nn;
                    memset(nn + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(*nn));

                    char (*ns)[32] = realloc(config->interface_sip, (size_t)new_cap * sizeof(*ns));
                    if (!ns) {
                        LOG_ERROR("realloc failed for interface_sip array");
                        free(json); dvledtx_config_free(config); return -1;
                    }
                    config->interface_sip = ns;
                    memset(ns + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(*ns));

                    char (*nd)[32] = realloc(config->interface_dip, (size_t)new_cap * sizeof(*nd));
                    if (!nd) {
                        LOG_ERROR("realloc failed for interface_dip array");
                        free(json); dvledtx_config_free(config); return -1;
                    }
                    config->interface_dip = nd;
                    memset(nd + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(*nd));

                    config->nic_cap = new_cap;
                }
                int n = config->nic_count;
                extract_json_string(iface_obj, iface_end, "name",
                                    config->interface_name[n], sizeof(config->interface_name[n]));
                extract_json_string(iface_obj, iface_end, "sip",
                                    config->interface_sip[n],  sizeof(config->interface_sip[n]));
                extract_json_string(iface_obj, iface_end, "dip",
                                    config->interface_dip[n],  sizeof(config->interface_dip[n]));
                config->nic_count++;
                icursor = iface_end + 1;
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
        extract_json_string(video_obj, video_end, "input_mode", config->input_mode, sizeof(config->input_mode));
        extract_json_string(video_obj, video_end, "screen_input", config->screen_input, sizeof(config->screen_input));
        extract_json_string(video_obj, video_end, "tx_url", config->tx_url, sizeof(config->tx_url));

        if (strcmp(config->input_mode, "screen_capture") == 0 && config->screen_input[0] == '\0') {
            strncpy(config->screen_input, ":0.0+0,0", sizeof(config->screen_input) - 1);
        }
    }

    /* --- tx_video block (transmission parameters) --- */
    const char* tx_video_obj = find_object(json, buf_end, "tx_video");
    if (tx_video_obj != NULL) {
        const char* tx_video_end = find_object_end(tx_video_obj, buf_end);
        if (tx_video_end == NULL) tx_video_end = buf_end;
        int v;
        v = extract_json_int(tx_video_obj, tx_video_end, "scale_width");  if (v > 0) config->scale_width  = v;
        v = extract_json_int(tx_video_obj, tx_video_end, "scale_height"); if (v > 0) config->scale_height = v;
        v = extract_json_int(tx_video_obj, tx_video_end, "fps");    if (v > 0) config->fps    = v;
        extract_json_string(tx_video_obj, tx_video_end, "fmt",    config->fmt,    sizeof(config->fmt));
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
    config->session_cap   = 0;

    while (cursor < sessions_end) {
        /* advance to next '{' */
        while (cursor < sessions_end && *cursor != '{') cursor++;
        if (cursor >= sessions_end) break;

        const char* sess_obj = cursor;
        const char* sess_end = find_object_end(sess_obj, sessions_end);
        if (sess_end == NULL) break;

        /* Grow sessions array if needed */
        if (config->session_count >= config->session_cap) {
            int new_cap = config->session_cap == 0 ? 8 : config->session_cap * 2;
            struct tx_session_config* ns = realloc(config->sessions,
                                                   (size_t)new_cap * sizeof(*ns));
            if (!ns) {
                LOG_ERROR("realloc failed for sessions array");
                free(json); dvledtx_config_free(config); return -1;
            }
            memset(ns + config->session_cap, 0,
                   (size_t)(new_cap - config->session_cap) * sizeof(*ns));
            config->sessions = ns;
            config->session_cap = new_cap;
        }

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
            s->payload_type = 96; /* default to 96 (first dynamic RTP payload type) */
        }

        v = extract_json_int(sess_obj, sess_end, "nic_index");
        s->nic_index = (v >= 0) ? v : 0;  /* default to NIC 0 for backward compat */

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

void dvledtx_config_free(struct dvledtx_config* config) {
    if (config == NULL) return;
    free(config->interface_name); config->interface_name = NULL;
    free(config->interface_sip);  config->interface_sip  = NULL;
    free(config->interface_dip);  config->interface_dip  = NULL;
    free(config->sessions);       config->sessions       = NULL;
    config->nic_count = config->nic_cap = 0;
    config->session_count = config->session_cap = 0;
}

int validate_tx_config(const struct dvledtx_config* config) {
    /* Interface validation — loop over all parsed NICs */
    if (config->nic_count == 0) {
        LOG_ERROR("'interfaces' array is empty or missing");
        return -1;
    }

    regex_t bdf_regex;
    int reti = regcomp(&bdf_regex,
        "^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\\.[0-9]$",
        REG_EXTENDED | REG_NOSUB);
    if (reti != 0) {
        LOG_ERROR("Internal error: PCI BDF regex compilation failed");
        return -1;
    }

    for (int ni = 0; ni < config->nic_count; ni++) {
        if (config->interface_name[ni][0] == '\0') {
            LOG_ERROR("interfaces[%d].name is required", ni);
            regfree(&bdf_regex);
            return -1;
        }
        if (config->interface_dip[ni][0] == '\0') {
            LOG_ERROR("interfaces[%d].dip is required", ni);
            regfree(&bdf_regex);
            return -1;
        }

        /* Validate source IP format */
        if (config->interface_sip[ni][0] != '\0') {
            struct in_addr tmp;
            if (inet_pton(AF_INET, config->interface_sip[ni], &tmp) != 1) {
                LOG_ERROR("interfaces[%d]: invalid source IP address '%s'",
                          ni, config->interface_sip[ni]);
                regfree(&bdf_regex);
                return -1;
            }
        }

        /* Validate destination IP — must be multicast */
        {
            struct in_addr tmp;
            if (inet_pton(AF_INET, config->interface_dip[ni], &tmp) != 1) {
                LOG_ERROR("interfaces[%d]: invalid destination IP address '%s'",
                          ni, config->interface_dip[ni]);
                regfree(&bdf_regex);
                return -1;
            }
            uint32_t dip_host = ntohl(tmp.s_addr);
            if ((dip_host & 0xF0000000U) != 0xE0000000U) {
                LOG_ERROR("interfaces[%d]: destination IP '%s' is not a valid "
                          "multicast address (must be in 224.0.0.0/4)",
                          ni, config->interface_dip[ni]);
                regfree(&bdf_regex);
                return -1;
            }
            if ((dip_host >> 24) != 239) {
                LOG_WARN("interfaces[%d]: destination IP '%s' is outside the "
                         "administratively-scoped multicast range (239.0.0.0/8)",
                         ni, config->interface_dip[ni]);
            }
        }

        /* S-2: Validate PCI BDF format (e.g. "0000:06:00.0") */
        reti = regexec(&bdf_regex, config->interface_name[ni], 0, NULL, 0);
        if (reti != 0) {
            LOG_ERROR("interfaces[%d]: invalid PCI BDF format '%s' "
                      "(expected DDDD:DD:DD.D hex pattern)",
                      ni, config->interface_name[ni]);
            regfree(&bdf_regex);
            return -1;
        }
    }
    regfree(&bdf_regex);

    /* Video resolution validation */
    if (config->width == 0 || config->height == 0) {
        LOG_ERROR("video width/height must be non-zero");
        return -1;
    }
    if (config->width > 3840 || config->height > 2160) {
        LOG_ERROR("video resolution %ux%u exceeds maximum 3840x2160",
               (unsigned)config->width, (unsigned)config->height);
        return -1;
    }
    if (config->width % 2 != 0) {
        LOG_ERROR("video width %u must be even for YUV formats", (unsigned)config->width);
        return -1;
    }

    /* Input mode validation */
    if (config->input_mode[0] == '\0' || strcmp(config->input_mode, "file") == 0) {
        /* Default/normal mode */
    } else if (strcmp(config->input_mode, "screen_capture") == 0) {
        if (config->screen_input[0] == '\0') {
            LOG_ERROR("video.screen_input must be set when input_mode is 'screen_capture'");
            return -1;
        }
    } else {
        LOG_ERROR("unsupported input_mode '%s' (supported: file, screen_capture)",
               config->input_mode);
        return -1;
    }

    /* Scale dimensions validation (optional — 0 means no scaling) */
    if (config->scale_width != 0 || config->scale_height != 0) {
        if (config->scale_width == 0 || config->scale_height == 0) {
            LOG_ERROR("scale_width and scale_height must both be set or both omitted");
            return -1;
        }
        if (config->scale_width > 3840 || config->scale_height > 2160) {
            LOG_ERROR("scale resolution %ux%u exceeds maximum 3840x2160",
                   (unsigned)config->scale_width, (unsigned)config->scale_height);
            return -1;
        }
        /* Validate scaled dimensions against pixel format chroma alignment */
        {
            const char* fmt_lookup = config->fmt[0] ? config->fmt : "yuv422p10le";
            if (strcmp(fmt_lookup, "yuv420") == 0) fmt_lookup = "yuv420p";
            enum AVPixelFormat pix_fmt = av_get_pix_fmt(fmt_lookup);
            const AVPixFmtDescriptor* desc =
                (pix_fmt != AV_PIX_FMT_NONE) ? av_pix_fmt_desc_get(pix_fmt) : NULL;
            int x_align = desc ? (1 << desc->log2_chroma_w) : 2;
            int y_align = desc ? (1 << desc->log2_chroma_h) : 1;

            if (x_align > 1 && config->scale_width % x_align != 0) {
                LOG_ERROR("scale_width %u must be a multiple of %d for pixel format '%s'",
                       (unsigned)config->scale_width, x_align, config->fmt);
                return -1;
            }
            if (y_align > 1 && config->scale_height % y_align != 0) {
                LOG_ERROR("scale_height %u must be a multiple of %d for pixel format '%s'",
                       (unsigned)config->scale_height, y_align, config->fmt);
                return -1;
            }
        }
    }

    /* Effective output dimensions (after scaling) for crop validation */
    uint32_t eff_width  = config->scale_width  > 0 ? config->scale_width  : config->width;
    uint32_t eff_height = config->scale_height > 0 ? config->scale_height : config->height;

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
        strcmp(config->fmt, "gbrp10le") != 0 &&
        strcmp(config->fmt, "yuv422p12le") != 0 &&
        strcmp(config->fmt, "yuv444p12le") != 0 &&
        strcmp(config->fmt, "gbrp12le") != 0) {
        LOG_ERROR("unsupported pixel format '%s'", config->fmt);
        LOG_ERROR("  Supported: yuv422p10le, yuv420, yuv444p10le, gbrp10le, "
                  "yuv422p12le, yuv444p12le, gbrp12le");
        return -1;
    }

    /* Video source file validation — screen_input was already validated
     * above in the "Input mode validation" block, so only check the file
     * path here when not in screen-capture mode. */
    if (strcmp(config->input_mode, "screen_capture") != 0 && config->tx_url[0] != '\0') {
        FILE* f = fopen(config->tx_url, "rb");
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

        /* nic_index must reference a parsed interface */
        if (s->nic_index < 0 || s->nic_index >= config->nic_count) {
            LOG_ERROR("session %d: nic_index %d is out of range [0..%d]",
                      i, s->nic_index, config->nic_count - 1);
            return -1;
        }

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
        if ((uint32_t)s->crop_x + (uint32_t)s->crop_w > eff_width) {
            LOG_ERROR("session %d: crop x=%d + w=%d = %u exceeds effective width %u",
                   i, s->crop_x, s->crop_w,
                   (uint32_t)s->crop_x + (uint32_t)s->crop_w, eff_width);
            return -1;
        }
        if ((uint32_t)s->crop_y + (uint32_t)s->crop_h > eff_height) {
            LOG_ERROR("session %d: crop y=%d + h=%d = %u exceeds effective height %u",
                   i, s->crop_y, s->crop_h,
                   (uint32_t)s->crop_y + (uint32_t)s->crop_h, eff_height);
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
            const char* fmt_lookup = config->fmt[0] ? config->fmt : "yuv422p10le";
            if (strcmp(fmt_lookup, "yuv420") == 0) fmt_lookup = "yuv420p";
            enum AVPixelFormat pix_fmt = av_get_pix_fmt(fmt_lookup);
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

        /* Check for duplicate UDP ports on the same NIC
         * (same port on different NICs is valid — separate interfaces) */
        for (int j = 0; j < i; j++) {
            if (config->sessions[j].udp_port == s->udp_port &&
                config->sessions[j].nic_index == s->nic_index) {
                LOG_ERROR("session %d and %d have duplicate udp_port %d on NIC %d",
                       j, i, s->udp_port, s->nic_index);
                return -1;
            }
        }
    }

    return 0;
}

int load_and_apply_config(struct dvledtx_context* app, const char* config_file) {
    if (app == NULL)
        return -1;
    if (config_file == NULL || config_file[0] == '\0')
        return 0; /* no config file — keep CLI defaults */

    struct dvledtx_config config;
    memset(&config, 0, sizeof(config));
    if (parse_tx_config(config_file, &config) != 0) {
        LOG_WARN("Failed to parse config file %s", config_file);
        dvledtx_config_free(&config);
        return -1;
    }
    if (validate_tx_config(&config) != 0) {
        LOG_WARN("Invalid config file %s", config_file);
        dvledtx_config_free(&config);
        return -1;
    }

    /* Allocate dynamic arrays in app context */
    dvledtx_context_free(app);  /* free any previous allocation */
    if (dvledtx_context_alloc(app, config.nic_count, config.session_count) < 0) {
        LOG_ERROR("Failed to allocate app context arrays");
        dvledtx_config_free(&config);
        return -1;
    }

    /* Interface — copy all NICs */
    for (int ni = 0; ni < config.nic_count; ni++) {
        strncpy(app->nics[ni].port, config.interface_name[ni],
                sizeof(app->nics[ni].port) - 1);
        app->nics[ni].port[sizeof(app->nics[ni].port) - 1] = '\0';

        if (config.interface_sip[ni][0] != '\0') {
            strncpy(app->nics[ni].sip_addr_str, config.interface_sip[ni],
                    sizeof(app->nics[ni].sip_addr_str) - 1);
            app->nics[ni].sip_addr_str[sizeof(app->nics[ni].sip_addr_str) - 1] = '\0';
        }

        strncpy(app->nics[ni].dip_addr_str, config.interface_dip[ni],
                sizeof(app->nics[ni].dip_addr_str) - 1);
        app->nics[ni].dip_addr_str[sizeof(app->nics[ni].dip_addr_str) - 1] = '\0';
    }

    /* Video */
    app->width  = config.width;
    app->height = config.height;
    app->scale_width  = config.scale_width;
    app->scale_height = config.scale_height;
    app->fps    = config.fps;

    if (strcmp(config.fmt, "yuv422p10le") == 0)       app->fmt = AV_PIX_FMT_YUV422P10LE;
    else if (strcmp(config.fmt, "yuv420") == 0)        app->fmt = AV_PIX_FMT_YUV420P;
    else if (strcmp(config.fmt, "yuv444p10le") == 0)  app->fmt = AV_PIX_FMT_YUV444P10LE;
    else if (strcmp(config.fmt, "gbrp10le") == 0)     app->fmt = AV_PIX_FMT_GBRP10LE;
    else if (strcmp(config.fmt, "yuv422p12le") == 0)  app->fmt = AV_PIX_FMT_YUV422P12LE;
    else if (strcmp(config.fmt, "yuv444p12le") == 0)  app->fmt = AV_PIX_FMT_YUV444P12LE;
    else if (strcmp(config.fmt, "gbrp12le") == 0)     app->fmt = AV_PIX_FMT_GBRP12LE;
    else {
        LOG_ERROR("Unsupported pixel format '%s'", config.fmt);
        dvledtx_config_free(&config);
        return -1;
    }

    if (config.tx_url[0] != '\0') {
        strncpy(app->tx_url, config.tx_url, sizeof(app->tx_url) - 1);
        app->tx_url[sizeof(app->tx_url) - 1] = '\0';
    }
    if (config.screen_input[0] != '\0') {
        strncpy(app->screen_input, config.screen_input, sizeof(app->screen_input) - 1);
        app->screen_input[sizeof(app->screen_input) - 1] = '\0';
    }
    app->use_screen_capture = (strcmp(config.input_mode, "screen_capture") == 0);

    /* Copy per-session network + crop into app->session_net[] */
    for (int i = 0; i < config.session_count; i++) {
        app->session_net[i].udp_port     = config.sessions[i].udp_port;
        app->session_net[i].payload_type = config.sessions[i].payload_type;
        app->session_net[i].crop_x       = config.sessions[i].crop_x;
        app->session_net[i].crop_y       = config.sessions[i].crop_y;
        app->session_net[i].crop_w       = config.sessions[i].crop_w;
        app->session_net[i].crop_h       = config.sessions[i].crop_h;
        app->session_net[i].nic_index    = config.sessions[i].nic_index;
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

    LOG_INFO("Config loaded: %s (%d NIC(s), %d session(s))",
             config_file, config.nic_count, config.session_count);
    for (int ni = 0; ni < config.nic_count; ni++)
        LOG_INFO("  NIC[%d]: interface=%s sip=%s dip=%s", ni,
                 config.interface_name[ni],
                 config.interface_sip[ni][0] ? config.interface_sip[ni] : "dhcp",
                 config.interface_dip[ni]);
    if (config.scale_width > 0 && config.scale_height > 0)
         LOG_INFO("Video: %ux%u -> scale %ux%u %dfps %s mode=%s source=%s",
               config.width, config.height,
               config.scale_width, config.scale_height,
               config.fps, config.fmt,
             config.input_mode[0] ? config.input_mode : "file",
             strcmp(config.input_mode, "screen_capture") == 0
               ? (config.screen_input[0] ? config.screen_input : "<none>")
               : (config.tx_url[0] ? config.tx_url : "<none>"));
    else
         LOG_INFO("Video: %ux%u %dfps %s mode=%s source=%s",
               config.width, config.height, config.fps, config.fmt,
             config.input_mode[0] ? config.input_mode : "file",
             strcmp(config.input_mode, "screen_capture") == 0
               ? (config.screen_input[0] ? config.screen_input : "<none>")
               : (config.tx_url[0] ? config.tx_url : "<none>"));
    for (int i = 0; i < config.session_count; i++)
        LOG_INFO("  Session %d: udp_port=%u pt=%u nic=%d crop=[%d,%d %dx%d]", i,
               config.sessions[i].udp_port, config.sessions[i].payload_type,
               config.sessions[i].nic_index,
               config.sessions[i].crop_x, config.sessions[i].crop_y,
               config.sessions[i].crop_w, config.sessions[i].crop_h);

    dvledtx_config_free(&config);
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
    for (int ni = 0; ni < ctx->nic_count; ni++) {
        if (ctx->nics[ni].sip_addr_str[0] != '\0') {
            if (inet_pton(AF_INET, ctx->nics[ni].sip_addr_str,
                          ctx->nics[ni].sip_addr) != 1) {
                LOG_ERROR("NIC[%d]: invalid source IP address %s",
                          ni, ctx->nics[ni].sip_addr_str);
                return -1;
            }
        } else {
            LOG_INFO("NIC[%d]: no source IP provided, DHCP mode", ni);
        }
        if (inet_pton(AF_INET, ctx->nics[ni].dip_addr_str,
                      ctx->nics[ni].dip_addr) != 1) {
            LOG_ERROR("NIC[%d]: invalid destination IP address %s",
                      ni, ctx->nics[ni].dip_addr_str);
            return -1;
        }
    }
    return 0;
}
