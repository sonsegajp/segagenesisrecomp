/*
 * crt_present.h — CRT presentation filters (opt-in, present-only).
 *
 * Included once by runner/main.c. The verified/hashed frame is untouched:
 * this only changes how the uploaded frame texture reaches the window. All
 * work is done by the SDL renderer on the GPU:
 *
 *   1. the frame is drawn into a render target the size of the picture on
 *      screen (physical pixels), optionally with a soft glow (a blurred, small
 *      copy added on top);
 *   2. a scanline texture and a phosphor-mask texture, generated for that exact
 *      size and the frame's line count, multiply it;
 *   3. the target reaches the window flat, or through a barrel-distorted mesh
 *      (SDL_RenderGeometry) for the curved presets, then a vignette.
 *
 * Presets (genesis_present_set_crt, or GENESIS_CRT=name for any game):
 *   0 off, 1 scanlines, 2 PVM (sharp lines, aperture grille), 3 arcade (slot
 *   mask, glow, slight curve), 4 TV (soft, shadow mask, glow, curve, vignette).
 * The effects fade out at small window sizes, where they would only alias.
 */
#ifndef CRT_PRESENT_H
#define CRT_PRESENT_H

#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { CRT_OFF, CRT_SCANLINES, CRT_PVM, CRT_ARCADE, CRT_TV, CRT_COUNT };

typedef struct {
    float scan;        /* scanline gap depth (0..1) */
    float scan_sharp;  /* profile exponent: higher = thinner gaps */
    int   mask;        /* 0 none, 1 aperture grille, 2 slot mask, 3 shadow mask */
    float mask_depth;  /* how much the off channels are dimmed */
    float glow;        /* added blurred light (0..1) */
    float boost;       /* added sharp light to pay back the darkening */
    float curve;       /* barrel distortion */
    float vignette;    /* corner darkening */
    int   soft;        /* linear filtering of the frame */
} CrtPreset;

static const CrtPreset k_crt_presets[CRT_COUNT] = {
    { 0 },
    { 0.45f, 2.0f, 0, 0.00f, 0.00f, 0.30f, 0.000f, 0.00f, 0 },   /* scanlines */
    { 0.60f, 3.0f, 1, 0.30f, 0.10f, 0.45f, 0.000f, 0.00f, 0 },   /* PVM */
    { 0.50f, 2.0f, 2, 0.35f, 0.22f, 0.35f, 0.035f, 0.18f, 0 },   /* arcade */
    { 0.35f, 1.5f, 3, 0.30f, 0.28f, 0.25f, 0.070f, 0.38f, 1 },   /* TV */
};

static int s_crt = CRT_OFF;
static int s_crt_env = -2;                 /* GENESIS_CRT: -1 unset, else the preset it forces */
static SDL_Texture *s_crt_target, *s_crt_small, *s_crt_scan, *s_crt_mask, *s_crt_vig;
static int s_crt_w, s_crt_h, s_crt_lines, s_crt_built = -1;

static void crt_env(void)
{
    if (s_crt_env != -2) return;
    s_crt_env = -1;
    const char *e = getenv("GENESIS_CRT");
    if (!e || !*e) return;
    static const char *const names[CRT_COUNT] = { "off", "scanlines", "pvm", "arcade", "tv" };
    for (int i = 0; i < CRT_COUNT; i++)
        if (!SDL_strcasecmp(e, names[i])) s_crt_env = i;
    if (s_crt_env >= 0) s_crt = s_crt_env;
}

/* The game's setting (GENESIS_CRT, when given, overrides it). */
void genesis_present_set_crt(int preset)
{
    crt_env();
    if (s_crt_env >= 0) return;
    s_crt = (preset >= 0 && preset < CRT_COUNT) ? preset : CRT_OFF;
}

static void crt_free(void)
{
    SDL_Texture **t[5] = { &s_crt_target, &s_crt_small, &s_crt_scan, &s_crt_mask, &s_crt_vig };
    for (int i = 0; i < 5; i++)
        if (*t[i]) { SDL_DestroyTexture(*t[i]); *t[i] = NULL; }
    s_crt_w = s_crt_h = 0;
    s_crt_built = -1;
}

