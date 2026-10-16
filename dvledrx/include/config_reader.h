/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_INTERFACES   8   /* MTL_PORT_MAX */
#define MAX_RX_SESSIONS  16
#define MAX_MIRRORS      4   /* extra outputs one session can be shown on */

/* Validation limits */
#define MAX_VIDEO_DIM    3840               /* either orientation, e.g. 3840x2160 or 2160x3840 */
#define MAX_VIDEO_PIXELS (3840 * 2160)      /* 4K UHD */
#define MAX_FPS          60
#define NIC_LINK_MBPS    10000              /* warn above this per NIC */

/* Interface configuration (from JSON "interfaces" array) */
struct interface_config {
    int nic_index;            /* id referenced by rx_sessions[].nic_index */
    char name[64];            /* PCI address, e.g. "0000:02:00.0" */
    char sip[32];             /* Source IP of this NIC */
    char dip[32];             /* Multicast destination IP */
};

/* Crop region within the full video frame */
struct crop_config {
    int x;                    /* Horizontal offset in pixels */
    int y;                    /* Vertical offset in pixels */
    int w;                    /* Width in pixels */
    int h;                    /* Height in pixels */
};

/* Additional placement of a session on another output (from "displays") */
struct display_placement {
    char output[32];
    struct crop_config window;
    int scale_w;
    int scale_h;
};

/* Per-stream RX session configuration (from JSON "rx_sessions" array) */
struct rx_session_config {
    int nic_index;            /* interface this stream arrives on (default 0) */
    int udp_port;             /* UDP port for this stream */
    int payload_type;         /* RTP payload type */
    struct crop_config crop;  /* Crop region within the full frame */
    char output[32];          /* XRandR output name, e.g. "HDMI-1" (empty = by index) */
    bool has_window;          /* "window" given: fixed-size window instead of fullscreen */
    struct crop_config window; /* x/y relative to the output; w/h 0 = crop size */
    int scale_w;              /* window/video size; overrides window.w/h (0 = unset) */
    int scale_h;
    bool display_assigned;    /* placed by the top-level "displays" section */
    struct display_placement mirrors[MAX_MIRRORS]; /* 2nd+ placements from "displays" */
    int num_mirrors;
};

/* Video parameters (from JSON "video" object) */
struct video_config {
    int width;                /* Full source video width */
    int height;               /* Full source video height */
    int fps;                  /* Frames per second */
    char fmt[32];             /* Pixel format, e.g. "yuv422p10le" */
};

/* Top-level configuration parsed from the JSON file */
struct rx_config {
    struct interface_config interfaces[MAX_INTERFACES];
    int num_interfaces;

    struct video_config video;

    struct rx_session_config rx_sessions[MAX_RX_SESSIONS];
    int num_rx_sessions;

    char log_file[256];       /* Optional log file path (empty = console) */
    char log_level[16];       /* Optional log level: debug|info|warn|error */
    char display_mode[16];    /* "composite" (default) | "per_output" */
    bool tasklet_sleep;       /* let the MTL scheduler sleep when idle (saves a busy core) */
};

/* Parse JSON configuration file into rx_config.  Returns 0 on success. */
int parse_rx_config(const char* config_file, struct rx_config* config);

/* Validate a parsed configuration.  Returns 0 on success. */
int validate_rx_config(const struct rx_config* config);

/* Convenience: parse + validate in one call. */
int load_and_validate_rx_config(const char* config_file, struct rx_config* config);

/* Position in config->interfaces (== MTL port index) of nic_index, or -1. */
int rx_config_port_for_nic(const struct rx_config* config, int nic_index);
