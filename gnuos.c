/*
 * GNUOS Executive is an MS-DOS Executive clone for Linux/X11
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
 * Compile:  gcc -O2 -o gnuos gnuos.c -lX11
 *
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "wmver.h"

/* ================================================================ */
/* Constants and macros */
/* ================================================================ */
#define MAX_DRIVES   16
#define MAX_FILES    4096
#define MAX_PATH     1024
#define MAX_NAME     256

#define WIN_W        900
#define WIN_H        620
#define MENUBAR_H    24
#define SEPARATOR_H  6
#define DRIVEBAR_H   30
#define PATHBAR_H    0
#define ROW_H        16
#define CHAR_W       7
#define MAX_COLS     3 // columns for short view

#define ALLOC(name, r, g, b) c.red=r; c.green=g; c.blue=b; XAllocColor(dpy, cmap, &c); name = c.pixel;

/* ================================================================ */
/* Data structures */
/* ================================================================ */
typedef struct {
  char label[32]; // e.g. "/" or "/boot/efi"
  char mount[MAX_PATH];
  char fs[16];
  int  is_floppy;
} Drive;

typedef struct {
  char name[MAX_NAME];
  int  is_dir;
  int  is_exec;
  long size;
  time_t mtime;
} FileEntry;

typedef enum {
  DIALOG_NONE = 0,
  DIALOG_INPUT,
  DIALOG_CONFIRM,
  DIALOG_BROWSE,
  DIALOG_MESSAGE,
  DIALOG_ABOUT,
  DIALOG_DELETE
} DialogType;

/* ================================================================ */
/* Globals */
/* ================================================================ */
static Display *dpy;
static int      scr;
static Window   win;
static GC       gc;
static XFontStruct *font, *font_bold;
static Colormap cmap;
static unsigned long col_bg, col_fg, col_hi_bg, col_hi_fg, col_btn,
  col_btn_hi, col_btn_lo, col_menu_bg, col_path_bg, col_dlg_bg;

static Drive     drives[MAX_DRIVES];
static int       ndrives = 0;
static int       cur_drive = 0;
static char      cwd[MAX_PATH];
static char      path_history[MAX_PATH];

static FileEntry files[MAX_FILES];
static int       nfiles = 0;
static int       sel_start = 0, sel_end = 0; // multiple selection range
static int       drag_anchor = -1;
static int       top_index = 0; // scroll offset
static int       short_view = 1;

/* menu state */
static int       menu_open = 0;
static int       menu_item = -1;

/* dialog state */
static DialogType dlg_type = DIALOG_NONE;
static char       dlg_prompt[512];
static char       dlg_input[MAX_PATH];
static int        dlg_input_pos = 0;
static int        dlg_result = -1; // Dialog Yes/No question answer: -1 pending, 0 no, 1 yes, 2 all
static char       dlg_browse_path[MAX_PATH];
static FileEntry  dlg_browse_files[MAX_FILES];
static int        dlg_browse_nfiles = 0;
static int        dlg_browse_sel = 0;
static int        dlg_browse_top = 0;

/* clipboard for copy/move */
static char       clip_files[256][MAX_PATH];
static int        clip_nfiles = 0;
static int        clip_is_move = 0;

/* feedback message */
static char       status_msg[256] = "";
static time_t     status_time = 0;

/* Track the actual window size */
static int win_w = WIN_W;
static int win_h = WIN_H;

/* Special menu */
static int special_menu_open = 0;
static int special_menu_separators = 1;

/* ================================================================ */
/* Utility */
/* ================================================================ */
static int cmp_entries(const void *a, const void *b) {
  const FileEntry *fa = a, *fb = b;
  if (fa->is_dir != fb->is_dir) return fb->is_dir - fa->is_dir;
  return strcasecmp(fa->name, fb->name);
}

static void set_status(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(status_msg, sizeof(status_msg), fmt, ap);
  va_end(ap);
  status_time = time(NULL);
}

/* Get home directory */
static void get_home(char *out) {
  const char *h = getenv("HOME");
  if (!h) {
    struct passwd *pw = getpwuid(getuid());
    h = pw ? pw->pw_dir : "/";
  }
  strncpy(out, h, MAX_PATH - 1);
  out[MAX_PATH-1] = 0;
}

/* ================================================================ */
/* Drive detection via df */
/* ================================================================ */
static void detect_drives(void) {
  ndrives = 0;
  FILE *fp = popen("df -T 2>/dev/null", "r");
  if (!fp) return;
  char line[1024];
  /* skip header */
  if (!fgets(line, sizeof(line), fp)) { pclose(fp); return; }
  while (fgets(line, sizeof(line), fp) && ndrives < MAX_DRIVES) {
    char dev[256], fs[32], mount[MAX_PATH];
    unsigned long blocks, used, avail;
    int pct;
    if (sscanf(line, "%255s %31s %lu %lu %lu %d%% %1023s",
               dev, fs, &blocks, &used, &avail, &pct, mount) != 7)
      continue;
    if (strcmp(fs, "ext4") != 0 && strcmp(fs, "ext3") != 0 &&
        strcmp(fs, "ext2") != 0 && strcmp(fs, "vfat") != 0 &&
        strcmp(fs, "fat") != 0 && strcmp(fs, "msdos") != 0 &&
        strcmp(fs, "xfs") != 0 && strcmp(fs, "btrfs") != 0)
      continue;
    /* skip pseudo filesystems */
    if (strncmp(dev, "/dev/", 5) != 0 &&
        strncmp(dev, "tmpfs", 5) != 0 &&
        strncmp(dev, "overlay", 7) != 0)
      continue;
    strncpy(drives[ndrives].mount, mount, MAX_PATH-1);
    strncpy(drives[ndrives].fs, fs, 15);
    if (strcmp(mount, "/") == 0)
      strcpy(drives[ndrives].label, "/");
    else
      strncpy(drives[ndrives].label, mount, 31);
    drives[ndrives].is_floppy = (strcmp(fs,"vfat")==0 ||
                                 strcmp(fs,"fat")==0 ||
                                 strcmp(fs,"msdos")==0);
    ndrives++;
  }
  pclose(fp);
  /* we have at least "/" */
  if (ndrives == 0) {
    strcpy(drives[0].label, "/");
    strcpy(drives[0].mount, "/");
    strcpy(drives[0].fs, "ext4");
    drives[0].is_floppy = 0;
    ndrives = 1;
  }
}

