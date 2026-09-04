/*
 * WM is a window manager (X basic-style)
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
#include <X11/Xlib.h>
#include "wmver.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ================================================================ */
/* X basic-style objects' handling */
/* ================================================================ */

/* Error events handler */
int wmCatch (Display *dpy, XErrorEvent *xe) {
  (void)dpy; /* Suppress unused parameter warning */
  printf ( "{ object: \"%s\", name: \"%s\", detail: { req: %u, err: %u }, message: \"%s\" }\n",
           program_invocation_short_name,
           WM_TUI_VERSION_SHORT,
           (*xe).request_code,
           (*xe).error_code,
           "Something went wrong");
  return EXIT_SUCCESS;
}

/* X basic-style window decoration */
static void wmWindowsBorder (Display *dpy, Window root, unsigned int brdw, unsigned long brdc) {
  unsigned int nwins, i;
  Window dummyw1, dummyw2, *wins;
  XWindowAttributes attr;
  XQueryTree(dpy, root, &dummyw1, &dummyw2, &wins, &nwins);
  /* XFlush(dpy); */

  for (i = 0; i < nwins; i++) {
    XGetWindowAttributes(dpy, wins[i], &attr);
    /* InputOutput is equals to 1; ... && attr.map_state == IsViewable */
    if (!attr.override_redirect && attr.class == InputOutput) {
      /* printf ( "{ object: \"%s\", name: \"%s\", attributes: [ %u, %u, %u ] }\n", */
      /*          program_invocation_short_name, */
      /*          WM_TUI_VERSION_SHORT, */
      /*          attr.class, */
      /*          attr.border_width, */
      /*          InputOutput ); */
      XSetWindowBorder(dpy, wins[i], brdc);
      XSetWindowBorderWidth(dpy, wins[i], brdw);
    }
  }

  XFree(wins);
}

/* ================================================================ */
/* Main launcher */
/* ================================================================ */

int main (void) {
  Display *dpy;
  Window root;
  int screen;
  XWindowAttributes attr;
  XSetWindowAttributes sattr;
  XButtonEvent start;
  int dragging = 0;
  XEvent ev;
  unsigned int brdw = 3;
  unsigned long brdc = 0xAFBFCF;

  if ( !(dpy = XOpenDisplay(0x0)) ) return EXIT_FAILURE;

  screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);

  sattr.border_pixel = 200;
  sattr.event_mask = SubstructureNotifyMask | StructureNotifyMask;
  XChangeWindowAttributes(dpy, root, CWEventMask | CWBorderPixel, &sattr);

  wmWindowsBorder(dpy, root, brdw, brdc);

  XGrabKey(dpy, XKeysymToKeycode(dpy, XStringToKeysym("F1")), Mod1Mask, root,
           True, GrabModeAsync, GrabModeAsync);
  XGrabButton(dpy, 1, Mod1Mask, root, True, ButtonPressMask, GrabModeAsync,
              GrabModeAsync, None, None);
  XGrabButton(dpy, 3, Mod1Mask, root, True, ButtonPressMask, GrabModeAsync,
              GrabModeAsync, None, None);

  XSetErrorHandler(wmCatch);

  /* Main event loop */
  for (;;) {
    XNextEvent ( dpy, &ev );
    if ( ev.type == KeyPress && ev.xkey.subwindow != None ) {
      XRaiseWindow ( dpy, ev.xkey.subwindow );
    } else if ( ev.type == ButtonPress && ev.xbutton.subwindow != None ) {
      XGrabPointer ( dpy, ev.xbutton.subwindow, True,
                     PointerMotionMask|ButtonReleaseMask, GrabModeAsync,
                     GrabModeAsync, None, None, CurrentTime );
      XGetWindowAttributes ( dpy, ev.xbutton.subwindow, &attr );
      start = ev.xbutton;
      dragging = 1;
    } else if ( ev.type == MotionNotify && dragging ) {
      int xdiff, ydiff;
      while ( XCheckTypedEvent(dpy, MotionNotify, &ev) );
      xdiff = ev.xbutton.x_root - start.x_root;
      ydiff = ev.xbutton.y_root - start.y_root;
      XMoveResizeWindow ( dpy, ev.xmotion.window,
                          attr.x + (start.button==1 ? xdiff : 0),
                          attr.y + (start.button==1 ? ydiff : 0),
                          MAX(1, attr.width + (start.button==3 ? xdiff : 0)),
                          MAX(1, attr.height + (start.button==3 ? ydiff : 0)) );
    } else if ( ev.type == ButtonRelease ) {
      XUngrabPointer ( dpy, CurrentTime );
      dragging = 0;
    } else if ( ev.type == CreateNotify ) {
      wmWindowsBorder(dpy, root, brdw, brdc);
    }
  }

  XSetErrorHandler (NULL);
}
