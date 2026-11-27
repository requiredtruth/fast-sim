// every model in the game is built here from code: cars, people, hair, guns, props, and the
// animation clips. nothing is loaded from disk. units: metres, y up, +z forward, +x is the model's left.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

CarModel car_models[CAR_MODELS];
Mesh body_mesh[BODY_COUNT];
Mesh hair_mesh[HAIR_COUNT];
Mesh gun_mesh[W_COUNT];
Mesh prop_mesh[PROP_COUNT];
Skeleton skel;
Clip clips[A_COUNT];

// ---------------------------------------------------------------- mesh builder

typedef struct {
    float *v, *n, *bw;
    unsigned char *c, *bi;
    unsigned short *ix;
    int nv, ni, cv, ci;
    bool skin;
} MB;

static Vector3 V3(float x, float y, float z) { return (Vector3){ x, y, z }; }
static Vector3 add(Vector3 a, Vector3 b) { return Vector3Add(a, b); }
static Vector3 sub(Vector3 a, Vector3 b) { return Vector3Subtract(a, b); }
static Vector3 scl(Vector3 a, float k) { return Vector3Scale(a, k); }

typedef struct { unsigned char r, g, b, a; } RGBA;
static RGBA rgba(uint32_t hex, int a) { return (RGBA){ hex >> 16 & 255, hex >> 8 & 255, hex & 255, (unsigned char)a }; }

// current skinning for new vertices: up to two bones
static int cur_b0, cur_b1;
static float cur_w1;

static int mb_vert(MB *m, Vector3 p, Vector3 n, RGBA c)
{
    if (m->nv == m->cv) {
        m->cv = m->cv ? m->cv * 2 : 512;
        m->v = realloc(m->v, m->cv * 12); m->n = realloc(m->n, m->cv * 12); m->c = realloc(m->c, m->cv * 4);
        if (m->skin) { m->bi = realloc(m->bi, m->cv * 4); m->bw = realloc(m->bw, m->cv * 16); }
    }
    int i = m->nv++;
    n = Vector3Normalize(n);
    memcpy(m->v + i * 3, &p, 12); memcpy(m->n + i * 3, &n, 12);
    m->c[i * 4] = c.r; m->c[i * 4 + 1] = c.g; m->c[i * 4 + 2] = c.b; m->c[i * 4 + 3] = c.a;
    if (m->skin) {
        unsigned char *b = m->bi + i * 4;
        float *w = m->bw + i * 4;
        b[0] = (unsigned char)cur_b0; b[1] = (unsigned char)cur_b1; b[2] = b[3] = 0;
        w[0] = 1 - cur_w1; w[1] = cur_w1; w[2] = w[3] = 0;
    }
    return i;
}

static void mb_tri(MB *m, int a, int b, int c)
{
    if (m->ni + 3 > m->ci) { m->ci = m->ci ? m->ci * 2 : 1024; m->ix = realloc(m->ix, m->ci * 2); }
    m->ix[m->ni++] = (unsigned short)a; m->ix[m->ni++] = (unsigned short)b; m->ix[m->ni++] = (unsigned short)c;
}

// flat quad; the normal is turned to face away from `in`, a point inside the part
static void mb_quad(MB *m, Vector3 a, Vector3 b, Vector3 c, Vector3 d, RGBA col, Vector3 in)
{
    Vector3 n = Vector3CrossProduct(sub(b, a), sub(d, a));
    if (Vector3LengthSqr(n) < 1e-12f) n = Vector3CrossProduct(sub(c, b), sub(d, b));
    Vector3 mid = scl(add(add(a, b), add(c, d)), 0.25f);
    if (Vector3DotProduct(n, sub(mid, in)) < 0) n = Vector3Negate(n);
    int i0 = mb_vert(m, a, n, col), i1 = mb_vert(m, b, n, col), i2 = mb_vert(m, c, n, col), i3 = mb_vert(m, d, n, col);
    mb_tri(m, i0, i1, i2); mb_tri(m, i0, i2, i3);
}

static void mb_tri_flat(MB *m, Vector3 a, Vector3 b, Vector3 c, RGBA col, Vector3 in)
{
    Vector3 n = Vector3CrossProduct(sub(b, a), sub(c, a));
    Vector3 mid = scl(add(add(a, b), c), 1 / 3.0f);
    if (Vector3DotProduct(n, sub(mid, in)) < 0) n = Vector3Negate(n);
    mb_tri(m, mb_vert(m, a, n, col), mb_vert(m, b, n, col), mb_vert(m, c, n, col));
}

// box from two corners
static void mb_box(MB *m, Vector3 lo, Vector3 hi, RGBA col)
{
    Vector3 c = scl(add(lo, hi), 0.5f);
    Vector3 p[8];
    for (int k = 0; k < 8; k++) p[k] = V3(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z);
    static const int f[6][4] = { { 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 } };
    for (int i = 0; i < 6; i++) mb_quad(m, p[f[i][0]], p[f[i][1]], p[f[i][2]], p[f[i][3]], col, c);
}

// box with its own axes: centre, half sizes, unit axes
static void mb_obox(MB *m, Vector3 c, Vector3 h, Vector3 ax, Vector3 ay, Vector3 az, RGBA col)
{
    Vector3 p[8];
    for (int k = 0; k < 8; k++)
        p[k] = add(c, add(scl(ax, k & 1 ? h.x : -h.x), add(scl(ay, k & 2 ? h.y : -h.y), scl(az, k & 4 ? h.z : -h.z))));
    static const int f[6][4] = { { 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 } };
    for (int i = 0; i < 6; i++) mb_quad(m, p[f[i][0]], p[f[i][1]], p[f[i][2]], p[f[i][3]], col, c);
}

// cylinder between two points, n sides, smooth sides and flat caps
static void mb_cyl(MB *m, Vector3 a, Vector3 b, float r0, float r1, int n, RGBA side, RGBA cap, bool caps)
{
    Vector3 ax = Vector3Normalize(sub(b, a));
    Vector3 u = Vector3Normalize(Vector3Perpendicular(ax)), w = Vector3CrossProduct(ax, u);
    int base = m->nv;
    for (int i = 0; i <= n; i++) {
        float t = i * 2 * PI / n;
        Vector3 d = add(scl(u, cosf(t)), scl(w, sinf(t)));
        Vector3 nn = Vector3Normalize(add(d, scl(ax, (r0 - r1) / fmaxf(Vector3Distance(a, b), 1e-4f))));
        mb_vert(m, add(a, scl(d, r0)), nn, side);
        mb_vert(m, add(b, scl(d, r1)), nn, side);
    }
    for (int i = 0; i < n; i++) {
        int k = base + i * 2;
        mb_tri(m, k, k + 2, k + 3); mb_tri(m, k, k + 3, k + 1);
    }
    if (!caps) return;
    for (int e = 0; e < 2; e++) {
        Vector3 c = e ? b : a, nn = e ? ax : Vector3Negate(ax);
        float r = e ? r1 : r0;
        int ci = mb_vert(m, c, nn, cap), first = m->nv;
        for (int i = 0; i <= n; i++) {
            float t = i * 2 * PI / n;
            mb_vert(m, add(c, add(scl(u, cosf(t) * r), scl(w, sinf(t) * r))), nn, cap);
        }
        for (int i = 0; i < n; i++) mb_tri(m, ci, first + i, first + i + 1);
    }
}

// ellipsoid, smooth
static void mb_ellipsoid(MB *m, Vector3 c, Vector3 r, int seg, int rings, RGBA col)
{
    int base = m->nv;
    for (int j = 0; j <= rings; j++) {
        float ph = PI * j / rings - PI / 2;
        for (int i = 0; i <= seg; i++) {
            float th = 2 * PI * i / seg;
            Vector3 d = V3(cosf(ph) * cosf(th), sinf(ph), cosf(ph) * sinf(th));
            mb_vert(m, add(c, V3(d.x * r.x, d.y * r.y, d.z * r.z)), V3(d.x / r.x, d.y / r.y, d.z / r.z), col);
        }
    }
    for (int j = 0; j < rings; j++)
        for (int i = 0; i < seg; i++) {
            int a = base + j * (seg + 1) + i, b = a + seg + 1;
            mb_tri(m, a, b, b + 1); mb_tri(m, a, b + 1, a + 1);
        }
}

static Mesh mb_upload(MB *m)
{
    Mesh r = { 0 };
    r.vertexCount = m->nv;
    r.triangleCount = m->ni / 3;
    r.vertices = m->v; r.normals = m->n; r.colors = m->c; r.indices = m->ix;
    if (m->skin) { r.boneIds = m->bi; r.boneWeights = m->bw; r.boneCount = skel.count; }
    UploadMesh(&r, false);
    memset(m, 0, sizeof *m);
    return r;
}

// ---------------------------------------------------------------- cars

// one cross-section station of a lofted body
typedef struct { float z, hw, y0, y1, ch; } St;

enum { F_SIDE, F_TOP, F_BOTTOM, F_CHAMF_TOP, F_CHAMF_BOT };
typedef RGBA (*FaceCol)(int kind, int seg, float z);

static void sect(const St *s, Vector3 *p)
{
    float w = s->hw, c = fminf(s->ch, fminf(w * 0.45f, (s->y1 - s->y0) * 0.45f));
    p[0] = V3(-w + c, s->y0, s->z); p[1] = V3(w - c, s->y0, s->z);
    p[2] = V3(w, s->y0 + c, s->z);  p[3] = V3(w, s->y1 - c, s->z);
    p[4] = V3(w - c, s->y1, s->z);  p[5] = V3(-w + c, s->y1, s->z);
    p[6] = V3(-w, s->y1 - c, s->z); p[7] = V3(-w, s->y0 + c, s->z);
}

static int face_kind(int e)
{
    switch (e) {
    case 0: return F_BOTTOM;
    case 1: case 7: return F_CHAMF_BOT;
    case 2: case 6: return F_SIDE;
    case 3: case 5: return F_CHAMF_TOP;
    default: return F_TOP;
    }
}

