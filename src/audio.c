// software mixer: one-shot voices with 3d position, plus synthesized engines, sirens, tyres, horns, fire
// and a city bed. everything is rendered in 64 frame blocks at 44100 Hz stereo float.
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "cops.h"

extern const unsigned char sfx_blob[];
extern const int sfx_blob_len, sfx_rate, sfx_offsets[], sfx_clip_start[];
extern const unsigned char voice_blob[];
extern const int voice_blob_len, voice_rate, sfx_plain, voice_clip_start[];

#define RATE 44100
#define BLK 64
#define NVOICE 48
#define RING 8192
#define RING_TARGET 1600
#define NENG 6
#define TAU 6.2831853f

// per sound: level, reverb send, steal priority, full volume distance, silent distance
static const struct { float vol, send, prio, mind, maxd, cd; } defs[S_COUNT] = {
    [S_PISTOL]      = { 1.00f, 0.55f, 6, 2, 160 },
    [S_SMG]         = { 0.95f, 0.50f, 6, 2, 160 },
    [S_SHOTGUN]     = { 1.00f, 0.65f, 7, 2, 180 },
    [S_EMPTY]       = { 0.50f, 0.05f, 3, 1.5f, 40 },
    [S_RELOAD]      = { 0.55f, 0.10f, 3, 1.5f, 40 },
    [S_PUNCH]       = { 0.75f, 0.10f, 4, 2, 60 },
    [S_HIT]         = { 0.75f, 0.10f, 4, 2, 70 },
    [S_CRASH_SMALL] = { 0.85f, 0.25f, 7, 4, 140 },
    [S_CRASH_BIG]   = { 1.00f, 0.45f, 9, 5, 200 },
    [S_GLASS]       = { 0.70f, 0.25f, 5, 3, 100 },
    [S_EXPLOSION]   = { 1.00f, 1.00f, 10, 12, 320 },
    [S_RICOCHET]    = { 0.60f, 0.30f, 4, 2, 90 },
    [S_STEP]        = { 0.35f, 0.00f, 1, 1.5f, 30 },
    [S_DOOR]        = { 0.70f, 0.10f, 4, 2, 60 },
    [S_SCREAM]      = { 0.70f, 0.20f, 5, 3, 110 },
    [S_PICKUP]      = { 0.55f, 0.00f, 3, 2, 40 },
    [S_CASH]        = { 0.55f, 0.00f, 3, 2, 40 },
    [S_BODYFALL]    = { 0.65f, 0.05f, 3, 2, 50 },
    [S_UI]          = { 0.45f, 0.00f, 8, 2, 40 },
    [S_V_FREEZE]    = { 1.00f, 0.30f, 6, 4, 70, 7 },
    [S_V_GROUND]    = { 1.00f, 0.30f, 6, 4, 70, 7 },
    [S_V_DROP]      = { 1.00f, 0.30f, 6, 4, 70, 8 },
    [S_V_PULLOVER]  = { 1.00f, 0.30f, 6, 4, 80, 8 },
    [S_V_HANDS]     = { 1.00f, 0.30f, 6, 4, 70, 6 },
    [S_V_SHOTS]     = { 1.00f, 0.30f, 6, 4, 90, 8 },
    [S_V_ONFOOT]    = { 1.00f, 0.30f, 6, 4, 90, 10 },
    [S_V_ARREST]    = { 1.00f, 0.30f, 7, 4, 70, 6 },
    [S_V_NORTH]     = { 0.45f, 0.00f, 5, 50, 100, 20 },
    [S_V_PURSUIT]   = { 0.45f, 0.00f, 5, 50, 100, 10 },
    [S_V_LOST]      = { 0.45f, 0.00f, 5, 50, 100, 10 },
    [S_V_TAKEIT]    = { 0.95f, 0.20f, 6, 3, 50, 10 },
    [S_V_DONTSHOOT] = { 0.95f, 0.20f, 6, 3, 50, 10 },
    [S_V_OUTTA]     = { 1.00f, 0.20f, 6, 3, 50, 10 },
    [S_V_PHONE]     = { 0.90f, 0.15f, 5, 3, 45, 12 },
    [S_V_NOTODAY]   = { 0.90f, 0.10f, 7, 50, 100, 6 },
    [S_V_SCREAM]    = { 0.75f, 0.25f, 5, 3, 90, 3 },
    [S_V_GRUNT]     = { 0.70f, 0.15f, 5, 2, 50, 2 },
    [S_V_MYCAR]     = { 0.95f, 0.25f, 5, 3, 60, 8 },
    [S_V_HELP]      = { 0.90f, 0.25f, 5, 3, 70, 5 },
    [S_V_COPS]      = { 0.90f, 0.25f, 5, 3, 70, 8 },
    [S_V_WHAT]      = { 0.90f, 0.20f, 5, 3, 50, 6 },
    [S_V_WATCH]     = { 0.95f, 0.25f, 5, 3, 50, 5 },
    [S_V_GUN]       = { 0.95f, 0.25f, 5, 3, 70, 8 },
};