static float crt_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static SDL_Texture *crt_static(SDL_Renderer *r, const uint32_t *px, int w, int h, SDL_BlendMode mode)
{
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, mode);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
    return t;
}

/* scanlines, mask and vignette for a w x h picture showing `lines` lines */
static void crt_build(SDL_Renderer *r, const CrtPreset *p, int w, int h, int lines)
{
    if (s_crt_scan) { SDL_DestroyTexture(s_crt_scan); s_crt_scan = NULL; }
    if (s_crt_mask) { SDL_DestroyTexture(s_crt_mask); s_crt_mask = NULL; }
    if (s_crt_vig) { SDL_DestroyTexture(s_crt_vig); s_crt_vig = NULL; }
    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!px) return;
    float scale = (float)h / (float)(lines > 0 ? lines : 224);
    float scan = p->scan * crt_clampf((scale - 1.5f) / 1.5f, 0.0f, 1.0f);       /* none below 1.5x */
    float mdep = p->mask_depth * crt_clampf((scale - 2.0f) / 2.0f, 0.0f, 1.0f); /* none below 2x */
    if (scan > 0.0f) {
        for (int y = 0; y < h; y++) {
            float s = ((float)y + 0.5f) * (float)lines / (float)h;
            float f = s - floorf(s);
            float d = fabsf(2.0f * f - 1.0f);                                     /* 0 mid-line, 1 between lines */
            float b = 1.0f - scan * powf(d, p->scan_sharp);
            uint32_t v = (uint32_t)(crt_clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            uint32_t c = 0xFF000000u | v << 16 | v << 8 | v;
            for (int x = 0; x < w; x++) px[(size_t)y * w + x] = c;
        }
        s_crt_scan = crt_static(r, px, w, h, SDL_BLENDMODE_MOD);
    }
    if (p->mask && mdep > 0.0f) {
        uint32_t off = (uint32_t)((1.0f - mdep) * 255.0f + 0.5f);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int ch, dark = 0;
                if (p->mask == 1) ch = x % 3;                                        /* aperture grille */
                else if (p->mask == 2) {                                             /* slot mask */
                    ch = x % 3;
                    int cell = (x / 3) & 1;
                    dark = ((y + cell * 2) % 4) == 3;
                } else ch = (x + (y & 1) * 2) % 3;                                   /* shadow mask (staggered dots) */
                uint32_t rr = ch == 0 ? 255 : off, gg = ch == 1 ? 255 : off, bb = ch == 2 ? 255 : off;
                if (dark) { rr = rr * off / 255; gg = gg * off / 255; bb = bb * off / 255; }
                px[(size_t)y * w + x] = 0xFF000000u | rr << 16 | gg << 8 | bb;
            }
        s_crt_mask = crt_static(r, px, w, h, SDL_BLENDMODE_MOD);
    }
    if (p->vignette > 0.0f) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float u = ((float)x + 0.5f) / (float)w * 2.0f - 1.0f, v = ((float)y + 0.5f) / (float)h * 2.0f - 1.0f;
                float k = 1.0f - p->vignette * powf(crt_clampf((u * u * 0.6f + v * v * 0.6f), 0.0f, 1.0f), 1.5f);
                uint32_t c = (uint32_t)(crt_clampf(k, 0.0f, 1.0f) * 255.0f + 0.5f);
                px[(size_t)y * w + x] = 0xFF000000u | c << 16 | c << 8 | c;
            }
        s_crt_vig = crt_static(r, px, w, h, SDL_BLENDMODE_MOD);
    }
    free(px);
}

/* The picture's size on screen, in physical pixels (the logical canvas, scaled). */
static void crt_phys_size(SDL_Renderer *r, int *w, int *h, int *lw, int *lh)
{
    float sx = 1.0f, sy = 1.0f;
    SDL_RenderGetLogicalSize(r, lw, lh);
    if (*lw <= 0 || *lh <= 0) {
        SDL_GetRendererOutputSize(r, lw, lh);
    } else {
        SDL_RenderGetScale(r, &sx, &sy);
    }
    *w = (int)((float)*lw * sx + 0.5f);
    *h = (int)((float)*lh * sy + 0.5f);
}