static void loft(MB *m, const St *st, int n, FaceCol fc, RGBA capf, RGBA capb)
{
    Vector3 in = V3(0, (st[0].y0 + st[0].y1) * 0.5f, (st[0].z + st[n - 1].z) * 0.5f);
    for (int s = 0; s + 1 < n; s++) {
        Vector3 a[8], b[8];
        sect(&st[s], a); sect(&st[s + 1], b);
        Vector3 inner = V3(0, (st[s].y0 + st[s].y1 + st[s + 1].y0 + st[s + 1].y1) * 0.25f, (st[s].z + st[s + 1].z) * 0.5f);
        for (int e = 0; e < 8; e++) {
            int f = (e + 1) & 7;
            // edge e runs from point e to e+1
            int kind = face_kind(e == 3 ? 3 : e == 4 ? 4 : e == 5 ? 5 : e);
            mb_quad(m, a[e], a[f], b[f], b[e], fc(kind, s, (st[s].z + st[s + 1].z) * 0.5f), inner);
        }
    }
    (void)in;
    for (int e = 0; e < 2; e++) {
        const St *s = e ? &st[n - 1] : &st[0];
        Vector3 p[8]; sect(s, p);
        Vector3 c = V3(0, (s->y0 + s->y1) * 0.5f, s->z + (e ? -1 : 1));
        RGBA col = e ? capf : capb;
        mb_quad(m, p[0], p[1], p[4], p[5], col, c);
        mb_quad(m, p[1], p[2], p[3], p[4], col, c);
        mb_quad(m, p[5], p[6], p[7], p[0], col, c);
    }
}

static RGBA PAINT, GLASS, DARK, TRIM, CHROME, HEAD, TAIL, RUST, AMBER;
static float g_belt, g_glass_z0, g_glass_z1;

static RGBA fc_body(int kind, int seg, float z) { return kind == F_BOTTOM ? DARK : PAINT; }
static RGBA fc_cab(int kind, int seg, float z)
{
    if (kind == F_TOP) return PAINT;
    if (kind == F_BOTTOM) return DARK;
    return GLASS;
}
static RGBA fc_dark(int kind, int seg, float z) { return DARK; }
static RGBA fc_box(int kind, int seg, float z) { return kind == F_BOTTOM ? DARK : rgba(0xd8d4cc, 0); }

static Mesh wheel_mesh(float r, float w, bool offroad)
{
    MB m = { 0 };
    RGBA tire = rgba(0x161616, 0), rim = rgba(offroad ? 0x3a3a3c : 0x9a9ca0, 0), hub = rgba(0x444446, 0);
    int n = 18;
    float hw = w * 0.5f, rr = r * (offroad ? 0.52f : 0.62f);
    // tread with a rounded shoulder: three bands
    mb_cyl(&m, V3(-hw, 0, 0), V3(-hw * 0.7f, 0, 0), r * 0.9f, r, n, tire, tire, false);
    mb_cyl(&m, V3(-hw * 0.7f, 0, 0), V3(hw * 0.7f, 0, 0), r, r, n, tire, tire, false);
    mb_cyl(&m, V3(hw * 0.7f, 0, 0), V3(hw, 0, 0), r, r * 0.9f, n, tire, tire, false);
    // sidewalls
    for (int s = -1; s <= 1; s += 2) {
        float x = hw * s;
        int base = m.nv;
        for (int i = 0; i <= n; i++) {
            float t = i * 2 * PI / n;
            Vector3 d = V3(0, cosf(t), sinf(t));
            mb_vert(&m, V3(x, d.y * r * 0.9f, d.z * r * 0.9f), V3((float)s, 0, 0), tire);
            mb_vert(&m, V3(x * 0.92f, d.y * rr, d.z * rr), V3((float)s, 0, 0), tire);
        }
        for (int i = 0; i < n; i++) { int k = base + i * 2; mb_tri(&m, k, k + 2, k + 3); mb_tri(&m, k, k + 3, k + 1); }
    }
    // rim dish and spokes on the outer (+x) side
    mb_cyl(&m, V3(hw * 0.92f, 0, 0), V3(hw * 0.6f, 0, 0), rr, rr * 0.85f, n, rim, rim, true);
    mb_cyl(&m, V3(hw * 0.95f, 0, 0), V3(hw * 0.8f, 0, 0), rr * 0.25f, rr * 0.25f, 8, hub, hub, true);
    for (int k = 0; k < (offroad ? 6 : 5); k++) {
        float t = k * 2 * PI / (offroad ? 6 : 5);
        Vector3 d = V3(0, cosf(t), sinf(t)), e = V3(0, -sinf(t), cosf(t));
        mb_obox(&m, add(V3(hw * 0.93f, 0, 0), scl(d, rr * 0.55f)), V3(0.012f, rr * 0.36f, 0.03f), V3(1, 0, 0), d, e, rim);
    }
    // the inner face of the wheel, dark
    mb_cyl(&m, V3(-hw * 0.92f, 0, 0), V3(-hw * 0.5f, 0, 0), rr, rr * 0.5f, n, hub, hub, true);
    return mb_upload(&m);
}

typedef struct {
    float len, width, wb, wr, ride;    // overall length, width, wheelbase, wheel radius, body floor height
    float hood_h, belt, roof, cab_z0, cab_z1, ws_rake, rear_rake;
    int style;                           // 0 sedan 1 hatch 2 wagon 3 pickup 4 suv 5 van 6 jeep 7 muscle 8 truck 9 raider
    float mass;
} CarSpec;

static void lamp(MB *m, float x, float y, float z, float hw, float hh, float d, RGBA c)
{
    mb_box(m, V3(x - hw, y - hh, z - d), V3(x + hw, y + hh, z + d), c);
}

