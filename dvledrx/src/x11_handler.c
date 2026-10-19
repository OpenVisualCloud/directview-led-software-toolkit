/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#include "x11_handler.h"
#include "config_reader.h"
#include "frame_converter.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xos.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

// Include MTL headers for st_frame definition
#include "st_pipeline_api.h"

#define LOGGER_TAG "x11"

/* X11 Error handler */
static int x11_error_handler(Display *display, XErrorEvent *error) {
  char error_text[256];
  XGetErrorText(display, error->error_code, error_text, sizeof(error_text));
  
  log_error(LOGGER_TAG, "X11 Error: %s", error_text);
  log_error(LOGGER_TAG, "  Request: %d (0x%02x)", error->request_code, error->request_code);
  log_error(LOGGER_TAG, "  Minor code: %d", error->minor_code);
  log_error(LOGGER_TAG, "  Resource ID: 0x%lx", error->resourceid);
  log_error(LOGGER_TAG, "  Serial: %lu", error->serial);
  
  // Don't exit on error, just log it
  return 0;
}

/* X11 IO Error handler */
static int x11_io_error_handler(Display *display) {
  log_error(LOGGER_TAG, "X11 IO Error: Connection lost to X server");
  exit(1);
}

/* ------------------------------------------------------------------
 * MIT-SHM image buffers
 * ------------------------------------------------------------------ */
static int shm_major_opcode = -1;
static volatile int shm_attach_failed;

static int shm_attach_error_handler(Display* dpy, XErrorEvent* error) {
  if (error->request_code == shm_major_opcode) {
    shm_attach_failed = 1;
    return 0;
  }
  return x11_error_handler(dpy, error);
}

/* Try to create a w x h XImage backed by a SysV shared memory segment that
 * the X server attaches to. Only called from the main thread. */
static XImage* create_shm_image(Display* dpy, Visual* vis, int depth,
                                uint32_t w, uint32_t h, XShmSegmentInfo** out_shm) {
  int ev_base, err_base;
  if (!XShmQueryExtension(dpy) ||
      !XQueryExtension(dpy, "MIT-SHM", &shm_major_opcode, &ev_base, &err_base)) {
    return NULL;
  }

  XShmSegmentInfo* shm = calloc(1, sizeof(*shm));
  if (!shm) return NULL;

  XImage* img = XShmCreateImage(dpy, vis, (unsigned)depth, ZPixmap, NULL, shm, w, h);
  if (!img) {
    free(shm);
    return NULL;
  }
  /* Converter and scaler assume a tightly packed 32-bit BGRA row. */
  if (img->bits_per_pixel != 32 || img->bytes_per_line != (int)(w * 4)) {
    XDestroyImage(img);
    free(shm);
    return NULL;
  }

  shm->shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * img->height, IPC_CREAT | 0600);
  if (shm->shmid < 0) {
    XDestroyImage(img);
    free(shm);
    return NULL;
  }
  /* Rootless Xorg runs as the desktop user; under sudo give it the segment. */
  const char* sudo_uid = getenv("SUDO_UID");
  if (geteuid() == 0 && sudo_uid) {
    char* end = NULL;
    unsigned long uid = strtoul(sudo_uid, &end, 10);
    struct shmid_ds ds;
    if (end && *end == '\0' && shmctl(shm->shmid, IPC_STAT, &ds) == 0) {
      ds.shm_perm.uid = (uid_t)uid;
      shmctl(shm->shmid, IPC_SET, &ds);
    }
  }
  shm->shmaddr = img->data = shmat(shm->shmid, NULL, 0);
  if (shm->shmaddr == (char*)-1) {
    shmctl(shm->shmid, IPC_RMID, NULL);
    img->data = NULL;
    XDestroyImage(img);
    free(shm);
    return NULL;
  }
  shm->readOnly = False;

  XSync(dpy, False);
  shm_attach_failed = 0;
  int (*prev_handler)(Display*, XErrorEvent*) = XSetErrorHandler(shm_attach_error_handler);
  Status ok = XShmAttach(dpy, shm);
  XSync(dpy, False);
  XSetErrorHandler(prev_handler);
  /* Mark for removal now so the segment is freed even if we crash. */
  shmctl(shm->shmid, IPC_RMID, NULL);

  if (!ok || shm_attach_failed) {
    shmdt(shm->shmaddr);
    img->data = NULL;
    XDestroyImage(img);
    free(shm);
    return NULL;
  }

  *out_shm = shm;
  return img;
}

static void destroy_image_buffer(Display* dpy, XImage* img, char* data, XShmSegmentInfo* shm) {
  if (shm) {
    XShmDetach(dpy, shm);
    XSync(dpy, False); /* server must detach before we unmap */
    if (img) {
      img->data = NULL;
      XDestroyImage(img);
    }
    shmdt(shm->shmaddr);
    free(shm);
  } else {
    if (img) {
      img->data = NULL; /* we own data */
      XDestroyImage(img);
    }
    free(data);
  }
}

/* Allocate display->ximage/image_data of w x h, preferring MIT-SHM. */
static int create_image_buffer(struct x11_display_ctx* display, uint32_t w, uint32_t h) {
  Display* dpy = (Display*)display->display;
  Visual* vis = (Visual*)display->visual;
  XShmSegmentInfo* shm = NULL;
  XImage* img = NULL;

  if (display->use_shm) {
    img = create_shm_image(dpy, vis, display->depth, w, h, &shm);
    if (!img) {
      log_warn(LOGGER_TAG, "MIT-SHM unavailable (remote display?), falling back to XPutImage");
      display->use_shm = false;
    }
  }

  if (!img) {
    char* data = malloc((size_t)w * h * 4);
    if (!data) {
      log_error(LOGGER_TAG, "Cannot allocate image buffer (%ux%u)", w, h);
      return -1;
    }
    img = XCreateImage(dpy, vis, (unsigned)display->depth, ZPixmap, 0, data, w, h, 32, (int)(w * 4));
    if (!img) {
      log_error(LOGGER_TAG, "Cannot create XImage (depth=%d, size=%ux%u)", display->depth, w, h);
      free(data);
      return -1;
    }
  }

  memset(img->data, 0, (size_t)w * h * 4);
  if (display->depth == 32) {  /* ARGB window: start opaque black, not transparent */
    uint32_t* p = (uint32_t*)img->data;
    for (size_t i = 0; i < (size_t)w * h; i++) p[i] = 0xFF000000u;
  }
  display->ximage = img;
  display->image_data = img->data;
  display->shminfo = shm;
  display->width = w;
  display->height = h;
  return 0;
}

/* Blit the whole image to the window. Caller must hold display->xlock. */
static void put_image_locked(struct x11_display_ctx* display) {
  Display* dpy = (Display*)display->display;
  Window win = (Window)(uintptr_t)display->window;
  GC gc = (GC)display->gc;
  XImage* img = (XImage*)display->ximage;

  if (!img) return;
  if (display->shminfo) {
    XShmPutImage(dpy, win, gc, img, 0, 0, 0, 0, display->width, display->height, False);
    /* Server reads the segment asynchronously; wait so the next frame cannot
     * overwrite it mid-read. Runs on the display thread, not the RX thread. */
    XSync(dpy, False);
  } else {
    XPutImage(dpy, win, gc, img, 0, 0, 0, 0, display->width, display->height);
    XFlush(dpy);
  }
}

