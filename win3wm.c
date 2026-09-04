/*
 * WIN3WM is a window manager (Windows 3.0-style)
 * Copyright (C) 2018  Victor C. Salas P. (aka nmag) <nmagko@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/wait.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "wmver.h"

#define MAX(a,b) ((a) > (b) ? (a) : (b))

/* VGA palette */
#define C_DESKTOP       0x00808080UL
#define C_FACE          0x00C0C0C0UL
#define C_WHITE         0x00FFFFFFUL
#define C_BLACK         0x00000000UL
#define C_DARKGRAY      0x00808080UL
#define C_ACTIVE_TITLE  0x00000080UL
#define C_INACTIVE      0x00808080UL

/* Compact geometry */
#define FRAME_EDGE        4
#define TITLE_H          19
#define CTRL_W           18
#define CTRL_H           17
#define CTRL_GAP          1

/* Icon grid */
#define ICON_CELL_W       88
#define ICON_CELL_H       52
#define ICON_ART_W        32
#define ICON_ART_H        32

/* Control menu, Minimize, and Maximize buttons */
#define WIN3_SHOW_CLOSE_BUTTON 0
#define STATE_NORMAL     0
#define STATE_MINIMIZED  1
#define STATE_MAXIMIZED  2

/* Misc constants */
#define MAX_MONITORS      16

#define TASK_W            330
#define TASK_H            250
#define TASK_MARGIN        10
#define TASK_LIST_Y        28
#define TASK_LIST_H       132
#define TASK_ROW_H         14
#define TASK_BTN_W         92
#define TASK_BTN_H         22
#define TASK_GAP            8

typedef struct Win3Window {
  Window client;
  Window frame;
  Window titlebar;
  Window iconwin;
  char *title;
  int state;
  int restore_state;
  int x, y;                 /* client position in root coordinates */
  int width, height;        /* client size */
  int normal_x, normal_y;
  int normal_width, normal_height;
  int ignore_unmap;
  unsigned long icon_seq;
  unsigned long focus_seq;
  struct Win3Window *next;
} Win3Window;

static Display *dpy;
static int screen;
static Window root;
static XFontStruct *font_info;
static Atom wm_delete_window_atom;
static Atom wm_protocols_atom;
static Atom wm_state_atom;
static Win3Window *windows;
static Win3Window *active_win;

static Win3Window *drag_win;
static int dragging;
static int drag_start_x_root, drag_start_y_root;
static int drag_frame_x, drag_frame_y;

/* Border resizing */
static Win3Window *resize_win;
static int resizing, resize_edges;
static int resize_start_x_root, resize_start_y_root;
static int resize_x, resize_y, resize_w, resize_h;
static int resize_start_x, resize_start_y, resize_start_w, resize_start_h;
static GC resize_gc;

#define RESIZE_LEFT   1
#define RESIZE_RIGHT  2
#define RESIZE_TOP    4
#define RESIZE_BOTTOM 8

static Time last_control_click_time;
static Window last_control_click_window;
static Time last_icon_click_time;
static Window last_icon_click_window;
static unsigned long next_icon_seq;
static unsigned long next_focus_seq;

/* ALT+TAB keeps a stable MRU snapshot while ALT is held */
static Win3Window **alt_tab_list;
static int alt_tab_count;
static int alt_tab_index;
static int alt_tab_active;

static Window task_win;
static int task_open;
static int task_selected;
static int task_top;
static Time last_task_click_time;
static int last_task_click_index = -1;

/* ================================================================ */
/* Handling monitors with xrandr */
/* ================================================================ */

typedef struct MonitorInfo {
  char name[64];
  char modes[64][32];
  int nmodes;
  int primary;
} MonitorInfo;

static int safe_output_name (const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  if (!s || !*s) return 0;
  for (; *p; ++p)
    if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') return 0;
  return 1;
}

static int parse_mode_size (const char *s, int *w, int *h) {
  char tail;
  return sscanf(s, "%dx%d%c", w, h, &tail) == 2 && *w > 0 && *h > 0;
}

static int read_monitors (MonitorInfo *mons, int maxmons) {
  FILE *fp;
  char line[512];
  int n = 0, current = -1;

  fp = popen("xrandr --query 2>/dev/null", "r");
  if (!fp) return -1;

  while (fgets(line, sizeof(line), fp)) {
    char name[64], status[32];
    if (line[0] != ' ' && line[0] != '\t') {
      current = -1;
      if (sscanf(line, "%63s %31s", name, status) == 2 &&
          strcmp(status, "connected") == 0 && n < maxmons &&
          safe_output_name(name)) {
        memset(&mons[n], 0, sizeof(mons[n]));
        snprintf(mons[n].name, sizeof(mons[n].name), "%s", name);
        mons[n].primary = strstr(line, " connected primary ") != NULL;
        current = n++;
      }
    } else if (current >= 0) {
      char mode[32];
      int mw, mh;
      char *p = line;
      while (*p == ' ' || *p == '\t') ++p;
      if (sscanf(p, "%31s", mode) == 1 && parse_mode_size(mode, &mw, &mh) &&
          mons[current].nmodes < (int)(sizeof(mons[current].modes) / sizeof(mons[current].modes[0]))) {
        int i, duplicate = 0;
        for (i = 0; i < mons[current].nmodes; ++i)
          if (strcmp(mons[current].modes[i], mode) == 0) duplicate = 1;
        if (!duplicate)
          snprintf(mons[current].modes[mons[current].nmodes++],
                   sizeof(mons[current].modes[0]), "%s", mode);
      }
    }
  }

  if (pclose(fp) == -1 && n == 0) return -1;
  return n;
}

static int monitor_has_mode (const MonitorInfo *m, const char *mode) {
  int i;
  for (i = 0; i < m->nmodes; ++i)
    if (strcmp(m->modes[i], mode) == 0) return 1;
  return 0;
}

static int append_cmd (char *cmd, size_t cap, const char *text) {
  size_t used = strlen(cmd), add = strlen(text);
  if (used + add + 1 > cap) return 0;
  memcpy(cmd + used, text, add + 1);
  return 1;
}

static int run_xrandr_command (const char *cmd) {
  int rc;
  fprintf(stderr, "%s: display setup: %s\n", program_invocation_short_name, cmd);
  rc = system(cmd);
  if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
    fprintf(stderr, "%s: xrandr display setup failed, keeping default layout.\n",
            program_invocation_short_name);
    return 0;
  }
  XSync(dpy, False);
  return 1;
}

/* I'm going to use xrandr to not complicate things with libXrandr */
static int configure_monitors (const char *arg) {
  /* MonitorInfo mons[16]; */
  MonitorInfo mons[MAX_MONITORS];
  int n, i, selected = -1;
  char cmd[4096] = "xrandr";

  if (!arg || strcmp(arg, "/extend") == 0 || strcmp(arg, "--extend") == 0)
    return 1;

  /* n = read_monitors(mons, (int)(sizeof(mons) / sizeof(mons[0]))); */
  n = read_monitors(mons, MAX_MONITORS);
  if (n <= 0) {
    fprintf(stderr, "%s: error reading connected outputs. Is xrandr installed?\n",
            program_invocation_short_name);
    return 0;
  }

  if (strcmp(arg, "/mirror") == 0 || strcmp(arg, "--mirror") == 0) {
    int ref = 0;
    const char *common = NULL;
    for (i = 0; i < n; ++i) if (mons[i].primary) { ref = i; break; }
    for (i = 0; i < mons[ref].nmodes && !common; ++i) {
      int j, all = 1;
      for (j = 0; j < n; ++j)
        if (!monitor_has_mode(&mons[j], mons[ref].modes[i])) { all = 0; break; }
      if (all) common = mons[ref].modes[i];
    }
    if (!common) {
      fprintf(stderr, "%s: no common video mode across all connected outputs\n",
              program_invocation_short_name);
      return 0;
    }
    for (i = 0; i < n; ++i) {
      char part[192];
      snprintf(part, sizeof(part), " --output %.63s --mode %.31s --pos 0x0%s",
               mons[i].name, common, i == ref ? " --primary" : "");
      if (!append_cmd(cmd, sizeof(cmd), part)) return 0;
    }
    return run_xrandr_command(cmd);
  }

  if (arg[0] == '/' && arg[1] && strspn(arg + 1, "0123456789") == strlen(arg + 1)) {
    long idx = strtol(arg + 1, NULL, 10);
    if (idx < 1 || idx > n) {
      fprintf(stderr, "%s: monitor %ld is invalid. Connected monitors are 1..%d\n",
              program_invocation_short_name, idx, n);
      return 0;
    }
    selected = (int)idx - 1;
    for (i = 0; i < n; ++i) {
      char part[160];
      if (i == selected)
        snprintf(part, sizeof(part), " --output %.63s --auto --pos 0x0 --primary", mons[i].name);
      else
        snprintf(part, sizeof(part), " --output %.63s --off", mons[i].name);
      if (!append_cmd(cmd, sizeof(cmd), part)) return 0;
    }
    return run_xrandr_command(cmd);
  }

  fprintf(stderr,
          "%s\n\nUsage: %s [/extend | /mirror | /N]\n"
          "  /extend   Xorg's current extended layout (default)\n"
          "  /mirror   Mirroring connected outputs with the best common mode\n"
          "  /N        use only monitor N (number), power the rest off\n",
          WM_TUI_VERSION_STRING, program_invocation_short_name);
  return 0;
}