static const uint8_t cylinders[CAR_MODELS] = { 6, 4, 4, 6, 8, 8, 4, 8, 8, 4 };

// ---- private random, never touches the simulation rng

static uint32_t arng = 0x9e3779b9u;
static float arand(void) { arng ^= arng << 13; arng ^= arng >> 17; arng ^= arng << 5; return (arng >> 8) * (1.0f / 16777216); }
static float anoise(void) { return arand() * 2 - 1; }
static float lpcoef(float hz) { return 1 - expf(-TAU * hz / RATE); }

// ---- listener

static struct { Vector3 pos, right, vel; } L;

static void listener(float dt)
{
    Vector3 fwd = Vector3Subtract(cam.target, cam.position);
    if (Vector3Length(fwd) < 1e-4f) fwd = (Vector3){ 0, 0, 1 };
    L.right = Vector3Normalize(Vector3CrossProduct(Vector3Normalize(fwd), (Vector3){ 0, 1, 0 }));
    Vector3 v = Vector3Scale(Vector3Subtract(cam.position, L.pos), 1 / fmaxf(dt, 1e-3f));
    if (Vector3Length(v) > 80) v = Vector3Zero(); // teleport
    L.vel = Vector3Lerp(L.vel, v, 0.2f);
    L.pos = cam.position;
}

static void spatial(Vector3 p, float mind, float maxd, float *att, float *pan, float *lpa)
{
    Vector3 r = Vector3Subtract(p, L.pos);
    float d = Vector3Length(r);
    float g = powf(mind / fmaxf(d, mind), 1.25f);
    if (d > 0.7f * maxd) g *= fmaxf(0, 1 - (d - 0.7f * maxd) / (0.3f * maxd));
    *att = g;
    *pan = d > 0.05f ? Vector3DotProduct(r, L.right) / d * fminf(1, d) : 0;
    *lpa = lpcoef(fmaxf(900, 18000 / (1 + d / 12)));
}

static float doppler(Vector3 p, Vector3 v)
{
    Vector3 u = Vector3Subtract(p, L.pos);
    float d = Vector3Length(u);
    if (d < 1) return 1;
    u = Vector3Scale(u, 1 / d);
    float away = Vector3DotProduct(v, u) - Vector3DotProduct(L.vel, u);
    return Clamp(343 / (343 + away), 0.8f, 1.25f);
}

// ---- a mono source placed in the stereo field: gains ramp across the block, distance low-pass

typedef struct { float tl, tr, la; float gl, gr, lp; } Ch;

static void aim(Ch *c, Vector3 p, float mind, float maxd, float vol)
{
    float a, pan;
    spatial(p, mind, maxd, &a, &pan, &c->la);
    c->tl = vol * a * cosf((pan + 1) * TAU / 8);
    c->tr = vol * a * sinf((pan + 1) * TAU / 8);
}

static void aim_inside(Ch *c, float vol, float hz)
{
    c->tl = c->tr = vol * 0.7071f;
    c->la = lpcoef(hz);
}

static bool idle(const Ch *c) { return c->tl + c->tr + c->gl + c->gr < 2e-4f; }
static void mute(Ch *c) { c->tl = c->tr = 0; }

static void put(Ch *c, const float *m, float *dry)
{
    for (int i = 0; i < BLK; i++) {
        c->lp += c->la * (m[i] - c->lp);
        c->gl += (c->tl - c->gl) * 0.004f;
        c->gr += (c->tr - c->gr) * 0.004f;
        dry[i * 2] += c->lp * c->gl;
        dry[i * 2 + 1] += c->lp * c->gr;
    }
}

// ---- sample bank

static float *bank;
static int nclips, nplain_samples;

float *ogg_decode(const unsigned char *blob, int len, int *n);
int out_open(void (*fill)(float *out, unsigned frames));