/* Barrel-distorted draw of the target over the logical canvas. */
static void crt_draw_curved(SDL_Renderer *r, SDL_Texture *t, int lw, int lh, float k)
{
    enum { GX = 32, GY = 24 };
    static SDL_Vertex v[(GX + 1) * (GY + 1)];
    static int idx[GX * GY * 6];
    for (int j = 0; j <= GY; j++)
        for (int i = 0; i <= GX; i++) {
            float u = (float)i / GX, w = (float)j / GY;
            float x = u * 2.0f - 1.0f, y = w * 2.0f - 1.0f;
            float d = 1.0f + k * (x * x + y * y);                                 /* edges pulled in: a bulging tube */
            SDL_Vertex *p = &v[j * (GX + 1) + i];
            p->position.x = ((x / d) * 0.5f + 0.5f) * (float)lw;
            p->position.y = ((y / d) * 0.5f + 0.5f) * (float)lh;
            p->color.r = p->color.g = p->color.b = p->color.a = 255;
            p->tex_coord.x = u;
            p->tex_coord.y = w;
        }
    int n = 0;
    for (int j = 0; j < GY; j++)
        for (int i = 0; i < GX; i++) {
            int a = j * (GX + 1) + i, b = a + 1, c = a + GX + 1, e = c + 1;
            idx[n++] = a; idx[n++] = b; idx[n++] = c;
            idx[n++] = b; idx[n++] = e; idx[n++] = c;
        }
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    SDL_RenderGeometry(r, t, v, (GX + 1) * (GY + 1), idx, n);
}

/* ---- Vsync (genesis_present_set_vsync, or GENESIS_VSYNC=auto|on|off) -----
 * The runner paces frames itself (frame_pacer.h). Vsync on top only helps a
 * display running near the game's rate (alignment, no tearing). On a
 * high-refresh panel the pacer already lands every frame, and some drivers
 * block a vsync present far longer than one refresh: D3D11 windowed on an
 * Intel iGPU at 144 Hz took ~26 ms per present, holding the game near 32 fps
 * (60.0 fps with vsync off). Auto keeps vsync only near the game's rate,
 * re-checked every two seconds so it follows the window between displays. */
static int s_vsync_mode, s_vsync_env = -2, s_vsync_applied = -1, s_vsync_last_mode = -1;
static unsigned s_vsync_tick;
void genesis_present_set_vsync(int mode) { s_vsync_mode = mode >= 0 && mode <= 2 ? mode : 0; }

static void vsync_apply(SDL_Renderer *r, double target_fps)
{
    if (s_vsync_env == -2) {
        const char *e = getenv("GENESIS_VSYNC");
        s_vsync_env = !e || !*e ? -1 : !SDL_strcasecmp(e, "on") ? 1 : !SDL_strcasecmp(e, "off") ? 2 : 0;
    }
    int mode = s_vsync_env >= 0 ? s_vsync_env : s_vsync_mode;
    if (mode == s_vsync_last_mode && (s_vsync_tick++ % 120)) return;
    s_vsync_last_mode = mode;
    int want = mode == 1;
    if (mode == 0) {
        SDL_DisplayMode dm;
        SDL_Window *w = SDL_RenderGetWindow(r);
        int d = w ? SDL_GetWindowDisplayIndex(w) : -1;
        want = d >= 0 && SDL_GetCurrentDisplayMode(d, &dm) == 0 && dm.refresh_rate > 0 &&
               fabs((double)dm.refresh_rate - target_fps) < 2.0;
    }
    if (want != s_vsync_applied && SDL_RenderSetVSync(r, want) == 0) {
        s_vsync_applied = want;
        fprintf(stderr, "[VIDEO] vsync %s\n", want ? "on" : "off (frame pacer only)");
    }
}

/* ---- Frame interpolation (genesis_present_set_interpolation) -------------
 * 0 off; otherwise a display rate in Hz to fill (120, 144...), or -1 for the
 * display's own refresh. The game draws in-between frames
 * (GameVideo.interpolate, from the previous tick to the latest: one tick of
 * latency); each 60 Hz tick presents n of them, n = rate / tick rate (at most
 * 4), fewer when the measured draw time would not fit the tick. */