/* ================================================================ */
/* Directory listing */
/* ================================================================ */
static void load_dir(const char *path) {
  nfiles = 0;
  DIR *d = opendir(path);
  if (!d) {
    set_status("Cannot open %s: %s", path, strerror(errno));
    return;
  }
  struct dirent *de;
  while ((de = readdir(d)) && nfiles < MAX_FILES) {
    if (strcmp(de->d_name, ".") == 0) continue;   /* keep ".." for nav */
    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    FileEntry *fe = &files[nfiles];
    strncpy(fe->name, de->d_name, MAX_NAME-1);
    fe->name[MAX_NAME-1] = 0;
    fe->is_dir = S_ISDIR(st.st_mode);
    fe->is_exec = !fe->is_dir && (st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH));
    fe->size = st.st_size;
    fe->mtime = st.st_mtime;
    nfiles++;
  }
  closedir(d);
  qsort(files, nfiles, sizeof(FileEntry), cmp_entries);
  sel_start = sel_end = 0;
  drag_anchor = -1;
  top_index = 0;
}

static void change_dir(const char *path) {
  char real[MAX_PATH];
  if (!realpath(path, real)) {
    set_status("Invalid path: %s", path);
    return;
  }
  struct stat st;
  if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
    set_status("Not a directory: %s", real);
    return;
  }
  strncpy(path_history, cwd, MAX_PATH-1);
  strncpy(cwd, real, MAX_PATH-1);
  load_dir(cwd);
  set_status("%s", cwd);
}

/* ================================================================ */
/* Drawing helpers */
/* ================================================================ */
static void set_fg(unsigned long c) { XSetForeground(dpy, gc, c); }

static void draw_text(int x, int y, const char *s, int bold) {
  XSetFont(dpy, gc, bold ? font_bold->fid : font->fid);
  XDrawString(dpy, win, gc, x, y, s, strlen(s));
}

static void fill_rect(int x, int y, int w, int h, unsigned long c) {
  set_fg(c);
  XFillRectangle(dpy, win, gc, x, y, w, h);
}

static void draw_rect(int x, int y, int w, int h, unsigned long c) {
  set_fg(c);
  XDrawRectangle(dpy, win, gc, x, y, w-1, h-1);
}

/* Draw a floppy-drive icon */
static void draw_floppy_drive(int x, int y, int w, int h, int active) {
  /* outer body */
  set_fg(active ? col_hi_fg : col_fg);
  XFillRectangle(dpy, win, gc, x, y, w, h);
  set_fg(col_fg);
  XDrawRectangle(dpy, win, gc, x, y, w, h);
  /* slot */
  int sw = w * 80 / 100;
  int sx = x + (w - sw) / 2;
  fill_rect(sx, y + h/2 - 1, sw, 2, active ? col_fg : col_bg);
  set_fg(active ? col_hi_fg : col_fg);
  XDrawRectangle(dpy, win, gc, sx, y + h/2 - 1, sw, 2);
  /* lock area */
  fill_rect(x + w/2 - sw/4, y + h/2 - 2, sw/2, 4, active ? col_hi_fg : col_bg);
  set_fg(active ? col_fg : col_fg);
  XDrawRectangle(dpy, win, gc, x + w/2 - sw/4, y + h/2 - 2, sw/2, 4);
}

/* Draw a floppy-disk icon */
static void draw_floppy(int x, int y, int w, int h, int active) {
  /* outer body */
  set_fg(active ? col_hi_fg : col_fg);
  XFillRectangle(dpy, win, gc, x, y, w, h);
  set_fg(col_fg);
  XDrawRectangle(dpy, win, gc, x, y, w, h);
  /* shutter */
  int sw = w * 40 / 100;
  int sx = x + (w - sw) / 2;
  fill_rect(sx, y + 1, sw, h / 3, active ? col_fg : col_bg);
  set_fg(active ? col_hi_fg : col_fg);
  XDrawRectangle(dpy, win, gc, sx, y + 1, sw-1, h/3 - 1);
  /* label */
  fill_rect(x + 2, y + h/2, w - 4, h/2 - 3, active ? col_hi_fg : col_bg);
  set_fg(active ? col_fg : col_fg);
  XDrawRectangle(dpy, win, gc, x + 2, y + h/2, w - 5, h/2 - 4);
}

/* Shortening a string with "..." to fit maxlen chars */
static void ellipsize(const char *src, char *dst, int maxlen) {
  int len = strlen(src);
  if (len <= maxlen) { strcpy(dst, src); return; }
  if (maxlen < 4) { strncpy(dst, src, maxlen); dst[maxlen]=0; return; }
  strncpy(dst, src, maxlen - 3);
  dst[maxlen-3] = 0;
  strcat(dst, "...");
}

static void start_about_dialog(void) {
  dlg_type = DIALOG_ABOUT;
  dlg_result = -1;
}

static void draw_special_menu(void) {
  if (!special_menu_open) return;
  int x = 6 + 40 + 40; // special x position after File and View
  int y = MENUBAR_H;
  int w = 220;
  const char *items[] = { "About...", "Exit" };
  int n = 2;
  int h = n * 18 + 6;
  fill_rect(x, y, w, h + SEPARATOR_H * special_menu_separators, col_menu_bg);
  set_fg(col_fg);
  XDrawRectangle(dpy, win, gc, x, y, w-1, h-1 + SEPARATOR_H * special_menu_separators);
  for (int i = 0; i < n; i++) {
    if (i == 1) { // separator before Exit
      set_fg(col_btn_lo);
      /* XDrawLine(dpy, win, gc, x + 4, y + 6 + i * 18, x + w - 5, y + 6 + i * 18); */
      y += SEPARATOR_H; // we add a separator size every time we find it
      XDrawLine(dpy, win, gc, x + 4, y + i * 18, x + w - 5, y + i * 18);
      set_fg(col_fg);
    }
    draw_text(x + 10, y + 16 + i * 18, items[i], 0);
  }
}