static void load_bank(void)
{
    int n1, n2;
    float *a = ogg_decode(sfx_blob, sfx_blob_len, &n1), *b = ogg_decode(voice_blob, voice_blob_len, &n2);
    if (!a || !b) return;
    bank = malloc(sizeof(float) * (n1 + n2));
    memcpy(bank, a, sizeof(float) * n1);
    memcpy(bank + n1, b, sizeof(float) * n2);
    free(a);
    free(b);
    nplain_samples = n1;
    nclips = sfx_offsets[S_COUNT];
}

static int clip_start(int c) { return c < sfx_plain ? sfx_clip_start[c] : nplain_samples + voice_clip_start[c - sfx_plain]; }
static int clip_len(int c)
{
    return c < sfx_plain ? sfx_clip_start[c + 1] - sfx_clip_start[c] : voice_clip_start[c - sfx_plain + 1] - voice_clip_start[c - sfx_plain];
}

// ---- one-shot voices

typedef struct {
    int clip, id;
    double pos;
    float rate, vol;
    Vector3 p;
    Ch c;
    bool fresh;
} Voice;

static Voice voices[NVOICE];

static float hermite(const float *b, int n, double pos)
{
    int i = (int)pos;
    float f = (float)(pos - i);
    float y0 = i > 0 ? b[i - 1] : 0, y1 = b[i], y2 = i + 1 < n ? b[i + 1] : 0, y3 = i + 2 < n ? b[i + 2] : 0;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3;
    return ((c3 * f + c2) * f + 0.5f * (y2 - y0)) * f + y1;
}

static float voice_score(const Voice *v)
{
    return defs[v->id].prio * (0.2f + v->c.gl + v->c.gr);
}

static bool play(int id, int clip, Vector3 p, float vol, float pitch)
{
    Voice nv = { .clip = clip, .id = id, .p = p, .fresh = true };
    nv.rate = pitch * (1 + 0.05f * anoise()) * (clip < sfx_plain ? sfx_rate : voice_rate) / RATE;
    nv.vol = vol * defs[id].vol * (0.9f + 0.1f * arand());
    float a, pan, la;
    spatial(p, defs[id].mind, defs[id].maxd, &a, &pan, &la);
    if (a * nv.vol < 0.003f) return false;
    int slot = -1;
    float worst = 1e30f;
    for (int i = 0; i < NVOICE; i++) {
        if (voices[i].clip < 0) { slot = i; break; }
        float s = voice_score(&voices[i]);
        if (s < worst) { worst = s; slot = i; }
    }
    if (voices[slot].clip >= 0 && voice_score(&voices[slot]) > defs[id].prio * (0.2f + 2 * a * nv.vol)) return false;
    Voice *old = &voices[slot];
    nv.c = old->c;
    *old = nv;
    return true;
}

void sfx(int id, Vector3 p, float vol, float pitch)
{
    if (id < 0 || id >= S_COUNT || !bank) return;
    int first = sfx_offsets[id], n = sfx_offsets[id + 1] - first;
    if (n > 0) play(id, first + (int)(arand() * n), p, vol, pitch);
}

// spoken lines. one voice at a time near the listener, a gap after each, a cooldown per line.
// dispatch is a radio: no position, never over itself.
static float vclock, near_until, radio_until, line_cd[S_COUNT];

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

void voice(int id, Vector3 p, float vol, int ped)
{
    if (id < S_V_FREEZE || id >= S_COUNT || !bank || vclock < line_cd[id]) return;
    bool radio = id >= S_V_NORTH && id <= S_V_LOST;
    if (radio) p = L.pos;
    bool near = Vector3Distance(p, L.pos) < 40;
    bool urgent = id == S_V_ARREST || id == S_V_NOTODAY;
    if (radio ? vclock < radio_until : near && !urgent && vclock < near_until) return;
    int first = sfx_offsets[id], n = sfx_offsets[id + 1] - first, pick = 0;
    float pitch = 1;
    if (id >= S_V_SCREAM && ped >= 0) {
        uint32_t h = hash32((uint32_t)ped * 2654435761u + (uint32_t)id);
        int nrec = id == S_V_SCREAM || id == S_V_GRUNT ? 3 : 1;
        pick = (int)(h >> 8) % nrec;
        if (peds[ped].body == BODY_FEMALE) {
            if (n > nrec) pick += nrec; else pitch = 1.26f;
        } else if (h % 4) pitch = 0.89f;
    }
    if (n <= 0 || !play(id, first + pick, p, vol, pitch)) return;
    float len_s = clip_len(first + pick) / (float)(voice_rate * pitch);
    line_cd[id] = vclock + defs[id].cd;
    if (radio) radio_until = vclock + len_s + 3;
    else if (near) near_until = vclock + len_s + 1.5f;
}