static void build_car(int id, CarSpec s)
{
    CarModel *cm = &car_models[id];
    MB m = { 0 };
    float L = s.len, W = s.width * 0.5f, zf = L * 0.5f, zr = -L * 0.5f, y0 = s.ride, yb = s.belt;
    float nose_y = s.hood_h - 0.12f;

    // lower body: nose, hood line, belt, tail
    St lower[] = {
        { zr, W - 0.08f, y0 + 0.08f, yb - 0.12f, 0.08f },
        { zr + 0.12f, W - 0.02f, y0, yb - 0.04f, 0.1f },
        { zr + 0.5f, W, y0, yb, 0.12f },
        { zf - 0.7f, W, y0, s.hood_h, 0.12f },
        { zf - 0.15f, W - 0.04f, y0 + 0.02f, nose_y, 0.1f },
        { zf, W - 0.1f, y0 + 0.08f, nose_y - 0.08f, 0.08f },
    };
    // the hood runs down from the windscreen base to the nose
    lower[3].y1 = fmaxf(s.hood_h, yb);
    if (s.style == 5 || s.style == 8) { // vans and trucks: blunt
        lower[3].y1 = yb; lower[4].y1 = yb - 0.05f; lower[5].y1 = yb - 0.15f;
    }
    if (s.style == 3 || s.style == 8) { // pickups and trucks: cab and bed are separate; body ends behind the cab
        lower[0].z = s.cab_z0 - 0.25f; lower[1].z = s.cab_z0 - 0.15f; lower[2].z = s.cab_z0;
    }
    loft(&m, lower, 6, fc_body, PAINT, PAINT);

    // cabin / greenhouse
    if (s.style != 9) {
        float z0 = s.cab_z0, z1 = s.cab_z1, top = s.roof;
        float cw = W - 0.1f;
        St cab[] = {
            { z0, cw - 0.04f, yb - 0.02f, top - (s.style == 0 || s.style == 7 ? 0.18f : 0.05f), 0.16f },
            { z0 + s.rear_rake, cw - 0.1f, yb - 0.02f, top, 0.18f },
            { z1 - s.ws_rake, cw - 0.1f, yb - 0.02f, top, 0.18f },
            { z1, cw - 0.04f, yb - 0.02f, yb + 0.06f, 0.05f },
        };
        loft(&m, cab, 4, fc_cab, GLASS, GLASS);
        // pillars over the glass
        float pz[3] = { z1 - s.ws_rake * 0.5f, (z0 + z1) * 0.5f + 0.1f, z0 + s.rear_rake * 0.5f };
        for (int k = 0; k < 3; k++) {
            if (k == 1 && z1 - z0 < 1.4f) continue;
            float pw = k == 1 ? 0.06f : 0.08f;
            for (int sd = -1; sd <= 1; sd += 2) {
                float x = sd * (cw - 0.1f + 0.012f);
                if (k == 1) mb_box(&m, V3(x - 0.012f, yb, pz[k] - pw), V3(x + 0.012f, top - 0.02f, pz[k] + pw), PAINT);
                else {
                    float zz0 = k == 0 ? z1 : z0, zz1 = k == 0 ? z1 - s.ws_rake : z0 + s.rear_rake;
                    Vector3 a = V3(x, yb + 0.02f, zz0), b = V3(x, top - 0.03f, zz1);
                    Vector3 d = Vector3Normalize(sub(b, a));
                    mb_obox(&m, scl(add(a, b), 0.5f), V3(0.014f, Vector3Distance(a, b) * 0.5f, pw * 0.6f),
                            V3(1, 0, 0), d, Vector3CrossProduct(V3(1, 0, 0), d), PAINT);
                }
            }
        }
        if (s.style == 5 || s.style == 4 || s.style == 2) { // tall rears get a dark rear pillar band
            for (int sd = -1; sd <= 1; sd += 2) {
                float x = sd * (cw - 0.1f + 0.014f);
                mb_box(&m, V3(x - 0.012f, yb, z0 + 0.02f), V3(x + 0.012f, top - 0.04f, z0 + 0.22f), PAINT);
            }
        }
    }
    // beds, boxes and cages
    if (s.style == 3) {
        float bz0 = zr + 0.05f, bz1 = s.cab_z0 - 0.05f, by = yb + 0.02f;
        St bed[] = { { bz0, W, y0, by, 0.06f }, { bz1, W, y0, by, 0.06f } };
        loft(&m, bed, 2, fc_body, PAINT, PAINT);
        mb_box(&m, V3(-W + 0.08f, by - 0.01f, bz0 + 0.08f), V3(W - 0.08f, by + 0.005f, bz1 - 0.04f), DARK);
        for (int sd = -1; sd <= 1; sd += 2) mb_box(&m, V3(sd * W - 0.04f, by, bz0), V3(sd * W + 0.04f, by + 0.03f, bz1), TRIM);
    }
    if (s.style == 8) {
        float bz0 = zr + 0.02f, bz1 = s.cab_z0 - 0.12f;
        St box[] = { { bz0, W + 0.05f, y0 + 0.1f, s.roof + 0.35f, 0.05f }, { bz1, W + 0.05f, y0 + 0.1f, s.roof + 0.35f, 0.05f } };
        loft(&m, box, 2, fc_box, rgba(0xc8c4bc, 0), rgba(0xd8d4cc, 0));
        for (int k = 0; k < 5; k++) {
            float z = bz0 + (bz1 - bz0) * k / 4;
            for (int sd = -1; sd <= 1; sd += 2)
                mb_box(&m, V3(sd * (W + 0.06f) - 0.01f, y0 + 0.12f, z - 0.03f), V3(sd * (W + 0.06f) + 0.01f, s.roof + 0.33f, z + 0.03f), TRIM);
        }
    }
    if (s.style == 9) { // raider buggy: open tub, roll cage, plates, spikes
        float top = s.roof;
        RGBA bar = rgba(0x2a2a2a, 0);
        float cz0 = s.cab_z0, cz1 = s.cab_z1;
        for (int sd = -1; sd <= 1; sd += 2) {
            float x = sd * (W - 0.12f);
            mb_cyl(&m, V3(x, yb, cz1), V3(x * 0.92f, top, cz1 - 0.45f), 0.035f, 0.035f, 6, bar, bar, false);
            mb_cyl(&m, V3(x * 0.92f, top, cz1 - 0.45f), V3(x * 0.92f, top, cz0 + 0.2f), 0.035f, 0.035f, 6, bar, bar, false);
            mb_cyl(&m, V3(x * 0.92f, top, cz0 + 0.2f), V3(x, yb, cz0), 0.035f, 0.035f, 6, bar, bar, false);
        }
        mb_cyl(&m, V3(-(W - 0.12f) * 0.92f, top, cz1 - 0.45f), V3((W - 0.12f) * 0.92f, top, cz1 - 0.45f), 0.035f, 0.035f, 6, bar, bar, false);
        mb_cyl(&m, V3(-(W - 0.12f) * 0.92f, top, cz0 + 0.2f), V3((W - 0.12f) * 0.92f, top, cz0 + 0.2f), 0.035f, 0.035f, 6, bar, bar, false);
        // seats
        for (int sd = -1; sd <= 1; sd += 2) {
            mb_box(&m, V3(sd * 0.36f - 0.2f, yb - 0.25f, cz0 + 0.25f), V3(sd * 0.36f + 0.2f, yb - 0.1f, cz0 + 0.75f), DARK);
            mb_box(&m, V3(sd * 0.36f - 0.2f, yb - 0.1f, cz0 + 0.15f), V3(sd * 0.36f + 0.2f, yb + 0.45f, cz0 + 0.28f), DARK);
        }
        // front plow
        for (int k = 0; k < 5; k++) {
            float x = -W + 0.15f + k * (2 * W - 0.3f) / 4;
            mb_obox(&m, V3(x, y0 + 0.25f, zf + 0.18f), V3(0.04f, 0.22f, 0.04f), V3(1, 0, 0), Vector3Normalize(V3(0, 1, 0.6f)), Vector3Normalize(V3(0, -0.6f, 1)), RUST);
        }
        mb_box(&m, V3(-W, y0 + 0.08f, zf + 0.02f), V3(W, y0 + 0.2f, zf + 0.12f), RUST);
        // side armour plates, bolted on unevenly
        for (int sd = -1; sd <= 1; sd += 2)
            for (int k = 0; k < 3; k++) {
                float z = zr + 0.6f + k * (L - 1.2f) / 2.6f;
                mb_box(&m, V3(sd * W - 0.02f + sd * 0.02f, y0 + 0.1f + (k & 1) * 0.05f, z), V3(sd * W + 0.03f * sd + 0.02f, yb + 0.05f, z + 0.7f), RUST);
            }
    }
    if (s.style == 6) { // jeep: spare on the back, flat fenders
        mb_cyl(&m, V3(0, yb, zr - 0.02f), V3(0, yb, zr - 0.26f), s.wr * 0.95f, s.wr * 0.95f, 14, rgba(0x161616, 0), rgba(0x303032, 0), true);
    }
    if (s.style == 2 || s.style == 4) { // roof rails
        for (int sd = -1; sd <= 1; sd += 2)
            mb_box(&m, V3(sd * (W - 0.3f) - 0.025f, s.roof, s.cab_z0 + 0.2f), V3(sd * (W - 0.3f) + 0.025f, s.roof + 0.06f, s.cab_z1 - s.ws_rake - 0.1f), TRIM);
    }
    if (s.style == 7) { // muscle: hood scoop and a spoiler
        mb_box(&m, V3(-0.25f, s.hood_h - 0.02f, zf - 1.3f), V3(0.25f, s.hood_h + 0.08f, zf - 0.75f), DARK);
        mb_box(&m, V3(-W + 0.1f, yb + 0.12f, zr + 0.05f), V3(W - 0.1f, yb + 0.16f, zr + 0.32f), DARK);
        for (int sd = -1; sd <= 1; sd += 2) mb_box(&m, V3(sd * (W - 0.3f) - 0.03f, yb - 0.02f, zr + 0.15f), V3(sd * (W - 0.3f) + 0.03f, yb + 0.13f, zr + 0.22f), DARK);
    }

    // wheel arches, bumpers, lights, grille, mirrors, plates
    float wz[2] = { s.wb * 0.5f, -s.wb * 0.5f };
    for (int a = 0; a < 2; a++)
        for (int sd = -1; sd <= 1; sd += 2) {
            float x = sd * (W + 0.005f);
            mb_box(&m, V3(x - 0.02f, y0 - 0.02f, wz[a] - s.wr - 0.12f), V3(x + 0.02f, s.wr * 2 + 0.06f - 0.05f, wz[a] + s.wr + 0.12f), DARK);
            mb_box(&m, V3(x - 0.03f * sd - 0.015f, s.wr * 2 + 0.01f, wz[a] - s.wr - 0.14f), V3(x + 0.015f + 0.03f * sd, s.wr * 2 + 0.07f, wz[a] + s.wr + 0.14f), s.style == 6 || s.style == 9 ? DARK : PAINT);
        }
    bool big = s.style >= 3 && s.style != 7;
    RGBA bump = s.style == 7 || s.style == 0 ? CHROME : DARK;
    mb_box(&m, V3(-W + 0.02f, y0 + 0.02f, zf - 0.05f), V3(W - 0.02f, y0 + (big ? 0.3f : 0.22f), zf + 0.08f), bump);
    mb_box(&m, V3(-W + 0.02f, y0 + 0.02f, zr - 0.08f), V3(W - 0.02f, y0 + (big ? 0.3f : 0.22f), zr + 0.05f), bump);
    float ly = (y0 + 0.22f + nose_y) * 0.5f;
    if (s.style == 5 || s.style == 8) ly = y0 + 0.5f;
    mb_box(&m, V3(-W * 0.55f, ly - 0.09f, zf - 0.02f), V3(W * 0.55f, ly + 0.09f, zf + 0.01f + 0.02f), rgba(0x1a1a1a, 0));
    for (int sd = -1; sd <= 1; sd += 2) {
        lamp(&m, sd * (W - 0.24f), ly, zf + 0.01f, 0.15f, 0.07f, 0.03f, HEAD);
        lamp(&m, sd * (W - 0.12f), ly - 0.12f, zf + 0.02f, 0.05f, 0.03f, 0.02f, AMBER);
        float ty = s.style == 3 ? yb - 0.15f : yb - 0.14f;
        if (s.style == 5 || s.style == 4 || s.style == 2 || s.style == 8) ty = yb - 0.05f;
        lamp(&m, sd * (W - 0.15f), ty, zr - 0.01f, 0.11f, 0.08f, 0.03f, TAIL);
        // mirror
        if (s.style != 9) mb_box(&m, V3(sd * W - 0.02f + sd * 0.07f, yb - 0.02f, s.cab_z1 - 0.15f), V3(sd * W + 0.02f + sd * 0.17f, yb + 0.11f, s.cab_z1 - 0.06f), PAINT);
        // door seams
        if (s.style != 9 && s.style != 8)
            mb_box(&m, V3(sd * (W + 0.006f) - 0.004f, y0 + 0.12f, (s.cab_z0 + s.cab_z1) * 0.5f - 0.01f), V3(sd * (W + 0.006f) + 0.004f, yb - 0.04f, (s.cab_z0 + s.cab_z1) * 0.5f + 0.01f), TRIM);
    }
    mb_box(&m, V3(-0.2f, y0 + 0.08f, zr - 0.1f), V3(0.2f, y0 + 0.2f, zr - 0.08f), rgba(0xe4e0c8, 0));
    // underside, so the ground doesn't show through from low angles
    mb_box(&m, V3(-W + 0.15f, y0 - 0.06f, zr + 0.3f), V3(W - 0.15f, y0, zf - 0.3f), rgba(0x0c0c0c, 0));

    cm->body = mb_upload(&m);
    cm->wheel = wheel_mesh(s.wr, s.wr * 0.75f, s.style == 6 || s.style == 9 || s.style == 4);
    float tx = W - s.wr * 0.38f;
    cm->wheel_pos[0] = V3(tx, s.wr, wz[0]); cm->wheel_pos[1] = V3(-tx, s.wr, wz[0]);
    cm->wheel_pos[2] = V3(tx, s.wr, wz[1]); cm->wheel_pos[3] = V3(-tx, s.wr, wz[1]);
    cm->wheel_r = s.wr;
    cm->size = V3(s.width, s.roof, s.len);
    cm->seat = V3(0.36f * s.width / 1.9f, s.belt - 0.18f, (s.cab_z0 + s.cab_z1) * 0.5f + 0.05f);
    if (s.style == 9) cm->seat = V3(0.36f, s.belt - 0.28f, s.cab_z0 + 0.5f);
    cm->mass_kg = s.mass;
}

