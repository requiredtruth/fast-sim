#include "city.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
// native threads: pulling in winpthreads for one fork/join costs 20 KB
typedef struct { void *(*fn)(void *); void *arg; void *h; } Thread;
__declspec(dllimport) void *__stdcall CreateThread(void *, size_t, unsigned long (__stdcall *)(void *), void *, unsigned long, unsigned long *);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *, unsigned long);
static unsigned long __stdcall thread_main(void *p) { Thread *t = p; t->fn(t->arg); return 0; }
static void thread_start(Thread *t, void *(*fn)(void *), void *arg) { t->fn = fn; t->arg = arg; t->h = CreateThread(0, 0, thread_main, t, 0, 0); }
static void thread_join(Thread *t) { WaitForSingleObject(t->h, 0xFFFFFFFF); }
#else
#include <pthread.h>
typedef pthread_t Thread;
static void thread_start(Thread *t, void *(*fn)(void *), void *arg) { pthread_create(t, NULL, fn, arg); }
static void thread_join(Thread *t) { pthread_join(*t, NULL); }
#endif

// ---------------------------------------------------------------- shared state

Lane *lanes; int nlanes;
WalkNode *walk; int nwalk;
Shop *shops; int nshops;
Vector3 hospital_pos, police_pos;
GLuint world_tex, sig_tex;
Vector2 layer_cells[L_COUNT];

#define CHN 32
#define CHS 64.0f
#define CH0 (-1024.0f)
#define SW 4.0f
#define SWY 0.10f
#define WATER_Y (-0.9f)

static float rf(float a, float b) { return frand(a, b); }
static int ri(int n) { return (int)(rnd() % (uint32_t)n); }
static float sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float sm(float a, float b, float x) { float t = sat((x - a) / (b - a)); return t * t * (3 - 2 * t); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static Vector3 V3(float x, float y, float z) { return (Vector3){ x, y, z }; }

// ---------------------------------------------------------------- procedural textures

#define TS 512

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

static float NT[TS * TS];

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
    { L_CONCRETE, p_concrete }, { L_PANEL, p_panel }, { L_BRICK, p_brick }, { L_STUCCO, p_stucco },
    { L_GRASS, p_grass }, { L_SAND, p_sand }, { L_GRAVEL, p_gravel }, { L_TILE, p_tile }, { L_SHINGLE, p_shingle },
    { L_WOOD, p_wood }, { L_METAL, p_metal }, { L_CONTAINER, p_container }, { L_PLAIN, p_plain }, { L_QUAY, p_quay },
    { L_WIN_STUCCO, p_win_stucco }, { L_WIN_BRICK, p_win_brick }, { L_WIN_CONC, p_win_conc },
    { L_WIN_GLASS, p_win_glass }, { L_STOREFRONT, p_storefront }, { L_WIN_PLAIN, p_win_plain },
    { L_DOOR, p_door }, { L_GARAGE, p_garage }, { L_ROLLER, p_roller }, { L_AWNING, p_awning },
    { L_CHAIN, p_chain }, { L_FENCE, p_fence }, { L_PALM, p_palm }, { L_LEAF, p_leaf }, { L_BARK, p_bark },
    { L_PALMBARK, p_palmbark }, { L_DASH, p_dash }, { L_YELLOW, p_yellow }, { L_EDGE, p_edge }, { L_ZEBRA, p_zebra },
    { L_STOP, p_arrow }, { L_MANHOLE, p_manhole }, { L_PARKING, p_parking }, { L_CROSS, p_cross }, { L_SOLAR, p_solar },
};
#define NDEFS ((int)(sizeof defs / sizeof defs[0]))

