#include <string.h>
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
