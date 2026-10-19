/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "config_reader.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <json-c/json.h>

#define LOGGER_TAG "config"

/* ------------------------------------------------------------------ */
/* Helper: safely copy a JSON string value into a fixed-size buffer.  */
/* ------------------------------------------------------------------ */
static void json_str_copy(json_object* obj, const char* key,
                          char* dst, size_t dst_size) {
    json_object* val = NULL;
    if (json_object_object_get_ex(obj, key, &val) &&
        json_object_is_type(val, json_type_string)) {
        strncpy(dst, json_object_get_string(val), dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

/* Helper: get an integer from a JSON object (returns fallback on miss). */
static int json_int_get(json_object* obj, const char* key, int fallback) {
    json_object* val = NULL;
    if (json_object_object_get_ex(obj, key, &val) &&
        json_object_is_type(val, json_type_int)) {
        return json_object_get_int(val);
    }
    return fallback;
}

/* ------------------------------------------------------------------ */
/* parse_rx_config – read the JSON file and populate struct rx_config. */
/* ------------------------------------------------------------------ */
int parse_rx_config(const char* config_file, struct rx_config* config) {
    if (!config_file || !config) return -1;

    memset(config, 0, sizeof(*config));

    json_object* root = json_object_from_file(config_file);
    if (!root) {
        log_error(LOGGER_TAG, "Cannot parse JSON config file %s", config_file);
        return -1;
    }

    /* ---------- interfaces ---------- */
    json_object* ifaces = NULL;
    if (json_object_object_get_ex(root, "interfaces", &ifaces) &&
        json_object_is_type(ifaces, json_type_array)) {
        int n = json_object_array_length(ifaces);
        if (n > MAX_INTERFACES) {
            log_warn(LOGGER_TAG, "%d interfaces configured, only the first %d are used",
                     n, MAX_INTERFACES);
            n = MAX_INTERFACES;
        }
        config->num_interfaces = n;
        for (int i = 0; i < n; i++) {
            json_object* item = json_object_array_get_idx(ifaces, i);
            config->interfaces[i].nic_index = json_int_get(item, "nic_index", i);
            json_str_copy(item, "name", config->interfaces[i].name,
                          sizeof(config->interfaces[i].name));
            /* Accept both the RX ("SIP") and TX ("sip") spellings. */
            json_str_copy(item, "SIP", config->interfaces[i].sip,
                          sizeof(config->interfaces[i].sip));
            if (config->interfaces[i].sip[0] == '\0')
                json_str_copy(item, "sip", config->interfaces[i].sip,
                              sizeof(config->interfaces[i].sip));
            json_str_copy(item, "DIP", config->interfaces[i].dip,
                          sizeof(config->interfaces[i].dip));
            if (config->interfaces[i].dip[0] == '\0')
                json_str_copy(item, "dip", config->interfaces[i].dip,
                              sizeof(config->interfaces[i].dip));
        }
    }

    /* ---------- video ---------- */
    json_object* video = NULL;
    if (json_object_object_get_ex(root, "video", &video) &&
        json_object_is_type(video, json_type_object)) {
        config->video.width  = json_int_get(video, "width",  0);
        config->video.height = json_int_get(video, "height", 0);
        config->video.fps    = json_int_get(video, "fps",    30);
        json_str_copy(video, "fmt",    config->video.fmt,    sizeof(config->video.fmt));
    }

    /* ---------- log_file ---------- */
    json_str_copy(root, "log_file", config->log_file, sizeof(config->log_file));
    json_str_copy(root, "log_level", config->log_level, sizeof(config->log_level));
    json_str_copy(root, "display_mode", config->display_mode, sizeof(config->display_mode));

    json_object* sleep_obj = NULL;
    if (json_object_object_get_ex(root, "tasklet_sleep", &sleep_obj))
        config->tasklet_sleep = json_object_get_boolean(sleep_obj);

    /* ---------- rx_sessions ---------- */
    json_object* sessions = NULL;
    if (json_object_object_get_ex(root, "rx_sessions", &sessions) &&
        json_object_is_type(sessions, json_type_array)) {
        int n = json_object_array_length(sessions);
        if (n > MAX_RX_SESSIONS) {
            log_warn(LOGGER_TAG, "%d rx_sessions configured, only the first %d are used",
                     n, MAX_RX_SESSIONS);
            n = MAX_RX_SESSIONS;
        }
        config->num_rx_sessions = n;
        for (int i = 0; i < n; i++) {
            json_object* s = json_object_array_get_idx(sessions, i);
            config->rx_sessions[i].nic_index    = json_int_get(s, "nic_index", 0);
            config->rx_sessions[i].udp_port     = json_int_get(s, "udp_port", 20000);
            config->rx_sessions[i].payload_type  = json_int_get(s, "payload_type", 96);
            json_str_copy(s, "output", config->rx_sessions[i].output,
                          sizeof(config->rx_sessions[i].output));

            json_object* scale = NULL;
            if (json_object_object_get_ex(s, "scale", &scale) &&
                json_object_is_type(scale, json_type_object)) {
                config->rx_sessions[i].scale_w = json_int_get(scale, "w", 0);
                config->rx_sessions[i].scale_h = json_int_get(scale, "h", 0);
            }

            json_object* crop = NULL;
            if (json_object_object_get_ex(s, "crop", &crop) &&
                json_object_is_type(crop, json_type_object)) {
                config->rx_sessions[i].crop.x = json_int_get(crop, "x", 0);
                config->rx_sessions[i].crop.y = json_int_get(crop, "y", 0);
                config->rx_sessions[i].crop.w = json_int_get(crop, "w", 0);
                config->rx_sessions[i].crop.h = json_int_get(crop, "h", 0);
            }

            json_object* win = NULL;
            if (json_object_object_get_ex(s, "window", &win) &&
                json_object_is_type(win, json_type_object)) {
                config->rx_sessions[i].has_window = true;
                config->rx_sessions[i].window.x = json_int_get(win, "x", 0);
                config->rx_sessions[i].window.y = json_int_get(win, "y", 0);
                config->rx_sessions[i].window.w = json_int_get(win, "w", 0);
                config->rx_sessions[i].window.h = json_int_get(win, "h", 0);
            }
        }
    }

    /* ---------- displays: output -> sessions placed on it ---------- */
    json_object* displays = NULL;
    if (json_object_object_get_ex(root, "displays", &displays) &&
        json_object_is_type(displays, json_type_array)) {
        if (config->display_mode[0] == '\0')
            snprintf(config->display_mode, sizeof(config->display_mode), "per_output");
        int nd = json_object_array_length(displays);
        for (int d = 0; d < nd; d++) {
            json_object* disp = json_object_array_get_idx(displays, d);
            char output[32] = "";
            json_str_copy(disp, "output", output, sizeof(output));

            json_object* list = NULL;
            if (!json_object_object_get_ex(disp, "sessions", &list) ||
                !json_object_is_type(list, json_type_array))
                continue;
            int ns = json_object_array_length(list);
            for (int k = 0; k < ns; k++) {
                json_object* e = json_object_array_get_idx(list, k);
                int idx = json_int_get(e, "session", -1);
                if (idx < 0 || idx >= config->num_rx_sessions) {
                    log_error(LOGGER_TAG, "displays[%d] (%s) references unknown session %d",
                              d, output, idx);
                    json_object_put(root);
                    return -1;
                }
                struct rx_session_config* s = &config->rx_sessions[idx];
                if (s->display_assigned) {
                    if (s->num_mirrors >= MAX_MIRRORS) {
                        log_error(LOGGER_TAG, "session %d is placed on more than %d displays",
                                  idx, MAX_MIRRORS + 1);
                        json_object_put(root);
                        return -1;
                    }
                    struct display_placement* m = &s->mirrors[s->num_mirrors++];
                    snprintf(m->output, sizeof(m->output), "%s", output);
                    m->window.x = json_int_get(e, "x", 0);
                    m->window.y = json_int_get(e, "y", 0);
                    m->window.w = json_int_get(e, "w", 0);
                    m->window.h = json_int_get(e, "h", 0);
                    m->scale_w  = json_int_get(e, "scale_w", 0);
                    m->scale_h  = json_int_get(e, "scale_h", 0);
                    continue;
                }
                s->display_assigned = true;
                snprintf(s->output, sizeof(s->output), "%s", output);
                s->has_window = true;
                s->window.x = json_int_get(e, "x", 0);
                s->window.y = json_int_get(e, "y", 0);
                s->window.w = json_int_get(e, "w", 0);
                s->window.h = json_int_get(e, "h", 0);
                s->scale_w  = json_int_get(e, "scale_w", 0);
                s->scale_h  = json_int_get(e, "scale_h", 0);
            }
        }
    }

    json_object_put(root);  /* free parsed tree */

    /* video width/height are optional: default to the bounding box of all crops. */
    if (config->video.width <= 0 || config->video.height <= 0) {
        int max_x = 0, max_y = 0;
        for (int i = 0; i < config->num_rx_sessions; i++) {
            const struct crop_config* c = &config->rx_sessions[i].crop;
            if (c->x + c->w > max_x) max_x = c->x + c->w;
            if (c->y + c->h > max_y) max_y = c->y + c->h;
        }
        if (config->video.width <= 0)  config->video.width  = max_x > 0 ? max_x : 1920;
        if (config->video.height <= 0) config->video.height = max_y > 0 ? max_y : 1080;
        log_info(LOGGER_TAG, "video size not set, using %dx%d from session crops",
                 config->video.width, config->video.height);
    }
    if (config->video.fps == 0) config->video.fps = 30;
    return 0;
}

/* Supported "fmt" names -> bits per pixel on the wire (ST 2110-20). */
static int fmt_bits_per_pixel(const char* fmt) {
    static const struct { const char* name; int bpp; } fmts[] = {
        {"yuv422p10le", 20}, {"yuv422p12le", 24},
        {"yuv444p10le", 30}, {"yuv444p12le", 36},
        {"gbrp10le",    30}, {"gbrp12le",    36},
        {"yuv420",      12},
    };
    if (!fmt || fmt[0] == '\0') return 20; /* default yuv422p10le */
    for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++)
        if (strcmp(fmt, fmts[i].name) == 0) return fmts[i].bpp;
    return -1;
}

static int check_window(const char* what, int idx, const char* output,
                        const struct crop_config* win, int scale_w, int scale_h) {
    if (output && output[0] == '\0') {
        log_error(LOGGER_TAG, "%s for session %d has no \"output\" name", what, idx);
        return -1;
    }
    if (win->x < 0 || win->y < 0 || win->w < 0 || win->h < 0 ||
        win->x > MAX_VIDEO_DIM || win->y > MAX_VIDEO_DIM ||
        win->w > MAX_VIDEO_DIM || win->h > MAX_VIDEO_DIM) {
        log_error(LOGGER_TAG, "%s for session %d has invalid window %d,%d %dx%d (0..%d)",
                  what, idx, win->x, win->y, win->w, win->h, MAX_VIDEO_DIM);
        return -1;
    }
    if (scale_w < 0 || scale_h < 0 || scale_w > MAX_VIDEO_DIM || scale_h > MAX_VIDEO_DIM ||
        (scale_w > 0) != (scale_h > 0)) {
        log_error(LOGGER_TAG, "%s for session %d has invalid scale %dx%d "
                  "(set both, each 1..%d)", what, idx, scale_w, scale_h, MAX_VIDEO_DIM);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* validate_rx_config                                                 */
/* ------------------------------------------------------------------ */
int validate_rx_config(const struct rx_config* config) {
    if (!config) return -1;

    if (config->num_interfaces < 1) {
        log_error(LOGGER_TAG, "At least one interface is required");
        return -1;
    }
    for (int i = 0; i < config->num_interfaces; i++) {
        const struct interface_config* ifc = &config->interfaces[i];
        struct in_addr addr;
        if (ifc->name[0] == '\0') {
            log_error(LOGGER_TAG, "interfaces[%d] has no PCI name", i);
            return -1;
        }
        if (inet_pton(AF_INET, ifc->sip, &addr) != 1) {
            log_error(LOGGER_TAG, "interfaces[%d] (%s) has invalid source IP '%s'",
                      i, ifc->name, ifc->sip);
            return -1;
        }
        if (ifc->dip[0] == '\0') {
            log_error(LOGGER_TAG, "interfaces[%d] (%s) has no destination IP (dip)", i, ifc->name);
            return -1;
        }
        if (inet_pton(AF_INET, ifc->dip, &addr) != 1) {
            log_error(LOGGER_TAG, "interfaces[%d] (%s) has invalid destination IP '%s'",
                      i, ifc->name, ifc->dip);
            return -1;
        }
        for (int j = 0; j < i; j++) {
            if (config->interfaces[j].nic_index == ifc->nic_index) {
                log_error(LOGGER_TAG, "interfaces[%d] and [%d] share nic_index %d",
                          j, i, ifc->nic_index);
                return -1;
            }
            if (strcmp(config->interfaces[j].name, ifc->name) == 0) {
                log_error(LOGGER_TAG, "interfaces[%d] and [%d] use the same NIC %s",
                          j, i, ifc->name);
                return -1;
            }
        }
    }
    const struct video_config* v = &config->video;
    if (v->width <= 0 || v->height <= 0 || v->width > MAX_VIDEO_DIM || v->height > MAX_VIDEO_DIM ||
        (long)v->width * v->height > MAX_VIDEO_PIXELS) {
        log_error(LOGGER_TAG, "Invalid video size %dx%d (max 4K: 3840x2160 or 2160x3840)",
                  v->width, v->height);
        return -1;
    }
    static const int fps_ok[] = {24, 25, 29, 30, 50, 59, 60};
    bool fps_valid = false;
    for (size_t i = 0; i < sizeof(fps_ok) / sizeof(fps_ok[0]); i++)
        fps_valid |= (v->fps == fps_ok[i]);
    if (!fps_valid) {
        log_error(LOGGER_TAG, "Invalid fps %d (use 24|25|29|30|50|59|60, max %d)", v->fps, MAX_FPS);
        return -1;
    }
    int bpp = fmt_bits_per_pixel(v->fmt);
    if (bpp < 0) {
        log_error(LOGGER_TAG, "Unknown fmt '%s' (use yuv422p10le|yuv422p12le|yuv444p10le|"
                  "yuv444p12le|gbrp10le|gbrp12le|yuv420)", v->fmt);
        return -1;
    }
    if (config->num_rx_sessions < 1) {
        log_error(LOGGER_TAG, "At least one rx_session is required");
        return -1;
    }
    if (config->display_mode[0] != '\0' &&
        strcmp(config->display_mode, "composite") != 0 &&
        strcmp(config->display_mode, "per_output") != 0) {
        log_error(LOGGER_TAG, "Invalid display_mode '%s' (use composite|per_output)",
                  config->display_mode);
        return -1;
    }
    double nic_mbps[MAX_INTERFACES] = {0};
    for (int i = 0; i < config->num_rx_sessions; i++) {
        const struct rx_session_config* s = &config->rx_sessions[i];
        if (s->crop.w <= 0 || s->crop.h <= 0 || s->crop.x < 0 || s->crop.y < 0) {
            log_error(LOGGER_TAG, "rx_session[%d] has invalid crop %d,%d %dx%d",
                      i, s->crop.x, s->crop.y, s->crop.w, s->crop.h);
            return -1;
        }
        if (s->crop.x + s->crop.w > v->width || s->crop.y + s->crop.h > v->height) {
            log_error(LOGGER_TAG, "rx_session[%d] crop %d,%d %dx%d is outside the %dx%d video",
                      i, s->crop.x, s->crop.y, s->crop.w, s->crop.h, v->width, v->height);
            return -1;
        }
        if (s->udp_port < 1 || s->udp_port > 65535) {
            log_error(LOGGER_TAG, "rx_session[%d] has invalid udp_port %d", i, s->udp_port);
            return -1;
        }
        if (s->payload_type < 96 || s->payload_type > 127) {
            log_error(LOGGER_TAG, "rx_session[%d] payload_type %d not in dynamic range 96..127",
                      i, s->payload_type);
            return -1;
        }
        for (int j = 0; j < i; j++) {
            if (config->rx_sessions[j].nic_index == s->nic_index &&
                config->rx_sessions[j].udp_port == s->udp_port) {
                log_error(LOGGER_TAG, "rx_session[%d] and [%d] both use nic_index %d udp_port %d",
                          j, i, s->nic_index, s->udp_port);
                return -1;
            }
        }
        int port = rx_config_port_for_nic(config, s->nic_index);
        if (port < 0) {
            log_error(LOGGER_TAG, "rx_session[%d] uses nic_index %d, which no interface has",
                      i, s->nic_index);
            return -1;
        }
        /* +5% for RTP/UDP/IP/Ethernet headers (matches measured MTL rx rate). */
        nic_mbps[port] += (double)s->crop.w * s->crop.h * bpp * v->fps * 1.05 / 1e6;

        const struct crop_config no_window = {0, 0, 0, 0};
        if (check_window("window", i, s->display_assigned ? s->output : NULL,
                         s->has_window ? &s->window : &no_window, s->scale_w, s->scale_h) < 0)
            return -1;
        for (int m = 0; m < s->num_mirrors; m++) {
            const struct display_placement* pl = &s->mirrors[m];
            if (check_window("mirror", i, pl->output, &pl->window, pl->scale_w, pl->scale_h) < 0)
                return -1;
        }
    }
    for (int p = 0; p < config->num_interfaces; p++) {
        if (nic_mbps[p] > NIC_LINK_MBPS) {
            log_warn(LOGGER_TAG, "interfaces[%d] (%s) needs ~%.0f Mb/s, above a %d Mb/s link",
                     p, config->interfaces[p].name, nic_mbps[p], NIC_LINK_MBPS);
        }
    }
    if (config->log_file[0] != '\0') {
        char why[512];
        if (!logger_path_is_safe(config->log_file, why, sizeof(why))) {
            log_error(LOGGER_TAG, "log_file '%s' rejected: %s", config->log_file, why);
            return -1;
        }
    }

    log_info(LOGGER_TAG, "Configuration validation passed (%d session%s, %d interface%s)",
             config->num_rx_sessions,
             config->num_rx_sessions > 1 ? "s - multi-display" : " - single display",
             config->num_interfaces, config->num_interfaces > 1 ? "s" : "");
    return 0;
}

/* ------------------------------------------------------------------ */
/* load_and_validate_rx_config                                        */
/* ------------------------------------------------------------------ */
int load_and_validate_rx_config(const char* config_file, struct rx_config* config) {
    if (!config_file || !config || config_file[0] == '\0') return -1;

    if (parse_rx_config(config_file, config) != 0 ||
        validate_rx_config(config) != 0) {
        log_error(LOGGER_TAG, "Config file %s is invalid", config_file);
        return -1;
    }

    log_info(LOGGER_TAG, "Loaded configuration from: %s", config_file);
    return 0;
}

int rx_config_port_for_nic(const struct rx_config* config, int nic_index) {
    for (int i = 0; i < config->num_interfaces; i++) {
        if (config->interfaces[i].nic_index == nic_index) return i;
    }
    return -1;
}