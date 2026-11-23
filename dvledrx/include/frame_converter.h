/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

// Forward declaration for st_frame
struct st_frame;

// Common YUV to RGB conversion functions
void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b);

// Get bit shift value for format
int get_format_bit_shift(int format);

// Check if format uses YUV422 subsampling
bool is_yuv422_format(int format);

// X11 display conversion function
int convert_frame_to_rgb(struct st_frame* frame, uint8_t* rgb_buffer, uint32_t width, uint32_t height);

// Nearest-neighbour resize fused with conversion (YUV422/YUV444/GBR planar only; -1 otherwise)
int convert_frame_to_rgb_scaled(struct st_frame* frame, uint8_t* rgb_buffer, uint32_t dst_w, uint32_t dst_h);