// metres one texture repeat covers, by layer. facades hold several cells per repeat.
static float layer_size[L_COUNT] = {
    [L_DETAIL] = 1.7f, [L_ASPHALT] = 8, [L_SIDEWALK] = 2.4f, [L_CURB] = 2.4f, [L_CONCRETE] = 4, [L_PANEL] = 4,
    [L_BRICK] = 1, [L_STUCCO] = 3, [L_GRASS] = 4, [L_SAND] = 6, [L_GRAVEL] = 2, [L_TILE] = 1, [L_SHINGLE] = 1,
    [L_WOOD] = 2, [L_METAL] = 2, [L_CONTAINER] = 2, [L_PLAIN] = 1, [L_QUAY] = 5,
    [L_CHAIN] = 1, [L_FENCE] = 2, [L_BARK] = 1, [L_PALMBARK] = 1, [L_DASH] = 9, [L_YELLOW] = 4, [L_EDGE] = 4,
    [L_ZEBRA] = 1, [L_PARKING] = 6, [L_SOLAR] = 1.6f,
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
    "POLICE", "HOSPITAL", "EMERGENCY", "ST MARYS MEDICAL", "LUCKY LIQUOR", "CORNER MARKET", "PACIFIC DINER",
    "BLUE MOON PIZZA", "SPEEDY CLEANERS", "PAWN KINGS", "ROYAL BARBERS", "TACO LOCO", "BARGAIN BOOKS",
    "VIDEO PALACE", "DR NGUYEN DDS", "SUN AND SURF", "CASH 4 GOLD", "DONUT HOLE", "24H PHARMACY", "THE SALTY DOG",
    "GOLDEN WOK", "MARCOS DELI", "ELECTRO HUT", "LAUNDROMAT", "HARDWARE", "CAFE SOL", "GAS AND GO", "HOTEL SEABREEZE",
    "BEACH CAFE", "BAIL BONDS", "PALM MOTEL", "TATTOO PARLOR",
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
        float sc = 0.74f * 64 / 10.0f;
        if (tw * sc > 480) sc = 480.0f / tw;
        float ox = (TS - tw * sc) * 0.5f, oy = (64 - 10 * sc) * 0.5f - sc * 0.5f;
        for (int y = 0; y < 64; y++)
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
                float border = (x < 6 || x > TS - 7 || y < 4 || y > 59) ? 1.0f : (x < 10 || x > TS - 11 || y < 8 || y > 55) ? 0.5f : 0;
                float cr = ((bg >> 16) & 255) / 255.0f, cg = ((bg >> 8) & 255) / 255.0f, cb = (bg & 255) / 255.0f;
                float fr = ((fg >> 16) & 255) / 255.0f, fgc = ((fg >> 8) & 255) / 255.0f, fb = (fg & 255) / 255.0f;
                if (border > 0.9f) { cr = fr; cg = fgc; cb = fb; }
                else if (border > 0.4f) { cr *= 0.6f; cg *= 0.6f; cb *= 0.6f; }
                float r = mixf(cr, fr, a), g = mixf(cg, fgc, a), b = mixf(cb, fb, a);
                uint8_t *q = dst + ((row * 64 + y) * TS + x) * 4;
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

static void texture_build(void)
{
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

// ---------------------------------------------------------------- geometry buffers

typedef struct { Vtx *v; uint32_t *i; int nv, ni, cv, ci; } Buf;
static Buf bufs[CHN * CHN * GEO_PARTS];

typedef struct { int layer; uint8_t r, g, b, flags; float seed; } Mat;
#define M(l, rgb) ((Mat){ (l), (uint8_t)((rgb) >> 16), (uint8_t)((rgb) >> 8), (uint8_t)(rgb), 0, 0 })

static Mat mf(Mat m, int flags, float seed) { m.flags |= flags; m.seed = seed; return m; }
static Mat tint(Mat m, float k)
{
    m.r = (uint8_t)fminf(255, m.r * k); m.g = (uint8_t)fminf(255, m.g * k); m.b = (uint8_t)fminf(255, m.b * k);
    return m;
}

static int chunk_of(float x, float z)
{
    int cx = (int)floorf((x - CH0) / CHS), cz = (int)floorf((z - CH0) / CHS);
    cx = cx < 0 ? 0 : cx >= CHN ? CHN - 1 : cx;
    cz = cz < 0 ? 0 : cz >= CHN ? CHN - 1 : cz;
    return cz * CHN + cx;
}

static Buf *buf_at(int part, float x, float z) { return &bufs[chunk_of(x, z) * GEO_PARTS + part]; }

static uint32_t vert(Buf *b, Vector3 p, Vector3 n, float u, float v, Mat m)
{
    if (b->nv == b->cv) {
        b->cv = b->cv ? b->cv * 2 : 256;
        b->v = realloc(b->v, b->cv * sizeof(Vtx));
    }
    Vtx *x = &b->v[b->nv];
    x->p[0] = p.x; x->p[1] = p.y; x->p[2] = p.z;
    x->uv[0] = u; x->uv[1] = v;
    x->n[0] = (int8_t)lrintf(n.x * 127); x->n[1] = (int8_t)lrintf(n.y * 127); x->n[2] = (int8_t)lrintf(n.z * 127); x->n[3] = 0;
    x->c[0] = m.r; x->c[1] = m.g; x->c[2] = m.b; x->c[3] = m.flags;
    x->layer = (float)m.layer; x->seed = m.seed;
    return (uint32_t)b->nv++;
}

static void tri(Buf *b, uint32_t a, uint32_t c, uint32_t d)
{
    if (b->ni + 3 > b->ci) {
        b->ci = b->ci ? b->ci * 2 : 384;
        b->i = realloc(b->i, b->ci * sizeof(uint32_t));
    }
    b->i[b->ni++] = a; b->i[b->ni++] = c; b->i[b->ni++] = d;
}

static void quad_pn(int part, Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, Vector3 n, float u0, float v0, float u1, float v1, Mat m)
{
    Buf *b = buf_at(part, (p0.x + p2.x) * 0.5f, (p0.z + p2.z) * 0.5f);
    uint32_t a = vert(b, p0, n, u0, v0, m), c = vert(b, p1, n, u1, v0, m), d = vert(b, p2, n, u1, v1, m), e = vert(b, p3, n, u0, v1, m);
    tri(b, a, c, d);
    tri(b, a, d, e);
}

// o + eu + ev, normal = eu x ev
static void quadv(int part, Vector3 o, Vector3 eu, Vector3 ev, float u0, float v0, float u1, float v1, Mat m)
{
    Vector3 n = Vector3Normalize(Vector3CrossProduct(eu, ev));
    quad_pn(part, o, Vector3Add(o, eu), Vector3Add(Vector3Add(o, eu), ev), Vector3Add(o, ev), n, u0, v0, u1, v1, m);
}

// horizontal quad facing up, world planar uv
static void hq(int part, float x0, float z0, float x1, float z1, float y, Mat m)
{
    if (x1 < x0) { float t = x0; x0 = x1; x1 = t; }
    if (z1 < z0) { float t = z0; z0 = z1; z1 = t; }
    float s = 1.0f / layer_size[m.layer];
    int cx0 = (int)floorf((x0 - CH0) / CHS), cx1 = (int)floorf((x1 - 0.001f - CH0) / CHS);
    int cz0 = (int)floorf((z0 - CH0) / CHS), cz1 = (int)floorf((z1 - 0.001f - CH0) / CHS);
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cx = cx0; cx <= cx1; cx++) {
            float ax = fmaxf(x0, CH0 + cx * CHS), bx = fminf(x1, CH0 + (cx + 1) * CHS);
            float az = fmaxf(z0, CH0 + cz * CHS), bz = fminf(z1, CH0 + (cz + 1) * CHS);
            if (bx <= ax || bz <= az) continue;
            quadv(GEO_GROUND, V3(ax, y, bz), V3(bx - ax, 0, 0), V3(0, 0, az - bz), ax * s, bz * s, bx * s, az * s, m);
        }
}

static void hq_part(int part, float x0, float z0, float x1, float z1, float y, Mat m)
{
    float s = 1.0f / layer_size[m.layer];
    quadv(part, V3(x0, y, z1), V3(x1 - x0, 0, 0), V3(0, 0, z0 - z1), x0 * s, z1 * s, x1 * s, z0 * s, m);
}

// which side faces outward: 0 = -z, 1 = +z, 2 = -x, 3 = +x
static void vf(int part, int side, float a0, float a1, float c, float y0, float y1, Mat m, float u0, float u1, float v0, float v1)
{
    float l = a1 - a0, h = y1 - y0;
    switch (side) {
    case 1: quadv(part, V3(a0, y0, c), V3(l, 0, 0), V3(0, h, 0), u0, v0, u1, v1, m); break;
    case 0: quadv(part, V3(a1, y0, c), V3(-l, 0, 0), V3(0, h, 0), u0, v0, u1, v1, m); break;
    case 3: quadv(part, V3(c, y0, a1), V3(0, 0, -l), V3(0, h, 0), u0, v0, u1, v1, m); break;
    default: quadv(part, V3(c, y0, a0), V3(0, 0, l), V3(0, h, 0), u0, v0, u1, v1, m); break;
    }
}

static void vfm(int part, int side, float a0, float a1, float c, float y0, float y1, Mat m)
{
    float s = 1.0f / layer_size[m.layer];
    vf(part, side, a0, a1, c, y0, y1, m, a0 * s, a1 * s, y0 * s, y1 * s);
}

// facade wall: whole cells so windows land on the wall and on floors
static void facade(int part, int side, float a0, float a1, float c, float y0, float y1, Mat m)
{
    int l = m.layer;
    float cx = layer_cells[l].x, cy = layer_cells[l].y;
    int nu = (int)fmaxf(1, roundf((a1 - a0) / cell_w(l))), nv = (int)fmaxf(1, roundf((y1 - y0) / cell_h(l)));
    vf(part, side, a0, a1, c, y0, y1, m, 0, nu / cx, 0, nv / cy);
}

// ---------------------------------------------------------------- collision

typedef struct { float x0, y0, z0, x1, y1, z1; } Box;
static Box *boxes;
static int nbox, cbox;

static void add_box(float x0, float y0, float z0, float x1, float y1, float z1)
{
    if (nbox == cbox) { cbox = cbox ? cbox * 2 : 1024; boxes = realloc(boxes, cbox * sizeof(Box)); }
    boxes[nbox++] = (Box){ fminf(x0, x1), y0, fminf(z0, z1), fmaxf(x0, x1), y1, fmaxf(z0, z1) };
}

static void box(int part, float x0, float y0, float z0, float x1, float y1, float z1, Mat side, Mat top)
{
    vfm(part, 1, x0, x1, z1, y0, y1, side);
    vfm(part, 0, x0, x1, z0, y0, y1, side);
    vfm(part, 3, z0, z1, x1, y0, y1, side);
    vfm(part, 2, z0, z1, x0, y0, y1, side);
    hq_part(part, x0, z0, x1, z1, y1, top);
}

static void sbox(float x0, float y0, float z0, float x1, float y1, float z1, Mat side, Mat top)
{
    box(GEO_SOLID, x0, y0, z0, x1, y1, z1, side, top);
}

static void solid(float x0, float y0, float z0, float x1, float y1, float z1, Mat side, Mat top)
{
    sbox(x0, y0, z0, x1, y1, z1, side, top);
    add_box(x0, y0, z0, x1, y1, z1);
}

// ---------------------------------------------------------------- round things

static void tube(int part, Vector3 a, Vector3 b, float r0, float r1, int n, Mat m, float urep, float vrep)
{
    Vector3 w = Vector3Normalize(Vector3Subtract(b, a));
    Vector3 t = fabsf(w.y) < 0.9f ? V3(0, 1, 0) : V3(1, 0, 0);
    Vector3 s = Vector3Normalize(Vector3CrossProduct(w, t)), q = Vector3CrossProduct(w, s);
    Buf *bf = buf_at(part, a.x, a.z);
    uint32_t prev0 = 0, prev1 = 0;
    for (int i = 0; i <= n; i++) {
        float ang = i * 6.2832f / n;
        Vector3 d = Vector3Add(Vector3Scale(s, cosf(ang)), Vector3Scale(q, sinf(ang)));
        uint32_t v0 = vert(bf, Vector3Add(a, Vector3Scale(d, r0)), d, urep * i / n, 0, m);
        uint32_t v1 = vert(bf, Vector3Add(b, Vector3Scale(d, r1)), d, urep * i / n, vrep, m);
        if (i) { tri(bf, prev0, v0, v1); tri(bf, prev0, v1, prev1); }
        prev0 = v0; prev1 = v1;
    }
}

static void disc(int part, Vector3 c, Vector3 nrm, float r, int n, Mat m)
{
    Vector3 t = fabsf(nrm.y) < 0.9f ? V3(0, 1, 0) : V3(1, 0, 0);
    Vector3 s = Vector3Normalize(Vector3CrossProduct(nrm, t)), q = Vector3CrossProduct(nrm, s);
    Buf *bf = buf_at(part, c.x, c.z);
    uint32_t ctr = vert(bf, c, nrm, 0.5f, 0.5f, m), first = 0, prev = 0;
    for (int i = 0; i <= n; i++) {
        float ang = i * 6.2832f / n;
        Vector3 d = Vector3Add(Vector3Scale(s, cosf(ang)), Vector3Scale(q, sinf(ang)));
        uint32_t v = vert(bf, Vector3Add(c, Vector3Scale(d, r)), nrm, 0.5f + 0.5f * cosf(ang), 0.5f + 0.5f * sinf(ang), m);
        if (i) tri(bf, ctr, prev, v); else first = v;
        prev = v;
    }
    (void)first;
}

static void cyl(int part, float x, float y0, float z, float y1, float r, int n, Mat side, Mat top)
{
    tube(part, V3(x, y0, z), V3(x, y1, z), r, r, n, side, 1, (y1 - y0) / layer_size[side.layer]);
    disc(part, V3(x, y1, z), V3(0, 1, 0), r, n, top);
}

// ---------------------------------------------------------------- registries

static Vector3 lamp_pos[2048];
static int nlamp;

static void add_lamp(Vector3 p) { if (nlamp < 2048) lamp_pos[nlamp++] = p; }

int world_lamps(Vector3 near, Vector4 *out, int max)
{
    float dist[32];
    int n = 0;
    for (int i = 0; i < nlamp; i++) {
        float dx = lamp_pos[i].x - near.x, dz = lamp_pos[i].z - near.z, d = dx * dx + dz * dz;
        if (d > 110.0f * 110.0f) continue;
        if (n == max && d >= dist[max - 1]) continue;
        int k = n < max ? n++ : max - 1;
        while (k > 0 && dist[k - 1] > d) { dist[k] = dist[k - 1]; out[k] = out[k - 1]; k--; }
        dist[k] = d;
        out[k] = (Vector4){ lamp_pos[i].x, lamp_pos[i].y - 0.3f, lamp_pos[i].z, 21.0f };
    }
    return n;
}

// ---------------------------------------------------------------- props

static Mat M_STEEL, M_BLACK, M_DARK;

static void street_lamp(float x, float z, int dirx, int dirz)
{
    float h = 7.6f;
    Vector3 base = V3(x, SWY, z), top = V3(x, SWY + h, z);
    tube(GEO_SOLID, base, top, 0.1f, 0.065f, 8, M_STEEL, 1, 3);
    tube(GEO_SOLID, V3(x, SWY, z), V3(x, SWY + 0.5f, z), 0.16f, 0.16f, 8, M_STEEL, 1, 1);
    Vector3 arm = V3(x + dirx * 2.1f, SWY + h + 0.35f, z + dirz * 2.1f);
    tube(GEO_SOLID, top, V3(x + dirx * 1.0f, SWY + h + 0.35f, z + dirz * 1.0f), 0.05f, 0.04f, 6, M_STEEL, 1, 1);
    tube(GEO_SOLID, V3(x + dirx * 1.0f, SWY + h + 0.35f, z + dirz * 1.0f), arm, 0.04f, 0.04f, 6, M_STEEL, 1, 1);
    float hx = dirx ? 0.6f : 0.2f, hz = dirz ? 0.6f : 0.2f;
    sbox(arm.x - hx, arm.y - 0.1f, arm.z - hz, arm.x + hx, arm.y + 0.08f, arm.z + hz, M_STEEL, M_STEEL);
    hq_part(GEO_SOLID, arm.x - hx * 0.9f, arm.z - hz * 0.9f, arm.x + hx * 0.9f, arm.z + hz * 0.9f, arm.y - 0.105f, mf(M(L_PLAIN, 0xfff2d0), F_SELF, 3.5f));
    add_box(x - 0.15f, 0, z - 0.15f, x + 0.15f, h, z + 0.15f);
    add_lamp(V3(arm.x, arm.y, arm.z));
}

static void signal_head(Vector3 p, int side, int group)
{
    // p = centre of the housing; side = which way it faces (0 = -z, 1 = +z, 2 = -x, 3 = +x)
    float w = 0.2f, d = 0.18f, h = 0.55f;
    bool zface = side < 2;
    float hx = zface ? w : d, hz = zface ? d : w;
    sbox(p.x - hx, p.y - h, p.z - hz, p.x + hx, p.y + h, p.z + hz, M_BLACK, M_BLACK);
    for (int k = 0; k < 3; k++) {
        float y = p.y + 0.34f - k * 0.34f;
        Mat lm = mf(M(L_PLAIN, 0xffffff), F_SIGNAL, (float)(group * 4 + k));
        float off = 0.01f;
        switch (side) {
        case 0: quadv(GEO_SOLID, V3(p.x + 0.13f, y - 0.13f, p.z - hz - off), V3(-0.26f, 0, 0), V3(0, 0.26f, 0), 0, 0, 1, 1, lm); break;
        case 1: quadv(GEO_SOLID, V3(p.x - 0.13f, y - 0.13f, p.z + hz + off), V3(0.26f, 0, 0), V3(0, 0.26f, 0), 0, 0, 1, 1, lm); break;
        case 2: quadv(GEO_SOLID, V3(p.x - hx - off, y - 0.13f, p.z - 0.13f), V3(0, 0, 0.26f), V3(0, 0.26f, 0), 0, 0, 1, 1, lm); break;
        default: quadv(GEO_SOLID, V3(p.x + hx + off, y - 0.13f, p.z + 0.13f), V3(0, 0, -0.26f), V3(0, 0.26f, 0), 0, 0, 1, 1, lm); break;
        }
        (void)zface;
    }
}

static void hydrant(float x, float z)
{
    Mat red = M(L_PLAIN, 0xc01818), cap = M(L_PLAIN, 0xd8c020);
    cyl(GEO_SOLID, x, SWY, z, SWY + 0.62f, 0.12f, 8, red, red);
    cyl(GEO_SOLID, x, SWY + 0.62f, z, SWY + 0.72f, 0.14f, 8, cap, cap);
    sbox(x - 0.24f, SWY + 0.38f, z - 0.05f, x + 0.24f, SWY + 0.5f, z + 0.05f, red, red);
    add_box(x - 0.2f, 0, z - 0.2f, x + 0.2f, 0.8f, z + 0.2f);
}

static void trash_can(float x, float z)
{
    Mat g = M(L_METAL, 0x40524a);
    cyl(GEO_SOLID, x, SWY, z, SWY + 0.85f, 0.28f, 10, g, M(L_PLAIN, 0x2a2e2c));
    add_box(x - 0.28f, 0, z - 0.28f, x + 0.28f, 1.0f, z + 0.28f);
}

static void bench(float x, float z, bool alongx)
{
    Mat wd = M(L_WOOD, 0xc8a070), st = M_STEEL;
    float l = 0.85f, w = 0.25f;
    float x0 = alongx ? x - l : x - w, x1 = alongx ? x + l : x + w, z0 = alongx ? z - w : z - l, z1 = alongx ? z + w : z + l;
    sbox(x0, SWY + 0.42f, z0, x1, SWY + 0.48f, z1, wd, wd);
    if (alongx) {
        sbox(x0, SWY + 0.5f, z0 - 0.0f, x1, SWY + 0.95f, z0 + 0.05f, wd, wd);
        sbox(x0 + 0.1f, SWY, z0 + 0.05f, x0 + 0.16f, SWY + 0.42f, z1 - 0.05f, st, st);
        sbox(x1 - 0.16f, SWY, z0 + 0.05f, x1 - 0.1f, SWY + 0.42f, z1 - 0.05f, st, st);
    } else {
        sbox(x0, SWY + 0.5f, z0, x0 + 0.05f, SWY + 0.95f, z1, wd, wd);
        sbox(x0 + 0.05f, SWY, z0 + 0.1f, x1 - 0.05f, SWY + 0.42f, z0 + 0.16f, st, st);
        sbox(x0 + 0.05f, SWY, z1 - 0.16f, x1 - 0.05f, SWY + 0.42f, z1 - 0.1f, st, st);
    }
    add_box(x0, 0, z0, x1, SWY + 0.5f, z1);
}

static void wire(Vector3 a, Vector3 b, float sag, Mat m)
{
    int n = 8;
    Vector3 pa = a;
    for (int i = 1; i <= n; i++) {
        float t = i / (float)n;
        Vector3 pb = Vector3Lerp(a, b, t);
        pb.y -= sag * 4 * t * (1 - t);
        Vector3 d = Vector3Subtract(pb, pa);
        Vector3 side = Vector3Scale(Vector3Normalize(V3(-d.z, 0, d.x)), 0.018f);
        quadv(GEO_SOLID, Vector3Subtract(pa, side), Vector3Scale(side, 2), Vector3Subtract(pb, pa), 0, 0, 1, 1, m);
        quadv(GEO_SOLID, Vector3Subtract(pa, V3(0, 0.018f, 0)), V3(0, 0.036f, 0), Vector3Subtract(pb, pa), 0, 0, 1, 1, m);
        pa = pb;
    }
}

static void power_pole(float x, float z, bool alongx)
{
    Mat wd = M(L_BARK, 0x8a7a68);
    float h = 9.0f;
    tube(GEO_SOLID, V3(x, SWY, z), V3(x, SWY + h, z), 0.17f, 0.12f, 8, wd, 1, 4);
    float ax = alongx ? 0 : 1.0f, az = alongx ? 1.0f : 0;
    sbox(x - ax - 0.06f, SWY + h - 0.7f, z - az - 0.06f, x + ax + 0.06f, SWY + h - 0.58f, z + az + 0.06f, wd, wd);
    for (int k = -1; k <= 1; k += 2) {
        sbox(x + k * ax * 0.9f - 0.04f, SWY + h - 0.58f, z + k * az * 0.9f - 0.04f, x + k * ax * 0.9f + 0.04f, SWY + h - 0.4f, z + k * az * 0.9f + 0.04f, M_DARK, M_DARK);
    }
    cyl(GEO_SOLID, x + (alongx ? 0.35f : 0), SWY + h - 2.6f, z + (alongx ? 0 : 0.35f), SWY + h - 1.7f, 0.22f, 8, M(L_METAL, 0x6a7078), M_STEEL);
    add_box(x - 0.2f, 0, z - 0.2f, x + 0.2f, h, z + 0.2f);
}

static void leaf_card(Vector3 c, Vector3 right, Vector3 up, Vector3 n, float s, Mat m)
{
    Vector3 r = Vector3Scale(right, s), u = Vector3Scale(up, s);
    quad_pn(GEO_SOLID, Vector3Subtract(Vector3Subtract(c, r), u), Vector3Subtract(Vector3Add(c, r), u), Vector3Add(Vector3Add(c, r), u),
            Vector3Add(Vector3Subtract(c, r), u), n, 0, 0, 1, 1, m);
}

static void tree_broad(float x, float z, float h)
{
    Mat bark = M(L_BARK, 0xb0a090);
    float lean = rf(-0.4f, 0.4f);
    Vector3 base = V3(x, SWY, z), mid = V3(x + lean, SWY + h * 0.42f, z + lean * 0.5f);
    tube(GEO_SOLID, base, mid, 0.24f, 0.16f, 8, bark, 1, h * 0.5f);
    Vector3 c = V3(x + lean, SWY + h * 0.72f, z + lean * 0.5f);
    float R = h * 0.36f;
    for (int i = 0; i < 4; i++) {
        float a = i * 1.57f + rf(0, 1);
        Vector3 e = V3(c.x + cosf(a) * R * 0.55f, c.y + R * 0.1f, c.z + sinf(a) * R * 0.55f);
        tube(GEO_SOLID, mid, e, 0.11f, 0.05f, 6, bark, 1, 2);
    }
    float hue = rf(0.8f, 1.15f);
    float warm = rf(-0.12f, 0.2f);
    for (int i = 0; i < 64; i++) {
        float a = rf(0, 6.2832f), b = acosf(rf(-0.55f, 1.0f)), rr = R * powf(rf(0.45f, 1.0f), 0.5f);
        Vector3 p = V3(c.x + sinf(b) * cosf(a) * rr * 1.1f, c.y + cosf(b) * rr * 0.85f, c.z + sinf(b) * sinf(a) * rr * 1.1f);
        Vector3 n = Vector3Normalize(Vector3Subtract(p, Vector3Add(c, V3(0, -R * 0.2f, 0))));
        float ra = rf(0, 3.1416f), t = rf(0.35f, 1.0f);
        Vector3 right = V3(cosf(ra), rf(-0.3f, 0.3f), sinf(ra)), up = Vector3Normalize(Vector3CrossProduct(right, n));
        right = Vector3Normalize(Vector3CrossProduct(n, up));
        float g = hue * rf(0.85f, 1.1f);
        Mat m = mf(M(L_LEAF, ((int)fminf(255, 205 * g * (1 + warm)) << 16) | ((int)fminf(255, 230 * g) << 8) | (int)(170 * (1 - warm))), F_CUT, sat((p.y - c.y) / R * 0.5f + 0.55f));
        leaf_card(p, right, up, n, 1.15f * t + 0.5f, m);
    }
    add_box(x - 0.25f, 0, z - 0.25f, x + 0.25f, h * 0.5f, z + 0.25f);
}

static void palm(float x, float z, float h)
{
    Mat bark = M(L_PALMBARK, 0xd8d0c0);
    float la = rf(0, 6.2832f), lean = h * rf(0.05f, 0.14f);
    Vector3 p[9];
    for (int i = 0; i < 9; i++) {
        float t = i / 8.0f;
        p[i] = V3(x + cosf(la) * lean * t * t, SWY + h * t, z + sinf(la) * lean * t * t);
    }
    for (int i = 0; i < 8; i++) tube(GEO_SOLID, p[i], p[i + 1], 0.27f - 0.1f * i / 8, 0.27f - 0.1f * (i + 1) / 8, 8, bark, 1, h / 8 / layer_size[L_PALMBARK]);
    Vector3 top = p[8];
    int nf = 13 + ri(4);
    for (int f = 0; f < nf; f++) {
        float az = f * 6.2832f / nf + rf(-0.15f, 0.15f);
        float el = f % 3 == 0 ? rf(0.55f, 0.9f) : rf(0.0f, 0.45f), len = rf(3.4f, 4.4f), droop = rf(1.4f, 2.4f);
        Vector3 dir = V3(cosf(az), 0, sinf(az));
        Vector3 side = V3(-dir.z, 0, dir.x);
        Vector3 prev = top;
        float hue = rf(0.9f, 1.1f);
        Mat m = mf(M(L_PALM, ((int)(220 * hue) << 16) | (255 << 8) | 200), F_CUT, 1);
        float pw = 0.04f;
        for (int k = 1; k <= 6; k++) {
            float t = k / 6.0f;
            Vector3 q = V3(top.x + dir.x * len * t, top.y + len * (sinf(el) * t) - droop * t * t, top.z + dir.z * len * t);
            float wd = (0.1f + 0.85f * sinf(3.1416f * powf(sat(t * 0.97f + 0.015f), 0.65f))) * (1 - 0.35f * t);
            Vector3 tg = Vector3Normalize(Vector3Subtract(q, prev));
            Vector3 nrm = Vector3Normalize(Vector3CrossProduct(tg, side));
            if (nrm.y < 0) nrm = Vector3Negate(nrm);
            float u0 = (k - 1) / 6.0f, droop_side = 0.5f;
            for (int sd = -1; sd <= 1; sd += 2) {
                Vector3 a0 = Vector3Add(prev, Vector3Add(Vector3Scale(side, sd * pw), V3(0, -pw * droop_side, 0)));
                Vector3 a1 = Vector3Add(q, Vector3Add(Vector3Scale(side, sd * wd), V3(0, -wd * droop_side, 0)));
                float v0 = sd < 0 ? 0.0f : 0.5f, v1 = sd < 0 ? 0.5f : 1.0f;
                if (sd < 0) quad_pn(GEO_SOLID, a0, a1, q, prev, nrm, u0, v0, t, v1, m);
                else quad_pn(GEO_SOLID, prev, q, a1, a0, nrm, u0, v0, t, v1, m);
            }
            prev = q;
            pw = wd;
        }
    }
    add_box(x - 0.3f, 0, z - 0.3f, x + 0.3f, h * 0.6f, z + 0.3f);
}

// ---------------------------------------------------------------- the grid

#define MAXL 12
static float gx[MAXL], gz[MAXL], rhx[MAXL], rhz[MAXL];   // street centres and half carriageway widths
static int ngx = 10, ngz = 10;
static bool avx[MAXL], avz[MAXL];
static float xmin_o = -608, xmax_o = 424, zmin_o = -608, zmax_o = 424;   // outer edge of the outermost sidewalks

typedef struct { float x0, z0, x1, z1; } Rect;
static Rect rect(float x0, float z0, float x1, float z1) { return (Rect){ x0, z0, x1, z1 }; }

static Rect lrect(Rect lot, int face, float a0, float d0, float a1, float d1)
{
    switch (face) {
    case 0: return rect(lot.x0 + a0, lot.z0 + d0, lot.x0 + a1, lot.z0 + d1);
    case 1: return rect(lot.x0 + a0, lot.z1 - d1, lot.x0 + a1, lot.z1 - d0);
    case 2: return rect(lot.x0 + d0, lot.z0 + a0, lot.x0 + d1, lot.z0 + a1);
    default: return rect(lot.x1 - d1, lot.z0 + a0, lot.x1 - d0, lot.z0 + a1);
    }
}
static float lot_w(Rect r, int face) { return face < 2 ? r.x1 - r.x0 : r.z1 - r.z0; }
static float lot_d(Rect r, int face) { return face < 2 ? r.z1 - r.z0 : r.x1 - r.x0; }

static void side_span(Rect r, int s, float *a0, float *a1, float *c)
{
    switch (s) {
    case 0: *a0 = r.x0; *a1 = r.x1; *c = r.z0; break;
    case 1: *a0 = r.x0; *a1 = r.x1; *c = r.z1; break;
    case 2: *a0 = r.z0; *a1 = r.z1; *c = r.x0; break;
    default: *a0 = r.z0; *a1 = r.z1; *c = r.x1; break;
    }
}
static Vector2 side_n(int s) { return s == 0 ? (Vector2){ 0, -1 } : s == 1 ? (Vector2){ 0, 1 } : s == 2 ? (Vector2){ -1, 0 } : (Vector2){ 1, 0 }; }
// world point at distance `along` on side s, pushed out by `out`
static Vector3 side_pt(Rect r, int s, float along, float out, float y)
{
    float a0, a1, c;
    side_span(r, s, &a0, &a1, &c);
    Vector2 n = side_n(s);
    float a = a0 + along;
    return s < 2 ? V3(a + n.x * out, y, c + n.y * out) : V3(c + n.x * out, y, a + n.y * out);
}

static float hw_of(bool av) { return av ? 9.0f : 5.5f; }

static void make_grid(void)
{
    static const bool ax[10] = { 0, 0, 1, 0, 0, 1, 0, 0, 0, 1 }, az[10] = { 0, 0, 0, 1, 0, 0, 1, 0, 0, 1 };
    for (int i = 0; i < 10; i++) { avx[i] = ax[i]; avz[i] = az[i]; rhx[i] = hw_of(ax[i]); rhz[i] = hw_of(az[i]); }
    for (int pass = 0; pass < 2; pass++) {
        float *g = pass ? gz : gx, *rh = pass ? rhz : rhx;
        float lo = (pass ? zmin_o : xmin_o) + SW + rh[0], hi = (pass ? zmax_o : xmax_o) - SW - rh[9];
        float gap[9], tot = 0;
        for (int i = 0; i < 9; i++) { gap[i] = rf(0.86f, 1.14f); tot += gap[i]; }
        g[0] = lo;
        for (int i = 0; i < 9; i++) g[i + 1] = g[i] + gap[i] / tot * (hi - lo);
    }
}

static Rect block_rect(int i, int j)
{
    return rect(gx[i] + rhx[i] + SW, gz[j] + rhz[j] + SW, gx[i + 1] - rhx[i + 1] - SW, gz[j + 1] - rhz[j + 1] - SW);
}

static const char *layout[9] = {
    "RRRRRRRRB", "RRRRRRRRB", "RPSSRRRRB", "RRSDDDHSB", "RRXDDDSRB", "SSSDDDSSB", "SSSSSSSSB", "IIRRPRRRB", "IIIIIIBBB",
};

// ---------------------------------------------------------------- roads

static Mat M_ASPHALT, M_SIDEWALK;
static float bseed_counter;
static float next_seed(void) { return bseed_counter += 1.0f + ri(3); }

static void paint_strip(Vector3 a, Vector3 b, float width, Mat m, float vtile)
{
    Vector3 d = Vector3Subtract(b, a);
    float len = Vector3Length(d);
    if (len < 0.05f) return;
    Vector3 dir = Vector3Scale(d, 1 / len), side = V3(-dir.z, 0, dir.x);
    Vector3 o = Vector3Subtract(a, Vector3Scale(side, width * 0.5f));
    o.y = 0.014f;
    m.flags |= F_CUT | F_DECAL;
    // split long strips so each piece lands in its own chunk
    int n = (int)ceilf(len / 48.0f);
    for (int k = 0; k < n; k++) {
        float t0 = (float)k / n, t1 = (float)(k + 1) / n;
        Vector3 p = Vector3Add(o, Vector3Scale(d, t0));
        quadv(GEO_GROUND, p, Vector3Scale(side, width), Vector3Scale(d, t1 - t0), 0, t0 * len / vtile, 1, t1 * len / vtile, m);
    }
}

static void build_intersection_paint(int i, int j)
{
    static const float dx[4] = { 1, 0, -1, 0 }, dz[4] = { 0, 1, 0, -1 };
    Vector3 C = V3(gx[i], 0, gz[j]);
    for (int d = 0; d < 4; d++) {
        bool alongx = d % 2 == 0;
        int ni = i + (int)dx[d], nj = j + (int)dz[d];
        if (ni < 0 || nj < 0 || ni >= ngx || nj >= ngz) continue;
        float sq = alongx ? rhx[i] : rhz[j];      // half extent of the square along this arm
        float hwr = alongx ? rhz[j] : rhx[i];     // half carriageway width of the arm road
        Vector3 dv = V3(dx[d], 0, dz[d]), r = V3(-dz[d], 0, dx[d]);
        // zebra crossing
        Vector3 zc = Vector3Add(C, Vector3Scale(dv, sq + 2.0f));
        Vector3 za = Vector3Subtract(zc, Vector3Scale(r, hwr)), zb = Vector3Add(zc, Vector3Scale(r, hwr));
        Vector3 o = za;
        o.y = 0.014f;
        Vector3 ev = Vector3Scale(dv, 1.5f);
        o = Vector3Subtract(o, ev);
        Mat zm = mf(M(L_ZEBRA, 0xffffff), F_CUT | F_DECAL, 0);
        quadv(GEO_GROUND, o, Vector3Subtract(zb, za), Vector3Scale(dv, 3.0f), 0, 0, 2 * hwr, 1, zm);
        // stop line on the incoming side: traffic arriving from this arm travels -dv
        Vector3 sl = Vector3Add(C, Vector3Scale(dv, sq + 4.35f));
        int nl = (int)(hwr > 7 ? 2 : 1);
        Vector3 rr = Vector3Negate(r);   // right of an approaching car (travelling -dv) is -r
        (void)rr;
        // approaching car travels -dv; its right-hand side is cross(-dv, up) = -(-dz, dx) = (dz, -dx)
        Vector3 rt = V3(dz[d], 0, -dx[d]);
        paint_strip(Vector3Add(sl, Vector3Scale(rt, 0.3f)), Vector3Add(sl, Vector3Scale(rt, nl * 3.5f)), 0.5f, M(L_EDGE, 0xffffff), 4);
        // wider tick: swap uv so the 0.5 m side runs along the road
        for (int k = 0; k < nl; k++) {
            Vector3 ac = Vector3Add(Vector3Add(C, Vector3Scale(dv, sq + 12.0f)), Vector3Scale(rt, 1.75f + 3.5f * k));
            Vector3 a = Vector3Subtract(ac, Vector3Scale(dv, 1.8f));
            Mat am = mf(M(L_STOP, 0xffffff), F_CUT | F_DECAL, 0);
            Vector3 dd = Vector3Negate(dv);   // arrow points the way the car travels
            Vector3 sd = V3(-dd.z, 0, dd.x);
            Vector3 ao = Vector3Subtract(Vector3Add(ac, Vector3Scale(dd, -1.8f)), Vector3Scale(sd, 0.5f));
            ao.y = 0.014f;
            (void)a;
            quadv(GEO_GROUND, ao, Vector3Scale(sd, 1.0f), Vector3Scale(dd, 3.6f), 0, 0, 1, 1, am);
        }
    }
}

static void build_road_paint(Vector3 a, Vector3 b, bool av)
{
    // a -> b along the road centre, already trimmed to the painted stretch
    paint_strip(a, b, 0.4f, M(L_YELLOW, 0xffffff), 4);
    Vector3 d = Vector3Normalize(Vector3Subtract(b, a)), s = V3(-d.z, 0, d.x);
    float edge = av ? 7.0f : 3.5f;
    for (int k = -1; k <= 1; k += 2) {
        paint_strip(Vector3Add(a, Vector3Scale(s, k * edge)), Vector3Add(b, Vector3Scale(s, k * edge)), 0.16f, M(L_EDGE, 0xffffff), 4);
        if (av) paint_strip(Vector3Add(a, Vector3Scale(s, k * 3.5f)), Vector3Add(b, Vector3Scale(s, k * 3.5f)), 0.16f, M(L_DASH, 0xffffff), 9);
    }
}

static void patches_and_manholes(Vector3 a, Vector3 b, float hw)
{
    Vector3 d = Vector3Subtract(b, a);
    float len = Vector3Length(d);
    if (len < 30) return;
    Vector3 dir = Vector3Scale(d, 1 / len), s = V3(-dir.z, 0, dir.x);
    int n = (int)(len / 55);
    for (int k = 0; k < n; k++) {
        float t = rf(10, len - 10), off = rf(-hw + 1.5f, hw - 1.5f);
        Vector3 p = Vector3Add(Vector3Add(a, Vector3Scale(dir, t)), Vector3Scale(s, off));
        if (ri(3) == 0) {
            Vector3 o = Vector3Subtract(Vector3Subtract(p, Vector3Scale(s, 0.4f)), Vector3Scale(dir, 0.4f));
            o.y = 0.014f;
            quadv(GEO_GROUND, o, Vector3Scale(s, 0.8f), Vector3Scale(dir, 0.8f), 0, 0, 1, 1, mf(M(L_MANHOLE, 0xffffff), F_CUT | F_DECAL, 0));
        } else {
            float pw = rf(1.2f, 2.4f), pl = rf(2.5f, 6.0f);
            Vector3 o = Vector3Subtract(p, Vector3Scale(s, pw * 0.5f));
            o.y = 0.013f;
            Mat pm = mf(M(L_ASPHALT, (int)(rf(0.62f, 0.86f) * 255) * 0x010101), F_DECAL, 0);
            Vector3 su = Vector3Scale(s, pw), dl = Vector3Scale(dir, pl);
            float us = 1.0f / layer_size[L_ASPHALT];
            quad_pn(GEO_GROUND, o, Vector3Add(o, su), Vector3Add(Vector3Add(o, su), dl), Vector3Add(o, dl), V3(0, 1, 0),
                    o.x * us, o.z * us, (o.x + su.x) * us, (o.z + su.z) * us, pm);
        }
    }
}

static void build_roads(void)
{
    M_ASPHALT = M(L_ASPHALT, 0xffffff);
    for (int i = 0; i < ngx; i++)
        for (int j = 0; j < ngz; j++) {
            hq(GEO_GROUND, gx[i] - rhx[i], gz[j] - rhz[j], gx[i] + rhx[i], gz[j] + rhz[j], 0, M_ASPHALT);
            build_intersection_paint(i, j);
        }
    for (int i = 0; i < ngx; i++)
        for (int j = 0; j < ngz - 1; j++) {
            float z0 = gz[j] + rhz[j], z1 = gz[j + 1] - rhz[j + 1];
            hq(GEO_GROUND, gx[i] - rhx[i], z0, gx[i] + rhx[i], z1, 0, M_ASPHALT);
            Vector3 a = V3(gx[i], 0, z0 + rhx[i] * 0 + 4.8f), b = V3(gx[i], 0, z1 - 4.8f);
            a.z = z0 + 4.9f; b.z = z1 - 4.9f;
            build_road_paint(a, b, avx[i]);
            patches_and_manholes(a, b, rhx[i]);
        }
    for (int j = 0; j < ngz; j++)
        for (int i = 0; i < ngx - 1; i++) {
            float x0 = gx[i] + rhx[i], x1 = gx[i + 1] - rhx[i + 1];
            hq(GEO_GROUND, x0, gz[j] - rhz[j], x1, gz[j] + rhz[j], 0, M_ASPHALT);
            Vector3 a = V3(x0 + 4.9f, 0, gz[j]), b = V3(x1 - 4.9f, 0, gz[j]);
            build_road_paint(a, b, avz[j]);
            patches_and_manholes(a, b, rhz[j]);
        }
}

// sidewalk ring around a cell. inner = the lot rect, or the cell itself when ring is false
static void curb_faces(float x0, float z0, float x1, float z1, bool outer_x0, bool outer_x1, bool outer_z0, bool outer_z1)
{
    Mat cm = M(L_CURB, 0xffffff);
    (void)outer_x0; (void)outer_x1; (void)outer_z0; (void)outer_z1;
    vfm(GEO_GROUND, 0, x0, x1, z0, 0, SWY, cm);
    vfm(GEO_GROUND, 1, x0, x1, z1, 0, SWY, cm);
    vfm(GEO_GROUND, 2, z0, z1, x0, 0, SWY, cm);
    vfm(GEO_GROUND, 3, z0, z1, x1, 0, SWY, cm);
}

static void build_sidewalks(void)
{
    M_SIDEWALK = M(L_SIDEWALK, 0xffffff);
    Mat top = M(L_CURB, 0xe0e0e0);
    for (int i = -1; i < ngx; i++)
        for (int j = -1; j < ngz; j++) {
            float x0 = i < 0 ? xmin_o : gx[i] + rhx[i], x1 = i == ngx - 1 ? xmax_o : gx[i + 1] - rhx[i + 1];
            float z0 = j < 0 ? zmin_o : gz[j] + rhz[j], z1 = j == ngz - 1 ? zmax_o : gz[j + 1] - rhz[j + 1];
            curb_faces(x0, z0, x1, z1, 0, 0, 0, 0);
            bool real = i >= 0 && j >= 0 && i < ngx - 1 && j < ngz - 1;
            if (!real) { hq(GEO_GROUND, x0, z0, x1, z1, SWY, M_SIDEWALK); continue; }
            hq(GEO_GROUND, x0, z0, x1, z0 + SW, SWY, M_SIDEWALK);
            hq(GEO_GROUND, x0, z1 - SW, x1, z1, SWY, M_SIDEWALK);
            hq(GEO_GROUND, x0, z0 + SW, x0 + SW, z1 - SW, SWY, M_SIDEWALK);
            hq(GEO_GROUND, x1 - SW, z0 + SW, x1, z1 - SW, SWY, M_SIDEWALK);
            // paler curb top
            hq(GEO_GROUND, x0, z0, x1, z0 + 0.22f, SWY + 0.002f, top);
            hq(GEO_GROUND, x0, z1 - 0.22f, x1, z1, SWY + 0.002f, top);
            hq(GEO_GROUND, x0, z0 + 0.22f, x0 + 0.22f, z1 - 0.22f, SWY + 0.002f, top);
            hq(GEO_GROUND, x1 - 0.22f, z0 + 0.22f, x1, z1 - 0.22f, SWY + 0.002f, top);
        }
}

static void build_signals(int i, int j)
{
    static const float dx[4] = { 1, 0, -1, 0 }, dz[4] = { 0, 1, 0, -1 };
    int id = i * ngz + j;
    for (int d = 0; d < 4; d++) {
        // traffic travelling in direction d arrives from the opposite arm
        int fi = i - (int)dx[d], fj = j - (int)dz[d];
        if (fi < 0 || fj < 0 || fi >= ngx || fj >= ngz) continue;
        bool alongx = d % 2 == 0;
        float hwc = alongx ? rhx[i] : rhz[j], hwr = alongx ? rhz[j] : rhx[i];
        Vector3 dv = V3(dx[d], 0, dz[d]), r = V3(-dz[d], 0, dx[d]);
        int group = id * 2 + (alongx ? 1 : 0);
        Vector3 C = V3(gx[i], 0, gz[j]);
        Vector3 along = Vector3Subtract(C, Vector3Scale(dv, hwc + 5.6f));
        Vector3 P = Vector3Add(along, Vector3Scale(r, hwr + 1.3f));
        float top = SWY + 6.4f;
        tube(GEO_SOLID, V3(P.x, SWY, P.z), V3(P.x, top, P.z), 0.13f, 0.09f, 8, M_STEEL, 1, 3);
        add_box(P.x - 0.18f, 0, P.z - 0.18f, P.x + 0.18f, 2, P.z + 0.18f);
        int nl = hwr > 7 ? 2 : 1;
        Vector3 tipc = Vector3Add(along, Vector3Scale(r, 1.75f + 3.5f * (nl - 1)));
        tube(GEO_SOLID, V3(P.x, top - 0.1f, P.z), V3(tipc.x, top, tipc.z), 0.08f, 0.05f, 6, M_STEEL, 1, 2);
        int face = alongx ? (dx[d] > 0 ? 2 : 3) : (dz[d] > 0 ? 0 : 1);
        for (int k = 0; k < nl; k++) {
            Vector3 hp = Vector3Add(along, Vector3Scale(r, 1.75f + 3.5f * k));
            signal_head(V3(hp.x, top - 0.75f, hp.z), face, group);
            tube(GEO_SOLID, V3(hp.x, top - 0.1f, hp.z), V3(hp.x, top - 0.2f, hp.z), 0.04f, 0.04f, 5, M_STEEL, 1, 1);
        }
    }
}

// ---------------------------------------------------------------- building pieces

static void parapet(Rect r, float y, float h, Mat wall, Mat top)
{
    float t = 0.3f;
    sbox(r.x0, y, r.z0, r.x1, y + h, r.z0 + t, wall, top);
    sbox(r.x0, y, r.z1 - t, r.x1, y + h, r.z1, wall, top);
    sbox(r.x0, y, r.z0 + t, r.x0 + t, y + h, r.z1 - t, wall, top);
    sbox(r.x1 - t, y, r.z0 + t, r.x1, y + h, r.z1 - t, wall, top);
}

static void cornice(Rect r, float y, Mat m)
{
    sbox(r.x0 - 0.3f, y - 0.35f, r.z0 - 0.3f, r.x1 + 0.3f, y + 0.12f, r.z1 + 0.3f, m, m);
}

static void roof_junk(Rect r, float y, float density)
{
    float w = r.x1 - r.x0, d = r.z1 - r.z0;
    int n = (int)(w * d / 60.0f * density) + 1;
    Mat metal = M(L_METAL, 0xb8bcc0), conc = M(L_CONCRETE, 0xc8c8c4);
    for (int k = 0; k < n && k < 8; k++) {
        float x = rf(r.x0 + 1.5f, r.x1 - 3), z = rf(r.z0 + 1.5f, r.z1 - 3);
        if (w < 5 || d < 5) break;
        switch (ri(4)) {
        case 0: sbox(x, y, z, x + 1.7f, y + 1.1f, z + 1.1f, metal, metal); break;
        case 1: cyl(GEO_SOLID, x + 0.6f, y, z + 0.6f, y + 1.3f, 0.35f, 8, metal, M(L_PLAIN, 0x707070)); break;
        case 2: sbox(x, y, z, x + 2.8f, y + 2.5f, z + 2.4f, conc, conc); break;
        default: {
            Mat leg = M_STEEL;
            for (int a = 0; a < 4; a++) tube(GEO_SOLID, V3(x + 0.1f + (a & 1) * 1.8f, y, z + 0.1f + (a >> 1) * 1.8f), V3(x + 0.1f + (a & 1) * 1.8f, y + 1.8f, z + 0.1f + (a >> 1) * 1.8f), 0.05f, 0.05f, 4, leg, 1, 1);
            cyl(GEO_SOLID, x + 1.0f, y + 1.8f, z + 1.0f, y + 3.4f, 1.15f, 10, M(L_WOOD, 0xb09070), M(L_PLAIN, 0x6a5a48));
            break; }
        }
    }
}

static void flat_roof(Rect r, float y, Mat wall, bool ledge)
{
    hq_part(GEO_SOLID, r.x0, r.z0, r.x1, r.z1, y, M(L_GRAVEL, 0xffffff));
    if (ledge) {
        cornice(r, y, tint(wall, 1.05f));
        parapet(r, y, 0.8f, wall, M(L_CONCRETE, 0xe8e8e8));
    }
}


// ---------------------------------------------------------------- roofs (clean versions)

static void quad_uv(int part, Vector3 *p, Vector3 n, float (*uv)[2], Mat m)
{
    Buf *b = buf_at(part, (p[0].x + p[2].x) * 0.5f, (p[0].z + p[2].z) * 0.5f);
    uint32_t id[4];
    for (int i = 0; i < 4; i++) id[i] = vert(b, p[i], n, uv[i][0], uv[i][1], m);
    tri(b, id[0], id[1], id[2]);
    tri(b, id[0], id[2], id[3]);
}

static void roof_face(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Mat m)
{
    Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(d, a));
    if (Vector3LengthSqr(n) < 1e-8f) n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
    n = Vector3Normalize(n);
    if (n.y < 0) n = Vector3Negate(n);
    float s = 1.0f / layer_size[m.layer], k = 1.15f;
    bool zf = fabsf(n.z) > fabsf(n.x);
    Vector3 p[4] = { a, b, c, d };
    float uv[4][2];
    for (int i = 0; i < 4; i++) { uv[i][0] = (zf ? p[i].x : p[i].z) * s; uv[i][1] = (zf ? p[i].z : p[i].x) * s * k; }
    quad_uv(GEO_SOLID, p, n, uv, m);
}

