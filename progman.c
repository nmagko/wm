/*
 * PROGMAN is the Program Manager clone for Linux/X11
 * Copyright (C) 2026  Victor C. Salas P. (aka nmag) <nmagko@gmail.com>
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
 * Compile: gcc -O2 -o progman progman.c -lX11
 *
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <png.h>
#include <signal.h>
#include "wmver.h"

/* ================================================================ */
/* Constants and Macros */
/* ================================================================ */
#define MAX_GROUPS 16
#define MAX_APPS 128
#define MAX_PATH 1024
#define MAX_NAME 256
#define LINE_LEN 14

/* Default window size */
#define DEF_WIN_W 900
#define DEF_WIN_H 620

#define MENUBAR_H 24
#define TITLEBAR_H 21 // 19
#define ICON_SIZE 32
#define ROW_H 16
#define CHAR_W 7 // 8
#define GROUP_ICON_W 40
#define GROUP_ICON_H 56
#define SCROLLBAR_W 16

/* MDI States */
#define STATE_NORMAL 0
#define STATE_MAXIMIZED 1
#define STATE_MINIMIZED 2

#define ALLOC(name, r, g, b) c.red=r; c.green=g; c.blue=b; XAllocColor(dpy, cmap, &c); name = c.pixel;

#define TARGET (progman_draw_target ? progman_draw_target : win)

/* ================================================================ */
/* Data Structures */
/* ================================================================ */
typedef struct {
  char name[MAX_NAME];
  char exec[MAX_PATH];
  char icon[MAX_NAME]; // Icon= value from the .desktop file
  Pixmap icon_pixmap; // Loaded icon, or None
  int    icon_w; // Loaded icon dimensions (may be 0)
  int    icon_h;
} App;

typedef struct {
  char name[64];
  int x, y, w, h;
  int normal_x, normal_y, normal_w, normal_h;
  int state;
  int scroll_offset; // current scroll position
  int max_scroll; // maximum scroll offset
  App apps[MAX_APPS];
  int n_apps;
  int selected_app;
} Group;

/* ================================================================ */
/* Globals */
/* ================================================================ */
static Display *dpy;
static int scr;
static Window root;
static Window win;
static GC gc;
static XFontStruct *font;
static Colormap cmap;

static unsigned long col_yellow, col_face, col_btn, col_white, col_black,
  col_gray, col_cyan, col_blue, col_active_title, col_inactive_title;

static Group groups[MAX_GROUPS];
static int n_groups = 0;
static int active_group = -1;

static int menu_open = 0;
static int menu_item = -1;

/* The actual window size is in g_win_w / g_win_h */
static int win_w = DEF_WIN_W;
static int win_h = DEF_WIN_H;

static Pixmap backbuf = None;
static int backbuf_w = 0;
static int backbuf_h = 0;

Window progman_draw_target = 0;

static int dlg_type = 0;   /* 0: none, 1: about, 2: exit-confirm */
static Atom wm_delete_window = None;

static volatile sig_atomic_t got_terminate_signal = 0;

/* ================================================================ */
/* System events */
/* ================================================================ */
static void on_terminate_signal(int sig) {
  (void)sig;
  got_terminate_signal = 1;
}

/* shell behavior, wait till window manager is ready */
static int wait_for_window_manager(int max_milliseconds) {
  Atom check_atom = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
  if (check_atom == None) return 0; // for older servers does nothing
  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after;
  unsigned char *prop = NULL;
  int waited = 0;
  while (waited < max_milliseconds) {
    /* Check if the property exists */
    if (XGetWindowProperty(dpy, root, check_atom, 0, 1, False, XA_WINDOW,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &prop) == Success && prop != NULL) {
      XFree(prop);
      prop = NULL;
      if (actual_type == XA_WINDOW && nitems == 1) {
        /* Verify the window references itself */
        Window wm_check_win = 0;
        unsigned char *prop2 = NULL;
        if (XGetWindowProperty(dpy, root, check_atom, 0, 1, False, XA_WINDOW,
                               &actual_type, &actual_format, &nitems,
                               &bytes_after, &prop2) == Success && prop2) {
          wm_check_win = *(Window *)prop2;
          XFree(prop2);
        }
        if (wm_check_win) {
          unsigned char *prop3 = NULL;
          if (XGetWindowProperty(dpy, wm_check_win, check_atom, 0, 1, False,
                                 XA_WINDOW, &actual_type, &actual_format,
                                 &nitems, &bytes_after, &prop3) == Success
              && prop3) {
            Window self_ref = *(Window *)prop3;
            XFree(prop3);
            if (self_ref == wm_check_win) return 1;
          }
        }
      }
    }
    /* Flush any pending events */
    XSync(dpy, False);
    usleep(50 * 1000);   /* 50 ms */
    waited += 50;
  }
  return 0;
}

/* ================================================================ */
/* Utility */
/* ================================================================ */
static void draw_text(int x, int y, const char *s) {
  XSetFont(dpy, gc, font->fid);
  XDrawString(dpy, TARGET, gc, x, y, s, strlen(s));
}

static void fill_rect(int x, int y, int width, int height, unsigned long c) {
  XSetForeground(dpy, gc, c);
  XFillRectangle(dpy, TARGET, gc, x, y, width, height);
}

static void draw_rect(int x, int y, int width, int height, unsigned long c) {
  XSetForeground(dpy, gc, c);
  XDrawRectangle(dpy, TARGET, gc, x, y, width - 1, height - 1);
}

static void draw_bevel(int x, int y, int width, int height, int raised) {
  unsigned long tl = raised ? col_white : col_gray;
  unsigned long br = raised ? col_gray : col_white;
  XSetForeground(dpy, gc, tl);
  XDrawLine(dpy, TARGET, gc, x, y, x + width - 1, y);
  XDrawLine(dpy, TARGET, gc, x, y, x, y + height - 1);
  XSetForeground(dpy, gc, br);
  XDrawLine(dpy, TARGET, gc, x, y + height - 1, x + width - 1, y + height - 1);
  XDrawLine(dpy, TARGET, gc, x + width - 1, y, x + width - 1, y + height - 1);
}

/* Truncate string to fit in max pixel width, adding ellipsis if needed */
static void fit_text(char *out, const char *in, int max_pixels) {
  int len = strlen(in);
  int w = XTextWidth(font, in, len);
  if (w <= max_pixels) {
    strcpy(out, in);
    return;
  }
  /* Trim one char at a time until the text with "..." fits. */
  while (len > 3) {
    len--;
    char tmp[256];
    strncpy(tmp, in, len);
    tmp[len] = 0;
    strcat(tmp, "...");
    if (XTextWidth(font, tmp, strlen(tmp)) <= max_pixels) {
      strcpy(out, tmp);
      return;
    }
  }
  strcpy(out, "...");
}

