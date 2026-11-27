// procedural textures for the world: every surface is painted here at startup into one texture array.
// (from Out on Parole's world.c; the city-only layers are gone)
#include "city.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
typedef struct { void *(*fn)(void *); void *arg; void *h; } Thread;
__declspec(dllimport) void *__stdcall CreateThread(void *, size_t, unsigned long (__stdcall *)(void *), void *, unsigned long, unsigned long *);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *, unsigned long);
static unsigned long __stdcall thread_main(void *p) { Thread *t = p; t->fn(t->arg); return 0; }
static void thread_start(Thread *t, void *(*fn)(void *), void *arg) { t->fn = fn; t->arg = arg; t->h = CreateThread(0, 0, thread_main, t, 0, 0); }
static void thread_join(Thread *t) { WaitForSingleObject(t->h, 0xFFFFFFFF); }
#elif defined(PLATFORM_WEB)
typedef struct { void *(*fn)(void *); void *arg; } Thread;
static void thread_start(Thread *t, void *(*fn)(void *), void *arg) { t->fn = fn; t->arg = arg; }
static void thread_join(Thread *t) { t->fn(t->arg); }
#else
#include <pthread.h>
typedef pthread_t Thread;
static void thread_start(Thread *t, void *(*fn)(void *), void *arg) { pthread_create(t, NULL, fn, arg); }
static void thread_join(Thread *t) { pthread_join(*t, NULL); }
#endif

GLuint world_tex, sig_tex;
Vector2 layer_cells[L_COUNT];
static int TS = 512;
static float sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float sm(float a, float b, float x) { float t = sat((x - a) / (b - a)); return t * t * (3 - 2 * t); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }

// ---------------------------------------------------------------- procedural textures


static uint32_t hh(int x, int y, int s)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float hf(int x, int y, int s) { return (hh(x, y, s) >> 8) * (1.0f / 16777216.0f); }

static float pnoise(float x, float y, int per, int s)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    int x0 = ((xi % per) + per) % per, y0 = ((yi % per) + per) % per, x1 = (x0 + 1) % per, y1 = (y0 + 1) % per;
    return mixf(mixf(hf(x0, y0, s), hf(x1, y0, s), fx), mixf(hf(x0, y1, s), hf(x1, y1, s), fx), fy);
}

static float NT[512 * 512];

static void noise_init(void)
{
    for (int y = 0; y < TS; y++)
        for (int x = 0; x < TS; x++) {
            float u = x / (float)TS, v = y / (float)TS, a = 0.5f, t = 0;
            int per = 8;
            for (int o = 0; o < 6; o++) { t += a * pnoise(u * per, v * per, per, 11 + o); per *= 2; a *= 0.5f; }
            NT[y * TS + x] = sat((t / 0.984f - 0.5f) * 1.7f + 0.5f);
        }
}

// tileable value noise, ku/kv repeats per tile, o = offset in tiles
static float tn(float u, float v, int ku, int kv, float ox, float oy)
{
    float x = (u * ku + ox) * TS, y = (v * kv + oy) * TS;
    float fx = x - floorf(x), fy = y - floorf(y);
    int x0 = ((int)floorf(x)) & (TS - 1), y0 = ((int)floorf(y)) & (TS - 1), x1 = (x0 + 1) & (TS - 1), y1 = (y0 + 1) & (TS - 1);
    return mixf(mixf(NT[y0 * TS + x0], NT[y0 * TS + x1], fx), mixf(NT[y1 * TS + x0], NT[y1 * TS + x1], fx), fy);
}

typedef void (*Paint)(float u, float v, int ix, int iy, float *c);
#define RGB(r, g, b) (c[0] = (r), c[1] = (g), c[2] = (b))

static void p_detail(float u, float v, int ix, int iy, float *c)
{
    float n = tn(u, v, 1, 1, 0, 0) * 0.35f + tn(u, v, 4, 4, .3f, .1f) * 0.4f + hf(ix >> 1, iy >> 1, 3) * 0.25f;
    float g = 0.5f + (n - 0.5f) * 1.0f + (hf(ix, iy, 4) - 0.5f) * 0.14f;
    RGB(g, g, g);
}

static void p_asphalt(float u, float v, int ix, int iy, float *c)
{
    float n = tn(u, v, 2, 2, 0, 0), m = tn(u, v, 8, 8, .2f, .5f), g = hf(ix, iy, 2), a = hf(ix >> 1, iy >> 1, 3);
    float b = 0.20f + 0.06f * (n - 0.5f) + 0.04f * (m - 0.5f) + 0.05f * (g - 0.5f) + (a > 0.93f ? 0.07f : 0);
    float cr = fabsf(tn(u, v, 1, 1, .4f, .1f) - 0.5f);
    if (cr < 0.006f) b *= 0.5f + cr * 80;
    float cr2 = fabsf(tn(u, v, 3, 3, .8f, .6f) - 0.5f);
    if (cr2 < 0.004f) b *= 0.55f + cr2 * 100;
    if (u > .18f && u < .48f && v > .30f && v < .43f) { b = b * 0.8f + 0.01f; if (u < .185f || u > .475f || v < .305f || v > .425f) b *= 0.6f; }
    float o = tn(u, v, 4, 4, .1f, .9f);
    if (o > 0.8f) b *= mixf(1, .78f, sm(.8f, .88f, o));
    RGB(b * 0.98f, b, b * 1.04f);
}

static void p_sidewalk(float u, float v, int ix, int iy, float *c)
{
    float fu = u * 2, fv = v * 2;
    int si = (int)fu, sj = (int)fv;
    float a = fu - si, b = fv - sj;
    float g = 0.62f + (hf(si, sj, 5) - 0.5f) * 0.07f + (tn(u, v, 4, 4, .3f, .2f) - 0.5f) * 0.08f + (hf(ix, iy, 6) - 0.5f) * 0.05f;
    float j = fminf(fminf(a, 1 - a), fminf(b, 1 - b));
    if (j < 0.011f) g *= 0.6f; else if (j < 0.02f) g *= 0.9f;
    RGB(g, g * 0.99f, g * 0.95f);
}