// ---- shared reverb: short urban slapback plus a small tail

static float slap[2][8192], comb[2][4][2048], apd[2][1024];
static float slap_lp[2], comb_lp[2][4];
static int rv_i;
static const int comb_len[2][4] = { { 1557, 1617, 1491, 1422 }, { 1601, 1553, 1467, 1389 } };

static void reverb(const float *wet, float *dry)
{
    for (int i = 0; i < BLK; i++, rv_i++) {
        float in = wet[i];
        float l = slap[0][(rv_i - 3660) & 8191], r = slap[1][(rv_i - 5560) & 8191];
        slap_lp[0] += 0.45f * (r - slap_lp[0]);
        slap_lp[1] += 0.45f * (l - slap_lp[1]);
        slap[0][rv_i & 8191] = in + slap_lp[0] * 0.34f;
        slap[1][rv_i & 8191] = in * 0.8f + slap_lp[1] * 0.34f;
        float out[2];
        for (int ch = 0; ch < 2; ch++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) {
                float *b = comb[ch][k];
                float y = b[(rv_i - comb_len[ch][k]) & 2047];
                comb_lp[ch][k] += 0.35f * (y - comb_lp[ch][k]);
                b[rv_i & 2047] = in + comb_lp[ch][k] * 0.78f;
                sum += y;
            }
            float x = sum * 0.25f, y = apd[ch][(rv_i - 347 - ch * 41) & 1023] - 0.5f * x;
            apd[ch][rv_i & 1023] = x + 0.5f * y;
            out[ch] = y;
        }
        dry[i * 2] += l * 0.5f + out[0] * 0.45f;
        dry[i * 2 + 1] += r * 0.5f + out[1] * 0.45f;
    }
}

static void voices_block(float *dry, float *wet)
{
    for (int k = 0; k < NVOICE; k++) {
        Voice *v = &voices[k];
        if (v->clip < 0) continue;
        int start = clip_start(v->clip), n = clip_len(v->clip);
        const float *b = bank + start;
        float a, pan, la;
        spatial(v->p, defs[v->id].mind, defs[v->id].maxd, &a, &pan, &la);
        float g = v->vol * a;
        v->c.tl = g * cosf((pan + 1) * TAU / 8);
        v->c.tr = g * sinf((pan + 1) * TAU / 8);
        v->c.la = la;
        if (v->fresh) { v->c.gl = v->c.tl; v->c.gr = v->c.tr; v->c.lp = 0; v->fresh = false; }
        float m[BLK] = { 0 }, w = defs[v->id].send * v->vol * sqrtf(a);
        for (int i = 0; i < BLK; i++) {
            if (v->pos >= n - 1) { v->clip = -1; break; }
            m[i] = hermite(b, n, v->pos);
            v->pos += v->rate;
        }
        put(&v->c, m, dry);
        for (int i = 0; i < BLK; i++) wet[i] += m[i] * w;
    }
}

// ---- engines

typedef struct {
    Ch c;
    float rpm_t, rpm, load_t, load, dop_t, dop;
    float ph, crank, amp, exhaust, intake, dc;
    int cyl;
} Engine;

static Engine eng[MAX_CARS];

static void engine_block(Engine *e, float *m)
{
    e->rpm += (e->rpm_t - e->rpm) * 0.06f;
    e->load += (e->load_t - e->load) * 0.03f;
    e->dop += (e->dop_t - e->dop) * 0.1f;
    float crank_step = (800 + 5700 * e->rpm) / (60 * RATE) * e->dop;
    float fire_step = crank_step * e->cyl * 0.5f;
    float a_ex = lpcoef(520 + 1270 * e->rpm + 450 * e->load);
    float a_in = lpcoef(850 + 1050 * e->rpm);
    float level = 0.55f + 0.37f * (e->rpm + e->load);
    for (int i = 0; i < BLK; i++) {
        e->ph += fire_step;
        if (e->ph >= 1) { e->ph -= 1; e->amp = 1 + 0.12f * anoise(); }
        e->crank += crank_step;
        if (e->crank >= 1) e->crank -= 1;
        // Smooth exhaust pulses fade to zero at each firing boundary.
        float q = 4 * e->ph * (1 - e->ph);
        float pulse = q * q * q * (1.45f - e->ph);
        e->exhaust += a_ex * (pulse * e->amp - e->exhaust);
        e->intake += a_in * (anoise() - e->intake);
        float y = e->exhaust * 1.55f + sinf(e->crank * TAU) * (0.17f + 0.05f * e->load)
            + e->intake * (0.09f + 0.27f * e->load) * (0.28f + 0.72f * pulse);
        e->dc += 0.003f * (y - e->dc);
        m[i] = (y - e->dc) * level;
    }
}