/* ================================================================ */
/* Scanning /usr/share/applications */
/* ================================================================ */
static void scan_applications(void) {
  static const char *app_dirs[] = {
    "/usr/share/applications",
    "/usr/local/share/applications",
    NULL
  };

  for (int dir_i = 0; app_dirs[dir_i]; dir_i++) {
    DIR *d = opendir(app_dirs[dir_i]);
    if (!d) continue;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (strstr(de->d_name, ".desktop") == NULL) continue;

      char path[MAX_PATH];
      snprintf(path, sizeof(path), "%s/%s", app_dirs[dir_i], de->d_name);

      FILE *fp = fopen(path, "r");
      if (!fp) continue;

      char line[1024];
      char name[MAX_NAME] = "";
      char exec[MAX_PATH] = "";
      char icon[MAX_NAME] = "";
      char categories[256] = "";
      int in_entry = 0;

      while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '[') {
          in_entry = (strcmp(line, "[Desktop Entry]") == 0);
          continue;
        }
        if (!in_entry) continue;

        if (strncmp(line, "Name=", 5) == 0) {
          strncpy(name, line + 5, MAX_NAME - 1);
        } else if (strncmp(line, "Exec=", 5) == 0) {
          strncpy(exec, line + 5, MAX_PATH - 1);
        } else if (strncmp(line, "Icon=", 5) == 0) {
          strncpy(icon, line + 5, MAX_NAME - 1);
        } else if (strncmp(line, "Categories=", 11) == 0) {
          strncpy(categories, line + 11, sizeof(categories) - 1);
        }
      }
      fclose(fp);

      if (name[0] == 0 || exec[0] == 0) continue;

      char group_name[64] = "Other";
      if (strstr(categories, "Utility")) strcpy(group_name, "Accessories");
      else if (strstr(categories, "Development")) strcpy(group_name, "Development");
      else if (strstr(categories, "Game")) strcpy(group_name, "Games");
      else if (strstr(categories, "Graphics")) strcpy(group_name, "Graphics");
      else if (strstr(categories, "Network")) strcpy(group_name, "Network");
      else if (strstr(categories, "Office")) strcpy(group_name, "Office");
      else if (strstr(categories, "System")) strcpy(group_name, "System");
      else if (strstr(categories, "AudioVideo")) strcpy(group_name, "Multimedia");

      int g_idx = -1;
      for (int i = 0; i < n_groups; i++) {
        if (strcmp(groups[i].name, group_name) == 0) { g_idx = i; break; }
      }
      if (g_idx == -1 && n_groups < MAX_GROUPS) {
        g_idx = n_groups++;
        strncpy(groups[g_idx].name, group_name, 63);
        groups[g_idx].n_apps = 0;
        groups[g_idx].state = STATE_MINIMIZED;
        groups[g_idx].selected_app = -1;
        groups[g_idx].scroll_offset = 0;
        groups[g_idx].max_scroll = 0;
      }

      if (g_idx >= 0 && groups[g_idx].n_apps < MAX_APPS) {
        App *a = &groups[g_idx].apps[groups[g_idx].n_apps++];
        strncpy(a->name, name, MAX_NAME - 1);
        strncpy(a->exec, exec, MAX_PATH - 1);
        strncpy(a->icon, icon, MAX_NAME - 1);
        a->icon_pixmap = None;
        a->icon_w = 0;
        a->icon_h = 0;
      }
    }
    closedir(d);
  }
}

/* ================================================================ */
/* Drawing Functions */
/* ================================================================ */
/* Try to find a PNG file for an icon name */
static int find_icon_png(const char *name, char *out, int outsz) {
  /* Absolute path supplied by the .desktop file */
  if (name[0] == '/') {
    FILE *fp = fopen(name, "r");
    if (fp) { fclose(fp); strncpy(out, name, outsz - 1); out[outsz - 1] = 0; return 1; }
    return 0;
  }

  static const char *sizes[] = { "48x48", "32x32", "64x64", "24x24", "256x256", NULL };

  /* User theme first, then system themes, then pixmaps fallback */
  const char *bases[] = {
    "/usr/share/icons/hicolor",
    "/usr/local/share/icons/hicolor",
    "/usr/share/icons/gnome",
    "/usr/share/icons/Adwaita",
    "/usr/share/pixmaps",
    NULL
  };

  for (int b = 0; bases[b]; b++) {
    /* pixmaps directory has no size subdirectory */
    if (strstr(bases[b], "pixmaps")) {
      char p[MAX_PATH];
      snprintf(p, sizeof(p), "%s/%s.png", bases[b], name);
      FILE *fp = fopen(p, "r");
      if (fp) { fclose(fp); strncpy(out, p, outsz - 1); out[outsz - 1] = 0; return 1; }
      continue;
    }
    for (int s = 0; sizes[s]; s++) {
      char p[MAX_PATH];
      snprintf(p, sizeof(p), "%s/%s/apps/%s.png", bases[b], sizes[s], name);
      FILE *fp = fopen(p, "r");
      if (fp) { fclose(fp); strncpy(out, p, outsz - 1); out[outsz - 1] = 0; return 1; }
      /* Also try the top-level of the theme */
      snprintf(p, sizeof(p), "%s/%s/%s.png", bases[b], sizes[s], name);
      fp = fopen(p, "r");
      if (fp) { fclose(fp); strncpy(out, p, outsz - 1); out[outsz - 1] = 0; return 1; }
    }
  }
  return 0;
}

/* Load a PNG into a Pixmap of the given size */
static Pixmap load_png_pixmap(const char *path, int *out_w, int *out_h) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return None;

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                           NULL, NULL, NULL);
  if (!png) { fclose(fp); return None; }
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return None; }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return None;
  }
  png_init_io(png, fp);
  png_read_info(png, info);

  png_uint_32 w = png_get_image_width(png, info);
  png_uint_32 h = png_get_image_height(png, info);
  png_byte color_type = png_get_color_type(png, info);
  png_byte bit_depth  = png_get_bit_depth(png, info);

  if (bit_depth == 16) png_set_strip_16(png);
  if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);

  png_read_update_info(png, info);

  png_bytep *rows = malloc(sizeof(png_bytep) * h);
  if (!rows) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return None; }
  size_t rowbytes = png_get_rowbytes(png, info);
  png_bytep data = malloc(rowbytes * h);
  if (!data) { free(rows); png_destroy_read_struct(&png, &info, NULL); fclose(fp); return None; }
  for (png_uint_32 y = 0; y < h; y++) rows[y] = data + y * rowbytes;

  png_read_image(png, rows);
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);
  fclose(fp);

  int outw = (int)w;
  int outh = (int)h;

  int depth = DefaultDepth(dpy, scr);
  Visual *visual = DefaultVisual(dpy, scr);

  /* Allocate the pixel buffer and create the XImage BEFORE filling it */
  char *ximg_data = calloc((size_t)outw * outh * 4, 1);
  if (!ximg_data) { free(data); free(rows); return None; }

  XImage *ximg = XCreateImage(dpy, visual, depth, ZPixmap, 0,
                              ximg_data, outw, outh, 32, 0);
  if (!ximg) { free(ximg_data); free(data); free(rows); return None; }

  /* Composite each pixel against the white icon background */
  const unsigned char bg_r = 0xFF, bg_g = 0xFF, bg_b = 0xFF;

  for (int y = 0; y < outh; y++) {
    png_bytep src = rows[y];
    for (int x = 0; x < outw; x++) {
      unsigned char r = src[x*4+0];
      unsigned char g = src[x*4+1];
      unsigned char b = src[x*4+2];
      unsigned char a = src[x*4+3];

      unsigned char cr = (r * a + bg_r * (255 - a)) / 255;
      unsigned char cg = (g * a + bg_g * (255 - a)) / 255;
      unsigned char cb = (b * a + bg_b * (255 - a)) / 255;

      /* Pack the channel values into their proper positions */
      unsigned long pixel = 0;
      pixel |= (unsigned long)cr << 16;
      pixel |= (unsigned long)cg <<  8;
      pixel |= (unsigned long)cb;

      /* Adjust the visual's masks */
      {
        unsigned long rm = visual->red_mask;
        unsigned long gm = visual->green_mask;
        unsigned long bm = visual->blue_mask;
        /* 8-bit-per-channel */
        (void)rm; (void)gm; (void)bm;
      }

      if (ximg->byte_order == LSBFirst) {
        ximg_data[(y * outw + x) * 4 + 0] = (pixel      ) & 0xFF;
        ximg_data[(y * outw + x) * 4 + 1] = (pixel >>  8) & 0xFF;
        ximg_data[(y * outw + x) * 4 + 2] = (pixel >> 16) & 0xFF;
        ximg_data[(y * outw + x) * 4 + 3] = 0;
      } else {
        ximg_data[(y * outw + x) * 4 + 0] = (pixel >> 24) & 0xFF;
        ximg_data[(y * outw + x) * 4 + 1] = (pixel >> 16) & 0xFF;
        ximg_data[(y * outw + x) * 4 + 2] = (pixel >>  8) & 0xFF;
        ximg_data[(y * outw + x) * 4 + 3] = (pixel      ) & 0xFF;
      }
    }
  }

  Pixmap pix = XCreatePixmap(dpy, win, outw, outh, depth);
  GC gc2 = XCreateGC(dpy, pix, 0, NULL);
  XPutImage(dpy, pix, gc2, ximg, 0, 0, 0, 0, outw, outh);
  XFreeGC(dpy, gc2);

  XDestroyImage(ximg); // frees ximg_data
  free(data);
  free(rows);

  *out_w = outw;
  *out_h = outh;
  return pix;
}