static void hip_roof(Rect r, float ye, float rise, float over, Mat m)
{
    bool alongx = r.x1 - r.x0 >= r.z1 - r.z0;
    float h = (alongx ? r.z1 - r.z0 : r.x1 - r.x0) * 0.5f;
    float x0 = r.x0 - over, x1 = r.x1 + over, z0 = r.z0 - over, z1 = r.z1 + over, yl = ye - over * rise / h;
    Vector3 ra, rb;
    if (alongx) { ra = V3(r.x0 + h, ye + rise, (r.z0 + r.z1) * 0.5f); rb = V3(r.x1 - h, ye + rise, ra.z); }
    else { ra = V3((r.x0 + r.x1) * 0.5f, ye + rise, r.z0 + h); rb = V3(ra.x, ye + rise, r.z1 - h); }
    Vector3 sw = V3(x0, yl, z1), se = V3(x1, yl, z1), nw = V3(x0, yl, z0), ne = V3(x1, yl, z0);
    roof_face(sw, se, rb, ra, m);
    roof_face(ne, nw, ra, rb, m);
    roof_face(nw, sw, ra, ra, m);
    roof_face(se, ne, rb, rb, m);
}

static void gable_roof(Rect r, float ye, float rise, float over, Mat m, Mat gable)
{
    bool alongx = r.x1 - r.x0 >= r.z1 - r.z0;
    float h = (alongx ? r.z1 - r.z0 : r.x1 - r.x0) * 0.5f, yl = ye - over * rise / h, yr = ye + rise;
    float x0 = r.x0 - over, x1 = r.x1 + over, z0 = r.z0 - over, z1 = r.z1 + over;
    float zc = (r.z0 + r.z1) * 0.5f, xc = (r.x0 + r.x1) * 0.5f;
    if (alongx) {
        roof_face(V3(x0, yl, z1), V3(x1, yl, z1), V3(x1, yr, zc), V3(x0, yr, zc), m);
        roof_face(V3(x1, yl, z0), V3(x0, yl, z0), V3(x0, yr, zc), V3(x1, yr, zc), m);
        for (int e = 0; e < 2; e++) {
            float x = e ? r.x1 : r.x0;
            quadv(GEO_SOLID, V3(x, ye, r.z0), V3(0, 0, r.z1 - r.z0), V3(0, yr - ye, 0), 0, 0, (r.z1 - r.z0) / 3, (yr - ye) / 3, gable);
        }
    } else {
        roof_face(V3(x1, yl, z0), V3(x1, yl, z1), V3(xc, yr, z1), V3(xc, yr, z0), m);
        roof_face(V3(x0, yl, z1), V3(x0, yl, z0), V3(xc, yr, z0), V3(xc, yr, z1), m);
        for (int e = 0; e < 2; e++) {
            float z = e ? r.z1 : r.z0;
            quadv(GEO_SOLID, V3(r.x0, ye, z), V3(r.x1 - r.x0, 0, 0), V3(0, yr - ye, 0), 0, 0, (r.x1 - r.x0) / 3, (yr - ye) / 3, gable);
        }
    }
}

static void sign_quad(int side, Rect r, float out, float a0, float a1, float y0, float y1, int idx, float glow)
{
    float aa0, aa1, c;
    side_span(r, side, &aa0, &aa1, &c);
    Vector2 n = side_n(side);
    float w = a1 - a0, h = y1 - y0;
    float k = fminf(0.5f, w / (16.0f * h));
    int layer = L_SIGN0 + idx / 8, row = idx % 8;
    float t_top = (row * 64 + 4) / 512.0f, t_bot = ((row + 1) * 64 - 4) / 512.0f;
    Mat m = mf(M(layer, 0xffffff), F_SELF, glow);
    float cc = c + (side < 2 ? n.y : n.x) * out;
    vf(GEO_SOLID, side, a0, a1, cc, y0, y1, m, 0.5f - k, 0.5f + k, t_bot, t_top);
}

