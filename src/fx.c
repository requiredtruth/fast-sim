#include "city.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define GLSL(...) #__VA_ARGS__

#define MAX_PART 3500
#define MAX_DECAL 400
#define MAX_SKID 6000
#define MAX_TRACER 64
#define MAX_FXL 32
#define SKID_KEYS (MAX_CARS * 4)

enum { SP_PUFF, SP_FIRE, SP_SPARK, SP_STREAK, SP_DEBRIS, SP_SHARD, SP_MUZZLE, SP_DUST, SP_DROP, SP_CORE, SP_RING,
       SP_BULLET = 12, SP_BLOOD, SP_SCORCH, SP_OIL };

typedef struct {
    Vector3 p, v;
    float life, max, s0, s1, rot, spin, grav, drag;
    Vector4 c0, c1;
    int sprite, kind;
    bool add, streak, bounce;
} Part;

typedef struct { Vector3 p, n; float size, rot, age; int kind; } Decal;
typedef struct { Vector3 a, b; float w, age; } Skid;
typedef struct { Vector3 a, b; float age; } Tracer;
typedef struct { Vector3 p, c; float r, age, life; } FxLight;

static Part parts[MAX_PART];
static int npart, part_head;
static Decal decals[MAX_DECAL];
static int decal_head, ndecal;
static Skid skids[MAX_SKID];
static int skid_head, nskid;
static Vector3 skid_last[SKID_KEYS];
static bool skid_on[SKID_KEYS];
static Tracer tracers[MAX_TRACER];
static int ntracer;
static FxLight fxl[MAX_FXL];
static int nfxl;
static float shake;

static GLuint prog, atlas, vao, vbo;
static int u_vp, u_tint;
static bool ready;

static const char *VS = GLSL(
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec2 aUV;
    layout(location = 3) in vec4 aCol;
    uniform mat4 uVP;
    out vec2 vUV; out vec4 vCol;
    void main() { vUV = aUV; vCol = aCol; gl_Position = uVP * vec4(aPos, 1.0); });

static const char *FS = GLSL(
    in vec2 vUV; in vec4 vCol;
    out vec4 oCol;
    uniform sampler2D uAtlas;
    uniform vec3 uTint;
    void main() {
        vec4 t = texture(uAtlas, vUV);
        float a = t.a * vCol.a;
        if (a < 0.003) discard;
        oCol = vec4(t.rgb * vCol.rgb * uTint, a);
    });

// ---------------------------------------------------------------- sprite sheet

static float hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h ^ (h >> 16)) / 4294967296.0f;
}

static float vn(float x, float y)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    return Lerp(Lerp(hash2(xi, yi), hash2(xi + 1, yi), fx), Lerp(hash2(xi, yi + 1), hash2(xi + 1, yi + 1), fx), fy);
}

static float fb(float x, float y)
{
    float a = 0.5f, s = 0;
    for (int i = 0; i < 4; i++) { s += a * vn(x, y); x = x * 2.1f + 7; y = y * 2.1f + 3; a *= 0.5f; }
    return s;
}

