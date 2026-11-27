#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

extern const unsigned char assets_blob[];
extern const int assets_blob_len;

CarModel car_models[CAR_MODELS];
Mesh body_mesh[BODY_COUNT];
Mesh hair_mesh[HAIR_COUNT];
Mesh gun_mesh[W_COUNT];
Mesh heli_mesh[2];
Skeleton skel;
Clip clips[A_COUNT];

typedef struct { const unsigned char *p; } Rd;

static int rd8(Rd *r) { return *r->p++; }
static int rd16(Rd *r) { int v = r->p[0] | r->p[1] << 8; r->p += 2; return v; }
static float rdf(Rd *r) { float f; memcpy(&f, r->p, 4); r->p += 4; return f; }

// n values stored as delta-coded low bytes then high bytes, restarting every `cols`
static void rd_deltas(Rd *r, int n, int cols, uint16_t *out)
{
    const unsigned char *lo = r->p, *hi = r->p + n;
    uint16_t acc = 0;
    for (int i = 0; i < n; i++) {
        if (i % cols == 0) acc = 0;
        acc += lo[i] | hi[i] << 8;
        out[i] = acc;
    }
    r->p += 2 * n;
}

static Mesh rd_mesh(Rd *r)
{
    Mesh m = { 0 };
    int n = rd16(r), nt = rd16(r), skinned = rd8(r);
    float off[3], sc[3];
    for (int i = 0; i < 3; i++) off[i] = rdf(r);
    for (int i = 0; i < 3; i++) sc[i] = rdf(r);
    m.vertexCount = n;
    m.triangleCount = nt;
    m.vertices = MemAlloc(n * 3 * sizeof(float));
    m.normals = MemAlloc(n * 3 * sizeof(float));
    m.colors = MemAlloc(n * 4);
    m.indices = MemAlloc(nt * 3 * sizeof(unsigned short));

    // positions: ref[i] = distance back to the vertex that has the same spot, 0 = a new spot (stored once)
    const unsigned char *rl = r->p, *rh = r->p + n;
    int *ref = MemAlloc(n * sizeof(int)), nspots = 0;
    for (int i = 0; i < n; i++) nspots += !(ref[i] = rl[i] | rh[i] << 8);
    r->p += 2 * n;
    uint16_t *q = MemAlloc(nspots * sizeof(uint16_t));
    for (int a = 0; a < 3; a++) {
        rd_deltas(r, nspots, nspots, q);
        for (int i = 0, u = 0; i < n; i++) {
            float *v = m.vertices + i * 3 + a;
            *v = ref[i] ? v[-ref[i] * 3] : off[a] + q[u++] * sc[a];
        }
    }
    MemFree(q);
    MemFree(ref);
    for (int a = 0; a < 3; a++)
        for (int i = 0; i < n; i++) m.normals[i * 3 + a] = (signed char)*r->p++ / 127.0f;

    if (!skinned) {
        for (int c = 0; c < 4; c++)
            for (int i = 0; i < n; i++) m.colors[i * 4 + c] = *r->p++;
    } else {
        for (int i = 0; i < n; i++) m.colors[i * 4] = *r->p++;
        for (int i = 0; i < n; i++) { m.colors[i * 4 + 1] = *r->p++; m.colors[i * 4 + 2] = 255; m.colors[i * 4 + 3] = 255; }
        m.boneIndices = MemAlloc(n * 4);
        m.boneWeights = MemAlloc(n * 4 * sizeof(float));
        m.boneCount = skel.count;
        for (int c = 0; c < 4; c++)
            for (int i = 0; i < n; i++) m.boneIndices[i * 4 + c] = *r->p++;
        for (int c = 0; c < 4; c++)
            for (int i = 0; i < n; i++) m.boneWeights[i * 4 + c] = *r->p++ / 255.0f;
    }

    // index codes: distance back from the highest vertex used so far (0 = next new vertex)
    int ni = nt * 3, hw = -1;
    const unsigned char *lo = r->p, *hi = r->p + ni;
    for (int i = 0; i < ni; i++) {
        int idx = hw + 1 - (lo[i] | hi[i] << 8);
        if (idx > hw) hw = idx;
        m.indices[i] = idx;
    }
    r->p += 2 * ni;
    UploadMesh(&m, false);
    return m;
}

static void rd_skeleton(Rd *r)
{
    skel.count = rd8(r);
    for (int i = 0; i < skel.count; i++) skel.parent[i] = (signed char)rd8(r);
    for (int i = 0; i < skel.count; i++) skel.bind_pos[i] = (Vector3){ rdf(r), rdf(r), rdf(r) };
    for (int i = 0; i < skel.count; i++) skel.bind_rot[i] = (Quaternion){ rdf(r), rdf(r), rdf(r), rdf(r) };
    skel.head = rd8(r); skel.hand_r = rd8(r); skel.hand_l = rd8(r); skel.hips = rd8(r); skel.spine = rd8(r);

    Matrix world[MAX_BONES];
    for (int i = 0; i < skel.count; i++) {
        Vector3 p = skel.bind_pos[i];
        Matrix local = MatrixMultiply(QuaternionToMatrix(skel.bind_rot[i]), MatrixTranslate(p.x, p.y, p.z));
        world[i] = skel.parent[i] < 0 ? local : MatrixMultiply(local, world[skel.parent[i]]);
        skel.inv_bind[i] = MatrixInvert(world[i]);
    }
}