// ---- sirens, tyres, horns, fire

typedef struct { Ch c; float t, f, ph, hp, lp, dop_t, dop; } Siren;
typedef struct { Ch c; float low, band, rubber, level, target; } Skid;
typedef struct { Ch c; float ph[2], lp; } Horn;
typedef struct { Ch c; float crack, lp, t; } Fire;

static Siren sir[MAX_CARS];
static Skid skid[MAX_CARS];
static Horn horn[MAX_CARS];
static Fire fire[MAX_CARS];

// 9 s of wail, then 6 s of yelp, repeating. returns sweep position 0..1
static float siren_sweep(float t)
{
    float c = fmodf(t, 15);
    float x = c < 9 ? c / 4.5f : (c - 9) / 0.28f;
    return 0.5f - 0.5f * cosf(TAU * x);
}

static void siren_block(Siren *s, float *m)
{
    s->dop += (s->dop_t - s->dop) * 0.1f;
    float ahp = lpcoef(500), alp = lpcoef(3200);
    for (int i = 0; i < BLK; i++) {
        s->t += 1.0f / RATE;
        float f = 650 * powf(1500.0f / 650, siren_sweep(s->t)) * s->dop;
        s->f += (f - s->f) * 0.002f;
        s->ph += s->f / RATE;
        if (s->ph >= 1) s->ph -= 1;
        float x = tanhf(3.5f * sinf(s->ph * TAU)) + 0.25f * sinf(s->ph * TAU * 3);
        s->hp += ahp * (x - s->hp);
        s->lp += alp * ((x - s->hp) - s->lp);
        m[i] = tanhf(s->lp * 1.8f) * 0.32f;
    }
}

typedef struct { Ch c; float ph, w, t, lp, dop_t, dop; } Rotor;
static Rotor rotor;

// blade slap at 11 Hz over a low noise body, plus a turbine whine
static void rotor_block(Rotor *r, float *m)
{
    r->dop += (r->dop_t - r->dop) * 0.05f;
    float alp = lpcoef(260);
    for (int i = 0; i < BLK; i++) {
        float n = anoise();
        r->t += 1.0f / RATE;
        r->ph += 11.0f * r->dop / RATE;
        if (r->ph >= 1) r->ph -= 1;
        r->w += 74.0f * r->dop / RATE;
        if (r->w >= 1) r->w -= 1;
        r->lp += alp * (n - r->lp);
        float env = expf(-r->ph * 15) * (0.8f + 0.2f * sinf(TAU * 0.37f * r->t));
        float thump = env * (r->lp * 3.5f + 0.5f * sinf(TAU * r->w)) + (n - r->lp) * 0.05f * env;
        float whine = sinf(TAU * fmodf(r->t * 2150 * r->dop, 1)) * 0.025f + sinf(TAU * fmodf(r->t * 3230 * r->dop, 1)) * 0.012f;
        m[i] = tanhf(thump) * 0.55f + whine;
    }
}

static void skid_block(Skid *k, float *m)
{
    k->level += (k->target - k->level) * 0.05f;
    float f = 2 * sinf(TAU * 0.5f * (670 + 700 * k->level) / RATE);
    float a = lpcoef(2250);
    for (int i = 0; i < BLK; i++) {
        k->low += f * k->band;
        k->band += f * (anoise() - k->low - 0.72f * k->band);
        k->rubber += a * (k->band * 0.72f + k->low * 0.12f - k->rubber);
        m[i] = k->rubber;
    }
}

static void horn_block(Horn *h, float *m, float base)
{
    float a = lpcoef(2800);
    for (int i = 0; i < BLK; i++) {
        for (int k = 0; k < 2; k++) {
            h->ph[k] += base * (k ? 1.26f : 1) / RATE;
            if (h->ph[k] >= 1) h->ph[k] -= 1;
        }
        float x = tanhf(3 * sinf(h->ph[0] * TAU)) + tanhf(3 * sinf(h->ph[1] * TAU));
        h->lp += a * (x - h->lp);
        m[i] = h->lp * 0.22f;
    }
}