/* ================================================================ */
/* Main window drawing */
/* ================================================================ */
static void draw_menubar(void) {
  fill_rect(0, 0, win_w, MENUBAR_H, col_menu_bg);
  set_fg(col_fg);
  XDrawLine(dpy, win, gc, 0, MENUBAR_H-1, win_w, MENUBAR_H-1);
  int x = 6;
  const char *items[] = { "File", "View", "Special" };
  for (int i = 0; i < 3; i++) {
    int w = strlen(items[i]) * CHAR_W + 12;
    int highlighted = (i == 2) ? special_menu_open : (menu_open && menu_item == i);
    if (highlighted) fill_rect(x, 2, w, MENUBAR_H-5, col_hi_bg);
    set_fg(highlighted ? col_hi_fg : col_fg);
    draw_text(x + 6, MENUBAR_H - 8, items[i], 0);
    x += w;
  }
  /* Title on the right */
  const char *title = "GNUOS Executive";
  int tw = strlen(title) * CHAR_W;
  set_fg(col_fg);
  draw_text(win_w - tw - 8, MENUBAR_H - 8, title, 0);
}

static void draw_drivebar(void) {
  fill_rect(0, MENUBAR_H, win_w, DRIVEBAR_H, col_bg);
  set_fg(col_fg);
  XDrawLine(dpy, win, gc, 0, MENUBAR_H + DRIVEBAR_H - 1, win_w, MENUBAR_H + DRIVEBAR_H - 1);
  int x = 8;
  int y = MENUBAR_H + 2;
  for (int i = 0; i < ndrives; i++) {
    int active = (i == cur_drive);
    /* fill with white when inactive and with black when active */
    fill_rect(x, y, 60+32, 26, active ? col_fg : col_bg); // +32
    if (active) {
      /* black body + white border */
      draw_rect(x, y, 60+32, 26, col_hi_fg); // +32
      draw_rect(x+1, y+1, 58+32, 24, col_hi_fg); // +32
    } else {
      /* white body + dark border + inner highlight */
      draw_rect(x, y, 60+32, 26, col_btn_lo); // +32
      draw_rect(x+1, y+1, 58+32, 24, col_btn_hi); // +32
    }
    draw_floppy_drive(x + 4, y + 5, 16+32, 16, active);
    char lbl[16];
    ellipsize(drives[i].label, lbl, 5);
    /* label color depends on the state */
    set_fg(active ? col_hi_fg : col_fg);
    draw_text(x + 24+32, y + 17, lbl, 0); // +32
    x += 68+32; // +32
    if (x + 60+32 > win_w - 200) break; // +32
  }
  /* path on the right of the drive icon */
  if (x + 10 < win_w) {
    set_fg(col_fg);
    draw_text(x + 10, y + 17, cwd, 0);
  }
}

/* return the rectangle of a file row */
static void file_row_rect(int idx, int *x, int *y, int *w, int *h) {
  int area_y = MENUBAR_H + DRIVEBAR_H + PATHBAR_H;
  int col = idx / ((win_h - area_y) / ROW_H);
  int row = idx % ((win_h - area_y) / ROW_H);
  *x = 6 + col * 300;
  *y = area_y + row * ROW_H;
  *w = 290;
  *h = ROW_H;
}

static void draw_filelist(void) {
  int area_y = MENUBAR_H + DRIVEBAR_H + PATHBAR_H;
  int area_h = win_h - area_y;
  fill_rect(0, area_y, win_w, area_h, col_bg);
  int rows = area_h / ROW_H;
  int ncols = (nfiles + rows - 1) / rows;
  if (ncols < 1) ncols = 1;
  /* Selection background for selected rows */
  for (int i = 0; i < nfiles; i++) {
    if (i >= sel_start && i <= sel_end) {
      int x, y, w, h;
      file_row_rect(i, &x, &y, &w, &h);
      fill_rect(x - 2, y, w, h, col_hi_bg);
    }
  }
  for (int i = 0; i < nfiles; i++) {
    int x, y, w, h;
    file_row_rect(i, &x, &y, &w, &h);
    if (y + ROW_H > win_h) continue;
    int selected = (i >= sel_start && i <= sel_end);
    unsigned long fg = selected ? col_hi_fg : col_fg;
    set_fg(fg);
    char label[32];
    ellipsize(files[i].name, label, 28);
    if (files[i].is_dir) {
      draw_text(x, y + ROW_H - 3, label, 1); // bold
    } else if (files[i].is_exec) {
      draw_text(x, y + ROW_H - 3, label, 0);
      /* underline executable */
      int tw = strlen(label) * CHAR_W;
      set_fg(fg);
      XDrawLine(dpy, win, gc, x, y + ROW_H - 2, x + tw, y + ROW_H - 2);
    } else {
      draw_text(x, y + ROW_H - 3, label, 0);
    }
    /* append size or dir mark on long view */
    if (!short_view) {
      char info[64];
      if (files[i].is_dir) strcpy(info, "<DIR>");
      else snprintf(info, sizeof(info), "%8ld", files[i].size);
      int tx = x + 200;
      if (tx + 60 < win_w) {
        set_fg(fg);
        draw_text(tx, y + ROW_H - 3, info, 0);
      }
    }
  }
  /* scrollbar indicator */
  if (nfiles > rows * MAX_COLS) {
    set_fg(col_fg);
    draw_text(win_w - 20, area_y + 16, "v", 0);
  }
}

static void draw_file_menu(void) {
  if (!menu_open || menu_item != 0) return;
  int x = 6;
  int y = MENUBAR_H;
  int w = 220;
  const char *items[] = { "Open", "Copy...", "Move...", "Rename...", "Delete", "Create Subdirectory..." };
  int n = 6;
  int h = n * 18 + 6;
  /* background + border */
  fill_rect(x, y, w, h, col_menu_bg);
  set_fg(col_fg);
  XDrawRectangle(dpy, win, gc, x, y, w-1, h-1);
  /* items on top */
  for (int i = 0; i < n; i++) {
    draw_text(x + 10, y + 16 + i * 18, items[i], 0);
  }
}

static void draw_all(void) {
  XClearWindow(dpy, win);
  draw_menubar();
  draw_drivebar();
  /* draw_pathbar(); */
  draw_filelist();
  /* draw_statusbar(); */
  draw_file_menu();
  draw_special_menu();
  XFlush(dpy);
}