static float sstep(float a, float b, float x)
{
    float t = Clamp((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}

static void paint_cell(uint8_t *px, int cell)
{
    int cx = cell % 4, cy = cell / 4;
    for (int j = 0; j < 128; j++)
        for (int i = 0; i < 128; i++) {
            float u = (i + 0.5f) / 64 - 1, v = (j + 0.5f) / 64 - 1, r = sqrtf(u * u + v * v);
            float n = fb(u * 3 + cell * 5, v * 3 + cell * 3), a = 0, c = 1;
            switch (cell) {
            case SP_PUFF: a = sstep(1.0f, 0.15f, r + (n - 0.5f) * 0.9f) * 0.9f; c = 0.75f + 0.25f * n; break;
            case SP_FIRE: a = sstep(1.0f, 0.05f, r + (n - 0.5f) * 0.7f); c = 1.0f - 0.3f * r; break;
            case SP_SPARK: a = expf(-r * r * 7); break;
            case SP_STREAK: a = sstep(1.0f, 0.1f, fabsf(u)) * sstep(0.55f, 0.0f, fabsf(v)); break;
            case SP_DEBRIS: a = sstep(0.8f, 0.7f, fmaxf(fabsf(u), fabsf(v) * 1.2f)); c = 0.6f + 0.4f * n; break;
            case SP_SHARD: a = (v > -0.7f && fabsf(u) < (0.8f - v) * 0.5f) ? 1 : 0; c = 0.8f + 0.2f * n; break;
            case SP_MUZZLE: {
                float ang = atan2f(v, u);
                a = fmaxf(powf(fabsf(cosf(ang * 2.5f)), 10) * sstep(1.0f, 0.0f, r), expf(-r * r * 9));
                break; }
            case SP_DUST: a = sstep(1.0f, 0.0f, r + (n - 0.5f) * 0.6f) * 0.55f; c = 0.85f + 0.15f * n; break;
            case SP_DROP: a = sstep(1.0f, 0.6f, r); break;
            case SP_CORE: a = powf(sstep(1.0f, 0.0f, r), 1.5f); break;
            case SP_RING: a = sstep(0.18f, 0.0f, fabsf(r - 0.8f)); break;
            case SP_BULLET: {
                float halo = sstep(0.55f, 0.15f, r + (n - 0.5f) * 0.5f) * 0.35f;
                a = fmaxf(halo, sstep(0.2f, 0.12f, r) * 0.95f);
                c = r < 0.16f ? 0.02f : 0.25f;
                break; }
            case SP_BLOOD: {
                a = sstep(0.55f, 0.35f, r + (n - 0.5f) * 0.8f) * 0.9f;
                if (hash2(i / 9, j / 9) > 0.93f && r > 0.5f && r < 0.9f) a = 0.8f;
                c = 0.3f * (0.7f + 0.5f * n);
                break; }
            case SP_SCORCH: a = sstep(1.0f, 0.1f, r + (n - 0.5f) * 0.8f) * 0.85f; c = 0.05f; break;
            case SP_OIL: a = sstep(0.85f, 0.55f, r + (n - 0.5f) * 0.9f) * 0.75f; c = 0.04f; break;
            }
            uint8_t *q = px + ((cy * 128 + j) * 512 + cx * 128 + i) * 4;
            float rr = c, gg = c, bb = c;
            if (cell == SP_BLOOD) { rr = c * 1.6f; gg = c * 0.08f; bb = c * 0.08f; }
            q[0] = (uint8_t)(Clamp(rr, 0, 1) * 255);
            q[1] = (uint8_t)(Clamp(gg, 0, 1) * 255);
            q[2] = (uint8_t)(Clamp(bb, 0, 1) * 255);
            q[3] = (uint8_t)(Clamp(a, 0, 1) * 255);
        }
}

static void fx_init(void)
{
    ready = true;
    prog = r_program(VS, FS);
    u_vp = glGetUniformLocation(prog, "uVP");
    u_tint = glGetUniformLocation(prog, "uTint");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uAtlas"), 0);
    static uint8_t px[512 * 512 * 4];
    for (int c = 0; c < 16; c++) paint_cell(px, c);
    glGenTextures(1, &atlas);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 36, (void *)12);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 36, (void *)20);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(3);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------- spawning

static Part *spawn(int sprite, int kind)
{
    Part *p = &parts[part_head];
    part_head = (part_head + 1) % MAX_PART;
    if (npart < MAX_PART) npart++;
    *p = (Part){ .sprite = sprite, .kind = kind };
    return p;
}

static Vector3 rv(float s) { return (Vector3){ frand(-s, s), frand(-s, s), frand(-s, s) }; }

static Vector3 spread(Vector3 dir, float amt)
{
    Vector3 d = Vector3Add(Vector3Normalize(dir), rv(amt));
    return Vector3Normalize(d);
}

void fx_light(Vector3 p, Vector3 rgb, float radius, float life)
{
    if (!ready) fx_init();
    FxLight *l = nfxl < MAX_FXL ? &fxl[nfxl++] : &fxl[rnd() % MAX_FXL];
    *l = (FxLight){ p, rgb, radius, 0, life };
}

void fx_shake(float amount) { shake = fminf(shake + amount, 2.0f); }
float fx_shake_amount(void) { return shake; }

static void sparks(Vector3 p, Vector3 dir, int n)
{
    for (int i = 0; i < n; i++) {
        Part *q = spawn(SP_SPARK, FX_SPARK);
        q->p = p;
        q->v = Vector3Scale(spread(dir, 0.7f), frand(3, 11));
        q->life = q->max = frand(0.25f, 0.7f);
        q->s0 = frand(0.05f, 0.1f); q->s1 = 0.02f;
        q->grav = 9; q->drag = 0.6f;
        q->c0 = (Vector4){ 5, 2.6f, 0.9f, 1 }; q->c1 = (Vector4){ 2, 0.4f, 0.05f, 0 };
        q->add = q->streak = q->bounce = true;
    }
}

static void smoke(Vector3 p, Vector3 dir, int n, float size, float life, float shade, float up)
{
    for (int i = 0; i < n; i++) {
        Part *q = spawn(SP_PUFF, FX_SMOKE);
        q->p = Vector3Add(p, rv(size * 0.3f));
        q->v = Vector3Add(Vector3Scale(dir, frand(0.2f, 1.2f)), (Vector3){ frand(-0.4f, 0.4f), up * frand(0.6f, 1.4f), frand(-0.4f, 0.4f) });
        q->life = q->max = life * frand(0.7f, 1.2f);
        q->s0 = size * 0.4f; q->s1 = size * frand(1.5f, 2.4f);
        q->rot = frand(0, 6.28f); q->spin = frand(-0.6f, 0.6f);
        q->drag = 0.6f;
        float s = shade * frand(0.8f, 1.1f);
        q->c0 = (Vector4){ s, s, s, 0.55f }; q->c1 = (Vector4){ s * 0.9f, s * 0.9f, s * 0.9f, 0 };
    }
}

static void fire(Vector3 p, int n, float size, float up)
{
    for (int i = 0; i < n; i++) {
        Part *q = spawn(SP_FIRE, FX_FIRE);
        q->p = Vector3Add(p, rv(size * 0.4f));
        q->v = (Vector3){ frand(-0.5f, 0.5f), up * frand(0.6f, 1.5f), frand(-0.5f, 0.5f) };
        q->life = q->max = frand(0.4f, 0.9f);
        q->s0 = size; q->s1 = size * 0.15f;
        q->rot = frand(0, 6.28f); q->spin = frand(-2, 2);
        q->drag = 0.8f;
        q->c0 = (Vector4){ 4.5f, 2.6f, 0.7f, 0.9f }; q->c1 = (Vector4){ 1.6f, 0.2f, 0.03f, 0 };
        q->add = true;
    }
}

static void chunks(Vector3 p, Vector3 dir, int n, int sprite, int kind, float spd, Vector4 col)
{
    for (int i = 0; i < n; i++) {
        Part *q = spawn(sprite, kind);
        q->p = p;
        q->v = Vector3Scale(spread(dir, 0.9f), frand(0.4f, 1.0f) * spd);
        q->life = q->max = frand(0.8f, 1.8f);
        q->s0 = q->s1 = frand(0.04f, 0.12f);
        q->rot = frand(0, 6.28f); q->spin = frand(-12, 12);
        q->grav = 12; q->drag = 0.2f; q->bounce = true;
        float k = frand(0.7f, 1.1f);
        q->c0 = (Vector4){ col.x * k, col.y * k, col.z * k, col.w };
        q->c1 = q->c0;
        q->c1.w = 0;
    }
}

static void explosion(Vector3 p)
{
    fx_light(p, (Vector3){ 9, 4.5f, 1.4f }, 55, 0.7f);
    for (int i = 0; i < 26; i++) {
        Part *q = spawn(SP_FIRE, FX_EXPLOSION);
        q->p = Vector3Add(p, rv(1.5f));
        q->v = Vector3Add(rv(5.0f), (Vector3){ 0, frand(1, 5), 0 });
        q->life = q->max = frand(0.5f, 1.1f);
        q->s0 = frand(1.2f, 2.4f); q->s1 = frand(3.0f, 5.0f);
        q->rot = frand(0, 6.28f); q->spin = frand(-2, 2);
        q->drag = 2.0f;
        q->c0 = (Vector4){ 6, 3.4f, 1.0f, 0.95f }; q->c1 = (Vector4){ 1.6f, 0.25f, 0.03f, 0 };
        q->add = true;
    }
    Part *c = spawn(SP_CORE, FX_EXPLOSION);
    c->p = Vector3Add(p, (Vector3){ 0, 1, 0 });
    c->life = c->max = 0.25f; c->s0 = 4; c->s1 = 9;
    c->c0 = (Vector4){ 10, 8, 5, 1 }; c->c1 = (Vector4){ 4, 2, 0.5f, 0 };
    c->add = true;
    Part *rg = spawn(SP_RING, FX_EXPLOSION);
    rg->p = Vector3Add(p, (Vector3){ 0, 0.3f, 0 });
    rg->life = rg->max = 0.5f; rg->s0 = 2; rg->s1 = 16;
    rg->c0 = (Vector4){ 2, 1.4f, 0.8f, 0.7f }; rg->c1 = (Vector4){ 1, 0.7f, 0.4f, 0 };
    rg->add = true;
    for (int i = 0; i < 18; i++) {
        Part *q = spawn(SP_PUFF, FX_SMOKE);
        q->p = Vector3Add(p, rv(2.0f));
        q->v = Vector3Add(rv(3.0f), (Vector3){ 0, frand(2, 7), 0 });
        q->life = q->max = frand(2.5f, 5.0f);
        q->s0 = frand(1.5f, 2.5f); q->s1 = frand(5, 8);
        q->rot = frand(0, 6.28f); q->spin = frand(-0.5f, 0.5f);
        q->drag = 0.9f;
        float s = frand(0.04f, 0.14f);
        q->c0 = (Vector4){ s, s, s, 0.8f }; q->c1 = (Vector4){ s * 1.4f, s * 1.4f, s * 1.4f, 0 };
    }
    sparks(p, (Vector3){ 0, 1, 0 }, 40);
    chunks(p, (Vector3){ 0, 1, 0 }, 24, SP_DEBRIS, FX_DEBRIS, 14, (Vector4){ 0.15f, 0.13f, 0.12f, 1 });
    float d = Vector3Distance(p, cam.position);
    fx_shake(Clamp(1.6f - d / 50.0f, 0, 1.4f));
}

void fx_burst(int kind, Vector3 p, Vector3 dir, int n)
{
    if (!ready) fx_init();
    if (Vector3LengthSqr(dir) < 1e-6f) dir = (Vector3){ 0, 1, 0 };
    switch (kind) {
    case FX_SPARK: sparks(p, dir, n); break;
    case FX_BLOOD:
        for (int i = 0; i < n; i++) {
            Part *q = spawn(SP_DROP, FX_BLOOD);
            q->p = p;
            q->v = Vector3Scale(spread(dir, 0.6f), frand(1.5f, 5));
            q->life = q->max = frand(0.5f, 1.0f);
            q->s0 = frand(0.04f, 0.1f); q->s1 = q->s0 * 0.6f;
            q->grav = 10; q->drag = 0.3f;
            q->c0 = (Vector4){ 0.45f, 0.01f, 0.01f, 0.95f }; q->c1 = (Vector4){ 0.25f, 0, 0, 0.3f };
        }
        break;
    case FX_SMOKE: smoke(p, dir, n, 0.6f, 2.6f, 0.4f, 1.2f); break;
    case FX_FIRE: fire(p, n, 0.7f, 2.0f); break;
    case FX_DEBRIS: chunks(p, dir, n, SP_DEBRIS, FX_DEBRIS, 6, (Vector4){ 0.3f, 0.28f, 0.25f, 1 }); break;
    case FX_GLASS: chunks(p, dir, n, SP_SHARD, FX_GLASS, 4, (Vector4){ 0.75f, 0.9f, 1.0f, 0.85f }); break;
    case FX_MUZZLE: {
        Part *q = spawn(SP_MUZZLE, FX_MUZZLE);
        q->p = p; q->life = q->max = 0.055f;
        q->s0 = frand(0.4f, 0.65f); q->s1 = q->s0 * 0.6f;
        q->rot = frand(0, 6.28f);
        q->c0 = (Vector4){ 7, 4.5f, 1.8f, 1 }; q->c1 = (Vector4){ 3, 1.2f, 0.3f, 0 };
        q->add = true;
        fx_light(p, (Vector3){ 4, 2.2f, 0.8f }, 12, 0.07f);
        break; }
    case FX_DUST: smoke(p, dir, n, 0.9f, 1.4f, 0.55f, 0.5f); break;
    case FX_EXPLOSION: explosion(p); break;
    case FX_WATER:
        for (int i = 0; i < n; i++) {
            Part *q = spawn(SP_DROP, FX_WATER);
            q->p = p;
            q->v = Vector3Add(Vector3Scale(spread(dir, 0.5f), frand(2, 6)), (Vector3){ 0, 2, 0 });
            q->life = q->max = frand(0.5f, 1.0f);
            q->s0 = frand(0.08f, 0.2f); q->s1 = q->s0 * 0.5f;
            q->grav = 9; q->drag = 0.2f;
            q->c0 = (Vector4){ 0.9f, 0.95f, 1.0f, 0.7f }; q->c1 = (Vector4){ 0.8f, 0.9f, 1, 0 };
        }
        break;
    }
}

void fx_decal(int kind, Vector3 p, Vector3 n, float size)
{
    if (!ready) fx_init();
    Decal *d = &decals[decal_head];
    decal_head = (decal_head + 1) % MAX_DECAL;
    if (ndecal < MAX_DECAL) ndecal++;
    *d = (Decal){ Vector3Add(p, Vector3Scale(n, 0.012f)), Vector3Normalize(n), size, frand(0, 6.28f), 0, kind };
}

void fx_skid(int key, Vector3 p, float width, bool on)
{
    if (key < 0 || key >= SKID_KEYS) return;
    if (!on) { skid_on[key] = false; return; }
    p.y = 0.03f;
    if (skid_on[key]) {
        if (Vector3DistanceSqr(skid_last[key], p) < 0.04f) return;
        skids[skid_head] = (Skid){ skid_last[key], p, width, 0 };
        skid_head = (skid_head + 1) % MAX_SKID;
        if (nskid < MAX_SKID) nskid++;
    }
    skid_last[key] = p;
    skid_on[key] = true;
}

void fx_tracer(Vector3 a, Vector3 b)
{
    if (ntracer < MAX_TRACER) tracers[ntracer++] = (Tracer){ a, b, 0 };
}

int fx_lights(Vector3 *p, Vector3 *rgb, float *radius, int max)
{
    int n = 0;
    for (int i = 0; i < nfxl && n < max; i++) {
        FxLight *l = &fxl[i];
        float t = l->age / l->life;
        float k = t < 0.15f ? 1.0f : powf(1.0f - (t - 0.15f) / 0.85f, 2.0f);
        p[n] = l->p;
        rgb[n] = Vector3Scale(l->c, k);
        radius[n] = l->r;
        n++;
    }
    return n;
}

// ---------------------------------------------------------------- update

void fx_tick(float dt)
{
    if (!ready) fx_init();
    shake *= expf(-dt * 4.0f);
    if (shake < 0.002f) shake = 0;

    int live = 0;
    for (int i = 0; i < MAX_PART; i++) {
        Part *q = &parts[i];
        if (q->life <= 0) continue;
        q->life -= dt;
        if (q->life <= 0) continue;
        live++;
        q->v.y -= q->grav * dt;
        q->v = Vector3Scale(q->v, expf(-q->drag * dt));
        q->p = Vector3Add(q->p, Vector3Scale(q->v, dt));
        q->rot += q->spin * dt;
        if (q->bounce && q->p.y < 0.02f && q->v.y < 0) {
            q->p.y = 0.02f;
            q->v.y *= -0.35f;
            q->v.x *= 0.6f; q->v.z *= 0.6f;
            q->spin *= 0.5f;
            if (q->kind == FX_SPARK) q->life = 0;
        }
        if ((q->kind == FX_BLOOD || q->kind == FX_WATER) && q->p.y < 0.01f) q->life = 0;
    }
    npart = live;

    for (int i = 0; i < nfxl; i++) fxl[i].age += dt;
    for (int i = nfxl - 1; i >= 0; i--)
        if (fxl[i].age >= fxl[i].life) fxl[i] = fxl[--nfxl];
    for (int i = 0; i < ndecal; i++) decals[i].age += dt;
    for (int i = 0; i < nskid; i++) skids[i].age += dt;
    for (int i = ntracer - 1; i >= 0; i--) {
        tracers[i].age += dt;
        if (tracers[i].age > 0.09f) tracers[i] = tracers[--ntracer];
    }
}

// ---------------------------------------------------------------- drawing

static float *vb;
static int nvb;
static float vbbuf[(MAX_PART * 6 + MAX_DECAL * 6 + MAX_SKID * 6 + MAX_TRACER * 6) * 9];

static void vtx(Vector3 p, float u, float v, Vector4 c)
{
    float *f = vb + nvb * 9;
    f[0] = p.x; f[1] = p.y; f[2] = p.z; f[3] = u; f[4] = v;
    f[5] = c.x; f[6] = c.y; f[7] = c.z; f[8] = c.w;
    nvb++;
}

static void quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, int sprite, Vector4 col)
{
    float u0 = (sprite % 4) * 0.25f + 0.002f, v0 = (sprite / 4) * 0.25f + 0.002f, w = 0.246f;
    vtx(a, u0, v0 + w, col); vtx(b, u0 + w, v0 + w, col); vtx(c, u0 + w, v0, col);
    vtx(a, u0, v0 + w, col); vtx(c, u0 + w, v0, col); vtx(d, u0, v0, col);
}