/* Try to load the icon for each app */
static void load_all_icons(void) {
  for (int gi = 0; gi < n_groups; gi++) {
    Group *g = &groups[gi];
    for (int ai = 0; ai < g->n_apps; ai++) {
      App *a = &g->apps[ai];
      if (a->icon[0] == 0) continue;
      char path[MAX_PATH];
      if (!find_icon_png(a->icon, path, sizeof(path))) continue;
      a->icon_pixmap = load_png_pixmap(path, &a->icon_w, &a->icon_h);
    }
  }
}

/* Windows 3.0 group icon */
static void draw_group_icon(Group *g, int x, int y) {
  int icon_w = GROUP_ICON_W;
  int icon_h = 34;

  /* White background of the group icon */
  fill_rect(x + 4, y + 3, icon_w - 4, icon_h - 4, col_gray);
  fill_rect(x + 2, y + 1, icon_w - 4, icon_h - 4, col_white);
  draw_bevel(x + 2, y + 1, icon_w - 4, icon_h - 4, 1);
  draw_rect(x + 2, y + 1, icon_w - 4, icon_h - 4, col_black);

  /* Tiny title bar at the top */
  fill_rect(x + 2, y + 1, icon_w - 4, 5, col_cyan);
  draw_rect(x + 2, y + 1, icon_w - 4, 5, col_black);

  /* 6 vertical blank sheets (2 rows of 3) */
  int sheet_w = 7;
  int sheet_h = 8;
  int sheet_gap_x = 3;
  int sheet_gap_y = 3;
  int start_x = x + (icon_w - (3 * sheet_w + 2 * sheet_gap_x)) / 2;
  int start_y = y + 2 + 4 + 2;

  for (int row = 0; row < 2; row++) {
    for (int col = 0; col < 3; col++) {
      int sx = start_x + col * (sheet_w + sheet_gap_x);
      int sy = start_y + row * (sheet_h + sheet_gap_y);
      /* Vertical white blank sheet */
      fill_rect(sx, sy, sheet_w, sheet_h, col_white);
      draw_rect(sx, sy, sheet_w, sheet_h, col_black);
      /* Vertical white blank notch */
      fill_rect(sx+sheet_w/2, sy, sheet_w/2, sheet_h/2, col_white);
      draw_rect(sx+sheet_w/2, sy, sheet_w/2, sheet_h/2, col_black);
      XSetForeground(dpy, gc, col_white);
      XPoint notch[] = {{sx+sheet_w/2, sy}, {sx+sheet_w, sy}, {sx+sheet_w, sy+sheet_h/2}};
      XFillPolygon(dpy, TARGET, gc, notch, 3, Convex, CoordModeOrigin);
      XSetForeground(dpy, gc, col_black);
      XDrawLine(dpy, TARGET, gc, sx+sheet_w/2, sy, sx+sheet_w-1, sy+sheet_h/2-1);
    }
  }

  /* Up to 2 lines of the icon group name */
  char label_buf[128];
  char *src = g->name;
  if (strlen(src) > 30) {
    strncpy(label_buf, src, 30);
    label_buf[30] = 0;
  } else {
    strcpy(label_buf, src);
  }

  char lines[2][40];
  int line_count = 0;
  int char_idx = 0;
  int src_len = strlen(label_buf);

  while (char_idx < src_len && line_count < 2) {
    int line_start = char_idx;
    int line_len = 0;
    int last_space = -1;
    while (char_idx < src_len && line_len < 13) {
      if (label_buf[char_idx] == ' ') last_space = char_idx - line_start;
      line_len++;
      char_idx++;
    }
    if (char_idx < src_len && last_space >= 0) {
      char_idx = line_start + last_space;
      line_len = last_space;
    }
    strncpy(lines[line_count], label_buf + line_start, line_len);
    lines[line_count][line_len] = 0;
    while (char_idx < src_len && label_buf[char_idx] == ' ') char_idx++;
    line_count++;
  }

  XSetForeground(dpy, gc, col_black);
  int line_y = y + icon_h + 16; // 12
  int icon_cx = x + GROUP_ICON_W / 2;
  for (int li = 0; li < line_count; li++) {
    int text_w = XTextWidth(font, lines[li], strlen(lines[li]));
    int text_x = icon_cx - text_w / 2 + 1;
    draw_text(text_x, line_y, lines[li]);
    line_y += 14;
  }
}

/* Windows 3.0 app icon */
static void draw_app_icon(App *a, int ix, int iy) {
  if (a->icon_pixmap != None && a->icon_w > 0 && a->icon_h > 0) {
    /* Scale down (simple nearest-neighbor) */
    if (a->icon_w == ICON_SIZE && a->icon_h == ICON_SIZE) {
      XCopyArea(dpy, a->icon_pixmap, TARGET, gc, 0, 0,
                ICON_SIZE, ICON_SIZE, ix, iy);
    } else {
      /* Nearest-neighbor downscale into the icon cell */
      for (int y = 0; y < ICON_SIZE; y++) {
        int sy = y * a->icon_h / ICON_SIZE;
        for (int x = 0; x < ICON_SIZE; x++) {
          int sx = x * a->icon_w / ICON_SIZE;
          XCopyArea(dpy, a->icon_pixmap, TARGET, gc,
                    sx, sy, 1, 1, ix + x, iy + y);
        }
      }
    }
    return;
  }
  /* Fall back to the default app icon */
  int lx = ix - 4;
  int iw = ICON_SIZE + 8;
  /* wide border */
  fill_rect(lx+1, iy+1, iw-2, ICON_SIZE-2, col_btn);
  draw_bevel(lx+1, iy+1, iw-2, ICON_SIZE-2, 1);
  draw_rect(lx, iy, iw, ICON_SIZE, col_black);
  fill_rect(lx + 4, iy + 4, iw - 8, ICON_SIZE - 8, col_white);
  draw_bevel(lx + 4, iy + 4, iw - 8, ICON_SIZE - 8, 0);
  /* window title */
  draw_rect(lx + 5, iy + 5, iw - 10, ICON_SIZE - 10, col_black);
  fill_rect(lx + 5, iy + 5, iw - 10, 5, col_cyan);
  draw_rect(lx + 5, iy + 5, iw - 10, 5, col_black);
  /* window content */
  fill_rect(lx + 8, iy + 13, iw - 16, 1, col_blue);
  fill_rect(lx + 8, iy + 16, iw - 17, 1, col_blue);
  fill_rect(lx + 8, iy + 19, iw - 16, 1, col_blue);
  fill_rect(lx + 8, iy + 22, iw - 17, 1, col_blue);
}