/* ================================================================ */
/* Dialog drawing & handling */
/* ================================================================ */
static void draw_dialog(void) {
  if (dlg_type == DIALOG_NONE) return;
  int dw, dh;
  char dlg_title[32] = {0};
  switch (dlg_type) {
  case DIALOG_INPUT:   dw = 460; dh = 120; strcat(dlg_title, "Input"); break;
  case DIALOG_DELETE:
  case DIALOG_CONFIRM: dw = 460; dh = 120; strcat(dlg_title, "Confirm"); break;
  case DIALOG_BROWSE:  dw = 520; dh = 400; strcat(dlg_title, "Browse"); break;
  case DIALOG_MESSAGE: dw = 400; dh = 110; strcat(dlg_title, "Message"); break;
  case DIALOG_ABOUT:   dw = 400; dh = 240; strcat(dlg_title, "About"); break;
  default: return;
  }
  int dx = (win_w - dw) / 2;
  int dy = (win_h - dh) / 2;
  /* outline, border, and dialog background */
  fill_rect(dx-3, dy-3, dw+6, dh+6, col_fg);
  fill_rect(dx-2, dy-2, dw+4, dh+4, col_hi_bg);
  fill_rect(dx+1, dy+1, dw-2, dh-2, col_dlg_bg);
  /* title bar */
  fill_rect(dx+2, dy+2, dw-4, 20, col_hi_bg);
  set_fg(col_hi_fg);
  int w0 = strlen(dlg_title) * CHAR_W;
  draw_text(dx + (dw - w0) / 2, dy + 17, dlg_title, 0);
  /* dialog content */
  set_fg(col_fg);
  if (dlg_type != DIALOG_ABOUT)
    draw_text(dx + 16, dy + 46, dlg_prompt, 0);
  if (dlg_type == DIALOG_INPUT) {
    /* input box */
    fill_rect(dx + 16, dy + 60, dw - 32, 24, col_bg);
    set_fg(col_fg);
    draw_rect(dx + 16, dy + 60, dw - 32, 24, col_fg);
    draw_text(dx + 22, dy + 77, dlg_input, 0);
    /* cursor */
    int cx = dx + 22 + dlg_input_pos * CHAR_W;
    set_fg(col_fg);
    XDrawLine(dpy, win, gc, cx, dy + 62, cx, dy + 80);
    /* OK hint */
    draw_text(dx + 16, dy + 100, "[Enter]=OK  [Esc]=Cancel", 0);
  } else if (dlg_type == DIALOG_DELETE) {
    draw_text(dx + 16, dy + 100, "[Enter]=OK  [Esc]=Cancel", 0);
  } else if (dlg_type == DIALOG_CONFIRM) {
    draw_text(dx + 16, dy + 80, "[Y]es  [N]o  [A]ll", 0);
  } else if (dlg_type == DIALOG_MESSAGE) {
    draw_text(dx + 16, dy + 80, "[Enter] or [Esc] to close", 0);
  } else if (dlg_type == DIALOG_BROWSE) {
    /* browser path */
    fill_rect(dx + 16, dy + 60, dw - 32, 22, col_path_bg);
    set_fg(col_fg);
    draw_rect(dx + 16, dy + 60, dw - 32, 22, col_fg);
    char bp[256];
    ellipsize(dlg_browse_path, bp, (dw - 40) / CHAR_W);
    draw_text(dx + 22, dy + 76, bp, 0);
    /* listing area */
    int lx = dx + 16;
    int ly = dy + 90;
    int lw = dw - 32;
    int lh = dh - 130;
    fill_rect(lx, ly, lw, lh, col_bg);
    set_fg(col_fg);
    draw_rect(lx, ly, lw, lh, col_fg);
    int rows = lh / ROW_H;
    for (int i = 0; i < rows && i + dlg_browse_top < dlg_browse_nfiles; i++) {
      int idx = i + dlg_browse_top;
      int ry = ly + i * ROW_H;
      if (idx == dlg_browse_sel) {
        fill_rect(lx + 1, ry, lw - 2, ROW_H, col_hi_bg);
        set_fg(col_hi_fg);
      } else {
        set_fg(col_fg);
      }
      const char *nm = dlg_browse_files[idx].name;
      char tb[128];
      ellipsize(nm, tb, (lw - 20) / CHAR_W);
      draw_text(lx + 8, ry + ROW_H - 3, tb,
                dlg_browse_files[idx].is_dir);
    }
    set_fg(col_fg);
    draw_text(dx + 16, dy + dh - 14, "[Enter]=Open dir  [Backspace]=Up  [Tab]=Choose  [Esc]=Cancel", 0);
  } else if (dlg_type == DIALOG_ABOUT) {
    /* floppy icon */
    int ix = dx + 40;
    int iy = dy + 40;
    /* draw_floppy(ix, iy, 48, 48, 0); */
    draw_floppy(ix, iy + 18, 32, 32, 0);
    /* text block centered */
    const char *l1 = WM_MGR_VERSION_NAME;
    const char *l2 = "GNUOS Executive";
    int w1 = strlen(l1) * CHAR_W;
    int w2 = strlen(l2) * CHAR_W;
    draw_text(dx + (dw - w1) / 2, iy + 18, l1, 0);
    draw_text(dx + (dw - w2) / 2, iy + 36, l2, 0);
    /* version + copyright centered */
    const char *l3 = WM_MGR_VERSION_NUMBER;
    const char *l4 = WM_MGR_COPYRIGHT_UPD;
    int w3 = strlen(l3) * CHAR_W;
    int w4 = strlen(l4) * CHAR_W;
    draw_text(dx + (dw - w3) / 2, dy + 130, l3, 0);
    draw_text(dx + (dw - w4) / 2, dy + 152, l4, 0);
    /* OK button */
    int bw = 80, bh = 26;
    int bx = dx + (dw - bw) / 2;
    int by = dy + dh - 46;
    fill_rect(bx, by, bw, bh, col_btn);
    draw_rect(bx, by, bw, bh, col_btn_lo);
    draw_rect(bx+1, by+1, bw-2, bh-2, col_btn_hi);
    int oktw = strlen("OK") * CHAR_W;
    set_fg(col_fg);
    draw_text(bx + (bw - oktw) / 2, by + 17, "OK", 0);
  }
  XFlush(dpy);
}