static void flush(bool additive, float *tint)
{
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (size_t)nvb * 36, vb, GL_STREAM_DRAW);
    glBindVertexArray(vao);
    glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    glUniform3fv(u_tint, 1, additive ? (float[]){ 1, 1, 1 } : tint);
    glDrawArrays(GL_TRIANGLES, 0, nvb);
    nvb = 0;
}

static Vector4 col_at(const Part *q, float t)
{
    return (Vector4){ Lerp(q->c0.x, q->c1.x, t), Lerp(q->c0.y, q->c1.y, t), Lerp(q->c0.z, q->c1.z, t), Lerp(q->c0.w, q->c1.w, t) };
}

static void draw_parts(bool additive)
{
    for (int i = 0; i < MAX_PART; i++) {
        Part *q = &parts[i];
        if (q->life <= 0 || q->add != additive) continue;
        float t = 1 - q->life / q->max;
        float s = Lerp(q->s0, q->s1, t);
        Vector4 c = col_at(q, t);
        if (q->kind == FX_SMOKE || q->kind == FX_DUST) c.w *= sstep(0.0f, 0.12f, t);
        Vector3 r, u;
        if (q->streak) {
            Vector3 ax = Vector3Normalize(q->v);
            Vector3 vw = Vector3Normalize(Vector3CrossProduct(ax, Vector3Subtract(q->p, cam.position)));
            float len = fmaxf(Vector3Length(q->v) * 0.04f, s * 2);
            r = Vector3Scale(ax, len);
            u = Vector3Scale(vw, s * 0.5f);
            quad(Vector3Subtract(Vector3Subtract(q->p, r), u), Vector3Subtract(Vector3Add(q->p, r), u),
                 Vector3Add(Vector3Add(q->p, r), u), Vector3Add(Vector3Subtract(q->p, r), u), SP_STREAK, c);
            continue;
        }
        float cr = cosf(q->rot) * s, sr = sinf(q->rot) * s;
        r = Vector3Add(Vector3Scale(r_cam_right, cr), Vector3Scale(r_cam_up, sr));
        u = Vector3Add(Vector3Scale(r_cam_right, -sr), Vector3Scale(r_cam_up, cr));
        quad(Vector3Subtract(Vector3Subtract(q->p, r), u), Vector3Subtract(Vector3Add(q->p, r), u),
             Vector3Add(Vector3Add(q->p, r), u), Vector3Add(Vector3Subtract(q->p, r), u), q->sprite, c);
    }
}

