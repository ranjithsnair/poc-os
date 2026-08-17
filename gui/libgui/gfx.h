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

// Rounded rectangle: same fill semantics as gfx_fill_rect, with the
// four corners of radius r clipped by a circle test.
void gfx_fill_rounded_rect(struct gfx_surface *s, int x0, int y0, int x1, int y1, int r, unsigned int color);

// Constant-alpha blend of `color` into the rect's *existing* pixels at
// the given alpha (0-255) - not a real per-pixel alpha channel (this
// rasterizer doesn't carry one), just a flat blend against whatever's
// already there. Used for translucent overlays (the login glass box,
// dimmed backgrounds) where the caller has already arranged for
// "what's already there" to be the right thing to blend against.
void gfx_fill_rect_alpha(struct gfx_surface *s, int x0, int y0, int x1, int y1, unsigned int color, unsigned char alpha);
void gfx_fill_rounded_rect_alpha(struct gfx_surface *s, int x0, int y0, int x1, int y1, int r, unsigned int color, unsigned char alpha);

// Top-to-bottom 2-stop linear gradient fill - ToaruOS's accent-blue
// highlight look (lib/menu.c's HILIGHT_GRADIENT_TOP/BOTTOM).
void gfx_fill_rect_gradient(struct gfx_surface *s, int x0, int y0, int x1, int y1, unsigned int color_top, unsigned int color_bottom);

// In-place horizontal+vertical box blur, `radius` pixels each pass -
// the same cheap trick ToaruOS's lib/graphics.c blur_context_box()
// uses to fake a Gaussian blur behind the login box. Call once on a
// scratch copy, not per-frame.
void gfx_box_blur(struct gfx_surface *s, int radius);

// Loads a tools/genraw.py-generated raw RGB asset (see its own comment
// for the on-disk format) into a freshly malloc'd surface, packed to
// match fmt_ref's pixel field layout (so gfx_blit() between the two
// stays a raw memcpy - same reasoning as this header's own top
// comment). Returns 0 with out->pixels left 0 on any failure (missing
// file, bad magic, ...): callers fall back to a flat color rather than
// treating this as fatal. Caller frees out->pixels when done.
int gfx_load_raw(struct gfx_surface *out, const char *path, struct gfx_surface *fmt_ref);

// A loaded RGBA image (tools/genraw.py's icon assets) - plain
// interleaved bytes, not packed to any native pixel format, since
// gfx_blit_alpha() below converts per-pixel at blit time instead.
struct gfx_image_rgba {
  unsigned char *pixels; // R,G,B,A bytes, row-major, no row padding
  unsigned int w, h;
};

// Same load contract as gfx_load_raw() above (0/pixels==0 on failure).
int gfx_load_raw_rgba(struct gfx_image_rgba *out, const char *path);

// Per-pixel alpha-composites src onto dst at (dx,dy), clipped to dst's
// bounds - the one place this rasterizer does real per-pixel alpha
// (icons over a wallpaper crop), as opposed to gfx_fill_rect_alpha's
// constant-alpha approximation above.
void gfx_blit_alpha(struct gfx_surface *dst, int dx, int dy,
                     struct gfx_image_rgba *src, int sx, int sy, int w, int h);

#endif