/* Load directory into browser */
static void load_browse_dir(const char *path) {
  char real[MAX_PATH];
  if (!realpath(path, real)) return;
  strncpy(dlg_browse_path, real, MAX_PATH-1);
  dlg_browse_nfiles = 0;
  DIR *d = opendir(real);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d)) && dlg_browse_nfiles < MAX_FILES) {
    if (strcmp(de->d_name, ".") == 0) continue;
    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", real, de->d_name);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    /* only show directories in browser */
    if (!S_ISDIR(st.st_mode)) continue;
    FileEntry *fe = &dlg_browse_files[dlg_browse_nfiles];
    strncpy(fe->name, de->d_name, MAX_NAME-1);
    fe->name[MAX_NAME-1] = 0;
    fe->is_dir = 1;
    fe->is_exec = 0;
    dlg_browse_nfiles++;
  }
  closedir(d);
  /* ".." goes first, then alphabetically */
  for (int i = 0; i < dlg_browse_nfiles - 1; i++) {
    for (int j = i + 1; j < dlg_browse_nfiles; j++) {
      int a_dot = strcmp(dlg_browse_files[i].name, "..") == 0;
      int b_dot = strcmp(dlg_browse_files[j].name, "..") == 0;
      if (b_dot && !a_dot) {
        FileEntry t = dlg_browse_files[i];
        dlg_browse_files[i] = dlg_browse_files[j];
        dlg_browse_files[j] = t;
      } else if (a_dot == b_dot && strcasecmp(dlg_browse_files[i].name, dlg_browse_files[j].name) > 0) {
        FileEntry t = dlg_browse_files[i];
        dlg_browse_files[i] = dlg_browse_files[j];
        dlg_browse_files[j] = t;
      }
    }
  }
  dlg_browse_sel = 0;
  dlg_browse_top = 0;
}

/* ================================================================ */
/* File operations */
/* ================================================================ */
static void do_copy_file(const char *src, const char *dst, int move, int *overwrite_all) {
  struct stat st;
  if (stat(src, &st) != 0) return;

  /* if exists, ask */
  if (access(dst, F_OK) == 0 && !*overwrite_all) {
    char prompt[512];
    const char *dfn = strrchr(dst, '/');
    dfn = dfn ? dfn + 1 : dst;
    char efn[64];
    ellipsize(dfn, efn, 48); // 45 chars + "..."
    snprintf(prompt, sizeof(prompt), "Overwrite \"%s\"?", efn);
    strncpy(dlg_prompt, prompt, sizeof(dlg_prompt)-1);
    dlg_type = DIALOG_CONFIRM;
    dlg_result = -1;
    draw_dialog();
    /* input wait */
    while (dlg_result == -1) {
      XEvent e;
      XNextEvent(dpy, &e);
      if (e.type == KeyPress) {
        KeySym k = XLookupKeysym(&e.xkey, 0);
        if (k == XK_y || k == XK_Y) dlg_result = 1;
        else if (k == XK_n || k == XK_N) dlg_result = 0;
        else if (k == XK_a || k == XK_A) dlg_result = 2;
      }
    }
    if (dlg_result == 0) { dlg_type = DIALOG_NONE; return; }
    if (dlg_result == 2) *overwrite_all = 1;
    dlg_type = DIALOG_NONE;
  }

  if (move) {
    if (rename(src, dst) != 0) {
      /* copy then delete */
      FILE *in = fopen(src, "rb");
      if (!in) return;
      FILE *out = fopen(dst, "wb");
      if (!out) { fclose(in); return; }
      char buf[65536];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
      fclose(in); fclose(out);
      unlink(src);
    }
  } else {
    FILE *in = fopen(src, "rb");
    if (!in) return;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
      fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
  }
}

static void do_copy_move_to(const char *destdir, int move) {
  int overwrite_all = 0;
  for (int i = 0; i < clip_nfiles; i++) {
    const char *src = clip_files[i];
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char dst[MAX_PATH];
    snprintf(dst, sizeof(dst), "%s/%s", destdir, base);
    if (strcmp(src, dst) == 0) continue;
    do_copy_file(src, dst, move, &overwrite_all);
  }
  clip_nfiles = 0;
  load_dir(cwd);
  set_status("%s complete.", move ? "Move" : "Copy");
}

static void do_delete(void) {
  for (int i = sel_start; i <= sel_end && i < nfiles; i++) {
    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", cwd, files[i].name);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (rmdir(full) != 0)
        set_status("Cannot remove directory: %s", strerror(errno));
    } else {
      unlink(full);
    }
  }
  load_dir(cwd);
}

static void do_rename(const char *newname) {
  if (sel_start != sel_end || sel_start >= nfiles) return;
  char src[MAX_PATH], dst[MAX_PATH];
  snprintf(src, sizeof(src), "%s/%s", cwd, files[sel_start].name);
  snprintf(dst, sizeof(dst), "%s/%s", cwd, newname);
  if (rename(src, dst) != 0)
    set_status("Rename failed: %s", strerror(errno));
  load_dir(cwd);
}

static void do_mkdir(const char *name) {
  char full[MAX_PATH];
  snprintf(full, sizeof(full), "%s/%s", cwd, name);
  if (mkdir(full, 0755) != 0)
    set_status("mkdir failed: %s", strerror(errno));
  load_dir(cwd);
}

/* ================================================================ */
/* Event handling */
/* ================================================================ */
static void start_input_dialog(const char *prompt, const char *initial) {
  dlg_type = DIALOG_INPUT;
  strncpy(dlg_prompt, prompt, sizeof(dlg_prompt)-1);
  strncpy(dlg_input, initial ? initial : "", MAX_PATH-1);
  dlg_input[MAX_PATH-1] = 0;
  dlg_input_pos = strlen(dlg_input);
  dlg_result = -1;
}

static void start_delete_dialog(const char *prompt) {
  dlg_type = DIALOG_DELETE;
  strncpy(dlg_prompt, prompt, sizeof(dlg_prompt)-1);
  dlg_result = -1;
}

static void start_message_dialog(const char *prompt) {
  dlg_type = DIALOG_MESSAGE;
  strncpy(dlg_prompt, prompt, sizeof(dlg_prompt)-1);
  dlg_result = -1;
}

