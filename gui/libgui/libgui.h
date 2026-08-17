// libgui: the client half of the GUI roadmap phase 7-10 stack - a
// real shared object (build/libgui.so, DT_NEEDED-linked, not another
// copy of the phase-2-style single-interpreter libc.so pattern) used
// by every client of gui/compositor.c (Phase 8): gui/login_gui.c
// (Phase 9), gui/terminal.c (Phase 10), and future apps. Mirrors
// ToaruOS's own libtoaru_graphics/libtoaru_yutani split.
#ifndef LIBGUI_H
#define LIBGUI_H

#include "gfx.h"

struct gui_conn {
  int fd;
  int surface_id;
  struct gfx_surface surface;
  unsigned int screen_w, screen_h;  // whole framebuffer, not just this surface
};

// Connects to the compositor's well-known AF_UNIX socket
// (GUI_SOCK_PATH, gui_proto.h). Returns 0 on success, -1 on failure.
int gui_connect(struct gui_conn *c, const char *sockpath);

// Requests a w x h surface with the given title (may be NULL), placed
// at (x,y) - or auto-cascaded, the original behavior, if x and y are
// both -1 - with the given gui_proto.h GUI_WIN_* flags (0 for a normal
// decorated app window). On success, c->surface is mmap()'d and ready
// to draw into via gfx.h's functions; c->surface_id identifies it to
// the compositor for gui_commit()/gui_destroy(); c->screen_w/screen_h
// report the whole framebuffer's size. Returns 0 on success, -1 on
// failure.
int gui_create_surface(struct gui_conn *c, int w, int h, int x, int y,
                        int flags, const char *title);

// Tells the compositor the surface's current pixel contents are ready
// to be composited (blitted into the real framebuffer on the next
// redraw). Returns 0 on success, -1 on failure.
int gui_commit(struct gui_conn *c);

// Tells the compositor to drop the surface and closes the connection.
void gui_destroy(struct gui_conn *c);

enum { GUI_EVENT_KEY, GUI_EVENT_POINTER, GUI_EVENT_FOCUS };

struct gui_event {
  int type;
  union {
    struct { int ch; } key;
    struct { int x, y, buttons; } pointer;
    struct { int focused; } focus;
  };
};

// Blocks until the compositor delivers one input event (only ever
// sent while this surface has focus, for key events - pointer/focus
// events are sent regardless). Returns 0 on success, -1 on failure
// (including the compositor closing the connection).
int gui_recv_event(struct gui_conn *c, struct gui_event *ev);

#endif