static void p_curb(float u, float v, int ix, int iy, float *c)
{
    float g = 0.66f + (tn(u, v, 4, 4, .1f, .6f) - 0.5f) * 0.1f + (hf(ix, iy, 7) - 0.5f) * 0.05f;
    float fu = u * 2 - floorf(u * 2);
    if (fu < 0.006f || fu > 0.994f) g *= 0.6f;
    RGB(g, g, g * 0.97f);
}

static void p_concrete(float u, float v, int ix, int iy, float *c)
{
    float g = 0.64f + (tn(u, v, 4, 4, 0, 0) - 0.5f) * 0.12f + (hf(ix, iy, 8) - 0.5f) * 0.05f;
    g -= (tn(u, v, 8, 1, .5f, .2f) - 0.5f) * 0.14f * sm(0.3f, 0.7f, tn(u, v, 2, 2, .3f, .3f));
    float fv = v * 4 - floorf(v * 4), fu = u * 4 - floorf(u * 4);
    if (fv < 0.008f) g *= 0.72f;
    if (fu < 0.004f) g *= 0.85f;
    float dx = fu - 0.5f, dy = fv - 0.5f;
    if (dx * dx + dy * dy < 0.00015f) g *= 0.45f;
    RGB(g, g * 0.99f, g * 0.97f);
}

static void p_panel(float u, float v, int ix, int iy, float *c)
{
    float fu = u * 2 - floorf(u * 2), fv = v * 2 - floorf(v * 2);
    int pi = (int)(u * 2), pj = (int)(v * 2);
    float g = 0.70f + (hf(pi, pj, 9) - 0.5f) * 0.08f + (tn(u, v, 2, 2, .6f, .1f) - 0.5f) * 0.07f + (hf(ix, iy, 10) - 0.5f) * 0.04f;
    float j = fminf(fminf(fu, 1 - fu), fminf(fv, 1 - fv));
    if (j < 0.008f) g *= 0.55f;
    RGB(g, g, g * 0.98f);
}

static void p_brick(float u, float v, int ix, int iy, float *c)
{
    float fu = u * 5, fv = v * 13;
    int row = (int)fv;
    fu += (row & 1) ? 0.5f : 0;
    int col = (int)floorf(fu);
    float a = fu - col, b = fv - row;
    float h = hf(col, row, 12);
    bool mortar = a > 0.95f || b > 0.87f;
    if (mortar) {
        float g = 0.58f + (hf(ix, iy, 13) - 0.5f) * 0.1f;
        RGB(g, g * 0.97f, g * 0.9f);
    } else {
        float edge = fminf(fminf(a, 0.95f - a) * 6, fminf(b, 0.87f - b) * 8);
        float k = 0.8f + 0.4f * h + (tn(u, v, 4, 4, .1f, .4f) - 0.5f) * 0.2f + (hf(ix, iy, 14) - 0.5f) * 0.12f;
        k *= 0.78f + 0.22f * sat(edge);
        RGB(0.58f * k, 0.25f * k, 0.18f * k);
    }
}

static void p_stucco(float u, float v, int ix, int iy, float *c)
{
    float g = 0.9f + (tn(u, v, 2, 2, .2f, .2f) - 0.5f) * 0.07f + (hf(ix >> 1, iy >> 1, 15) - 0.5f) * 0.06f + (hf(ix, iy, 16) - 0.5f) * 0.04f;
    RGB(g, g * 0.97f, g * 0.9f);
}

static void p_snow(float u, float v, int ix, int iy, float *c)
{
    float n = tn(u, v, 4, 4, .3f, .6f), m = tn(u, v, 1, 1, .1f, .2f), g = hf(ix, iy, 61);
    float k = 0.86f + 0.08f * n + 0.05f * (m - 0.5f) + 0.04f * (g - 0.5f);
    if (hf(ix >> 1, iy >> 1, 62) > 0.992f) k += 0.08f;
    RGB(k * 0.97f, k * 0.98f, k);
}

static void p_grass(float u, float v, int ix, int iy, float *c)
{
    float n = tn(u, v, 4, 4, 0, 0), m = tn(u, v, 2, 2, .5f, .5f), h = hf(ix, iy, 16), h2 = hf(ix >> 2, iy >> 1, 19);
    float k = 0.75f + 0.4f * n + 0.3f * (m - 0.5f) + 0.3f * (h - 0.5f) + 0.2f * (h2 - 0.5f);
    RGB(0.23f * k, 0.30f * k, 0.13f * k);
    if (tn(u, v, 3, 3, .2f, .8f) > 0.7f) { c[0] += 0.05f; c[1] += 0.03f; }
}

static void p_sand(float u, float v, int ix, int iy, float *c)
{
    float rip = sinf((v + tn(u, v, 2, 2, 0, 0) * 0.3f) * 6.2832f * 14) * 0.5f + 0.5f;
    float k = 0.92f + 0.12f * (tn(u, v, 4, 4, .1f, .1f) - 0.5f) + 0.05f * rip * tn(u, v, 1, 1, .5f, .5f) + 0.1f * (hf(ix, iy, 17) - 0.5f);
    if (hf(ix >> 1, iy >> 1, 18) > 0.985f) k *= 0.7f;
    RGB(0.66f * k, 0.55f * k, 0.38f * k);
}

static void p_gravel(float u, float v, int ix, int iy, float *c)
{
    float h = hf(ix >> 1, iy >> 1, 19);
    float g = 0.27f + 0.2f * h + (tn(u, v, 4, 4, .7f, .7f) - 0.5f) * 0.1f;
    RGB(g, g * 0.98f, g * 0.95f);
}