static int run_input_dialog(char *out, int outsz) {
  /* input dialogs modal loop */
  draw_all();
  draw_dialog();
  while (dlg_type == DIALOG_INPUT || dlg_type == DIALOG_CONFIRM ||
         dlg_type == DIALOG_MESSAGE || dlg_type == DIALOG_ABOUT ||
         dlg_type == DIALOG_BROWSE || dlg_type == DIALOG_DELETE) {
    XEvent e;
    XNextEvent(dpy, &e);
    if (e.type == Expose) {
      draw_all();
      draw_dialog();
    } else if (e.type == KeyPress) {
      KeySym k = XLookupKeysym(&e.xkey, 0);
      if (dlg_type == DIALOG_INPUT) {
        if (k == XK_Escape) {
          dlg_type = DIALOG_NONE; dlg_result = 0; break;
        } else if (k == XK_Return || k == XK_KP_Enter) {
          strncpy(out, dlg_input, outsz-1);
          out[outsz-1] = 0;
          dlg_type = DIALOG_NONE; dlg_result = 1; break;
        } else if (k == XK_BackSpace) {
          if (dlg_input_pos > 0) {
            dlg_input[--dlg_input_pos] = 0;
          }
        } else if (k == XK_Left) {
          if (dlg_input_pos > 0) dlg_input_pos--;
        } else if (k == XK_Right) {
          if (dlg_input[dlg_input_pos]) dlg_input_pos++;
        } else if (k == XK_Home) {
          dlg_input_pos = 0;
        } else if (k == XK_End) {
          dlg_input_pos = strlen(dlg_input);
        } else {
          char buf[8];
          int n = XLookupString(&e.xkey, buf, sizeof(buf), NULL, NULL);
          if (n > 0 && isprint((unsigned char)buf[0]) &&
              dlg_input_pos < MAX_PATH - 2) {
            memmove(&dlg_input[dlg_input_pos+1],
                    &dlg_input[dlg_input_pos],
                    strlen(&dlg_input[dlg_input_pos]) + 1);
            dlg_input[dlg_input_pos++] = buf[0];
          }
        }
        draw_all(); draw_dialog();
      } else if (dlg_type == DIALOG_DELETE) {
        if (k == XK_Escape) {
          dlg_type = DIALOG_NONE; dlg_result = 0; break;
        } else if (k == XK_Return || k == XK_KP_Enter) {
          dlg_type = DIALOG_NONE; dlg_result = 1; break;
        }
        draw_all(); draw_dialog();
      } else if (dlg_type == DIALOG_CONFIRM) {
        if (k == XK_y || k == XK_Y) { dlg_result = 1;
          dlg_type = DIALOG_NONE; break; }
        if (k == XK_n || k == XK_N) { dlg_result = 0;
          dlg_type = DIALOG_NONE; break; }
        if (k == XK_a || k == XK_A) { dlg_result = 2;
          dlg_type = DIALOG_NONE; break; }
        if (k == XK_Escape) { dlg_result = 0;
          dlg_type = DIALOG_NONE; break; }
      } else if (dlg_type == DIALOG_MESSAGE) {
        if (k == XK_Return || k == XK_KP_Enter || k == XK_Escape) {
          dlg_type = DIALOG_NONE; dlg_result = 1; break;
        }
      } else if (dlg_type == DIALOG_ABOUT) {
        if (k == XK_Return || k == XK_KP_Enter || k == XK_Escape) {
          dlg_type = DIALOG_NONE; dlg_result = 1; break;
        }
      } else if (dlg_type == DIALOG_BROWSE) {
        if (k == XK_Escape) {
          dlg_type = DIALOG_NONE; dlg_result = 0; break;
        } else if (k == XK_Return || k == XK_KP_Enter) {
          if (dlg_browse_nfiles > 0) {
            const char *nm = dlg_browse_files[dlg_browse_sel].name;
            char np[MAX_PATH];
            snprintf(np, sizeof(np), "%s/%s",
                     dlg_browse_path, nm);
            load_browse_dir(np);
          }
        } else if (k == XK_BackSpace) {
          char np[MAX_PATH];
          snprintf(np, sizeof(np), "%s/..", dlg_browse_path);
          load_browse_dir(np);
        } else if (k == XK_Tab) {
          strncpy(out, dlg_browse_path, outsz-1);
          out[outsz-1] = 0;
          dlg_type = DIALOG_NONE; dlg_result = 1; break;
        } else if (k == XK_Up) {
          if (dlg_browse_sel > 0) dlg_browse_sel--;
        } else if (k == XK_Down) {
          if (dlg_browse_sel < dlg_browse_nfiles - 1)
            dlg_browse_sel++;
        }
        draw_all(); draw_dialog();
      }
    } else if (e.type == ButtonPress) {
      int mx = e.xbutton.x, my = e.xbutton.y;
      if (dlg_type == DIALOG_MESSAGE || dlg_type == DIALOG_ABOUT) {
        dlg_type = DIALOG_NONE; dlg_result = 1; break;
      }
      if (dlg_type == DIALOG_BROWSE) {
        int dx = (win_w - 520) / 2;
        int dy = (win_h - 400) / 2;
        int lx = dx + 16, ly = dy + 90;
        int lh = 400 - 130;
        /* int rows = lh / ROW_H; */
        if (mx >= lx && mx < lx + 520 - 32 && my >= ly && my < ly + lh) {
          int idx = (my - ly) / ROW_H + dlg_browse_top;
          if (idx < dlg_browse_nfiles) {
            dlg_browse_sel = idx;
          }
        }
        /* double-click detection */
        static Time last_click = 0;
        static int  last_idx = -1;
        if (e.xbutton.button == 1) {
          Time now = e.xbutton.time;
          int idx = (my - ly) / ROW_H + dlg_browse_top;
          if (now - last_click < 400 && idx == last_idx && idx >= 0 && idx < dlg_browse_nfiles) {
            const char *nm = dlg_browse_files[idx].name;
            char np[MAX_PATH];
            snprintf(np, sizeof(np), "%s/%s", dlg_browse_path, nm);
            load_browse_dir(np);
          }
          last_click = now;
          last_idx = idx;
        }
        draw_all(); draw_dialog();
      }
      if (dlg_type == DIALOG_CONFIRM) {
        /* Any click means "no" to avoid accidental overwrites */
        dlg_type = DIALOG_NONE; dlg_result = 0; break;
      }
    }
  }
  return dlg_result;
}

