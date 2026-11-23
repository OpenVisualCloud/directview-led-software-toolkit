/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef X11_HANDLER_H
#define X11_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

// Forward declarations
struct st_frame;
struct crop_config;

enum {
  X11_WALL_NONE = 0,   /* ordinary decorated window: Escape does nothing */
  X11_WALL_FULLSCREEN, /* EWMH fullscreen on an output */
  X11_WALL_FIXED,      /* override-redirect window at a fixed spot on an output */
};

/* X11 Display context */
struct x11_display_ctx {
  void* display;     /* Display* - X11 display connection */
  void* window;      /* Window - X11 window handle */
  void* gc;          /* GC - Graphics context */
  void* visual;      /* Visual* - X11 visual info */
  void* ximage;      /* XImage* - X11 image buffer */
  int screen;        /* Screen number */
  uint32_t width;    /* Window width */
  uint32_t height;   /* Window height */
  uint32_t frame_width;   /* Latest incoming frame width */
  uint32_t frame_height;  /* Latest incoming frame height */
  int depth;         /* Color depth */
  unsigned long colormap; /* Colormap of a 32-bit ARGB window, else 0 */
  char* image_data;  /* Image buffer */
  char* frame_scratch; /* Frame-sized intermediate BGRA buffer */
  size_t frame_scratch_cap; /* Allocated bytes in frame_scratch */
  void* shminfo;     /* XShmSegmentInfo* when image_data is MIT-SHM backed, else NULL */
  bool use_shm;      /* Try MIT-SHM when (re)allocating the image buffer */
  uint32_t frame_counter;
  char output_name[32]; /* XRandR output the window is on, or "(window)" */
  int out_x, out_y;     /* window position relative to that output */
  int wall_kind;        /* X11_WALL_* : how the window is shown in wall mode */
  int mon_x, mon_y, mon_w, mon_h; /* output geometry in root coordinates */
  uint32_t home_w, home_h;        /* wall-mode window size */
  int monitor_idx;      /* Xinerama index of the output, -1 if none */
  bool windowed;        /* currently a normal decorated window (Escape toggles) */
  bool wall_pending;    /* waiting for the WM to release the window before going back */
  long long pending_ms; /* when wall_pending was set (CLOCK_MONOTONIC) */
  bool initialized;  /* Initialization status */
  pthread_mutex_t xlock; /* Protect Display* from concurrent RX/main-thread access */
};

/*
 * Composite display context - single X11 window showing N side-by-side strips.
 * All N session threads write their strip, hit a barrier, then session 0 flushes.
 */
struct composite_x11_ctx {
  void* display;           /* Display* */
  void* window;            /* Window (full composite width) */
  void* gc;                /* GC */
  void* visual;            /* Visual* */
  void* ximage;            /* XImage* (full_width × height) - native composite buffer */
  int screen;
  uint32_t full_width;     /* native composite width (sum of crop regions) */
  uint32_t strip_width;    /* pixel width of one session's strip (legacy) */
  uint32_t height;         /* native composite height */
  int depth;
  char* image_data;        /* full_width * height * 4 bytes (BGRA) - native composite buffer */
  /* Per-frame barrier for the session threads; composite_abort() releases it for shutdown. */
  pthread_mutex_t bar_lock;
  pthread_cond_t bar_cond;
  int bar_count;           /* threads waiting in the current round */
  unsigned bar_round;
  bool bar_aborted;
  pthread_mutex_t xlock;      /* protect X11 Display* from concurrent access */
  int num_sessions;
  bool initialized;

  /* Current on-screen window size. Starts equal to full_width/height, but
   * diverges when the window manager resizes/fullscreens the window (e.g.
   * user hits the fullscreen shortcut). When it differs, the native
   * composite buffer is nearest-neighbour scaled into scaled_image_data /
   * scaled_ximage before XPutImage, so the composite window fills the
   * screen the same way a single-session window does. */
  uint32_t win_width;
  uint32_t win_height;
  void* scaled_ximage;      /* XImage* (win_width × win_height); NULL when win size == native size */
  char* scaled_image_data;  /* backing store for scaled_ximage */
};

/* Per-session X11 initialization and cleanup.
 * fullscreen_on_output=false: normal window of width x height.
 * fullscreen_on_output=true : borderless fullscreen window on the XRandR output
 *   named output_name (e.g. "HDMI-1"), or the session_idx-th active monitor
 *   when output_name is NULL/empty.
 * window_rect (only with fullscreen_on_output): non-NULL gives a fixed borderless
 *   window of window_rect->w x h at (x, y) relative to the output, instead of fullscreen. */
int init_x11_display(struct x11_display_ctx* display, uint32_t width, uint32_t height,
                     int session_idx, const char* output_name, bool fullscreen_on_output,
                     const struct crop_config* window_rect);
void cleanup_x11_display(struct x11_display_ctx* display);

/* Per-session frame display */
void display_frame_x11(struct x11_display_ctx* display, struct st_frame* frame);

/* Show src's last rendered image in dst when both windows are the same size.
 * Returns false (nothing drawn) when sizes differ. */
bool display_copy_x11(struct x11_display_ctx* dst, struct x11_display_ctx* src);

/* Per-session event processing.  Escape in any window toggles every wall window
 * between wall mode and a normal window with title bar (minimize/maximize/close). */
void process_x11_events(struct x11_display_ctx* display);

/* True once the user clicked a window's close button. */
bool x11_close_requested(void);

/* Composite display initialization / cleanup.
 * strip_width is the total display width (not per-strip). */
int init_composite_x11_display(struct composite_x11_ctx* comp,
                                uint32_t strip_width, uint32_t height,
                                int num_sessions);
void cleanup_composite_x11_display(struct composite_x11_ctx* comp);

/* Release every session thread waiting at the composite barrier, now and from now on. */
void composite_abort(struct composite_x11_ctx* comp);

/*
 * Write one strip to the composite buffer, wait at barrier, then
 * session 0 calls XPutImage to flush the complete frame.
 */
void composite_write_strip(struct composite_x11_ctx* comp,
                            struct st_frame* frame, int session_idx);

/*
 * Write a frame into the composite buffer at an arbitrary crop position.
 * The frame dimensions are crop->w × crop->h and it is placed at (crop->x, crop->y).
 * After all sessions have called this, the serial-thread flushes the display.
 */
void composite_write_crop(struct composite_x11_ctx* comp,
                           struct st_frame* frame,
                           const struct crop_config* crop);

/* Event processing for the composite window */
void process_composite_x11_events(struct composite_x11_ctx* comp);

#endif /* X11_HANDLER_H */