static void fire_block(Fire *f, float *m)
{
    float a = lpcoef(260);
    for (int i = 0; i < BLK; i++) {
        f->t += 1.0f / RATE;
        if (arand() < 38.0f / RATE) f->crack += 0.3f + 0.7f * arand();
        f->crack *= 0.9945f;
        f->lp += a * (anoise() - f->lp);
        float gust = 0.55f + 0.45f * sinf(f->t * 2.3f) * sinf(f->t * 0.9f + 1);
        m[i] = (f->crack * anoise() * 0.35f + f->lp * gust * 1.4f + anoise() * 0.015f);
    }
}

// ---- city bed

typedef struct { float wait, t, dur, f0, f1, ph, pan, amp; int left; bool on; } Bird;

static struct {
    float day, day_t, night_t;
    float lp[4], lvl, t;
    Bird bird;
    float cricket_t[2];
} amb;

static void ambience_block(float *dry)
{
    amb.t += (float)BLK / RATE;
    amb.day += (amb.day_t - amb.day) * 0.003f;
    float night = 1 - amb.day;
    float swell = 0.7f + 0.3f * sinf(amb.t * 0.21f) * sinf(amb.t * 0.057f + 2);
    float a = lpcoef(140), b = lpcoef(900);
    for (int i = 0; i < BLK; i++) {
        for (int ch = 0; ch < 2; ch++) {
            float n = anoise();
            amb.lp[ch] += a * (n - amb.lp[ch]);
            amb.lp[2 + ch] += b * (n - amb.lp[2 + ch]);
            dry[i * 2 + ch] += (amb.lp[ch] * 1.2f + amb.lp[2 + ch] * 0.12f) * 0.05f * swell;
        }
    }
    // birds: short rising or falling whistles in phrases
    Bird *bd = &amb.bird;
    if (!bd->on && amb.day > 0.02f) {
        bd->wait -= (float)BLK / RATE;
        if (bd->wait <= 0) {
            bd->on = true; bd->t = 0; bd->dur = 0.05f + 0.07f * arand();
            bd->f0 = 2600 + 2400 * arand(); bd->f1 = bd->f0 * (0.7f + 0.7f * arand());
            if (bd->left <= 0) { bd->left = 1 + (int)(arand() * 4); bd->pan = anoise() * 0.9f; bd->amp = 0.012f + 0.012f * arand(); }
        }
    }
    if (bd->on) {
        float gl = cosf((bd->pan + 1) * TAU / 8) * bd->amp * amb.day, gr = sinf((bd->pan + 1) * TAU / 8) * bd->amp * amb.day;
        for (int i = 0; i < BLK; i++) {
            float x = bd->t / bd->dur;
            if (x >= 1) { bd->on = false; bd->left--; bd->wait = bd->left > 0 ? 0.05f + 0.08f * arand() : 2 + 7 * arand(); break; }
            bd->ph += (bd->f0 + (bd->f1 - bd->f0) * x) / RATE;
            if (bd->ph >= 1) bd->ph -= 1;
            float s = sinf(bd->ph * TAU) * sinf(3.14159f * x) * sinf(3.14159f * x);
            dry[i * 2] += s * gl;
            dry[i * 2 + 1] += s * gr;
            bd->t += 1.0f / RATE;
        }
    }
    // crickets: three short pulses, repeated
    if (night > 0.02f) {
        static const float freq[2] = { 4300, 5200 }, period[2] = { 0.52f, 0.67f }, pan[2] = { -0.6f, 0.5f };
        for (int k = 0; k < 2; k++) {
            float g = 0.007f * night;
            for (int i = 0; i < BLK; i++) {
                float t = amb.cricket_t[k] += 1.0f / RATE;
                float c = fmodf(t, period[k]), env = 0;
                for (int p = 0; p < 3; p++) {
                    float u = (c - p * 0.07f) / 0.045f;
                    if (u > 0 && u < 1) env = sinf(3.14159f * u);
                }
                float s = sinf(TAU * fmodf(t * freq[k], 1)) * env * (0.7f + 0.3f * sinf(TAU * 60 * t)) * g;
                dry[i * 2] += s * cosf((pan[k] + 1) * TAU / 8);
                dry[i * 2 + 1] += s * sinf((pan[k] + 1) * TAU / 8);
            }
        }
    }
}

// ---- block render, limiter and the pull interface

static float raw_prev[BLK * 2], raw_cur[BLK * 2];
static float lim_g = 1;
static bool primed;

static float dc[2];