static void open_selected(void) {
  if (sel_start >= nfiles) return;
  FileEntry *fe = &files[sel_start];
  char full[MAX_PATH];
  snprintf(full, sizeof(full), "%s/%s", cwd, fe->name);
  if (fe->is_dir) {
    change_dir(full);
  } else if (fe->is_exec) {
    /* launch asynchronously */
    if (fork() == 0) {
      execl(full, fe->name, (char*)NULL);
      _exit(127);
    }
    set_status("Launched %s", fe->name);
  } else {
    /* what to do with non-executable */
    char prompt[512];
    snprintf(prompt, sizeof(prompt), "Open \"%s\" with which program?", fe->name);
    start_input_dialog(prompt, "");
    char cmd[MAX_PATH];
    if (run_input_dialog(cmd, sizeof(cmd)) == 1 && cmd[0]) {
      if (fork() == 0) {
        execlp(cmd, cmd, full, (char*)NULL);
        _exit(127);
      }
    }
  }
}

/* ================================================================ */
/* Menu action dispatch */
/* ================================================================ */
static void menu_action(int item) {
  char buf[MAX_PATH];
  switch (item) {
  case 0: // Open
    open_selected();
    break;
  case 1: // Copy
  case 2: // Move
    if (sel_start >= nfiles) break;
    clip_nfiles = 0;
    clip_is_move = (item == 2);
    for (int i = sel_start; i <= sel_end && i < nfiles; i++) {
      snprintf(clip_files[clip_nfiles], MAX_PATH, "%s/%s", cwd, files[i].name);
      clip_nfiles++;
    }
    strcpy(dlg_prompt, clip_is_move ? "Select destination for MOVE:" : "Select destination for COPY:");
    load_browse_dir(cwd);
    dlg_type = DIALOG_BROWSE;
    dlg_result = -1;
    if (run_input_dialog(buf, sizeof(buf)) == 1)
      do_copy_move_to(buf, clip_is_move);
    break;
  case 3: // Rename
    if (sel_start != sel_end || sel_start >= nfiles) {
      start_message_dialog("Select a single file to rename.");
      run_input_dialog(buf, sizeof(buf));
      break;
    }
    start_input_dialog("New name:", files[sel_start].name);
    if (run_input_dialog(buf, sizeof(buf)) == 1 && buf[0])
      do_rename(buf);
    break;
  case 4: // Delete
    if (sel_start >= nfiles) break;
    start_delete_dialog("Delete selected?");
    if (run_input_dialog(buf, sizeof(buf)) == 1)
      do_delete();
    break;
  case 5: // Create Subdirectory
    start_input_dialog("Name of new directory:", "");
    if (run_input_dialog(buf, sizeof(buf)) == 1 && buf[0])
      do_mkdir(buf);
    break;
  }
}

/* ================================================================ */
/* Mouse/key handling for main window */
/* ================================================================ */
static int hit_menu(int mx, int my, int *which) {
  if (my >= 0 && my < MENUBAR_H) {
    int x = 6;
    const char *items[] = { "File", "View", "Special" };
    for (int i = 0; i < 3; i++) {
      int w = strlen(items[i]) * CHAR_W + 12;
      if (mx >= x && mx < x + w) { *which = i; return 1; }
      x += w;
    }
  }
  return 0;
}

static int hit_drive(int mx, int my, int *which) {
  int y = MENUBAR_H + 2;
  if (my < y || my > y + 26) return 0;
  int x = 8;
  for (int i = 0; i < ndrives; i++) {
    if (mx >= x && mx < x + 60+32) { *which = i; return 1; } // +32
    x += 68+32; // +32
    if (x + 60+32 > win_w - 200) break; // +32
  }
  return 0;
}

static int hit_file(int mx, int my, int *which) {
  int area_y = MENUBAR_H + DRIVEBAR_H + PATHBAR_H;
  if (my < area_y) return 0;
  int rows = (win_h - area_y) / ROW_H;
  int col = mx / 300;
  if (col < 0 || col * 300 >= win_w) return 0;
  int row = (my - area_y) / ROW_H;
  if (row < 0 || row >= rows) return 0;
  int idx = col * rows + row;
  if (idx < 0 || idx >= nfiles) return 0;
  *which = idx;
  return 1;
}