static void p_tile(float u, float v, int ix, int iy, float *c)
{
    float fu = u * 5, fv = v * 4;
    int row = (int)fv;
    fu += (row & 1) ? 0.5f : 0;
    int col = (int)floorf(fu);
    float a = fu - col, b = fv - row;
    float h = hf(col, row, 20);
    float curve = 0.62f + 0.38f * sinf(a * 3.1416f);
    float lip = b < 0.14f ? 0.55f + b * 3 : 1;
    float k = curve * lip * (0.82f + 0.3f * h) * (0.9f + 0.2f * tn(u, v, 8, 8, .2f, .9f));
    RGB(0.6f * k, 0.32f * k, 0.22f * k);
}

static void p_shingle(float u, float v, int ix, int iy, float *c)
{
    float fv = v * 6;
    int row = (int)fv;
    float b = fv - row, fu = u * 6 + ((row & 1) ? 0.5f : 0);
    int col = (int)floorf(fu);
    float a = fu - col;
    float h = hf(col, row, 21);
    float k = (0.55f + 0.5f * h) * (b < 0.1f ? 0.5f + b * 5 : 1) * (a < 0.03f || a > 0.97f ? 0.7f : 1) + (hf(ix, iy, 22) - 0.5f) * 0.1f;
    RGB(0.5f * k, 0.47f * k, 0.44f * k);
}

static void p_wood(float u, float v, int ix, int iy, float *c)
{
    float fu = u * 14;
    int pl = (int)fu;
    float a = fu - pl, h = hf(pl, 0, 23);
    float grain = tn(u * 0 + v, v * 0 + u, 1, 1, h, 0) * 0.0f;
    (void)grain;
    float g = tn(v, u * 0.2f + h, 6, 1, h * 3, 0) * 0.5f + tn(v, u, 40, 1, h, h) * 0.5f;
    float k = (0.7f + 0.5f * h) * (0.75f + 0.5f * g);
    if (a < 0.05f || a > 0.95f) k *= 0.45f;
    RGB(0.5f * k, 0.38f * k, 0.27f * k);
}

static void rib(float u, float v, int ix, int iy, float *c, float pitch, float r, float g, float b)
{
    float ph = u * pitch;
    float s = sinf(ph * 6.2832f);
    float k = 0.82f + 0.2f * s + (tn(u, v, 4, 4, .3f, .3f) - 0.5f) * 0.15f;
    k *= 1 - 0.35f * sm(0.5f, 1.0f, tn(u, v, 6, 1, .4f, .1f)) * (1 - v);
    k += (hf(ix, iy, 24) - 0.5f) * 0.04f;
    RGB(r * k, g * k, b * k);
}
static void p_metal(float u, float v, int ix, int iy, float *c) { rib(u, v, ix, iy, c, 20, 0.66f, 0.69f, 0.71f); }
static void p_container(float u, float v, int ix, int iy, float *c)
{
    rib(u, v, ix, iy, c, 14, 0.9f, 0.9f, 0.9f);
    if (tn(u, v, 5, 5, .6f, .2f) > 0.72f) { c[0] *= 0.8f; c[1] *= 0.7f; c[2] *= 0.6f; }
    if (v < 0.03f || v > 0.97f) { c[0] *= 0.6f; c[1] *= 0.6f; c[2] *= 0.6f; }
}

static void p_plain(float u, float v, int ix, int iy, float *c)
{
    float g = 0.97f + (hf(ix, iy, 25) - 0.5f) * 0.04f;
    RGB(g, g, g);
}

static void p_quay(float u, float v, int ix, int iy, float *c)
{
    float fu = u * 2, fv = v * 2;
    int si = (int)fu, sj = (int)fv;
    float a = fu - si, b = fv - sj;
    float g = 0.42f + (hf(si, sj, 26) - 0.5f) * 0.06f + (tn(u, v, 3, 3, .9f, .1f) - 0.5f) * 0.12f + (hf(ix, iy, 27) - 0.5f) * 0.05f;
    if (fminf(fminf(a, 1 - a), fminf(b, 1 - b)) < 0.008f) g *= 0.55f;
    if (tn(u, v, 2, 2, .2f, .6f) > 0.7f) g *= 0.75f;
    RGB(g, g * 1.0f, g * 1.02f);
}

// windows. a cell is one window bay and floor, v runs up.
typedef struct { float x0, x1, y0, y1, frame; float fr, fg, fb; } WS;

static void glass_paint(float fu, float fv, float h, const WS *w, float *c)
{
    float gx0 = w->x0 + w->frame, gx1 = w->x1 - w->frame, gy0 = w->y0 + w->frame, gy1 = w->y1 - w->frame * 0.8f;
    if (fu < gx0 || fu > gx1 || fv < gy0 || fv > gy1) {
        float e = fminf(fminf(fu - w->x0, w->x1 - fu), fminf(fv - w->y0, w->y1 - fv)) / w->frame;
        float k = 0.85f + 0.15f * sat(e);
        RGB(w->fr * k, w->fg * k, w->fb * k);
        c[3] = 0;
        return;
    }
    float t = (fv - gy0) / (gy1 - gy0), s = (fu - gx0) / (gx1 - gx0);
    float r = mixf(0.07f, 0.20f, t * t), g = mixf(0.10f, 0.28f, t * t), b = mixf(0.15f, 0.38f, t * t);
    float streak = sm(0.0f, 0.3f, 1 - fabsf(s * 1.3f + t * 0.6f - 0.9f) * 2.2f) * 0.12f;
    r += streak; g += streak; b += streak;
    if (h < 0.3f) {
        float drop = 0.3f + 0.7f * hf((int)(h * 1000), 3, 31);
        if (t > 1 - drop) {
            float slat = fmodf((1 - t) / 0.06f, 1.0f);
            float k = slat < 0.75f ? 0.85f : 0.55f;
            r = 0.82f * k; g = 0.78f * k; b = 0.68f * k;
        }
    } else if (h < 0.55f) {
        float cw = 0.28f + 0.1f * hf((int)(h * 1000), 4, 32);
        if (s < cw || s > 1 - cw) {
            float f = 0.8f + 0.2f * sinf(s * 60);
            float hue = hf((int)(h * 1000), 5, 33);
            r = (0.5f + 0.4f * hue) * f; g = (0.4f + 0.2f * hue) * f; b = (0.35f + 0.3f * (1 - hue)) * f;
        }
    }
    float shade = (1 - 0.45f * sm(0.12f, 0.0f, t > 0.88f ? 1 - t : 1.0f)) * (1 - 0.4f * sm(0.1f, 0.0f, s));
    RGB(r * shade, g * shade, b * shade);
    c[3] = 1;
}

