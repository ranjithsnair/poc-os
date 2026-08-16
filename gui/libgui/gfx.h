// gfx: minimal software rasterizer shared by every GUI roadmap phase
// 7+ client and the compositor itself - generalizes the ad hoc
// pack()/fill_rect() bash/poc/guitest.c already proved (phase 5) into
// something reusable against any pixel buffer, not just the real
// framebuffer. Always assumes 32bpp/4-bytes-per-pixel, same
// simplification guitest.c already made (VBE phase 2 only ever picks
// a 32bpp mode).
#ifndef GUI_GFX_H
#define GUI_GFX_H

struct gfx_surface {
  unsigned char *pixels;
  unsigned int w, h, pitch;
  // Mirrors include/fb.h's struct fb_info field-by-field: a client
  // surface is created with the *compositor's real framebuffer's*
  // layout (sent back in GUI_MSG_SURFACE_CREATED), so gfx_blit() can
  // be a raw per-scanline memcpy rather than a per-pixel format
  // conversion.
  unsigned char red_mask_size, red_field_pos;
  unsigned char green_mask_size, green_field_pos;
  unsigned char blue_mask_size, blue_field_pos;
};

// color is 0xRRGGBB regardless of the surface's actual field layout -
// gfx_pack() converts once at draw time, same as guitest.c's pack().
unsigned int gfx_pack(struct gfx_surface *s, unsigned int color);
void gfx_fill_rect(struct gfx_surface *s, int x0, int y0, int x1, int y1, unsigned int color);
// Copies src's pixels (already in dst's own layout - see the struct
// comment above) into dst at (dx,dy), clipped to dst's bounds.
void gfx_blit(struct gfx_surface *dst, int dx, int dy,
              struct gfx_surface *src, int sx, int sy, int w, int h);

void gfx_draw_glyph(struct gfx_surface *s, int x, int y, char ch, unsigned int color);
void gfx_draw_string(struct gfx_surface *s, int x, int y, const char *str, unsigned int color);

#endif