static void draw_decals(void)
{
    for (int i = 0; i < ndecal; i++) {
        Decal *d = &decals[i];
        Vector3 up = fabsf(d->n.y) > 0.9f ? (Vector3){ 1, 0, 0 } : (Vector3){ 0, 1, 0 };
        Vector3 a = Vector3Normalize(Vector3CrossProduct(d->n, up)), b = Vector3CrossProduct(d->n, a);
        float c = cosf(d->rot) * d->size * 0.5f, s = sinf(d->rot) * d->size * 0.5f;
        Vector3 r = Vector3Add(Vector3Scale(a, c), Vector3Scale(b, s)), u = Vector3Add(Vector3Scale(a, -s), Vector3Scale(b, c));
        float fade = d->kind == DECAL_BLOOD ? 1 - sstep(90, 240, d->age) * 0.7f : 1 - sstep(200, 400, d->age) * 0.8f;
        Vector4 col = { 1, 1, 1, fade };
        quad(Vector3Subtract(Vector3Subtract(d->p, r), u), Vector3Subtract(Vector3Add(d->p, r), u),
             Vector3Add(Vector3Add(d->p, r), u), Vector3Add(Vector3Subtract(d->p, r), u), SP_BULLET + d->kind, col);
    }
}

static void draw_skids(void)
{
    for (int i = 0; i < nskid; i++) {
        Skid *s = &skids[i];
        Vector3 d = Vector3Subtract(s->b, s->a);
        float len = Vector3Length(d);
        if (len < 1e-3f) continue;
        Vector3 side = Vector3Scale((Vector3){ -d.z, 0, d.x }, s->w * 0.5f / len);
        Vector4 c = { 0.02f, 0.02f, 0.02f, 0.5f * (1 - sstep(60, 180, s->age)) };
        // SP_SCORCH centre is solid enough to read as rubber, uv squeezed to its middle
        float cu = 2 * 0.25f + 0.125f, cv = 3 * 0.25f + 0.125f, e = 0.02f;
        Vector3 p0 = Vector3Subtract(s->a, side), p1 = Vector3Add(s->a, side), p2 = Vector3Add(s->b, side), p3 = Vector3Subtract(s->b, side);
        vtx(p0, cu - e, cv - e, c); vtx(p3, cu - e, cv + e, c); vtx(p2, cu + e, cv + e, c);
        vtx(p0, cu - e, cv - e, c); vtx(p2, cu + e, cv + e, c); vtx(p1, cu + e, cv - e, c);
    }
}