static void build_cars(void)
{
    PAINT = rgba(0xffffff, CM_PAINT); GLASS = rgba(0x202830, CM_GLASS); DARK = rgba(0x1c1c1e, CM_PLAIN);
    TRIM = rgba(0x101012, CM_PLAIN); CHROME = rgba(0xb8bcc2, CM_PLAIN); HEAD = rgba(0xf4f0e0, CM_HEAD);
    TAIL = rgba(0xc01010, CM_TAIL); RUST = rgba(0x5a3a26, CM_PLAIN); AMBER = rgba(0xe08a10, CM_PLAIN);
    //            len   width wb    wr    ride  hood  belt  roof  cab0   cab1  ws    rear  style mass
    build_car(CAR_SEDAN,  (CarSpec){ 4.7f, 1.82f, 2.75f, 0.33f, 0.28f, 0.86f, 0.92f, 1.45f, -1.2f, 0.85f, 0.55f, 0.5f, 0, 1500 });
    build_car(CAR_HATCH,  (CarSpec){ 4.0f, 1.72f, 2.5f, 0.31f, 0.26f, 0.84f, 0.9f, 1.48f, -1.65f, 0.75f, 0.55f, 0.25f, 1, 1200 });
    build_car(CAR_WAGON,  (CarSpec){ 4.85f, 1.82f, 2.8f, 0.33f, 0.28f, 0.86f, 0.92f, 1.5f, -2.15f, 0.85f, 0.55f, 0.12f, 2, 1600 });
    build_car(CAR_PICKUP, (CarSpec){ 5.3f, 1.95f, 3.2f, 0.4f, 0.45f, 1.12f, 1.14f, 1.85f, -0.35f, 1.15f, 0.45f, 0.08f, 3, 2000 });
    build_car(CAR_SUV,    (CarSpec){ 4.8f, 1.92f, 2.85f, 0.39f, 0.42f, 1.08f, 1.1f, 1.82f, -2.0f, 0.95f, 0.5f, 0.12f, 4, 2000 });
    build_car(CAR_VAN,    (CarSpec){ 5.0f, 1.95f, 3.0f, 0.35f, 0.34f, 1.0f, 1.08f, 2.05f, -2.4f, 1.55f, 0.55f, 0.05f, 5, 1950 });
    build_car(CAR_JEEP,   (CarSpec){ 4.1f, 1.85f, 2.45f, 0.42f, 0.5f, 1.12f, 1.14f, 1.9f, -1.7f, 0.55f, 0.18f, 0.05f, 6, 1700 });
    build_car(CAR_MUSCLE, (CarSpec){ 4.75f, 1.88f, 2.8f, 0.34f, 0.24f, 0.8f, 0.86f, 1.32f, -1.15f, 0.55f, 0.6f, 0.65f, 7, 1550 });
    build_car(CAR_TRUCK,  (CarSpec){ 6.6f, 2.2f, 4.0f, 0.46f, 0.5f, 1.35f, 1.4f, 2.3f, 1.4f, 3.0f, 0.3f, 0.02f, 8, 4200 });
    build_car(CAR_RAIDER, (CarSpec){ 4.2f, 1.95f, 2.6f, 0.45f, 0.45f, 0.95f, 0.95f, 1.85f, -1.1f, 0.65f, 0.3f, 0.1f, 9, 1500 });
}

// ---------------------------------------------------------------- skeleton

// pelvis spine1 spine2 chest neck head | clavL armL forearmL handL | clavR armR forearmR handR |
// thighL calfL footL ballL | thighR calfR footR ballR. bind pose: standing, arms down, facing +z, bone frames = world.
static const int bone_parent[22] = { -1, 0, 1, 2, 3, 4, 3, 6, 7, 8, 3, 10, 11, 12, 0, 14, 15, 16, 0, 18, 19, 20 };
static const float bone_off[22][3] = {
    { 0, 0.98f, 0 }, { 0, 0.1f, 0 }, { 0, 0.14f, 0 }, { 0, 0.14f, 0 }, { 0, 0.15f, 0 }, { 0, 0.08f, 0 },
    { 0.05f, 0.09f, 0 }, { 0.15f, 0.0f, 0 }, { 0, -0.28f, 0 }, { 0, -0.25f, 0 },
    { -0.05f, 0.09f, 0 }, { -0.15f, 0.0f, 0 }, { 0, -0.28f, 0 }, { 0, -0.25f, 0 },
    { 0.095f, -0.05f, 0 }, { 0, -0.43f, 0 }, { 0, -0.42f, 0.0f }, { 0, -0.06f, 0.12f },
    { -0.095f, -0.05f, 0 }, { 0, -0.43f, 0 }, { 0, -0.42f, 0.0f }, { 0, -0.06f, 0.12f },
};
enum { B_PELVIS, B_SP1, B_SP2, B_CHEST, B_NECK, B_HEAD, B_CLAV_L, B_ARM_L, B_FORE_L, B_HAND_L, B_CLAV_R, B_ARM_R, B_FORE_R,
       B_HAND_R, B_THIGH_L, B_CALF_L, B_FOOT_L, B_BALL_L, B_THIGH_R, B_CALF_R, B_FOOT_R, B_BALL_R, B_N };
static Vector3 bw[B_N];   // bind world positions

static void build_skeleton(void)
{
    skel.count = B_N;
    for (int i = 0; i < B_N; i++) {
        skel.parent[i] = bone_parent[i];
        skel.bind_pos[i] = V3(bone_off[i][0], bone_off[i][1], bone_off[i][2]);
        skel.bind_rot[i] = QuaternionIdentity();
        bw[i] = bone_parent[i] < 0 ? skel.bind_pos[i] : add(bw[bone_parent[i]], skel.bind_pos[i]);
        skel.inv_bind[i] = MatrixTranslate(-bw[i].x, -bw[i].y, -bw[i].z);
    }
    skel.head = B_HEAD; skel.hand_r = B_HAND_R; skel.hand_l = B_HAND_L; skel.hips = B_PELVIS; skel.spine = B_SP2;
}

// ---------------------------------------------------------------- people

static RGBA reg(int r) { return (RGBA){ (unsigned char)r, 200, 255, 255 }; }

// a tapered tube from a to b with elliptical rings; weights blend from bone `pb` at the start to `b` along it
typedef struct { float t, rx, rz; } Ring;

static void limb(MB *m, Vector3 a, Vector3 b, const Ring *rings, int nr, int bone, int pb, float blend, int region)
{
    Vector3 ax = sub(b, a);
    float len = Vector3Length(ax);
    ax = scl(ax, 1 / len);
    Vector3 side = fabsf(ax.x) > 0.9f ? V3(0, 1, 0) : V3(1, 0, 0);
    Vector3 u = Vector3Normalize(Vector3CrossProduct(ax, Vector3CrossProduct(side, ax)));
    Vector3 w = Vector3CrossProduct(ax, u);
    // u ~ x, w ~ z for vertical limbs
    if (Vector3DotProduct(u, V3(1, 0, 0)) < 0) u = Vector3Negate(u);
    w = Vector3CrossProduct(ax, u);
    const int n = 10;
    RGBA col = reg(region);
    int base = m->nv;
    for (int r = 0; r < nr; r++) {
        Vector3 c = add(a, scl(ax, rings[r].t * len));
        float k = rings[r].t < blend ? 0.5f * (1 - rings[r].t / blend) : 0;
        cur_b0 = bone; cur_b1 = pb >= 0 ? pb : bone; cur_w1 = pb >= 0 ? k : 0;
        for (int i = 0; i <= n; i++) {
            float t = i * 2 * PI / n;
            Vector3 d = add(scl(u, cosf(t) * rings[r].rx), scl(w, sinf(t) * rings[r].rz));
            Vector3 nn = add(scl(u, cosf(t) / rings[r].rx), scl(w, sinf(t) / rings[r].rz));
            if (r == 0) nn = add(nn, scl(ax, -6));
            if (r == nr - 1) nn = add(nn, scl(ax, 6));
            mb_vert(m, add(c, d), nn, col);
        }
    }
    for (int r = 0; r + 1 < nr; r++)
        for (int i = 0; i < n; i++) {
            int p = base + r * (n + 1) + i, q = p + n + 1;
            mb_tri(m, p, q, q + 1); mb_tri(m, p, q + 1, p + 1);
        }
    // caps
    for (int e = 0; e < 2; e++) {
        int r = e ? nr - 1 : 0;
        Vector3 c = add(a, scl(ax, rings[r].t * len));
        cur_b0 = bone; cur_b1 = bone; cur_w1 = 0;
        int ci = mb_vert(m, add(c, scl(ax, e ? 0.01f : -0.01f)), e ? ax : Vector3Negate(ax), col);
        for (int i = 0; i < n; i++) {
            int p = base + r * (n + 1) + i;
            mb_tri(m, ci, p, p + 1);
        }
    }
}

static void skin_to(int b0, int b1, float w1) { cur_b0 = b0; cur_b1 = b1; cur_w1 = w1; }

// torso: rings stacked along y with weights spread over the spine bones by height
static void torso(MB *m, bool female)
{
    typedef struct { float y, rx, rz, dz; int region; } TR;
    TR fem[] = {
        { 0.84f, 0.15f, 0.11f, 0.0f, R_PANTS }, { 0.95f, 0.175f, 0.125f, -0.005f, R_PANTS }, { 1.04f, 0.15f, 0.11f, 0, R_SHIRT },
        { 1.13f, 0.125f, 0.1f, 0.005f, R_SHIRT }, { 1.25f, 0.14f, 0.115f, 0.015f, R_SHIRT }, { 1.34f, 0.155f, 0.125f, 0.02f, R_SHIRT },
        { 1.43f, 0.165f, 0.115f, 0.0f, R_SHIRT }, { 1.49f, 0.15f, 0.09f, -0.01f, R_SHIRT }, { 1.53f, 0.07f, 0.06f, -0.01f, R_SHIRT },
    };
    TR mal[] = {
        { 0.84f, 0.15f, 0.11f, 0.0f, R_PANTS }, { 0.95f, 0.16f, 0.12f, 0.0f, R_PANTS }, { 1.04f, 0.155f, 0.115f, 0.005f, R_SHIRT },
        { 1.13f, 0.155f, 0.115f, 0.01f, R_SHIRT }, { 1.25f, 0.17f, 0.12f, 0.012f, R_SHIRT }, { 1.34f, 0.185f, 0.125f, 0.01f, R_SHIRT },
        { 1.43f, 0.195f, 0.115f, 0.0f, R_SHIRT }, { 1.5f, 0.17f, 0.09f, -0.01f, R_SHIRT }, { 1.54f, 0.075f, 0.065f, -0.01f, R_SHIRT },
    };
    TR *t = female ? fem : mal;
    const int nr = 9, n = 14;
    int base = m->nv;
    for (int r = 0; r < nr; r++) {
        float y = t[r].y;
        // pick the two spine bones this height sits between
        static const float by[5] = { 0.98f, 1.08f, 1.22f, 1.36f, 1.51f };
        static const int bb[5] = { B_PELVIS, B_SP1, B_SP2, B_CHEST, B_NECK };
        int k = 0;
        while (k < 3 && y > by[k + 1]) k++;
        float f = Clamp((y - by[k]) / (by[k + 1] - by[k]), 0, 1);
        if (y < by[0]) { k = 0; f = 0; }
        skin_to(bb[k], bb[k + 1], f * f * (3 - 2 * f));
        // waist band: the shirt/pants border sits at the ring between pants and shirt
        for (int i = 0; i <= n; i++) {
            float a = i * 2 * PI / n;
            float cx = cosf(a), sz = sinf(a);
            float rz = t[r].rz * (sz > 0 ? 1.0f : 0.92f);
            if (female && r == 5 && sz > 0.3f) rz *= 1.12f;
            Vector3 p = V3(cx * t[r].rx, y, sz * rz + t[r].dz);
            mb_vert(m, p, V3(cx / t[r].rx, 0, sz / rz), reg(t[r].region));
        }
    }
    for (int r = 0; r + 1 < nr; r++)
        for (int i = 0; i < n; i++) {
            int p = base + r * (n + 1) + i, q = p + n + 1;
            mb_tri(m, p, q, q + 1); mb_tri(m, p, q + 1, p + 1);
        }
    // seat of the pants, closing the bottom
    skin_to(B_PELVIS, B_PELVIS, 0);
    int c = mb_vert(m, V3(0, 0.8f, 0), V3(0, -1, 0), reg(R_PANTS));
    for (int i = 0; i < n; i++) mb_tri(m, c, base + i, base + i + 1);
    // belt
    skin_to(B_PELVIS, B_SP1, 0.3f);
    limb(m, V3(0, 1.0f, 0), V3(0, 1.035f, 0), (Ring[]){ { 0, female ? 0.158f : 0.162f, 0.122f }, { 1, female ? 0.154f : 0.16f, 0.12f } }, 2, B_PELVIS, B_SP1, 0.0f, R_SHOES);
}