static void canopy(Vector3 c, float R, int n, float hue)
{
    float warm = rf(-0.12f, 0.2f);
    for (int i = 0; i < n; i++) {
        float a = rf(0, 6.2832f), b = acosf(rf(-0.55f, 1.0f)), rr = R * powf(rf(0.45f, 1.0f), 0.5f);
        Vector3 p = V3(c.x + sinf(b) * cosf(a) * rr * 1.1f, c.y + cosf(b) * rr * 0.85f, c.z + sinf(b) * sinf(a) * rr * 1.1f);
        Vector3 nrm = Vector3Normalize(Vector3Subtract(p, Vector3Add(c, V3(0, -R * 0.2f, 0))));
        float ra = rf(0, 3.1416f), t = rf(0.35f, 1.0f);
        Vector3 right = V3(cosf(ra), rf(-0.3f, 0.3f), sinf(ra)), up = Vector3Normalize(Vector3CrossProduct(right, nrm));
        right = Vector3Normalize(Vector3CrossProduct(nrm, up));
        float g = hue * rf(0.85f, 1.1f);
        Mat m = mf(M(L_LEAF, ((int)fminf(255, 205 * g * (1 + warm)) << 16) | ((int)fminf(255, 230 * g) << 8) | (int)(170 * (1 - warm))), F_CUT, sat((p.y - c.y) / R * 0.5f + 0.55f));
        leaf_card(p, right, up, nrm, (1.15f * t + 0.5f) * (R / 4.5f > 0.5f ? 1.0f : 0.6f), m);
    }
}

// ---------------------------------------------------------------- houses

static const uint32_t pastel[10] = { 0xf2e6cf, 0xf0d8b8, 0xe8d0c0, 0xd8e4d0, 0xcfe0e8, 0xf0e0a8, 0xe9c8b8, 0xf4f0e8, 0xe0d4e8, 0xc8dcc8 };
static const uint32_t doorc[6] = { 0xa03030, 0x305a90, 0x2f6a45, 0x8a6a40, 0x303030, 0xd8c8a0 };
static const uint32_t shing[4] = { 0xd0ccc8, 0xb8a898, 0xa8a8a8, 0xc0b098 };

static void walls4(Rect r, float y0, float y1, Mat m)
{
    for (int s = 0; s < 4; s++) {
        float a0, a1, c;
        side_span(r, s, &a0, &a1, &c);
        facade(GEO_SOLID, s, a0, a1, c, y0, y1, m);
    }
}

static float front_offset(int s) { return (s == 0 || s == 2) ? -0.03f : 0.03f; }

static void house(Rect lot, int face)
{
    float W = lot_w(lot, face), D = lot_d(lot, face);
    float sb = rf(6, 8), garw = W >= 20.5f ? 5.6f : 0;
    float hd = fminf(D - sb - 4.5f, rf(10, 12.5f)), hw = fminf(W - 2.5f - garw, rf(10.5f, 14));
    if (hw < 7 || hd < 7.5f) return;
    bool gleft = ri(2);
    float a0 = (W - hw - garw) * 0.5f, ha0 = gleft ? a0 + garw : a0, ga0 = gleft ? a0 : a0 + hw;
    Rect hr = lrect(lot, face, ha0, sb, ha0 + hw, sb + hd);
    Rect gr = lrect(lot, face, ga0, sb - 0.8f, ga0 + garw, sb + 6.0f);
    int floors = ri(3) == 0 ? 2 : 1;
    float ye = SWY + floors * 3.4f;
    uint32_t col = pastel[ri(10)];
    Mat wall = mf(M(L_WIN_STUCCO, col), F_WIN, next_seed()), plain = M(L_STUCCO, col);
    walls4(hr, SWY, ye, wall);
    add_box(hr.x0, 0, hr.z0, hr.x1, ye, hr.z1);

    float fa0, fa1, fc;
    side_span(hr, face, &fa0, &fa1, &fc);
    int ncell = (int)fmaxf(2, roundf((fa1 - fa0) / cell_w(L_WIN_STUCCO)));
    float door_a = fa0 + (fa1 - fa0) * (ncell / 2) / ncell;
    float off = front_offset(face);
    vf(GEO_SOLID, face, door_a - 0.55f, door_a + 0.55f, fc + off, SWY, SWY + 2.15f, M(L_DOOR, doorc[ri(6)]), 0, 1, 0, 1);
    // porch slab
    Vector2 n = side_n(face);
    if (face < 2) sbox(door_a - 1.3f, SWY + 2.4f, fc + (n.y > 0 ? 0 : -1.4f), door_a + 1.3f, SWY + 2.52f, fc + (n.y > 0 ? 1.4f : 0), plain, plain);
    else sbox(fc + (n.x > 0 ? 0 : -1.4f), SWY + 2.4f, door_a - 1.3f, fc + (n.x > 0 ? 1.4f : 0), SWY + 2.52f, door_a + 1.3f, plain, plain);

    // garage
    float gh = 2.9f;
    if (garw > 0) {
        walls4(gr, SWY, SWY + gh, plain);
        float ga, gb, gc;
        side_span(gr, face, &ga, &gb, &gc);
        vf(GEO_SOLID, face, ga + 0.4f, gb - 0.4f, gc + off, SWY, SWY + 2.4f, M(L_GARAGE, 0xf4f0e8), 0, 1, 0, 1);
        add_box(gr.x0, 0, gr.z0, gr.x1, SWY + gh, gr.z1);
        hip_roof(gr, SWY + gh, 0.9f, 0.3f, M(L_SHINGLE, shing[ri(4)]));
    }

    // roofs
    Mat roof = ri(3) == 0 ? M(L_TILE, ri(2) ? 0xffffff : 0xd8b8a0) : M(L_SHINGLE, shing[ri(4)]);
    float rise = 2.2f + floors * 0.3f;
    if (ri(2)) hip_roof(hr, ye, rise, 0.5f, roof);
    else gable_roof(hr, ye, rise, 0.5f, roof, plain);
    if (ri(3) == 0) sbox(hr.x0 + 1, ye, hr.z0 + 1, hr.x0 + 1.8f, ye + rise + 1.0f, hr.z0 + 1.8f, M(L_BRICK, 0xffffff), M(L_PLAIN, 0x404040));

    // drive and walk
    Mat conc = M(L_SIDEWALK, 0xd8d8d0);
    Rect dr = lrect(lot, face, ga0 + 0.3f, 0, ga0 + garw - 0.3f + (garw > 0 ? 0 : 0.0f), sb - 0.8f), wr = lrect(lot, face, door_a - fa0 + ha0 - 0.6f, 0, door_a - fa0 + ha0 + 0.6f, sb);
    if (garw > 0) hq(GEO_GROUND, dr.x0, dr.z0, dr.x1, dr.z1, SWY + 0.004f, mf(conc, F_DECAL, 0));
    hq(GEO_GROUND, wr.x0, wr.z0, wr.x1, wr.z1, SWY + 0.004f, mf(conc, F_DECAL, 0));

    // fence along the street
    if (ri(2)) {
        float fa = ha0 - 0.5f, fb = ha0 + hw + 0.5f;
        Rect fr = lrect(lot, face, fa, 1.0f, fb, 1.0f);
        float f0, f1, fcc;
        side_span(fr, face, &f0, &f1, &fcc);
        float u1 = (f1 - f0) / 2.0f;
        vf(GEO_SOLID, face, f0, f1, fcc, SWY, SWY + 1.1f, mf(M(L_FENCE, 0xffffff), F_CUT, 0), 0, u1, 0, 1);
    }
    // yard
    float ta = rf(1.5f, W - 1.5f);
    if ((ta < ga0 - 1 || ta > ga0 + garw + 1) && (ta < ha0 - 1.5f || ta > ha0 + hw + 1.5f)) {
        Rect tr = lrect(lot, face, ta, rf(1.8f, sb - 1.5f), ta, 0);
        float tx = tr.x0, tz = tr.z0;
        if (ri(2)) palm(tx, tz, rf(8, 13)); else tree_broad(tx, tz, rf(6.5f, 9));
    }
    for (int k = 0; k < 2; k++) {
        float ba = ha0 + rf(0.5f, hw - 0.5f);
        Rect br = lrect(lot, face, ba, 0, ba, 0);
        Vector3 bp = side_pt(hr, face, ba - ha0, 0.7f, SWY + 0.5f);
        (void)br;
        canopy(bp, 0.9f, 7, rf(0.8f, 1.1f));
    }
    // lot side fences
    for (int e = 0; e < 2; e++) {
        Rect fr = lrect(lot, face, e ? W : 0, sb - 1.5f, e ? W : 0, D);
        float f0, f1, fc;
        side_span(fr, face < 2 ? 2 : 0, &f0, &f1, &fc);
        vfm(GEO_SOLID, face < 2 ? 2 : 0, f0, f1, fc, SWY, SWY + 1.8f, M(L_WOOD, 0xd8ccb8));
    }
    // back yard
    if (ri(7) == 0) {
        float ba = rf(2, W - 6);
        Rect sr = lrect(lot, face, ba, D - 4.5f, ba + 3, D - 1.5f);
        solid(sr.x0, SWY, sr.z0, sr.x1, SWY + 2.4f, sr.z1, M(L_WOOD, 0xd0b898), M(L_METAL, 0x908a84));
    }
    {
        float ba = rf(2, W - 2);
        Rect tr = lrect(lot, face, ba, D - 3, ba, 0);
        if (ri(2)) tree_broad(tr.x0, tr.z0, rf(6.5f, 9)); else palm(tr.x0, tr.z0, rf(8, 12));
    }
    // ac unit and solar
    if (ri(3) == 0) {
        Rect sr = lrect(hr, face, 0, 0, 0, 0);
        (void)sr;
    }
}

// ---------------------------------------------------------------- shops

static int rob_count, rob_in_block, rob_face;
static bool rob_block[MAXL][MAXL];
static int cur_i, cur_j;

static const uint32_t brickt[5] = { 0xffffff, 0xe8c8b0, 0xf0d0c0, 0xb8a090, 0xd0b098 };
static const uint32_t awn[6] = { 0xffffff, 0xffe0a0, 0xb0e0c0, 0xa0c0ff, 0xffb0b0, 0xe0e0e0 };

static void shop_front_open(Rect r, int face, float W, float D0, int sign, Mat wall_up)
{
    // r is the whole building rect. a, d are measured from the street edge of the building
    float y0 = SWY, hy = 4.2f;
    Rect lot = r;
    float door_a = W * 0.5f;
    Mat glass = M(L_PLAIN, 0xc0d8e0);
    Mat dark = M(L_CONCRETE, 0x707070);
    Mat inter = mf(M(L_PLAIN, 0xe8e0d0), F_INTER, 0), floor = mf(M(L_PANEL, 0xd8d0c0), F_INTER, 0);
    float depth = 8.0f;
    // front wall pieces
    Rect pl = lrect(lot, face, 0, 0, 0.5f, 0.4f), pr = lrect(lot, face, W - 0.5f, 0, W, 0.4f);
    solid(pl.x0, y0, pl.z0, pl.x1, y0 + hy, pl.z1, wall_up, wall_up);
    solid(pr.x0, y0, pr.z0, pr.x1, y0 + hy, pr.z1, wall_up, wall_up);
    Rect bl = lrect(lot, face, 0.5f, 0, door_a - 0.7f, 0.4f), br = lrect(lot, face, door_a + 0.7f, 0, W - 0.5f, 0.4f);
    solid(bl.x0, y0, bl.z0, bl.x1, y0 + 0.75f, bl.z1, dark, dark);
    solid(br.x0, y0, br.z0, br.x1, y0 + 0.75f, br.z1, dark, dark);
    add_box(bl.x0, y0, bl.z0, bl.x1, y0 + 3.5f, bl.z1);
    add_box(br.x0, y0, br.z0, br.x1, y0 + 3.5f, br.z1);
    Rect lin = lrect(lot, face, 0.5f, 0, W - 0.5f, 0.4f);
    sbox(lin.x0, y0 + 3.5f, lin.z0, lin.x1, y0 + hy, lin.z1, wall_up, wall_up);
    // glass
    float a0, a1, c;
    side_span(bl, face, &a0, &a1, &c);
    float cf = c + (face == 0 || face == 2 ? 0.2f : -0.2f) * 0;
    Rect gl = lrect(lot, face, 0.5f, 0.2f, door_a - 0.7f, 0.2f), gr = lrect(lot, face, door_a + 0.7f, 0.2f, W - 0.5f, 0.2f);
    float g0, g1, gc;
    side_span(gl, face, &g0, &g1, &gc);
    vf(GEO_GLASS, face, g0, g1, gc, y0 + 0.75f, y0 + 3.5f, glass, 0, 1, 0, 1);
    side_span(gr, face, &g0, &g1, &gc);
    vf(GEO_GLASS, face, g0, g1, gc, y0 + 0.75f, y0 + 3.5f, glass, 0, 1, 0, 1);
    (void)cf; (void)a0; (void)a1;
    // door frame
    Rect df0 = lrect(lot, face, door_a - 0.75f, 0, door_a - 0.65f, 0.4f), df1 = lrect(lot, face, door_a + 0.65f, 0, door_a + 0.75f, 0.4f);
    sbox(df0.x0, y0, df0.z0, df0.x1, y0 + 3.5f, df0.z1, M_BLACK, M_BLACK);
    sbox(df1.x0, y0, df1.z0, df1.x1, y0 + 3.5f, df1.z1, M_BLACK, M_BLACK);
    Rect dh = lrect(lot, face, door_a - 0.75f, 0, door_a + 0.75f, 0.4f);
    sbox(dh.x0, y0 + 2.4f, dh.z0, dh.x1, y0 + 2.5f, dh.z1, M_BLACK, M_BLACK);
    // interior
    Rect fl = lrect(lot, face, 0.4f, 0.4f, W - 0.4f, depth);
    hq(GEO_GROUND, fl.x0, fl.z0, fl.x1, fl.z1, y0 + 0.03f, floor);
    hq(GEO_GROUND, fl.x0, fl.z0, fl.x1, fl.z1, y0 + 3.65f, inter);
    Rect bw = lrect(lot, face, 0.4f, depth, W - 0.4f, depth);
    side_span(bw, face, &g0, &g1, &gc);
    vf(GEO_GROUND, face, g0, g1, gc, y0, y0 + 3.65f, inter, 0, 1, 0, 1);
    { Rect bc = lrect(lot, face, 0, depth, W, D0); add_box(bc.x0, y0, bc.z0, bc.x1, y0 + 4, bc.z1); }
    int ws = face < 2 ? 2 : 0;
    for (int e = 0; e < 2; e++) {
        float ea = e ? W - 0.4f : 0.4f;
        Rect sw = lrect(lot, face, ea, 0.4f, ea, depth);
        float s0, s1, sc;
        side_span(sw, ws + (face < 2 ? e : e), &s0, &s1, &sc);
        vf(GEO_GROUND, ws + e, s0, s1, sc, y0, y0 + 3.65f, inter, 0, 1, 0, 1);
        { Rect wc = lrect(lot, face, e ? W - 0.4f : 0, 0.4f, e ? W : 0.4f, depth); add_box(wc.x0, y0, wc.z0, wc.x1, y0 + 4, wc.z1); }
    }
    // shelves, counter, lights
    Rect sh = lrect(lot, face, 0.9f, depth - 0.9f, W - 0.9f, depth - 0.3f);
    uint32_t goods[4] = { 0xd04030, 0xe0b030, 0x3060b0, 0x40a060 };
    for (int k = 0; k < 4; k++) {
        float h0 = y0 + 0.2f + k * 0.55f;
        Mat gm = mf(M(L_PLAIN, goods[(k + sign) % 4]), F_INTER, 0);
        sbox(sh.x0, h0, sh.z0, sh.x1, h0 + 0.42f, sh.z1, gm, gm);
    }
    add_box(sh.x0, y0, sh.z0, sh.x1, y0 + 2.4f, sh.z1);
    Rect ct = lrect(lot, face, door_a - 2.2f, 4.0f, door_a + 2.2f, 4.9f);
    Mat cm = mf(M(L_WOOD, 0xe0c8a0), F_INTER, 0), ctop = mf(M(L_PLAIN, 0x404040), F_INTER, 0);
    solid(ct.x0, y0, ct.z0, ct.x1, y0 + 1.0f, ct.z1, cm, ctop);
    for (int k = 0; k < 3; k++) {
        Rect lg = lrect(lot, face, W * (0.2f + 0.3f * k) - 0.8f, 2.5f, W * (0.2f + 0.3f * k) + 0.8f, 3.1f);
        hq(GEO_GROUND, lg.x0, lg.z0, lg.x1, lg.z1, y0 + 3.62f, mf(M(L_PLAIN, 0xffffff), F_INTER | F_SELF, 3));
    }
    // sign and awning
    float sa0, sa1, sc2;
    side_span(r, face, &sa0, &sa1, &sc2);
    float sw = fminf(W - 1.0f, 7.2f), mid = (sa0 + sa1) * 0.5f;
    sign_quad(face, r, 0.06f, mid - sw * 0.5f, mid + sw * 0.5f, y0 + 3.5f, y0 + 4.1f, sign, 1.8f);
    // behind the counter
    Shop sp = { 0 };
    Vector2 n = side_n(face);
    Vector3 in = side_pt(lrect(lot, face, door_a, 5.6f, door_a, 5.6f), face, 0, 0, 0);
    Rect pt = lrect(lot, face, door_a, 5.6f, door_a, 5.6f);
    in = V3(pt.x0, SWY, pt.z0);
    sp.inside = in;
    sp.door = V3(in.x, SWY, in.z);
    Rect dp = lrect(lot, face, door_a, 0, door_a, 0);
    sp.door = V3(dp.x0 + n.x * 1.6f, SWY, dp.z0 + n.y * 1.6f);
    sp.yaw = atan2f(n.x, n.y);
    sp.kind = sign;
    sp.robbed_at = -1000;
    shops = realloc(shops, (nshops + 1) * sizeof(Shop));
    shops[nshops++] = sp;
    rob_count++;
}