static int xerror (Display *display, XErrorEvent *e) {
  char text[128];
  (void)display;
  XGetErrorText(dpy, e->error_code, text, sizeof(text));
  fprintf(stderr, "%s: X error: request=%u error=%u (%s) resource=0x%lx\n",
          program_invocation_short_name,
          e->request_code, e->error_code, text, e->resourceid);
  return 0;
}

/* Handling BadWindow if a client disappears while inspecting it */
static int save_set_error;

static int save_set_xerror (Display *display, XErrorEvent *e) {
  (void)display;
  save_set_error = e->error_code;
  return 0;
}

static int change_save_set_safely (Window window, int mode) {
  int (*old_handler)(Display *, XErrorEvent *);
  XSync(dpy, False);
  save_set_error = 0;
  old_handler = XSetErrorHandler(save_set_xerror);
  XChangeSaveSet(dpy, window, mode);
  XSync(dpy, False);
  XSetErrorHandler(old_handler);
  return save_set_error == 0;
}

/* ================================================================ */
/* Windows 3.0-style decorations */
/* ================================================================ */

static int frame_width (const Win3Window *w) {
  return w->width + 2 * FRAME_EDGE;
}

static int frame_height (const Win3Window *w) {
  return w->height + TITLE_H + 2 * FRAME_EDGE;
}

static int client_off_x (void) { return FRAME_EDGE; }
static int client_off_y (void) { return FRAME_EDGE + TITLE_H; }

static Win3Window *find_client (Window window) {
  Win3Window *w;
  for (w = windows; w; w = w->next)
    if (w->client == window) return w;
  return NULL;
}

static Win3Window *find_any (Window window) {
  Win3Window *w;
  for (w = windows; w; w = w->next)
    if (w->client == window || w->frame == window || w->titlebar == window ||
        w->iconwin == window)
      return w;
  return NULL;
}

static void set_wm_state (Win3Window *w, long state) {
  long data[2] = { state, None };
  XChangeProperty(dpy, w->client, wm_state_atom, wm_state_atom,
                  32, PropModeReplace, (unsigned char *)data, 2);
}

static void send_delete (Win3Window *w) {
  Atom *protocols = NULL;
  int n = 0, i;
  int supports_delete = 0;

  if (XGetWMProtocols(dpy, w->client, &protocols, &n)) {
    for (i = 0; i < n; ++i)
      if (protocols[i] == wm_delete_window_atom) supports_delete = 1;
    if (protocols) XFree(protocols);
  }

  if (supports_delete) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w->client;
    ev.xclient.message_type = wm_protocols_atom;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)wm_delete_window_atom;
    ev.xclient.data.l[1] = (long)CurrentTime;
    XSendEvent(dpy, w->client, False, NoEventMask, &ev);
  } else {
    XKillClient(dpy, w->client);
  }
}

static char *get_title (Window window) {
  XTextProperty prop;
  char **list = NULL;
  int count = 0;
  char *result = NULL;

  if (XGetWMName(dpy, window, &prop) && prop.value) {
    if (prop.encoding == XA_STRING) {
      result = strdup((char *)prop.value);
    } else if (XmbTextPropertyToTextList(dpy, &prop, &list, &count) >= Success &&
               count > 0 && list && list[0]) {
      result = strdup(list[0]);
    }
    if (list) XFreeStringList(list);
    XFree(prop.value);
  }
  return result ? result : strdup("Untitled");
}

static void draw_bevel (Window window, GC gc, int x, int y, int width, int height, int raised) {
  unsigned long tl = raised ? C_WHITE : C_DARKGRAY;
  unsigned long br = raised ? C_DARKGRAY : C_WHITE;
  XSetForeground(dpy, gc, tl);
  XDrawLine(dpy, window, gc, x, y, x + width - 1, y);
  XDrawLine(dpy, window, gc, x, y, x, y + height - 1);
  XSetForeground(dpy, gc, br);
  XDrawLine(dpy, window, gc, x, y + height - 1, x + width - 1, y + height - 1);
  XDrawLine(dpy, window, gc, x + width - 1, y, x + width - 1, y + height - 1);
}

/* Short horizontal bar as a control menu */
static void draw_control_box (Window window, GC gc, int pressed) {
  int x = 1, y = 1, w = CTRL_W - 2, h = CTRL_H - 2;
  XSetForeground(dpy, gc, C_FACE);
  XFillRectangle(dpy, window, gc, x, y, (unsigned)w, (unsigned)h);
  draw_bevel(window, gc, x, y, w, h, !pressed);
  XSetForeground(dpy, gc, C_BLACK);
  XFillRectangle(dpy, window, gc, x + 4, y + 6, (unsigned)(w - 8), 2);
}

/* Downward triangle as a minimize button */
static void draw_min_button (Window window, GC gc, int x, int pressed) {
  int y = 1, w = CTRL_W - 2, h = CTRL_H - 2;
  XSetForeground(dpy, gc, C_FACE);
  XFillRectangle(dpy, window, gc, x, y, (unsigned)w, (unsigned)h);
  draw_bevel(window, gc, x, y, w, h, !pressed);
  XSetForeground(dpy, gc, C_BLACK);
  XDrawLine(dpy, window, gc, x + 5, y + 6, x + w / 2, y + 10);
  XDrawLine(dpy, window, gc, x + w / 2, y + 10, x + w - 6, y + 6);
  XDrawLine(dpy, window, gc, x + 6, y + 7, x + w - 7, y + 7);
}

/* Upward triangle as a maximize button */
static void draw_max_button (Window window, GC gc, int x, int pressed, int maximized) {
  int y = 1, w = CTRL_W - 2, h = CTRL_H - 2;
  XSetForeground(dpy, gc, C_FACE);
  XFillRectangle(dpy, window, gc, x, y, (unsigned)w, (unsigned)h);
  draw_bevel(window, gc, x, y, w, h, !pressed);
  XSetForeground(dpy, gc, C_BLACK);
  if (!maximized) {
    XDrawLine(dpy, window, gc, x + 5, y + 9, x + w / 2, y + 5);
    XDrawLine(dpy, window, gc, x + w / 2, y + 5, x + w - 6, y + 9);
    XDrawLine(dpy, window, gc, x + 6, y + 8, x + w - 7, y + 8);
  } else {
    XDrawRectangle(dpy, window, gc, x + 5, y + 5, 6, 5);
    XDrawRectangle(dpy, window, gc, x + 7, y + 7, 6, 5);
  }
}

#if WIN3_SHOW_CLOSE_BUTTON
static void draw_close_button (Window window, GC gc, int x, int pressed) {
  int y = 1, w = CTRL_W - 2, h = CTRL_H - 2;
  XSetForeground(dpy, gc, C_FACE);
  XFillRectangle(dpy, window, gc, x, y, (unsigned)w, (unsigned)h);
  draw_bevel(window, gc, x, y, w, h, !pressed);
  XSetForeground(dpy, gc, C_BLACK);
  XDrawLine(dpy, window, gc, x + 5, y + 5, x + w - 6, y + h - 6);
  XDrawLine(dpy, window, gc, x + w - 6, y + 5, x + 5, y + h - 6);
}
#endif

static int right_button_count (void) {
  return WIN3_SHOW_CLOSE_BUTTON ? 3 : 2;
}

static int buttons_left (const Win3Window *w) {
  return frame_width(w) - 2 * FRAME_EDGE -
    right_button_count() * (CTRL_W + CTRL_GAP) + CTRL_GAP;
}

