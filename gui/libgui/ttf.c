#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// stb_truetype needs a handful of libm entry points (sqrt/floor/ceil/
// fabs/pow/fmod/cos/acos) that this build's libc.so doesn't export
// (only malloc/free/memcpy/strlen do - see gui/libgui/gfx.c's own
// comment on why raw open()/read() replaced stdio for the same
// reason). Overriding STBTT_* before the #include avoids <math.h>
// entirely rather than hitting the same undefined-reference failure.
// ifloor/iceil/fabs/sqrt are real, precision-bearing implementations
// (all reachable from the ordinary antialiased glyph-bitmap path this
// wrapper actually calls); pow/fmod/cos/acos are only ever reached by
// stb_truetype's signed-distance-field code path (stbtt_GetGlyphSDF
// and friends), which ttf_draw_string() below never calls - crude
// placeholders are fine there, they just need to compile and link.
static int stbtt__ifloor(float x) { int i = (int)x; return (x < (float)i) ? i - 1 : i; }
static int stbtt__iceil(float x)  { int i = (int)x; return (x > (float)i) ? i + 1 : i; }
static float stbtt__fabs(float x) { return x < 0 ? -x : x; }
static float
stbtt__sqrt(float x)
{
  float g;
  int i;

  if (x <= 0)
    return 0;
  g = x > 1 ? x : 1;
  for (i = 0; i < 24; i++)
    g = 0.5f * (g + x / g);
  return g;
}
static float stbtt__pow(float x, float y) { (void)y; return x; }         // SDF-only, unused
static float stbtt__fmod(float x, float y) { int q = (int)(x / y); return x - (float)q * y; }  // SDF-only, unused
static float stbtt__cos(float x) { float x2 = x * x; return 1.0f - x2 / 2.0f + x2 * x2 / 24.0f; }  // SDF-only, unused
static float stbtt__acos(float x) { (void)x; return 0; }                 // SDF-only, unused

#define STBTT_ifloor stbtt__ifloor
#define STBTT_iceil  stbtt__iceil
#define STBTT_fabs   stbtt__fabs
#define STBTT_sqrt   stbtt__sqrt
#define STBTT_pow    stbtt__pow
#define STBTT_fmod   stbtt__fmod
#define STBTT_cos    stbtt__cos
#define STBTT_acos   stbtt__acos
#define STBTT_malloc(x,u) ((void)(u), malloc(x))
#define STBTT_free(x,u)   ((void)(u), free(x))
#define STBTT_assert(x)   ((void)0)
#define STBTT_strlen(x)   strlen(x)
#define STBTT_memcpy      memcpy
#define STBTT_memset      memset

#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"

#include "gfx.h"
#include "ttf.h"

struct glyph_entry {
  int codepoint;
  int size_px;
  int w, h, xoff, yoff;   // stbtt_GetCodepointBitmapBox, plus advance below
  int advance;
  unsigned char *coverage;  // w*h bytes, 0-255 alpha, or NULL if glyph is blank (e.g. space)
};

struct ttf_font {
  unsigned char *filedata;
  stbtt_fontinfo info;
  struct glyph_entry *cache;
  int cache_n, cache_cap;
};

static int
read_whole_file(const char *path, unsigned char **out, long *out_len)
{
  int fd;
  struct stat st;
  unsigned char *buf;
  long got = 0;
  int r;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  if (fstat(fd, &st) < 0 || st.st_size <= 0) {
    close(fd);
    return 0;
  }
  buf = malloc((unsigned long)st.st_size);
  if (!buf) {
    close(fd);
    return 0;
  }
  while (got < st.st_size) {
    r = read(fd, buf + got, (int)(st.st_size - got));
    if (r <= 0) {
      free(buf);
      close(fd);
      return 0;
    }
    got += r;
  }
  close(fd);
  *out = buf;
  *out_len = st.st_size;
  return 1;
}

struct ttf_font *
ttf_load(const char *path)
{
  struct ttf_font *f;
  unsigned char *data;
  long len;

  if (!read_whole_file(path, &data, &len))
    return 0;

  f = malloc(sizeof(*f));
  if (!f) {
    free(data);
    return 0;
  }
  memset(f, 0, sizeof(*f));
  f->filedata = data;
  if (!stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
    free(data);
    free(f);
    return 0;
  }
  return f;
}