static void head(MB *m, bool female)
{
    skin_to(B_HEAD, B_HEAD, 0);
    // neck
    limb(m, V3(0, 1.48f, -0.005f), V3(0, 1.62f, 0.0f), (Ring[]){ { 0, 0.055f, 0.055f }, { 1, 0.05f, 0.05f } }, 2, B_NECK, B_HEAD, 0.5f, R_SKIN);
    skin_to(B_HEAD, B_HEAD, 0);
    mb_ellipsoid(m, V3(0, 1.71f, 0.015f), V3(0.088f, 0.115f, 0.1f), 14, 10, reg(R_SKIN));
    // jaw, nose, ears
    mb_ellipsoid(m, V3(0, 1.645f, 0.04f), V3(0.07f, 0.05f, 0.07f), 10, 6, reg(R_SKIN));
    mb_ellipsoid(m, V3(0, 1.69f, 0.112f), V3(0.016f, 0.026f, 0.022f), 6, 4, reg(R_SKIN));
    for (int s = -1; s <= 1; s += 2) {
        mb_ellipsoid(m, V3(s * 0.088f, 1.705f, 0.0f), V3(0.015f, 0.03f, 0.022f), 6, 4, reg(R_SKIN));
        mb_ellipsoid(m, V3(s * 0.034f, 1.722f, 0.096f), V3(0.014f, 0.01f, 0.008f), 6, 4, reg(R_EYES));
        mb_box(m, V3(s * 0.034f - 0.022f, 1.743f, 0.093f), V3(s * 0.034f + 0.022f, 1.75f, 0.104f), reg(R_HAIR));
    }
    if (female) mb_box(m, V3(-0.022f, 1.656f, 0.098f), V3(0.022f, 1.662f, 0.106f), reg(R_EYES));
}

static void arms_legs(MB *m, bool female)
{
    float sh = female ? 0.9f : 1.0f;
    for (int s = 0; s < 2; s++) {
        int clav = s ? B_CLAV_R : B_CLAV_L, arm = s ? B_ARM_R : B_ARM_L, fore = s ? B_FORE_R : B_FORE_L, hand = s ? B_HAND_R : B_HAND_L;
        float sx = s ? -1 : 1;
        Vector3 shp = add(bw[arm], V3(-sx * 0.02f * sh, 0, 0));
        (void)clav;
        limb(m, add(shp, V3(0, 0.03f, 0)), bw[fore], (Ring[]){ { 0, 0.06f * sh, 0.065f }, { 0.15f, 0.058f * sh, 0.06f }, { 0.6f, 0.048f * sh, 0.052f }, { 1, 0.042f * sh, 0.045f } }, 4, arm, B_CHEST, 0.2f, R_SHIRT);
        limb(m, bw[fore], add(bw[hand], V3(0, 0.02f, 0)), (Ring[]){ { 0, 0.042f * sh, 0.045f }, { 0.3f, 0.043f * sh, 0.044f }, { 1, 0.032f * sh, 0.03f } }, 3, fore, arm, 0.15f, female ? R_SKIN : R_SKIN);
        // hand: a flattened block with a thumb
        skin_to(hand, hand, 0);
        Vector3 h = bw[hand];
        mb_ellipsoid(m, add(h, V3(0, -0.06f, 0.005f)), V3(0.024f, 0.06f, 0.042f), 8, 6, reg(R_SKIN));
        mb_ellipsoid(m, add(h, V3(-sx * 0.0f, -0.035f, 0.042f)), V3(0.014f, 0.032f, 0.014f), 6, 4, reg(R_SKIN));
        // short sleeve cuff
        limb(m, add(bw[fore], V3(0, 0.1f, 0)), add(bw[fore], V3(0, 0.08f, 0)), (Ring[]){ { 0, 0.05f * sh, 0.054f }, { 1, 0.05f * sh, 0.054f } }, 2, arm, fore, 0.0f, R_SHIRT);
    }
    for (int s = 0; s < 2; s++) {
        int th = s ? B_THIGH_R : B_THIGH_L, ca = s ? B_CALF_R : B_CALF_L, ft = s ? B_FOOT_R : B_FOOT_L, ball = s ? B_BALL_R : B_BALL_L;
        float hw = female ? 1.04f : 1.0f;
        limb(m, add(bw[th], V3(0, 0.08f, 0)), bw[ca], (Ring[]){ { 0, 0.085f * hw, 0.09f }, { 0.2f, 0.082f * hw, 0.085f }, { 0.75f, 0.06f, 0.064f }, { 1, 0.054f, 0.058f } }, 4, th, B_PELVIS, 0.18f, R_PANTS);
        limb(m, bw[ca], add(bw[ft], V3(0, 0.02f, 0)), (Ring[]){ { 0, 0.054f, 0.058f }, { 0.25f, 0.056f, 0.062f }, { 0.85f, 0.042f, 0.045f }, { 1, 0.046f, 0.05f } }, 4, ca, th, 0.12f, R_PANTS);
        // shoe: heel block plus toe box skinned to the ball
        skin_to(ft, ft, 0);
        Vector3 f = bw[ft];
        mb_box(m, add(f, V3(-0.048f, -0.085f, -0.055f)), add(f, V3(0.048f, 0.01f, 0.06f)), reg(R_SHOES));
        skin_to(ball, ft, 0.3f);
        mb_box(m, add(f, V3(-0.046f, -0.085f, 0.06f)), add(f, V3(0.046f, -0.025f, 0.17f)), reg(R_SHOES));
    }
}

static Mesh body(bool female)
{
    MB m = { .skin = true };
    torso(&m, female);
    head(&m, female);
    arms_legs(&m, female);
    if (female) { // a little skirt flare at the hips, and sleeveless look via skin on the upper arm is skipped for clarity
        skin_to(B_PELVIS, B_PELVIS, 0);
    }
    return mb_upload(&m);
}

static Mesh hair(int kind)
{
    MB m = { .skin = true };
    skin_to(B_HEAD, B_HEAD, 0);
    RGBA h = reg(R_HAIR);
    Vector3 c = V3(0, 1.725f, 0.01f);
    switch (kind) {
    case HAIR_BUZZ:
        mb_ellipsoid(&m, add(c, V3(0, 0.012f, -0.008f)), V3(0.094f, 0.112f, 0.104f), 14, 8, h);
        break;
    case HAIR_PARTED:
        mb_ellipsoid(&m, add(c, V3(0, 0.025f, -0.01f)), V3(0.1f, 0.11f, 0.11f), 14, 8, h);
        mb_ellipsoid(&m, add(c, V3(0.035f, 0.085f, 0.04f)), V3(0.07f, 0.035f, 0.07f), 10, 6, h);
        break;
    case HAIR_LONG:
        mb_ellipsoid(&m, add(c, V3(0, 0.02f, -0.012f)), V3(0.104f, 0.118f, 0.112f), 14, 8, h);
        skin_to(B_HEAD, B_NECK, 0.4f);
        mb_box(&m, V3(-0.1f, 1.53f, -0.1f), V3(0.1f, 1.74f, -0.03f), h);
        for (int s = -1; s <= 1; s += 2) mb_box(&m, V3(s * 0.1f - 0.025f, 1.57f, -0.06f), V3(s * 0.1f + 0.025f, 1.74f, 0.04f), h);
        break;
    case HAIR_BUNS:
        mb_ellipsoid(&m, add(c, V3(0, 0.02f, -0.01f)), V3(0.1f, 0.114f, 0.108f), 14, 8, h);
        mb_ellipsoid(&m, add(c, V3(0, 0.1f, -0.085f)), V3(0.055f, 0.05f, 0.055f), 10, 6, h);
        break;
    case HAIR_BEARD:
        mb_ellipsoid(&m, add(c, V3(0, 0.012f, -0.008f)), V3(0.094f, 0.112f, 0.104f), 14, 8, h);
        mb_ellipsoid(&m, V3(0, 1.64f, 0.05f), V3(0.078f, 0.06f, 0.07f), 12, 6, h);
        break;
    case HAIR_CAP: {
        mb_ellipsoid(&m, add(c, V3(0, 0.03f, -0.01f)), V3(0.104f, 0.09f, 0.112f), 14, 8, h);
        mb_box(&m, V3(-0.085f, 1.775f, 0.08f), V3(0.085f, 1.79f, 0.2f), h);
        break;
    }
    }
    return mb_upload(&m);
}

// ---------------------------------------------------------------- guns and props

static Mesh gun(int w)
{
    MB m = { 0 };
    RGBA steel = rgba(0x2a2c30, 0), wood = rgba(0x6a4424, 0), dark = rgba(0x141416, 0);
    switch (w) {
    case W_PISTOL: // revolver
        mb_box(&m, V3(-0.016f, -0.1f, -0.04f), V3(0.016f, 0.0f, 0.0f), wood);
        mb_cyl(&m, V3(0, 0.03f, -0.01f), V3(0, 0.03f, 0.04f), 0.022f, 0.022f, 8, steel, steel, true);
        mb_cyl(&m, V3(0, 0.035f, 0.04f), V3(0, 0.035f, 0.2f), 0.009f, 0.009f, 8, steel, steel, true);
        mb_box(&m, V3(-0.012f, 0.0f, -0.05f), V3(0.012f, 0.05f, 0.04f), steel);
        break;
    case W_RIFLE:
        mb_box(&m, V3(-0.02f, -0.06f, -0.34f), V3(0.02f, 0.03f, -0.02f), wood);
        mb_box(&m, V3(-0.018f, -0.04f, -0.02f), V3(0.018f, 0.03f, 0.3f), wood);
        mb_cyl(&m, V3(0, 0.04f, -0.05f), V3(0, 0.04f, 0.62f), 0.011f, 0.009f, 8, steel, steel, true);
        mb_cyl(&m, V3(0, 0.09f, -0.06f), V3(0, 0.09f, 0.2f), 0.018f, 0.018f, 8, dark, dark, true);
        mb_box(&m, V3(-0.006f, 0.05f, 0.0f), V3(0.006f, 0.08f, 0.02f), dark);
        mb_box(&m, V3(-0.006f, 0.05f, 0.14f), V3(0.006f, 0.08f, 0.16f), dark);
        break;
    case W_SHOTGUN:
        mb_box(&m, V3(-0.022f, -0.07f, -0.36f), V3(0.022f, 0.03f, -0.03f), wood);
        mb_box(&m, V3(-0.02f, -0.02f, -0.04f), V3(0.02f, 0.035f, 0.1f), steel);
        mb_cyl(&m, V3(0.011f, 0.035f, 0.08f), V3(0.011f, 0.035f, 0.6f), 0.012f, 0.012f, 8, steel, steel, true);
        mb_cyl(&m, V3(-0.011f, 0.035f, 0.08f), V3(-0.011f, 0.035f, 0.6f), 0.012f, 0.012f, 8, steel, steel, true);
        mb_box(&m, V3(-0.024f, 0.0f, 0.18f), V3(0.024f, 0.03f, 0.34f), wood);
        break;
    case W_BAT: // a pipe with tape: melee
        mb_cyl(&m, V3(0, 0, -0.08f), V3(0, 0, 0.62f), 0.018f, 0.022f, 8, rgba(0x7a7c80, 0), steel, true);
        mb_cyl(&m, V3(0, 0, -0.08f), V3(0, 0, 0.12f), 0.022f, 0.022f, 8, rgba(0x202020, 0), dark, true);
        break;
    }
    return mb_upload(&m);
}