/* Windows 3.0 about window icon */
static void draw_app_window(int ix, int iy) {
  /* shadow */
  fill_rect(2+ix+2, 2+iy, ICON_SIZE - 4, 1, col_gray);
  fill_rect(2+ix+3, 2+iy + 1, ICON_SIZE - 6, 1, col_gray);
  fill_rect(2+ix+2, 2+iy + ICON_SIZE, ICON_SIZE - 4, 1, col_gray);
  fill_rect(2+ix+3, 2+iy + ICON_SIZE-1, ICON_SIZE - 6, 1, col_gray);
  fill_rect(2+ix+4, 2+iy, ICON_SIZE - 8, ICON_SIZE, col_gray);
  /* base */
  fill_rect(ix+2, iy, ICON_SIZE - 4, 1, col_black);
  fill_rect(ix+3, iy + 1, ICON_SIZE - 6, 1, col_black);
  fill_rect(ix+2, iy + ICON_SIZE, ICON_SIZE - 4, 1, col_black);
  fill_rect(ix+3, iy + ICON_SIZE-1, ICON_SIZE - 6, 1, col_black);
  fill_rect(ix+4, iy, ICON_SIZE - 8, ICON_SIZE, col_black);
  /* upper glass */
  fill_rect(ix+6, iy + 2, ICON_SIZE - 12, ICON_SIZE/2 - 2, col_btn);
  fill_rect(ix+8, iy + 4, ICON_SIZE - 16, ICON_SIZE/2 - 6, col_white);
  draw_rect(ix+7, iy + 3, ICON_SIZE - 14, ICON_SIZE/2 - 4, col_gray);
  /* lower glass */
  fill_rect(ix+6, iy + ICON_SIZE/2 + 1, ICON_SIZE - 12, ICON_SIZE/2 - 2, col_btn);
  fill_rect(ix+8, iy + ICON_SIZE/2 + 4, ICON_SIZE - 16, ICON_SIZE/2 - 6, col_white);
  draw_rect(ix+7, iy + ICON_SIZE/2 + 2, ICON_SIZE - 14, ICON_SIZE/2 - 4, col_gray);
}

/* Windows 3.0 drawing content */
static void draw_group_content(Group *g) {
  int w = g->w, h = g->h;
  int base_y = g->y + TITLEBAR_H;
  int content_h = h - TITLEBAR_H - 2;
  int content_w = w - 4;
  int has_scroll = (g->max_scroll > 0);
  int sb_w = has_scroll ? SCROLLBAR_W : 0;

  if (has_scroll) content_w -= sb_w;

  fill_rect(g->x + 2 + 1, base_y + 1, w - 4 - 2, content_h - 2, col_white);

  /* Each column slot is 104 px wide */
  int slot_w = 104;
  int left_margin = 16;
  int icons_per_row = (content_w - left_margin * 2) / slot_w;
  if (icons_per_row < 1) icons_per_row = 1;

  /* Center the block of columns horizontally in the content area */
  int used_w = icons_per_row * slot_w;
  int block_x = g->x + 2 + (content_w - used_w) / 2;

  /* 32 (icon) + 3 * 14 (labels) + 12 (gap) = 86 */
  int slot_h = 86;

  XRectangle clip;
  clip.x = g->x + 2;
  clip.y = base_y;
  clip.width = content_w;
  clip.height = content_h;
  XSetClipRectangles(dpy, gc, 0, 0, &clip, 1, Unsorted);

  for (int i = 0; i < g->n_apps; i++) {
    int col = i % icons_per_row;
    int row = i / icons_per_row;
    int slot_x = block_x + col * slot_w;
    int ix = slot_x + (slot_w - ICON_SIZE) / 2;
    int iy = base_y + 10 + row * slot_h - g->scroll_offset;

    if (iy + ICON_SIZE + 3 * 14 < base_y) continue;
    if (iy > base_y + content_h) continue;

    draw_app_icon(&g->apps[i], ix, iy);

    /* Label 40 chars max, wrapped at LINE_LEN chars per line, up to 3 lines */
    char label_buf[64];
    char *src = g->apps[i].name;
    int total_len = strlen(src);

    if (total_len > 40) {
      strncpy(label_buf, src, 40);
      label_buf[40] = 0;
    } else {
      strcpy(label_buf, src);
    }

    char lines[3][64];
    int line_count = 0;
    int char_idx = 0;
    int src_len = strlen(label_buf);

    while (char_idx < src_len && line_count < 3) {
      int line_start = char_idx;
      int line_len = 0;
      int last_space = -1;
      while (char_idx < src_len && line_len < LINE_LEN) {
        if (label_buf[char_idx] == ' ') last_space = char_idx - line_start;
        line_len++;
        char_idx++;
      }
      if (char_idx < src_len && last_space >= 0) {
        char_idx = line_start + last_space;
        line_len = last_space;
      }
      strncpy(lines[line_count], label_buf + line_start, line_len);
      lines[line_count][line_len] = 0;
      while (char_idx < src_len && label_buf[char_idx] == ' ') char_idx++;
      line_count++;
    }

    /* Ellipsize if label > 40 and last line is full */
    if (total_len > 40 && line_count > 0) {
      int last = line_count - 1;
      int llen = strlen(lines[last]);
      if (llen > 2) {
        lines[last][llen - 1] = 0;
        strcat(lines[last], ".");
        strcat(lines[last], ".");
      }
    }

    int line_y = iy + ICON_SIZE + 16; // 12
    if (i == g->selected_app) {
      fill_rect(slot_x + 1, line_y - 14, slot_w - 2, line_count * 14 + 5, col_active_title);
      XSetForeground(dpy, gc, col_white);
    } else {
      XSetForeground(dpy, gc, col_black);
    }
    for (int li = 0; li < line_count; li++) {
      int text_w = XTextWidth(font, lines[li], strlen(lines[li]));
      int icon_cx = ix + ICON_SIZE / 2;
      int text_x = icon_cx - text_w / 2;
      draw_text(text_x, line_y, lines[li]);
      line_y += 14;
    }
  }

  XSetClipMask(dpy, gc, None);

  /* Scrollbar */
  if (has_scroll) {
    int sb_x = g->x + w - 2 - SCROLLBAR_W;
    int sb_y = base_y;
    int sb_h = content_h;

    fill_rect(sb_x, sb_y, SCROLLBAR_W, sb_h, col_btn); // col_face
    draw_bevel(sb_x, sb_y, SCROLLBAR_W, sb_h, 0);

    int arrow_h = 16;
    fill_rect(sb_x + 1, sb_y + 1, SCROLLBAR_W - 2, arrow_h, col_btn); // col_face
    XSetForeground(dpy, gc, col_black);
    XPoint up_pts[] = {{sb_x + 4, sb_y + arrow_h - 4},
                       {sb_x + SCROLLBAR_W / 2, sb_y + 5},
                       {sb_x + SCROLLBAR_W - 4, sb_y + arrow_h - 4}};
    XFillPolygon(dpy, TARGET, gc, up_pts, 3, Convex, CoordModeOrigin);

    int dn_y = sb_y + sb_h - arrow_h;
    fill_rect(sb_x + 1, dn_y, SCROLLBAR_W - 2, arrow_h - 1, col_btn); // col_face
    XPoint dn_pts[] = {{sb_x + 5, dn_y + 4},
                       {sb_x + SCROLLBAR_W / 2, dn_y + arrow_h - 6},
                       {sb_x + SCROLLBAR_W - 4, dn_y + 4}};
    XSetForeground(dpy, gc, col_black);
    XFillPolygon(dpy, TARGET, gc, dn_pts, 3, Convex, CoordModeOrigin);

    int track_y = sb_y + arrow_h;
    int track_h = sb_h - 2 * arrow_h;
    int thumb_h = (content_h * track_h) / (g->max_scroll + content_h);
    if (thumb_h < 16) thumb_h = 16;
    if (thumb_h > track_h) thumb_h = track_h;
    int thumb_y = track_y;
    if (g->max_scroll > 0) {
      thumb_y += (g->scroll_offset * (track_h - thumb_h)) / g->max_scroll;
    }
    fill_rect(sb_x + 1, thumb_y, SCROLLBAR_W - 2, thumb_h, col_white);
    draw_bevel(sb_x + 1, thumb_y, SCROLLBAR_W - 2, thumb_h, 1);
  }
}