static void draw_titlebar (Win3Window *w) {
  XWindowAttributes a;
  GC gc;
  int title_width, text_x, text_y, max_text_width, len;
  unsigned long bg = (w == active_win) ? C_ACTIVE_TITLE : C_INACTIVE;

  if (!w->titlebar || !XGetWindowAttributes(dpy, w->titlebar, &a)) return;
  title_width = a.width;
  gc = XCreateGC(dpy, w->titlebar, 0, NULL);

  XSetForeground(dpy, gc, bg);
  XFillRectangle(dpy, w->titlebar, gc, 0, 0,
                 (unsigned)title_width, (unsigned)TITLE_H);

  draw_control_box(w->titlebar, gc, 0);

  {
    int x = buttons_left(w) - FRAME_EDGE;
    draw_min_button(w->titlebar, gc, x, 0);
    x += CTRL_W + CTRL_GAP;
    draw_max_button(w->titlebar, gc, x, 0, w->state == STATE_MAXIMIZED);
#if WIN3_SHOW_CLOSE_BUTTON
    x += CTRL_W + CTRL_GAP;
    draw_close_button(w->titlebar, gc, x, 0);
#endif
  }

  if (font_info && w->title) {
    text_x = CTRL_W + 6;
    text_y = (TITLE_H + font_info->ascent - font_info->descent) / 2;
    max_text_width = buttons_left(w) - FRAME_EDGE - text_x - 4;
    len = (int)strlen(w->title);
    while (len > 0 && XTextWidth(font_info, w->title, len) > max_text_width) --len;

    XSetFont(dpy, gc, font_info->fid);
    XSetForeground(dpy, gc, (w == active_win) ? C_WHITE : C_BLACK);
    if (len > 0)
      XDrawString(dpy, w->titlebar, gc, text_x, text_y, w->title, len);
  }

  XFreeGC(dpy, gc);
}

/* Edge frame highlighted from the upper left, shading it to the lower right */
static void draw_frame (Win3Window *w) {
  GC gc;
  int fw = frame_width(w), fh = frame_height(w);
  gc = XCreateGC(dpy, w->frame, 0, NULL);

  XSetForeground(dpy, gc, C_FACE);
  XFillRectangle(dpy, w->frame, gc, 0, 0, (unsigned)fw, (unsigned)fh);

  XSetForeground(dpy, gc, C_BLACK);
  XDrawRectangle(dpy, w->frame, gc, 0, 0, (unsigned)(fw - 1), (unsigned)(fh - 1));
  draw_bevel(w->frame, gc, 1, 1, fw - 2, fh - 2, 1);

  XSetForeground(dpy, gc, C_BLACK);
  XDrawRectangle(dpy, w->frame, gc,
                 FRAME_EDGE - 1, FRAME_EDGE + TITLE_H - 1,
                 (unsigned)(w->width + 1), (unsigned)(w->height + 1));
  XFreeGC(dpy, gc);
}

/* ================================================================ */
/* Windows 3.0-style objects' behavior */
/* ================================================================ */

static void focus_window (Win3Window *w);
static void constrain_client_position (Win3Window *w);
static void apply_geometry (Win3Window *w);
static void restore_from_icon (Win3Window *w);
static void end_alt_tab (void);
static void draw_task_list (void);
static void draw_resize_outline (void);

static void draw_fallback_icon (Window window, GC gc, int x, int y) {
  XSetForeground(dpy, gc, C_FACE);
  XFillRectangle(dpy, window, gc, x + 3, y + 4, 26, 23);
  XSetForeground(dpy, gc, C_BLACK);
  XDrawRectangle(dpy, window, gc, x + 3, y + 4, 25, 22);
  XSetForeground(dpy, gc, C_ACTIVE_TITLE);
  XFillRectangle(dpy, window, gc, x + 5, y + 6, 22, 5);
  XSetForeground(dpy, gc, C_WHITE);
  XDrawLine(dpy, window, gc, x + 6, y + 7, x + 24, y + 7);
  XSetForeground(dpy, gc, C_BLACK);
  XDrawRectangle(dpy, window, gc, x + 8, y + 14, 15, 8);
}

static int draw_wm_icon_pixmap (Win3Window *w, Window window, GC gc, int box_x, int box_y) {
  XWMHints *hints;
  Window rr;
  int px, py;
  unsigned int pw, ph, bw, depth;
  int dx, dy;
  unsigned int cw, ch;
  int ok = 0;

  hints = XGetWMHints(dpy, w->client);
  if (!hints || !(hints->flags & IconPixmapHint) || hints->icon_pixmap == None)
    goto out;
  if (!XGetGeometry(dpy, hints->icon_pixmap, &rr, &px, &py, &pw, &ph, &bw, &depth))
    goto out;

  cw = pw > ICON_ART_W ? ICON_ART_W : pw;
  ch = ph > ICON_ART_H ? ICON_ART_H : ph;
  dx = box_x + (ICON_ART_W - (int)cw) / 2;
  dy = box_y + (ICON_ART_H - (int)ch) / 2;

  if (depth == 1) {
    XSetForeground(dpy, gc, C_BLACK);
    XSetBackground(dpy, gc, C_DESKTOP);
    XCopyPlane(dpy, hints->icon_pixmap, window, gc, 0, 0, cw, ch, dx, dy, 1);
    ok = 1;
  } else if (depth == (unsigned)DefaultDepth(dpy, screen)) {
    XCopyArea(dpy, hints->icon_pixmap, window, gc, 0, 0, cw, ch, dx, dy);
    ok = 1;
  }

 out:
  if (hints) XFree(hints);
  return ok;
}

static void draw_icon (Win3Window *w) {
  GC gc;
  int art_x = (ICON_CELL_W - ICON_ART_W) / 2;
  int art_y = 1;
  int len, tw, tx, baseline;

  if (!w || !w->iconwin) return;
  gc = XCreateGC(dpy, w->iconwin, 0, NULL);
  XSetForeground(dpy, gc, C_DESKTOP);
  XFillRectangle(dpy, w->iconwin, gc, 0, 0, ICON_CELL_W, ICON_CELL_H);

  if (!draw_wm_icon_pixmap(w, w->iconwin, gc, art_x, art_y))
    draw_fallback_icon(w->iconwin, gc, art_x, art_y);

  if (font_info && w->title) {
    len = (int)strlen(w->title);
    while (len > 0 && XTextWidth(font_info, w->title, len) > ICON_CELL_W - 4) --len;
    XSetFont(dpy, gc, font_info->fid);
    tw = len > 0 ? XTextWidth(font_info, w->title, len) : 0;
    tx = MAX(2, (ICON_CELL_W - tw) / 2);
    baseline = ICON_CELL_H - 3;
    if (w == active_win) {
      XSetForeground(dpy, gc, C_ACTIVE_TITLE);
      XFillRectangle(dpy, w->iconwin, gc, tx - 1,
                     baseline - font_info->ascent, (unsigned)(tw + 2),
                     (unsigned)(font_info->ascent + font_info->descent + 1));
      XSetForeground(dpy, gc, C_WHITE);
    } else {
      XSetForeground(dpy, gc, C_BLACK);
    }
    if (len > 0) XDrawString(dpy, w->iconwin, gc, tx, baseline, w->title, len);
  }
  XFreeGC(dpy, gc);
}

static int icon_seq_cmp (const void *aa, const void *bb) {
  const Win3Window *a = *(Win3Window * const *)aa;
  const Win3Window *b = *(Win3Window * const *)bb;
  return a->icon_seq < b->icon_seq ? -1 : a->icon_seq > b->icon_seq ? 1 : 0;
}

static void arrange_icons (void) {
  XWindowAttributes ra;
  Win3Window *w;
  Win3Window **list;
  int count = 0, i = 0, cols;

  for (w = windows; w; w = w->next)
    if (w->state == STATE_MINIMIZED && w->iconwin) ++count;
  if (!count || !XGetWindowAttributes(dpy, root, &ra)) return;

  list = malloc((size_t)count * sizeof(*list));
  if (!list) return;
  for (w = windows; w; w = w->next)
    if (w->state == STATE_MINIMIZED && w->iconwin) list[i++] = w;
  qsort(list, (size_t)count, sizeof(*list), icon_seq_cmp);

  cols = MAX(1, ra.width / ICON_CELL_W);
  for (i = 0; i < count; ++i) {
    int col = i % cols;
    int row = i / cols;
    int x = col * ICON_CELL_W;
    int y = ra.height - (row + 1) * ICON_CELL_H;
    if (y < 0) y = 0;
    XMoveWindow(dpy, list[i]->iconwin, x, y);
  }
  free(list);
}

static void ensure_icon_window (Win3Window *w) {
  XSetWindowAttributes ia;
  if (w->iconwin) return;
  memset(&ia, 0, sizeof(ia));
  ia.override_redirect = True;
  ia.background_pixel = C_DESKTOP;
  ia.event_mask = ExposureMask | ButtonPressMask;
  w->iconwin = XCreateWindow(dpy, root, 0, 0, ICON_CELL_W, ICON_CELL_H, 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWBackPixel | CWEventMask, &ia);
}

