#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gfx.h"

unsigned int
gfx_pack(struct gfx_surface *s, unsigned int color)
{
  unsigned int r = (color >> 16) & 0xff;
  unsigned int g = (color >> 8) & 0xff;
  unsigned int b = color & 0xff;

  return (r << s->red_field_pos) | (g << s->green_field_pos) | (b << s->blue_field_pos);
}

void
gfx_fill_rect(struct gfx_surface *s, int x0, int y0, int x1, int y1, unsigned int color)
{
  unsigned int px = gfx_pack(s, color);
  int x, y;

  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > (int)s->w) x1 = s->w;
  if (y1 > (int)s->h) y1 = s->h;

  for (y = y0; y < y1; y++) {
    unsigned char *row = s->pixels + (unsigned long)y * s->pitch;

    for (x = x0; x < x1; x++)
      *(unsigned int *)(row + x * 4) = px;
  }
}

void
gfx_blit(struct gfx_surface *dst, int dx, int dy,
         struct gfx_surface *src, int sx, int sy, int w, int h)
{
  int y, cw = w, ch = h;

  if (dx < 0) { sx -= dx; cw += dx; dx = 0; }
  if (dy < 0) { sy -= dy; ch += dy; dy = 0; }
  if (dx + cw > (int)dst->w) cw = (int)dst->w - dx;
  if (dy + ch > (int)dst->h) ch = (int)dst->h - dy;
  if (cw <= 0 || ch <= 0)
    return;

  for (y = 0; y < ch; y++) {
    unsigned char *srow = src->pixels + (unsigned long)(sy + y) * src->pitch + (unsigned long)sx * 4;
    unsigned char *drow = dst->pixels + (unsigned long)(dy + y) * dst->pitch + (unsigned long)dx * 4;

    memcpy(drow, srow, (unsigned long)cw * 4);
  }
}

static unsigned int
gfx_unpack_channel(unsigned int px, unsigned char field_pos)
{
  return (px >> field_pos) & 0xFF;
}

static unsigned char
lerp8(unsigned char a, unsigned char b, unsigned char alpha)
{
  return (unsigned char)(((unsigned int)a * (255 - alpha) + (unsigned int)b * alpha) / 255);
}

static void
clamp_rect(struct gfx_surface *s, int *x0, int *y0, int *x1, int *y1)
{
  if (*x0 < 0) *x0 = 0;
  if (*y0 < 0) *y0 = 0;
  if (*x1 > (int)s->w) *x1 = s->w;
  if (*y1 > (int)s->h) *y1 = s->h;
}

static int
clamp_radius(int r, int x0, int y0, int x1, int y1)
{
  if (r < 0) r = 0;
  if (2 * r > x1 - x0) r = (x1 - x0) / 2;
  if (2 * r > y1 - y0) r = (y1 - y0) / 2;
  return r;
}