static Mesh prop(int k)
{
    MB m = { 0 };
    switch (k) {
    case PROP_CRATE: {
        RGBA a = rgba(0x8a6a40, 0), b = rgba(0x6a4e2c, 0);
        mb_box(&m, V3(-0.4f, 0, -0.3f), V3(0.4f, 0.5f, 0.3f), a);
        for (int s = -1; s <= 1; s += 2) {
            mb_box(&m, V3(-0.42f, 0.0f, s * 0.3f - 0.02f), V3(0.42f, 0.06f, s * 0.3f + 0.02f), b);
            mb_box(&m, V3(-0.42f, 0.44f, s * 0.3f - 0.02f), V3(0.42f, 0.5f, s * 0.3f + 0.02f), b);
        }
        break;
    }
    case PROP_BAG:
        mb_ellipsoid(&m, V3(0, 0.15f, 0), V3(0.22f, 0.15f, 0.16f), 10, 6, rgba(0x4a5a3a, 0));
        mb_cyl(&m, V3(-0.1f, 0.28f, 0), V3(0.1f, 0.28f, 0), 0.02f, 0.02f, 6, rgba(0x2a2a2a, 0), rgba(0x2a2a2a, 0), true);
        break;
    case PROP_JERRY:
        mb_box(&m, V3(-0.09f, 0, -0.17f), V3(0.09f, 0.42f, 0.17f), rgba(0xb02020, 0));
        mb_box(&m, V3(-0.04f, 0.42f, -0.1f), V3(0.04f, 0.47f, 0.06f), rgba(0x801818, 0));
        mb_cyl(&m, V3(0, 0.42f, 0.11f), V3(0, 0.47f, 0.13f), 0.025f, 0.025f, 8, rgba(0x202020, 0), rgba(0x202020, 0), true);
        break;
    case PROP_FIRE: { // stones and logs
        for (int i = 0; i < 9; i++) {
            float a = i * 2 * PI / 9;
            mb_ellipsoid(&m, V3(cosf(a) * 0.45f, 0.05f, sinf(a) * 0.45f), V3(0.11f, 0.08f, 0.1f), 6, 4, rgba(0x6a6660, 0));
        }
        for (int i = 0; i < 4; i++) {
            float a = i * PI / 2 + 0.3f;
            mb_cyl(&m, V3(cosf(a) * 0.35f, 0.05f, sinf(a) * 0.35f), V3(cosf(a) * -0.05f, 0.25f, sinf(a) * -0.05f), 0.05f, 0.04f, 6, rgba(0x4a3220, 0), rgba(0x8a6a48, 0), true);
        }
        break;
    }
    case PROP_TENT: {
        RGBA c = rgba(0x3a6a8a, 0), d = rgba(0x2a4a60, 0);
        Vector3 in = V3(0, 0.5f, 0);
        mb_quad(&m, V3(-1.1f, 0, -1.3f), V3(-1.1f, 0, 1.3f), V3(0, 1.3f, 1.3f), V3(0, 1.3f, -1.3f), c, in);
        mb_quad(&m, V3(1.1f, 0, -1.3f), V3(1.1f, 0, 1.3f), V3(0, 1.3f, 1.3f), V3(0, 1.3f, -1.3f), c, in);
        mb_tri_flat(&m, V3(-1.1f, 0, -1.3f), V3(1.1f, 0, -1.3f), V3(0, 1.3f, -1.3f), d, in);
        mb_tri_flat(&m, V3(-1.1f, 0, 1.3f), V3(-0.15f, 0, 1.3f), V3(0, 1.3f, 1.3f), d, in);
        mb_tri_flat(&m, V3(1.1f, 0, 1.3f), V3(0.15f, 0, 1.3f), V3(0, 1.3f, 1.3f), d, in);
        break;
    }
    case PROP_DEER: { // a simple quadruped, built standing on y = 0 facing +z
        RGBA f = rgba(0x8a6040, 0), l = rgba(0xd8c8b0, 0), dk = rgba(0x2a1c14, 0);
        mb_ellipsoid(&m, V3(0, 0.95f, 0), V3(0.2f, 0.24f, 0.55f), 12, 8, f);
        mb_ellipsoid(&m, V3(0, 1.25f, 0.55f), V3(0.08f, 0.22f, 0.1f), 8, 6, f);
        mb_ellipsoid(&m, V3(0, 1.45f, 0.68f), V3(0.08f, 0.08f, 0.16f), 8, 6, f);
        mb_ellipsoid(&m, V3(0, 1.42f, 0.83f), V3(0.03f, 0.03f, 0.03f), 6, 4, dk);
        for (int s = -1; s <= 1; s += 2) {
            mb_ellipsoid(&m, V3(s * 0.07f, 1.54f, 0.62f), V3(0.05f, 0.08f, 0.02f), 6, 4, f);
            mb_cyl(&m, V3(s * 0.04f, 1.52f, 0.66f), V3(s * 0.16f, 1.8f, 0.6f), 0.012f, 0.008f, 5, dk, dk, true);
            mb_cyl(&m, V3(s * 0.12f, 1.68f, 0.62f), V3(s * 0.05f, 1.84f, 0.72f), 0.008f, 0.006f, 5, dk, dk, true);
            for (int e = -1; e <= 1; e += 2)
                mb_cyl(&m, V3(s * 0.11f, 0.85f, e * 0.38f), V3(s * 0.11f, 0.0f, e * 0.38f + 0.02f), 0.045f, 0.022f, 6, f, dk, true);
        }
        mb_ellipsoid(&m, V3(0, 1.05f, -0.55f), V3(0.06f, 0.08f, 0.05f), 6, 4, l);
        break;
    }
    case PROP_WOLF: {
        RGBA f = rgba(0x6a6460, 0), l = rgba(0xb0a898, 0), dk = rgba(0x1c1a18, 0);
        mb_ellipsoid(&m, V3(0, 0.62f, 0), V3(0.16f, 0.18f, 0.48f), 12, 8, f);
        mb_ellipsoid(&m, V3(0, 0.72f, 0.42f), V3(0.12f, 0.14f, 0.12f), 8, 6, f);
        mb_ellipsoid(&m, V3(0, 0.74f, 0.58f), V3(0.08f, 0.08f, 0.13f), 8, 6, f);
        mb_ellipsoid(&m, V3(0, 0.7f, 0.7f), V3(0.035f, 0.035f, 0.05f), 6, 4, dk);
        mb_ellipsoid(&m, V3(0, 0.6f, 0.5f), V3(0.09f, 0.06f, 0.1f), 8, 4, l);
        for (int s = -1; s <= 1; s += 2) {
            mb_cyl(&m, V3(s * 0.05f, 0.82f, 0.52f), V3(s * 0.07f, 0.95f, 0.5f), 0.035f, 0.005f, 4, f, f, true);
            for (int e = -1; e <= 1; e += 2)
                mb_cyl(&m, V3(s * 0.09f, 0.55f, e * 0.32f), V3(s * 0.09f, 0.0f, e * 0.32f + 0.03f), 0.04f, 0.025f, 6, f, dk, true);
        }
        mb_cyl(&m, V3(0, 0.66f, -0.45f), V3(0, 0.42f, -0.78f), 0.06f, 0.03f, 6, f, l, true);
        break;
    }
    case PROP_CORPSE: // a covered body: a tarp mound
        mb_ellipsoid(&m, V3(0, 0.1f, 0), V3(0.3f, 0.13f, 0.95f), 10, 6, rgba(0x3a4a3a, 0));
        break;
    case PROP_BARREL:
        mb_cyl(&m, V3(0, 0, 0), V3(0, 0.88f, 0), 0.29f, 0.29f, 14, rgba(0x2a5a7a, 0), rgba(0x244a62, 0), true);
        for (int k = 0; k < 2; k++) mb_cyl(&m, V3(0, 0.25f + k * 0.4f, 0), V3(0, 0.29f + k * 0.4f, 0), 0.3f, 0.3f, 14, rgba(0x1a3a52, 0), rgba(0x1a3a52, 0), false);
        break;
    case PROP_PUMP: {
        RGBA r = rgba(0xc02a20, 0), w = rgba(0xe8e4dc, 0), dk = rgba(0x202020, 0);
        mb_box(&m, V3(-0.35f, 0, -0.25f), V3(0.35f, 1.6f, 0.25f), r);
        mb_box(&m, V3(-0.3f, 1.0f, 0.25f), V3(0.3f, 1.45f, 0.27f), w);
        mb_box(&m, V3(-0.2f, 1.1f, 0.27f), V3(0.2f, 1.3f, 0.28f), dk);
        mb_box(&m, V3(-0.38f, 1.6f, -0.28f), V3(0.38f, 1.75f, 0.28f), w);
        mb_box(&m, V3(0.35f, 0.7f, -0.05f), V3(0.42f, 0.95f, 0.05f), dk);
        break;
    }
    case PROP_SIGN: { // a mileage sign on two posts
        RGBA g = rgba(0x1e6a3a, 0), p = rgba(0x9aa0a6, 0);
        mb_box(&m, V3(-1.4f, 1.6f, -0.03f), V3(1.4f, 2.6f, 0.03f), g);
        mb_box(&m, V3(-1.3f, 1.68f, 0.03f), V3(1.3f, 1.7f, 0.035f), rgba(0xf0f0f0, 0));
        mb_box(&m, V3(-1.3f, 2.5f, 0.03f), V3(1.3f, 2.52f, 0.035f), rgba(0xf0f0f0, 0));
        for (int s = -1; s <= 1; s += 2) mb_box(&m, V3(s * 1.0f - 0.04f, 0, -0.04f), V3(s * 1.0f + 0.04f, 1.6f, 0.04f), p);
        break;
    }
    }
    return mb_upload(&m);
}

