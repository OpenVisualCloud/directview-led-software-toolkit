/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2026 Intel Corporation
 */

#include "config_reader.h"
#include "tx_app_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper function to extract string value from JSON
static char* extract_json_string(const char* json, const char* key, char* buffer, size_t buffer_size) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);

    char* key_pos = strstr(json, search_key);
    if (!key_pos) return NULL;

    // Find the colon after the key
    char* colon_pos = strchr(key_pos, ':');
    if (!colon_pos) return NULL;

    // Skip whitespace and find opening quote of value
    char* value_start = colon_pos + 1;
    while (*value_start && (*value_start == ' ' || *value_start == '\t' || *value_start == '\n')) {
        value_start++;
    }

    if (*value_start != '"') return NULL;
    value_start++; // Skip opening quote

    char* value_end = strchr(value_start, '"');
    if (!value_end) return NULL;

    size_t value_len = value_end - value_start;
    if (value_len >= buffer_size) value_len = buffer_size - 1;

    strncpy(buffer, value_start, value_len);
    buffer[value_len] = '\0';
    return buffer;
}

// Helper function to extract integer value from JSON
static int extract_json_int(const char* json, const char* key) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);

    char* key_pos = strstr(json, search_key);
    if (!key_pos) return -1;

    char* value_start = key_pos + strlen(search_key);
    while (*value_start == ' ' || *value_start == '\t') value_start++;

    return atoi(value_start);
}

// Parse TX JSON configuration file using standard MTL config format
int parse_tx_config(const char* config_file, struct tx_config* config) {
    FILE* file = fopen(config_file, "r");
    if (!file) {
        printf("Error: Cannot open config file %s\n", config_file);
        return -1;
    }

    // Read entire file
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* json_content = malloc(file_size + 1);
    if (!json_content) {
        fclose(file);
        return -1;
    }

    fread(json_content, 1, file_size, file);
    json_content[file_size] = '\0';
    fclose(file);

    // Initialize config with defaults
    memset(config, 0, sizeof(struct tx_config));

    // Parse interfaces section - find first interface
    char* interfaces_section = strstr(json_content, "\"interfaces\":");
    if (interfaces_section) {
        char* interface_start = strchr(interfaces_section, '[');
        if (interface_start) {
            char* first_interface = strchr(interface_start, '{');
            if (first_interface) {
                extract_json_string(first_interface, "name", config->interface_name, sizeof(config->interface_name));
                extract_json_string(first_interface, "ip", config->interface_ip, sizeof(config->interface_ip));
            }
        }
    }

    // Parse tx_sessions section - find first session
    char* tx_sessions_section = strstr(json_content, "\"tx_sessions\":");
    if (tx_sessions_section) {
        char* session_start = strchr(tx_sessions_section, '[');
        if (session_start) {
            char* first_session = strchr(session_start, '{');
            if (first_session) {
                // Parse dip array - get first entry
                char* dip_section = strstr(first_session, "\"dip\":");
                if (dip_section) {
                    char* dip_array = strchr(dip_section, '[');
                    if (dip_array) {
                        char* first_dip = strchr(dip_array, '"');
                        if (first_dip) {
                            first_dip++;
                            char* dip_end = strchr(first_dip, '"');
                            if (dip_end) {
                                size_t len = dip_end - first_dip;
                                if (len < sizeof(config->dip)) {
                                    strncpy(config->dip, first_dip, len);
                                    config->dip[len] = '\0';
                                }
                            }
                        }
                    }
                }

                // Parse st20p array - find first st20p session
                char* st20p_section = strstr(first_session, "\"st20p\":");
                if (st20p_section) {
                    config->video_enable = true;

                    char* st20p_array = strchr(st20p_section, '[');
                    if (st20p_array) {
                        char* first_st20p = strchr(st20p_array, '{');
                        if (first_st20p) {
                            // Parse st20p parameters from standard format
                            config->start_port = extract_json_int(first_st20p, "start_port");
                            if (config->start_port <= 0) config->start_port = 20000;

                            config->payload_type = extract_json_int(first_st20p, "payload_type");
                            if (config->payload_type <= 0) config->payload_type = 112;

                            config->width = extract_json_int(first_st20p, "width");
                            if (config->width <= 0) config->width = 1920;

                            config->height = extract_json_int(first_st20p, "height");
                            if (config->height <= 0) config->height = 1080;

                            extract_json_string(first_st20p, "fps", config->fps_str, sizeof(config->fps_str));
                            if (strlen(config->fps_str) == 0) {
                                strncpy(config->fps_str, "p25", sizeof(config->fps_str) - 1);
                                config->fps_str[sizeof(config->fps_str) - 1] = '\0';
                            }

                            extract_json_string(first_st20p, "device", config->device, sizeof(config->device));
                            if (strlen(config->device) == 0) {
                                strncpy(config->device, "AUTO", sizeof(config->device) - 1);
                                config->device[sizeof(config->device) - 1] = '\0';
                            }

                            extract_json_string(first_st20p, "input_format", config->input_format, sizeof(config->input_format));
                            if (strlen(config->input_format) == 0) {
                                strncpy(config->input_format, "YUV422PLANAR10LE", sizeof(config->input_format) - 1);
                                config->input_format[sizeof(config->input_format) - 1] = '\0';
                            }

                            extract_json_string(first_st20p, "transport_format", config->transport_format, sizeof(config->transport_format));
                            if (strlen(config->transport_format) == 0) {
                                strncpy(config->transport_format, "YUV_422_10bit", sizeof(config->transport_format) - 1);
                                config->transport_format[sizeof(config->transport_format) - 1] = '\0';
                            }

                            extract_json_string(first_st20p, "st20p_url", config->st20p_url, sizeof(config->st20p_url));

                            config->fps_value = fps_string_to_value(config->fps_str);
                        }
                    }
                }

                // Parse st30p array - find first st30p session if exists
                char* st30p_section = strstr(first_session, "\"st30p\":");
                if (st30p_section) {
                    char* st30p_array = strchr(st30p_section, '[');
                    if (st30p_array) {
                        char* first_st30p = strchr(st30p_array, '{');
                        if (first_st30p) {
                            config->audio_enable = true;
                            extract_json_string(first_st30p, "audio_format", config->audio_codec, sizeof(config->audio_codec));
                            extract_json_string(first_st30p, "audio_sampling", config->audio_sampling, sizeof(config->audio_sampling));
                            extract_json_string(first_st30p, "audio_ptime", config->audio_ptime, sizeof(config->audio_ptime));
                        }
                    }
                }
            }
        }
    }

    free(json_content);
    return 0;
}