static void window_cell(float u, float v, int nx, int ny, const WS *w, float *c)
{
    float fu = u * nx, fv = v * ny;
    int ci = (int)floorf(fu), cj = (int)floorf(fv);
    fu -= ci; fv -= cj;
    float h = hf(ci, cj, 30);
    if (fu >= w->x0 && fu <= w->x1 && fv >= w->y0 && fv <= w->y1) glass_paint(fu, fv, h, w, c);
    else {
        c[3] = 0;
        if (fu > w->x0 - 0.03f && fu < w->x1 + 0.03f && fv > w->y0 - 0.045f && fv <= w->y0) { c[0] *= 1.08f; c[1] *= 1.08f; c[2] *= 1.08f; }
        else if (fu > w->x0 - 0.02f && fu < w->x1 + 0.02f && fv > w->y1 && fv < w->y1 + 0.03f) { c[0] *= 0.82f; c[1] *= 0.82f; c[2] *= 0.82f; }
    }
}

static void p_win_stucco(float u, float v, int ix, int iy, float *c)
{
    p_stucco(u, v, ix, iy, c);
    WS w = { 0.31f, 0.69f, 0.2f, 0.78f, 0.028f, 0.93f, 0.93f, 0.9f };
    window_cell(u, v, 2, 2, &w, c);
}

static void p_win_brick(float u, float v, int ix, int iy, float *c)
{
    p_brick(u * 3.6f - floorf(u * 3.6f), v * 3.4f - floorf(v * 3.4f), ix, iy, c);
    WS w = { 0.33f, 0.67f, 0.2f, 0.74f, 0.03f, 0.2f, 0.17f, 0.15f };
    float cc[4] = { c[0], c[1], c[2], 0 };
    window_cell(u, v, 2, 2, &w, cc);
    c[0] = cc[0]; c[1] = cc[1]; c[2] = cc[2]; c[3] = cc[3];
    float fu = u * 2 - floorf(u * 2), fv = v * 2 - floorf(v * 2);
    if (fu > 0.29f && fu < 0.71f && fv > 0.74f && fv < 0.79f) { c[0] = 0.62f; c[1] = 0.6f; c[2] = 0.56f; c[3] = 0; }
    if (fu > 0.28f && fu < 0.72f && fv > 0.15f && fv < 0.2f) { c[0] = 0.6f; c[1] = 0.58f; c[2] = 0.54f; c[3] = 0; }
}

static void p_win_conc(float u, float v, int ix, int iy, float *c)
{
    p_panel(u * 2 - floorf(u * 2), v, ix, iy, c);
    c[0] *= 0.96f; c[1] *= 0.96f; c[2] *= 0.97f;
    WS w = { 0.015f, 0.985f, 0.3f, 0.74f, 0.025f, 0.35f, 0.35f, 0.37f };
    window_cell(u, v, 2, 1, &w, c);
    float fv = v - floorf(v);
    if (fv > 0.0f && fv < 0.06f) { c[0] *= 0.8f; c[1] *= 0.8f; c[2] *= 0.8f; }
}

static void p_win_glass(float u, float v, int ix, int iy, float *c)
{
    float fu = u * 3 - floorf(u * 3), fv = v;
    int pane = (int)(u * 3);
    float h = hf(pane, 0, 34), hy = hf((int)(v * 4), pane, 35);
    c[3] = 0;
    if (fu < 0.025f || fu > 0.975f || fv > 0.975f) { RGB(0.5f, 0.52f, 0.55f); return; }
    if (fv < 0.26f) {
        float k = 0.55f + 0.1f * tn(u, v, 4, 4, 0, 0);
        RGB(0.13f * k, 0.15f * k, 0.18f * k);
        return;
    }
    float t = (fv - 0.26f) / 0.71f;
    float k = 0.75f + 0.5f * tn(u, v, 6, 3, h, 0) + 0.2f * (hy - 0.5f);
    RGB(mixf(0.22f, 0.5f, t) * k, mixf(0.35f, 0.66f, t) * k, mixf(0.45f, 0.8f, t) * k);
    c[3] = 1;
}