// Corner regions (r x r squares at each of the 4 corners) get a circle
// test; everything else in the rect (including the cross-shaped
// middle) is unconditionally inside.
static int
rounded_rect_contains(int x, int y, int x0, int y0, int x1, int y1, int r)
{
  int cx, cy, dx, dy;

  if (x < x0 + r && y < y0 + r) { cx = x0 + r; cy = y0 + r; }
  else if (x >= x1 - r && y < y0 + r) { cx = x1 - r - 1; cy = y0 + r; }
  else if (x < x0 + r && y >= y1 - r) { cx = x0 + r; cy = y1 - r - 1; }
  else if (x >= x1 - r && y >= y1 - r) { cx = x1 - r - 1; cy = y1 - r - 1; }
  else return 1;
  dx = x - cx;
  dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

void
gfx_fill_rounded_rect(struct gfx_surface *s, int x0, int y0, int x1, int y1, int r, unsigned int color)
{
  unsigned int px = gfx_pack(s, color);
  int x, y;

  clamp_rect(s, &x0, &y0, &x1, &y1);
  r = clamp_radius(r, x0, y0, x1, y1);

  for (y = y0; y < y1; y++) {
    unsigned char *row = s->pixels + (unsigned long)y * s->pitch;

    for (x = x0; x < x1; x++)
      if (rounded_rect_contains(x, y, x0, y0, x1, y1, r))
        *(unsigned int *)(row + x * 4) = px;
  }
}

void
gfx_fill_rect_alpha(struct gfx_surface *s, int x0, int y0, int x1, int y1, unsigned int color, unsigned char alpha)
{
  unsigned int cr = (color >> 16) & 0xff, cg = (color >> 8) & 0xff, cb = color & 0xff;
  int x, y;

  clamp_rect(s, &x0, &y0, &x1, &y1);

  for (y = y0; y < y1; y++) {
    unsigned char *row = s->pixels + (unsigned long)y * s->pitch;

    for (x = x0; x < x1; x++) {
      unsigned int *p = (unsigned int *)(row + x * 4);
      unsigned int er = gfx_unpack_channel(*p, s->red_field_pos);
      unsigned int eg = gfx_unpack_channel(*p, s->green_field_pos);
      unsigned int eb = gfx_unpack_channel(*p, s->blue_field_pos);
      unsigned int nc = ((unsigned int)lerp8((unsigned char)er, (unsigned char)cr, alpha) << 16) |
                         ((unsigned int)lerp8((unsigned char)eg, (unsigned char)cg, alpha) << 8) |
                         lerp8((unsigned char)eb, (unsigned char)cb, alpha);
      *p = gfx_pack(s, nc);
    }
  }
}

void
gfx_fill_rounded_rect_alpha(struct gfx_surface *s, int x0, int y0, int x1, int y1, int r, unsigned int color, unsigned char alpha)
{
  unsigned int cr = (color >> 16) & 0xff, cg = (color >> 8) & 0xff, cb = color & 0xff;
  int x, y;

  clamp_rect(s, &x0, &y0, &x1, &y1);
  r = clamp_radius(r, x0, y0, x1, y1);

  for (y = y0; y < y1; y++) {
    unsigned char *row = s->pixels + (unsigned long)y * s->pitch;

    for (x = x0; x < x1; x++) {
      unsigned int *p, er, eg, eb, nc;

      if (!rounded_rect_contains(x, y, x0, y0, x1, y1, r))
        continue;
      p = (unsigned int *)(row + x * 4);
      er = gfx_unpack_channel(*p, s->red_field_pos);
      eg = gfx_unpack_channel(*p, s->green_field_pos);
      eb = gfx_unpack_channel(*p, s->blue_field_pos);
      nc = ((unsigned int)lerp8((unsigned char)er, (unsigned char)cr, alpha) << 16) |
           ((unsigned int)lerp8((unsigned char)eg, (unsigned char)cg, alpha) << 8) |
           lerp8((unsigned char)eb, (unsigned char)cb, alpha);
      *p = gfx_pack(s, nc);
    }
  }
}

void
gfx_fill_rect_gradient(struct gfx_surface *s, int x0, int y0, int x1, int y1, unsigned int color_top, unsigned int color_bottom)
{
  unsigned int tr = (color_top >> 16) & 0xff, tg = (color_top >> 8) & 0xff, tb = color_top & 0xff;
  unsigned int br = (color_bottom >> 16) & 0xff, bg = (color_bottom >> 8) & 0xff, bb = color_bottom & 0xff;
  int x, y, h;

  clamp_rect(s, &x0, &y0, &x1, &y1);
  h = y1 - y0;
  if (h <= 0)
    return;

  for (y = y0; y < y1; y++) {
    unsigned char alpha = (unsigned char)((long)(y - y0) * 255 / (h > 1 ? h - 1 : 1));
    unsigned int r = lerp8((unsigned char)tr, (unsigned char)br, alpha);
    unsigned int g = lerp8((unsigned char)tg, (unsigned char)bg, alpha);
    unsigned int b = lerp8((unsigned char)tb, (unsigned char)bb, alpha);
    unsigned int px = gfx_pack(s, (r << 16) | (g << 8) | b);
    unsigned char *row = s->pixels + (unsigned long)y * s->pitch;

    for (x = x0; x < x1; x++)
      *(unsigned int *)(row + x * 4) = px;
  }
}

// Sliding-window box blur, O(w*h) regardless of radius: the running
// sum only ever adds/removes the two pixels entering/leaving the
// window, with clamped-to-edge indices on both ends so the window
// size (2*radius+1) never actually shrinks near a border.
void
gfx_box_blur(struct gfx_surface *s, int radius)
{
  unsigned char *tmp;
  int x, y, w = (int)s->w, h = (int)s->h;
  int count = 2 * radius + 1;

  if (radius <= 0 || !s->pixels || w <= 0 || h <= 0)
    return;
  tmp = malloc((unsigned long)s->pitch * (unsigned long)h);
  if (!tmp)
    return;

  for (y = 0; y < h; y++) {
    unsigned char *srow = s->pixels + (unsigned long)y * s->pitch;
    unsigned char *drow = tmp + (unsigned long)y * s->pitch;
    long sr = 0, sg = 0, sb = 0;

    for (x = -radius; x <= radius; x++) {
      int sx = x < 0 ? 0 : (x >= w ? w - 1 : x);
      unsigned int px = *(unsigned int *)(srow + sx * 4);

      sr += gfx_unpack_channel(px, s->red_field_pos);
      sg += gfx_unpack_channel(px, s->green_field_pos);
      sb += gfx_unpack_channel(px, s->blue_field_pos);
    }
    for (x = 0; x < w; x++) {
      int old_x = x - radius, new_x = x + radius + 1;
      unsigned int old_px, new_px;

      *(unsigned int *)(drow + x * 4) = gfx_pack(s,
        ((unsigned int)(sr / count) << 16) | ((unsigned int)(sg / count) << 8) | (unsigned int)(sb / count));

      old_x = old_x < 0 ? 0 : old_x;
      new_x = new_x >= w ? w - 1 : new_x;
      old_px = *(unsigned int *)(srow + old_x * 4);
      new_px = *(unsigned int *)(srow + new_x * 4);
      sr += (long)gfx_unpack_channel(new_px, s->red_field_pos) - (long)gfx_unpack_channel(old_px, s->red_field_pos);
      sg += (long)gfx_unpack_channel(new_px, s->green_field_pos) - (long)gfx_unpack_channel(old_px, s->green_field_pos);
      sb += (long)gfx_unpack_channel(new_px, s->blue_field_pos) - (long)gfx_unpack_channel(old_px, s->blue_field_pos);
    }
  }

  for (x = 0; x < w; x++) {
    long sr = 0, sg = 0, sb = 0;
    int y2;

    for (y2 = -radius; y2 <= radius; y2++) {
      int sy = y2 < 0 ? 0 : (y2 >= h ? h - 1 : y2);
      unsigned int px = *(unsigned int *)(tmp + (unsigned long)sy * s->pitch + (unsigned long)x * 4);

      sr += gfx_unpack_channel(px, s->red_field_pos);
      sg += gfx_unpack_channel(px, s->green_field_pos);
      sb += gfx_unpack_channel(px, s->blue_field_pos);
    }
    for (y = 0; y < h; y++) {
      int old_y = y - radius, new_y = y + radius + 1;
      unsigned int old_px, new_px;

      *(unsigned int *)(s->pixels + (unsigned long)y * s->pitch + (unsigned long)x * 4) = gfx_pack(s,
        ((unsigned int)(sr / count) << 16) | ((unsigned int)(sg / count) << 8) | (unsigned int)(sb / count));

      old_y = old_y < 0 ? 0 : old_y;
      new_y = new_y >= h ? h - 1 : new_y;
      old_px = *(unsigned int *)(tmp + (unsigned long)old_y * s->pitch + (unsigned long)x * 4);
      new_px = *(unsigned int *)(tmp + (unsigned long)new_y * s->pitch + (unsigned long)x * 4);
      sr += (long)gfx_unpack_channel(new_px, s->red_field_pos) - (long)gfx_unpack_channel(old_px, s->red_field_pos);
      sg += (long)gfx_unpack_channel(new_px, s->green_field_pos) - (long)gfx_unpack_channel(old_px, s->green_field_pos);
      sb += (long)gfx_unpack_channel(new_px, s->blue_field_pos) - (long)gfx_unpack_channel(old_px, s->blue_field_pos);
    }
  }

  free(tmp);
}

// On-disk format (see tools/genraw.py): 4-byte magic, u32 width, u32
// height (all native-endian - both sides are the same x86 build), then
// width*height*3 (RGB) or *4 (RGBA) raw bytes, row-major, no padding.
// Raw open()/read() rather than stdio: libc.so here doesn't export
// buffered FILE* stdio (nothing else in this codebase uses it either -
// see e.g. gui/terminal.c's own pipe I/O), only the lower syscalls.
#define GFX_RAW_MAGIC  0x57415250u  /* "PRAW" */
#define GFX_RAWA_MAGIC 0x41475250u  /* "PRGA" */

static int
read_full(int fd, void *buf, unsigned long n)
{
  unsigned long got = 0;
  int r;

  while (got < n) {
    r = read(fd, (char *)buf + got, (int)(n - got));
    if (r <= 0)
      return 0;
    got += (unsigned long)r;
  }
  return 1;
}

int
gfx_load_raw(struct gfx_surface *out, const char *path, struct gfx_surface *fmt_ref)
{
  int fd;
  unsigned int hdr[3];
  unsigned long npix, i;
  unsigned char *rgb;

  out->pixels = 0;
  fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  if (!read_full(fd, hdr, sizeof(hdr)) || hdr[0] != GFX_RAW_MAGIC || hdr[1] == 0 || hdr[2] == 0) {
    close(fd);
    return 0;
  }
  npix = (unsigned long)hdr[1] * (unsigned long)hdr[2];
  rgb = malloc(npix * 3);
  if (!rgb || !read_full(fd, rgb, npix * 3)) {
    free(rgb);
    close(fd);
    return 0;
  }
  close(fd);

  out->w = hdr[1];
  out->h = hdr[2];
  out->pitch = out->w * 4;
  out->red_mask_size = fmt_ref->red_mask_size;
  out->red_field_pos = fmt_ref->red_field_pos;
  out->green_mask_size = fmt_ref->green_mask_size;
  out->green_field_pos = fmt_ref->green_field_pos;
  out->blue_mask_size = fmt_ref->blue_mask_size;
  out->blue_field_pos = fmt_ref->blue_field_pos;
  out->pixels = malloc((unsigned long)out->pitch * out->h);
  if (!out->pixels) {
    free(rgb);
    return 0;
  }

  for (i = 0; i < npix; i++) {
    unsigned int color = ((unsigned int)rgb[i * 3] << 16) | ((unsigned int)rgb[i * 3 + 1] << 8) | rgb[i * 3 + 2];

    *(unsigned int *)(out->pixels + i * 4) = gfx_pack(out, color);
  }
  free(rgb);
  return 1;
}

int
gfx_load_raw_rgba(struct gfx_image_rgba *out, const char *path)
{
  int fd;
  unsigned int hdr[3];
  unsigned long npix;

  out->pixels = 0;
  fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  if (!read_full(fd, hdr, sizeof(hdr)) || hdr[0] != GFX_RAWA_MAGIC || hdr[1] == 0 || hdr[2] == 0) {
    close(fd);
    return 0;
  }
  npix = (unsigned long)hdr[1] * (unsigned long)hdr[2];
  out->w = hdr[1];
  out->h = hdr[2];
  out->pixels = malloc(npix * 4);
  if (!out->pixels || !read_full(fd, out->pixels, npix * 4)) {
    free(out->pixels);
    out->pixels = 0;
    close(fd);
    return 0;
  }
  close(fd);
  return 1;
}

void
gfx_blit_alpha(struct gfx_surface *dst, int dx, int dy,
               struct gfx_image_rgba *src, int sx, int sy, int w, int h)
{
  int x, y, cw = w, ch = h;

  if (dx < 0) { sx -= dx; cw += dx; dx = 0; }
  if (dy < 0) { sy -= dy; ch += dy; dy = 0; }
  if (dx + cw > (int)dst->w) cw = (int)dst->w - dx;
  if (dy + ch > (int)dst->h) ch = (int)dst->h - dy;
  if (cw <= 0 || ch <= 0)
    return;

  for (y = 0; y < ch; y++) {
    unsigned char *srow = src->pixels + (unsigned long)(sy + y) * src->w * 4 + (unsigned long)sx * 4;
    unsigned char *drow = dst->pixels + (unsigned long)(dy + y) * dst->pitch + (unsigned long)dx * 4;

    for (x = 0; x < cw; x++) {
      unsigned char *sp = srow + x * 4;
      unsigned int a = sp[3];
      unsigned int *dp, er, eg, eb, nc;

      if (a == 0)
        continue;
      dp = (unsigned int *)(drow + x * 4);
      if (a == 255) {
        *dp = gfx_pack(dst, ((unsigned int)sp[0] << 16) | ((unsigned int)sp[1] << 8) | sp[2]);
        continue;
      }
      er = gfx_unpack_channel(*dp, dst->red_field_pos);
      eg = gfx_unpack_channel(*dp, dst->green_field_pos);
      eb = gfx_unpack_channel(*dp, dst->blue_field_pos);
      nc = ((unsigned int)lerp8((unsigned char)er, sp[0], (unsigned char)a) << 16) |
           ((unsigned int)lerp8((unsigned char)eg, sp[1], (unsigned char)a) << 8) |
           lerp8((unsigned char)eb, sp[2], (unsigned char)a);
      *dp = gfx_pack(dst, nc);
    }
  }
}
