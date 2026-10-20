/*
 * touch_overlay.c — an on-screen Genesis pad for touch screens (see
 * touch_overlay.h). Sizes follow the screen's height, so the controls keep
 * their size in any aspect (on a squarer or upright screen, a foldable's,
 * they shrink until the pad and the buttons fit side by side); positions hug
 * the corners, over the letterbox bars a 16:9 picture leaves on a wider phone.
 */
#include "touch_overlay.h"

#include <math.h>
#include <string.h>

/* Genesis pad bits (input_map.c k_btn_bit) */
enum { P_UP = 0x0001, P_DOWN = 0x0002, P_LEFT = 0x0004, P_RIGHT = 0x0008,
       P_A = 0x0040, P_B = 0x0010, P_C = 0x0020, P_START = 0x0080 };

#define MAX_FINGERS 10
enum { K_NONE, K_DPAD, K_BUTTONS, K_START };

typedef struct {
    SDL_FingerID id;
    int used, kind;
    float x, y;                 /* window pixels */
} Finger;

static Finger s_f[MAX_FINGERS];
static int s_visible = -1;      /* -1: not decided yet (on with a touch screen) */
static int s_start_pulse;       /* frames of Start from the Back button */
static int s_w = 1, s_h = 1;    /* the output, as last drawn */
static int s_enabled = 1;       /* the player's choice (touch_overlay_configure) */
static uint16_t s_tap_bits;     /* a press is held at least this long, so a tap */
static Uint32 s_tap_until;      /*  shorter than a frame still reaches the game */
static float s_scale = 1.0f;    /* the controls' size, 1 the standard */

void touch_overlay_configure(int enabled, float scale)
{
    s_enabled = enabled != 0;
    s_scale = scale > 0.3f && scale < 3.0f ? scale : 1.0f;
}

/* a size: a fraction of the screen's height, at the chosen scale (the
 * layout needs about 1.1 heights across: narrower, the width sets it) */
static float u(float f)
{
    float base = s_w < s_h * 1.25f ? s_w / 1.25f : (float)s_h;
    return f * base * s_scale;
}

/* ---- layout (pixels, from the output size) ----------------------------- */
typedef struct { float x, y, r; } Circle;

static Circle dpad(void)
{
    float r = u(0.19f);
    return (Circle){ r + u(0.07f), s_h - r - u(0.09f), r };
}

static Circle button(int i)    /* 0 A, 1 B, 2 C: a rising row, as on the pad */
{
    float r = u(0.085f);
    float x = s_w - u(0.12f) - (2 - i) * u(0.215f);
    float y = s_h - u(0.17f) - i * u(0.10f);
    return (Circle){ x, y, r };
}

static SDL_FRect start_rect(void)
{
    float w = u(0.20f), h = u(0.075f);
    return (SDL_FRect){ (s_w - w) * 0.5f, 0.025f * s_h, w, h };
}

static int in_start(float x, float y)
{
    SDL_FRect r = start_rect();
    float pad = u(0.04f);
    return x >= r.x - pad && x <= r.x + r.w + pad && y >= r.y - pad && y <= r.y + r.h + pad;
}

static float dist(float x, float y, Circle c) { return hypotf(x - c.x, y - c.y); }

static int kind_at(float x, float y)
{
    if (!s_enabled) return K_NONE;
    if (in_start(x, y)) return K_START;
    Circle d = dpad();
    if (x < s_w * 0.5f && dist(x, y, d) < d.r * 1.7f) return K_DPAD;
    for (int i = 0; i < 3; i++) {
        Circle b = button(i);
        if (dist(x, y, b) < b.r * 1.45f) return K_BUTTONS;
    }
    return K_NONE;
}

static uint16_t dpad_bits(float x, float y)
{
    Circle d = dpad();
    float dx = x - d.x, dy = y - d.y, len = hypotf(dx, dy);
    if (len < d.r * 0.22f) return 0;
    uint16_t m = 0;
    if (dx >  0.38f * len) m |= P_RIGHT;
    if (dx < -0.38f * len) m |= P_LEFT;
    if (dy >  0.38f * len) m |= P_DOWN;
    if (dy < -0.38f * len) m |= P_UP;
    return m;
}

static uint16_t button_bits(float x, float y)
{
    static const uint16_t bit[3] = { P_A, P_B, P_C };
    int best = -1;
    float bd = 0;
    for (int i = 0; i < 3; i++) {
        Circle b = button(i);
        float d = dist(x, y, b);
        if (d < b.r * 1.45f && (best < 0 || d < bd)) { best = i; bd = d; }
    }
    return best < 0 ? 0 : bit[best];
}

