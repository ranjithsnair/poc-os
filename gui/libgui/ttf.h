// ttf: thin antialiased TrueType text wrapper around vendor/
// stb_truetype.h (public domain, github.com/nothings/stb) - real glyph
// outline rasterization, not this codebase's original 8x16 bitmap font
// (gui/font8x16.h/gui/libgui/font.c, retired by this rewrite). Used by
// all three GUI roadmap ToaruOS-style screens (login, desktop, terminal)
// via gui/assets/fonts/*.ttf (copied straight from ToaruOS's own DejaVu
// fonts, see tools/genraw.py's sibling asset-install Makefile rules).
#ifndef GUI_TTF_H
#define GUI_TTF_H

#include "gfx.h"

struct ttf_font;

// Loads a TrueType font file into a freshly malloc'd struct ttf_font.
// Returns NULL on failure (missing file, bad/corrupt font data).
struct ttf_font *ttf_load(const char *path);

// Draws str with its top-left at (x,y) - not the baseline, matching
// gfx_draw_string()'s existing convention - at size_px pixels tall, in
// color, antialiased into s. Like gfx_fill_rect_alpha, this rasterizer
// has no true per-pixel destination alpha: glyph coverage blends
// straight into whatever's already at each pixel, so the caller must
// have already arranged for that to be the right background.
void ttf_draw_string(struct gfx_surface *s, struct ttf_font *f, int x, int y,
                      const char *str, int size_px, unsigned int color);

// Same as ttf_draw_string, but first draws a 1px-down-right dark copy
// behind it - the cheap "shadow" trick ToaruOS's tt_draw_string_shadow
// (apps/glogin-provider.c) uses for text sitting directly over a
// photographic background (wallpaper, desktop icon labels).
void ttf_draw_string_shadow(struct gfx_surface *s, struct ttf_font *f, int x, int y,
                             const char *str, int size_px, unsigned int color,
                             unsigned int shadow_color);

// Rendered width of str at size_px, for centering/right-alignment.
int ttf_string_width(struct ttf_font *f, const char *str, int size_px);

// Recommended line height (ascent+descent+lineGap) at size_px, for
// vertical centering or laying out a fixed cell grid (the terminal).
int ttf_line_height(struct ttf_font *f, int size_px);

#endif