static int s_interp_hz;
static double s_interp_cost_ms = 4.0;     /* running estimate of one in-between draw */
void genesis_present_set_interpolation(int hz) { s_interp_hz = hz; }

static int interp_slots(SDL_Renderer *r, double target_fps, double work_ms)
{
    if (!s_interp_hz || target_fps <= 0) return 1;
    int hz = s_interp_hz;
    if (hz < 0) {
        SDL_DisplayMode dm;
        SDL_Window *w = SDL_RenderGetWindow(r);
        int d = w ? SDL_GetWindowDisplayIndex(w) : -1;
        hz = d >= 0 && SDL_GetCurrentDisplayMode(d, &dm) == 0 && dm.refresh_rate > 0 ? dm.refresh_rate : 60;
    }
    int n = (int)((double)hz / target_fps + 0.5);
    if (n > 4) n = 4;
    double budget = 1000.0 / target_fps - work_ms - 1.5;
    int want = n;
    while (n > 1 && n * s_interp_cost_ms > budget) n--;
    /* Backed off, nothing re-measures the draw: let the estimate decay so a
     * one-off slow draw (a disk hiccup) can't switch interpolation off for good. */
    if (n < want) s_interp_cost_ms *= 0.97;
    return n < 1 ? 1 : n;
}

/* ---- Scaling (genesis_present_set_scaling / _integer_scale) -------------
 * A game may choose how the frame reaches the window, live:
 *   0 sharp  — nearest neighbour (blocky, uneven pixel widths off-integer);
 *   1 smooth — bilinear (soft);
 *   2 crisp  — nearest to the largest whole multiple that fits, then bilinear
 *              for the remainder: even, sharp pixels without shimmer.
 * -1 (the default) leaves settings.ini's linear_filter in charge. Integer
 * scaling snaps the picture to whole multiples (letterboxed). */
static int s_scale_mode = -1, s_integer_scale;
static SDL_Texture *s_crisp;
static int s_crisp_w, s_crisp_h;
void genesis_present_set_scaling(int mode) { s_scale_mode = mode >= 0 && mode <= 2 ? mode : -1; }
void genesis_present_set_integer_scale(int on) { s_integer_scale = on != 0; }
/* The filter the frame texture should use (-1: no opinion). */
static int scale_wants_linear(void) { return s_scale_mode < 0 ? -1 : s_scale_mode == 1; }

static void scale_apply(SDL_Renderer *r)
{
    if (SDL_RenderGetIntegerScale(r) != (s_integer_scale ? SDL_TRUE : SDL_FALSE))
        SDL_RenderSetIntegerScale(r, s_integer_scale ? SDL_TRUE : SDL_FALSE);
}

/* Crisp: 0 = not handled (draw as usual). */
static int crisp_present(SDL_Renderer *r, SDL_Texture *tex, const SDL_Rect *src)
{
    if (s_scale_mode != 2) {
        if (s_crisp) { SDL_DestroyTexture(s_crisp); s_crisp = NULL; s_crisp_w = s_crisp_h = 0; }
        return 0;
    }
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(r, &info) != 0 || !(info.flags & SDL_RENDERER_TARGETTEXTURE)) return 0;
    int w, h, lw, lh;
    crt_phys_size(r, &w, &h, &lw, &lh);
    if (src->w <= 0 || src->h <= 0 || lw <= 0 || lh <= 0) return 0;
    /* the frame covers the logical canvas: its physical size per source pixel */
    int kx = w / src->w, ky = h / src->h;
    if (kx < 1) kx = 1;
    if (ky < 1) ky = 1;
    int tw = src->w * kx, th = src->h * ky;
    if (tw > 16384 || th > 16384) return 0;
    if (!s_crisp || tw != s_crisp_w || th != s_crisp_h) {
        if (s_crisp) SDL_DestroyTexture(s_crisp);
        s_crisp = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, tw, th);
        if (!s_crisp) { s_crisp_w = s_crisp_h = 0; return 0; }
        s_crisp_w = tw;
        s_crisp_h = th;
    }
    SDL_Texture *old = SDL_GetRenderTarget(r);
    SDL_BlendMode mode = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(tex, &mode);
    SDL_ScaleMode sm = SDL_ScaleModeNearest;
    SDL_GetTextureScaleMode(tex, &sm);
    SDL_SetRenderTarget(r, s_crisp);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    SDL_RenderCopy(r, tex, src, NULL);
    SDL_SetTextureScaleMode(tex, sm);
    SDL_SetTextureBlendMode(tex, mode);
    SDL_SetRenderTarget(r, old);
    SDL_SetTextureBlendMode(s_crisp, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(s_crisp, SDL_ScaleModeLinear);
    SDL_RenderCopy(r, s_crisp, NULL, NULL);
    return 1;
}