/* ---- input --------------------------------------------------------------- */
static Finger *finger(SDL_FingerID id, int make)
{
    for (int i = 0; i < MAX_FINGERS; i++)
        if (s_f[i].used && s_f[i].id == id) return &s_f[i];
    if (!make) return NULL;
    for (int i = 0; i < MAX_FINGERS; i++)
        if (!s_f[i].used) { memset(&s_f[i], 0, sizeof s_f[i]); s_f[i].used = 1; s_f[i].id = id; return &s_f[i]; }
    return NULL;
}

void touch_overlay_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP: {
        float x = e->tfinger.x * s_w, y = e->tfinger.y * s_h;
        s_visible = 1;
        if (e->type == SDL_FINGERUP) {
            Finger *f = finger(e->tfinger.fingerId, 0);
            if (f) f->used = 0;
            break;
        }
        Finger *f = finger(e->tfinger.fingerId, e->type == SDL_FINGERDOWN);
        if (!f) break;
        if (e->type == SDL_FINGERDOWN) {
            f->kind = kind_at(x, y);
            uint16_t b = f->kind == K_DPAD ? dpad_bits(x, y) : f->kind == K_BUTTONS ? button_bits(x, y)
                       : f->kind == K_START ? P_START : 0;
            if (b) { s_tap_bits |= b; s_tap_until = SDL_GetTicks() + 60; }
        }
        else if (f->kind == K_BUTTONS || f->kind == K_NONE) {   /* (slide onto, between buttons) */
            int k = kind_at(x, y);
            f->kind = k == K_DPAD ? f->kind : k;
        }
        f->x = x; f->y = y;
        break;
    }
    case SDL_KEYDOWN:
        if (e->key.keysym.scancode == SDL_SCANCODE_AC_BACK) { s_start_pulse = 6; break; }
        s_visible = 0;
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        s_visible = 0;
        break;
    default:
        break;
    }
}

uint16_t touch_overlay_mask(void)
{
    uint16_t m = s_start_pulse > 0 ? P_START : 0;
    if (s_tap_bits) {
        if (SDL_TICKS_PASSED(SDL_GetTicks(), s_tap_until)) s_tap_bits = 0;
        else m |= s_tap_bits;
    }
    for (int i = 0; i < MAX_FINGERS; i++) {
        const Finger *f = &s_f[i];
        if (!f->used) continue;
        if (f->kind == K_DPAD) m |= dpad_bits(f->x, f->y);
        else if (f->kind == K_BUTTONS) m |= button_bits(f->x, f->y);
        else if (f->kind == K_START) m |= P_START;
    }
    return m;
}

/* ---- drawing --------------------------------------------------------------- */
static void disc(SDL_Renderer *r, Circle c, SDL_Color col)
{
    enum { N = 40 };
    SDL_Vertex v[N + 1];
    int idx[N * 3];
    v[0] = (SDL_Vertex){ { c.x, c.y }, col, { 0, 0 } };
    for (int i = 0; i < N; i++) {
        float a = (float)i * 6.2831853f / N;
        v[i + 1] = (SDL_Vertex){ { c.x + cosf(a) * c.r, c.y + sinf(a) * c.r }, col, { 0, 0 } };
        idx[i * 3] = 0; idx[i * 3 + 1] = i + 1; idx[i * 3 + 2] = (i + 1) % N + 1;
    }
    SDL_RenderGeometry(r, NULL, v, N + 1, idx, N * 3);
}

static void ring(SDL_Renderer *r, Circle c, float w, SDL_Color col)
{
    enum { N = 40 };
    SDL_Vertex v[N * 2];
    int idx[N * 6];
    for (int i = 0; i < N; i++) {
        float a = (float)i * 6.2831853f / N, ca = cosf(a), sa = sinf(a);
        v[i * 2]     = (SDL_Vertex){ { c.x + ca * c.r, c.y + sa * c.r }, col, { 0, 0 } };
        v[i * 2 + 1] = (SDL_Vertex){ { c.x + ca * (c.r - w), c.y + sa * (c.r - w) }, col, { 0, 0 } };
        int j = (i + 1) % N;
        int q[6] = { i * 2, i * 2 + 1, j * 2, j * 2, i * 2 + 1, j * 2 + 1 };
        memcpy(&idx[i * 6], q, sizeof q);
    }
    SDL_RenderGeometry(r, NULL, v, N * 2, idx, N * 6);
}

