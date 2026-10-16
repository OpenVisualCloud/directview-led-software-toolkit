/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "frame_converter.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// Include MTL headers for st_frame definition
#include "st_pipeline_api.h"

#define LOGGER_TAG "converter"

/*
 * YUV/RGB conversion of received ST2110 frames into the BGRA buffers shown by
 * the X11 display, with proper bit-depth scaling and chroma subsampling.
 */

static inline uint8_t clamp_u8(int val) {
	return (val < 0) ? 0 : (val > 255) ? 255 : val;
}

void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
	// ITU-R BT.601 conversion (studio range)
	int c = y - 16;
	int d = u - 128;
	int e = v - 128;
	*r = clamp_u8((298 * c + 409 * e + 128) >> 8);
	*g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
	*b = clamp_u8((298 * c + 516 * d + 128) >> 8);
}

/* --- BT.601 conversion lookup tables -------------------------------------
 * The per-pixel YUV->RGB math reduces to a few multiplies that depend only
 * on the 8-bit Y/U/V inputs.  Precomputing them as tables turns the inner
 * loop into pure lookups + adds (no multiplies, no function call), which is
 * the difference between keeping up with 30fps at 2560x1440 and not.
 *
 *   r = 298*(Y-16)            + 409*(V-128) + 128
 *   g = 298*(Y-16) - 100*(U-128) - 208*(V-128) + 128
 *   b = 298*(Y-16) + 516*(U-128)              + 128
 */
static int g_yc_tab[256];   /* 298*(Y-16) + 128 (rounding folded in) */
static int g_rv_tab[256];   /* 409*(V-128) */
static int g_gu_tab[256];   /* -100*(U-128) */
static int g_gv_tab[256];   /* -208*(V-128) */
static int g_bu_tab[256];   /* 516*(U-128) */
static pthread_once_t g_yuv_tabs_once = PTHREAD_ONCE_INIT;

static void init_yuv_tables_impl(void) {
	for (int i = 0; i < 256; i++) {
		g_yc_tab[i] = 298 * (i - 16) + 128;
		g_rv_tab[i] = 409 * (i - 128);
		g_gu_tab[i] = -100 * (i - 128);
		g_gv_tab[i] = -208 * (i - 128);
		g_bu_tab[i] = 516 * (i - 128);
	}
}

static void init_yuv_tables(void) {
	pthread_once(&g_yuv_tabs_once, init_yuv_tables_impl);
}

int get_format_bit_shift(int format) {
	switch (format) {
		case ST_FRAME_FMT_YUV422PLANAR12LE:
		case ST_FRAME_FMT_YUV444PLANAR12LE:
		case ST_FRAME_FMT_GBRPLANAR12LE:
			return 4; // 12-bit formats
		case ST_FRAME_FMT_YUV422PLANAR10LE:
		case ST_FRAME_FMT_YUV444PLANAR10LE:
		case ST_FRAME_FMT_GBRPLANAR10LE:
			return 2; // 10-bit formats
		case ST_FRAME_FMT_YUV420PLANAR8:	case ST_FRAME_FMT_YUV420CUSTOM8:		default:
			return 0; // 8-bit formats
	}
}

bool is_yuv422_format(int format) {
	return (format == ST_FRAME_FMT_YUV422PLANAR10LE || format == ST_FRAME_FMT_YUV422PLANAR12LE);
}