static void draw_group_title(Group *g) {
  int is_active = (active_group >= 0 && g == &groups[active_group]);
  int tx = g->x + 2 + 1;
  int ty = g->y + 2 + 1;
  int tw = g->w - 4 - 2;
  int th = TITLEBAR_H;

  fill_rect(tx, ty, tw, th, is_active ? col_active_title : col_inactive_title);

  /* Compute available width for title text */
  int left_w = 20; // control menu box
  int right_w = 2 * (15 + 2) + 4; // two buttons + gaps + margin
  int avail = tw - left_w - right_w - 8;
  if (avail < 20) avail = 20;

  char title_buf[128];
  fit_text(title_buf, g->name, avail);

  XSetForeground(dpy, gc, col_white);
  draw_text(tx + left_w + 2, ty + 15, title_buf);

  int bw = 16, bh = 16;

  /* Control menu box */
  fill_rect(tx + 2, ty + 2, bw, bh, col_btn);
  draw_bevel(tx + 2, ty + 2, bw, bh, 1);
  draw_rect(tx + 6, ty + 8, 7, 3, col_black);

  /* Maximize button */
  int bx = tx + tw - bw - 4;
  fill_rect(bx, ty + 2, bw, bh, col_btn);
  draw_bevel(bx, ty + 2, bw, bh, 1);
  XSetForeground(dpy, gc, col_black);
  if (g->state == STATE_MAXIMIZED) {
    XPoint uverts[] = {{bx + 3, ty + 9}, {bx + bw/2, ty + 4}, {bx + bw - 4, ty + 9}};
    XFillPolygon(dpy, TARGET, gc, uverts, 3, Convex, CoordModeOrigin);
    XPoint lverts[] = {{bx + 4, ty + 11}, {bx + bw/2, ty + 15}, {bx + bw - 4, ty + 11}};
    XFillPolygon(dpy, TARGET, gc, lverts, 3, Convex, CoordModeOrigin);
  } else {
    XPoint verts[] = {{bx + 3, ty + 11}, {bx + bw/2, ty + 6}, {bx + bw - 4, ty + 11}};
    XFillPolygon(dpy, TARGET, gc, verts, 3, Convex, CoordModeOrigin);
  }

  /* Minimize button */
  bx -= (bw + 2);
  fill_rect(bx, ty + 2, bw, bh, col_btn);
  draw_bevel(bx, ty + 2, bw, bh, 1);
  XSetForeground(dpy, gc, col_black);
  XPoint verts[] = {{bx + 4, ty + 8}, {bx + bw/2, ty + 12}, {bx + bw - 4, ty + 8}};
  XFillPolygon(dpy, TARGET, gc, verts, 3, Convex, CoordModeOrigin);
}

static void draw_group_frame(Group *g) {
  fill_rect(g->x, g->y, g->w, g->h, col_face);
  draw_bevel(g->x + 1, g->y + 1, g->w - 2, g->h - 2, 1);
  draw_rect(g->x, g->y, g->w, g->h, col_black);
}

static void recalc_scroll(Group *g) {
  int content_h = g->h - TITLEBAR_H - 2;
  int content_w = g->w - 4;
  int slot_w = 104;
  int left_margin = 16;
  int icons_per_row;

  icons_per_row = (content_w - left_margin * 2) / slot_w;
  if (icons_per_row < 1) icons_per_row = 1;
  int rows = (g->n_apps + icons_per_row - 1) / icons_per_row;
  int total_h = rows * 86 + 20;

  if (total_h > content_h) {
    content_w -= SCROLLBAR_W;
    icons_per_row = (content_w - left_margin * 2) / slot_w;
    if (icons_per_row < 1) icons_per_row = 1;
    rows = (g->n_apps + icons_per_row - 1) / icons_per_row;
    total_h = rows * 86 + 20;
    g->max_scroll = total_h - content_h;
    if (g->max_scroll < 0) g->max_scroll = 0;
  } else {
    g->max_scroll = 0;
  }

  if (g->scroll_offset > g->max_scroll) g->scroll_offset = g->max_scroll;
  if (g->scroll_offset < 0) g->scroll_offset = 0;
}

static void update_group_window(Group *g) {
  if (g->state == STATE_MINIMIZED) return;
  recalc_scroll(g);
  draw_group_frame(g);
  draw_group_title(g);
  draw_group_content(g);
}

/* ================================================================ */
/* Menu Drawing */
/* ================================================================ */
static void draw_menubar(void) {
  fill_rect(0, 0, win_w, MENUBAR_H, col_white); // col_face
  XSetForeground(dpy, gc, col_black);
  XDrawLine(dpy, TARGET, gc, 0, MENUBAR_H - 1, win_w, MENUBAR_H - 1);

  const char *items[] = { "File", "Window", "Help" };
  int x = 6;
  for (int i = 0; i < 3; i++) {
    int w = strlen(items[i]) * CHAR_W + 12;
    int highlighted = (menu_open && menu_item == i);
    if (highlighted) fill_rect(x, 2, w, MENUBAR_H - 5, col_active_title);
    XSetForeground(dpy, gc, highlighted ? col_white : col_black);
    draw_text(x + 6, MENUBAR_H - 8, items[i]);
    x += w;
  }
}

static void draw_dropdown(void) {
  if (!menu_open) return;
  int x = 6;
  for (int i = 0; i < menu_item; i++) {
    const char *items[] = { "File", "Window", "Help" };
    x += strlen(items[i]) * CHAR_W + 12;
  }

  if (menu_item == 0) {
    int y = MENUBAR_H, w = 150, h = 22;
    fill_rect(x, y, w, h, col_white);
    draw_rect(x, y, w, h, col_black);
    draw_text(x + 10, y + 16, "Exit");
  } else if (menu_item == 1) {
    int y = MENUBAR_H, w = 180, h = 44;
    fill_rect(x, y, w, h, col_white);
    draw_rect(x, y, w, h, col_black);
    draw_text(x + 10, y + 16, "Cascade  Shift+F5");
    draw_text(x + 10, y + 38, "Tile     Shift+F4");
  } else if (menu_item == 2) {
    int y = MENUBAR_H, w = 150, h = 22;
    fill_rect(x, y, w, h, col_white);
    draw_rect(x, y, w, h, col_black);
    draw_text(x + 10, y + 16, "About...");
  }
}

/* ================================================================ */
/* Actions */
/* ================================================================ */
static void cascade_groups(void) {
  int visible = 0;
  for (int i = 0; i < n_groups; i++) {
    if (groups[i].state != STATE_MINIMIZED) visible++;
  }
  if (visible == 0) return;

  int step = 28;
  int start_x = 8;
  int start_y = MENUBAR_H + 8;

  /* Last window room */
  int w = win_w - 2 * start_x - (visible - 1) * step;
  int h = win_h - start_y - 8 - (visible - 1) * step;
  if (w < 120) w = 120;
  if (h < 120) h = 120;

  int idx = 0;
  for (int i = 0; i < n_groups; i++) {
    if (groups[i].state == STATE_MINIMIZED) continue;
    groups[i].x = start_x + idx * step;
    groups[i].y = start_y + idx * step;
    groups[i].w = w;
    groups[i].h = h;
    idx++;
  }
}

static void tile_groups(void) {
  int visible = 0;
  for (int i = 0; i < n_groups; i++) {
    if (groups[i].state != STATE_MINIMIZED) visible++;
  }
  if (visible == 0) return;

  int cols = 1;
  while (cols * cols < visible) cols++;
  int rows = (visible + cols - 1) / cols;
  int w = win_w / cols;
  int h = (win_h - MENUBAR_H) / rows;
  int idx = 0;
  for (int i = 0; i < n_groups; i++) {
    if (groups[i].state == STATE_MINIMIZED) continue;
    int col = idx % cols;
    int row = idx / cols;
    groups[i].x = col * w;
    groups[i].y = MENUBAR_H + row * h;
    groups[i].w = w;
    groups[i].h = h;
    idx++;
  }
}