static void draw_tracers(void)
{
    for (int i = 0; i < ntracer; i++) {
        Tracer *t = &tracers[i];
        Vector3 d = Vector3Subtract(t->b, t->a);
        float len = Vector3Length(d);
        if (len < 0.1f) continue;
        Vector3 dir = Vector3Scale(d, 1 / len);
        float head = fminf(len, (t->age / 0.09f) * len + 3), tail = fmaxf(0, head - 6);
        Vector3 a = Vector3Add(t->a, Vector3Scale(dir, tail)), b = Vector3Add(t->a, Vector3Scale(dir, head));
        Vector3 w = Vector3Scale(Vector3Normalize(Vector3CrossProduct(dir, Vector3Subtract(a, cam.position))), 0.025f);
        Vector4 c = { 6, 4.5f, 2, 1 - t->age / 0.09f };
        quad(Vector3Subtract(a, w), Vector3Subtract(b, w), Vector3Add(b, w), Vector3Add(a, w), SP_STREAK, c);
    }
}

void fx_draw(int pass)
{
    if (pass != PASS_MAIN) return;
    if (!ready) fx_init();
    vb = vbbuf;
    nvb = 0;
    glUseProgram(prog);
    gl_mat(u_vp, r_vp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    float tint[3] = {
        Clamp(r_amb_col.x * 1.6f + r_sun_col.x * 0.18f, 0.02f, 1.2f),
        Clamp(r_amb_col.y * 1.6f + r_sun_col.y * 0.18f, 0.02f, 1.2f),
        Clamp(r_amb_col.z * 1.6f + r_sun_col.z * 0.18f, 0.02f, 1.2f),
    };

    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-2, -2);
    float skid_tint[3] = { 1, 1, 1 };
    draw_skids();
    if (nvb) flush(false, skid_tint);
    draw_decals();
    if (nvb) flush(false, tint);
    glDisable(GL_POLYGON_OFFSET_FILL);

    draw_parts(false);
    if (nvb) flush(false, tint);
    draw_parts(true);
    draw_tracers();
    if (nvb) flush(true, tint);
    glBindVertexArray(0);
}