/* Convert frame to RGB format for X11 display */
int convert_frame_to_rgb(struct st_frame* frame, uint8_t* rgb_buffer, uint32_t width, uint32_t height) {
	if (!frame || !rgb_buffer) {
		log_error(LOGGER_TAG, "convert_frame_to_rgb - NULL pointer (frame=%p, buffer=%p)", frame, rgb_buffer);
		return -1;
	}
	
	if (!frame->addr[0]) {
		log_error(LOGGER_TAG, "convert_frame_to_rgb - No frame data (addr[0]=NULL)");
		return -1;
	}
	
	if (frame->width != width || frame->height != height) {
		log_error(LOGGER_TAG, "convert_frame_to_rgb - Size mismatch (frame=%dx%d, target=%dx%d)",
		       frame->width, frame->height, width, height);
		return -1;
	}

	switch (frame->fmt) {
		case ST_FRAME_FMT_BGRA: {
			/* MTL already converted to BGRA — direct memcpy, no pixel conversion needed */
			memcpy(rgb_buffer, frame->addr[0], width * height * 4);
			break;
		}

		case ST_FRAME_FMT_YUV422PLANAR10LE:
		case ST_FRAME_FMT_YUV422PLANAR12LE: {
			uint16_t* src_y = (uint16_t*)frame->addr[0];
			uint16_t* src_u = (uint16_t*)frame->addr[1];
			uint16_t* src_v = (uint16_t*)frame->addr[2];
			int bit_shift = (frame->fmt == ST_FRAME_FMT_YUV422PLANAR12LE) ? 4 : 2;

			// Use linesize from frame (in bytes), convert to stride in pixels for 16-bit data
			size_t y_stride = frame->linesize[0] / 2;  // Y linesize in 16-bit pixels
			size_t u_stride = frame->linesize[1] / 2;  // U linesize in 16-bit pixels
			size_t v_stride = frame->linesize[2] / 2;  // V linesize in 16-bit pixels

			init_yuv_tables();

			for (uint32_t y = 0; y < height; y++) {
				const uint16_t* yrow = src_y + (size_t)y * y_stride;
				const uint16_t* urow = src_u + (size_t)y * u_stride;
				const uint16_t* vrow = src_v + (size_t)y * v_stride;
				uint8_t* drow = rgb_buffer + (size_t)y * width * 4;

				for (uint32_t x = 0; x < width; x++) {
					// YUV422: horizontal subsampling for U,V
					uint8_t y_val = (yrow[x]     >> bit_shift) & 0xFF;
					uint8_t u_val = (urow[x >> 1] >> bit_shift) & 0xFF;
					uint8_t v_val = (vrow[x >> 1] >> bit_shift) & 0xFF;

					int yc = g_yc_tab[y_val];
					int r = (yc + g_rv_tab[v_val]) >> 8;
					int g = (yc + g_gu_tab[u_val] + g_gv_tab[v_val]) >> 8;
					int b = (yc + g_bu_tab[u_val]) >> 8;

					uint8_t* px = drow + (size_t)x * 4;
					px[0] = clamp_u8(b);     // Blue
					px[1] = clamp_u8(g);     // Green
					px[2] = clamp_u8(r);     // Red
					px[3] = 255;             // Alpha
				}
			}
			break;
		}

		case ST_FRAME_FMT_YUV444PLANAR10LE:
		case ST_FRAME_FMT_YUV444PLANAR12LE: {
			uint16_t* src_y = (uint16_t*)frame->addr[0];
			uint16_t* src_u = (uint16_t*)frame->addr[1];
			uint16_t* src_v = (uint16_t*)frame->addr[2];
			int bit_shift = (frame->fmt == ST_FRAME_FMT_YUV444PLANAR12LE) ? 4 : 2;

			size_t y_stride = frame->linesize[0] / 2;
			size_t u_stride = frame->linesize[1] / 2;
			size_t v_stride = frame->linesize[2] / 2;

			init_yuv_tables();

			for (uint32_t y = 0; y < height; y++) {
				const uint16_t* yrow = src_y + (size_t)y * y_stride;
				const uint16_t* urow = src_u + (size_t)y * u_stride;
				const uint16_t* vrow = src_v + (size_t)y * v_stride;
				uint8_t* drow = rgb_buffer + (size_t)y * width * 4;

				for (uint32_t x = 0; x < width; x++) {
					// YUV444: no subsampling, each pixel has its own U,V
					uint8_t y_val = (yrow[x] >> bit_shift) & 0xFF;
					uint8_t u_val = (urow[x] >> bit_shift) & 0xFF;
					uint8_t v_val = (vrow[x] >> bit_shift) & 0xFF;

					int yc = g_yc_tab[y_val];
					int r = (yc + g_rv_tab[v_val]) >> 8;
					int g = (yc + g_gu_tab[u_val] + g_gv_tab[v_val]) >> 8;
					int b = (yc + g_bu_tab[u_val]) >> 8;

					uint8_t* px = drow + (size_t)x * 4;
					px[0] = clamp_u8(b);
					px[1] = clamp_u8(g);
					px[2] = clamp_u8(r);
					px[3] = 255;
				}
			}
			break;
		}

		case ST_FRAME_FMT_GBRPLANAR10LE:
		case ST_FRAME_FMT_GBRPLANAR12LE: {
			/* GBR planar 10-bit: plane 0 = Green, plane 1 = Blue, plane 2 = Red */
			uint16_t* src_g = (uint16_t*)frame->addr[0];
			uint16_t* src_b = (uint16_t*)frame->addr[1];
			uint16_t* src_r = (uint16_t*)frame->addr[2];
			int bit_shift = (frame->fmt == ST_FRAME_FMT_GBRPLANAR12LE) ? 4 : 2;

			size_t g_stride = frame->linesize[0] / 2;
			size_t b_stride = frame->linesize[1] / 2;
			size_t r_stride = frame->linesize[2] / 2;

			for (uint32_t y = 0; y < height; y++) {
				const uint16_t* grow = src_g + (size_t)y * g_stride;
				const uint16_t* brow = src_b + (size_t)y * b_stride;
				const uint16_t* rrow = src_r + (size_t)y * r_stride;
				uint8_t* drow = rgb_buffer + (size_t)y * width * 4;

				for (uint32_t x = 0; x < width; x++) {
					uint8_t* px = drow + (size_t)x * 4;
					px[0] = (brow[x] >> bit_shift) & 0xFF;
					px[1] = (grow[x] >> bit_shift) & 0xFF;
					px[2] = (rrow[x] >> bit_shift) & 0xFF;
					px[3] = 255;
				}
			}
			break;
		}

		case ST_FRAME_FMT_YUV420PLANAR8: {
			uint8_t* src_y = (uint8_t*)frame->addr[0];
			uint8_t* src_u = (uint8_t*)frame->addr[1];
			uint8_t* src_v = (uint8_t*)frame->addr[2];

			for (uint32_t y = 0; y < height; y++) {
				for (uint32_t x = 0; x < width; x++) {
					uint8_t y_val = src_y[y * width + x];
					uint8_t u_val = src_u[(y/2) * (width/2) + (x/2)];
					uint8_t v_val = src_v[(y/2) * (width/2) + (x/2)];

					uint8_t r, g, b;
					yuv_to_rgb(y_val, u_val, v_val, &r, &g, &b);

					uint32_t idx = (y * width + x) * 4;
					rgb_buffer[idx + 0] = b;     // Blue
					rgb_buffer[idx + 1] = g;     // Green
					rgb_buffer[idx + 2] = r;     // Red
					rgb_buffer[idx + 3] = 255;   // Alpha
				}
			}
			break;
		}

		case ST_FRAME_FMT_YUV420CUSTOM8: {
			/* I420 planar in single buffer: Y(w*h) + U(w/2*h/2) + V(w/2*h/2)
			 * Optimised: process 2 horizontal pixels at a time sharing U/V,
			 * inline BT.601 conversion with pre-computed chroma terms. */
			uint8_t* base = (uint8_t*)frame->addr[0];
			uint32_t y_plane_sz = width * height;
			uint32_t uv_plane_sz = (width / 2) * (height / 2);
			const uint8_t* src_y = base;
			const uint8_t* src_u = base + y_plane_sz;
			const uint8_t* src_v = base + y_plane_sz + uv_plane_sz;
			uint32_t half_w = width / 2;

			for (uint32_t row = 0; row < height; row++) {
				const uint8_t* y_row = src_y + row * width;
				const uint8_t* u_row = src_u + (row / 2) * half_w;
				const uint8_t* v_row = src_v + (row / 2) * half_w;
				uint8_t* dst = rgb_buffer + row * width * 4;

				for (uint32_t cx = 0; cx < half_w; cx++) {
					int d = (int)u_row[cx] - 128;
					int e = (int)v_row[cx] - 128;
					/* Pre-compute chroma contributions (shared by 2 pixels) */
					int cr_r = 409 * e + 128;
					int cr_g = -100 * d - 208 * e + 128;
					int cr_b = 516 * d + 128;

					for (int k = 0; k < 2; k++) {
						int c = (int)y_row[cx * 2 + k] - 16;
						int c298 = 298 * c;
						int rv = (c298 + cr_r) >> 8;
						int gv = (c298 + cr_g) >> 8;
						int bv = (c298 + cr_b) >> 8;
						if (rv < 0) rv = 0; else if (rv > 255) rv = 255;
						if (gv < 0) gv = 0; else if (gv > 255) gv = 255;
						if (bv < 0) bv = 0; else if (bv > 255) bv = 255;
						dst[0] = (uint8_t)bv;
						dst[1] = (uint8_t)gv;
						dst[2] = (uint8_t)rv;
						dst[3] = 255;
						dst += 4;
					}
				}
			}
			break;
		}

		default:
			log_warn(LOGGER_TAG, "Unsupported format %d for X11 display, filling with test pattern", frame->fmt);
			// Fill with test pattern
			for (uint32_t y = 0; y < height; y++) {
				for (uint32_t x = 0; x < width; x++) {
					uint32_t idx = (y * width + x) * 4;
					rgb_buffer[idx + 0] = (x + y) % 256;     // Blue gradient
					rgb_buffer[idx + 1] = x % 256;           // Green gradient
					rgb_buffer[idx + 2] = y % 256;           // Red gradient
					rgb_buffer[idx + 3] = 255;               // Alpha
				}
			}
			break;
	}

	return 0;
}