static void launch_app(Group *g, int idx) {
  if (idx < 0 || idx >= g->n_apps) return;
  if (fork() == 0) {
    execlp("/bin/sh", "sh", "-c", g->apps[idx].exec, (char*)NULL);
    _exit(127);
  }
}

/* Bring a group to the front by rotating it to the end of the array */
static void bring_to_front(int index) {
  if (index < 0 || index >= n_groups) return;
  if (index == n_groups - 1) return;

  Group temp = groups[index];
  for (int i = index; i < n_groups - 1; i++) {
    groups[i] = groups[i + 1];
  }
  groups[n_groups - 1] = temp;
  active_group = n_groups - 1;
}

/* Compute the next cascade slot for a newly opened group window */
static void place_new_group(Group *g) {
  int step = 28;
  int ws_x = 0;
  int ws_y = MENUBAR_H;
  int ws_w = win_w;
  int ws_h = win_h - MENUBAR_H;

  int gw = g->w;
  int gh = g->h;

  /* If nothing else is visible, put it at the top-left */
  int visible = 0;
  for (int i = 0; i < n_groups; i++) {
    if (&groups[i] == g) continue;
    if (groups[i].state != STATE_MINIMIZED) visible++;
  }
  if (visible == 0) {
    g->x = ws_x;
    g->y = ws_y;
    return;
  }

  /* Find the last-visible group to offset from */
  Group *last = NULL;
  for (int i = 0; i < n_groups; i++) {
    if (&groups[i] == g) continue;
    if (groups[i].state != STATE_MINIMIZED) last = &groups[i];
  }
  if (!last) {
    g->x = ws_x;
    g->y = ws_y;
    return;
  }

  int nx = last->x + step;
  int ny = last->y + step;

  /* bottom exceed, wrap to the top and shift right */
  if (ny + gh > ws_y + ws_h) {
    ny = ws_y;
    nx = last->x + step;
    /* right side exceed, wrap fully to the top-left */
    if (nx + gw > ws_x + ws_w) {
      nx = ws_x;
    }
  }
  /* right side exceed, wrap down and back to the left */
  if (nx + gw > ws_x + ws_w) {
    nx = ws_x;
    ny = last->y + step;
    if (ny + gh > ws_y + ws_h) {
      ny = ws_y;
    }
  }

  g->x = nx;
  g->y = ny;
}

/* ================================================================ */
/* Dialog */
/* ================================================================ */
static void draw_about_dialog(void) {
  int dw = 400, dh = 200;
  int dx = (win_w - dw) / 2;
  int dy = (win_h - dh) / 2;
  const char *l1 = WM_MGR_VERSION_NAME;
  const char *l2 = WM_MGR_VERSION_NUMBER;
  const char *l3 = WM_MGR_COPYRIGHT_UPD;
  int w1 = strlen(l1) * CHAR_W;
  int w2 = strlen(l2) * CHAR_W;
  int w3 = strlen(l3) * CHAR_W;

  fill_rect(dx-3, dy-3, dw+6, dh+6, col_black);
  fill_rect(dx-2, dy-2, dw+4, dh+4, col_active_title);
  fill_rect(dx+1, dy+1, dw-2, dh-2, col_white);
  fill_rect(dx+2, dy+2, dw-4, 20, col_active_title);
  XSetForeground(dpy, gc, col_white);
  draw_text(dx + (dw - 5 * CHAR_W) / 2, dy + 17, "About");
  XSetForeground(dpy, gc, col_black);

  draw_text(dx + (dw - w1)/2, dy + 60, l1);
  draw_text(dx + (dw - w2)/2, dy + 80, l2);
  draw_text(dx + (dw - w3)/2, dy + 100, l3);

  int bw = 80, bh = 26;
  int bx = dx + (dw - bw) / 2;
  int by = dy + dh - 46;
  fill_rect(bx, by, bw, bh, col_btn); // col_face
  draw_bevel(bx, by, bw, bh, 1);
  draw_rect(bx-1, by-1, bw+2, bh+2, col_black);
  XSetForeground(dpy, gc, col_black);
  draw_text(bx + (bw - 2 * CHAR_W) / 2, by + 17, "OK");

  draw_app_window(dx+40, dy+40);
}

static void draw_exit_dialog(void) {
  int dw = 400, dh = 160;
  int dx = (win_w - dw) / 2;
  int dy = (win_h - dh) / 2;

  /* Outer border, title bar, and face */
  fill_rect(dx-3, dy-3, dw+6, dh+6, col_black);
  fill_rect(dx-2, dy-2, dw+4, dh+4, col_active_title);
  fill_rect(dx+1, dy+1, dw-2, dh-2, col_white);
  fill_rect(dx+2, dy+2, dw-4, 20, col_active_title);
  XSetForeground(dpy, gc, col_white);
  draw_text(dx + (dw - 4 * CHAR_W) / 2, dy + 17, "Exit");
  XSetForeground(dpy, gc, col_black);

  /* Message */
  const char *msg = "Are you sure you want to exit?";
  int mw = XTextWidth(font, msg, strlen(msg));
  draw_text(dx + (dw - mw) / 2, dy + 60, msg);

  /* Yes / No buttons */
  int bw = 80, bh = 26;
  int gap = 20;
  int bx_yes = dx + (dw - (2 * bw + gap)) / 2;
  int bx_no  = bx_yes + bw + gap;
  int by = dy + dh - 46;

  /* Yes button */
  fill_rect(bx_yes, by, bw, bh, col_btn);
  draw_bevel(bx_yes, by, bw, bh, 1);
  draw_rect(bx_yes-1, by-1, bw+2, bh+2, col_black);
  XSetForeground(dpy, gc, col_black);
  draw_text(bx_yes + (bw - 3 * CHAR_W) / 2, by + 17, "Yes");

  /* No button */
  fill_rect(bx_no, by, bw, bh, col_btn);
  draw_bevel(bx_no, by, bw, bh, 1);
  draw_rect(bx_no-1, by-1, bw+2, bh+2, col_black);
  XSetForeground(dpy, gc, col_black);
  draw_text(bx_no + (bw - 2 * CHAR_W) / 2, by + 17, "No");
}

/* ================================================================ */
/* Main Window Drawing */
/* ================================================================ */
static void draw_all(void) {
  /* If no backbuffer or window resized, recreate it */
  if (backbuf == None || backbuf_w != win_w || backbuf_h != win_h) {
    if (backbuf != None) XFreePixmap(dpy, backbuf);
    backbuf = XCreatePixmap(dpy, TARGET, win_w, win_h,
                            DefaultDepth(dpy, scr));
    backbuf_w = win_w;
    backbuf_h = win_h;
  }

  /* Switch to the pixmap */
  XSetWindowBackground(dpy, TARGET, col_yellow);

  /* Draw everything into the backbuffer */
  extern Window progman_draw_target;
  progman_draw_target = backbuf;

  fill_rect(0, 0, win_w, win_h, col_yellow);

  /* Minimized group icons */
  int icon_col = 0;
  int group_slot_w = 80;
  int group_left = 30;
  for (int i = 0; i < n_groups; i++) {
    if (groups[i].state == STATE_MINIMIZED) {
      int y = win_h - GROUP_ICON_H - 12;
      int x = group_left + icon_col * group_slot_w;
      if (x + GROUP_ICON_W > win_w - 10) {
        y -= (GROUP_ICON_H + 10);
        x = group_left;
        icon_col = 0;
      }
      draw_group_icon(&groups[i], x, y);
      icon_col++;
    }
  }

  /* Visible array order group windows */
  for (int i = 0; i < n_groups; i++) {
    if (groups[i].state != STATE_MINIMIZED) {
      update_group_window(&groups[i]);
    }
  }

  /* Menu bar and dropdown last */
  draw_menubar();
  draw_dropdown();

  if (dlg_type == 1) draw_about_dialog();
  if (dlg_type == 2) draw_exit_dialog();

  /* Restore the target to the actual window */
  progman_draw_target = win;

  /* Blit the pixmap to the window in one operation */
  XCopyArea(dpy, backbuf, win, gc, 0, 0, win_w, win_h, 0, 0);
  XFlush(dpy);
}