// ---------------------------------------------------------------- animation

typedef struct { Vector3 r[B_N]; Vector3 hips; } Pose;   // euler xyz per bone, hips position

static void pose_zero(Pose *p) { memset(p, 0, sizeof *p); p->hips = bw[B_PELVIS]; }

static Quaternion eul(Vector3 e)
{
    Quaternion qx = QuaternionFromAxisAngle(V3(1, 0, 0), e.x), qy = QuaternionFromAxisAngle(V3(0, 1, 0), e.y);
    Quaternion qz = QuaternionFromAxisAngle(V3(0, 0, 1), e.z);
    return QuaternionMultiply(qy, QuaternionMultiply(qx, qz));
}

typedef void (*PoseFn)(float t, Pose *p);   // t in 0..1 over the clip

static void make_clip(int id, int frames, bool loop, PoseFn fn)
{
    Clip *c = &clips[id];
    c->frames = frames;
    c->fps = 30;
    c->loop = loop;
    c->rot = MemAlloc(frames * B_N * sizeof(Quaternion));
    c->hips = MemAlloc(frames * sizeof(Vector3));
    for (int f = 0; f < frames; f++) {
        Pose p;
        pose_zero(&p);
        fn(frames > 1 ? (float)f / (frames - 1) : 0, &p);
        for (int b = 0; b < B_N; b++) c->rot[f * B_N + b] = eul(p.r[b]);
        c->hips[f] = p.hips;
    }
}