/* Same sampling as scale_bgra_nearest() in x11_handler.c, but only the sampled
 * source pixels are converted, so cost scales with the output size. */
int convert_frame_to_rgb_scaled(struct st_frame* frame, uint8_t* rgb_buffer, uint32_t dst_w, uint32_t dst_h) {
	if (!frame || !rgb_buffer || !frame->addr[0] || !frame->addr[1] || !frame->addr[2] ||
	    !dst_w || !dst_h || !frame->width || !frame->height)
		return -1;

	bool yuv422 = frame->fmt == ST_FRAME_FMT_YUV422PLANAR10LE || frame->fmt == ST_FRAME_FMT_YUV422PLANAR12LE;
	bool yuv444 = frame->fmt == ST_FRAME_FMT_YUV444PLANAR10LE || frame->fmt == ST_FRAME_FMT_YUV444PLANAR12LE;
	bool gbr    = frame->fmt == ST_FRAME_FMT_GBRPLANAR10LE    || frame->fmt == ST_FRAME_FMT_GBRPLANAR12LE;
	if (!yuv422 && !yuv444 && !gbr) return -1;

	uint32_t src_w = frame->width, src_h = frame->height;
	uint32_t* sx_tab = malloc((size_t)dst_w * sizeof(*sx_tab));
	if (!sx_tab) return -1;
	for (uint32_t x = 0; x < dst_w; x++)
		sx_tab[x] = (uint32_t)(((uint64_t)x * src_w) / dst_w);

	const uint16_t* p0 = (const uint16_t*)frame->addr[0];
	const uint16_t* p1 = (const uint16_t*)frame->addr[1];
	const uint16_t* p2 = (const uint16_t*)frame->addr[2];
	size_t s0 = frame->linesize[0] / 2, s1 = frame->linesize[1] / 2, s2 = frame->linesize[2] / 2;
	int bit_shift = get_format_bit_shift(frame->fmt);
	int uv_shift = yuv422 ? 1 : 0;

	init_yuv_tables();

	for (uint32_t y = 0; y < dst_h; y++) {
		uint32_t sy = (uint32_t)(((uint64_t)y * src_h) / dst_h);
		const uint16_t* r0 = p0 + (size_t)sy * s0;
		const uint16_t* r1 = p1 + (size_t)sy * s1;
		const uint16_t* r2 = p2 + (size_t)sy * s2;
		uint8_t* drow = rgb_buffer + (size_t)y * dst_w * 4;

		if (gbr) {
			for (uint32_t x = 0; x < dst_w; x++) {
				uint32_t sx = sx_tab[x];
				uint8_t* px = drow + (size_t)x * 4;
				px[0] = (r1[sx] >> bit_shift) & 0xFF;
				px[1] = (r0[sx] >> bit_shift) & 0xFF;
				px[2] = (r2[sx] >> bit_shift) & 0xFF;
				px[3] = 255;
			}
			continue;
		}

		for (uint32_t x = 0; x < dst_w; x++) {
			uint32_t sx = sx_tab[x];
			uint32_t cx = sx >> uv_shift;
			uint8_t y_val = (r0[sx] >> bit_shift) & 0xFF;
			uint8_t u_val = (r1[cx] >> bit_shift) & 0xFF;
			uint8_t v_val = (r2[cx] >> bit_shift) & 0xFF;

			int yc = g_yc_tab[y_val];
			uint8_t* px = drow + (size_t)x * 4;
			px[0] = clamp_u8((yc + g_bu_tab[u_val]) >> 8);
			px[1] = clamp_u8((yc + g_gu_tab[u_val] + g_gv_tab[v_val]) >> 8);
			px[2] = clamp_u8((yc + g_rv_tab[v_val]) >> 8);
			px[3] = 255;
		}
	}

	free(sx_tab);
	return 0;
}