static void shop_lot(Rect lot, int face, int k, int nlot)
{
    float W = lot_w(lot, face), D = lot_d(lot, face);
    bool corner = k == 0 || k == nlot - 1;
    (void)corner;
    int floors = 2 + ri(3);
    bool brick = ri(2);
    uint32_t col = brick ? brickt[ri(5)] : pastel[ri(10)];
    int layer = brick ? L_WIN_BRICK : L_WIN_STUCCO;
    float top = SWY + 4.2f + (floors - 1) * cell_h(layer);
    Mat wall = mf(M(layer, col), F_WIN, next_seed());
    Rect r = lot;
    bool open = rob_block[cur_i][cur_j] && !rob_in_block && rob_count < 10 && W > 7.0f && face == rob_face && k == nlot / 2;
    if (open) rob_in_block = 1;
    int sign = 4 + ri(28);
    sign = 4 + ((cur_i * 7 + cur_j * 3 + k * 5 + ri(3)) % 28);
    if (sign >= 32) sign = 31;

    for (int s = 0; s < 4; s++) {
        float a0, a1, c;
        side_span(r, s, &a0, &a1, &c);
        float y0 = SWY;
        if (s == face) {
            facade(GEO_SOLID, s, a0, a1, c, SWY + 4.2f, top, wall);
            if (!open) {
                int ncell = (int)fmaxf(1, roundf((a1 - a0) / cell_w(L_STOREFRONT)));
                Mat sm2 = mf(M(L_STOREFRONT, 0xffffff), F_WIN, next_seed());
                vf(GEO_SOLID, s, a0, a1, c, y0, y0 + 4.2f, sm2, 0, ncell / 2.0f, 0, 1);
            }
        } else facade(GEO_SOLID, s, a0, a1, c, y0, top, wall);
    }
    // roof
    Mat plainw = M(L_CONCRETE, 0xdcd8d0);
    flat_roof(r, top, plainw, true);
    roof_junk(r, top, 1.0f);
    if (open) {
        Mat wallup = M(layer == L_WIN_BRICK ? L_BRICK : L_STUCCO, col);
        shop_front_open(r, face, W, D, sign, wallup);
        // everything above and behind the open ground floor
        Rect up = lrect(lot, face, 0, 0, W, D);
        add_box(up.x0, SWY + 4.2f, up.z0, up.x1, top, up.z1);
        Rect back = lrect(lot, face, 0, 8.0f, W, D);
        add_box(back.x0, 0, back.z0, back.x1, SWY + 4.2f, back.z1);
    } else {
        add_box(r.x0, 0, r.z0, r.x1, top, r.z1);
        float a0, a1, c;
        side_span(r, face, &a0, &a1, &c);
        sign_quad(face, r, 0.06f, (a0 + a1) * 0.5f - fminf(W - 1, 7.2f) * 0.5f, (a0 + a1) * 0.5f + fminf(W - 1, 7.2f) * 0.5f, SWY + 3.3f, SWY + 4.05f, sign, 1.8f);
    }
    // awning
    {
        float a0, a1, c;
        side_span(r, face, &a0, &a1, &c);
        Vector2 n = side_n(face);
        float y = SWY + 3.25f, yl = SWY + 2.7f, pr = 1.6f;
        Mat am = M(L_AWNING, awn[ri(6)]);
        Vector3 w0, w1, f0, f1;
        if (face < 2) { w0 = V3(a0 + 0.3f, y, c + n.y * 0.06f); w1 = V3(a1 - 0.3f, y, w0.z); f0 = V3(w0.x, yl, c + n.y * pr); f1 = V3(w1.x, yl, f0.z); }
        else { w0 = V3(c + n.x * 0.06f, y, a0 + 0.3f); w1 = V3(w0.x, y, a1 - 0.3f); f0 = V3(c + n.x * pr, yl, w0.z); f1 = V3(f0.x, yl, w1.z); }
        float len = Vector3Distance(w0, w1);
        quad_pn(GEO_SOLID, w0, w1, f1, f0, Vector3Normalize(V3(n.x * 0.5f, 0.8f, n.y * 0.5f)), 0, 0, len / 1.0f, 1, mf(am, 0, 0));
        Vector3 fd = V3(0, 0.3f, 0);
        quad_pn(GEO_SOLID, f0, f1, Vector3Subtract(f1, fd), Vector3Subtract(f0, fd), V3(n.x, 0, n.y), 0, 0, len, 0.2f, am);
    }
}

// ---------------------------------------------------------------- towers and big buildings

static const uint32_t glasst[5] = { 0xffffff, 0xc8f0e0, 0xe8d8b8, 0xb8c8f0, 0xd0e0e8 };
static const uint32_t stonet[5] = { 0xe8dcc4, 0xd0d4d8, 0xe0c8b8, 0xf0ece4, 0xc8c4b8 };

static void tier(Rect r, float y0, int floors, int layer, uint32_t col, float seed)
{
    float h = floors * cell_h(layer);
    walls4(r, y0, y0 + h, mf(M(layer, col), F_WIN | (layer == L_WIN_GLASS ? F_GLASSY : 0), seed));
    add_box(r.x0, 0, r.z0, r.x1, y0 + h, r.z1);
}

static Rect inset(Rect r, float k) { return rect(r.x0 + k, r.z0 + k, r.x1 - k, r.z1 - k); }

static void tower(Rect lot)
{
    Rect fp = inset(lot, rf(1.0f, 2.5f));
    float w = fp.x1 - fp.x0, d = fp.z1 - fp.z0;
    float seed = next_seed();
    hq(GEO_GROUND, lot.x0, lot.z0, lot.x1, lot.z1, SWY + 0.003f, mf(M(L_SIDEWALK, 0xa8a8a2), F_DECAL, 0));
    int style = ri(10) < 5 ? 0 : (ri(3) ? 1 : 2);
    uint32_t col = style == 0 ? glasst[ri(5)] : style == 1 ? stonet[ri(5)] : brickt[ri(5)];
    int pfl = style == 2 ? 5 + ri(6) : 3 + ri(3);
    int player = style == 2 ? L_WIN_BRICK : L_WIN_CONC;
    float pcol = style == 2 ? col : stonet[ri(5)];
    float y = SWY;
    // ground floor: storefront all round
    for (int s = 0; s < 4; s++) {
        float a0, a1, c;
        side_span(fp, s, &a0, &a1, &c);
        int ncell = (int)fmaxf(1, roundf((a1 - a0) / cell_w(L_STOREFRONT)));
        vf(GEO_SOLID, s, a0, a1, c, y, y + 4.2f, mf(M(L_STOREFRONT, 0xffffff), F_WIN, seed + s), 0, ncell / 2.0f, 0, 1);
    }
    y += 4.2f;
    int pf = pfl - 1;
    walls4(fp, y, y + pf * cell_h(player), mf(M(player, (uint32_t)pcol), F_WIN, seed + 7));
    add_box(fp.x0, 0, fp.z0, fp.x1, y + pf * cell_h(player), fp.z1);
    y += pf * cell_h(player);
    cornice(fp, y, M(L_CONCRETE, 0xe0dcd4));
    flat_roof(fp, y, M(L_CONCRETE, 0xd8d4cc), false);
    if (style == 2) {   // older mid-rise: add a few floors in brick and finish
        int nf = 3 + ri(5);
        tier(inset(fp, 0.3f), y, nf, L_WIN_BRICK, col, seed + 3);
        y += nf * cell_h(L_WIN_BRICK);
        cornice(inset(fp, 0.3f), y, M(L_CONCRETE, 0xe8e0d0));
        flat_roof(inset(fp, 0.3f), y, M(L_CONCRETE, 0xd8d4cc), true);
        roof_junk(inset(fp, 0.3f), y, 1.2f);
        return;
    }
    float ins = fminf(rf(3.5f, 7.0f), fminf(w, d) * 0.22f);
    Rect s1 = inset(fp, ins);
    int layer = style == 0 ? L_WIN_GLASS : L_WIN_CONC;
    int nf = 10 + ri(26);
    int nf1 = nf > 20 ? (int)(nf * rf(0.55f, 0.7f)) : nf;
    tier(s1, y, nf1, layer, col, seed + 1);
    if (style == 0) {
        for (int cx = 0; cx < 4; cx++) {
            float px = (cx & 1) ? s1.x1 - 0.5f : s1.x0, pz = (cx >> 1) ? s1.z1 - 0.5f : s1.z0;
            sbox(px - 0.02f, y, pz - 0.02f, px + 0.52f, y + nf1 * cell_h(layer), pz + 0.52f, M(L_PLAIN, 0x2b3036), M(L_PLAIN, 0x2b3036));
        }
    }
    y += nf1 * cell_h(layer);
    Rect top = s1;
    if (nf > 20) {
        top = inset(s1, fminf(rf(2.5f, 4.5f), fminf(s1.x1 - s1.x0, s1.z1 - s1.z0) * 0.2f));
        cornice(s1, y, M(L_CONCRETE, 0xd8d8d8));
        flat_roof(s1, y, M(L_CONCRETE, 0xd0d0d0), false);
        int nf2 = nf - nf1;
        tier(top, y, nf2, layer, col, seed + 2);
        y += nf2 * cell_h(layer);
    }
    cornice(top, y, M(L_CONCRETE, 0xdcdcdc));
    flat_roof(top, y, M(L_CONCRETE, 0xd0d0d0), true);
    Rect mech = inset(top, fminf((top.x1 - top.x0) * 0.3f, 5));
    float mh = rf(3.5f, 6.5f);
    solid(mech.x0, y, mech.z0, mech.x1, y + mh, mech.z1, M(L_PANEL, 0xc0c4c8), M(L_PLAIN, 0x909498));
    float cx = (mech.x0 + mech.x1) * 0.5f, cz = (mech.z0 + mech.z1) * 0.5f;
    if (ri(3) == 0) hip_roof(inset(mech, -0.2f), y + mh, rf(3, 6), 0.2f, M(L_METAL, 0xa8c8b0));
    else {
        float ah = rf(10, 22);
        tube(GEO_SOLID, V3(cx, y + mh, cz), V3(cx, y + mh + ah, cz), 0.25f, 0.08f, 6, M_STEEL, 1, 4);
        sbox(cx - 0.12f, y + mh + ah, cz - 0.12f, cx + 0.12f, y + mh + ah + 0.4f, cz + 0.12f, mf(M(L_PLAIN, 0xff2010), F_SELF, 6), mf(M(L_PLAIN, 0xff2010), F_SELF, 6));
    }
    roof_junk(top, y, 0.8f);
}

static void hospital(Rect b, int face)
{
    float W = lot_w(b, face), D = lot_d(b, face);
    Rect lot = b;
    hq(GEO_GROUND, b.x0, b.z0, b.x1, b.z1, SWY, M(L_CONCRETE, 0xd0d0d0));
    Rect r = lrect(lot, face, 8, 7, W - 8, fminf(D - 8, 34));
    float seed = next_seed();
    int floors = 6;
    float y1 = SWY + floors * cell_h(L_WIN_CONC);
    walls4(r, SWY, y1, mf(M(L_WIN_CONC, 0xf4f4f4), F_WIN, seed));
    add_box(r.x0, 0, r.z0, r.x1, y1, r.z1);
    cornice(r, y1, M(L_CONCRETE, 0xf0f0f0));
    flat_roof(r, y1, M(L_CONCRETE, 0xf0f0f0), true);
    roof_junk(r, y1, 1.2f);
    // helipad
    Rect hp = lrect(r, face, 4, 4, 14, 14);
    hq(GEO_SOLID, hp.x0, hp.z0, hp.x1, hp.z1, y1 + 0.02f, M(L_PLAIN, 0x38423f));
    float hxm = (hp.x0 + hp.x1) * 0.5f, hzm = (hp.z0 + hp.z1) * 0.5f;
    Mat wm = M(L_PLAIN, 0xf4f4f0);
    hq(GEO_SOLID, hxm - 2.2f, hzm - 0.25f, hxm + 2.2f, hzm + 0.25f, y1 + 0.04f, wm);
    hq(GEO_SOLID, hxm - 2.2f, hzm - 2.0f, hxm - 1.7f, hzm + 2.0f, y1 + 0.04f, wm);
    hq(GEO_SOLID, hxm + 1.7f, hzm - 2.0f, hxm + 2.2f, hzm + 2.0f, y1 + 0.04f, wm);
    // front canopy and signs
    Rect cp = lrect(lot, face, W * 0.5f - 6, 1.5f, W * 0.5f + 6, 8);
    sbox(cp.x0, SWY + 4.0f, cp.z0, cp.x1, SWY + 4.3f, cp.z1, M(L_CONCRETE, 0xe8e8e8), M(L_CONCRETE, 0xe8e8e8));
    for (int k = 0; k < 4; k++) {
        Rect pp = lrect(lot, face, W * 0.5f - 5.6f + k * 3.6f, 2.0f, W * 0.5f - 5.4f + k * 3.6f, 2.3f);
        solid(pp.x0, SWY, pp.z0, pp.x1 + 0.3f, SWY + 4.0f, pp.z1 + 0.3f, M(L_CONCRETE, 0xf0f0f0), M(L_CONCRETE, 0xf0f0f0));
    }
    float a0, a1, c;
    side_span(r, face, &a0, &a1, &c);
    float mid = (a0 + a1) * 0.5f;
    sign_quad(face, r, 0.08f, mid - 6, mid + 6, SWY + 4.6f, SWY + 6.1f, 1, 2.0f);
    Vector2 n = side_n(face);
    float yc = SWY + 10.0f;
    vf(GEO_SOLID, face, mid + 8, mid + 13, c + (face < 2 ? n.y : n.x) * 0.1f, yc, yc + 5, mf(M(L_CROSS, 0xffffff), F_SELF, 1.5f), 0, 1, 0, 1);
    // pos on the sidewalk in front
    Vector3 fp = side_pt(b, face, W * 0.5f, 2.0f + 0 * D, SWY);
    fp = side_pt(b, face, W * 0.5f, SW - 2.0f, SWY);
    hospital_pos = fp;
}

static void police(Rect b, int face)
{
    float W = lot_w(b, face), D = lot_d(b, face);
    Rect lot = b;
    hq(GEO_GROUND, b.x0, b.z0, b.x1, b.z1, SWY, M(L_ASPHALT, 0xc0c0c0));
    Rect r = lrect(lot, face, 8, 4, W - 8, fminf(D * 0.5f, 22));
    float seed = next_seed();
    int floors = 3;
    float y1 = SWY + 4.2f + (floors - 1) * cell_h(L_WIN_BRICK);
    walls4(r, SWY + 4.2f, y1, mf(M(L_WIN_BRICK, 0xc8d0e0), F_WIN, seed));
    float a0, a1, c;
    for (int s = 0; s < 4; s++) {
        side_span(r, s, &a0, &a1, &c);
        int ncell = (int)fmaxf(1, roundf((a1 - a0) / cell_w(L_STOREFRONT)));
        vf(GEO_SOLID, s, a0, a1, c, SWY, SWY + 4.2f, mf(M(L_STOREFRONT, 0xc0c8d8), F_WIN, seed + s), 0, ncell / 2.0f, 0, 1);
    }
    add_box(r.x0, 0, r.z0, r.x1, y1, r.z1);
    cornice(r, y1, M(L_CONCRETE, 0xc8ccd4));
    flat_roof(r, y1, M(L_CONCRETE, 0xc8ccd4), true);
    roof_junk(r, y1, 1.0f);
    side_span(r, face, &a0, &a1, &c);
    float mid = (a0 + a1) * 0.5f;
    sign_quad(face, r, 0.08f, mid - 5, mid + 5, SWY + 4.5f, SWY + 6.1f, 0, 2.2f);
    Rect cp = lrect(lot, face, W * 0.5f - 3, 1.5f, W * 0.5f + 3, 4.0f);
    sbox(cp.x0, SWY + 3.6f, cp.z0, cp.x1, SWY + 3.85f, cp.z1, M(L_CONCRETE, 0x30407a), M(L_CONCRETE, 0x30407a));
    // flag pole
    Rect fpr = lrect(lot, face, W * 0.5f + 8, 3.0f, W * 0.5f + 8.2f, 3.2f);
    tube(GEO_SOLID, V3(fpr.x0, SWY, fpr.z0), V3(fpr.x0, SWY + 9, fpr.z0), 0.07f, 0.05f, 6, M_STEEL, 1, 3);
    // lot lines and cars bays
    Rect pk = lrect(lot, face, 4, D * 0.5f + 4, W - 4, D - 3);
    for (int k = 0; k < 8; k++) {
        float t = (pk.x1 - pk.x0 > pk.z1 - pk.z0) ? pk.x0 + k * (pk.x1 - pk.x0) / 8.0f : pk.z0 + k * (pk.z1 - pk.z0) / 8.0f;
        Mat pm = mf(M(L_EDGE, 0xffffff), F_CUT | F_DECAL, 0);
        if (pk.x1 - pk.x0 > pk.z1 - pk.z0) quadv(GEO_GROUND, V3(t, SWY + 0.02f, pk.z0), V3(0.12f, 0, 0), V3(0, 0, 5.4f), 0, 0, 1, 1.35f, pm);
        else quadv(GEO_GROUND, V3(pk.x0, SWY + 0.02f, t), V3(5.4f, 0, 0), V3(0, 0, 0.12f), 0, 0, 1.35f, 1, pm);
    }
    police_pos = side_pt(b, face, W * 0.5f, SW - 2.0f, SWY);
}

// ---------------------------------------------------------------- industrial

static void container(float x, float z, bool alongx, int layers)
{
    uint32_t cols[6] = { 0xb83a30, 0x2f5f9a, 0x3a7a4a, 0xd8902a, 0x808890, 0x6a3a7a };
    float l = ri(3) == 0 ? 12.2f : 6.1f, w = 2.44f, h = 2.59f;
    uint32_t col = cols[ri(6)];
    for (int k = 0; k < layers; k++) {
        float x0 = alongx ? x : x, x1 = alongx ? x + l : x + w, z0 = alongx ? z : z, z1 = alongx ? z + w : z + l;
        Mat side = M(L_CONTAINER, col), top = M(L_PLAIN, (col >> 1) & 0x7f7f7f);
        sbox(x0, SWY + k * h, z0, x1, SWY + (k + 1) * h, z1, side, top);
        if (k == 0) add_box(x0, 0, z0, x1, SWY + layers * h, z1);
        col = cols[ri(6)];
    }
}

static void warehouse(Rect r, int face)
{
    float h = rf(8.5f, 11.5f);
    Mat wall = M(L_METAL, ri(2) ? 0xd8dce0 : 0xc8d4c8);
    walls4(r, SWY, SWY + h, wall);
    Mat roofm = M(L_METAL, 0x98a0a8);
    gable_roof(r, SWY + h, 1.8f, 0.4f, roofm, wall);
    add_box(r.x0, 0, r.z0, r.x1, SWY + h, r.z1);
    float a0, a1, c;
    side_span(r, face, &a0, &a1, &c);
    int nd = 2 + ri(2);
    float off = front_offset(face);
    for (int k = 0; k < nd; k++) {
        float mid = a0 + (a1 - a0) * (k + 0.5f) / nd;
        vf(GEO_SOLID, face, mid - 2.2f, mid + 2.2f, c + off, SWY, SWY + 4.6f, M(L_ROLLER, 0xffffff), 0, 1, 0, 1);
    }
    // a band of windows high up on the long sides
    for (int s = 0; s < 4; s++) {
        side_span(r, s, &a0, &a1, &c);
        vf(GEO_SOLID, s, a0 + 2, a1 - 2, c + front_offset(s), SWY + h - 3.0f, SWY + h - 1.2f, mf(M(L_WIN_PLAIN, 0xffffff), F_WIN, next_seed()), 0, (a1 - a0 - 4) / 6.0f, 0.2f, 0.9f);
    }
}