#define TAU_ (2 * PI)
static float sn(float x) { return sinf(TAU_ * x); }
static float cs(float x) { return cosf(TAU_ * x); }
static float sm(float a, float b, float x) { float t = Clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static float bump(float a, float b, float x) { return sm(a, (a + b) * 0.5f, x) * (1 - sm((a + b) * 0.5f, b, x)); }

// arms resting down, a touch out from the body
static void arms_rest(Pose *p, float out)
{
    p->r[B_ARM_L].z = out; p->r[B_ARM_R].z = -out;
    p->r[B_FORE_L].x = -0.15f; p->r[B_FORE_R].x = -0.15f;
}

// leg with gait phase u (0..1): thigh swing amplitude a, knee lift k
static void leg(Pose *p, int side, float u, float a, float k, float stance_k)
{
    int th = side ? B_THIGH_R : B_THIGH_L, ca = side ? B_CALF_R : B_CALF_L, ft = side ? B_FOOT_R : B_FOOT_L;
    float thigh = -a * cs(u);                       // u = 0: leg forward
    float swing = fmaxf(0, -sn(u));                 // u in 0.5..1: swinging forward
    float knee = k * powf(swing, 1.2f) + stance_k * bump(0.0f, 0.3f, u);
    p->r[th].x = thigh - knee * 0.25f;
    p->r[ca].x = knee;
    p->r[ft].x = -(thigh - knee * 0.25f) - knee * 0.8f + 0.25f * bump(0.3f, 0.55f, u);
}

static void gait(float t, Pose *p, float a, float k, float arm, float bob, float lean, float stance_k)
{
    leg(p, 0, t, a, k, stance_k);
    leg(p, 1, t + 0.5f, a, k, stance_k);
    arms_rest(p, 0.08f);
    p->r[B_ARM_L].x = arm * cs(t + 0.5f) * -1;
    p->r[B_ARM_R].x = arm * cs(t) * -1;
    p->r[B_FORE_L].x = -0.25f - arm * 0.6f * (0.5f + 0.5f * cs(t + 0.5f));
    p->r[B_FORE_R].x = -0.25f - arm * 0.6f * (0.5f + 0.5f * cs(t));
    p->r[B_PELVIS].y = 0.12f * a * cs(t);
    p->r[B_CHEST].y = -0.16f * a * cs(t);
    p->r[B_SP1].x = lean * 0.5f; p->r[B_SP2].x = lean * 0.3f; p->r[B_CHEST].x = lean * 0.2f;
    p->r[B_NECK].x = -lean * 0.6f;
    p->r[B_PELVIS].z = 0.04f * a * sn(t);
    p->hips.y = bw[B_PELVIS].y - bob * (0.5f + 0.5f * cs(2 * t)) - bob * 0.3f;
}

static void c_idle(float t, Pose *p)
{
    arms_rest(p, 0.07f);
    float b = sn(t);
    p->r[B_CHEST].x = 0.02f * b; p->r[B_NECK].x = -0.02f * b;
    p->r[B_ARM_L].x = 0.03f * b; p->r[B_ARM_R].x = 0.03f * b;
    p->r[B_HEAD].y = 0.15f * sn(t * 0.5f + 0.2f) * bump(0.2f, 0.9f, t);
    p->r[B_THIGH_L].z = 0.04f; p->r[B_THIGH_R].z = -0.02f; p->r[B_FOOT_L].z = -0.04f;
    p->hips.y -= 0.005f + 0.004f * b;
}
static void c_walk(float t, Pose *p) { gait(t, p, 0.38f, 0.9f, 0.32f, 0.02f, 0.03f, 0.15f); }
static void c_jog(float t, Pose *p)
{
    gait(t, p, 0.55f, 1.5f, 0.55f, 0.04f, 0.12f, 0.35f);
    p->r[B_FORE_L].x = -1.3f; p->r[B_FORE_R].x = -1.3f;
}
static void c_sprint(float t, Pose *p)
{
    gait(t, p, 0.75f, 1.9f, 0.85f, 0.05f, 0.25f, 0.4f);
    p->r[B_FORE_L].x = -1.5f; p->r[B_FORE_R].x = -1.5f;
}
static void c_crouch_walk(float t, Pose *p)
{
    gait(t, p, 0.35f, 0.6f, 0.15f, 0.015f, 0.45f, 0.1f);
    p->r[B_THIGH_L].x -= 0.9f; p->r[B_THIGH_R].x -= 0.9f;
    p->r[B_CALF_L].x += 1.3f; p->r[B_CALF_R].x += 1.3f;
    p->r[B_FOOT_L].x -= 0.4f; p->r[B_FOOT_R].x -= 0.4f;
    p->hips.y -= 0.3f;
    p->r[B_ARM_L].x = -0.4f; p->r[B_ARM_R].x = -0.4f; p->r[B_FORE_L].x = -0.9f; p->r[B_FORE_R].x = -0.9f;
}
static void squat(Pose *p, float k)
{
    p->r[B_THIGH_L].x = -1.1f * k; p->r[B_THIGH_R].x = -1.1f * k;
    p->r[B_CALF_L].x = 1.9f * k; p->r[B_CALF_R].x = 1.9f * k;
    p->r[B_FOOT_L].x = -0.8f * k; p->r[B_FOOT_R].x = -0.8f * k;
    p->r[B_SP1].x = 0.45f * k;
    p->hips.y = bw[B_PELVIS].y - 0.42f * k;
}
static void c_jump_start(float t, Pose *p)
{
    float k = bump(0, 1.4f, t);
    squat(p, k * 0.6f);
    arms_rest(p, 0.1f);
    p->r[B_ARM_L].x = 0.6f * k - 0.8f * sm(0.5f, 1, t); p->r[B_ARM_R].x = p->r[B_ARM_L].x;
}
static void c_jump_loop(float t, Pose *p)
{
    arms_rest(p, 0.5f);
    p->r[B_ARM_L].x = -0.7f; p->r[B_ARM_R].x = -0.6f;
    p->r[B_THIGH_L].x = -0.9f; p->r[B_CALF_L].x = 1.2f; p->r[B_THIGH_R].x = -0.2f; p->r[B_CALF_R].x = 0.6f;
    p->r[B_SP1].x = 0.1f + 0.03f * sn(t);
}
static void c_jump_land(float t, Pose *p)
{
    float k = 1 - sm(0.15f, 1, t);
    squat(p, k * 0.5f);
    arms_rest(p, 0.2f + 0.3f * k);
    p->r[B_ARM_L].x = -0.5f * k; p->r[B_ARM_R].x = -0.5f * k;
}
static void c_death(float t, Pose *p)
{
    float k = sm(0, 0.6f, t);
    squat(p, 0.7f * k);
    p->r[B_SP1].x = 0.8f * k; p->r[B_HEAD].x = 0.5f * k;
    arms_rest(p, 0.3f * k);
    p->hips.y = bw[B_PELVIS].y - 0.75f * k;
}
static void drive_pose(Pose *p, float steer)
{
    p->hips.y = 0.73f;
    p->r[B_THIGH_L].x = -1.5f; p->r[B_THIGH_R].x = -1.5f; p->r[B_THIGH_L].z = 0.08f; p->r[B_THIGH_R].z = -0.08f;
    p->r[B_CALF_L].x = 1.35f; p->r[B_CALF_R].x = 1.25f;
    p->r[B_FOOT_L].x = 0.1f; p->r[B_FOOT_R].x = 0.15f;
    p->r[B_SP1].x = -0.12f;
    p->r[B_ARM_L].x = -1.05f + steer * 0.2f; p->r[B_ARM_R].x = -1.05f - steer * 0.2f;
    p->r[B_ARM_L].z = 0.2f; p->r[B_ARM_R].z = -0.2f;
    p->r[B_FORE_L].x = -0.55f; p->r[B_FORE_R].x = -0.55f;
    p->r[B_FORE_L].z = -0.35f; p->r[B_FORE_R].z = 0.35f;
}
static void c_drive(float t, Pose *p) { drive_pose(p, 0.15f * sn(t)); p->r[B_HEAD].y = 0.08f * sn(t * 2); }
static void c_hit_chest(float t, Pose *p)
{
    float k = bump(0, 1, t);
    arms_rest(p, 0.15f + 0.3f * k);
    p->r[B_SP1].x = -0.25f * k; p->r[B_CHEST].x = -0.3f * k; p->r[B_HEAD].x = 0.3f * k;
    p->r[B_ARM_L].x = 0.3f * k; p->r[B_ARM_R].x = 0.3f * k;
}
static void c_hit_head(float t, Pose *p)
{
    float k = bump(0, 1, t);
    arms_rest(p, 0.1f);
    p->r[B_NECK].x = -0.5f * k; p->r[B_HEAD].y = 0.5f * k; p->r[B_CHEST].x = -0.15f * k;
    p->r[B_ARM_R].x = -1.2f * k; p->r[B_FORE_R].x = -1.6f * k;
}
// gun poses: right arm carries the gun forward, the left supports it
static void gun_arms(Pose *p, float up, float k)
{
    p->r[B_ARM_R].x = -0.6f - 0.95f * up; p->r[B_ARM_R].z = 0.12f * up; p->r[B_ARM_R].y = 0.0f;
    p->r[B_FORE_R].x = -0.75f + 0.65f * up;
    p->r[B_HAND_R].x = 0.3f - 0.3f * up;
    p->r[B_ARM_L].x = -0.5f - 0.95f * up * k; p->r[B_ARM_L].z = -0.35f * up * k; p->r[B_ARM_L].y = 0.3f * up * k;
    p->r[B_FORE_L].x = -0.9f + 0.3f * up * k; p->r[B_FORE_L].z = -0.3f * k;
    p->r[B_CHEST].y = 0.15f * up; p->r[B_NECK].y = -0.12f * up;
}
static void c_pistol_idle(float t, Pose *p) { arms_rest(p, 0.08f); gun_arms(p, 0.0f, 0.6f); p->r[B_CHEST].x = 0.02f * sn(t); }
static void c_pistol_aim(float t, Pose *p) { arms_rest(p, 0.0f); gun_arms(p, 1.0f, 1.0f); p->r[B_CHEST].x = 0.01f * sn(t); }
static void c_pistol_shoot(float t, Pose *p)
{
    gun_arms(p, 1.0f, 1.0f);
    float k = bump(0, 0.8f, t);
    p->r[B_ARM_R].x -= 0.25f * k; p->r[B_FORE_R].x -= 0.3f * k; p->r[B_HAND_R].x -= 0.3f * k; p->r[B_CHEST].x = -0.06f * k;
}
static void c_pistol_reload(float t, Pose *p)
{
    gun_arms(p, 1 - bump(0.05f, 0.95f, t), 1);
    float k = bump(0.05f, 0.95f, t);
    p->r[B_ARM_R].x = Lerp(p->r[B_ARM_R].x, -0.35f, k); p->r[B_FORE_R].x = Lerp(p->r[B_FORE_R].x, -1.5f, k);
    p->r[B_ARM_L].x = Lerp(p->r[B_ARM_L].x, -0.2f - 0.4f * bump(0.3f, 0.7f, t), k);
    p->r[B_FORE_L].x = Lerp(p->r[B_FORE_L].x, -1.6f, k);
    p->r[B_HEAD].x = 0.35f * k;
}
static void c_punch_jab(float t, Pose *p)
{
    arms_rest(p, 0.1f);
    float k = bump(0.1f, 0.6f, t), g = 1 - bump(0.6f, 1.6f, t) * 0;
    p->r[B_ARM_L].x = -0.9f - 0.6f * k; p->r[B_FORE_L].x = -1.6f + 1.5f * k;
    p->r[B_ARM_R].x = -0.7f * g; p->r[B_FORE_R].x = -1.9f * g;
    p->r[B_CHEST].y = -0.3f * k; p->r[B_PELVIS].y = -0.1f * k;
    p->r[B_THIGH_L].x = -0.2f; p->r[B_CALF_L].x = 0.25f;
}
static void c_punch_cross(float t, Pose *p)
{
    arms_rest(p, 0.1f);
    float k = bump(0.15f, 0.65f, t);
    p->r[B_ARM_R].x = -0.9f - 0.65f * k; p->r[B_FORE_R].x = -1.6f + 1.5f * k;
    p->r[B_ARM_L].x = -0.7f; p->r[B_FORE_L].x = -1.9f;
    p->r[B_CHEST].y = 0.4f * k; p->r[B_PELVIS].y = 0.15f * k;
    p->r[B_THIGH_R].x = 0.2f; p->r[B_CALF_R].x = 0.2f;
}
static void c_cower(float t, Pose *p)
{
    squat(p, 0.85f);
    p->r[B_SP1].x = 0.6f; p->r[B_CHEST].x = 0.3f; p->r[B_HEAD].x = 0.4f;
    p->r[B_ARM_L].x = -2.4f; p->r[B_ARM_R].x = -2.4f; p->r[B_ARM_L].z = -0.3f; p->r[B_ARM_R].z = 0.3f;
    p->r[B_FORE_L].x = -1.9f; p->r[B_FORE_R].x = -1.9f;
    p->hips.y += 0.01f * sn(t * 3);
}
static void c_roll(float t, Pose *p)
{
    float k = bump(0, 1, t);
    squat(p, 0.9f * k);
    p->r[B_PELVIS].x = TAU_ * sm(0.1f, 0.85f, t);
    p->r[B_SP1].x = 0.6f * k; p->r[B_HEAD].x = 0.5f * k;
    p->r[B_ARM_L].x = -1.2f * k; p->r[B_ARM_R].x = -1.2f * k; p->r[B_FORE_L].x = -1.5f * k; p->r[B_FORE_R].x = -1.5f * k;
    p->hips.y = bw[B_PELVIS].y - 0.55f * k;
}
static void c_interact(float t, Pose *p)
{
    float k = bump(0, 1, t);
    arms_rest(p, 0.1f);
    p->r[B_SP1].x = 0.5f * k; p->r[B_SP2].x = 0.3f * k; p->r[B_NECK].x = 0.2f * k;
    p->r[B_THIGH_L].x = -0.35f * k; p->r[B_CALF_L].x = 0.6f * k; p->r[B_THIGH_R].x = -0.2f * k; p->r[B_CALF_R].x = 0.45f * k;
    p->r[B_FOOT_L].x = -0.25f * k; p->r[B_FOOT_R].x = -0.25f * k;
    p->hips.y -= 0.1f * k;
    float rum = 0.25f * sn(t * 5) * k;
    p->r[B_ARM_R].x = -1.0f * k + rum; p->r[B_FORE_R].x = -0.6f * k;
    p->r[B_ARM_L].x = -0.9f * k - rum; p->r[B_FORE_L].x = -0.7f * k;
}
static void c_talk(float t, Pose *p)
{
    arms_rest(p, 0.1f);
    p->r[B_ARM_R].x = -0.5f - 0.2f * sn(t * 2); p->r[B_FORE_R].x = -1.2f - 0.3f * sn(t * 3 + 0.2f);
    p->r[B_ARM_L].x = -0.2f * bump(0.3f, 0.8f, t); p->r[B_FORE_L].x = -0.6f * bump(0.3f, 0.8f, t) - 0.15f;
    p->r[B_HEAD].x = 0.06f * sn(t * 4); p->r[B_HEAD].y = 0.15f * sn(t);
    p->r[B_THIGH_L].z = 0.05f;
}

static void build_clips(void)
{
    // walk 1.2 m/s, jog 3.0, sprint 4.4, crouch 1.0: the cycle lengths match those speeds
    make_clip(A_IDLE, 91, true, c_idle);
    make_clip(A_WALK, 34, true, c_walk);
    make_clip(A_JOG, 21, true, c_jog);
    make_clip(A_SPRINT, 19, true, c_sprint);
    make_clip(A_JUMP_START, 9, false, c_jump_start);
    make_clip(A_JUMP_LOOP, 19, true, c_jump_loop);
    make_clip(A_JUMP_LAND, 13, false, c_jump_land);
    make_clip(A_DEATH, 37, false, c_death);
    make_clip(A_DRIVE, 61, true, c_drive);
    make_clip(A_HIT_CHEST, 13, false, c_hit_chest);
    make_clip(A_HIT_HEAD, 13, false, c_hit_head);
    make_clip(A_PISTOL_IDLE, 61, true, c_pistol_idle);
    make_clip(A_PISTOL_AIM, 61, true, c_pistol_aim);
    make_clip(A_PISTOL_SHOOT, 7, false, c_pistol_shoot);
    make_clip(A_PISTOL_RELOAD, 37, false, c_pistol_reload);
    make_clip(A_PUNCH_JAB, 13, false, c_punch_jab);
    make_clip(A_PUNCH_CROSS, 15, false, c_punch_cross);
    make_clip(A_COWER, 61, true, c_cower);
    make_clip(A_ROLL, 25, false, c_roll);
    make_clip(A_INTERACT, 31, false, c_interact);
    make_clip(A_TALK, 91, true, c_talk);
    make_clip(A_CROUCH_WALK, 28, true, c_crouch_walk);
}

void assets_load(void)
{
    build_skeleton();
    build_cars();
    body_mesh[BODY_MALE] = body(false);
    body_mesh[BODY_FEMALE] = body(true);
    for (int i = 0; i < HAIR_COUNT; i++) hair_mesh[i] = hair(i);
    for (int i = W_PISTOL; i < W_COUNT; i++) gun_mesh[i] = gun(i);
    for (int i = 0; i < PROP_COUNT; i++) prop_mesh[i] = prop(i);
    build_clips();
}

// ---------------------------------------------------------------- sampling (as upstream)

void pose_sample(const Clip *c, float t, Quaternion *rot, Vector3 *hips)
{
    int nb = skel.count, last = c->frames - 1;
    float ft = t * c->fps, span = (float)last;
    ft = c->loop ? fmodf(fmodf(ft, span) + span, span) : Clamp(ft, 0, span);
    int f0 = (int)ft, f1 = f0 < last ? f0 + 1 : (c->loop ? 0 : f0);
    float k = ft - f0;
    const Quaternion *a = c->rot + f0 * nb, *b = c->rot + f1 * nb;
    for (int i = 0; i < nb; i++) {
        Quaternion q = b[i];
        if (a[i].x * q.x + a[i].y * q.y + a[i].z * q.z + a[i].w * q.w < 0) q = (Quaternion){ -q.x, -q.y, -q.z, -q.w };
        rot[i] = QuaternionNormalize(QuaternionLerp(a[i], q, k));
    }
    *hips = Vector3Lerp(c->hips[f0], c->hips[f1], k);
}

// raylib multiplies left-to-right: MatrixMultiply(a, b) applies a first, then b
void pose_matrices(const Quaternion *rot, Vector3 hips, Matrix *out)
{
    Matrix world[MAX_BONES];
    for (int i = 0; i < skel.count; i++) {
        Vector3 p = i == skel.hips ? hips : skel.bind_pos[i];
        Matrix local = MatrixMultiply(QuaternionToMatrix(rot[i]), MatrixTranslate(p.x, p.y, p.z));
        world[i] = skel.parent[i] < 0 ? local : MatrixMultiply(local, world[skel.parent[i]]);
        out[i] = MatrixMultiply(skel.inv_bind[i], world[i]);
    }
}