static void make_block(float *dry)
{
    float wet[BLK] = { 0 }, m[BLK];
    memset(dry, 0, sizeof(float) * BLK * 2);
    ambience_block(dry);
    voices_block(dry, wet);
    for (int i = 0; i < MAX_CARS; i++) {
        if (!idle(&eng[i].c)) { engine_block(&eng[i], m); put(&eng[i].c, m, dry); }
        if (!idle(&sir[i].c)) { siren_block(&sir[i], m); put(&sir[i].c, m, dry); }
        if (!idle(&skid[i].c)) { skid_block(&skid[i], m); put(&skid[i].c, m, dry); }
        if (!idle(&horn[i].c)) { horn_block(&horn[i], m, 400 + 17 * (i % 5)); put(&horn[i].c, m, dry); }
        if (!idle(&fire[i].c)) { fire_block(&fire[i], m); put(&fire[i].c, m, dry); }
    }
    if (!idle(&rotor.c)) { rotor_block(&rotor, m); put(&rotor.c, m, dry); }
    reverb(wet, dry);
    for (int i = 0; i < BLK * 2; i++) {      // dc blocker, ~20 Hz
        dc[i & 1] += 0.0028f * (dry[i] - dc[i & 1]);
        dry[i] -= dc[i & 1];
    }
}

static float peak_of(const float *b)
{
    float p = 0;
    for (int i = 0; i < BLK * 2; i++) p = fmaxf(p, fabsf(b[i]));
    return p;
}

// one block behind so the limiter can see the next block's peak before it plays the current one
static void produce(float *out)
{
    if (!primed) { make_block(raw_cur); primed = true; }
    memcpy(raw_prev, raw_cur, sizeof raw_prev);
    make_block(raw_cur);
    const float master = 1.15f, ceil_ = 0.89f;
    float d_prev = fminf(1, ceil_ / (peak_of(raw_prev) * master + 1e-9f));
    float d_next = fminf(1, ceil_ / (peak_of(raw_cur) * master + 1e-9f));
    float g_end = fminf(fminf(d_prev, d_next), lim_g + 0.0025f);
    for (int i = 0; i < BLK; i++) {
        float g = fminf(lim_g + (g_end - lim_g) * (i + 1) * (1.0f / BLK), d_prev);
        out[i * 2] = Clamp(raw_prev[i * 2] * master * g, -1, 1);
        out[i * 2 + 1] = Clamp(raw_prev[i * 2 + 1] * master * g, -1, 1);
    }
    lim_g = g_end;
}

static float rem[BLK * 2];
static int rem_pos = BLK;

void audio_render(float *out, int frames)
{
    while (frames > 0) {
        if (rem_pos == BLK) { produce(rem); rem_pos = 0; }
        int n = frames < BLK - rem_pos ? frames : BLK - rem_pos;
        memcpy(out, rem + rem_pos * 2, sizeof(float) * n * 2);
        out += n * 2;
        rem_pos += n;
        frames -= n;
    }
}

// ---- live output: the stream callback drains a ring the main thread keeps topped up

static float ring[RING * 2];
static atomic_uint ring_r, ring_w;
static bool live;

static void pull(float *o, unsigned frames)
{
    unsigned r = atomic_load_explicit(&ring_r, memory_order_relaxed), w = atomic_load_explicit(&ring_w, memory_order_acquire);
    for (unsigned i = 0; i < frames; i++) {
        if (r != w) {
            o[i * 2] = ring[(r & (RING - 1)) * 2];
            o[i * 2 + 1] = ring[(r & (RING - 1)) * 2 + 1];
            r++;
        } else o[i * 2] = o[i * 2 + 1] = 0;
    }
    atomic_store_explicit(&ring_r, r, memory_order_release);
}

static void pump(void)
{
    unsigned w = atomic_load_explicit(&ring_w, memory_order_relaxed);
    unsigned fill = w - atomic_load_explicit(&ring_r, memory_order_acquire);
    while (fill < RING_TARGET) {
        float tmp[BLK * 2];
        audio_render(tmp, BLK);
        memcpy(&ring[(w & (RING - 1)) * 2], tmp, sizeof tmp);
        w += BLK;
        fill += BLK;
        atomic_store_explicit(&ring_w, w, memory_order_release);
    }
}

void audio_init(void)
{
    for (int i = 0; i < NVOICE; i++) voices[i].clip = -1;
    amb.bird.wait = 1;
    load_bank();
    if (audio_offline) return;
    arng ^= (uint32_t)(GetTime() * 1000) * 2654435761u | 1;
    live = out_open(pull);
}

// ---- per tick: aim every continuous source from game state