static void chain_fence(float x0, float z0, float x1, float z1)
{
    // straight run, axis aligned
    float h = 2.4f;
    bool alongx = fabsf(x1 - x0) > fabsf(z1 - z0);
    float len = alongx ? fabsf(x1 - x0) : fabsf(z1 - z0);
    Mat cm = mf(M(L_CHAIN, 0xffffff), F_CUT, 0);
    if (alongx) vf(GEO_SOLID, 1, fminf(x0, x1), fmaxf(x0, x1), z0, SWY, SWY + h, cm, 0, len, 0, h);
    else vf(GEO_SOLID, 3, fminf(z0, z1), fmaxf(z0, z1), x0, SWY, SWY + h, cm, 0, len, 0, h);
    int n = (int)(len / 3.0f);
    for (int k = 0; k <= n; k++) {
        float t = k / (float)n;
        float px = mixf(x0, x1, t), pz = mixf(z0, z1, t);
        tube(GEO_SOLID, V3(px, SWY, pz), V3(px, SWY + h + 0.1f, pz), 0.05f, 0.05f, 5, M_STEEL, 1, 1);
    }
    if (alongx) add_box(fminf(x0, x1), 0, z0 - 0.05f, fmaxf(x0, x1), h, z0 + 0.05f);
    else add_box(x0 - 0.05f, 0, fminf(z0, z1), x0 + 0.05f, h, fmaxf(z0, z1));
}

static void block_industrial(Rect b)
{
    hq(GEO_GROUND, b.x0, b.z0, b.x1, b.z1, SWY, M(L_QUAY, 0xc8c8c8));
    float W = b.x1 - b.x0, D = b.z1 - b.z0;
    int face = cur_j == ngz - 2 ? 1 : 0;
    // two or three warehouses along the street side, containers behind
    float depth = fminf(D * 0.42f, 30);
    int n = 2;
    for (int k = 0; k < n; k++) {
        float gap = 6;
        float w0 = b.x0 + 6 + k * (W - 12) / n, w1 = b.x0 + 6 + (k + 1) * (W - 12) / n - gap;
        Rect r = face == 1 ? rect(w0, b.z1 - 5 - depth, w1, b.z1 - 5) : rect(w0, b.z0 + 5, w1, b.z0 + 5 + depth);
        warehouse(r, face);
    }
    Rect yard = face == 1 ? rect(b.x0 + 4, b.z0 + 4, b.x1 - 4, b.z1 - 5 - depth - 4) : rect(b.x0 + 4, b.z0 + 5 + depth + 4, b.x1 - 4, b.z1 - 4);
    for (float z = yard.z0; z < yard.z1 - 14; z += 12.5f)
        for (float x = yard.x0; x < yard.x1 - 16; x += 15.0f) {
            if (ri(4) == 0) continue;
            container(x + rf(0, 1.5f), z + rf(0, 1), true, 1 + ri(3));
        }
    // fence around the block
    chain_fence(b.x0 + 1, b.z0 + 1, b.x1 - 1, b.z0 + 1);
    chain_fence(b.x0 + 1, b.z1 - 1, b.x1 - 1, b.z1 - 1);
    chain_fence(b.x0 + 1, b.z0 + 1, b.x0 + 1, b.z1 - 1);
    chain_fence(b.x1 - 1, b.z0 + 1, b.x1 - 1, b.z1 - 1);
}

// ---------------------------------------------------------------- park

static void block_park(Rect b)
{
    hq(GEO_GROUND, b.x0, b.z0, b.x1, b.z1, SWY, M(L_GRASS, 0xffffff));
    float cx = (b.x0 + b.x1) * 0.5f, cz = (b.z0 + b.z1) * 0.5f;
    Mat path = mf(M(L_SIDEWALK, 0xd8d0c0), F_DECAL, 0);
    hq(GEO_GROUND, cx - 1.6f, b.z0, cx + 1.6f, b.z1, SWY + 0.004f, path);
    hq(GEO_GROUND, b.x0, cz - 1.6f, b.x1, cz + 1.6f, SWY + 0.004f, path);
    hq(GEO_GROUND, cx - 8, cz - 8, cx + 8, cz + 8, SWY + 0.005f, mf(M(L_SIDEWALK, 0xe8e0d0), F_DECAL, 0));
    // fountain
    Mat stone = M(L_CONCRETE, 0xe0dcd0);
    cyl(GEO_SOLID, cx, SWY, cz, SWY + 0.7f, 3.2f, 16, stone, M(L_PLAIN, 0x3a90a8));
    cyl(GEO_SOLID, cx, SWY, cz, SWY + 1.8f, 0.5f, 10, stone, stone);
    cyl(GEO_SOLID, cx, SWY + 1.8f, cz, SWY + 2.0f, 1.2f, 12, stone, M(L_PLAIN, 0x3a90a8));
    add_box(cx - 3.2f, 0, cz - 3.2f, cx + 3.2f, 0.8f, cz + 3.2f);
    for (int k = 0; k < 40; k++) {
        float x = rf(b.x0 + 3, b.x1 - 3), z = rf(b.z0 + 3, b.z1 - 3);
        if (fabsf(x - cx) < 3.5f || fabsf(z - cz) < 3.5f) continue;
        if (ri(3) == 0) palm(x, z, rf(8, 13)); else tree_broad(x, z, rf(7, 11));
    }
    for (int k = 0; k < 4; k++) {
        float s = k % 2 ? 1.0f : -1.0f;
        bench(cx + s * 9.5f, cz + (k < 2 ? 3.0f : -3.0f), false);
        bench(cx + (k < 2 ? 3.0f : -3.0f), cz + s * 9.5f, true);
    }
    for (int k = 0; k < 6; k++) {
        float x = cx + (k < 3 ? -1 : 1) * rf(14, b.x1 - cx - 6), z = cz + 2.2f;
        bench(x, z, true);
        trash_can(x + 2, z);
    }
    for (int a = -1; a <= 1; a += 2) {
        street_lamp(cx + a * 12, cz - 2.2f, 0, -1);
        street_lamp(cx - 2.2f, cz + a * 12, -1, 0);
    }
    // pavilion
    float px = b.x0 + (b.x1 - b.x0) * 0.25f, pz = b.z0 + (b.z1 - b.z0) * 0.25f;
    for (int k = 0; k < 6; k++) {
        float a = k * 1.0472f;
        tube(GEO_SOLID, V3(px + cosf(a) * 2.6f, SWY, pz + sinf(a) * 2.6f), V3(px + cosf(a) * 2.6f, SWY + 2.8f, pz + sinf(a) * 2.6f), 0.1f, 0.1f, 6, M(L_PLAIN, 0xf0f0e8), 1, 1);
    }
    tube(GEO_SOLID, V3(px, SWY + 2.8f, pz), V3(px, SWY + 4.4f, pz), 3.3f, 0.1f, 12, M(L_METAL, 0x6a9a80), 2, 1);
    cyl(GEO_SOLID, px, SWY, pz, SWY + 0.2f, 3.0f, 12, stone, stone);
    add_box(px - 3, 0, pz - 3, px + 3, 0.3f, pz + 3);
}

// ---------------------------------------------------------------- districts

typedef void (*LotFn)(Rect lot, int face, int k, int n);

static void lots_ring(Rect b, float depth, float wmin, float wmax, LotFn fn)
{
    for (int side = 0; side < 4; side++) {
        float len = side < 2 ? b.x1 - b.x0 : b.z1 - b.z0, start = 0;
        if (side >= 2) { len -= 2 * depth; start = depth; }
        int n = (int)fmaxf(1, roundf(len / ((wmin + wmax) * 0.5f)));
        float wsum = 0, wts[24];
        for (int k = 0; k < n; k++) { wts[k] = rf(0.8f, 1.25f); wsum += wts[k]; }
        float p = 0;
        for (int k = 0; k < n; k++) {
            float w = len * wts[k] / wsum;
            Rect lot;
            switch (side) {
            case 0: lot = rect(b.x0 + p, b.z0, b.x0 + p + w, b.z0 + depth); break;
            case 1: lot = rect(b.x0 + p, b.z1 - depth, b.x0 + p + w, b.z1); break;
            case 2: lot = rect(b.x0, b.z0 + start + p, b.x0 + depth, b.z0 + start + p + w); break;
            default: lot = rect(b.x1 - depth, b.z0 + start + p, b.x1, b.z0 + start + p + w); break;
            }
            fn(lot, side, k, n);
            p += w;
        }
    }
}

static void lot_house(Rect lot, int face, int k, int n) { house(lot, face); }

static void block_residential(Rect b)
{
    hq(GEO_GROUND, b.x0, b.z0, b.x1, b.z1, SWY, M(L_GRASS, 0xffffff));
    float D = fminf(36, fminf(b.x1 - b.x0, b.z1 - b.z0) * 0.4f);
    lots_ring(b, D, 18, 24, lot_house);
    for (int k = 0; k < 5; k++) {
        float x = rf(b.x0 + D, b.x1 - D), z = rf(b.z0 + D, b.z1 - D);
        if (ri(2)) tree_broad(x, z, rf(6, 9)); else palm(x, z, rf(8, 12));
    }
}

static void lot_shop(Rect lot, int face, int k, int n) { shop_lot(lot, face, k, n); }

static void block_shops(Rect b, bool beach)
{
    hq(GEO_GROUND, b.x0, b.z0, b.x1, b.z1, SWY, M(L_ASPHALT, 0xd0d0d0));
    float D = 20;
    float inner_w = b.x1 - b.x0 - 2 * D, inner_d = b.z1 - b.z0 - 2 * D;
    lots_ring(b, D, 9, 15, lot_shop);
    if (inner_w > 6 && inner_d > 6) {
        Rect in = rect(b.x0 + D, b.z0 + D, b.x1 - D, b.z1 - D);
        hq(GEO_GROUND, in.x0, in.z0, in.x1, in.z1, SWY, M(L_ASPHALT, 0xc8c8c8));
        for (float x = in.x0 + 2; x < in.x1 - 2; x += 2.8f) {
            Mat pm = mf(M(L_EDGE, 0xffffff), F_CUT | F_DECAL, 0);
            quadv(GEO_GROUND, V3(x, SWY + 0.02f, in.z0 + 1), V3(0.12f, 0, 0), V3(0, 0, 5), 0, 0, 1, 1.2f, pm);
        }
        (void)beach;
    }
}

static void block_downtown(Rect b)
{
    hq(GEO_GROUND, b.x0, b.z0, b.x1, b.z1, SWY, M(L_SIDEWALK, 0xb0b0aa));
    float W = b.x1 - b.x0, D = b.z1 - b.z0;
    int nx = W > 85 ? 2 : 1, nz = D > 85 ? 2 : 1;
    for (int a = 0; a < nx; a++)
        for (int c = 0; c < nz; c++) {
            Rect lot = rect(b.x0 + a * W / nx + (a ? 2.5f : 0), b.z0 + c * D / nz + (c ? 2.5f : 0), b.x0 + (a + 1) * W / nx - (a < nx - 1 ? 2.5f : 0), b.z0 + (c + 1) * D / nz - (c < nz - 1 ? 2.5f : 0));
            tower(lot);
        }
}

static void block_special(Rect b, char kind)
{
    int face = kind == 'X' ? 3 : 2;   // police faces east to the avenue, hospital west
    if (kind == 'H') face = 1;
    if (kind == 'H') hospital(b, face); else police(b, face);
}

// ---------------------------------------------------------------- coast, terrain, water

static float coast_x(float z) { return 505 + 18 * sinf(z * 0.011f + 1) + 9 * sinf(z * 0.031f); }
static float coast_z(float x)
{
    float w = 500 + 16 * sinf(x * 0.013f + 0.5f) + 8 * sinf(x * 0.037f + 2);
    return mixf(484, w, sm(120, 220, x));
}
float world_coast(float x, float z) { return fminf(coast_x(z) - x, coast_z(x) - z); }

static float th(float x, float z)
{
    float d = world_coast(x, z);
    if (d >= 0) return 0;
    return -1.2f * sm(0, 2.5f, -d) - 3.0f * sat((-d - 2.5f) / 25);
}

static void terrain_cell(float x, float z, float s)
{
    float x1 = x + s, z1 = z + s;
    if (x >= xmin_o && x1 <= xmax_o && z >= zmin_o && z1 <= zmax_o) return;
    float cx[4] = { x, x1, x1, x }, cz[4] = { z, z, z1, z1 }, h[4], dmax = -1e9f;
    for (int k = 0; k < 4; k++) {
        h[k] = th(cx[k], cz[k]);
        dmax = fmaxf(dmax, world_coast(cx[k], cz[k]));
    }
    if (dmax < -26) return;
    float mx = (x + x1) * 0.5f, mz = (z + z1) * 0.5f;
    bool quay = mz >= zmax_o && x1 <= 152 && x >= xmin_o;
    float w = quay ? 0 : sm(60, 150, world_coast(mx, mz));
    float nz = pnoise(mx * 0.03f, mz * 0.03f, 1 << 16, 77) * 0.35f + pnoise(mx * 0.2f, mz * 0.2f, 1 << 16, 78) * 0.15f;
    float k = 0.88f + nz * 0.4f;
    float r = mixf(1.0f, 0.82f, w) * k, g = mixf(1.0f, 0.74f, w) * k, b = mixf(1.0f, 0.5f, w) * k;
    Mat m = quay ? M(L_QUAY, 0xffffff)
                 : mf(M(L_SAND, ((int)(sat(r) * 255) << 16) | ((int)(sat(g) * 255) << 8) | (int)(sat(b) * 255)), F_BLEND, w);
    Buf *bf = buf_at(GEO_GROUND, mx, mz);
    float us = 1.0f / layer_size[m.layer];
    uint32_t id[4];
    for (int q = 0; q < 4; q++) {
        float hx = th(cx[q] + 1, cz[q]) - th(cx[q] - 1, cz[q]), hz = th(cx[q], cz[q] + 1) - th(cx[q], cz[q] - 1);
        id[q] = vert(bf, V3(cx[q], h[q], cz[q]), Vector3Normalize(V3(-hx * 0.5f, 1, -hz * 0.5f)), cx[q] * us, cz[q] * us, m);
    }
    tri(bf, id[0], id[3], id[2]);
    tri(bf, id[0], id[2], id[1]);
}

static void build_terrain(void)
{
    for (float z = CH0; z < -CH0; z += 32)
        for (float x = CH0; x < -CH0; x += 32) {
            bool near = x > xmin_o - 330 && x < xmax_o + 330 && z > zmin_o - 330 && z < zmax_o + 330;
            if (near) {
                for (int a = 0; a < 4; a++)
                    for (int b = 0; b < 4; b++) terrain_cell(x + a * 8, z + b * 8, 8);
            } else terrain_cell(x, z, 32);
        }
}

static void water_cell(float x, float z, float s)
{
    float cx[4] = { x, x + s, x + s, x }, cz[4] = { z, z, z + s, z + s }, dep[4], dmin = 1e9f;
    for (int k = 0; k < 4; k++) {
        dep[k] = fmaxf(0, WATER_Y - th(cx[k], cz[k]));
        dmin = fminf(dmin, world_coast(cx[k], cz[k]));
    }
    if (dmin > 8) return;
    Buf *bf = buf_at(GEO_WATER, x + s * 0.5f, z + s * 0.5f);
    Mat m = M(L_PLAIN, 0xffffff);
    uint32_t id[4];
    for (int q = 0; q < 4; q++) {
        m.seed = dep[q];
        id[q] = vert(bf, V3(cx[q], WATER_Y, cz[q]), V3(0, 1, 0), 0, 0, m);
    }
    tri(bf, id[0], id[3], id[2]);
    tri(bf, id[0], id[2], id[1]);
}

static void build_water(void)
{
    for (float z = CH0; z < -CH0; z += 16)
        for (float x = CH0; x < -CH0; x += 16) {
            float d = world_coast(x + 8, z + 8);
            if (d < -60) water_cell(x, z, 16);
            else if (d < 40) for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) water_cell(x + a * 8, z + b * 8, 8);
        }
    Mat m = M(L_PLAIN, 0xffffff);
    m.seed = 12;
    quadv(GEO_WATER, V3(-CH0, WATER_Y, 8000), V3(8000 + CH0, 0, 0), V3(0, 0, -16000), 0, 0, 1, 1, m);
    quadv(GEO_WATER, V3(-8000, WATER_Y, 8000), V3(8000 - CH0, 0, 0), V3(0, 0, CH0 - 8000), 0, 0, 1, 1, m);
}

// ---------------------------------------------------------------- beach, boardwalk, docks

typedef struct { float x0, z0, x1, z1; } PierRect;
static PierRect piers[3];

static void plank_strip(float x0, float z0, float x1, float z1, float y, bool along_x)
{
    Mat m = M(L_WOOD, 0xb8aa98);
    float s = 1.0f / layer_size[L_WOOD];
    if (along_x) quadv(GEO_GROUND, V3(x0, y, z1), V3(x1 - x0, 0, 0), V3(0, 0, z0 - z1), z1 * s, x0 * s, z0 * s, x1 * s, m);
    else quadv(GEO_GROUND, V3(x0, y, z1), V3(x1 - x0, 0, 0), V3(0, 0, z0 - z1), x0 * s, z1 * s, x1 * s, z0 * s, m);
}

static void lifeguard_tower(float x, float z)
{
    Mat wd = M(L_WOOD, 0xf0f0f0), red = M(L_PLAIN, 0xd03028), wh = M(L_PLAIN, 0xf4f4f0);
    float h = 2.3f;
    for (int a = 0; a < 4; a++) {
        float px = x + ((a & 1) ? 1.0f : -1.0f), pz = z + ((a >> 1) ? 1.0f : -1.0f);
        tube(GEO_SOLID, V3(px, 0, pz), V3(px * 0.9f + x * 0.1f, h, pz * 0.9f + z * 0.1f), 0.1f, 0.08f, 6, wd, 1, 2);
    }
    sbox(x - 1.5f, h, z - 1.5f, x + 1.5f, h + 0.12f, z + 1.5f, wd, wd);
    sbox(x - 1.4f, h + 0.12f, z - 1.4f, x + 1.4f, h + 1.2f, z - 1.25f, wh, wh);
    sbox(x - 1.4f, h + 0.12f, z - 1.4f, x - 1.25f, h + 1.2f, z + 1.0f, wh, wh);
    sbox(x + 1.25f, h + 0.12f, z - 1.4f, x + 1.4f, h + 1.2f, z + 1.0f, wh, wh);
    sbox(x - 1.7f, h + 2.3f, z - 1.7f, x + 1.7f, h + 2.45f, z + 1.7f, red, red);
    for (int a = 0; a < 4; a++) {
        float px = x + ((a & 1) ? 1.5f : -1.5f), pz = z + ((a >> 1) ? 1.5f : -1.5f);
        tube(GEO_SOLID, V3(px, h + 0.12f, pz), V3(px, h + 2.3f, pz), 0.05f, 0.05f, 4, wd, 1, 1);
    }
    sbox(x - 1.2f, h + 0.9f, z - 1.4f, x + 1.2f, h + 1.0f, z - 1.2f, red, red);
    sbox(x - 0.5f, 0, z + 1.4f, x + 0.5f, h, z + 2.6f, wd, wd);
    add_box(x - 1.4f, 0, z - 1.4f, x + 1.4f, 3.5f, z + 1.4f);
}

static void umbrella(float x, float z, uint32_t col)
{
    tube(GEO_SOLID, V3(x, 0, z), V3(x, 2.3f, z), 0.03f, 0.03f, 5, M_STEEL, 1, 1);
    tube(GEO_SOLID, V3(x, 2.1f, z), V3(x, 2.45f, z), 1.35f, 0.05f, 10, M(L_AWNING, col), 3, 1);
    float a = rf(0, 6.28f);
    float tx = x + cosf(a) * 1.5f, tz = z + sinf(a) * 1.5f;
    hq(GEO_GROUND, tx - 0.45f, tz - 0.9f, tx + 0.45f, tz + 0.9f, 0.025f, mf(M(L_PLAIN, 0x2060c0 + ((uint32_t)ri(0x60) << 16) + ((uint32_t)ri(0x60) << 8)), F_DECAL, 0));
}