static int hit_menu_item(int mx, int my, int *which) {
  if (!menu_open || menu_item != 0) return 0;
  int x = 6, y = MENUBAR_H, w = 220, h = 6 * 18 + 6;
  if (mx < x || mx > x + w) return 0;
  if (my < y || my > y + h) return 0;
  int idx = (my - y - 6) / 18;
  if (idx < 0 || idx > 5) return 0;
  *which = idx;
  return 1;
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
  scr = DefaultScreen(dpy);

  /* Colors */
  cmap = DefaultColormap(dpy, scr);
  XColor c;
  ALLOC(col_bg, 0xFFFF, 0xFFFF, 0xFFFF); // white
  ALLOC(col_fg, 0x0000, 0x0000, 0x0000); // black
  /* ALLOC(col_hi_bg, 0x0000, 0x0000, 0x8000); // windows 3.1 dark blue */
  ALLOC(col_hi_bg, 0x5353, 0x7F7F, 0xADAD); // windows 3.0 blue
  ALLOC(col_hi_fg, 0xFFFF, 0xFFFF, 0xFFFF);
  ALLOC(col_btn, 0xC0C0, 0xC0C0, 0xC0C0);
  ALLOC(col_btn_hi, 0xFFFF, 0xFFFF, 0xFFFF);
  ALLOC(col_btn_lo, 0x8080, 0x8080, 0x8080);
  ALLOC(col_menu_bg, 0xFFFF, 0xFFFF, 0xFFFF);
  ALLOC(col_path_bg, 0xFFFF, 0xFFFF, 0xFFFF);
  /* ALLOC(col_dlg_bg, 0xC0C0, 0xC0C0, 0xC0C0) */
  ALLOC(col_dlg_bg, 0xFFFF, 0xFFFF, 0xFFFF);

  /* Fonts */
  font = XLoadQueryFont(dpy, "-*-fixed-medium-r-normal--14-*-*-*-*-*-iso8859-1");
  if (!font) font = XLoadQueryFont(dpy, "fixed");
  font_bold = XLoadQueryFont(dpy, "-*-fixed-bold-r-normal--14-*-*-*-*-*-iso8859-1");
  if (!font_bold) font_bold = XLoadQueryFont(dpy, "-*-fixed-bold-r-normal--*-*-*-*-*-iso8859-1");
  if (!font_bold) font_bold = font;

  win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, WIN_W, WIN_H, 1, col_fg, col_bg);
  XStoreName(dpy, win, "GNUOS Executive");
  XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask);
  XMapWindow(dpy, win);

  gc = XCreateGC(dpy, win, 0, NULL);
  XSetFont(dpy, gc, font->fid);

  /* Initialise */
  detect_drives();
  get_home(cwd);
  load_dir(cwd);

  /* Drag selection state */
  static Time last_click_time = 0;
  static int  last_click_idx = -1;

  /* Event loop */
  for (;;) {
    XEvent e;
    XNextEvent(dpy, &e);
    if (e.type == Expose) {
      if (e.xexpose.count == 0) {
        draw_all();
        draw_dialog();
      }
    } else if (e.type == ConfigureNotify) {
      win_w = e.xconfigure.width;
      win_h = e.xconfigure.height;
      draw_all();
    } else if (e.type == KeyPress) {
      KeySym k = XLookupKeysym(&e.xkey, 0);
      switch (k) {
      case XK_Up:
        if (sel_start > 0) {
          sel_start--; sel_end = sel_start;
        }
        break;
      case XK_Down:
        if (sel_end < nfiles - 1) {
          sel_end++; sel_start = sel_end;
        }
        break;
      case XK_Home:
        sel_start = sel_end = 0;
        break;
      case XK_End:
        sel_start = sel_end = nfiles - 1;
        break;
      case XK_Return:
      case XK_KP_Enter:
        open_selected();
        break;
      case XK_BackSpace: {
        char up[MAX_PATH];
        snprintf(up, sizeof(up), "%s/..", cwd);
        change_dir(up);
        break;
      }
      case XK_F5:
        load_dir(cwd);
        break;
      case XK_F2: // quick rename
        if (sel_start == sel_end && sel_start < nfiles) {
          char b[MAX_PATH];
          start_input_dialog("New name:", files[sel_start].name);
          if (run_input_dialog(b, sizeof(b)) == 1 && b[0])
            do_rename(b);
        }
        break;
      case XK_F7:
        {
          char b[MAX_PATH];
          start_input_dialog("Name of new directory:", "");
          if (run_input_dialog(b, sizeof(b)) == 1 && b[0])
            do_mkdir(b);
        }
        break;
      case XK_F8:
      case XK_Delete:
        {
          char b[MAX_PATH];
          start_delete_dialog("Delete selected?");
          if (run_input_dialog(b, sizeof(b)) == 1)
            do_delete();
        }
        break;
      default:
        if (k == XK_Escape) {
          if (menu_open) { menu_open = 0; draw_all(); }
        }
        break;
      }
      draw_all();
    } else if (e.type == ButtonPress) {
      int mx = e.xbutton.x, my = e.xbutton.y;
      int shift = (e.xbutton.state & ShiftMask) != 0;
      if (e.xbutton.button == 1) {
        int which;
        /* Special dropdown has highest priority while open */
        if (special_menu_open) {
          int sx = 6 + 40 + 40; // matches draw_special_menu
          int sy = MENUBAR_H;
          int sw = 220;
          int sh = 2 * 18 + 6 + SEPARATOR_H * special_menu_separators;
          if (mx >= sx && mx < sx + sw && my >= sy && my < sy + sh) {
            int idx = (my - sy - 6 + SEPARATOR_H * special_menu_separators / 2)
              / (18 + SEPARATOR_H * special_menu_separators / 2);
            special_menu_open = 0;
            if (idx == 0) { // About
              XSync(dpy, False);
              while (XPending(dpy)) { XEvent d; XNextEvent(dpy, &d); }
              start_about_dialog();
              char tmp[4];
              run_input_dialog(tmp, sizeof(tmp));
            } else if (idx == 1) { // Exit
              XCloseDisplay(dpy);
              exit(0);
            }
            draw_all();
            continue; // done
          }
          /* Clear the dropdown menu if the click outside menubar. */
          if (my >= MENUBAR_H) {
            special_menu_open = 0;
          }
        }
        /* Menubar */
        if (hit_menu(mx, my, &which)) {
          if (which == 0) { // File
            special_menu_open = 0;
            if (menu_open && menu_item == 0) menu_open = 0;
            else { menu_open = 1; menu_item = 0; }
          } else if (which == 1) { // View
            short_view = !short_view;
            menu_open = 0;
            special_menu_open = 0;
          } else if (which == 2) { // Special
            int was_open = special_menu_open;
            menu_open = 0;
            special_menu_open = !was_open;
          }
          draw_all();
        }
        /* File menu items */
        else if (hit_menu_item(mx, my, &which)) {
          menu_open = 0;
          menu_action(which);
          draw_all();
        }
        /* Drive buttons */
        else if (hit_drive(mx, my, &which)) {
          cur_drive = which;
          change_dir(drives[which].mount);
          draw_all();
        }
        /* File list */
        else if (hit_file(mx, my, &which)) {
          Time now = e.xbutton.time;
          int dbl = (last_click_idx == which && now - last_click_time < 400);
          last_click_time = now;
          last_click_idx = which;
          if (shift && drag_anchor >= 0) {
            if (which < drag_anchor) { sel_start = which; sel_end = drag_anchor; }
            else { sel_start = drag_anchor; sel_end = which; }
          } else {
            sel_start = sel_end = which;
            drag_anchor = which;
          }
          draw_all();
          if (dbl && !shift) {
            open_selected();
            draw_all();
            last_click_idx = -1;
          }
        }
        /* Anywhere else: close menus */
        else {
          if (menu_open || special_menu_open) {
            menu_open = 0;
            special_menu_open = 0;
            draw_all();
          }
        }
      }
    }
  }
  return 0;
}