static void restore_from_icon (Win3Window *w) {
  if (!w || w->state != STATE_MINIMIZED) return;
  if (w->iconwin) XUnmapWindow(dpy, w->iconwin);
  w->state = (w->restore_state == STATE_MAXIMIZED) ? STATE_MAXIMIZED : STATE_NORMAL;
  if (w->state == STATE_NORMAL) {
    w->x = w->normal_x;
    w->y = w->normal_y;
    w->width = w->normal_width;
    w->height = w->normal_height;
  }
  constrain_client_position(w);
  apply_geometry(w);
  XMapWindow(dpy, w->titlebar);
  XMapWindow(dpy, w->client);
  XMapWindow(dpy, w->frame);
  set_wm_state(w, NormalState);
  focus_window(w);
  arrange_icons();
}

static void focus_window (Win3Window *w) {
  Win3Window *old = active_win;
  if (!w) return;
  active_win = w;
  w->focus_seq = ++next_focus_seq;
  XRaiseWindow(dpy, w->frame);
  XSetInputFocus(dpy, w->client, RevertToPointerRoot, CurrentTime);
  if (old && old != w) draw_titlebar(old);
  draw_titlebar(w);
}

static void send_configure (Win3Window *w) {
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xconfigure.type = ConfigureNotify;
  ev.xconfigure.display = dpy;
  ev.xconfigure.event = w->client;
  ev.xconfigure.window = w->client;
  ev.xconfigure.x = w->x;
  ev.xconfigure.y = w->y;
  ev.xconfigure.width = w->width;
  ev.xconfigure.height = w->height;
  ev.xconfigure.border_width = 0;
  ev.xconfigure.above = None;
  ev.xconfigure.override_redirect = False;
  XSendEvent(dpy, w->client, False, StructureNotifyMask, &ev);
}

/* Keeping the decoration based on source coordinates to extend the frame edges */
static void constrain_client_position (Win3Window *w) {
  XWindowAttributes ra;
  int min_x = FRAME_EDGE;
  int min_y = FRAME_EDGE + TITLE_H;
  int max_x, max_y;

  if (!XGetWindowAttributes(dpy, root, &ra)) return;

  max_x = MAX(min_x, ra.width - w->width - FRAME_EDGE);
  max_y = MAX(min_y, ra.height - w->height - FRAME_EDGE);

  if (w->x < min_x) w->x = min_x;
  if (w->y < min_y) w->y = min_y;
  if (w->x > max_x) w->x = max_x;
  if (w->y > max_y) w->y = max_y;
}

static void apply_geometry (Win3Window *w) {
  int fx = w->x - FRAME_EDGE;
  int fy = w->y - FRAME_EDGE - TITLE_H;
  int fw = frame_width(w);
  int fh = frame_height(w);

  XMoveResizeWindow(dpy, w->frame, fx, fy, (unsigned)fw, (unsigned)fh);
  XMoveResizeWindow(dpy, w->titlebar, FRAME_EDGE, FRAME_EDGE,
                    (unsigned)(fw - 2 * FRAME_EDGE), (unsigned)TITLE_H);
  XMoveResizeWindow(dpy, w->client, client_off_x(), client_off_y(),
                    (unsigned)MAX(1, w->width), (unsigned)MAX(1, w->height));
  send_configure(w);
}

static void read_normal_hints (Win3Window *w) {
  XSizeHints hints;
  long supplied = 0;
  if (XGetWMNormalHints(dpy, w->client, &hints, &supplied)) {
    if ((hints.flags & PPosition) || (hints.flags & USPosition)) {
      w->x = hints.x;
      w->y = hints.y;
    }
    if (((hints.flags & PSize) || (hints.flags & USSize)) &&
        hints.width > 0 && hints.height > 0) {
      w->width = hints.width;
      w->height = hints.height;
    }
  }
}

/* ================================================================ */
/* Windows 3.0-style objects' handling */
/* ================================================================ */

static Win3Window *manage (Window client) {
  XWindowAttributes a;
  XSetWindowAttributes fa, ta;
  Win3Window *w;
  int fx, fy;

  if (find_client(client)) return find_client(client);
  if (!XGetWindowAttributes(dpy, client, &a)) return NULL;
  if (a.override_redirect || a.class == InputOnly) return NULL;

  w = calloc(1, sizeof(*w));
  if (!w) return NULL;
  w->client = client;
  w->title = get_title(client);
  w->state = STATE_NORMAL;
  w->restore_state = STATE_NORMAL;
  w->x = a.x;
  w->y = a.y;
  w->width = MAX(1, a.width);
  w->height = MAX(1, a.height);
  read_normal_hints(w);
  constrain_client_position(w);
  w->normal_x = w->x;
  w->normal_y = w->y;
  w->normal_width = w->width;
  w->normal_height = w->height;

  /* adding children to the list to be assumed as internal and notified */
  w->next = windows;
  windows = w;

  fx = w->x - FRAME_EDGE;
  fy = w->y - FRAME_EDGE - TITLE_H;

  memset(&fa, 0, sizeof(fa));
  fa.override_redirect = True;
  fa.background_pixel = C_FACE;
  fa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
    PointerMotionMask | StructureNotifyMask;
  w->frame = XCreateWindow(dpy, root, fx, fy,
                           (unsigned)frame_width(w), (unsigned)frame_height(w),
                           0, CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel | CWEventMask, &fa);

  memset(&ta, 0, sizeof(ta));
  ta.background_pixel = C_ACTIVE_TITLE;
  ta.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
    PointerMotionMask;
  w->titlebar = XCreateWindow(dpy, w->frame, FRAME_EDGE, FRAME_EDGE,
                              (unsigned)(frame_width(w) - 2 * FRAME_EDGE),
                              TITLE_H, 0, CopyFromParent, InputOutput,
                              CopyFromParent, CWBackPixel | CWEventMask, &ta);

  XSelectInput(dpy, client, PropertyChangeMask | StructureNotifyMask |
               FocusChangeMask);
  /* Clean the client up if it vanished while manage() was being assembled */
  if (!change_save_set_safely(client, SetModeInsert)) {
    XDestroyWindow(dpy, w->frame);
    windows = w->next;
    free(w->title);
    free(w);
    return NULL;
  }
  XSetWindowBorderWidth(dpy, client, 0);

  /* Reparenting a client which was already mapped at WM startup */
  if (a.map_state != IsUnmapped) ++w->ignore_unmap;
  XReparentWindow(dpy, client, w->frame, client_off_x(), client_off_y());

  apply_geometry(w);

  /* The mapping goes first, then expose to avoid the title hiding the buttons */
  XMapWindow(dpy, w->titlebar);
  XMapWindow(dpy, client);
  XMapWindow(dpy, w->frame);
  set_wm_state(w, NormalState);
  focus_window(w);
  XFlush(dpy);
  return w;
}

static void unmanage (Win3Window *w, int client_destroyed) {
  Win3Window **p;
  if (!w) return;
  if (alt_tab_active) end_alt_tab();

  if (active_win == w) active_win = NULL;
  if (drag_win == w) { drag_win = NULL; dragging = 0; }
  if (resize_win == w) {
    if (resizing) draw_resize_outline();
    resize_win = NULL; resizing = 0;
    XUngrabPointer(dpy, CurrentTime);
  }

  if (!client_destroyed) {
    XWindowAttributes fa;
    int rx = w->x, ry = w->y;
    if (w->frame && XGetWindowAttributes(dpy, w->frame, &fa)) {
      rx = fa.x + FRAME_EDGE;
      ry = fa.y + FRAME_EDGE + TITLE_H;
    }
    change_save_set_safely(w->client, SetModeDelete);
    XReparentWindow(dpy, w->client, root, rx, ry);
    XSetWindowBorderWidth(dpy, w->client, 0);
  }
  if (w->iconwin) { XDestroyWindow(dpy, w->iconwin); w->iconwin = None; }
  if (w->frame) XDestroyWindow(dpy, w->frame);

  for (p = &windows; *p && *p != w; p = &(*p)->next) {}
  if (*p) *p = w->next;
  arrange_icons();
  free(w->title);
  free(w);
  if (task_open) draw_task_list();
}

static void maximize_or_restore (Win3Window *w) {
  XWindowAttributes ra;
  if (w->state == STATE_MAXIMIZED) {
    w->state = STATE_NORMAL;
    w->x = w->normal_x; w->y = w->normal_y;
    w->width = w->normal_width; w->height = w->normal_height;
  } else {
    XGetWindowAttributes(dpy, root, &ra);
    w->normal_x = w->x; w->normal_y = w->y;
    w->normal_width = w->width; w->normal_height = w->height;
    w->state = STATE_MAXIMIZED;
    w->x = FRAME_EDGE;
    w->y = FRAME_EDGE + TITLE_H;
    w->width = ra.width - 2 * FRAME_EDGE;
    w->height = ra.height - TITLE_H - 2 * FRAME_EDGE;
  }
  apply_geometry(w);
  XClearWindow(dpy, w->frame);
  XClearWindow(dpy, w->titlebar);
}