static int player_car(void)
{
    int c = peds[PLAYER].car;
    return c >= 0 && c < MAX_CARS && cars[c].used ? c : -1;
}

// fills out[] with the n nearest cars the test accepts; returns how many
static int nearest(bool (*ok)(const Car *), int n, int *out)
{
    float best[NENG];
    int cnt = 0;
    for (int i = 0; i < MAX_CARS; i++) {
        if (!cars[i].used || !ok(&cars[i])) continue;
        float d = Vector3DistanceSqr(cars[i].pos, L.pos);
        int j;
        if (cnt < n) j = cnt++;
        else if (d < best[n - 1]) j = n - 1;
        else continue;
        best[j] = d; out[j] = i;
        for (; j > 0 && best[j] < best[j - 1]; j--) {
            float tf = best[j]; best[j] = best[j - 1]; best[j - 1] = tf;
            int ti = out[j]; out[j] = out[j - 1]; out[j - 1] = ti;
        }
    }
    return cnt;
}

static bool has_driver(const Car *c) { return c->driver >= 0; }
static bool has_siren(const Car *c) { return c->siren; }
#define SKID_START 0.24f
static bool is_skidding(const Car *c) { return c->skid > SKID_START; }
static bool has_horn(const Car *c) { return c->horn; }
static bool is_burning(const Car *c) { return c->fire_s > 0; }

#define CLEAR(arr) for (int i = 0; i < MAX_CARS; i++) mute(&arr[i].c)

static void aim_cars(void)
{
    int sel[NENG], pc = player_car();
    CLEAR(eng); CLEAR(sir); CLEAR(horn); CLEAR(fire);
    for (int i = 0; i < MAX_CARS; i++) {
        mute(&skid[i].c);
        skid[i].target = 0;
    }

    int n = nearest(has_driver, NENG, sel);
    bool have = false;
    for (int k = 0; k < n; k++) have |= sel[k] == pc;
    if (pc >= 0 && !have) sel[n < NENG ? n++ : NENG - 1] = pc;
    for (int k = 0; k < n; k++) {
        Car *c = &cars[sel[k]];
        Engine *e = &eng[sel[k]];
        e->cyl = cylinders[(int)Clamp(c->model, 0, CAR_MODELS - 1)];
        e->rpm_t = Clamp(c->rpm, 0, 1);
        e->load_t = Clamp(fabsf(c->throttle), 0, 1);
        e->dop_t = doppler(c->pos, c->vel);
        if (e->dop == 0) e->dop = e->dop_t;
        if (sel[k] == pc) { aim_inside(&e->c, 0.6f, 5200); e->dop_t = 1; }
        else aim(&e->c, c->pos, 4, 90, 0.7f);
    }

    mute(&rotor.c);
    Vector3 hp, hv;
    if (cops_heli(&hp, &hv)) {
        rotor.dop_t = doppler(hp, hv);
        if (rotor.dop == 0) rotor.dop = rotor.dop_t;
        aim(&rotor.c, hp, 25, 420, 0.9f);
    }

    n = nearest(has_siren, 4, sel);
    for (int k = 0; k < n; k++) {
        Car *c = &cars[sel[k]];
        Siren *s = &sir[sel[k]];
        s->dop_t = doppler(c->pos, c->vel);
        if (s->dop == 0) s->dop = s->dop_t;
        if (!s->t) s->t = sel[k] * 3.7f;
        aim(&s->c, c->pos, 6, 260, 0.7f);
    }
    n = nearest(is_skidding, 4, sel);
    for (int k = 0; k < n; k++) {
        Car *c = &cars[sel[k]];
        Skid *s = &skid[sel[k]];
        s->target = Clamp((c->skid - SKID_START) / (1 - SKID_START), 0, 1);
        aim(&s->c, c->pos, 3, 80, 0.68f * s->target * s->target);
    }
    n = nearest(has_horn, 3, sel);
    for (int k = 0; k < n; k++) {
        aim(&horn[sel[k]].c, cars[sel[k]].pos, 4, 140, 0.9f);
    }
    n = nearest(is_burning, 3, sel);
    for (int k = 0; k < n; k++) {
        aim(&fire[sel[k]].c, cars[sel[k]].pos, 3, 60, 0.6f);
    }
}

void audio_tick(float dt)
{
    vclock += dt;
    listener(dt);
    aim_cars();
    float h = day_hours;
    amb.day_t = Clamp((h - 5) / 2, 0, 1) * Clamp((20 - h) / 2, 0, 1);
    if (!audio_offline && live) pump();
}