// Convert fps string to enum value (same as RX)
int fps_string_to_value(const char* fps_str) {
    if (strcmp(fps_str, "p25") == 0) return 25;
    if (strcmp(fps_str, "p30") == 0) return 30;
    if (strcmp(fps_str, "p50") == 0) return 50;
    if (strcmp(fps_str, "p59") == 0 || strcmp(fps_str, "p60") == 0) return 60;
    return 25; // Default
}

// Validate TX configuration
int validate_tx_config(const struct tx_config* config) {
    if (strlen(config->interface_name) == 0) {
        printf("Error: Interface name is required\n");
        return -1;
    }

    // Interface IP can be empty for DHCP mode
    if (strlen(config->interface_ip) == 0) {
        printf("Info: Empty interface IP - will use DHCP mode\n");
    }

    if (strlen(config->dip) == 0) {
        printf("Error: Destination IP is required\n");
        return -1;
    }

    if (config->width <= 0 || config->height <= 0) {
        printf("Error: Invalid video dimensions\n");
        return -1;
    }

    printf("Configuration validation passed\n");
    return 0;
}

// Function to load and apply JSON configuration to app context
int load_and_apply_config(struct tx_app_context* app, const char* config_file) {
    if (!app || !config_file || strlen(config_file) == 0) {
        return 0; // No config file specified, use defaults
    }

    struct tx_config config;

    // Parse the JSON configuration
    if (parse_tx_config(config_file, &config) != 0) {
        printf("Warning: Failed to parse config file %s, using defaults\n", config_file);
        return -1;
    }

    // Validate the configuration
    if (validate_tx_config(&config) != 0) {
        printf("Warning: Invalid config file %s, using defaults\n", config_file);
        return -1;
    }

    printf("Loaded configuration from: %s\n", config_file);

    // Apply configuration values to app context
    app->width  = config.width;
    app->height = config.height;
    app->fps    = config.fps_value; /* plain int: 25/30/50/60 */

    // Update network configuration
    strncpy(app->port, config.interface_name, sizeof(app->port) - 1);
    app->port[sizeof(app->port) - 1] = '\0';

    if (strlen(config.interface_ip) > 0) {
        strncpy(app->sip_addr_str, config.interface_ip, sizeof(app->sip_addr_str) - 1);
        app->sip_addr_str[sizeof(app->sip_addr_str) - 1] = '\0';
    }

    strncpy(app->dip_addr_str, config.dip, sizeof(app->dip_addr_str) - 1);
    app->dip_addr_str[sizeof(app->dip_addr_str) - 1] = '\0';

    app->udp_port = config.start_port;

    // Update video file path if specified
    if (strlen(config.st20p_url) > 0) {
        strncpy(app->tx_url, config.st20p_url, sizeof(app->tx_url) - 1);
        app->tx_url[sizeof(app->tx_url) - 1] = '\0';
    }

    printf("Using config values - Resolution: %dx%d, FPS: %s, Interface: %s\n",
           config.width, config.height, config.fps_str, config.interface_name);
    printf("Network: %s -> %s:%d, Video file: %s\n",
           config.interface_ip, config.dip, config.start_port,
           strlen(config.st20p_url) > 0 ? config.st20p_url : "test patterns");

    return 0;
}