static void crane(float x, float z)
{
    Mat o = M(L_PLAIN, 0xd8581e), g = M(L_PLAIN, 0x8a9094);
    float h = 28;
    for (int a = 0; a < 4; a++) {
        float px = x + ((a & 1) ? 18 : 0), pz = z + ((a >> 1) ? 12 : 0);
        solid(px, 0, pz, px + 1.1f, h, pz + 1.1f, o, o);
    }
    sbox(x, h - 1.5f, z, x + 19.1f, h, z + 1.1f, g, g);
    sbox(x, h - 1.5f, z + 12, x + 19.1f, h, z + 13.1f, g, g);
    sbox(x, h - 1.5f, z, x + 1.1f, h, z + 13.1f, g, g);
    sbox(x + 18, h - 1.5f, z, x + 19.1f, h, z + 13.1f, g, g);
    sbox(x + 7, h, z + 4, x + 12, h + 3.5f, z + 9, o, o);
    sbox(x + 8.5f, h + 0.4f, z + 9, x + 10.5f, h + 2.0f, z + 62, o, g);
    sbox(x + 8.5f, h + 0.4f, z - 25, x + 10.5f, h + 2.0f, z + 4, o, g);
    sbox(x + 9.3f, h - 10, z + 55, x + 9.7f, h + 0.4f, z + 55.4f, M_STEEL, M_STEEL);
}

static void rail_run(float x0, float z0, float x1, float z1)
{
    bool alongx = fabsf(x1 - x0) > fabsf(z1 - z0);
    float len = alongx ? fabsf(x1 - x0) : fabsf(z1 - z0);
    Mat wd = M(L_WOOD, 0xe8e0d0);
    int n = (int)(len / 4.0f);
    for (int k = 0; k <= n; k++) {
        float t = k / (float)n, px = mixf(x0, x1, t), pz = mixf(z0, z1, t);
        if (fmodf(k, 22) > 19) continue;
        sbox(px - 0.06f, SWY, pz - 0.06f, px + 0.06f, SWY + 1.05f, pz + 0.06f, wd, wd);
        if (k < n && fmodf(k, 22) < 18) {
            float qx = mixf(x0, x1, (k + 1) / (float)n), qz = mixf(z0, z1, (k + 1) / (float)n);
            sbox(fminf(px, qx), SWY + 0.95f, fminf(pz, qz) - 0.04f, fmaxf(px, qx) + 0.0f, SWY + 1.03f, fmaxf(pz, qz) + 0.04f, wd, wd);
        }
    }
}

static void build_outside(void)
{
    float bw = 9.0f;
    plank_strip(xmax_o, zmin_o, xmax_o + bw, zmax_o + bw, SWY + 0.01f, false);
    plank_strip(152, zmax_o, xmax_o, zmax_o + bw, SWY + 0.01f, true);
    Mat edge = M(L_WOOD, 0xb8a080);
    vfm(GEO_GROUND, 3, zmin_o, zmax_o + bw, xmax_o + bw, 0, SWY + 0.01f, edge);
    vfm(GEO_GROUND, 1, 152, xmax_o, zmax_o + bw, 0, SWY + 0.01f, edge);
    rail_run(xmax_o + bw - 0.2f, zmin_o + 8, xmax_o + bw - 0.2f, zmax_o + bw - 6);
    rail_run(152, zmax_o + bw - 0.2f, xmax_o, zmax_o + bw - 0.2f);
    for (float z = zmin_o + 20; z < zmax_o; z += 19) {
        palm(xmax_o + bw + 2.5f, z + rf(-2, 2), rf(9, 14));
        if (fmodf(z - zmin_o, 76) < 19) { street_lamp(xmax_o + bw - 1.0f, z + 9, -1, 0); bench(xmax_o + bw - 1.6f, z + 4, false); }
    }
    for (float x = 170; x < xmax_o; x += 19) {
        palm(x, zmax_o + bw + 2.5f, rf(9, 14));
        if (fmodf(x, 76) < 19) { street_lamp(x, zmax_o + bw - 1.0f, 0, -1); bench(x + 4, zmax_o + bw - 1.6f, true); }
    }
    // sand: towers, umbrellas
    for (int k = 0; k < 6; k++) lifeguard_tower(xmax_o + 42 + rf(-4, 4), zmin_o + 120 + k * 170);
    lifeguard_tower(xmax_o + 40, zmax_o + 20);
    lifeguard_tower(260, zmax_o + 44);
    lifeguard_tower(340, zmax_o + 44);
    uint32_t uc[5] = { 0xffffff, 0xffe0a0, 0xffb0b0, 0xa0c8ff, 0xffd8d0 };
    for (int c = 0; c < 18; c++) {
        float cx = xmax_o + rf(22, 62), cz = zmin_o + 60 + c * 55 + rf(-10, 10);
        if (cz > zmax_o) continue;
        for (int k = 0; k < 4; k++) umbrella(cx + rf(-9, 9), cz + rf(-9, 9), uc[ri(5)]);
    }
    for (int c = 0; c < 8; c++) {
        float cx = 190 + c * 28, cz = zmax_o + rf(24, 54);
        for (int k = 0; k < 3; k++) umbrella(cx + rf(-6, 6), cz + rf(-6, 6), uc[ri(5)]);
    }
    // docks
    float qx0 = xmin_o, qx1 = 152, edge_z = 484;
    for (float x = qx0; x < qx1; x += 40) {
        float xe = fminf(x + 40, qx1);
        sbox(x, -1.5f, edge_z - 0.2f, xe, 0, edge_z + 1.3f, M(L_QUAY, 0xb0b0b0), M(L_QUAY, 0xffffff));
        for (float bx = x + 10; bx < xe; bx += 20) cyl(GEO_SOLID, bx, 0, edge_z - 0.8f, 0.55f, 0.28f, 8, M_BLACK, M_BLACK);
    }
    float pxs[3] = { -440, -230, 10 };
    for (int p = 0; p < 3; p++) {
        piers[p] = (PierRect){ pxs[p], edge_z, pxs[p] + 12, edge_z + 56 };
        plank_strip(pxs[p], edge_z, pxs[p] + 12, edge_z + 56, 0.05f, false);
        vfm(GEO_GROUND, 2, edge_z, edge_z + 56, pxs[p], -1.4f, 0.05f, edge);
        vfm(GEO_GROUND, 3, edge_z, edge_z + 56, pxs[p] + 12, -1.4f, 0.05f, edge);
        vfm(GEO_GROUND, 1, pxs[p], pxs[p] + 12, edge_z + 56, -1.4f, 0.05f, edge);
        for (float z = edge_z + 4; z < edge_z + 56; z += 6) {
            tube(GEO_SOLID, V3(pxs[p] + 0.3f, -2.5f, z), V3(pxs[p] + 0.3f, 0.7f, z), 0.18f, 0.18f, 6, M(L_BARK, 0x706050), 1, 2);
            tube(GEO_SOLID, V3(pxs[p] + 11.7f, -2.5f, z), V3(pxs[p] + 11.7f, 0.7f, z), 0.18f, 0.18f, 6, M(L_BARK, 0x706050), 1, 2);
        }
        if (p == 1) solid(pxs[p] + 3, 0.05f, edge_z + 40, pxs[p] + 9, 3.8f, edge_z + 52, M(L_METAL, 0xc84a3a), M(L_METAL, 0x888888));
    }
    crane(-340, 438);
    crane(-120, 438);
    for (int k = 0; k < 40; k++) {
        float x = rf(xmin_o + 10, 130), z = rf(zmax_o + 14, 410 + 20);
        if (z > 466) continue;
        container(x, z, ri(2), 1 + ri(3));
    }
    for (float x = xmin_o + 30; x < 150; x += 60) street_lamp(x, 470, 0, 1);
}

// ---------------------------------------------------------------- streets: props along block edges

static void block_edge_props(int bi, int bj, Rect b, char kind)
{
    bool coastal = bi >= 7 || bj >= 7 || kind == 'B';
    for (int s = 0; s < 4; s++) {
        float len = s < 2 ? b.x1 - b.x0 : b.z1 - b.z0;
        Vector2 n = side_n(s);
        float lamp_a[8];
        int nl = 0;
        for (float a = 14; a < len - 14 && nl < 8; a += 38) {
            lamp_a[nl++] = a;
            Vector3 p = side_pt(b, s, a, 3.1f, SWY);
            street_lamp(p.x, p.z, (int)n.x, (int)n.y);
        }
        if (kind != 'I' && kind != 'P') {
            for (float a = 9; a < len - 9; a += 17) {
                float aa = a + rf(-2, 2);
                bool ok = true;
                for (int k = 0; k < nl; k++) if (fabsf(aa - lamp_a[k]) < 4.5f) ok = false;
                if (!ok) continue;
                Vector3 p = side_pt(b, s, aa, 2.9f, SWY);
                if (coastal || ri(3) == 0) palm(p.x, p.z, rf(8, 12));
                else tree_broad(p.x, p.z, kind == 'D' ? rf(5.5f, 7.5f) : rf(6.5f, 9));
            }
        }
        if ((bi + bj + s) % 2 == 0) { Vector3 p = side_pt(b, s, 4, 3.4f, SWY); hydrant(p.x, p.z); }
        if (kind == 'S' || kind == 'B' || kind == 'D') {
            Vector3 p = side_pt(b, s, len * 0.5f + rf(-6, 6), 2.2f, SWY);
            bench(p.x, p.z, s < 2);
            Vector3 q = side_pt(b, s, len * 0.5f + 4, 3.4f, SWY);
            trash_can(q.x, q.z);
        }
        if (kind == 'R') {
            Vector3 prev = { 0 };
            bool have = false;
            for (float a = 20; a < len - 20; a += 40) {
                Vector3 p = side_pt(b, s, a + 20, 3.7f, SWY);
                power_pole(p.x, p.z, s < 2);
                float topy = SWY + 8.4f;
                if (have) for (int w = -1; w <= 1; w++) {
                    Vector3 o = s < 2 ? V3(0, 0, w * 0.8f) : V3(w * 0.8f, 0, 0);
                    wire(V3(prev.x + o.x, topy, prev.z + o.z), V3(p.x + o.x, topy, p.z + o.z), 0.6f, M_BLACK);
                }
                prev = p; have = true;
            }
        }
    }
}

// ---------------------------------------------------------------- blocks

static void build_blocks(void)
{
    int cand[40][2], nc = 0;
    for (int j = 0; j < 9; j++)
        for (int i = 0; i < 9; i++)
            if (layout[j][i] == 'S' || layout[j][i] == 'B') { cand[nc][0] = i; cand[nc][1] = j; nc++; }
    for (int k = nc - 1; k > 0; k--) {
        int r = ri(k + 1);
        int ti = cand[k][0], tj = cand[k][1];
        cand[k][0] = cand[r][0]; cand[k][1] = cand[r][1]; cand[r][0] = ti; cand[r][1] = tj;
    }
    for (int k = 0; k < 11 && k < nc; k++) rob_block[cand[k][0]][cand[k][1]] = true;
    for (int j = 0; j < ngz - 1; j++)
        for (int i = 0; i < ngx - 1; i++) {
            Rect b = block_rect(i, j);
            char kind = layout[j][i];
            cur_i = i; cur_j = j;
            rob_in_block = 0;
            rob_face = ri(4);
            switch (kind) {
            case 'R': block_residential(b); break;
            case 'S': block_shops(b, false); break;
            case 'B': block_shops(b, true); break;
            case 'D': block_downtown(b); break;
            case 'I': block_industrial(b); break;
            case 'P': block_park(b); break;
            default: block_special(b, kind); break;
            }
            block_edge_props(i, j, b, kind);
        }
}

// ---------------------------------------------------------------- traffic lights

static float clock_s;
static float sig_off[256];
static uint8_t sig_state[256];

static int phase_state(float t)
{
    if (t < 12) return 0;
    if (t < 15) return 1;
    return 2;
}

static void sig_update(void)
{
    for (int g = 0; g < ngx * ngz * 2 && g < 256; g++) {
        float t = fmodf(clock_s + sig_off[g / 2], 33.0f);
        if (g & 1) t = fmodf(t + 33.0f - 16.5f, 33.0f);
        sig_state[g] = (uint8_t)phase_state(t);
    }
}

void world_tick(float dt)
{
    clock_s += dt;
    sig_update();
    day_hours += dt / 60.0f;
    if (day_hours >= 24) day_hours -= 24;
}

int light_state(int light) { return light < 0 || light >= 256 ? 0 : sig_state[light]; }

bool walk_light_ok(int light)
{
    if (light < 0 || light >= 256) return true;
    return sig_state[light] == 2 && sig_state[light ^ 1] == 0;
}

void world_frame(void)
{
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, sig_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RED, GL_UNSIGNED_BYTE, sig_state);
    glActiveTexture(GL_TEXTURE0);
}

// ---------------------------------------------------------------- lane graph

typedef struct { int first, n; } Seg;
static Seg seg[MAXL][MAXL][4];
static const int DX[4] = { 1, 0, -1, 0 }, DZ[4] = { 0, 1, 0, -1 };

static int new_lane(Vector3 a, Vector3 b, float speed, int light)
{
    Lane *l = &lanes[nlanes];
    *l = (Lane){ a, b, { -1, -1, -1 }, 0, light, Vector3Distance(a, b), speed };
    return nlanes++;
}

static bool in_grid(int i, int j) { return i >= 0 && j >= 0 && i < ngx && j < ngz; }
static float sq_at(int i, int j, int d) { return d % 2 == 0 ? rhx[i] : rhz[j]; }

static void link_lane(int from, int to)
{
    Lane *l = &lanes[from];
    if (l->nnext < 3) l->next[l->nnext++] = to;
}

static void connector(int from, int to, int turn, float speed)
{
    Vector3 b = lanes[from].b, a2 = lanes[to].a;
    if (turn == 0) {
        int c = new_lane(b, a2, speed, -1);
        link_lane(from, c);
        link_lane(c, to);
        return;
    }
    bool din_x = fabsf(lanes[from].b.x - lanes[from].a.x) > fabsf(lanes[from].b.z - lanes[from].a.z);
    Vector3 P = din_x ? V3(a2.x, 0, b.z) : V3(b.x, 0, a2.z);
    int pieces = turn > 0 ? 3 : 4;
    Vector3 prev = b;
    int prev_id = from;
    for (int k = 1; k <= pieces; k++) {
        float t = (float)k / pieces, u = 1 - t;
        Vector3 q = { u * u * b.x + 2 * u * t * P.x + t * t * a2.x, 0, u * u * b.z + 2 * u * t * P.z + t * t * a2.z };
        int c = new_lane(prev, q, speed, -1);
        link_lane(prev_id, c);
        prev = q; prev_id = c;
    }
    link_lane(prev_id, to);
}

static void build_lanes(void)
{
    lanes = calloc(30000, sizeof(Lane));
    nlanes = 0;
    for (int i = 0; i < ngx; i++)
        for (int j = 0; j < ngz; j++)
            for (int d = 0; d < 4; d++) {
                int ni = i + DX[d], nj = j + DZ[d];
                if (!in_grid(ni, nj)) continue;
                bool alongx = d % 2 == 0, av = alongx ? avz[j] : avx[i];
                int n = av ? 2 : 1;
                float s0 = sq_at(i, j, d), s1 = sq_at(ni, nj, d);
                Vector3 dv = V3(DX[d], 0, DZ[d]), r = V3(-DZ[d], 0, DX[d]);
                Vector3 c0 = V3(gx[i], 0, gz[j]), c1 = V3(gx[ni], 0, gz[nj]);
                float speed = av ? 17.0f : rf(11, 14);
                int light = (ni * ngz + nj) * 2 + (alongx ? 1 : 0);
                seg[i][j][d] = (Seg){ nlanes, n };
                for (int k = 0; k < n; k++) {
                    float off = 1.75f + 3.5f * k;
                    Vector3 a = Vector3Add(Vector3Add(c0, Vector3Scale(dv, s0)), Vector3Scale(r, off));
                    Vector3 b = Vector3Add(Vector3Subtract(c1, Vector3Scale(dv, s1 + 4.9f)), Vector3Scale(r, off));
                    new_lane(a, b, speed, light);
                }
            }
    for (int i = 0; i < ngx; i++)
        for (int j = 0; j < ngz; j++)
            for (int din = 0; din < 4; din++) {
                int pi = i - DX[din], pj = j - DZ[din];
                if (!in_grid(pi, pj)) continue;
                Seg in = seg[pi][pj][din];
                for (int k = 0; k < in.n; k++) {
                    int from = in.first + k;
                    for (int relax = 0; relax < 2 && lanes[from].nnext == 0; relax++)
                        for (int opt = 0; opt < 3; opt++) {
                            int dout = opt == 0 ? din : opt == 1 ? (din + 1) % 4 : (din + 3) % 4;
                            if (!in_grid(i + DX[dout], j + DZ[dout])) continue;
                            if (!relax && opt == 1 && k != in.n - 1) continue;
                            if (!relax && opt == 2 && k != 0) continue;
                            Seg out = seg[i][j][dout];
                            if (!out.n) continue;
                            int ko = opt == 0 ? (k < out.n ? k : out.n - 1) : opt == 1 ? out.n - 1 : 0;
                            connector(from, out.first + ko, opt == 0 ? 0 : opt == 1 ? 1 : -1, opt == 0 ? lanes[from].speed : opt == 1 ? 6.5f : 7.5f);
                        }
                }
            }
}

// ---------------------------------------------------------------- walk graph

static int nwalk_cap;
static int idxA[MAXL][MAXL][4], idxB[MAXL][MAXL][4];

static int walk_add(Vector3 p)
{
    if (nwalk == nwalk_cap) { nwalk_cap = nwalk_cap ? nwalk_cap * 2 : 2048; walk = realloc(walk, nwalk_cap * sizeof(WalkNode)); }
    walk[nwalk] = (WalkNode){ p, { -1, -1, -1, -1 }, 0, -1 };
    return nwalk++;
}

static void walk_link(int a, int b)
{
    if (a < 0 || b < 0) return;
    if (walk[a].n < 4) walk[a].nb[walk[a].n++] = b;
    if (walk[b].n < 4) walk[b].nb[walk[b].n++] = a;
}

static void walk_chain(int a, int b)
{
    if (a < 0 || b < 0) return;
    Vector3 pa = walk[a].p, pb = walk[b].p;
    float len = Vector3Distance(pa, pb);
    int n = (int)fmaxf(1, ceilf(len / 20.0f));
    int prev = a;
    for (int k = 1; k < n; k++) {
        int id = walk_add(Vector3Lerp(pa, pb, (float)k / n));
        walk_link(prev, id);
        prev = id;
    }
    walk_link(prev, b);
}