static void p_storefront(float u, float v, int ix, int iy, float *c)
{
    float fu = u * 2 - floorf(u * 2), fv = v;
    c[3] = 0;
    float wall = 0.82f + (tn(u, v, 4, 4, .4f, .2f) - 0.5f) * 0.1f;
    RGB(wall, wall * 0.97f, wall * 0.9f);
    if (fu < 0.045f || fu > 0.955f) { RGB(0.72f, 0.7f, 0.66f); return; }
    if (fv < 0.14f) {
        float k = 0.3f + (tn(u, v, 6, 6, .1f, .3f) - 0.5f) * 0.1f;
        RGB(k, k * 0.95f, k * 0.9f);
        if (fv > 0.125f) RGB(0.55f, 0.52f, 0.5f);
        return;
    }
    if (fv < 0.76f) {
        bool mull = fabsf(fu - 0.5f) < 0.012f || fv < 0.16f || fv > 0.745f;
        if (mull) { RGB(0.12f, 0.12f, 0.13f); return; }
        float t = (fv - 0.16f) / 0.58f;
        float shelf = 0.5f + 0.5f * sinf(t * 31 + fu * 7);
        float k = 0.10f + 0.07f * shelf * (1 - t) + 0.25f * sm(0.85f, 1.0f, t) + 0.08f * tn(u, v, 8, 4, .6f, .6f);
        float sky = 0.05f * sm(0.0f, 1.0f, 1 - fabsf(fu * 1.4f + t * 0.8f - 1.0f) * 2);
        RGB(k + sky, k * 0.95f + sky, k * 0.9f + sky * 1.2f);
        c[3] = 1;
        return;
    }
    if (fv > 0.78f && fv < 0.985f) { RGB(0.88f, 0.86f, 0.8f); return; }
    if (fv > 0.985f) RGB(0.6f, 0.58f, 0.55f);
}

static void p_win_plain(float u, float v, int ix, int iy, float *c)
{
    p_concrete(u, v, ix, iy, c);
    c[0] *= 0.9f; c[1] *= 0.92f; c[2] *= 0.95f;
    WS w = { 0.1f, 0.9f, 0.62f, 0.86f, 0.02f, 0.4f, 0.4f, 0.42f };
    window_cell(u, v, 1, 1, &w, c);
}

static void p_door(float u, float v, int ix, int iy, float *c)
{
    float k = 0.8f + (tn(u, v, 12, 12, .2f, .2f) - 0.5f) * 0.12f;
    RGB(0.78f * k, 0.78f * k, 0.78f * k);
    bool panel = (u > 0.14f && u < 0.86f) && ((v > 0.06f && v < 0.42f) || (v > 0.5f && v < 0.94f));
    float e = fminf(fminf(u - 0.14f, 0.86f - u), fminf(v < 0.46f ? fminf(v - 0.06f, 0.42f - v) : fminf(v - 0.5f, 0.94f - v), 1));
    if (panel) { float s = e < 0.025f ? 0.6f : 1.0f; c[0] *= 0.88f * s; c[1] *= 0.88f * s; c[2] *= 0.88f * s; }
    float hx = u - 0.84f, hy = v - 0.48f;
    if (hx * hx + hy * hy < 0.0007f) RGB(0.8f, 0.7f, 0.3f);
}

static void p_garage(float u, float v, int ix, int iy, float *c)
{
    float row = v * 4, fr = row - floorf(row);
    float k = 0.82f + (tn(u, v, 6, 6, .1f, .7f) - 0.5f) * 0.1f;
    float fu = u * 4 - floorf(u * 4);
    bool inset = fr > 0.12f && fr < 0.88f && fu > 0.08f && fu < 0.92f;
    k *= inset ? 0.92f : 1.0f;
    if (fr < 0.03f) k *= 0.55f;
    RGB(k, k, k * 0.98f);
}

static void p_roller(float u, float v, int ix, int iy, float *c)
{
    float s = sinf(v * 6.2832f * 24) * 0.5f + 0.5f;
    float k = 0.55f + 0.2f * s + (tn(u, v, 4, 4, .8f, .1f) - 0.5f) * 0.1f;
    k *= 1 - 0.3f * sm(0.6f, 1, tn(u, v, 6, 6, .3f, .3f)) * (1 - v);
    RGB(k * 0.8f, k * 0.82f, k * 0.85f);
}

static void p_awning(float u, float v, int ix, int iy, float *c)
{
    float s = fmodf(u * 8, 1.0f);
    bool red = ((int)(u * 8)) & 1;
    float k = 0.9f + 0.1f * sinf(v * 3) + (hf(ix, iy, 36) - 0.5f) * 0.06f;
    if (red) RGB(0.78f * k, 0.12f * k, 0.1f * k); else RGB(0.95f * k, 0.93f * k, 0.88f * k);
    if (s < 0.02f) { c[0] *= 0.8f; c[1] *= 0.8f; c[2] *= 0.8f; }
}

static void p_chain(float u, float v, int ix, int iy, float *c)
{
    float a = fmodf((u + v) * 16, 1.0f), b = fmodf((u - v + 1) * 16, 1.0f);
    float da = fminf(a, 1 - a), db = fminf(b, 1 - b);
    c[3] = (da < 0.065f || db < 0.065f) ? 1 : 0;
    RGB(0.62f, 0.64f, 0.66f);
}

static void p_fence(float u, float v, int ix, int iy, float *c)
{
    float fu = u * 13, a = fu - floorf(fu);
    float top = 0.86f + (a < 0.5f ? a : 1 - a) * 0.0f;
    bool picket = a > 0.1f && a < 0.9f && v < top + 0.0f - fabsf(a - 0.5f) * 0.12f * 0 ;
    if (v > 0.8f) picket = picket && (1 - v) > fabsf(a - 0.5f) * 0.45f;
    bool rail = (v > 0.18f && v < 0.28f) || (v > 0.58f && v < 0.68f);
    c[3] = (picket || rail) ? 1 : 0;
    float k = 0.92f + (hf(ix, iy, 37) - 0.5f) * 0.08f;
    RGB(0.94f * k, 0.93f * k, 0.9f * k);
    if (!picket && rail) { c[0] *= 0.75f; c[1] *= 0.75f; c[2] *= 0.72f; }
}

