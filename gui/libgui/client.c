#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "libgui.h"
#include "gui_proto.h"
#include "wire.h"

int
gui_connect(struct gui_conn *c, const char *sockpath)
{
  struct sockaddr_un addr;

  memset(c, 0, sizeof(*c));
  c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (c->fd < 0)
    return -1;

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, sockpath);
  if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(c->fd);
    return -1;
  }
  return 0;
}

int
gui_create_surface(struct gui_conn *c, int w, int h, const char *title)
{
  struct gui_msg_create_surface req;
  union gui_msg resp;
  int shmfd = -1;

  memset(&req, 0, sizeof(req));
  req.type = GUI_MSG_CREATE_SURFACE;
  req.w = w;
  req.h = h;
  if (title) {
    unsigned long n = strlen(title);

    if (n >= sizeof(req.title))
      n = sizeof(req.title) - 1;
    memcpy(req.title, title, n);
  }

  if (wire_send(c->fd, &req, sizeof(req), -1) != (int)sizeof(req))
    return -1;

  if (wire_recv(c->fd, &resp, sizeof(resp), &shmfd) <= 0)
    return -1;
  if (resp.type != GUI_MSG_SURFACE_CREATED || shmfd < 0)
    return -1;

  c->surface_id = resp.surface_created.surface_id;
  c->surface.w = resp.surface_created.w;
  c->surface.h = resp.surface_created.h;
  c->surface.pitch = resp.surface_created.pitch;
  c->surface.red_mask_size = resp.surface_created.red_mask_size;
  c->surface.red_field_pos = resp.surface_created.red_field_pos;
  c->surface.green_mask_size = resp.surface_created.green_mask_size;
  c->surface.green_field_pos = resp.surface_created.green_field_pos;
  c->surface.blue_mask_size = resp.surface_created.blue_mask_size;
  c->surface.blue_field_pos = resp.surface_created.blue_field_pos;

  c->surface.pixels = mmap(0, (unsigned long)resp.surface_created.pitch * resp.surface_created.h,
                            PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
  close(shmfd);
  return c->surface.pixels == MAP_FAILED ? -1 : 0;
}

int
gui_commit(struct gui_conn *c)
{
  struct gui_msg_commit msg;

  msg.type = GUI_MSG_COMMIT;
  msg.surface_id = c->surface_id;
  return wire_send(c->fd, &msg, sizeof(msg), -1) == (int)sizeof(msg) ? 0 : -1;
}

void
gui_destroy(struct gui_conn *c)
{
  struct gui_msg_destroy msg;

  msg.type = GUI_MSG_DESTROY;
  msg.surface_id = c->surface_id;
  wire_send(c->fd, &msg, sizeof(msg), -1);
  close(c->fd);
}

int
gui_recv_event(struct gui_conn *c, struct gui_event *ev)
{
  union gui_msg raw;

  if (wire_recv(c->fd, &raw, sizeof(raw), 0) <= 0)
    return -1;

  switch (raw.type) {
  case GUI_MSG_KEY_EVENT:
    ev->type = GUI_EVENT_KEY;
    ev->key.ch = raw.key_event.ch;
    break;
  case GUI_MSG_POINTER_EVENT:
    ev->type = GUI_EVENT_POINTER;
    ev->pointer.x = raw.pointer_event.x;
    ev->pointer.y = raw.pointer_event.y;
    ev->pointer.buttons = raw.pointer_event.buttons;
    break;
  case GUI_MSG_FOCUS_EVENT:
    ev->type = GUI_EVENT_FOCUS;
    ev->focus.focused = raw.focus_event.focused;
    break;
  default:
    return -1;
  }
  return 0;
}