static void build_walk(void)
{
    memset(idxA, -1, sizeof idxA);
    memset(idxB, -1, sizeof idxB);
    for (int i = 0; i < ngx; i++)
        for (int j = 0; j < ngz; j++)
            for (int q = 0; q < 4; q++) {
                int sx = (q & 1) ? 1 : -1, sz = (q & 2) ? 1 : -1;
                int bi = i + (sx > 0 ? 0 : -1), bj = j + (sz > 0 ? 0 : -1);
                if (bi < 0 || bj < 0 || bi >= ngx - 1 || bj >= ngz - 1) continue;
                int a = walk_add(V3(gx[i] + sx * (rhx[i] + 1.5f), SWY, gz[j] + sz * (rhz[j] + 2.0f)));
                int b = walk_add(V3(gx[i] + sx * (rhx[i] + 2.0f), SWY, gz[j] + sz * (rhz[j] + 1.5f)));
                idxA[i][j][q] = a; idxB[i][j][q] = b;
                walk_link(a, b);
            }
    for (int i = 0; i < ngx; i++)
        for (int j = 0; j < ngz; j++)
            for (int q = 0; q < 4; q++) {
                int sx = (q & 1) ? 1 : -1, sz = (q & 2) ? 1 : -1;
                int id = i * ngz + j;
                if (sx > 0 && idxA[i][j][q] >= 0 && idxA[i][j][q ^ 1] >= 0 && in_grid(i, j + sz)) {
                    walk_link(idxA[i][j][q], idxA[i][j][q ^ 1]);
                    walk[idxA[i][j][q]].light = walk[idxA[i][j][q ^ 1]].light = id * 2;
                }
                if (sz > 0 && idxB[i][j][q] >= 0 && idxB[i][j][q ^ 2] >= 0 && in_grid(i + sx, j)) {
                    walk_link(idxB[i][j][q], idxB[i][j][q ^ 2]);
                    walk[idxB[i][j][q]].light = walk[idxB[i][j][q ^ 2]].light = id * 2 + 1;
                }
            }
    for (int i = 0; i < ngx - 1; i++)
        for (int j = 0; j < ngz - 1; j++) {
            walk_chain(idxA[i][j][3], idxA[i + 1][j][2]);
            walk_chain(idxA[i][j + 1][1], idxA[i + 1][j + 1][0]);
            walk_chain(idxB[i][j][3], idxB[i][j + 1][1]);
            walk_chain(idxB[i + 1][j][2], idxB[i + 1][j + 1][0]);
        }
}

// ---------------------------------------------------------------- spatial grids and queries

#define GN 64
#define GC 32.0f

static int *box_start, *box_items;
static int *lane_start, *lane_items;
static int *walk_start, *walk_items;
static int *mark, mark_stamp;

static int cell_of(float v)
{
    int c = (int)floorf((v - CH0) / GC);
    return c < 0 ? 0 : c >= GN ? GN - 1 : c;
}

typedef void (*CellVisit)(int item, int cell, void *ud);

// fill a compressed grid: visit(count pass), then (fill pass)
static void grid_fill(int **start, int **items, int n, void (*span)(int item, int *x0, int *z0, int *x1, int *z1))
{
    *start = calloc(GN * GN + 1, sizeof(int));
    for (int pass = 0; pass < 2; pass++) {
        int *cursor = pass ? calloc(GN * GN, sizeof(int)) : NULL;
        for (int it = 0; it < n; it++) {
            int x0, z0, x1, z1;
            span(it, &x0, &z0, &x1, &z1);
            for (int cz = z0; cz <= z1; cz++)
                for (int cx = x0; cx <= x1; cx++) {
                    int c = cz * GN + cx;
                    if (!pass) (*start)[c + 1]++;
                    else (*items)[(*start)[c] + cursor[c]++] = it;
                }
        }
        if (!pass) {
            for (int c = 0; c < GN * GN; c++) (*start)[c + 1] += (*start)[c];
            *items = malloc((size_t)((*start)[GN * GN] + 1) * sizeof(int));
        } else free(cursor);
    }
}

static void box_span(int i, int *x0, int *z0, int *x1, int *z1)
{
    *x0 = cell_of(boxes[i].x0); *x1 = cell_of(boxes[i].x1); *z0 = cell_of(boxes[i].z0); *z1 = cell_of(boxes[i].z1);
}
static void lane_span(int i, int *x0, int *z0, int *x1, int *z1)
{
    *x0 = cell_of(fminf(lanes[i].a.x, lanes[i].b.x) - 1); *x1 = cell_of(fmaxf(lanes[i].a.x, lanes[i].b.x) + 1);
    *z0 = cell_of(fminf(lanes[i].a.z, lanes[i].b.z) - 1); *z1 = cell_of(fmaxf(lanes[i].a.z, lanes[i].b.z) + 1);
}
static void walk_span(int i, int *x0, int *z0, int *x1, int *z1)
{
    *x0 = *x1 = cell_of(walk[i].p.x); *z0 = *z1 = cell_of(walk[i].p.z);
}

static void grids_build(void)
{
    grid_fill(&box_start, &box_items, nbox, box_span);
    grid_fill(&lane_start, &lane_items, nlanes, lane_span);
    grid_fill(&walk_start, &walk_items, nwalk, walk_span);
    mark = calloc((size_t)nbox + 1, sizeof(int));
}

static float seg_dist2(Vector3 p, Vector3 a, Vector3 b)
{
    float dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
    float t = l2 > 1e-6f ? ((p.x - a.x) * dx + (p.z - a.z) * dz) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float qx = a.x + dx * t - p.x, qz = a.z + dz * t - p.z;
    return qx * qx + qz * qz;
}

int nearest_lane(Vector3 p, float maxd)
{
    int best = -1;
    float bd = maxd * maxd;
    int x0 = cell_of(p.x - maxd), x1 = cell_of(p.x + maxd), z0 = cell_of(p.z - maxd), z1 = cell_of(p.z + maxd);
    for (int cz = z0; cz <= z1; cz++)
        for (int cx = x0; cx <= x1; cx++)
            for (int k = lane_start[cz * GN + cx]; k < lane_start[cz * GN + cx + 1]; k++) {
                int l = lane_items[k];
                float d = seg_dist2(p, lanes[l].a, lanes[l].b);
                if (d < bd) { bd = d; best = l; }
            }
    return best;
}

int nearest_walk(Vector3 p, float maxd)
{
    int best = -1;
    float bd = maxd * maxd;
    int x0 = cell_of(p.x - maxd), x1 = cell_of(p.x + maxd), z0 = cell_of(p.z - maxd), z1 = cell_of(p.z + maxd);
    for (int cz = z0; cz <= z1; cz++)
        for (int cx = x0; cx <= x1; cx++)
            for (int k = walk_start[cz * GN + cx]; k < walk_start[cz * GN + cx + 1]; k++) {
                int w = walk_items[k];
                float dx = walk[w].p.x - p.x, dz = walk[w].p.z - p.z, d = dx * dx + dz * dz;
                if (d < bd) { bd = d; best = w; }
            }
    return best;
}

static bool on_pier(float x, float z)
{
    for (int i = 0; i < 3; i++)
        if (x > piers[i].x0 && x < piers[i].x1 && z > piers[i].z0 && z < piers[i].z1) return true;
    return false;
}

bool is_water(float x, float z) { return world_coast(x, z) < -1.7f && !on_pier(x, z); }

float ground_y(float x, float z)
{
    if (on_pier(x, z)) return 0.05f;
    if (is_water(x, z)) return -2;
    if (x > xmin_o && x < xmax_o && z > zmin_o && z < zmax_o) {
        for (int i = 0; i < ngx; i++) if (fabsf(x - gx[i]) < rhx[i]) return 0;
        for (int j = 0; j < ngz; j++) if (fabsf(z - gz[j]) < rhz[j]) return 0;
        return SWY;
    }
    if (x >= xmax_o && x < xmax_o + 9 && z > zmin_o && z < zmax_o + 9) return SWY + 0.01f;
    if (z >= zmax_o && z < zmax_o + 9 && x > 152 && x < xmax_o) return SWY + 0.01f;
    return 0;
}

bool world_push(Vector3 *p, float radius, float height)
{
    bool hit = false;
    for (int it = 0; it < 3; it++) {
        bool again = false;
        int x0 = cell_of(p->x - radius), x1 = cell_of(p->x + radius), z0 = cell_of(p->z - radius), z1 = cell_of(p->z + radius);
        for (int cz = z0; cz <= z1; cz++)
            for (int cx = x0; cx <= x1; cx++)
                for (int k = box_start[cz * GN + cx]; k < box_start[cz * GN + cx + 1]; k++) {
                    const Box *b = &boxes[box_items[k]];
                    if (b->y1 < p->y + 0.3f || b->y0 > p->y + height) continue;
                    float qx = fminf(fmaxf(p->x, b->x0), b->x1), qz = fminf(fmaxf(p->z, b->z0), b->z1);
                    float dx = p->x - qx, dz = p->z - qz, d2 = dx * dx + dz * dz;
                    if (d2 >= radius * radius) continue;
                    if (d2 > 1e-8f) {
                        float d = sqrtf(d2), s = (radius - d) / d;
                        p->x += dx * s; p->z += dz * s;
                    } else {
                        float l = p->x - b->x0, r = b->x1 - p->x, t = p->z - b->z0, bt = b->z1 - p->z;
                        float m = fminf(fminf(l, r), fminf(t, bt));
                        if (m == l) p->x = b->x0 - radius; else if (m == r) p->x = b->x1 + radius;
                        else if (m == t) p->z = b->z0 - radius; else p->z = b->z1 + radius;
                    }
                    hit = again = true;
                }
        if (!again) break;
    }
    return hit;
}

int world_boxes(Vector3 c, float r, BoundingBox *out, int max)
{
    int n = 0;
    mark_stamp++;
    int x0 = cell_of(c.x - r), x1 = cell_of(c.x + r), z0 = cell_of(c.z - r), z1 = cell_of(c.z + r);
    for (int cz = z0; cz <= z1; cz++)
        for (int cx = x0; cx <= x1; cx++)
            for (int k = box_start[cz * GN + cx]; k < box_start[cz * GN + cx + 1] && n < max; k++) {
                int id = box_items[k];
                if (mark[id] == mark_stamp) continue;
                mark[id] = mark_stamp;
                const Box *b = &boxes[id];
                float qx = fminf(fmaxf(c.x, b->x0), b->x1), qz = fminf(fmaxf(c.z, b->z0), b->z1);
                if ((c.x - qx) * (c.x - qx) + (c.z - qz) * (c.z - qz) > r * r) continue;
                out[n++] = (BoundingBox){ { b->x0, b->y0, b->z0 }, { b->x1, b->y1, b->z1 } };
            }
    return n;
}

bool world_ray(Vector3 o, Vector3 dir, float maxd, Hit *h)
{
    float best = maxd;
    Vector3 bn = { 0, 1, 0 };
    bool found = false;
    if (dir.y < -1e-6f) {
        float t = -o.y / dir.y;
        if (t > 0 && t < best) {
            Vector3 p = Vector3Add(o, Vector3Scale(dir, t));
            if (is_water(p.x, p.z)) t = (WATER_Y - o.y) / dir.y;
            if (t > 0 && t < best) { best = t; bn = V3(0, 1, 0); found = true; }
        }
    }
    mark_stamp++;
    for (float s = 0; s <= best + 4; s += 8) {
        Vector3 q = Vector3Add(o, Vector3Scale(dir, fminf(s, best)));
        int cx = cell_of(q.x), cz = cell_of(q.z);
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                int ax = cx + dx, az = cz + dz;
                if (ax < 0 || az < 0 || ax >= GN || az >= GN) continue;
                for (int k = box_start[az * GN + ax]; k < box_start[az * GN + ax + 1]; k++) {
                    int id = box_items[k];
                    if (mark[id] == mark_stamp) continue;
                    mark[id] = mark_stamp;
                    const Box *b = &boxes[id];
                    float t0 = 0, t1 = best;
                    float lo[3] = { b->x0, b->y0, b->z0 }, hi[3] = { b->x1, b->y1, b->z1 };
                    float oo[3] = { o.x, o.y, o.z }, dd[3] = { dir.x, dir.y, dir.z };
                    int axis = -1, sgn = 0;
                    bool ok = true;
                    for (int a = 0; a < 3 && ok; a++) {
                        if (fabsf(dd[a]) < 1e-9f) { if (oo[a] < lo[a] || oo[a] > hi[a]) ok = false; continue; }
                        float ta = (lo[a] - oo[a]) / dd[a], tb = (hi[a] - oo[a]) / dd[a];
                        int sg = -1;
                        if (ta > tb) { float t = ta; ta = tb; tb = t; sg = 1; }
                        if (ta > t0) { t0 = ta; axis = a; sgn = sg; }
                        if (tb < t1) t1 = tb;
                        if (t0 > t1) ok = false;
                    }
                    if (ok && axis >= 0 && t0 < best) {
                        best = t0;
                        bn = V3(axis == 0 ? sgn : 0, axis == 1 ? sgn : 0, axis == 2 ? sgn : 0);
                        found = true;
                    }
                }
            }
    }
    if (found && h) {
        h->t = best; h->p = Vector3Add(o, Vector3Scale(dir, best)); h->n = bn; h->kind = HIT_WORLD; h->id = 0;
    }
    return found;
}

// ---------------------------------------------------------------- upload and draw

typedef struct { Vector3 mn, mx; int first[GEO_PARTS], count[GEO_PARTS]; } Chunk;
static Chunk chunks[CHN * CHN];
static GLuint vaos[GEO_PARTS], vbos[GEO_PARTS], ebos[GEO_PARTS];

static void upload(void)
{
    for (int c = 0; c < CHN * CHN; c++) chunks[c].mn = V3(1e9f, 1e9f, 1e9f), chunks[c].mx = V3(-1e9f, -1e9f, -1e9f);
    long total_v = 0, total_i = 0;
    for (int part = 0; part < GEO_PARTS; part++) {
        long nv = 0, ni = 0;
        for (int c = 0; c < CHN * CHN; c++) { nv += bufs[c * GEO_PARTS + part].nv; ni += bufs[c * GEO_PARTS + part].ni; }
        Vtx *V = malloc((size_t)(nv + 1) * sizeof(Vtx));
        uint32_t *I = malloc((size_t)(ni + 1) * sizeof(uint32_t));
        long vb = 0, ib = 0;
        for (int c = 0; c < CHN * CHN; c++) {
            Buf *b = &bufs[c * GEO_PARTS + part];
            chunks[c].first[part] = (int)ib;
            chunks[c].count[part] = b->ni;
            for (int k = 0; k < b->nv; k++) {
                const Vtx *v = &b->v[k];
                Chunk *ch = &chunks[c];
                ch->mn = V3(fminf(ch->mn.x, v->p[0]), fminf(ch->mn.y, v->p[1]), fminf(ch->mn.z, v->p[2]));
                ch->mx = V3(fmaxf(ch->mx.x, v->p[0]), fmaxf(ch->mx.y, v->p[1]), fmaxf(ch->mx.z, v->p[2]));
            }
            memcpy(V + vb, b->v, (size_t)b->nv * sizeof(Vtx));
            for (int k = 0; k < b->ni; k++) I[ib + k] = b->i[k] + (uint32_t)vb;
            vb += b->nv; ib += b->ni;
            free(b->v); free(b->i);
        }
        total_v += nv; total_i += ni;
        glGenVertexArrays(1, &vaos[part]);
        glGenBuffers(1, &vbos[part]);
        glGenBuffers(1, &ebos[part]);
        glBindVertexArray(vaos[part]);
        glBindBuffer(GL_ARRAY_BUFFER, vbos[part]);
        glBufferData(GL_ARRAY_BUFFER, nv * (long)sizeof(Vtx), V, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebos[part]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, ni * 4L, I, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)12);
        glVertexAttribPointer(2, 3, GL_BYTE, GL_TRUE, sizeof(Vtx), (void *)20);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void *)24);
        glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)28);
        for (int a = 0; a < 6; a++) if (a != 4) glEnableVertexAttribArray(a);
        glBindVertexArray(0);
        free(V); free(I);
    }
    fprintf(stderr, "city: %ld vertices, %ld triangles, %d boxes, %d lanes, %d walk nodes, %d shops\n",
            total_v, total_i / 3, nbox, nlanes, nwalk, nshops);
}

static float planes[6][4];
static Vector3 cull_c;
static float cull_r;

void world_cull(Matrix m, Vector3 center, float radius)
{
    float r0[4] = { m.m3 + m.m0, m.m7 + m.m4, m.m11 + m.m8, m.m15 + m.m12 };
    float r1[4] = { m.m3 - m.m0, m.m7 - m.m4, m.m11 - m.m8, m.m15 - m.m12 };
    float r2[4] = { m.m3 + m.m1, m.m7 + m.m5, m.m11 + m.m9, m.m15 + m.m13 };
    float r3[4] = { m.m3 - m.m1, m.m7 - m.m5, m.m11 - m.m9, m.m15 - m.m13 };
    float r4[4] = { m.m3 + m.m2, m.m7 + m.m6, m.m11 + m.m10, m.m15 + m.m14 };
    float r5[4] = { m.m3 - m.m2, m.m7 - m.m6, m.m11 - m.m10, m.m15 - m.m14 };
    float *src[6] = { r0, r1, r2, r3, r4, r5 };
    for (int i = 0; i < 6; i++) {
        float l = sqrtf(src[i][0] * src[i][0] + src[i][1] * src[i][1] + src[i][2] * src[i][2]);
        for (int k = 0; k < 4; k++) planes[i][k] = src[i][k] / l;
    }
    cull_c = center;
    cull_r = radius;
}

static bool chunk_visible(const Chunk *c)
{
    if (c->mn.x > c->mx.x) return false;
    for (int i = 0; i < 6; i++) {
        float px = planes[i][0] > 0 ? c->mx.x : c->mn.x, py = planes[i][1] > 0 ? c->mx.y : c->mn.y, pz = planes[i][2] > 0 ? c->mx.z : c->mn.z;
        if (planes[i][0] * px + planes[i][1] * py + planes[i][2] * pz + planes[i][3] < 0) return false;
    }
    if (cull_r < 1e8f) {
        float dx = fmaxf(fmaxf(c->mn.x - cull_c.x, cull_c.x - c->mx.x), 0), dz = fmaxf(fmaxf(c->mn.z - cull_c.z, cull_c.z - c->mx.z), 0);
        if (dx * dx + dz * dz > cull_r * cull_r) return false;
    }
    return true;
}

void world_draw_part(int part)
{
    static GLsizei counts[CHN * CHN];
    static const void *offs[CHN * CHN];
    int n = 0;
    for (int c = 0; c < CHN * CHN; c++) {
        if (!chunks[c].count[part] || !chunk_visible(&chunks[c])) continue;
        counts[n] = chunks[c].count[part];
        offs[n] = (const void *)((uintptr_t)chunks[c].first[part] * 4);
        n++;
    }
    if (!n) return;
    glBindVertexArray(vaos[part]);
    glMultiDrawElements(GL_TRIANGLES, counts, GL_UNSIGNED_INT, offs, n);
    glBindVertexArray(0);
}

void world_draw(int pass)
{
    world_draw_part(GEO_GROUND);
    world_draw_part(GEO_SOLID);
    if (pass == PASS_MAIN) { world_draw_part(GEO_WATER); world_draw_part(GEO_GLASS); }
}


// ---------------------------------------------------------------- build

void world_build(uint32_t seed)
{
    (void)seed;
    M_STEEL = M(L_METAL, 0x9aa0a6); M_BLACK = M(L_PLAIN, 0x202224); M_DARK = M(L_PLAIN, 0x3a3e42);
    texture_build();
    make_grid();
    build_terrain();
    build_water();
    build_roads();
    build_sidewalks();
    for (int i = 0; i < ngx; i++) for (int j = 0; j < ngz; j++) build_signals(i, j);
    build_blocks();
    build_outside();
    build_lanes();
    build_walk();
    grids_build();
    upload();
    for (int g = 0; g < 256; g++) sig_off[g] = rf(0, 33);
    sig_update();
}