static void p_palm(float u, float v, int ix, int iy, float *c)
{
    float a = fabsf(v - 0.5f);
    float w = (0.06f + 0.44f * sinf(3.1416f * powf(sat(u * 0.97f + 0.015f), 0.65f))) * (1 - 0.35f * u);
    float leaf = fmodf(u * 30 - a * 7.0f + 100, 1.0f);
    float cut = 0.7f - 0.2f * u;
    bool in = a < w && leaf < cut && u > 0.01f && u < 0.995f;
    bool rib = a < 0.014f * (1 - 0.7f * u) + 0.004f && u < 0.995f;
    c[3] = (in || rib) ? 1 : 0;
    float k = 0.8f + 0.45f * tn(u, v, 6, 2, .3f, .3f) + 0.3f * u + 0.2f * (hf(ix, iy, 43) - 0.5f);
    if (rib) RGB(0.45f, 0.48f, 0.2f);
    else RGB(0.2f * k, 0.42f * k, 0.1f * k);
}

typedef struct { float x, y, r, a, g; } LeafSpec;
static LeafSpec leaves[90];
static bool leaves_ready;

static void leaf_init(void)
{
    for (int i = 0; i < 90; i++) {
        leaves[i].x = hf(i, 0, 40); leaves[i].y = hf(i, 1, 40);
        leaves[i].r = 0.035f + 0.035f * hf(i, 2, 40);
        leaves[i].a = hf(i, 3, 40) * 3.1416f;
        leaves[i].g = hf(i, 4, 40);
    }
    leaves_ready = true;
}

static void p_leaf(float u, float v, int ix, int iy, float *c)
{
    float best = -1, shade = 0;
    for (int i = 0; i < 90; i++) {
        float dx = u - leaves[i].x, dy = v - leaves[i].y;
        dx -= roundf(dx); dy -= roundf(dy);
        float ca = cosf(leaves[i].a), sa = sinf(leaves[i].a);
        float lx = (dx * ca + dy * sa) / (leaves[i].r * 1.7f), ly = (-dx * sa + dy * ca) / (leaves[i].r * 0.75f);
        float d = lx * lx + ly * ly;
        if (d < 1 && leaves[i].g + (1 - d) * 0.3f > best) { best = leaves[i].g + (1 - d) * 0.3f; shade = leaves[i].g; }
    }
    c[3] = best >= 0 ? 1 : 0;
    float k = 0.55f + 0.7f * shade;
    RGB(0.2f * k, 0.4f * k, 0.1f * k);
}

static void p_bark(float u, float v, int ix, int iy, float *c)
{
    float g = tn(u, v, 6, 1, .1f, .5f) * 0.6f + tn(u, v, 4, 2, .5f, .1f) * 0.4f;
    float k = 0.6f + 0.7f * g;
    RGB(0.46f * k, 0.38f * k, 0.3f * k);
}

static void p_palmbark(float u, float v, int ix, int iy, float *c)
{
    float ring = fmodf(v * 9, 1.0f);
    float k = 0.7f + 0.3f * sinf(ring * 3.1416f) + (tn(u, v, 6, 1, .2f, .2f) - 0.5f) * 0.3f + (hf(ix, iy, 44) - 0.5f) * 0.1f;
    RGB(0.66f * k, 0.57f * k, 0.45f * k);
}

static float wear(float u, float v, int s)
{
    return sm(0.28f, 0.5f, tn(u, v, 6, 6, hf(s, 0, 41), hf(s, 1, 41)) * 0.6f + hf((int)(u * TS), (int)(v * TS), 42) * 0.5f);
}

static void p_dash(float u, float v, int ix, int iy, float *c)
{
    bool on = v < 0.3333f && u > 0.1f && u < 0.9f;
    c[3] = on ? 0.97f * wear(u, v, 1) + 0.03f : 0;
    c[3] = c[3] > 0.45f ? 1 : 0;
    RGB(0.93f, 0.93f, 0.9f);
}

static void p_yellow(float u, float v, int ix, int iy, float *c)
{
    bool on = (u > 0.1f && u < 0.4f) || (u > 0.6f && u < 0.9f);
    c[3] = on && wear(u, v, 2) > 0.4f ? 1 : 0;
    RGB(0.95f, 0.72f, 0.08f);
}

static void p_edge(float u, float v, int ix, int iy, float *c)
{
    c[3] = u > 0.08f && u < 0.92f && wear(u, v, 3) > 0.4f ? 1 : 0;
    RGB(0.93f, 0.93f, 0.9f);
}

static void p_zebra(float u, float v, int ix, int iy, float *c)
{
    float f = fmodf(u, 1.0f);
    c[3] = f < 0.5f && wear(u, v, 4) > 0.42f ? 1 : 0;
    RGB(0.93f, 0.93f, 0.9f);
}

static void p_arrow(float u, float v, int ix, int iy, float *c)
{
    bool shaft = u > 0.4f && u < 0.6f && v > 0.05f && v < 0.62f;
    bool head = v >= 0.62f && v < 0.98f && fabsf(u - 0.5f) < (0.98f - v) * 0.9f;
    c[3] = (shaft || head) && wear(u, v, 5) > 0.4f ? 1 : 0;
    RGB(0.93f, 0.93f, 0.9f);
}

static void p_manhole(float u, float v, int ix, int iy, float *c)
{
    float x = u - 0.5f, y = v - 0.5f, r = sqrtf(x * x + y * y) * 2;
    c[3] = r < 0.96f ? 1 : 0;
    float k = 0.28f + 0.1f * tn(u, v, 6, 6, .1f, .1f);
    if (r < 0.88f && (fmodf(r * 7, 1.0f) < 0.3f || fabsf(x) < 0.02f || fabsf(y) < 0.02f)) k *= 0.6f;
    if (r > 0.9f) k *= 0.5f;
    RGB(k, k * 0.95f, k * 0.9f);
}

static void p_parking(float u, float v, int ix, int iy, float *c)
{
    c[3] = (v < 0.03f || (u > 0.96f)) && wear(u, v, 6) > 0.4f ? 1 : 0;
    RGB(0.93f, 0.93f, 0.9f);
}