/* Present `src` of `tex` through the active preset. 0 = not handled (draw as usual). */
static int crt_present(SDL_Renderer *r, SDL_Texture *tex, const SDL_Rect *src, int filtered)
{
    crt_env();
    if (s_crt <= CRT_OFF || s_crt >= CRT_COUNT) {
        if (s_crt_target) crt_free();
        return 0;
    }
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(r, &info) != 0 || !(info.flags & SDL_RENDERER_TARGETTEXTURE)) return 0;
    const CrtPreset *p = &k_crt_presets[s_crt];
    int w, h, lw, lh;
    crt_phys_size(r, &w, &h, &lw, &lh);
    if (w < 16 || h < 16) return 0;
    if (w != s_crt_w || h != s_crt_h) {
        crt_free();
        s_crt_target = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        s_crt_small = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                        w / 6 > 8 ? w / 6 : 8, h / 6 > 8 ? h / 6 : 8);
        if (!s_crt_target || !s_crt_small) { crt_free(); return 0; }
        s_crt_w = w;
        s_crt_h = h;
    }
    if (s_crt_built != s_crt || s_crt_lines != src->h) {
        crt_build(r, p, w, h, src->h);
        s_crt_built = s_crt;
        s_crt_lines = src->h;
    }

    SDL_Texture *old = SDL_GetRenderTarget(r);
    SDL_BlendMode tex_mode = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(tex, &tex_mode);
    SDL_SetTextureScaleMode(tex, p->soft ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    if (p->glow > 0.0f) {                                                          /* the blurred copy */
        SDL_SetRenderTarget(r, s_crt_small);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
        SDL_RenderCopy(r, tex, src, NULL);
        SDL_SetTextureScaleMode(tex, p->soft ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    }
    SDL_SetRenderTarget(r, s_crt_target);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(r, tex, src, NULL);
    if (p->boost > 0.0f) {                                                         /* light back for the gaps */
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaMod(tex, (Uint8)(p->boost * 255.0f));
        SDL_RenderCopy(r, tex, src, NULL);
        SDL_SetTextureAlphaMod(tex, 255);
    }
    if (s_crt_scan) SDL_RenderCopy(r, s_crt_scan, NULL, NULL);
    if (s_crt_mask) SDL_RenderCopy(r, s_crt_mask, NULL, NULL);
    if (p->glow > 0.0f) {
        SDL_SetTextureScaleMode(s_crt_small, SDL_ScaleModeLinear);
        SDL_SetTextureBlendMode(s_crt_small, SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaMod(s_crt_small, (Uint8)(p->glow * 255.0f));
        SDL_RenderCopy(r, s_crt_small, NULL, NULL);
    }
    SDL_SetTextureBlendMode(tex, tex_mode);
    SDL_SetTextureScaleMode(tex, filtered ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_SetRenderTarget(r, old);

    SDL_SetTextureBlendMode(s_crt_target, SDL_BLENDMODE_NONE);
    if (p->curve > 0.0f) crt_draw_curved(r, s_crt_target, lw, lh, p->curve);
    else {
        SDL_SetTextureScaleMode(s_crt_target, SDL_ScaleModeNearest);
        SDL_RenderCopy(r, s_crt_target, NULL, NULL);
    }
    if (s_crt_vig) SDL_RenderCopy(r, s_crt_vig, NULL, NULL);
    return 1;
}

#endif