static void rd_clip(Rd *r, Clip *c)
{
    int nb = skel.count;
    c->frames = rd16(r);
    c->fps = 30;
    c->loop = rd8(r);
    int nq = nb * 3 * c->frames, nf = c->frames;
    uint16_t *q = MemAlloc(nq * sizeof(uint16_t)), *h = MemAlloc(nf * 3 * sizeof(uint16_t));
    rd_deltas(r, nq, nf, q);
    rd_deltas(r, nf * 3, nf, h);
    c->rot = MemAlloc(nf * nb * sizeof(Quaternion));
    c->hips = MemAlloc(nf * sizeof(Vector3));
    for (int f = 0; f < nf; f++) {
        for (int b = 0; b < nb; b++) {
            float x = (int16_t)q[(b * 3 + 0) * nf + f] / 4096.0f;
            float y = (int16_t)q[(b * 3 + 1) * nf + f] / 4096.0f;
            float z = (int16_t)q[(b * 3 + 2) * nf + f] / 4096.0f;
            float w = sqrtf(fmaxf(0, 1 - x * x - y * y - z * z));
            c->rot[f * nb + b] = QuaternionNormalize((Quaternion){ x, y, z, w });
        }
        c->hips[f] = (Vector3){ (int16_t)h[f] / 1000.0f, (int16_t)h[nf + f] / 1000.0f, (int16_t)h[2 * nf + f] / 1000.0f };
    }
    MemFree(q);
    MemFree(h);
}

static void rd_cars(Rd *r)
{
    for (int i = 0; i < CAR_MODELS; i++) {
        CarModel *c = &car_models[i];
        for (int k = 0; k < 4; k++) c->wheel_pos[k] = (Vector3){ rdf(r), rdf(r), rdf(r) };
        c->wheel_r = rdf(r);
        c->size = (Vector3){ rdf(r), rdf(r), rdf(r) };
        c->seat = (Vector3){ rdf(r), rdf(r), rdf(r) };
        c->mass_kg = rdf(r);
        c->wheel = rd_mesh(r);
        c->body = rd_mesh(r);
    }
}

static void rd_chars(Rd *r)
{
    rd_skeleton(r);
    for (int i = 0; i < BODY_COUNT; i++) body_mesh[i] = rd_mesh(r);
    for (int i = 0; i < HAIR_COUNT; i++) hair_mesh[i] = rd_mesh(r);
}

static void rd_guns(Rd *r)
{
    for (int i = W_PISTOL; i < W_COUNT; i++) gun_mesh[i] = rd_mesh(r);
}

static void rd_heli(Rd *r)
{
    heli_mesh[0] = rd_mesh(r);
    heli_mesh[1] = rd_mesh(r);
}

static void rd_clips(Rd *r)
{
    for (int i = 0; i < A_COUNT; i++) rd_clip(r, &clips[i]);
}

// raw lzma stream (no header): props byte, then the range coder. decodes exactly n bytes
typedef unsigned short u16;
static const unsigned char *lz_in;
static unsigned lz_range, lz_code;

static int lz_bit(u16 *p)
{
    unsigned bound = (lz_range >> 11) * *p;
    int b = lz_code >= bound;
    if (b) { lz_range -= bound; lz_code -= bound; *p -= *p >> 5; }
    else { lz_range = bound; *p += (2048 - *p) >> 5; }
    if (lz_range < 1u << 24) { lz_range <<= 8; lz_code = lz_code << 8 | *lz_in++; }
    return b;
}

static int lz_tree(u16 *p, int bits)
{
    int m = 1;
    for (int i = 0; i < bits; i++) m = m * 2 + lz_bit(p + m);
    return m - (1 << bits);
}

static int lz_rtree(u16 *p, int bits)
{
    int m = 1, s = 0;
    for (int i = 0; i < bits; i++) { int b = lz_bit(p + m); m = m * 2 + b; s |= b << i; }
    return s;
}

// choice, choice2, low[16][8], mid[16][8], high[256]
static int lz_len(u16 *p, int ps)
{
    if (!lz_bit(p)) return lz_tree(p + 2 + ps * 8, 3);
    if (!lz_bit(p + 1)) return 8 + lz_tree(p + 130 + ps * 8, 3);
    return 16 + lz_tree(p + 258, 8);
}

enum { LZ_MATCH = 0, LZ_REP = 192, LZ_G0 = 204, LZ_G1 = 216, LZ_G2 = 228, LZ_R0L = 240, LZ_SLOT = 432,
       LZ_SPEC = 688, LZ_ALIGN = 803, LZ_LENS = 819, LZ_REPLEN = 1333, LZ_LIT = 1847 };