static void p_cross(float u, float v, int ix, int iy, float *c)
{
    RGB(0.96f, 0.96f, 0.96f);
    float x = fabsf(u - 0.5f), y = fabsf(v - 0.5f);
    if ((x < 0.09f && y < 0.3f) || (y < 0.09f && x < 0.3f)) RGB(0.85f, 0.08f, 0.08f);
    if (u < 0.02f || u > 0.98f || v < 0.02f || v > 0.98f) RGB(0.2f, 0.2f, 0.25f);
}

static void p_solar(float u, float v, int ix, int iy, float *c)
{
    float fu = fmodf(u * 6, 1.0f), fv = fmodf(v * 10, 1.0f);
    float k = 0.7f + 0.3f * tn(u, v, 3, 3, .1f, .1f);
    RGB(0.06f * k, 0.1f * k, 0.22f * k);
    if (fu < 0.04f || fv < 0.05f) RGB(0.6f, 0.62f, 0.65f);
    else if (fabsf(fmodf(fu * 6, 1.0f) - 0.5f) < 0.02f) { c[0] *= 1.6f; c[1] *= 1.6f; c[2] *= 1.6f; }
}

typedef struct { int layer; Paint fn; float size_m; int cx, cy; float tw, th; } LayerDef;

static const LayerDef defs[] = {
    { L_DETAIL, p_detail }, { L_ASPHALT, p_asphalt }, { L_SIDEWALK, p_sidewalk }, { L_CURB, p_curb },
    { L_CONCRETE, p_concrete }, { L_BRICK, p_brick }, { L_STUCCO, p_stucco },
    { L_GRASS, p_grass }, { L_SAND, p_sand }, { L_GRAVEL, p_gravel }, { L_TILE, p_tile }, { L_SHINGLE, p_shingle },
    { L_WOOD, p_wood }, { L_METAL, p_metal }, { L_PLAIN, p_plain }, 
    { L_WIN_STUCCO, p_win_stucco }, { L_WIN_BRICK, p_win_brick }, { L_STOREFRONT, p_storefront }, { L_WIN_PLAIN, p_win_plain },
    { L_DOOR, p_door }, { L_GARAGE, p_garage }, { L_ROLLER, p_roller }, { L_AWNING, p_awning },
    { L_CHAIN, p_chain }, { L_FENCE, p_fence }, { L_LEAF, p_leaf }, { L_BARK, p_bark },
    { L_DASH, p_dash }, { L_YELLOW, p_yellow }, { L_EDGE, p_edge }, { L_SNOW, p_snow },
};
#define NDEFS ((int)(sizeof defs / sizeof defs[0]))

// metres one texture repeat covers, by layer. facades hold several cells per repeat.
float layer_size[L_COUNT] = {
    [L_DETAIL] = 1.7f, [L_ASPHALT] = 8, [L_SIDEWALK] = 2.4f, [L_CURB] = 2.4f, [L_CONCRETE] = 4, [L_PANEL] = 4,
    [L_BRICK] = 1, [L_STUCCO] = 3, [L_GRASS] = 4, [L_SAND] = 6, [L_GRAVEL] = 2, [L_TILE] = 1, [L_SHINGLE] = 1,
    [L_WOOD] = 2, [L_METAL] = 2, [L_CONTAINER] = 2, [L_PLAIN] = 1, [L_QUAY] = 5,
    [L_CHAIN] = 1, [L_FENCE] = 2, [L_BARK] = 1, [L_PALMBARK] = 1, [L_DASH] = 9, [L_YELLOW] = 4, [L_EDGE] = 4,
    [L_ZEBRA] = 1, [L_PARKING] = 6, [L_SOLAR] = 1.6f, [L_SNOW] = 5, [L_AWNING] = 2, [L_DOOR] = 1, [L_GARAGE] = 3, [L_ROLLER] = 3,
};
static float tile_w[L_COUNT], tile_h[L_COUNT];

static void facade_defs(void)
{
    layer_cells[L_WIN_STUCCO] = (Vector2){ 2, 2 }; tile_w[L_WIN_STUCCO] = 7.2f; tile_h[L_WIN_STUCCO] = 6.8f;
    layer_cells[L_WIN_BRICK] = (Vector2){ 2, 2 }; tile_w[L_WIN_BRICK] = 7.2f; tile_h[L_WIN_BRICK] = 6.8f;
    layer_cells[L_WIN_CONC] = (Vector2){ 2, 1 }; tile_w[L_WIN_CONC] = 6.0f; tile_h[L_WIN_CONC] = 3.6f;
    layer_cells[L_WIN_GLASS] = (Vector2){ 3, 1 }; tile_w[L_WIN_GLASS] = 4.5f; tile_h[L_WIN_GLASS] = 3.6f;
    layer_cells[L_STOREFRONT] = (Vector2){ 2, 1 }; tile_w[L_STOREFRONT] = 7.2f; tile_h[L_STOREFRONT] = 4.2f;
    layer_cells[L_WIN_PLAIN] = (Vector2){ 1, 1 }; tile_w[L_WIN_PLAIN] = 6.0f; tile_h[L_WIN_PLAIN] = 4.0f;
}

static float cell_w(int l) { return tile_w[l] / layer_cells[l].x; }
static float cell_h(int l) { return tile_h[l] / layer_cells[l].y; }

