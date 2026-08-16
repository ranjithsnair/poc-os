// gui_proto: a small bespoke wire protocol over AF_UNIX SOCK_STREAM +
// SCM_RIGHTS - deliberately *not* a continuation of the real
// Wayland-wire opcodes GUI roadmap phase 4 built (bash/poc/wire.h,
// deleted in e149182 along with the rest of that phase's IPC layer).
// There is no upstream Wayland client this needs to interoperate
// with, so matching wl_seat/xdg_shell/etc.'s real spec would be
// unnecessary complexity for what's otherwise the same
// shm-buffer-passing mechanism ToaruOS's own bespoke "yutani"
// protocol already demonstrates is sufficient. See
// /Users/ranjith/.claude/plans/serene-coalescing-raven.md, Phase 7.
//
// Each message is one whole struct sent via a single sendmsg() call -
// kernel/socket.c's sockrecv()/socksend() are message-boundary-
// preserving (one sendmsg = one recvmsg, up to SOCKMSGSIZE=512 bytes,
// include/socket.h), so no length-prefix framing is needed on top;
// every message here also fits well under that 512-byte cap.
#ifndef GUI_PROTO_H
#define GUI_PROTO_H

#define GUI_SOCK_PATH "guisrv.sock"
#define GUI_TITLE_MAX 32

enum {
  GUI_MSG_CREATE_SURFACE = 1,  // client -> compositor
  GUI_MSG_SURFACE_CREATED = 2, // compositor -> client, shm fd rides via SCM_RIGHTS
  GUI_MSG_COMMIT = 3,          // client -> compositor
  GUI_MSG_DESTROY = 4,         // client -> compositor
  GUI_MSG_KEY_EVENT = 5,       // compositor -> client
  GUI_MSG_POINTER_EVENT = 6,   // compositor -> client
  GUI_MSG_FOCUS_EVENT = 7,     // compositor -> client
};

struct gui_msg_create_surface {
  int type;
  int w, h;
  char title[GUI_TITLE_MAX];
};

// Field layout mirrors include/fb.h's struct fb_info exactly - the
// client's gfx_surface uses the compositor's real framebuffer's own
// pixel format, so gfx_blit() is a raw per-scanline memcpy (see
// gfx.h's own comment).
struct gui_msg_surface_created {
  int type;
  int surface_id;
  unsigned int w, h, pitch;
  unsigned char bpp;
  unsigned char red_mask_size, red_field_pos;
  unsigned char green_mask_size, green_field_pos;
  unsigned char blue_mask_size, blue_field_pos;
};

struct gui_msg_commit {
  int type;
  int surface_id;
};

struct gui_msg_destroy {
  int type;
  int surface_id;
};

struct gui_msg_key_event {
  int type;
  int ch;      // ASCII byte from kbdgetc() (kernel/kbd.c) - press-only, no separate release event
};

struct gui_msg_pointer_event {
  int type;
  int x, y;    // window-relative
  int buttons; // bit 0 = left button
};

struct gui_msg_focus_event {
  int type;
  int focused; // 1 = gained focus, 0 = lost
};

// Largest of the above - sized so a single fixed-size recvmsg() buffer
// on either side can hold any message type without a preliminary
// "peek the type first" round trip.
union gui_msg {
  int type;
  struct gui_msg_create_surface create_surface;
  struct gui_msg_surface_created surface_created;
  struct gui_msg_commit commit;
  struct gui_msg_destroy destroy;
  struct gui_msg_key_event key_event;
  struct gui_msg_pointer_event pointer_event;
  struct gui_msg_focus_event focus_event;
};

#endif