/* No taskbar, a minimized app became an icon on the desktop */
static void minimize_window (Win3Window *w) {
  if (!w || w->state == STATE_MINIMIZED) return;
  w->restore_state = w->state;
  if (w->state == STATE_NORMAL) {
    w->normal_x = w->x; w->normal_y = w->y;
    w->normal_width = w->width; w->normal_height = w->height;
  }
  w->state = STATE_MINIMIZED;
  w->icon_seq = ++next_icon_seq;
  ensure_icon_window(w);
  XUnmapWindow(dpy, w->frame);
  set_wm_state(w, IconicState);
  if (active_win == w) active_win = NULL;
  arrange_icons();
  XMapRaised(dpy, w->iconwin);
  XClearWindow(dpy, w->iconwin);
}

static int control_box_hit (int x, int y) {
  return x >= 1 && x < CTRL_W - 1 && y >= 1 && y < CTRL_H - 1;
}

static int title_button_at (Win3Window *w, int x, int y) {
  int bx = buttons_left(w) - FRAME_EDGE;
  int bw = CTRL_W - 2;
  if (y < 1 || y >= CTRL_H - 1) return 0;
  if (x >= bx && x < bx + bw) return 1; /* min */
  bx += CTRL_W + CTRL_GAP;
  if (x >= bx && x < bx + bw) return 2; /* max/restore */
#if WIN3_SHOW_CLOSE_BUTTON
  bx += CTRL_W + CTRL_GAP;
  if (x >= bx && x < bx + bw) return 3;
#endif
  return 0;
}

/* Resize a normal window by dragging its border */
static int resize_edges_at (Win3Window *w, int x, int y) {
  int edges = 0;
  int fw = frame_width(w), fh = frame_height(w);
  if (x < FRAME_EDGE) edges |= RESIZE_LEFT;
  else if (x >= fw - FRAME_EDGE) edges |= RESIZE_RIGHT;
  if (y < FRAME_EDGE) edges |= RESIZE_TOP;
  else if (y >= fh - FRAME_EDGE) edges |= RESIZE_BOTTOM;
  return edges;
}

static void constrain_resize_size (Win3Window *w, int *cw, int *ch) {
  XSizeHints h;
  long supplied = 0;
  int basew = 0, baseh = 0;

  if (!XGetWMNormalHints(dpy, w->client, &h, &supplied)) return;
  if ((h.flags & PMinSize)) {
    if (*cw < h.min_width) *cw = h.min_width;
    if (*ch < h.min_height) *ch = h.min_height;
  }
  if ((h.flags & PMaxSize)) {
    if (*cw > h.max_width) *cw = h.max_width;
    if (*ch > h.max_height) *ch = h.max_height;
  }
  if (h.flags & PBaseSize) { basew = h.base_width; baseh = h.base_height; }
  else if (h.flags & PMinSize) { basew = h.min_width; baseh = h.min_height; }
  if ((h.flags & PResizeInc) && h.width_inc > 0)
    *cw = basew + MAX(0, (*cw - basew) / h.width_inc) * h.width_inc;
  if ((h.flags & PResizeInc) && h.height_inc > 0)
    *ch = baseh + MAX(0, (*ch - baseh) / h.height_inc) * h.height_inc;
}

static void draw_resize_outline (void) {
  if (!resize_gc) {
    XGCValues gcv;
    memset(&gcv, 0, sizeof(gcv));
    /* gcv.function = GXinvert; */
    /* gcv.plane_mask = AllPlanes; */
    /* gcv.subwindow_mode = IncludeInferiors; */
    /* resize_gc = XCreateGC(dpy, root, GCFunction | GCPlaneMask | GCSubwindowMode, &gcv); */
    gcv.function = GXxor;
    gcv.foreground = C_DESKTOP ^ BlackPixel(dpy, screen);
    gcv.subwindow_mode = IncludeInferiors;
    resize_gc = XCreateGC(dpy, root, GCFunction | GCForeground | GCSubwindowMode, &gcv);
  }
  XDrawRectangle(dpy, root, resize_gc, resize_x, resize_y,
                 (unsigned)MAX(1, resize_w - 1), (unsigned)MAX(1, resize_h - 1));
}