/* ================================================================ */
/* Event Handling */
/* ================================================================ */
static int hit_menu(int mx, int my, int *which) {
  if (my < 0 || my >= MENUBAR_H) return 0;
  int x = 6;
  const char *items[] = { "File", "Window", "Help" };
  for (int i = 0; i < 3; i++) {
    int w = strlen(items[i]) * CHAR_W + 12;
    if (mx >= x && mx < x + w) { *which = i; return 1; }
    x += w;
  }
  return 0;
}

static int hit_title_button(Group *g, int mx, int my) {
  int tx = g->x + 2;
  int ty = g->y + 2;
  int tw = g->w - 4;
  int bw = 15, bh = 15;

  int bx = tx + tw - bw - 4;
  if (mx >= bx && mx <= bx + bw && my >= ty && my <= ty + bh) return 2;

  bx -= (bw + 2);
  if (mx >= bx && mx <= bx + bw && my >= ty && my <= ty + bh) return 1;

  if (mx >= tx + 2 && mx <= tx + 17 && my >= ty + 2 && my <= ty + 17) return 3;

  return 0;
}

/* ================================================================ */
/* Main */
/* ================================================================ */
int main(int argc, char **argv) {
  (void)argc; (void)argv;

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "Cannot open display\n");
    return 1;
  }
  signal(SIGTERM, on_terminate_signal);
  signal(SIGINT,  on_terminate_signal);
  signal(SIGHUP,  on_terminate_signal);
  scr = DefaultScreen(dpy);
  root = RootWindow(dpy, scr);
  wait_for_window_manager(5000);
  cmap = DefaultColormap(dpy, scr);
  XColor c;

  ALLOC(col_yellow, 0xFFFF, 0xFFFF, 0xEEEE);
  ALLOC(col_face, 0xC0C0, 0xC0C0, 0xC0C0);
  ALLOC(col_btn, 0xC0C0, 0xC4C4, 0xC8C8);
  ALLOC(col_white, 0xFFFF, 0xFFFF, 0xFFFF);
  ALLOC(col_black, 0x0000, 0x0000, 0x0000);
  ALLOC(col_gray, 0x8080, 0x8080, 0x8080);
  ALLOC(col_cyan, 0x0000, 0xC7C7, 0xCFCF);
  ALLOC(col_blue, 0x0000, 0x0000, 0xCFCF);
  ALLOC(col_active_title, 0x5353, 0x7F7F, 0xADAD);
  ALLOC(col_inactive_title, 0xA6A6, 0xA6A6, 0xA6A6);

  font = XLoadQueryFont(dpy, "-*-fixed-medium-r-normal--14-*-*-*-*-*-iso8859-1");
  if (!font) font = XLoadQueryFont(dpy, "fixed");

  /* Compute centered position on the screen. */
  XWindowAttributes root_attrs;
  int init_x = 0, init_y = 0;
  if (XGetWindowAttributes(dpy, root, &root_attrs)) {
    init_x = (root_attrs.width  - DEF_WIN_W) / 2 - MENUBAR_H / 2;
    init_y = (root_attrs.height - DEF_WIN_H) / 2 - MENUBAR_H / 2;
    if (init_x < 0) init_x = 0;
    if (init_y < 0) init_y = 0;
  }
  win = XCreateSimpleWindow(dpy, root, init_x, init_y, DEF_WIN_W, DEF_WIN_H, 0, col_black, col_yellow);
  progman_draw_target = win;
  {
    Cursor cursor = XCreateFontCursor(dpy, XC_left_ptr);
    XDefineCursor(dpy, win, cursor);
  }
  XStoreName(dpy, TARGET, "Program Manager");
  XSelectInput(dpy, TARGET, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
  wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wm_delete_window, 1);
  gc = XCreateGC(dpy, TARGET, 0, NULL);
  XSetFont(dpy, gc, font->fid);

  scan_applications();
  load_all_icons();

  /* Set initial window positions for all groups */
  int ws_x = 0;
  int ws_y = MENUBAR_H;
  int ws_w = win_w;
  int ws_h = win_h - MENUBAR_H;
  int def_w = (ws_w * 2) / 3;
  int def_h = (ws_h * 2) / 3;
  for (int i = 0; i < n_groups; i++) {
    groups[i].x = ws_x;
    groups[i].y = ws_y;
    groups[i].w = def_w;
    groups[i].h = def_h;
    groups[i].normal_x = groups[i].x;
    groups[i].normal_y = groups[i].y;
    groups[i].normal_w = groups[i].w;
    groups[i].normal_h = groups[i].h;
    groups[i].scroll_offset = 0;
  }

  /* Set the default group to be opened */
  int main_idx = -1;
  for (int i = 0; i < n_groups; i++) {
    if (strcmp(groups[i].name, "Accessories") == 0) {
      main_idx = i;
      break;
    }
  }
  if (main_idx == -1 && n_groups > 0) main_idx = 0;
  for (int i = 0; i < n_groups; i++) {
    groups[i].state = (i == main_idx) ? STATE_NORMAL : STATE_MINIMIZED;
  }

  /* Move the default group to the end so it's drawn on top */
  if (main_idx >= 0) {
    bring_to_front(main_idx);
  } else {
    active_group = -1;
  }

  XMapWindow(dpy, win);

  /* Main loop */
  for (;;) {
    /* Check if an external term signal has arrived */
    if (got_terminate_signal) {
      got_terminate_signal = 0;
      dlg_type = 2;
      draw_all();
    }

    XEvent ev;
    XNextEvent(dpy, &ev);

    if (ev.type == ClientMessage) {
      if ((Atom)ev.xclient.data.l[0] == wm_delete_window) {
        dlg_type = 2;
        draw_all();
      }
    } else if (ev.type == Expose) {
      if (ev.xexpose.count == 0) {
        draw_all();
      }
    } else if (ev.type == ConfigureNotify) {
      if (ev.xconfigure.window == win) {
        if (win_w != ev.xconfigure.width || win_h != ev.xconfigure.height) {
          win_w = ev.xconfigure.width;
          win_h = ev.xconfigure.height;
          draw_all();
        }
      }
    } else if (ev.type == KeyPress) {
      KeySym ks = XLookupKeysym(&ev.xkey, 0);
      if (dlg_type == 2) {
        if (ks == XK_y || ks == XK_Y || ks == XK_Return || ks == XK_KP_Enter) {
          XCloseDisplay(dpy);
          return 0;
        } else if (ks == XK_n || ks == XK_N || ks == XK_Escape) {
          dlg_type = 0;
          draw_all();
        }
      } else if (ks == XK_F4 && (ev.xkey.state & ShiftMask)) {
        tile_groups();
        draw_all();
      } else if (ks == XK_F5 && (ev.xkey.state & ShiftMask)) {
        cascade_groups();
        draw_all();
      } else if (ks == XK_Escape) {
        if (dlg_type == 1 || dlg_type == 2) { dlg_type = 0; draw_all(); }
        else if (menu_open) { menu_open = 0; draw_all(); }
      }
    } else if (ev.type == ButtonPress) {
      int mx = ev.xbutton.x, my = ev.xbutton.y;
      unsigned int button = ev.xbutton.button;
      int needs_redraw = 0;
      if (dlg_type == 2) {
        /* Exit confirmation dialog */
        int dw = 400, dh = 160;
        int dx = (win_w - dw) / 2;
        int dy = (win_h - dh) / 2;
        int bw = 80, bh = 26;
        int gap = 20;
        int bx_yes = dx + (dw - (2 * bw + gap)) / 2;
        int bx_no  = bx_yes + bw + gap;
        int by = dy + dh - 46;

        if (mx >= bx_yes && mx <= bx_yes + bw && my >= by && my <= by + bh) {
          /* Yes then exit */
          XCloseDisplay(dpy);
          return 0;
        } else if (mx >= bx_no && mx <= bx_no + bw && my >= by && my <= by + bh) {
          dlg_type = 0;
          needs_redraw = 1;
        } else {
          /* Any other click is No */
          dlg_type = 0;
          needs_redraw = 1;
        }
      } else if (dlg_type == 1) {
        dlg_type = 0;
        needs_redraw = 1;
      }
      /* Mouse wheel scrolling for the group under the cursor */
      else if (button == Button4 || button == Button5) {
        for (int i = n_groups - 1; i >= 0; i--) {
          Group *g = &groups[i];
          if (g->state == STATE_MINIMIZED) continue;
          if (mx >= g->x && mx <= g->x + g->w && my >= g->y && my <= g->y + g->h) {
            if (button == Button4) g->scroll_offset -= 20;
            else                  g->scroll_offset += 20;
            if (g->scroll_offset < 0) g->scroll_offset = 0;
            if (g->scroll_offset > g->max_scroll) g->scroll_offset = g->max_scroll;
            active_group = i;
            break;
          }
        }
        needs_redraw = 1;
      } else {
        int which;
        if (hit_menu(mx, my, &which)) {
          if (which == 0) { menu_open = !menu_open; menu_item = 0; }
          else if (which == 1) { menu_open = !menu_open; menu_item = 1; }
          else if (which == 2) { menu_open = !menu_open; menu_item = 2; }
          needs_redraw = 1;
        } else if (menu_open) {
          int x = 6;
          const char *items[] = { "File", "Window", "Help" };
          for (int i = 0; i < menu_item; i++) x += strlen(items[i]) * CHAR_W + 12;

          if (menu_item == 0 && my >= MENUBAR_H && my < MENUBAR_H + 22) {
            if (mx >= x && mx < x + 150) { dlg_type = 2; }
          } else if (menu_item == 1 && my >= MENUBAR_H && my < MENUBAR_H + 44) {
            if (my < MENUBAR_H + 22) cascade_groups();
            else tile_groups();
          } else if (menu_item == 2 && my >= MENUBAR_H && my < MENUBAR_H + 22) {
            if (mx >= x && mx < x + 150) { dlg_type = 1; }
          }
          menu_open = 0;
          needs_redraw = 1;
        } else {
          int handled = 0;

          /* Walk groups from top to bottom */
          for (int i = n_groups - 1; i >= 0 && !handled; i--) {
            Group *g = &groups[i];
            if (g->state == STATE_MINIMIZED) continue;

            /* Confirm the click is inside the group */
            int inside = (mx >= g->x && mx <= g->x + g->w &&
                          my >= g->y && my <= g->y + g->h);
            if (!inside) continue;

            /* 1. Title bar */
            if (my <= g->y + TITLEBAR_H) {
              int btn = hit_title_button(g, mx, my);
              if (btn == 1) {
                g->state = STATE_MINIMIZED;
                active_group = -1;
              } else if (btn == 2) {
                if (g->state == STATE_NORMAL) {
                  g->normal_x = g->x; g->normal_y = g->y;
                  g->normal_w = g->w; g->normal_h = g->h;
                  g->state = STATE_MAXIMIZED;
                  g->x = 0; g->y = MENUBAR_H;
                  g->w = win_w; g->h = win_h - MENUBAR_H;
                } else {
                  g->state = STATE_NORMAL;
                  g->x = g->normal_x; g->y = g->normal_y;
                  g->w = g->normal_w; g->h = g->normal_h;
                }
              } else {
                bring_to_front(i);
              }
              handled = 1;
              break;
            }

            /* 2. Scrollbar */
            if (g->max_scroll > 0) {
              int sb_x = g->x + g->w - 2 - SCROLLBAR_W;
              int sb_y = g->y + TITLEBAR_H;
              int sb_h = g->h - TITLEBAR_H - 2;
              if (mx >= sb_x && mx < sb_x + SCROLLBAR_W && my >= sb_y && my < sb_y + sb_h) {
                active_group = i;
                int arrow_h = 16;
                int track_y = sb_y + arrow_h;
                int track_h = sb_h - 2 * arrow_h;
                int thumb_h = (sb_h * track_h) / (g->max_scroll + sb_h);
                if (thumb_h < 16) thumb_h = 16;
                if (thumb_h > track_h) thumb_h = track_h;
                int thumb_y = track_y;
                if (g->max_scroll > 0)
                  thumb_y += (g->scroll_offset * (track_h - thumb_h)) / g->max_scroll;

                if (my < track_y) {
                  g->scroll_offset -= 20;
                  if (g->scroll_offset < 0) g->scroll_offset = 0;
                } else if (my >= track_y + track_h) {
                  g->scroll_offset += 20;
                  if (g->scroll_offset > g->max_scroll) g->scroll_offset = g->max_scroll;
                } else if (my < thumb_y) {
                  g->scroll_offset -= sb_h / 2;
                  if (g->scroll_offset < 0) g->scroll_offset = 0;
                } else {
                  g->scroll_offset += sb_h / 2;
                  if (g->scroll_offset > g->max_scroll) g->scroll_offset = g->max_scroll;
                }
                handled = 1;
                break;
              }
            }

            /* 3. Content area */
            {
              int app_idx = -1;
              int content_w = g->w - 4;
              if (g->max_scroll > 0) content_w -= SCROLLBAR_W;
              int slot_w = 104;
              int left_margin = 16;
              int icons_per_row = (content_w - left_margin * 2) / slot_w;
              if (icons_per_row < 1) icons_per_row = 1;
              int used_w = icons_per_row * slot_w;
              int block_x = g->x + 2 + (content_w - used_w) / 2;
              int base_y = g->y + TITLEBAR_H;
              int col = (mx - block_x) / slot_w;
              int row = (my - base_y - 10 + g->scroll_offset) / 86;
              if (col >= 0 && col < icons_per_row) {
                int idx = row * icons_per_row + col;
                if (idx >= 0 && idx < g->n_apps) app_idx = idx;
              }

              /* Bring the clicked group to the front */
              bring_to_front(i);
              g = &groups[n_groups - 1];
              int new_idx = n_groups - 1;
              if (app_idx >= 0) {
                g->selected_app = app_idx;
                static Time last_click = 0;
                static int last_g_idx = -1;
                static int last_i = -1;
                if (last_g_idx == new_idx && last_i == app_idx && ev.xbutton.time - last_click < 400) {
                  launch_app(g, app_idx);
                } else {
                  last_click = ev.xbutton.time;
                  last_g_idx = new_idx;
                  last_i = app_idx;
                }
              }
              handled = 1;
              break;
            }
          }

          /* 4. If nothing else was hit, minimized group icon */
          if (!handled) {
            int icon_col = 0;
            int group_slot_w = 80;
            int group_left = 30;
            for (int i = 0; i < n_groups && !handled; i++) {
              if (groups[i].state != STATE_MINIMIZED) continue;
              int y = win_h - GROUP_ICON_H - 12;
              int x = group_left + icon_col * group_slot_w;
              if (x + GROUP_ICON_W > win_w - 10) {
                y -= (GROUP_ICON_H + 10);
                x = group_left;
                icon_col = 0;
              }
              if (mx >= x - 20 && mx <= x + group_slot_w - 20 && my >= y && my <= y + GROUP_ICON_H) {
                groups[i].state = STATE_NORMAL;
                place_new_group(&groups[i]);
                groups[i].normal_x = groups[i].x;
                groups[i].normal_y = groups[i].y;
                bring_to_front(i);
                handled = 1;
              }
              icon_col++;
            }
          }

          if (!handled) {
            menu_open = 0;
          }
          needs_redraw = 1;
        }
      }
      if (needs_redraw) draw_all();
    }
  }
  return 0;
}