/* ------------------------------------------------------------------
 * XRandR output lookup / fullscreen placement
 * ------------------------------------------------------------------ */

/* Find an active monitor by XRandR output name, or by index when name is
 * empty. mon_idx receives the monitor's Xinerama-order index. */
static int find_output_geometry(Display* dpy, Window root, const char* name, int index,
                                int* x, int* y, int* w, int* h, int* mon_idx,
                                char* matched, size_t matched_len) {
  int ev_base, err_base;
  if (!XRRQueryExtension(dpy, &ev_base, &err_base)) {
    log_error(LOGGER_TAG, "XRandR extension not available on this X server");
    return -1;
  }

  int nmon = 0;
  XRRMonitorInfo* mons = XRRGetMonitors(dpy, root, True, &nmon);
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
  bool by_name = name && name[0];
  int found = -1;

  for (int m = 0; m < nmon; m++) {
    for (int o = 0; o < mons[m].noutput; o++) {
      XRROutputInfo* info = res ? XRRGetOutputInfo(dpy, res, mons[m].outputs[o]) : NULL;
      const char* oname = info ? info->name : "?";
      log_debug(LOGGER_TAG, "Monitor %d: output %s %dx%d+%d+%d", m, oname,
                mons[m].width, mons[m].height, mons[m].x, mons[m].y);
      if (found < 0 && ((by_name && strcmp(oname, name) == 0) || (!by_name && m == index))) {
        found = m;
        snprintf(matched, matched_len, "%s", oname);
      }
      if (info) XRRFreeOutputInfo(info);
    }
  }

  if (found >= 0) {
    *x = mons[found].x;
    *y = mons[found].y;
    *w = mons[found].width;
    *h = mons[found].height;
    *mon_idx = found;
  } else {
    log_warn(LOGGER_TAG, "No active monitor matches %s%s%s (index %d); %d active monitor(s)",
             by_name ? "output '" : "", by_name ? name : "", by_name ? "'" : "", index, nmon);
  }

  if (res) XRRFreeScreenResources(res);
  if (mons) XRRFreeMonitors(mons);
  return found >= 0 ? 0 : -1;
}

/* Pre-map hints: fixed position/size, no decorations, EWMH fullscreen. */
static void set_position_hints(Display* dpy, Window win, int x, int y, uint32_t w, uint32_t h) {
  XSizeHints* hints = XAllocSizeHints();
  if (hints) {
    hints->flags = USPosition | USSize;
    hints->x = x;
    hints->y = y;
    hints->width = (int)w;
    hints->height = (int)h;
    XSetWMNormalHints(dpy, win, hints);
    XFree(hints);
  }
}

static void set_decorations(Display* dpy, Window win, bool on) {
  long motif[5] = {2 /* MWM_HINTS_DECORATIONS */, 0, on ? 1 /* MWM_DECOR_ALL */ : 0, 0, 0};
  Atom motif_atom = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
  XChangeProperty(dpy, win, motif_atom, motif_atom, 32, PropModeReplace,
                  (unsigned char*)motif, 5);
}

static void set_fullscreen_hints(Display* dpy, Window win, int x, int y, uint32_t w, uint32_t h) {
  set_position_hints(dpy, win, x, y, w, h);
  set_decorations(dpy, win, false);
  Atom fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
  XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False), XA_ATOM, 32,
                  PropModeReplace, (unsigned char*)&fs, 1);
}

