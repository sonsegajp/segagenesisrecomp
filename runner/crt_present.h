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