static void start_resize (Win3Window *w, XButtonEvent *e) {
  XWindowAttributes a;
  int edges;
  if (!w || w->state != STATE_NORMAL || e->button != Button1) return;
  edges = resize_edges_at(w, e->x, e->y);
  if (!edges || !XGetWindowAttributes(dpy, w->frame, &a)) return;

  focus_window(w);
  resizing = 1;
  resize_win = w;
  resize_edges = edges;
  resize_start_x_root = e->x_root;
  resize_start_y_root = e->y_root;
  resize_start_x = resize_x = a.x;
  resize_start_y = resize_y = a.y;
  resize_start_w = resize_w = a.width;
  resize_start_h = resize_h = a.height;
  XGrabPointer(dpy, w->frame, False, PointerMotionMask | ButtonReleaseMask,
               GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
  draw_resize_outline();
}

static void update_resize (int x_root, int y_root) {
  int dx, dy, x, y, fw, fh, cw, ch;
  int right = resize_start_x + resize_start_w;
  int bottom = resize_start_y + resize_start_h;
  if (!resizing || !resize_win) return;

  draw_resize_outline();                 /* erase old XOR outline */
  dx = x_root - resize_start_x_root;
  dy = y_root - resize_start_y_root;
  x = resize_start_x; y = resize_start_y;
  fw = resize_start_w; fh = resize_start_h;
  if (resize_edges & RESIZE_LEFT)   { x += dx; fw -= dx; }
  if (resize_edges & RESIZE_RIGHT)  fw += dx;
  if (resize_edges & RESIZE_TOP)    { y += dy; fh -= dy; }
  if (resize_edges & RESIZE_BOTTOM) fh += dy;

  cw = MAX(1, fw - 2 * FRAME_EDGE);
  ch = MAX(1, fh - TITLE_H - 2 * FRAME_EDGE);
  constrain_resize_size(resize_win, &cw, &ch);
  fw = cw + 2 * FRAME_EDGE;
  fh = ch + TITLE_H + 2 * FRAME_EDGE;
  if (resize_edges & RESIZE_LEFT) x = right - fw;
  if (resize_edges & RESIZE_TOP) y = bottom - fh;

  resize_x = x; resize_y = y; resize_w = fw; resize_h = fh;
  draw_resize_outline();                 /* draw new outline */
}

static void finish_resize (void) {
  Win3Window *w = resize_win;
  if (!resizing) return;
  draw_resize_outline();                 /* erase final outline */
  XUngrabPointer(dpy, CurrentTime);
  resizing = 0;
  resize_win = NULL;
  if (!w) return;

  w->x = resize_x + FRAME_EDGE;
  w->y = resize_y + FRAME_EDGE + TITLE_H;
  w->width = MAX(1, resize_w - 2 * FRAME_EDGE);
  w->height = MAX(1, resize_h - TITLE_H - 2 * FRAME_EDGE);
  w->normal_x = w->x; w->normal_y = w->y;
  w->normal_width = w->width; w->normal_height = w->height;
  apply_geometry(w);
  draw_frame(w);
  draw_titlebar(w);
}

/* You had to double-click on the Control menu to close the app */
static void handle_title_press (Win3Window *w, XButtonEvent *e) {
  int button;
  focus_window(w);
  if (control_box_hit(e->x, e->y)) {
    if (last_control_click_window == w->client &&
        e->time - last_control_click_time <= 350) {
      send_delete(w);
      last_control_click_window = None;
      last_control_click_time = 0;
    } else {
      last_control_click_window = w->client;
      last_control_click_time = e->time;
    }
    return;
  }

  button = title_button_at(w, e->x, e->y);
  if (button == 1) { minimize_window(w); return; }
  if (button == 2) { maximize_or_restore(w); return; }
#if WIN3_SHOW_CLOSE_BUTTON
  if (button == 3) { send_delete(w); return; }
#endif

  if (e->button == Button1 && w->state == STATE_NORMAL) {
    XWindowAttributes a;
    XGetWindowAttributes(dpy, w->frame, &a);
    dragging = 1;
    drag_win = w;
    drag_start_x_root = e->x_root;
    drag_start_y_root = e->y_root;
    drag_frame_x = a.x;
    drag_frame_y = a.y;
    XGrabPointer(dpy, w->titlebar, False,
                 PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
  }
}

static void handle_configure_request (XConfigureRequestEvent *e) {
  Win3Window *w = find_client(e->window);
  if (!w) {
    XWindowChanges wc;
    wc.x = e->x; wc.y = e->y;
    wc.width = e->width; wc.height = e->height;
    wc.border_width = e->border_width;
    wc.sibling = e->above;
    wc.stack_mode = e->detail;
    XConfigureWindow(dpy, e->window, e->value_mask, &wc);
    return;
  }

  if (w->state == STATE_NORMAL) {
    if (e->value_mask & CWX) w->x = e->x;
    if (e->value_mask & CWY) w->y = e->y;
    if (e->value_mask & CWWidth) w->width = MAX(1, e->width);
    if (e->value_mask & CWHeight) w->height = MAX(1, e->height);
    constrain_client_position(w);
    w->normal_x = w->x; w->normal_y = w->y;
    w->normal_width = w->width; w->normal_height = w->height;
    apply_geometry(w);
  } else {
    send_configure(w);
  }

  if (e->value_mask & CWStackMode) {
    XWindowChanges wc;
    memset(&wc, 0, sizeof(wc));
    wc.stack_mode = e->detail;
    if (e->value_mask & CWSibling) {
      Win3Window *sib = find_client(e->above);
      wc.sibling = sib ? sib->frame : e->above;
      XConfigureWindow(dpy, w->frame, CWStackMode | CWSibling, &wc);
    } else {
      XConfigureWindow(dpy, w->frame, CWStackMode, &wc);
    }
  }
}

/* ================================================================ */
/* Windows 3.0-style task switching */
/* ================================================================ */

static int focus_seq_cmp (const void *aa, const void *bb) {
  const Win3Window *a = *(Win3Window * const *)aa;
  const Win3Window *b = *(Win3Window * const *)bb;
  if (a->focus_seq > b->focus_seq) return -1;
  if (a->focus_seq < b->focus_seq) return 1;
  return 0;
}

static void end_alt_tab (void) {
  free(alt_tab_list);
  alt_tab_list = NULL;
  alt_tab_count = 0;
  alt_tab_index = 0;
  alt_tab_active = 0;
}

static int build_alt_tab_list (void) {
  Win3Window *w;
  int i = 0;

  end_alt_tab();
  for (w = windows; w; w = w->next) ++alt_tab_count;
  if (alt_tab_count < 2) {
    alt_tab_count = 0;
    return 0;
  }

  alt_tab_list = malloc((size_t)alt_tab_count * sizeof(*alt_tab_list));
  if (!alt_tab_list) {
    alt_tab_count = 0;
    return 0;
  }

  for (w = windows; w; w = w->next) alt_tab_list[i++] = w;
  qsort(alt_tab_list, (size_t)alt_tab_count, sizeof(*alt_tab_list), focus_seq_cmp);

  /* Make the current application the starting point */
  if (active_win) {
    for (i = 0; i < alt_tab_count; ++i) {
      if (alt_tab_list[i] == active_win) {
        Win3Window *tmp = alt_tab_list[0];
        alt_tab_list[0] = alt_tab_list[i];
        alt_tab_list[i] = tmp;
        break;
      }
    }
  }

  alt_tab_index = 0;
  alt_tab_active = 1;
  return 1;
}

static void switch_to_window (Win3Window *w) {
  if (!w) return;
  if (w->state == STATE_MINIMIZED) restore_from_icon(w);
  else focus_window(w);
}

/* ALT+ESC selects an application window or minimized icon */
static void select_window_or_icon (Win3Window *w) {
  Win3Window *old = active_win;
  if (!w) return;

  if (w->state != STATE_MINIMIZED) {
    focus_window(w);
    return;
  }

  active_win = w;
  w->focus_seq = ++next_focus_seq;
  XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
  if (w->iconwin) XRaiseWindow(dpy, w->iconwin);

  if (old && old != w) {
    if (old->state == STATE_MINIMIZED) draw_icon(old);
    else draw_titlebar(old);
  }
  draw_icon(w);
}

static void handle_alt_esc (void) {
  Win3Window *w;
  if (!windows) return;
  if (!active_win) { select_window_or_icon(windows); return; }

  for (w = windows; w; w = w->next)
    if (w == active_win) {
      select_window_or_icon(w->next ? w->next : windows);
      return;
    }
  select_window_or_icon(windows);
}

static void handle_alt_tab (int backwards) {
  if (!alt_tab_active && !build_alt_tab_list()) return;
  if (!alt_tab_list || alt_tab_count < 2) return;

  if (backwards) {
    --alt_tab_index;
    if (alt_tab_index < 0) alt_tab_index = alt_tab_count - 1;
  } else {
    alt_tab_index = (alt_tab_index + 1) % alt_tab_count;
  }
  switch_to_window(alt_tab_list[alt_tab_index]);
}

/* ================================================================ */
/* Windows 3.0-style task list */
/* ================================================================ */

static int task_count (void) {
  Win3Window *w;
  int n = 0;
  for (w = windows; w; w = w->next) ++n;
  return n;
}

static Win3Window *task_at (int index) {
  Win3Window *w;
  int i = 0;
  for (w = windows; w; w = w->next, ++i)
    if (i == index) return w;
  return NULL;
}

static int task_index_of (Win3Window *needle) {
  Win3Window *w;
  int i = 0;
  for (w = windows; w; w = w->next, ++i)
    if (w == needle) return i;
  return -1;
}

static void draw_task_button (GC gc, int x, int y, const char *label) {
  int len = (int)strlen(label);
  int tw = font_info ? XTextWidth(font_info, label, len) : 0;
  int tx = x + (TASK_BTN_W - tw) / 2;
  int ty = y + (TASK_BTN_H + (font_info ? font_info->ascent - font_info->descent : 8)) / 2;

  XSetForeground(dpy, gc, C_FACE);
  XFillRectangle(dpy, task_win, gc, x, y, TASK_BTN_W, TASK_BTN_H);
  draw_bevel(task_win, gc, x, y, TASK_BTN_W, TASK_BTN_H, 1);
  XSetForeground(dpy, gc, C_BLACK);
  if (font_info) XSetFont(dpy, gc, font_info->fid);
  XDrawString(dpy, task_win, gc, tx, ty, label, len);
}

static void draw_task_list (void) {
  GC gc;
  int n = task_count();
  int rows = TASK_LIST_H / TASK_ROW_H;
  int i, visible;

  if (!task_open || !task_win) return;
  if (n <= 0) { task_selected = task_top = 0; }
  else {
    if (task_selected < 0) task_selected = 0;
    if (task_selected >= n) task_selected = n - 1;
    if (task_selected < task_top) task_top = task_selected;
    if (task_selected >= task_top + rows) task_top = task_selected - rows + 1;
  }

  gc = XCreateGC(dpy, task_win, 0, NULL);
  XSetForeground(dpy, gc, C_FACE);
  XFillRectangle(dpy, task_win, gc, 0, 0, TASK_W, TASK_H);
  XSetForeground(dpy, gc, C_BLACK);
  XDrawRectangle(dpy, task_win, gc, 0, 0, TASK_W - 1, TASK_H - 1);
  draw_bevel(task_win, gc, 1, 1, TASK_W - 2, TASK_H - 2, 1);

  if (font_info) XSetFont(dpy, gc, font_info->fid);
  XSetForeground(dpy, gc, C_BLACK);
  XDrawString(dpy, task_win, gc, TASK_MARGIN, 18, "Task List", 9);

  XSetForeground(dpy, gc, C_WHITE);
  XFillRectangle(dpy, task_win, gc, TASK_MARGIN, TASK_LIST_Y,
                 TASK_W - 2 * TASK_MARGIN, TASK_LIST_H);
  XSetForeground(dpy, gc, C_BLACK);
  XDrawRectangle(dpy, task_win, gc, TASK_MARGIN, TASK_LIST_Y,
                 TASK_W - 2 * TASK_MARGIN - 1, TASK_LIST_H - 1);

  visible = n - task_top;
  if (visible > rows) visible = rows;
  for (i = 0; i < visible; ++i) {
    int idx = task_top + i;
    Win3Window *w = task_at(idx);
    int y = TASK_LIST_Y + 1 + i * TASK_ROW_H;
    int baseline = y + (font_info ? font_info->ascent + 1 : 11);
    int len;
    if (!w || !w->title) continue;

    if (idx == task_selected) {
      XSetForeground(dpy, gc, C_ACTIVE_TITLE);
      XFillRectangle(dpy, task_win, gc, TASK_MARGIN + 2, y,
                     TASK_W - 2 * TASK_MARGIN - 4, TASK_ROW_H);
      XSetForeground(dpy, gc, C_WHITE);
    } else XSetForeground(dpy, gc, C_BLACK);

    len = (int)strlen(w->title);
    while (len > 0 && font_info &&
           XTextWidth(font_info, w->title, len) > TASK_W - 2 * TASK_MARGIN - 8) --len;
    if (len) XDrawString(dpy, task_win, gc, TASK_MARGIN + 4, baseline, w->title, len);
  }

  draw_task_button(gc, TASK_MARGIN, 169, "Switch To");
  draw_task_button(gc, TASK_MARGIN + TASK_BTN_W + TASK_GAP, 169, "End Task");
  draw_task_button(gc, TASK_MARGIN + 2 * (TASK_BTN_W + TASK_GAP), 169, "Cancel");

  draw_task_button(gc, TASK_MARGIN, 211, "Cascade");
  draw_task_button(gc, TASK_MARGIN + TASK_BTN_W + TASK_GAP, 211, "Tile");
  draw_task_button(gc, TASK_MARGIN + 2 * (TASK_BTN_W + TASK_GAP), 211, "Arrange Icons");

  XFreeGC(dpy, gc);
}

static void close_task_list (void) {
  if (!task_open) return;
  task_open = 0;
  XUnmapWindow(dpy, task_win);
  if (active_win && active_win->state != STATE_MINIMIZED)
    XSetInputFocus(dpy, active_win->client, RevertToPointerRoot, CurrentTime);
  else
    XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
}

static void show_task_list (void) {
  XWindowAttributes ra;
  XSetWindowAttributes a;
  int idx;

  if (task_open) { close_task_list(); return; }
  if (!XGetWindowAttributes(dpy, root, &ra)) return;

  if (!task_win) {
    memset(&a, 0, sizeof(a));
    a.override_redirect = True;
    a.background_pixel = C_FACE;
    a.event_mask = ExposureMask | ButtonPressMask | KeyPressMask;
    task_win = XCreateWindow(dpy, root, (ra.width - TASK_W) / 2,
                             (ra.height - TASK_H) / 2, TASK_W, TASK_H, 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
  } else {
    XMoveWindow(dpy, task_win, (ra.width - TASK_W) / 2, (ra.height - TASK_H) / 2);
  }

  idx = task_index_of(active_win);
  task_selected = idx >= 0 ? idx : 0;
  task_top = 0;
  task_open = 1;
  XMapRaised(dpy, task_win);
  XSetInputFocus(dpy, task_win, RevertToPointerRoot, CurrentTime);
  draw_task_list();
}

static int visible_task_count (void) {
  Win3Window *w;
  int n = 0;
  for (w = windows; w; w = w->next)
    if (w->state != STATE_MINIMIZED) ++n;
  return n;
}

static void normal_for_arrange (Win3Window *w) {
  if (w->state != STATE_MAXIMIZED) return;
  w->state = STATE_NORMAL;
  w->x = w->normal_x; w->y = w->normal_y;
  w->width = w->normal_width; w->height = w->normal_height;
}

static void cascade_windows (void) {
  XWindowAttributes ra;
  Win3Window *w;
  int n = visible_task_count(), i = 0;
  int step = TITLE_H + 3, ww, hh;
  if (!n || !XGetWindowAttributes(dpy, root, &ra)) return;

  ww = MAX(160, ra.width - step * MAX(1, n - 1) - 2 * FRAME_EDGE);
  hh = MAX(100, ra.height - step * MAX(1, n - 1) - TITLE_H - 2 * FRAME_EDGE);
  for (w = windows; w; w = w->next) {
    if (w->state == STATE_MINIMIZED) continue;
    normal_for_arrange(w);
    w->x = FRAME_EDGE + i * step;
    w->y = FRAME_EDGE + TITLE_H + i * step;
    w->width = ww; w->height = hh;
    constrain_client_position(w);
    w->normal_x = w->x; w->normal_y = w->y;
    w->normal_width = w->width; w->normal_height = w->height;
    apply_geometry(w);
    XRaiseWindow(dpy, w->frame);
    ++i;
  }
}

static void tile_windows (void) {
  XWindowAttributes ra;
  Win3Window *w;
  int n = visible_task_count(), cols = 1, rows, i = 0;
  int cw, ch;
  if (!n || !XGetWindowAttributes(dpy, root, &ra)) return;

  while (cols * cols < n) ++cols;
  rows = (n + cols - 1) / cols;
  cw = MAX(1, ra.width / cols);
  ch = MAX(1, ra.height / rows);

  for (w = windows; w; w = w->next) {
    int col, row;
    if (w->state == STATE_MINIMIZED) continue;
    normal_for_arrange(w);
    col = i % cols; row = i / cols;
    w->x = col * cw + FRAME_EDGE;
    w->y = row * ch + FRAME_EDGE + TITLE_H;
    w->width = MAX(1, cw - 2 * FRAME_EDGE);
    w->height = MAX(1, ch - TITLE_H - 2 * FRAME_EDGE);
    constrain_client_position(w);
    w->normal_x = w->x; w->normal_y = w->y;
    w->normal_width = w->width; w->normal_height = w->height;
    apply_geometry(w);
    ++i;
  }
}

static void task_switch_selected (void) {
  Win3Window *w = task_at(task_selected);
  close_task_list();
  switch_to_window(w);
}

static void task_button_press (XButtonEvent *e) {
  int x = e->x, y = e->y;
  if (y >= TASK_LIST_Y && y < TASK_LIST_Y + TASK_LIST_H &&
      x >= TASK_MARGIN && x < TASK_W - TASK_MARGIN) {
    int idx = task_top + (y - TASK_LIST_Y - 1) / TASK_ROW_H;
    if (idx >= 0 && idx < task_count()) {
      task_selected = idx;
      if (last_task_click_index == idx && e->time - last_task_click_time <= 350) {
        last_task_click_index = -1; last_task_click_time = 0;
        task_switch_selected();
      } else {
        last_task_click_index = idx; last_task_click_time = e->time;
        draw_task_list();
      }
    }
    return;
  }

  if (y >= 169 && y < 169 + TASK_BTN_H) {
    if (x < TASK_MARGIN + TASK_BTN_W) task_switch_selected();
    else if (x < TASK_MARGIN + 2 * TASK_BTN_W + TASK_GAP) {
      Win3Window *w = task_at(task_selected);
      close_task_list();
      if (w) send_delete(w);
    } else close_task_list();
  } else if (y >= 211 && y < 211 + TASK_BTN_H) {
    close_task_list();
    if (x < TASK_MARGIN + TASK_BTN_W) cascade_windows();
    else if (x < TASK_MARGIN + 2 * TASK_BTN_W + TASK_GAP) tile_windows();
    else arrange_icons();
  }
}

static void task_key_press (XKeyEvent *e) {
  KeySym ks = XLookupKeysym(e, 0);
  int n = task_count();
  int rows = TASK_LIST_H / TASK_ROW_H;

  if (ks == XK_Escape) close_task_list();
  else if (ks == XK_Return || ks == XK_KP_Enter) task_switch_selected();
  else if (ks == XK_Up && n) {
    task_selected = task_selected ? task_selected - 1 : n - 1;
    if (task_selected < task_top) task_top = task_selected;
    if (task_selected >= task_top + rows) task_top = task_selected - rows + 1;
    draw_task_list();
  } else if (ks == XK_Down && n) {
    task_selected = (task_selected + 1) % n;
    if (task_selected < task_top) task_top = task_selected;
    if (task_selected >= task_top + rows) task_top = task_selected - rows + 1;
    draw_task_list();
  }
}

static unsigned int numlock_mask (void) {
  XModifierKeymap *map;
  KeyCode numlock;
  unsigned int mask = 0;
  int mod, key;

  numlock = XKeysymToKeycode(dpy, XK_Num_Lock);
  map = XGetModifierMapping(dpy);
  if (!map) return 0;

  for (mod = 0; mod < 8; ++mod) {
    for (key = 0; key < map->max_keypermod; ++key) {
      if (map->modifiermap[mod * map->max_keypermod + key] == numlock) {
        mask = (unsigned int)(1U << mod);
        break;
      }
    }
    if (mask) break;
  }
  XFreeModifiermap(map);
  return mask;
}

static void grab_task_switch_keys (void) {
  KeyCode tab = XKeysymToKeycode(dpy, XK_Tab);
  KeyCode esc = XKeysymToKeycode(dpy, XK_Escape);
  unsigned int nl = numlock_mask();
  unsigned int extras[4];
  int i;

  extras[0] = 0;
  extras[1] = LockMask;
  extras[2] = nl;
  extras[3] = LockMask | nl;

  for (i = 0; i < 4; ++i) {
    XGrabKey(dpy, (int)tab, Mod1Mask | extras[i], root, True,
             GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, (int)tab, Mod1Mask | ShiftMask | extras[i], root, True,
             GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, (int)esc, Mod1Mask | extras[i], root, True,
             GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, (int)esc, ControlMask | extras[i], root, True,
             GrabModeAsync, GrabModeAsync);
  }
}

/* ================================================================ */
/* Main event loop */
/* ================================================================ */

static void event_loop (void) {
  XEvent ev;
  for (;;) {
    XNextEvent(dpy, &ev);
    switch (ev.type) {
    case KeyPress: {
      KeySym ks = XLookupKeysym(&ev.xkey, 0);
      if (task_open && ev.xkey.window == task_win) {
        task_key_press(&ev.xkey);
      } else if (ks == XK_Tab && (ev.xkey.state & Mod1Mask)) {
        handle_alt_tab((ev.xkey.state & ShiftMask) != 0);
      } else if (ks == XK_Escape && (ev.xkey.state & Mod1Mask)) {
        if (alt_tab_active) end_alt_tab();
        handle_alt_esc();
      } else if (ks == XK_Escape && (ev.xkey.state & ControlMask)) {
        if (alt_tab_active) end_alt_tab();
        show_task_list();
      }
      break;
    }
    case KeyRelease: {
      KeySym ks = XLookupKeysym(&ev.xkey, 0);
      if (ks == XK_Alt_L || ks == XK_Alt_R || ks == XK_Meta_L || ks == XK_Meta_R)
        end_alt_tab();
      break;
    }
    case MapRequest: {
      Win3Window *w = find_client(ev.xmaprequest.window);
      if (!w) w = manage(ev.xmaprequest.window);
      if (w) {
        if (w->state == STATE_MINIMIZED) {
          restore_from_icon(w);
        } else {
          XMapWindow(dpy, w->titlebar);
          XMapWindow(dpy, w->client);
          XMapWindow(dpy, w->frame);
          set_wm_state(w, NormalState);
          focus_window(w);
        }
      }
      break;
    }
    case ConfigureRequest:
      handle_configure_request(&ev.xconfigurerequest);
      break;
    case Expose: {
      Win3Window *w;
      if (ev.xexpose.window == task_win) {
        if (ev.xexpose.count == 0) draw_task_list();
        break;
      }
      w = find_any(ev.xexpose.window);
      if (w && ev.xexpose.count == 0) {
        if (ev.xexpose.window == w->frame) draw_frame(w);
        else if (ev.xexpose.window == w->titlebar) draw_titlebar(w);
        else if (ev.xexpose.window == w->iconwin) draw_icon(w);
      }
      break;
    }
    case ButtonPress: {
      Win3Window *w;
      if (ev.xbutton.window == task_win) {
        if (ev.xbutton.button == Button1) task_button_press(&ev.xbutton);
        break;
      }
      w = find_any(ev.xbutton.window);
      if (!w) break;
      if (ev.xbutton.window == w->iconwin && ev.xbutton.button == Button1) {
        if (last_icon_click_window == w->client &&
            ev.xbutton.time - last_icon_click_time <= 350) {
          last_icon_click_window = None;
          last_icon_click_time = 0;
          restore_from_icon(w);
        } else {
          last_icon_click_window = w->client;
          last_icon_click_time = ev.xbutton.time;
          XRaiseWindow(dpy, w->iconwin);
        }
      } else if (ev.xbutton.window == w->titlebar)
        handle_title_press(w, &ev.xbutton);
      else if (ev.xbutton.window == w->frame) {
        if (resize_edges_at(w, ev.xbutton.x, ev.xbutton.y))
          start_resize(w, &ev.xbutton);
        else
          focus_window(w);
      } else if (ev.xbutton.window == w->client)
        focus_window(w);
      break;
    }
    case MotionNotify:
      if (resizing && resize_win) {
        XEvent last = ev;
        while (XCheckTypedEvent(dpy, MotionNotify, &ev)) last = ev;
        update_resize(last.xmotion.x_root, last.xmotion.y_root);
      } else if (dragging && drag_win) {
        XEvent last = ev;
        while (XCheckTypedEvent(dpy, MotionNotify, &ev)) last = ev;
        XMoveWindow(dpy, drag_win->frame,
                    drag_frame_x + last.xmotion.x_root - drag_start_x_root,
                    drag_frame_y + last.xmotion.y_root - drag_start_y_root);
        drag_win->x = drag_frame_x + last.xmotion.x_root - drag_start_x_root + FRAME_EDGE;
        drag_win->y = drag_frame_y + last.xmotion.y_root - drag_start_y_root + FRAME_EDGE + TITLE_H;
      }
      break;
    case ButtonRelease:
      if (resizing) {
        finish_resize();
      } else if (dragging) {
        XUngrabPointer(dpy, CurrentTime);
        if (drag_win) {
          drag_win->normal_x = drag_win->x;
          drag_win->normal_y = drag_win->y;
          send_configure(drag_win);
        }
        dragging = 0;
        drag_win = NULL;
      }
      break;
    case PropertyNotify: {
      Win3Window *w = find_client(ev.xproperty.window);
      if (w && (ev.xproperty.atom == XA_WM_NAME ||
                ev.xproperty.atom == XInternAtom(dpy, "_NET_WM_NAME", False))) {
        char *new_title = get_title(w->client);
        free(w->title);
        w->title = new_title;
        draw_titlebar(w);
        if (w->state == STATE_MINIMIZED) draw_icon(w);
        if (task_open) draw_task_list();
      }
      break;
    }
    case FocusIn: {
      Win3Window *w = find_client(ev.xfocus.window);
      Window actual_focus;
      int revert_to;

      /* FocusIn events can be stale after a fast ALT+TAB sequence */
      if (!w || w->state == STATE_MINIMIZED) break;
      XGetInputFocus(dpy, &actual_focus, &revert_to);
      if (actual_focus != w->client) break;

      if (w != active_win) {
        Win3Window *old = active_win;
        active_win = w;
        w->focus_seq = ++next_focus_seq;
        if (old && old != w) draw_titlebar(old);
        draw_titlebar(w);
      }
      break;
    }
    case UnmapNotify: {
      Win3Window *w = find_client(ev.xunmap.window);
      if (w) {
        if (w->ignore_unmap > 0) --w->ignore_unmap;
        else if (w->state != STATE_MINIMIZED) unmanage(w, 0);
      }
      break;
    }
    case DestroyNotify: {
      Win3Window *w = find_client(ev.xdestroywindow.window);
      if (w) unmanage(w, 1);
      break;
    }
    case ConfigureNotify:
      if (ev.xconfigure.window == root) {
        arrange_icons();
        if (task_open && task_win)
          XMoveWindow(dpy, task_win, (ev.xconfigure.width - TASK_W) / 2,
                      (ev.xconfigure.height - TASK_H) / 2);
      }
      break;
    default:
      break;
    }
  }
}

/* ================================================================ */
/* Main launcher */
/* ================================================================ */

int main (int argc, char **argv) {
  XSetWindowAttributes ra;
  Window dummy1, dummy2, *children = NULL;
  unsigned int nchildren = 0, i;

  dpy = XOpenDisplay(NULL);
  if (!dpy) return EXIT_FAILURE;
  screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);

  if (argc > 2 || (argc == 2 && !configure_monitors(argv[1]))) {
    XCloseDisplay(dpy);
    return EXIT_FAILURE;
  }

  wm_delete_window_atom = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  wm_protocols_atom = XInternAtom(dpy, "WM_PROTOCOLS", False);
  wm_state_atom = XInternAtom(dpy, "WM_STATE", False);

  font_info = XLoadQueryFont(dpy, "-misc-fixed-bold-r-normal--13-*-*-*-*-*-iso8859-1");
  if (!font_info) font_info = XLoadQueryFont(dpy, "6x13bold");
  if (!font_info) font_info = XLoadQueryFont(dpy, "fixed");

  /* WM ownership before installing the runtime handler */
  XSetErrorHandler(xerror);
  memset(&ra, 0, sizeof(ra));
  ra.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
    StructureNotifyMask | ButtonPressMask;
  XChangeWindowAttributes(dpy, root, CWEventMask, &ra);
  XSync(dpy, False);

  /* Task switching */
  grab_task_switch_keys();

  /* Desktop color (gray) */
  XSetWindowBackground(dpy, root, C_DESKTOP);
  XClearWindow(dpy, root);

  /* Adopt only viewable clients at startup */
  if (XQueryTree(dpy, root, &dummy1, &dummy2, &children, &nchildren)) {
    for (i = 0; i < nchildren; ++i) {
      XWindowAttributes a;
      if (XGetWindowAttributes(dpy, children[i], &a) &&
          !a.override_redirect && a.class != InputOnly &&
          a.map_state == IsViewable)
        manage(children[i]);
    }
    if (children) XFree(children);
  }

  event_loop();
  return EXIT_SUCCESS;
}