static void tri(SDL_Renderer *r, float x0, float y0, float x1, float y1, float x2, float y2, SDL_Color col)
{
    SDL_Vertex v[3] = { { { x0, y0 }, col, { 0, 0 } }, { { x1, y1 }, col, { 0, 0 } }, { { x2, y2 }, col, { 0, 0 } } };
    SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

/* 5x7 letters for the button faces */
static void glyph(SDL_Renderer *r, char ch, float cx, float cy, float px, SDL_Color col)
{
    static const char *const A[7] = { ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" };
    static const char *const B[7] = { "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####." };
    static const char *const C[7] = { ".####", "#....", "#....", "#....", "#....", "#....", ".####" };
    const char *const *g = ch == 'A' ? A : ch == 'B' ? B : C;
    SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 5; x++)
            if (g[y][x] == '#') {
                SDL_FRect q = { cx + (x - 2.5f) * px, cy + (y - 3.5f) * px, px, px };
                SDL_RenderFillRectF(r, &q);
            }
}

void touch_overlay_draw(SDL_Renderer *r)
{
    if (s_start_pulse > 0) s_start_pulse--;
    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(r, &w, &h) != 0 || w <= 0 || h <= 0) return;
    s_w = w; s_h = h;
    if (s_visible < 0) s_visible = SDL_GetNumTouchDevices() > 0;
    if (!s_visible || !s_enabled) return;

    /* draw in output pixels, then put the game's mapping back */
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(r, &lw, &lh);
    SDL_Rect vp;
    SDL_RenderGetViewport(r, &vp);
    float sx = 1, sy = 1;
    SDL_RenderGetScale(r, &sx, &sy);
    SDL_BlendMode bm;
    SDL_GetRenderDrawBlendMode(r, &bm);
    Uint8 cr, cg, cb, ca;                             /* (the next frame's clear uses it) */
    SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
    SDL_RenderSetLogicalSize(r, 0, 0);
    SDL_RenderSetViewport(r, NULL);
    SDL_RenderSetScale(r, 1, 1);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    uint16_t held = touch_overlay_mask();
    /* dark glass with white marks: they read over bright and dark scenes alike */
    SDL_Color base = { 0, 0, 0, 70 }, edge = { 255, 255, 255, 150 }, lit = { 255, 255, 255, 110 };
    SDL_Color mark = { 255, 255, 255, 215 };

    Circle d = dpad();
    disc(r, d, base);
    ring(r, d, u(0.012f), edge);
    float a = d.r * 0.62f, t = d.r * 0.2f;            /* the four arrows, lit when held */
    tri(r, d.x, d.y - a - t, d.x - t, d.y - a + t * 0.6f, d.x + t, d.y - a + t * 0.6f, held & P_UP ? lit : mark);
    tri(r, d.x, d.y + a + t, d.x + t, d.y + a - t * 0.6f, d.x - t, d.y + a - t * 0.6f, held & P_DOWN ? lit : mark);
    tri(r, d.x - a - t, d.y, d.x - a + t * 0.6f, d.y + t, d.x - a + t * 0.6f, d.y - t, held & P_LEFT ? lit : mark);
    tri(r, d.x + a + t, d.y, d.x + a - t * 0.6f, d.y - t, d.x + a - t * 0.6f, d.y + t, held & P_RIGHT ? lit : mark);

    static const uint16_t bit[3] = { P_A, P_B, P_C };
    for (int i = 0; i < 3; i++) {
        Circle b = button(i);
        disc(r, b, held & bit[i] ? lit : base);
        ring(r, b, u(0.010f), edge);
        glyph(r, "ABC"[i], b.x, b.y, b.r * 0.13f, mark);
    }

    SDL_FRect s = start_rect();                       /* Start: a pill with a pause sign */
    float rr = s.h * 0.5f;
    disc(r, (Circle){ s.x + rr, s.y + rr, rr }, held & P_START ? lit : base);
    disc(r, (Circle){ s.x + s.w - rr, s.y + rr, rr }, held & P_START ? lit : base);
    SDL_Color fill = held & P_START ? lit : base;
    SDL_SetRenderDrawColor(r, fill.r, fill.g, fill.b, fill.a);
    SDL_FRect mid = { s.x + rr, s.y, s.w - 2 * rr, s.h };
    SDL_RenderFillRectF(r, &mid);
    SDL_SetRenderDrawColor(r, mark.r, mark.g, mark.b, mark.a);
    SDL_FRect b1 = { s.x + s.w * 0.5f - s.h * 0.28f, s.y + s.h * 0.25f, s.h * 0.16f, s.h * 0.5f };
    SDL_FRect b2 = { s.x + s.w * 0.5f + s.h * 0.12f, s.y + s.h * 0.25f, s.h * 0.16f, s.h * 0.5f };
    SDL_RenderFillRectF(r, &b1);
    SDL_RenderFillRectF(r, &b2);

    SDL_SetRenderDrawBlendMode(r, bm);
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    if (lw > 0 && lh > 0) SDL_RenderSetLogicalSize(r, lw, lh);
    else { SDL_RenderSetViewport(r, &vp); SDL_RenderSetScale(r, sx, sy); }
}