static struct glyph_entry *
get_glyph(struct ttf_font *f, int codepoint, int size_px)
{
  int i, adv, lsb, x0, y0, x1, y1, w, h;
  float scale;
  struct glyph_entry *e;

  for (i = 0; i < f->cache_n; i++)
    if (f->cache[i].codepoint == codepoint && f->cache[i].size_px == size_px)
      return &f->cache[i];

  if (f->cache_n == f->cache_cap) {
    int newcap = f->cache_cap ? f->cache_cap * 2 : 64;
    struct glyph_entry *newcache = realloc(f->cache, (unsigned long)newcap * sizeof(*newcache));

    if (!newcache)
      return 0;
    f->cache = newcache;
    f->cache_cap = newcap;
  }

  scale = stbtt_ScaleForPixelHeight(&f->info, (float)size_px);
  stbtt_GetCodepointHMetrics(&f->info, codepoint, &adv, &lsb);
  stbtt_GetCodepointBitmapBox(&f->info, codepoint, scale, scale, &x0, &y0, &x1, &y1);
  w = x1 - x0;
  h = y1 - y0;

  e = &f->cache[f->cache_n++];
  e->codepoint = codepoint;
  e->size_px = size_px;
  e->w = w;
  e->h = h;
  e->xoff = x0;
  e->yoff = y0;
  e->advance = (int)(adv * scale + 0.5f);
  e->coverage = 0;
  if (w > 0 && h > 0) {
    // A small slack margin past the exact w*h stb_truetype is told to
    // fill: an intermittent, timing-sensitive heap-corruption crash
    // was traced to somewhere in this rasterization step (found via
    // trace prints bisecting gui/terminal.c - a heap-metadata-corrupting
    // overflow whose actual crash happens much later, at whatever
    // malloc() call next touches the corrupted free-list, hence the
    // "sometimes reproduces, sometimes doesn't" symptom depending on
    // unrelated allocation timing). Root cause not pinned down exactly
    // (either this vendored rasterizer or this file's own STBTT_ifloor/
    // iceil overrides, both plausible - see this file's top comment),
    // but this padding reliably absorbs it.
    e->coverage = malloc((unsigned long)w * (unsigned long)h + 64);
    if (e->coverage)
      stbtt_MakeCodepointBitmap(&f->info, e->coverage, w, h, w, scale, scale, codepoint);
  }
  return e;
}

static int
font_ascent_px(struct ttf_font *f, int size_px)
{
  int ascent, descent, linegap;
  float scale = stbtt_ScaleForPixelHeight(&f->info, (float)size_px);

  stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &linegap);
  return (int)(ascent * scale + 0.5f);
}

int
ttf_line_height(struct ttf_font *f, int size_px)
{
  int ascent, descent, linegap;
  float scale;

  if (!f)
    return 0;
  scale = stbtt_ScaleForPixelHeight(&f->info, (float)size_px);
  stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &linegap);
  return (int)((ascent - descent + linegap) * scale + 0.5f);
}

int
ttf_string_width(struct ttf_font *f, const char *str, int size_px)
{
  int width = 0;
  const unsigned char *p;
  struct glyph_entry *g;

  if (!f)
    return 0;
  for (p = (const unsigned char *)str; *p; p++) {
    g = get_glyph(f, *p, size_px);
    if (g)
      width += g->advance;
  }
  return width;
}

static void
blend_coverage(struct gfx_surface *s, int px, int py, unsigned char cov, unsigned int color)
{
  unsigned int *dp;
  unsigned int er, eg, eb, cr, cg, cb, nr, ng, nb;

  if (px < 0 || py < 0 || px >= (int)s->w || py >= (int)s->h || cov == 0)
    return;
  dp = (unsigned int *)(s->pixels + (unsigned long)py * s->pitch + (unsigned long)px * 4);
  er = (*dp >> s->red_field_pos) & 0xFF;
  eg = (*dp >> s->green_field_pos) & 0xFF;
  eb = (*dp >> s->blue_field_pos) & 0xFF;
  cr = (color >> 16) & 0xFF;
  cg = (color >> 8) & 0xFF;
  cb = color & 0xFF;
  nr = (er * (255 - cov) + cr * cov) / 255;
  ng = (eg * (255 - cov) + cg * cov) / 255;
  nb = (eb * (255 - cov) + cb * cov) / 255;
  *dp = gfx_pack(s, (nr << 16) | (ng << 8) | nb);
}

void
ttf_draw_string(struct gfx_surface *s, struct ttf_font *f, int x, int y,
                 const char *str, int size_px, unsigned int color)
{
  const unsigned char *p;
  int pen_x = x;
  int baseline_y;
  struct glyph_entry *g;
  int gx, gy;

  if (!f || !str)
    return;
  baseline_y = y + font_ascent_px(f, size_px);
  for (p = (const unsigned char *)str; *p; p++) {
    g = get_glyph(f, *p, size_px);
    if (!g)
      continue;
    if (g->coverage) {
      for (gy = 0; gy < g->h; gy++)
        for (gx = 0; gx < g->w; gx++)
          blend_coverage(s, pen_x + g->xoff + gx, baseline_y + g->yoff + gy,
                          g->coverage[gy * g->w + gx], color);
    }
    pen_x += g->advance;
  }
}

void
ttf_draw_string_shadow(struct gfx_surface *s, struct ttf_font *f, int x, int y,
                        const char *str, int size_px, unsigned int color,
                        unsigned int shadow_color)
{
  ttf_draw_string(s, f, x + 1, y + 1, str, size_px, shadow_color);
  ttf_draw_string(s, f, x, y, str, size_px, color);
}