/* Ask the WM to keep the fullscreen window on exactly one monitor. */
static void pin_fullscreen_to_monitor(Display* dpy, int screen, Window win, int mon_idx) {
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xclient.type = ClientMessage;
  ev.xclient.window = win;
  ev.xclient.message_type = XInternAtom(dpy, "_NET_WM_FULLSCREEN_MONITORS", False);
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = mon_idx; /* top */
  ev.xclient.data.l[1] = mon_idx; /* bottom */
  ev.xclient.data.l[2] = mon_idx; /* left */
  ev.xclient.data.l[3] = mon_idx; /* right */
  ev.xclient.data.l[4] = 1;       /* source: normal application */
  XSendEvent(dpy, RootWindow(dpy, screen), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

/* ------------------------------------------------------------------
 * Wall mode <-> normal window (Escape)
 * ------------------------------------------------------------------ */
#define MAX_WALL_WINDOWS (MAX_RX_SESSIONS * (1 + MAX_MIRRORS)) /* primary + mirrors */

static pthread_mutex_t g_wall_lock = PTHREAD_MUTEX_INITIALIZER;
static struct x11_display_ctx* g_wall[MAX_WALL_WINDOWS];
static int g_nwall;
static bool g_all_windowed;
static bool g_esc_grabbed;
static bool g_close_requested;
static bool g_esc_down;
static long long g_esc_last_ms;

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool x11_close_requested(void) {
  return __atomic_load_n(&g_close_requested, __ATOMIC_RELAXED);
}

static void handle_close_message(Display* dpy, const XEvent* ev) {
  if (ev->xclient.message_type == XInternAtom(dpy, "WM_PROTOCOLS", True) &&
      (Atom)ev->xclient.data.l[0] == XInternAtom(dpy, "WM_DELETE_WINDOW", True)) {
    if (!__atomic_exchange_n(&g_close_requested, true, __ATOMIC_RELAXED))
      log_info(LOGGER_TAG, "Window closed by the user, stopping");
  }
}

static void send_wm_state(Display* dpy, int screen, Window win, bool add, const char* state) {
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xclient.type = ClientMessage;
  ev.xclient.window = win;
  ev.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = add ? 1 : 0; /* _NET_WM_STATE_ADD / _REMOVE */
  ev.xclient.data.l[1] = (long)XInternAtom(dpy, state, False);
  ev.xclient.data.l[3] = 1;
  XSendEvent(dpy, RootWindow(dpy, screen), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

/* Windowed mode keeps the wall position and size; only the title bar is added. */
static void windowed_geometry(const struct x11_display_ctx* d, int* x, int* y,
                              uint32_t* w, uint32_t* h) {
  *x = d->mon_x + d->out_x;
  *y = d->mon_y + d->out_y;
  *w = d->home_w;
  *h = d->home_h;
}

/* Caller holds g_wall_lock. */
static void enter_windowed(struct x11_display_ctx* d) {
  Display* dpy = (Display*)d->display;
  Window win = (Window)(uintptr_t)d->window;
  int x, y;
  uint32_t w, h;
  windowed_geometry(d, &x, &y, &w, &h);

  pthread_mutex_lock(&d->xlock);
  if (d->wall_kind == X11_WALL_FULLSCREEN) {
    send_wm_state(dpy, d->screen, win, false, "_NET_WM_STATE_FULLSCREEN");
    set_decorations(dpy, win, true);
    set_position_hints(dpy, win, x, y, w, h);
    XMoveResizeWindow(dpy, win, x, y, w, h);
  } else {
    /* An override-redirect window must be unmapped to become WM-managed. */
    XUnmapWindow(dpy, win);
    XSetWindowAttributes attrs;
    attrs.override_redirect = False;
    XChangeWindowAttributes(dpy, win, CWOverrideRedirect, &attrs);
    set_decorations(dpy, win, true);
    set_position_hints(dpy, win, x, y, w, h);
    XMoveResizeWindow(dpy, win, x, y, w, h);
    XMapRaised(dpy, win);
  }
  d->windowed = true;
  XFlush(dpy);
  pthread_mutex_unlock(&d->xlock);
}

/* Second half of leaving windowed mode for a fixed window, once the WM has
 * released it (or after a timeout). Caller holds g_wall_lock. */
static void finish_fixed_wall(struct x11_display_ctx* d) {
  Display* dpy = (Display*)d->display;
  Window win = (Window)(uintptr_t)d->window;
  Window root = RootWindow(dpy, d->screen), parent = root, rret, *kids = NULL;
  unsigned int nkids = 0;
  int hx = d->mon_x + d->out_x, hy = d->mon_y + d->out_y;

  pthread_mutex_lock(&d->xlock);
  if (XQueryTree(dpy, win, &rret, &parent, &kids, &nkids) && kids) XFree(kids);
  if (parent != root) XReparentWindow(dpy, win, root, hx, hy);
  XSetWindowAttributes attrs;
  attrs.override_redirect = True;
  XChangeWindowAttributes(dpy, win, CWOverrideRedirect, &attrs);
  XMoveResizeWindow(dpy, win, hx, hy, d->home_w, d->home_h);
  XMapRaised(dpy, win);
  XFlush(dpy);
  pthread_mutex_unlock(&d->xlock);
  d->wall_pending = false;
  d->windowed = false;
}

/* Caller holds g_wall_lock. */
static void leave_windowed(struct x11_display_ctx* d) {
  Display* dpy = (Display*)d->display;
  Window win = (Window)(uintptr_t)d->window;

  pthread_mutex_lock(&d->xlock);
  if (d->wall_kind == X11_WALL_FULLSCREEN) {
    set_decorations(dpy, win, false);
    XMapRaised(dpy, win); /* un-minimize */
    send_wm_state(dpy, d->screen, win, true, "_NET_WM_STATE_FULLSCREEN");
    pin_fullscreen_to_monitor(dpy, d->screen, win, d->monitor_idx);
    d->windowed = false;
  } else {
    /* Withdraw (also works when minimized); the WM reparents it back to the
     * root and finish_fixed_wall() makes it override-redirect again. */
    XWithdrawWindow(dpy, win, d->screen);
    d->wall_pending = true;
    d->pending_ms = now_ms();
  }
  XFlush(dpy);
  pthread_mutex_unlock(&d->xlock);
}

static void toggle_wall_mode(void) {
  pthread_mutex_lock(&g_wall_lock);
  for (int i = 0; i < g_nwall; i++) {
    if (g_wall[i]->wall_pending) { /* previous toggle still in progress */
      pthread_mutex_unlock(&g_wall_lock);
      return;
    }
  }
  g_all_windowed = !g_all_windowed;
  for (int i = 0; i < g_nwall; i++) {
    if (g_all_windowed) enter_windowed(g_wall[i]);
    else leave_windowed(g_wall[i]);
  }
  int n = g_nwall;
  pthread_mutex_unlock(&g_wall_lock);
  log_info(LOGGER_TAG, "Escape: %d window(s) %s", n,
           g_all_windowed ? "switched to normal windows (Escape again to go back)"
                          : "back to full-screen wall mode");
}

/* True only for a fresh Escape press, not keyboard auto-repeat. The 1 s timeout
 * recovers if a KeyRelease is ever missed. */
static bool escape_pressed(void) {
  long long now = now_ms();
  pthread_mutex_lock(&g_wall_lock);
  bool fresh = !g_esc_down || now - g_esc_last_ms > 1000;
  g_esc_down = true;
  g_esc_last_ms = now;
  pthread_mutex_unlock(&g_wall_lock);
  return fresh;
}

static void escape_released(void) {
  pthread_mutex_lock(&g_wall_lock);
  g_esc_down = false;
  pthread_mutex_unlock(&g_wall_lock);
}

/* Grab Escape on the root window so it works whichever window has focus. */
static void grab_escape(struct x11_display_ctx* d) {
  Display* dpy = (Display*)d->display;
  Window root = RootWindow(dpy, d->screen);
  KeyCode kc = XKeysymToKeycode(dpy, XK_Escape);
  /* Also grab with Caps Lock / Num Lock on, or Escape is missed while they are lit. */
  unsigned int mods[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
  for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++)
    XGrabKey(dpy, kc, mods[i], root, True, GrabModeAsync, GrabModeAsync);
  XFlush(dpy);
  log_info(LOGGER_TAG, "Escape toggles the title bars from any window");
}

static void wall_register(struct x11_display_ctx* d) {
  pthread_mutex_lock(&g_wall_lock);
  if (g_nwall < MAX_WALL_WINDOWS) g_wall[g_nwall++] = d;
  bool grab = !g_esc_grabbed;
  g_esc_grabbed = true;
  pthread_mutex_unlock(&g_wall_lock);
  if (grab) grab_escape(d);
}

static void wall_unregister(struct x11_display_ctx* d) {
  pthread_mutex_lock(&g_wall_lock);
  for (int i = 0; i < g_nwall; i++) {
    if (g_wall[i] == d) {
      g_wall[i] = g_wall[--g_nwall];
      break;
    }
  }
  if (g_nwall == 0) g_all_windowed = false;
  pthread_mutex_unlock(&g_wall_lock);
}

/* Initialize X11 display */
int init_x11_display(struct x11_display_ctx* display, uint32_t width, uint32_t height,
                     int session_idx, const char* output_name, bool fullscreen_on_output,
                     const struct crop_config* window_rect) {
  Display* x_display;
  Window x_window;
  GC x_gc;
  Visual* x_visual;
  char window_title[96];
  int win_x = 100 + (session_idx * 50);
  int win_y = 100 + (session_idx * 50);
  int monitor_idx = -1;
  char matched_output[32] = "";
  bool fixed_window = false;

  // Initialize structure
  memset(display, 0, sizeof(*display));
  pthread_mutex_init(&display->xlock, NULL);

  // Check DISPLAY environment variable
  const char* display_env = getenv("DISPLAY");
  if (!display_env) {
    log_error(LOGGER_TAG, "DISPLAY environment variable not set. X11 display not available.");
    log_info(LOGGER_TAG, "Hint: Run 'export DISPLAY=:0' or use SSH with X11 forwarding (-X flag)");
    return -1;
  }
  
  // Set up X11 error handlers
  XSetErrorHandler(x11_error_handler);
  XSetIOErrorHandler(x11_io_error_handler);
  
  // Open X11 display connection
  x_display = XOpenDisplay(NULL);
  if (!x_display) {
    log_error(LOGGER_TAG, "Cannot open X11 display '%s'", display_env);
    log_info(LOGGER_TAG, "Hint: Check if X11 server is running or use SSH with X11 forwarding");
    return -1;
  }
  
  display->display = x_display;
  display->screen = DefaultScreen(x_display);
  display->depth = DefaultDepth(x_display, display->screen);
  /* Held keys repeat KeyPress only, without a KeyRelease between repeats. */
  XkbSetDetectableAutoRepeat(x_display, True, NULL);
  display->frame_width = width;
  display->frame_height = height;
  display->frame_scratch = NULL;

  if (fullscreen_on_output) {
    int mx = 0, my = 0, mw = 0, mh = 0;
    if (find_output_geometry(x_display, RootWindow(x_display, display->screen),
                             output_name, session_idx, &mx, &my, &mw, &mh,
                             &monitor_idx, matched_output, sizeof(matched_output)) == 0) {
      display->mon_x = mx;
      display->mon_y = my;
      display->mon_w = mw;
      display->mon_h = mh;
      if (window_rect) {
        win_x = mx + window_rect->x;
        win_y = my + window_rect->y;
        display->out_x = window_rect->x;
        display->out_y = window_rect->y;
        width = (uint32_t)window_rect->w;
        height = (uint32_t)window_rect->h;
        fixed_window = true;
        if (window_rect->x + window_rect->w > mw || window_rect->y + window_rect->h > mh) {
          log_warn(LOGGER_TAG, "Session %d: window %d,%d %dx%d extends beyond %s (%dx%d)",
                   session_idx, window_rect->x, window_rect->y, window_rect->w,
                   window_rect->h, matched_output, mw, mh);
        }
      } else {
        win_x = mx;
        win_y = my;
        width = (uint32_t)mw;
        height = (uint32_t)mh;
      }
    } else {
      /* A fallback window would land on another session's panel. */
      log_warn(LOGGER_TAG, "Session %d: output %s not connected, no window will be shown",
               session_idx, (output_name && output_name[0]) ? output_name : "(by index)");
      XCloseDisplay(x_display);
      display->display = NULL;
      return -1;
    }
  }
  bool on_output = fullscreen_on_output;
  fullscreen_on_output = on_output && !fixed_window;
  
  // Get visual info
  x_visual = DefaultVisual(x_display, display->screen);
  unsigned long black = BlackPixel(x_display, display->screen);
  XVisualInfo argb;
  bool use_argb = fixed_window &&
                  XMatchVisualInfo(x_display, display->screen, 32, TrueColor, &argb);
  if (use_argb) {
    /* Mutter draws a drop shadow around fixed windows (a dark band where two sessions
     * meet) but never around ARGB windows; every pixel we write has alpha 255. */
    x_visual = argb.visual;
    display->depth = 32;
    display->colormap = XCreateColormap(x_display, RootWindow(x_display, display->screen),
                                        x_visual, AllocNone);
    black = 0xFF000000UL;
  }
  display->visual = x_visual;
  
  // Create window
  if (on_output) {
    snprintf(window_title, sizeof(window_title), "dvledrx - Session %d on %s",
             session_idx, matched_output);
  } else {
    snprintf(window_title, sizeof(window_title), "dvledrx - Session %d (%ux%u)",
             session_idx, width, height);
  }
  
  if (use_argb) {
    XSetWindowAttributes wa;
    wa.colormap = (Colormap)display->colormap;
    wa.border_pixel = black;
    wa.background_pixel = black;
    x_window = XCreateWindow(x_display, RootWindow(x_display, display->screen),
                             win_x, win_y, width, height, 0, 32, InputOutput, x_visual,
                             CWColormap | CWBorderPixel | CWBackPixel, &wa);
  } else {
    x_window = XCreateSimpleWindow(x_display,
                                  RootWindow(x_display, display->screen),
                                  win_x, win_y,
                                  width, height,
                                  on_output ? 0 : 1, // border width
                                  black,  // border
                                  black   // background
    );
  }
  
  if (!x_window) {
    log_error(LOGGER_TAG, "Cannot create X11 window");
    if (display->colormap) XFreeColormap(x_display, (Colormap)display->colormap);
    XCloseDisplay(x_display);
    return -1;
  }
  
  display->window = (void*)(uintptr_t)x_window;
  
  // Set window properties
  XStoreName(x_display, x_window, window_title);
  XSelectInput(x_display, x_window,
               ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask);
  Atom wm_delete = XInternAtom(x_display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(x_display, x_window, &wm_delete, 1); /* close button: ClientMessage, not a kill */
  if (fullscreen_on_output) {
    set_fullscreen_hints(x_display, x_window, win_x, win_y, width, height);
  } else if (fixed_window) {
    /* Bypass the WM so it can't move the window under panels or resize it. */
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    XChangeWindowAttributes(x_display, x_window, CWOverrideRedirect, &attrs);
  }
  
  // Create graphics context
  x_gc = XCreateGC(x_display, x_window, 0, NULL);
  display->gc = x_gc;
  
  // Image buffer uses the real screen depth (24 or 32) with a 4 bytes/pixel stride.
  display->use_shm = true;
  if (create_image_buffer(display, width, height) < 0) {
    XFreeGC(x_display, x_gc);
    XDestroyWindow(x_display, x_window);
    XCloseDisplay(x_display);
    return -1;
  }
  
  // Map window (make it visible)
  XMapWindow(x_display, x_window);
  if (fullscreen_on_output) {
    pin_fullscreen_to_monitor(x_display, display->screen, x_window, monitor_idx);
  } else if (fixed_window) {
    XRaiseWindow(x_display, x_window);
  }
  XFlush(x_display);

  display->initialized = true;
  snprintf(display->output_name, sizeof(display->output_name), "%s",
           on_output ? matched_output : "(window)");
  display->monitor_idx = monitor_idx;
  display->home_w = width;
  display->home_h = height;
  display->wall_kind = fullscreen_on_output ? X11_WALL_FULLSCREEN
                       : fixed_window       ? X11_WALL_FIXED
                                            : X11_WALL_NONE;
  if (display->wall_kind != X11_WALL_NONE) wall_register(display);

  log_info(LOGGER_TAG, "X11 display initialized: %ux%u+%d+%d, depth=%d, session=%d, output=%s (%s), shm=%s",
         width, height, win_x, win_y, display->depth, session_idx,
         on_output ? matched_output : "(window)",
         fixed_window ? "fixed" : fullscreen_on_output ? "fullscreen" : "normal",
         display->shminfo ? "yes" : "no");

  return 0;
}

/* Cleanup X11 display */
void cleanup_x11_display(struct x11_display_ctx* display) {
  if (!display || !display->initialized) {
    return;
  }
  
  Display* x_display = (Display*)display->display;
  Window x_window = (Window)(uintptr_t)display->window;
  GC x_gc = (GC)display->gc;

  wall_unregister(display);
  destroy_image_buffer(x_display, (XImage*)display->ximage, display->image_data,
                       (XShmSegmentInfo*)display->shminfo);

  if (display->frame_scratch) {
    free(display->frame_scratch);
  }
  
  if (x_gc) {
    XFreeGC(x_display, x_gc);
  }
  
  if (x_window) {
    XDestroyWindow(x_display, x_window);
  }

  if (display->colormap) {
    XFreeColormap(x_display, (Colormap)display->colormap);
  }
  
  if (x_display) {
    XCloseDisplay(x_display);
  }
  
  memset(display, 0, sizeof(*display));
  
  log_info(LOGGER_TAG, "X11 display cleaned up");
}

/* Resize only XImage buffer to match the current X11 window size. */
static int resize_x11_buffers(struct x11_display_ctx* display,
                              uint32_t new_width, uint32_t new_height) {
  if (!display || !display->initialized || !new_width || !new_height) return -1;
  if (new_width == display->width && new_height == display->height) return 0;

  Display* x_display = (Display*)display->display;
  XImage* old_image = (XImage*)display->ximage;
  char* old_image_data = display->image_data;
  XShmSegmentInfo* old_shm = (XShmSegmentInfo*)display->shminfo;
  uint32_t old_w = display->width;
  uint32_t old_h = display->height;

  if (create_image_buffer(display, new_width, new_height) < 0) {
    display->ximage = old_image;
    display->image_data = old_image_data;
    display->shminfo = old_shm;
    display->width = old_w;
    display->height = old_h;
    return -1;
  }

  destroy_image_buffer(x_display, old_image, old_image_data, old_shm);
  return 0;
}

/* Nearest-neighbour scaler for BGRA buffers. */
static void scale_bgra_nearest(const uint8_t* src, uint32_t src_w, uint32_t src_h,
                               uint8_t* dst, uint32_t dst_w, uint32_t dst_h) {
  for (uint32_t y = 0; y < dst_h; y++) {
    uint32_t sy = (uint32_t)(((uint64_t)y * src_h) / dst_h);
    for (uint32_t x = 0; x < dst_w; x++) {
      uint32_t sx = (uint32_t)(((uint64_t)x * src_w) / dst_w);
      const uint8_t* sp = src + ((size_t)sy * src_w + sx) * 4;
      uint8_t* dp = dst + ((size_t)y * dst_w + x) * 4;
      dp[0] = sp[0];
      dp[1] = sp[1];
      dp[2] = sp[2];
      dp[3] = sp[3];
    }
  }
}

/* Display frame using X11 - uses unified convert_frame_to_rgb with proper linesize */
void display_frame_x11(struct x11_display_ctx* display, struct st_frame* frame) {
  if (!display || !display->initialized) {
    return;
  }

  Display* x_display = (Display*)display->display;
  Window x_window = (Window)(uintptr_t)display->window;
  GC x_gc = (GC)display->gc;

  int frame_counter = (int)++display->frame_counter;

  bool video_displayed = false;

  if (frame && frame->addr[0]) {
    if (frame->width > 0 && frame->height > 0) {
      display->frame_width = frame->width;
      display->frame_height = frame->height;
    }

    uint32_t frame_w = display->frame_width;
    uint32_t frame_h = display->frame_height;
    size_t scratch_size = (size_t)frame_w * frame_h * 4;

    if (display->frame_scratch_cap < scratch_size) {
      free(display->frame_scratch);
      display->frame_scratch = (char*)malloc(scratch_size);
      display->frame_scratch_cap = display->frame_scratch ? scratch_size : 0;
      if (!display->frame_scratch) {
        log_error(LOGGER_TAG, "Cannot allocate frame scratch buffer (%ux%u)", frame_w, frame_h);
        return;
      }
    }
    /* -------------------------------------------------------
     * DIAGNOSTIC: log raw YUV plane info for first 5 frames
     * to help identify green-screen root cause.
     * ------------------------------------------------------- */
    if (frame_counter <= 5) {
      log_debug(LOGGER_TAG, "DIAG[rx frame %d]: fmt=%d  %ux%u  data_size=%zu",
             frame_counter, frame->fmt, frame->width, frame->height,
             frame->data_size);
      log_debug(LOGGER_TAG, "DIAG[rx frame %d]: addr  Y=%p  U=%p  V=%p",
             frame_counter, frame->addr[0], frame->addr[1], frame->addr[2]);
      log_debug(LOGGER_TAG, "DIAG[rx frame %d]: linesize  Y=%zu  U=%zu  V=%zu",
             frame_counter, frame->linesize[0], frame->linesize[1],
             frame->linesize[2]);

      if ((frame->fmt == ST_FRAME_FMT_YUV422PLANAR10LE ||
           frame->fmt == ST_FRAME_FMT_YUV422PLANAR12LE) &&
          frame->addr[0] && frame->addr[1] && frame->addr[2]) {
        int bit_shift = (frame->fmt == ST_FRAME_FMT_YUV422PLANAR12LE) ? 4 : 2;
        uint16_t* yp = (uint16_t*)frame->addr[0];
        uint16_t* up = (uint16_t*)frame->addr[1];
        uint16_t* vp = (uint16_t*)frame->addr[2];
        size_t y_stride = frame->linesize[0] / 2;
        size_t u_stride = frame->linesize[1] / 2;

        /* top-left pixel */
        log_debug(LOGGER_TAG, "DIAG[rx frame %d]: TL  raw16 Y=0x%04x U=0x%04x V=0x%04x  "
               "8-bit Y=%u U=%u V=%u",
               frame_counter,
               yp[0], up[0], vp[0],
               (yp[0] >> bit_shift) & 0xFF,
               (up[0] >> bit_shift) & 0xFF,
               (vp[0] >> bit_shift) & 0xFF);

        /* centre pixel */
        uint32_t cx = frame_w / 2, cy = frame_h / 2;
        uint16_t cy16 = yp[cy * y_stride + cx];
        uint16_t cu16 = up[cy * u_stride + cx / 2];
        uint16_t cv16 = vp[cy * u_stride + cx / 2];
        log_debug(LOGGER_TAG, "DIAG[rx frame %d]: CTR raw16 Y=0x%04x U=0x%04x V=0x%04x  "
               "8-bit Y=%u U=%u V=%u",
               frame_counter,
               cy16, cu16, cv16,
               (cy16 >> bit_shift) & 0xFF,
               (cu16 >> bit_shift) & 0xFF,
               (cv16 >> bit_shift) & 0xFF);

        /* Print first 4 raw U values to spot if U-plane is zero/garbage */
        log_debug(LOGGER_TAG, "DIAG[rx frame %d]: U-plane first 4 words: 0x%04x 0x%04x 0x%04x 0x%04x",
               frame_counter, up[0], up[1], up[2], up[3]);
        log_debug(LOGGER_TAG, "DIAG[rx frame %d]: V-plane first 4 words: 0x%04x 0x%04x 0x%04x 0x%04x",
               frame_counter, vp[0], vp[1], vp[2], vp[3]);
      }
    }

    /* -------------------------------------------------------
     * Convert using the unified converter that respects
     * frame->linesize[], supporting all planar formats.
     * The old convert_yuv422_to_rgb_safe used hardcoded stride
     * (width/2) and would read wrong UV data if the frame had
     * any row padding – producing all-green output.
     * ------------------------------------------------------- */
    bool converted = false;
    bool direct;

    /* Window size may change on the main thread (ConfigureNotify), so compare
     * and write into image_data under the same lock. */
    pthread_mutex_lock(&display->xlock);
    direct = (frame_w == display->width && frame_h == display->height);
    if (direct) {
      /* No scaling needed: convert straight into the X image buffer. */
      converted = (convert_frame_to_rgb(frame, (uint8_t*)display->image_data, frame_w, frame_h) == 0);
      if (converted) put_image_locked(display);
    }
    pthread_mutex_unlock(&display->xlock);

    if (!direct) {
      /* Convert only the sampled pixels, straight into the X image (no scratch pass). */
      pthread_mutex_lock(&display->xlock);
      if (convert_frame_to_rgb_scaled(frame, (uint8_t*)display->image_data,
                                      display->width, display->height) == 0) {
        converted = true;
        put_image_locked(display);
      }
      pthread_mutex_unlock(&display->xlock);
    }

    if (!direct && !converted) {
      converted = (convert_frame_to_rgb(frame, (uint8_t*)display->frame_scratch, frame_w, frame_h) == 0);
      if (converted) {
        pthread_mutex_lock(&display->xlock);
        scale_bgra_nearest((uint8_t*)display->frame_scratch, frame_w, frame_h,
                           (uint8_t*)display->image_data, display->width, display->height);
        put_image_locked(display);
        pthread_mutex_unlock(&display->xlock);
      }
    }

    if (converted) {
      video_displayed = true;
    } else {
      log_warn(LOGGER_TAG, "convert_frame_to_rgb failed "
             "(fmt=%d, frame=%ux%u, win=%ux%u). Falling back.",
             frame->fmt, frame_w, frame_h,
             display->width, display->height);
    }
  }

  /* Fallback animated display when conversion is unavailable */
  if (!video_displayed) {
    pthread_mutex_lock(&display->xlock);
    uint32_t width  = display->width;
    uint32_t height = display->height;
    unsigned long alpha = display->depth == 32 ? 0xFF000000UL : 0;
    XSetForeground(x_display, x_gc, alpha | 0x404040);
    XFillRectangle(x_display, x_window, x_gc, 0, 0, width, height);
    XSetForeground(x_display, x_gc, alpha | (uint32_t)((frame_counter * 0x1234) & 0xFFFFFF));
    int rect_x = (frame_counter * 2) % (int)(width  > 100 ? width  - 100 : 1);
    int rect_y = (frame_counter)     % (int)(height > 100 ? height - 100 : 1);
    XFillRectangle(x_display, x_window, x_gc, rect_x, rect_y, 100, 100);
    XSetForeground(x_display, x_gc, alpha | 0xFFFFFF);
    char status_text[96];
    if (frame && frame->addr[0])
      snprintf(status_text, sizeof(status_text),
               "Converting frame %d (fmt=%d)...", frame_counter,
               frame ? frame->fmt : -1);
    else
      snprintf(status_text, sizeof(status_text), "Waiting for video data...");
    XDrawString(x_display, x_window, x_gc, 10, 30, status_text, strlen(status_text));
    pthread_mutex_unlock(&display->xlock);
  }

  pthread_mutex_lock(&display->xlock);
  XFlush(x_display);
  pthread_mutex_unlock(&display->xlock);
}

/* Mirror src's already-converted image into dst (lock order: src, then dst). */
bool display_copy_x11(struct x11_display_ctx* dst, struct x11_display_ctx* src) {
  if (!dst || !src || !dst->initialized || !src->initialized) return false;

  bool copied = false;
  pthread_mutex_lock(&src->xlock);
  pthread_mutex_lock(&dst->xlock);
  if (dst->width == src->width && dst->height == src->height &&
      dst->image_data && src->image_data) {
    memcpy(dst->image_data, src->image_data, (size_t)dst->width * dst->height * 4);
    put_image_locked(dst);
    XFlush((Display*)dst->display);
    copied = true;
  }
  pthread_mutex_unlock(&dst->xlock);
  pthread_mutex_unlock(&src->xlock);
  return copied;
}

/* Process X11 events (non-blocking) */
void process_x11_events(struct x11_display_ctx* display) {
  if (!display || !display->initialized) {
    return;
  }
  
  // Safe, minimal event processing - only handle essential events
  static int event_counter = 0;
  
  Display* x_display = (Display*)display->display;
  
  // Check if X11 connection is still valid
  if (!x_display || XConnectionNumber(x_display) < 0) {
    log_error(LOGGER_TAG, "X11 connection lost in process_x11_events");
    display->initialized = false;
    return;
  }
  XEvent event;
  
  // Process events very carefully - avoid any operations that might corrupt protocol
  int events_processed = 0;
  
  while (XPending(x_display) && events_processed < 10) {  // Limit events per call
    XNextEvent(x_display, &event);
    events_processed++;
    
    switch (event.type) {
      case Expose:
        /* Do NOT XClearWindow here — it erases the last XPutImage frame.
         * Re-blit the existing XImage instead so the window repaints
         * correctly after being minimised / obscured. */
        pthread_mutex_lock(&display->xlock);
        put_image_locked(display);
        pthread_mutex_unlock(&display->xlock);
        break;
        
      case KeyPress:
        if (display->wall_kind != X11_WALL_NONE &&
            XLookupKeysym(&event.xkey, 0) == XK_Escape && escape_pressed()) {
          toggle_wall_mode();
        }
        break;

      case KeyRelease:
        if (XLookupKeysym(&event.xkey, 0) == XK_Escape) escape_released();
        break;

      case ReparentNotify:
        if (display->wall_pending &&
            event.xreparent.parent == RootWindow(x_display, display->screen)) {
          pthread_mutex_lock(&g_wall_lock);
          if (display->wall_pending) finish_fixed_wall(display);
          pthread_mutex_unlock(&g_wall_lock);
        }
        break;

      case ClientMessage:
        handle_close_message(x_display, &event);
        break;
        
      case ConfigureNotify:
        if (event.xconfigure.width > 0 && event.xconfigure.height > 0) {
          pthread_mutex_lock(&display->xlock);
          if (resize_x11_buffers(display,
                                 (uint32_t)event.xconfigure.width,
                                 (uint32_t)event.xconfigure.height) == 0 &&
              ++event_counter % 5 == 0) {
            log_debug(LOGGER_TAG, "X11 window resized to %dx%d (event %d)",
                   event.xconfigure.width, event.xconfigure.height, event_counter);
          }
          pthread_mutex_unlock(&display->xlock);
        }
        break;
        
      default:
        // Ignore all other events to avoid potential issues
        break;
    }
  }
  
  // Simple flush without sync
  if (events_processed > 0) {
    XFlush(x_display);
  }

  if (display->wall_pending && now_ms() - display->pending_ms > 1000) {
    pthread_mutex_lock(&g_wall_lock); /* WM never released the window */
    if (display->wall_pending) finish_fixed_wall(display);
    pthread_mutex_unlock(&g_wall_lock);
  }
}

/* ============================================================
 * Composite display implementation
 * N session threads each write their strip then synchronise
 * via a barrier; session 0 calls XPutImage once per frame.
 * ============================================================ */

int init_composite_x11_display(struct composite_x11_ctx* comp,
                                uint32_t strip_width, uint32_t height,
                                int num_sessions) {
  if (!comp || num_sessions < 1) return -1;

  memset(comp, 0, sizeof(*comp));

  const char* display_env = getenv("DISPLAY");
  if (!display_env) {
    log_error(LOGGER_TAG, "DISPLAY not set for composite display");
    return -1;
  }

  XSetErrorHandler(x11_error_handler);
  XSetIOErrorHandler(x11_io_error_handler);

  Display* x_display = XOpenDisplay(NULL);
  if (!x_display) {
    log_error(LOGGER_TAG, "Cannot open X11 display for composite");
    return -1;
  }

  comp->display    = x_display;
  comp->screen     = DefaultScreen(x_display);
  comp->depth      = DefaultDepth(x_display, comp->screen);
  comp->strip_width = strip_width;
  comp->full_width  = strip_width;   /* caller passes the total display width */
  comp->height      = height;
  comp->num_sessions = num_sessions;
  comp->win_width   = strip_width;   /* on-screen size starts equal to native size */
  comp->win_height  = height;
  comp->scaled_ximage     = NULL;
  comp->scaled_image_data = NULL;

  Visual* x_visual = DefaultVisual(x_display, comp->screen);
  comp->visual = x_visual;

  /* Full-width composite window */
  char title[128];
  snprintf(title, sizeof(title),
           "dvledrx Composite (%dx%d, %d sessions)",
           comp->full_width, height, num_sessions);

  Window x_window = XCreateSimpleWindow(
      x_display, RootWindow(x_display, comp->screen),
      50, 50, comp->full_width, height,
      1,
      BlackPixel(x_display, comp->screen),
      BlackPixel(x_display, comp->screen));

  if (!x_window) {
    log_error(LOGGER_TAG, "Cannot create composite X11 window");
    XCloseDisplay(x_display);
    return -1;
  }

  comp->window = (void*)(uintptr_t)x_window;

  XStoreName(x_display, x_window, title);
  XSelectInput(x_display, x_window, ExposureMask | KeyPressMask | StructureNotifyMask);
  Atom wm_delete = XInternAtom(x_display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(x_display, x_window, &wm_delete, 1);

  GC x_gc = XCreateGC(x_display, x_window, 0, NULL);
  comp->gc = x_gc;

  /* Allocate composite pixel buffer */
  size_t buf_size = (size_t)comp->full_width * height * 4;
  char* image_data = malloc(buf_size);
  if (!image_data) {
    log_error(LOGGER_TAG, "Cannot allocate composite image buffer (%zu bytes)", buf_size);
    XFreeGC(x_display, x_gc);
    XDestroyWindow(x_display, x_window);
    XCloseDisplay(x_display);
    return -1;
  }
  memset(image_data, 0, buf_size);
  comp->image_data = image_data;

  XImage* x_image = XCreateImage(x_display, x_visual,
                                  comp->depth, ZPixmap, 0,
                                  image_data,
                                  comp->full_width, height,
                                  32, (int)(comp->full_width * 4));
  if (!x_image) {
    log_error(LOGGER_TAG, "Cannot create composite XImage");
    free(image_data);
    XFreeGC(x_display, x_gc);
    XDestroyWindow(x_display, x_window);
    XCloseDisplay(x_display);
    return -1;
  }
  comp->ximage = x_image;

  XMapWindow(x_display, x_window);
  XFlush(x_display);

  /* Default-attribute mutex/cond init cannot fail on Linux. */
  pthread_mutex_init(&comp->bar_lock, NULL);
  pthread_cond_init(&comp->bar_cond, NULL);
  pthread_mutex_init(&comp->xlock, NULL);

  comp->initialized = true;

  log_info(LOGGER_TAG, "Composite X11 display initialised: %dx%d (%d sessions)",
         comp->full_width, height, num_sessions);
  return 0;
}

void cleanup_composite_x11_display(struct composite_x11_ctx* comp) {
  if (!comp || !comp->initialized) return;

  pthread_cond_destroy(&comp->bar_cond);
  pthread_mutex_destroy(&comp->bar_lock);
  pthread_mutex_destroy(&comp->xlock);

  Display* x_display = (Display*)comp->display;
  GC       x_gc      = (GC)comp->gc;
  Window   x_window  = (Window)(uintptr_t)comp->window;
  XImage*  x_image   = (XImage*)comp->ximage;
  XImage*  scaled_image = (XImage*)comp->scaled_ximage;

  if (scaled_image) {
    scaled_image->data = NULL; /* we own scaled_image_data; don't let XDestroyImage free it */
    XDestroyImage(scaled_image);
  }
  free(comp->scaled_image_data);

  if (x_image) {
    x_image->data = NULL; /* We own image_data; don't let XDestroyImage free it */
    XDestroyImage(x_image);
  }
  free(comp->image_data);
  if (x_gc)     XFreeGC(x_display, x_gc);
  if (x_window) XDestroyWindow(x_display, x_window);
  if (x_display) XCloseDisplay(x_display);

  memset(comp, 0, sizeof(*comp));
  log_info(LOGGER_TAG, "Composite X11 display cleaned up");
}

/* Reallocate the window-sized scaled buffer to match new_w × new_h.
 * Must be called with comp->xlock held. When new_w/new_h equal the native
 * full_width/height, the scaled buffer is freed instead (XPutImage will use
 * the native buffer directly). */
static void resize_composite_scaled_buffer(struct composite_x11_ctx* comp,
                                           uint32_t new_w, uint32_t new_h) {
  if (!comp || !new_w || !new_h) return;
  if (new_w == comp->win_width && new_h == comp->win_height) return;

  Display* x_display = (Display*)comp->display;
  XImage* old_scaled = (XImage*)comp->scaled_ximage;
  char* old_scaled_data = comp->scaled_image_data;

  comp->win_width  = new_w;
  comp->win_height = new_h;

  if (new_w == comp->full_width && new_h == comp->height) {
    /* Back to native size: drop the scaled buffer, use comp->ximage directly */
    comp->scaled_ximage = NULL;
    comp->scaled_image_data = NULL;
    if (old_scaled) {
      old_scaled->data = NULL;
      XDestroyImage(old_scaled);
    }
    free(old_scaled_data);
    return;
  }

  size_t buf_size = (size_t)new_w * new_h * 4;
  char* new_data = (char*)malloc(buf_size);
  if (!new_data) {
    log_error(LOGGER_TAG, "Cannot allocate scaled composite buffer (%ux%u)", new_w, new_h);
    return;
  }
  memset(new_data, 0, buf_size);

  XImage* new_image = XCreateImage(x_display, (Visual*)comp->visual,
                                    comp->depth, ZPixmap, 0,
                                    new_data, new_w, new_h,
                                    32, (int)(new_w * 4));
  if (!new_image) {
    log_error(LOGGER_TAG, "Cannot create scaled composite XImage (%ux%u)", new_w, new_h);
    free(new_data);
    return;
  }

  comp->scaled_image_data = new_data;
  comp->scaled_ximage = new_image;

  if (old_scaled) {
    old_scaled->data = NULL;
    XDestroyImage(old_scaled);
  }
  free(old_scaled_data);
}

/* Blit the composite buffer to the window, scaling to the current window
 * size if it differs from the native composite resolution (e.g. after the
 * window manager fullscreens/resizes the window). Must be called with
 * comp->xlock held. */
static void composite_flush_locked(struct composite_x11_ctx* comp) {
  Display* x_display = (Display*)comp->display;
  Window   x_window  = (Window)(uintptr_t)comp->window;
  GC       x_gc      = (GC)comp->gc;

  if (comp->scaled_ximage &&
      (comp->win_width != comp->full_width || comp->win_height != comp->height)) {
    scale_bgra_nearest((uint8_t*)comp->image_data, comp->full_width, comp->height,
                       (uint8_t*)comp->scaled_image_data, comp->win_width, comp->win_height);
    XPutImage(x_display, x_window, x_gc, (XImage*)comp->scaled_ximage,
              0, 0, 0, 0, comp->win_width, comp->win_height);
  } else {
    XPutImage(x_display, x_window, x_gc, (XImage*)comp->ximage,
              0, 0, 0, 0, comp->full_width, comp->height);
  }
  XFlush(x_display);
}

/* Wait until every session has written its part of this frame. Returns true for
 * exactly one caller per round (it flushes); false for the others or once aborted. */
static bool composite_barrier_wait(struct composite_x11_ctx* comp) {
  bool last = false;
  pthread_mutex_lock(&comp->bar_lock);
  if (!comp->bar_aborted) {
    unsigned round = comp->bar_round;
    if (++comp->bar_count == comp->num_sessions) {
      comp->bar_count = 0;
      comp->bar_round++;
      pthread_cond_broadcast(&comp->bar_cond);
      last = true;
    } else {
      while (round == comp->bar_round && !comp->bar_aborted)
        pthread_cond_wait(&comp->bar_cond, &comp->bar_lock);
    }
  }
  pthread_mutex_unlock(&comp->bar_lock);
  return last;
}

void composite_abort(struct composite_x11_ctx* comp) {
  if (!comp || !comp->initialized) return;
  pthread_mutex_lock(&comp->bar_lock);
  comp->bar_aborted = true;
  pthread_cond_broadcast(&comp->bar_cond);
  pthread_mutex_unlock(&comp->bar_lock);
}

void composite_write_strip(struct composite_x11_ctx* comp,
                            struct st_frame* frame, int session_idx) {
  if (!comp || !comp->initialized || !frame) return;

  uint32_t strip_w = comp->strip_width;
  uint32_t h       = comp->height;

  /* Convert this session's strip to BGRA (4 bytes/pixel) into a temporary buffer */
  size_t   strip_bytes = (size_t)strip_w * h * 4;
  uint8_t* strip_buf   = malloc(strip_bytes);
  if (!strip_buf) {
    log_error(LOGGER_TAG, "composite_write_strip: OOM for session %d", session_idx);
    /* Still need to hit the barrier to avoid deadlock */
    composite_barrier_wait(comp);
    return;
  }

  convert_frame_to_rgb(frame, strip_buf, strip_w, h);

  /* Blit strip into composite buffer at the correct X offset.
   * Each row in the composite buffer is full_width * 4 bytes wide. */
  uint32_t row_stride_comp  = comp->full_width * 4;  /* bytes per composite row */
  uint32_t row_stride_strip = strip_w * 4;            /* bytes per strip row */
  uint32_t x_byte_off       = (uint32_t)session_idx * strip_w * 4; /* byte offset in row */
  uint8_t* dst_base = (uint8_t*)comp->image_data;

  for (uint32_t row = 0; row < h; row++) {
    uint8_t* dst = dst_base + row * row_stride_comp + x_byte_off;
    uint8_t* src = strip_buf + row * row_stride_strip;
    memcpy(dst, src, row_stride_strip);
  }

  free(strip_buf);

  /* Synchronise: wait for all strips; the last one to arrive flushes. */
  if (composite_barrier_wait(comp)) {
    pthread_mutex_lock(&comp->xlock);
    composite_flush_locked(comp);
    pthread_mutex_unlock(&comp->xlock);
  }
}

#include "config_reader.h"   /* struct crop_config */

void composite_write_crop(struct composite_x11_ctx* comp,
                           struct st_frame* frame,
                           const struct crop_config* crop) {
  if (!comp || !comp->initialized || !frame || !crop) return;

  uint32_t cw = (uint32_t)crop->w;
  uint32_t ch = (uint32_t)crop->h;

  /* Convert this session's crop-sized frame to BGRA */
  size_t   strip_bytes = (size_t)cw * ch * 4;
  uint8_t* strip_buf   = malloc(strip_bytes);
  if (!strip_buf) {
    log_error(LOGGER_TAG, "composite_write_crop: OOM (%ux%u)", cw, ch);
    composite_barrier_wait(comp);
    return;
  }

  convert_frame_to_rgb(frame, strip_buf, cw, ch);

  /* Blit into the composite buffer at (crop->x, crop->y).
   * Clamp to the composite dimensions to prevent out-of-bounds writes. */
  uint32_t dst_x = (uint32_t)crop->x;
  uint32_t dst_y = (uint32_t)crop->y;
  uint32_t row_stride_comp = comp->full_width * 4;
  uint8_t* dst_base = (uint8_t*)comp->image_data;

  for (uint32_t row = 0; row < ch; row++) {
    uint32_t dy = dst_y + row;
    if (dy >= comp->height) break;

    uint32_t copy_w = cw;
    if (dst_x + copy_w > comp->full_width)
      copy_w = comp->full_width - dst_x;

    uint8_t* dst = dst_base + dy * row_stride_comp + dst_x * 4;
    uint8_t* src = strip_buf + row * cw * 4;
    memcpy(dst, src, copy_w * 4);
  }

  free(strip_buf);

  /* Synchronise with other sessions */
  if (composite_barrier_wait(comp)) {
    pthread_mutex_lock(&comp->xlock);
    composite_flush_locked(comp);
    pthread_mutex_unlock(&comp->xlock);
  }
}

void process_composite_x11_events(struct composite_x11_ctx* comp) {
  if (!comp || !comp->initialized) return;

  Display* x_display = (Display*)comp->display;

  pthread_mutex_lock(&comp->xlock);
  while (XPending(x_display) > 0) {
    XEvent event;
    XNextEvent(x_display, &event);

    if (event.type == Expose && event.xexpose.count == 0) {
      /* Repaint composite image on expose */
      composite_flush_locked(comp);
    } else if (event.type == ConfigureNotify) {
      /* Window manager resized/fullscreened the window: rescale the
       * composite buffer to fill the new window size, matching the
       * behaviour of single-session windows. */
      if (event.xconfigure.width > 0 && event.xconfigure.height > 0) {
        resize_composite_scaled_buffer(comp,
                                       (uint32_t)event.xconfigure.width,
                                       (uint32_t)event.xconfigure.height);
        composite_flush_locked(comp);
      }
    } else if (event.type == ClientMessage) {
      handle_close_message(x_display, &event);
    }
  }
  pthread_mutex_unlock(&comp->xlock);
}