// sign sheets: 8 rows of 512 x 64, text from raylib's pixel font blown up and softened
static const char *sign_text[32] = {
    "GAS", "DINER", "MOTEL", "EAT", "TRADING POST", "GARAGE", "RANGER STATION", "FARM FRESH",
    "VACANCY", "NO VACANCY", "OPEN 24 HRS", "COLD BEER", "AUTO REPAIR", "GENERAL STORE", "LAST GAS 200 KM", "CAMPGROUND",
    "ROUTE 9", "DUST DEVIL MOTEL", "SILVER SPUR", "BIG SKY DINER", "PINE HOLLOW", "RED MESA", "COYOTE CREEK", "LONESOME",
    "BAIT AND TACKLE", "HOT COFFEE", "TIRES", "ICE", "STOP", "KEEP OUT", "HELP", "WATER",
};
static const uint32_t sign_bg[8] = { 0x1b2a6b, 0x7a1d1d, 0xf0e6c8, 0x1d5a3a, 0x222222, 0xe8c030, 0x0e5a7a, 0x6b2a5a };
static const uint32_t sign_fg[8] = { 0xffffff, 0xffe9a0, 0x7a1d1d, 0xf4f0e0, 0xffd34a, 0x202020, 0xffffff, 0xffd0f0 };

static void paint_sign_sheet(uint8_t *dst, int sheet)
{
    memset(dst, 0, TS * TS * 4);
    for (int row = 0; row < 8; row++) {
        int idx = sheet * 8 + row;
        const char *t = sign_text[idx];
        uint32_t bg = sign_bg[(idx * 5 + 3) % 8], fg = sign_fg[(idx * 5 + 3) % 8];
        if (idx == 0) { bg = 0x15286b; fg = 0xffffff; }
        if (idx == 1 || idx == 2 || idx == 3) { bg = 0xf2f2f2; fg = 0xc01818; }
        int tw = MeasureText(t, 10);
        Image im = GenImageColor(tw + 4, 12, BLANK);
        ImageDrawText(&im, t, 2, 1, 10, WHITE);
        Color *src = LoadImageColors(im);
        int RH = TS / 8;
        float sc = 0.74f * RH / 10.0f;
        if (tw * sc > TS * 0.94f) sc = TS * 0.94f / tw;
        float ox = (TS - tw * sc) * 0.5f, oy = (RH - 10 * sc) * 0.5f - sc * 0.5f;
        for (int y = 0; y < RH; y++)
            for (int x = 0; x < TS; x++) {
                float sx = (x - ox) / sc + 1.5f - 0.5f, sy = (y - oy) / sc + 1.0f - 0.5f;
                int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
                float fx = sx - x0, fy = sy - y0, a = 0;
                for (int k = 0; k < 4; k++) {
                    int xx = x0 + (k & 1), yy = y0 + (k >> 1);
                    float w = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                    if (xx >= 0 && yy >= 0 && xx < im.width && yy < im.height) a += w * (src[yy * im.width + xx].a / 255.0f);
                }
                a = sm(0.32f, 0.62f, a);
                float border = (x < 6 || x > TS - 7 || y < 4 || y > RH - 5) ? 1.0f : (x < 10 || x > TS - 11 || y < 8 || y > RH - 9) ? 0.5f : 0;
                float cr = ((bg >> 16) & 255) / 255.0f, cg = ((bg >> 8) & 255) / 255.0f, cb = (bg & 255) / 255.0f;
                float fr = ((fg >> 16) & 255) / 255.0f, fgc = ((fg >> 8) & 255) / 255.0f, fb = (fg & 255) / 255.0f;
                if (border > 0.9f) { cr = fr; cg = fgc; cb = fb; }
                else if (border > 0.4f) { cr *= 0.6f; cg *= 0.6f; cb *= 0.6f; }
                float r = mixf(cr, fr, a), g = mixf(cg, fgc, a), b = mixf(cb, fb, a);
                uint8_t *q = dst + ((row * RH + y) * TS + x) * 4;
                q[0] = (uint8_t)(sat(r) * 255); q[1] = (uint8_t)(sat(g) * 255); q[2] = (uint8_t)(sat(b) * 255); q[3] = 255;
            }
        UnloadImageColors(src);
        UnloadImage(im);
    }
}

typedef struct { uint8_t *data; int from, to; } Job;

static void *paint_job(void *arg)
{
    Job *j = arg;
    for (int d = j->from; d < j->to; d++) {
        const LayerDef *ld = &defs[d];
        uint8_t *dst = j->data + (size_t)ld->layer * TS * TS * 4;
        for (int y = 0; y < TS; y++)
            for (int x = 0; x < TS; x++) {
                float c[4] = { 0.5f, 0.5f, 0.5f, 0 };
                ld->fn((x + 0.5f) / TS, (y + 0.5f) / TS, x, y, c);
                uint8_t *q = dst + (y * TS + x) * 4;
                q[0] = (uint8_t)(sat(c[0]) * 255 + 0.5f); q[1] = (uint8_t)(sat(c[1]) * 255 + 0.5f);
                q[2] = (uint8_t)(sat(c[2]) * 255 + 0.5f); q[3] = (uint8_t)(sat(c[3]) * 255 + 0.5f);
            }
    }
    return NULL;
}

void texture_build(int ts)
{
    TS = ts;
    facade_defs();
    noise_init();
    leaf_init();
    uint8_t *data = calloc((size_t)L_COUNT * TS * TS, 4);
    enum { NT_THREADS = 6 };
    Thread th[NT_THREADS];
    Job jobs[NT_THREADS];
    for (int t = 0; t < NT_THREADS; t++) {
        jobs[t] = (Job){ data, t * NDEFS / NT_THREADS, (t + 1) * NDEFS / NT_THREADS };
        thread_start(&th[t], paint_job, &jobs[t]);
    }
    for (int t = 0; t < NT_THREADS; t++) thread_join(&th[t]);
    for (int s = 0; s < 4; s++) paint_sign_sheet(data + (size_t)(L_SIGN0 + s) * TS * TS * 4, s);

    glGenTextures(1, &world_tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, world_tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8, TS, TS, L_COUNT, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D_ARRAY, 0x84FE, 8.0f);
    free(data);

    glGenTextures(1, &sig_tex);
    glBindTexture(GL_TEXTURE_2D, sig_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}