void lzma_unpack(const unsigned char *in, unsigned char *out, int n)
{
    int lc = in[0] % 9, lp = in[0] / 9 % 5, pb = in[0] / 45, nprob = LZ_LIT + (0x300 << (lc + lp));
    u16 *pr = malloc(nprob * 2);
    for (int i = 0; i < nprob; i++) pr[i] = 1024;
    lz_in = in + 2;
    lz_range = 0xFFFFFFFF;
    lz_code = lz_in[0] << 24 | lz_in[1] << 16 | lz_in[2] << 8 | lz_in[3];
    lz_in += 4;
    int state = 0, pos = 0;
    unsigned rep[4] = { 0 };
    while (pos < n) {
        int ps = pos & ((1 << pb) - 1), len;
        if (!lz_bit(pr + LZ_MATCH + (state << 4) + ps)) {
            u16 *l = pr + LZ_LIT + 0x300 * (((pos & ((1 << lp) - 1)) << lc) + ((pos ? out[pos - 1] : 0) >> (8 - lc)));
            int sym = 1;
            if (state >= 7) {
                int mb = out[pos - rep[0] - 1];
                while (sym < 0x100) {
                    int m = mb >> 7 & 1, b = lz_bit(l + ((1 + m) << 8) + sym);
                    mb <<= 1;
                    sym = sym * 2 + b;
                    if (m != b) break;
                }
            }
            while (sym < 0x100) sym = sym * 2 + lz_bit(l + sym);
            out[pos++] = sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        if (lz_bit(pr + LZ_REP + state)) {
            if (!lz_bit(pr + LZ_G0 + state)) {
                if (!lz_bit(pr + LZ_R0L + (state << 4) + ps)) {
                    state = state < 7 ? 9 : 11;
                    out[pos] = out[pos - rep[0] - 1];
                    pos++;
                    continue;
                }
            } else {
                unsigned d;
                if (!lz_bit(pr + LZ_G1 + state)) d = rep[1];
                else {
                    if (!lz_bit(pr + LZ_G2 + state)) d = rep[2];
                    else { d = rep[3]; rep[3] = rep[2]; }
                    rep[2] = rep[1];
                }
                rep[1] = rep[0];
                rep[0] = d;
            }
            len = lz_len(pr + LZ_REPLEN, ps);
            state = state < 7 ? 8 : 11;
        } else {
            rep[3] = rep[2]; rep[2] = rep[1]; rep[1] = rep[0];
            len = lz_len(pr + LZ_LENS, ps);
            state = state < 7 ? 7 : 10;
            int slot = lz_tree(pr + LZ_SLOT + ((len < 4 ? len : 3) << 6), 6);
            if (slot < 4) rep[0] = slot;
            else {
                int nd = (slot >> 1) - 1;
                rep[0] = (2 | (slot & 1)) << nd;
                if (slot < 14) rep[0] += lz_rtree(pr + LZ_SPEC + rep[0] - slot - 1, nd);
                else {
                    unsigned d = 0;
                    for (int i = 0; i < nd - 4; i++) {
                        lz_range >>= 1;
                        lz_code -= lz_range;
                        unsigned t = 0u - (lz_code >> 31);
                        lz_code += lz_range & t;
                        d = d * 2 + t + 1;
                        if (lz_range < 1u << 24) { lz_range <<= 8; lz_code = lz_code << 8 | *lz_in++; }
                    }
                    rep[0] += d << 4;
                    rep[0] += lz_rtree(pr + LZ_ALIGN, 4);
                }
            }
        }
        for (len += 2; len-- && pos < n; pos++) out[pos] = out[pos - rep[0] - 1];
    }
    free(pr);
}

static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

void assets_load(void)
{
    // sections must be read in dependency order: chars (skeleton) before clips
    static void (*const read[])(Rd *) = { rd_cars, rd_chars, rd_guns, rd_clips, rd_heli };
    int nsec = rd32(assets_blob), total = 0;
    for (int s = 0; s < nsec; s++) total += rd32(assets_blob + 4 + s * 4);
    unsigned char *raw = MemAlloc(total), *at = raw;
    lzma_unpack(assets_blob + 4 + nsec * 4, raw, total);
    for (int s = 0; s < nsec; s++) {
        Rd r = { at };
        read[s](&r);
        at += rd32(assets_blob + 4 + s * 4);
    }
    MemFree(raw);
}

void pose_sample(const Clip *c, float t, Quaternion *rot, Vector3 *hips)
{
    int nb = skel.count, last = c->frames - 1;
    float ft = t * c->fps, span = (float)last;
    ft = c->loop ? fmodf(fmodf(ft, span) + span, span) : Clamp(ft, 0, span);
    int f0 = (int)ft, f1 = f0 < last ? f0 + 1 : f0;
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
