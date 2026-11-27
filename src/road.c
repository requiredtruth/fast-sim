// the world: one endless two-lane highway through changing country. the land is streamed in
// 128 m segments built from the seed, so the same seed always gives the same road, stops and wrecks.
// geometry is kept relative to each segment's anchor, so the far reaches of the road stay precise.
#include "city.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double world_ox, world_oz;
Lane lanes[MAX_LANES]; int nlanes = MAX_LANES;
WalkNode *walk; int nwalk;
Cont conts[MAX_CONTS];
Stop stops[MAX_STOPS];
float weather_rain, weather_fog, weather_dust, weather_snow, weather_wind;
static uint32_t wseed;

#define SEG_LEN 128.0
#define RSTEP 4.0
#define ASPH 4.4f          // asphalt half width
#define LANE_OFF 1.85f      // lane centre from the centreline
#define NSEG 22             // segments kept built
#define LANE_RING 128
#define START_S 1400.0      // where a new game begins
#define START_K 11          // the first gas station is on this segment
#define BLOCK 12            // segments per stop block

// ---------------------------------------------------------------- hashing and noise

static uint32_t h32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
static uint32_t h3(int a, int b, int c) { return h32((uint32_t)a * 73856093u ^ h32((uint32_t)b * 19349663u ^ h32((uint32_t)c ^ wseed))); }
static float hu(int a, int b, int c) { return (h3(a, b, c) >> 8) * (1.0f / 16777216.0f); }
static float sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float sm(float a, float b, float x) { float t = sat((x - a) / (b - a)); return t * t * (3 - 2 * t); }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static float bump(float a, float b, float x) { return sm(a, (a + b) * 0.5f, x) * (1 - sm((a + b) * 0.5f, b, x)); }
static Vector3 V3(float x, float y, float z) { return (Vector3){ x, y, z }; }

static float vnoise(double x, double z, int salt)
{
    double fx = floor(x), fz = floor(z);
    int ix = (int)fx, iz = (int)fz;
    float tx = (float)(x - fx), tz = (float)(z - fz);
    tx = tx * tx * (3 - 2 * tx); tz = tz * tz * (3 - 2 * tz);
    float a = hu(ix, iz, salt), b = hu(ix + 1, iz, salt), c = hu(ix, iz + 1, salt), d = hu(ix + 1, iz + 1, salt);
    return mixf(mixf(a, b, tx), mixf(c, d, tx), tz);
}

static float fbm(double x, double z, int oct, int salt)
{
    float s = 0, a = 0.5f, n = 0;
    for (int i = 0; i < oct; i++) { s += a * vnoise(x, z, salt + i); n += a; x = x * 2.03 + 17.3; z = z * 2.03 + 5.1; a *= 0.5f; }
    return s / n;
}

// segment-local rng for building
static uint32_t brng;
static float rf(float a, float b) { brng ^= brng << 13; brng ^= brng >> 17; brng ^= brng << 5; return a + (b - a) * (brng >> 8) * (1.0f / 16777216.0f); }
static int ri(int n) { return (int)(rf(0, 0.99999f) * n); }

// ---------------------------------------------------------------- biomes

#define REGION 6500.0
static const float biome_amp[BIOME_COUNT] = { 9, 4, 16, 18, 20 };
static int rb_k[64], rb_v[64];
static bool rb_init;
static int region_biome_cached(int r)
{
    static const int fixed[WORLD_COUNT] = {
        [WORLD_CITY] = BIOME_PLAINS, [WORLD_FOREST] = BIOME_FOREST, [WORLD_SNOW] = BIOME_SNOW, [WORLD_MESA] = BIOME_BADLANDS
    };
    if (world_kind != WORLD_ROAD) return fixed[world_kind];
    int *ck = rb_k, *cv = rb_v;
    if (!rb_init) { for (int i = 0; i < 64; i++) ck[i] = INT32_MIN; rb_init = true; }
    int slot = r & 63;
    if (ck[slot] == r) return cv[slot];
    int b;
    if (r <= 0) b = BIOME_DESERT;
    else {
        uint32_t h = h3(r, 77, 3);
        b = (int)(h % BIOME_COUNT);
        int pb = region_biome_cached(r - 1);
        if (b == pb) b = (b + 1 + (int)((h >> 20) % (BIOME_COUNT - 1))) % BIOME_COUNT;
        if (b == pb) b = (b + 1) % BIOME_COUNT;
        if (b == BIOME_SNOW && r < 3) b = pb == BIOME_FOREST ? BIOME_PLAINS : BIOME_FOREST;
    }
    ck[slot] = r; cv[slot] = b;
    return b;
}

float biome_mix(float s, int b)
{
    double t = s / REGION;
    int r = (int)floor(t);
    float f = (float)(t - r);
    float w = sm(0.86f, 1.0f, f);
    float k = 0;
    if (region_biome_cached(r) == b) k += 1 - w;
    if (region_biome_cached(r + 1) == b) k += w;
    return k;
}

int biome_at(float s)
{
    double t = s / REGION;
    int r = (int)floor(t);
    return (t - r) > 0.93 ? region_biome_cached(r + 1) : region_biome_cached(r);
}

const char *biome_name(int b)
{
    static const char *n[BIOME_COUNT] = { "Desert", "Plains", "Forest", "Badlands", "Mountains" };
    return b >= 0 && b < BIOME_COUNT ? n[b] : "";
}

// ---------------------------------------------------------------- the road line

typedef struct { double x, z; float h, th; } RP;
static RP *rt;
static int nrt, crt;
static float ph[8];

static float heading_at(double s)
{
    if (world_kind == WORLD_CITY) return 0;
    float a = 0.5f * sinf((float)(s / 2100.0) + ph[0]) + 0.3f * sinf((float)(s / 830.0) + ph[1]) + 0.13f * sinf((float)(s / 360.0) + ph[2]);
    return a * sm(200, 1200, (float)s);
}

static float elev_at(double s)
{
    if (world_kind == WORLD_CITY) return 0;
    float amp = 0;
    for (int b = 0; b < BIOME_COUNT; b++) amp += biome_mix((float)s, b) * biome_amp[b];
    amp *= 0.9f;
    return amp * (0.6f * sinf((float)(s / 950.0) + ph[3]) + 0.4f * sinf((float)(s / 380.0) + ph[4])) * sm(300, 1500, (float)s)
         + 30 * sinf((float)(s / 7000.0) + ph[5]);
}

static void road_ensure(int k)
{
    if (k < nrt) return;
    if (k >= crt) { crt = (k + 1024) * 2; rt = realloc(rt, crt * sizeof(RP)); }
    while (nrt <= k) {
        if (nrt == 0) rt[0] = (RP){ 0, 0, elev_at(0), heading_at(0) };
        else {
            RP p = rt[nrt - 1];
            double s = (nrt - 0.5) * RSTEP;
            float th = heading_at(s);
            rt[nrt] = (RP){ p.x + sin(th) * RSTEP, p.z + cos(th) * RSTEP, elev_at(nrt * RSTEP), heading_at(nrt * RSTEP) };
        }
        nrt++;
    }
}

// absolute position on the line at road distance s
static void line_at(double s, double *x, double *z, float *h, float *th)
{
    if (s < 0) s = 0;
    double t = s / RSTEP;
    int k = (int)floor(t);
    road_ensure(k + 2);
    float f = (float)(t - k);
    RP a = rt[k], b = rt[k + 1];
    *x = a.x + (b.x - a.x) * f; *z = a.z + (b.z - a.z) * f;
    if (h) *h = a.h + (b.h - a.h) * f;
    if (th) *th = a.th + (b.th - a.th) * f;
}

static Vector3 dir_of(float th) { return V3(sinf(th), 0, cosf(th)); }
static Vector3 left_of(float th) { return V3(cosf(th), 0, -sinf(th)); }

// nearest road distance for an absolute point; z grows along the road, so a search on z finds the stretch
static int last_k;
static double road_s_abs(double X, double Z, float *off)
{
    road_ensure(64);
    int lo = 0, hi = nrt - 1;
    if (Z > rt[nrt - 1].z) { road_ensure(nrt + (int)((Z - rt[nrt - 1].z) / RSTEP) + 64); hi = nrt - 1; }
    // try the last answer first: most queries are near each other
    int k;
    if (last_k > 0 && last_k < nrt - 1 && rt[last_k].z <= Z && rt[last_k + 1].z > Z) k = last_k;
    else {
        while (hi - lo > 1) { int m = (lo + hi) / 2; if (rt[m].z <= Z) lo = m; else hi = m; }
        k = lo;
        last_k = k;
    }
    double s = k * RSTEP;
    for (int it = 0; it < 4; it++) {
        double x, z; float th;
        line_at(s, &x, &z, NULL, &th);
        double ds = (X - x) * sin(th) + (Z - z) * cos(th);
        if (ds > 200) ds = 200;
        if (ds < -200) ds = -200;
        s += ds;
        if (s < 0) { s = 0; break; }
        if (fabs(ds) < 0.01) break;
    }
    if (off) {
        double x, z; float th;
        line_at(s, &x, &z, NULL, &th);
        *off = (float)((X - x) * cos(th) - (Z - z) * sin(th));
    }
    return s;
}

float road_s(Vector3 p) { return (float)road_s_abs(p.x + world_ox, p.z + world_oz, NULL); }
float road_offset(float x, float z, float *s_out)
{
    float off;
    double s = road_s_abs(x + world_ox, z + world_oz, &off);
    if (s_out) *s_out = (float)s;
    return off;
}

Vector3 road_at(float s, Vector3 *dir)
{
    double x, z; float h, th;
    line_at(s, &x, &z, &h, &th);
    if (dir) *dir = dir_of(th);
    return V3((float)(x - world_ox), h + 0.03f, (float)(z - world_oz));
}

static float cross_street_z(int k) { return k % 2 == 0 ? 112 : 16; }

bool on_road(float x, float z)
{
    float s;
    float d = fabsf(road_offset(x, z, &s));
    if (d < ASPH) return true;
    if (world_kind != WORLD_CITY) return false;
    int k = (int)floorf(s / (float)SEG_LEN);
    return fabsf(d - 112) < 4 ||
           (d < 136 && fabsf(s - k * (float)SEG_LEN - cross_street_z(k)) < 4);
}

// ---------------------------------------------------------------- stops

typedef struct {
    int k, kind, side, biome;
    double s, px, pz;   // lot centre, absolute
    float d, R, y, yaw;
    bool pond; double wx, wz; float wr;   // pond centre and radius
    uint32_t name;
} StopDesc;

static float kind_weight(int kind, int biome)
{
    static const float w[STOP_COUNT][BIOME_COUNT] = {
        //            desert plains forest badlands snow
        [STOP_GAS]    = { 18, 16, 14, 16, 14 },
        [STOP_DINER]  = { 10, 12, 8, 8, 6 },
        [STOP_MOTEL]  = { 10, 9, 7, 9, 8 },
        [STOP_FARM]   = { 3, 18, 12, 2, 4 },
        [STOP_WRECKS] = { 12, 9, 8, 14, 10 },
        [STOP_CAMP]   = { 5, 8, 16, 5, 9 },
        [STOP_TOWN]   = { 5, 6, 5, 4, 4 },
        [STOP_TRADER] = { 9, 8, 8, 9, 9 },
        [STOP_RANGER] = { 2, 3, 10, 4, 10 },
        [STOP_GARAGE] = { 8, 8, 6, 8, 6 },
    };
    return w[kind][biome];
}

static const float stop_R[STOP_COUNT] = { 0, 27, 22, 30, 36, 14, 22, 56, 18, 20, 24 };

static StopDesc sd_cache[256];
static bool sd_init;
static StopDesc *stop_desc(int k)
{
    if (!sd_init) { for (int i = 0; i < 256; i++) sd_cache[i].k = -99999; sd_init = true; }
    StopDesc *d = &sd_cache[k & 255];
    if (d->k == k) return d;
    memset(d, 0, sizeof *d);
    d->k = k;
    d->kind = STOP_NONE;
    if (k == START_K) d->kind = STOP_GAS;
    else if (world_kind == WORLD_CITY && k > 2) {
        static const int districts[] = { STOP_TOWN, STOP_GARAGE, STOP_TOWN, STOP_DINER, STOP_TOWN, STOP_TRADER, STOP_TOWN, STOP_MOTEL };
        d->kind = districts[k % (sizeof districts / sizeof *districts)];
    } else if (world_kind == WORLD_FOREST && k > START_K && k % 5 == 0) {
        d->kind = k % 10 == 0 ? STOP_RANGER : STOP_CAMP;
    } else if (world_kind == WORLD_SNOW && k > START_K && k % 7 == 0) {
        d->kind = k % 2 == 0 ? STOP_MOTEL : STOP_GARAGE;
    } else if (world_kind == WORLD_MESA && k > START_K && k % 7 == 0) {
        d->kind = k % 2 == 0 ? STOP_WRECKS : STOP_TRADER;
    } else if (k > START_K + 3) {
        int b = (k - START_K - 4) / BLOCK, o = (k - START_K - 4) % BLOCK;
        int o1 = (int)(hu(b, 5, 1) * (BLOCK - 4)) + 1;
        bool two = hu(b, 5, 2) < 0.3f;
        int o2 = (o1 + 4 + (int)(hu(b, 5, 3) * (BLOCK - 8))) % BLOCK;
        if (o == o1 || (two && o == o2)) {
            int biome = biome_at((float)(k * SEG_LEN + 64));
            float tot = 0;
            for (int i = 1; i < STOP_COUNT; i++) tot += kind_weight(i, biome);
            float r = hu(k, 6, 1) * tot;
            for (int i = 1; i < STOP_COUNT; i++) { r -= kind_weight(i, biome); if (r <= 0) { d->kind = i; break; } }
            if (d->kind == STOP_NONE) d->kind = STOP_GAS;
            if (o == o2 && d->kind == STOP_TOWN) d->kind = STOP_CAMP;
        }
    }
    if (d->kind == STOP_NONE) return d;
    d->s = k * SEG_LEN + 64;
    d->side = world_kind == WORLD_CITY && k != START_K ? (k % 2 ? -1 : 1) : (k == START_K || hu(k, 7, 1) < 0.5f) ? -1 : 1;
    d->biome = biome_at((float)d->s);
    d->R = d->kind == STOP_WRECKS ? 0 : stop_R[d->kind];
    float dist = d->kind == STOP_TOWN ? 48 : d->kind == STOP_WRECKS ? 0 : d->R * 0.9f + 9 + hu(k, 7, 2) * 6;
    d->d = d->side * dist;
    double x, z; float h, th;
    line_at(d->s, &x, &z, &h, &th);
    Vector3 l = left_of(th);
    d->px = x + l.x * d->d; d->pz = z + l.z * d->d;
    d->y = h + 0.12f;
    // the lot faces the road: local +z points back at it
    Vector3 to = Vector3Scale(l, -(float)d->side);
    d->yaw = atan2f(to.x, to.z);
    d->name = h3(k, 8, 1);
    if ((d->kind == STOP_CAMP || d->kind == STOP_FARM || d->kind == STOP_RANGER) && d->biome != BIOME_DESERT && d->biome != BIOME_BADLANDS) {
        d->pond = true;
        d->wr = 9 + hu(k, 9, 1) * 6;
        Vector3 back = Vector3Scale(to, -(d->R * 0.55f + d->wr));
        Vector3 sd = Vector3Scale(dir_of(th), (hu(k, 9, 2) - 0.5f) * 20);
        d->wx = d->px + back.x + sd.x; d->wz = d->pz + back.z + sd.z;
    }
    return d;
}

static const char *name_a[24] = { "Coyote", "Silver", "Dry", "Lone", "Red", "Copper", "Dust", "Pine", "Cedar", "Rattler",
                                  "Hollow", "Juniper", "Mesa", "Buzzard", "Iron", "Sage", "Willow", "Granite", "Antelope",
                                  "Broken", "Sunset", "Cottonwood", "Black", "Bitter" };
static const char *name_b[12] = { "Creek", "Springs", "Flats", "Ridge", "Gulch", "Well", "Butte", "Fork", "Bend", "Pass",
                                  "Valley", "Crossing" };
static const char *kind_word[STOP_COUNT] = { "", "Gas", "Diner", "Motel", "Farm", "Pileup", "Campground", "", "Trading Post",
                                             "Ranger Station", "Garage" };

static void stop_name(const StopDesc *d, char *out, int n)
{
    const char *a = name_a[d->name % 24], *b = name_b[(d->name >> 8) % 12];
    if (d->kind == STOP_TOWN) snprintf(out, n, "%s %s", a, b);
    else if (d->kind == STOP_WRECKS) snprintf(out, n, "Pileup near %s %s", a, b);
    else snprintf(out, n, "%s %s %s", a, b, kind_word[d->kind]);
}

// ---------------------------------------------------------------- height

static float hills(double X, double Z, double s)
{
    float amp = 0, mesa = 0;
    for (int b = 0; b < BIOME_COUNT; b++) {
        float w = biome_mix((float)s, b);
        amp += w * biome_amp[b];
        if (b == BIOME_BADLANDS) mesa = w;
    }
    float n = fbm(X / 260, Z / 260, 3, 101) - 0.5f;
    float n2 = vnoise(X / 55, Z / 55, 111) - 0.5f;
    float h = amp * (n * 2.4f + n2 * 0.35f);
    if (mesa > 0) {
        float m = fbm(X / 330, Z / 330, 2, 121);
        float terr = sm(0.52f, 0.58f, m) * 26 + sm(0.66f, 0.7f, m) * 18;
        h = mixf(h, h * 0.3f + terr, mesa);
    }
    return h;
}

static float lot_blend(double X, double Z, int k, float h)
{
    for (int kk = k - 1; kk <= k + 1; kk++) {
        if (kk < 0) continue;
        StopDesc *d = stop_desc(kk);
        if (d->kind == STOP_NONE) continue;
        if (d->R > 0) {
            float dist = (float)hypot(X - d->px, Z - d->pz);
            float w = sm(d->R, d->R + 22, dist);
            h = mixf(d->y, h, w);
        }
        if (d->pond) {
            float dist = (float)hypot(X - d->wx, Z - d->wz);
            if (dist < d->wr + 6) {
                float bowl = d->y - 2.6f * (1 - sm(0, d->wr + 1.5f, dist)) - 0.4f;
                h = fminf(h, mixf(bowl, h, sm(d->wr - 1, d->wr + 6, dist)));
            }
        }
    }
    return h;
}

#define WATER_DY (-0.55f)

static float height_sd(double s, float d, double X, double Z)
{
    float e = elev_at(s);
    float ad = fabsf(d);
    if (ad < ASPH) return e + 0.03f;
    if (world_kind == WORLD_CITY && ad < 153) return 0;
    float w = sm(ASPH + 3, ASPH + 70, ad);
    float ditch = -0.6f * bump(ASPH + 0.6f, ASPH + 7.5f, ad);
    float h = mixf(e + ditch, e + hills(X, Z, s), w);
    if (ad < ASPH + 0.9f) h = e - (ad - ASPH) * 0.12f;
    return lot_blend(X, Z, (int)floor(s / SEG_LEN), h);
}

static float height_abs(double X, double Z)
{
    float d;
    double s = road_s_abs(X, Z, &d);
    return height_sd(s, d, X, Z);
}

float ground_y(float x, float z) { return height_abs(x + world_ox, z + world_oz); }

static bool water_abs(double X, double Z, float *depth)
{
    float d;
    double s = road_s_abs(X, Z, &d);
    int k = (int)floor(s / SEG_LEN);
    for (int kk = k - 1; kk <= k + 1; kk++) {
        if (kk < 0) continue;
        StopDesc *sd = stop_desc(kk);
        if (!sd->pond) continue;
        float dist = (float)hypot(X - sd->wx, Z - sd->wz);
        if (dist < sd->wr) {
            if (depth) *depth = sd->y + WATER_DY - height_sd(s, d, X, Z);
            return true;
        }
    }
    return false;
}

bool is_water(float x, float z)
{
    float dep;
    return water_abs(x + world_ox, z + world_oz, &dep) && dep > 0.6f;
}

// ---------------------------------------------------------------- building buffers

typedef struct { Vtx *v; uint32_t *i; int nv, ni, cv, ci; } Buf;
typedef struct { float cx, cz, hx, hz, y0, y1, c, s; } Box;   // centre, half sizes, yaw (cos, sin), anchor-relative

typedef struct { int kind; Vector3 p; float yaw; int slot; } ContDef;
typedef struct { int model; Vector3 p; float yaw; bool burnt; int slot; } CarDef;

typedef struct {
    bool used, built, live;
    int k;
    double ax, az;
    GLuint vao[GEO_PARTS], vbo[GEO_PARTS], ebo[GEO_PARTS];
    int count[GEO_PARTS];
    Vector3 mn, mx;
    Box *box; int nbox;
    int *cell_start, *cell_items, gnx, gnz; float gx0, gz0;
    ContDef cont[40]; int ncont;
    CarDef car[10]; int ncar;
    Vector3 lamp[12]; int nlamp;
    int stop_slot;
    float age;
} Seg;

static Seg segs[NSEG];
static Buf bufs[GEO_PARTS];
static Box *bx; static int nbx, cbx;
static Seg *cur;            // segment being built
static double cax, caz;     // its anchor

// local building frame: rotation about y then translation (anchor relative)
static float fr_c = 1, fr_s = 0;
static Vector3 fr_o;

static Vector3 fr_p(Vector3 p) { return V3(fr_o.x + p.x * fr_c + p.z * fr_s, fr_o.y + p.y, fr_o.z - p.x * fr_s + p.z * fr_c); }
static Vector3 fr_n(Vector3 n) { return V3(n.x * fr_c + n.z * fr_s, n.y, -n.x * fr_s + n.z * fr_c); }
static void frame_set(Vector3 o, float yaw) { fr_o = o; fr_c = cosf(yaw); fr_s = sinf(yaw); }
static void frame_reset(void) { fr_o = V3(0, 0, 0); fr_c = 1; fr_s = 0; }

typedef struct { int layer; uint8_t r, g, b, flags; float seed; } Mat;
#define PICK(...) (((const uint32_t[]){ __VA_ARGS__ })[ri((int)(sizeof((const uint32_t[]){ __VA_ARGS__ }) / 4))])
static Mat Mf(int l, uint32_t rgb) { return (Mat){ l, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 0, 0 }; }
#define M(l, rgb) Mf((l), (rgb))
static Mat mf(Mat m, int flags, float seed) { m.flags |= flags; m.seed = seed; return m; }
static Mat tint(Mat m, float k)
{
    m.r = (uint8_t)fminf(255, m.r * k); m.g = (uint8_t)fminf(255, m.g * k); m.b = (uint8_t)fminf(255, m.b * k);
    return m;
}
static Mat M_STEEL, M_BLACK, M_DARK, M_WHITE;

static float lsz(int l) { return l >= 0 && l < L_COUNT && layer_size[l] > 0 ? layer_size[l] : 2.0f; }

static uint32_t vert(Buf *b, Vector3 p, Vector3 n, float u, float v, Mat m)
{
    if (b->nv == b->cv) { b->cv = b->cv ? b->cv * 2 : 1024; b->v = realloc(b->v, b->cv * sizeof(Vtx)); }
    p = fr_p(p); n = fr_n(n);
    Vtx *x = &b->v[b->nv];
    x->p[0] = p.x; x->p[1] = p.y; x->p[2] = p.z;
    x->uv[0] = u; x->uv[1] = v;
    x->n[0] = (int8_t)lrintf(Clamp(n.x, -1, 1) * 127); x->n[1] = (int8_t)lrintf(Clamp(n.y, -1, 1) * 127);
    x->n[2] = (int8_t)lrintf(Clamp(n.z, -1, 1) * 127); x->n[3] = 0;
    x->c[0] = m.r; x->c[1] = m.g; x->c[2] = m.b; x->c[3] = m.flags;
    x->layer = (float)m.layer; x->seed = m.seed;
    return (uint32_t)b->nv++;
}

static void tri(Buf *b, uint32_t a, uint32_t c, uint32_t d)
{
    if (b->ni + 3 > b->ci) { b->ci = b->ci ? b->ci * 2 : 2048; b->i = realloc(b->i, b->ci * sizeof(uint32_t)); }
    b->i[b->ni++] = a; b->i[b->ni++] = c; b->i[b->ni++] = d;
}

static Buf *buf_at(int part, float x, float z) { (void)x; (void)z; return &bufs[part]; }

static void quad_pn(int part, Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, Vector3 n, float u0, float v0, float u1, float v1, Mat m)
{
    Buf *b = buf_at(part, 0, 0);
    uint32_t a = vert(b, p0, n, u0, v0, m), c = vert(b, p1, n, u1, v0, m), d = vert(b, p2, n, u1, v1, m), e = vert(b, p3, n, u0, v1, m);
    tri(b, a, c, d);
    tri(b, a, d, e);
}

static void quadv(int part, Vector3 o, Vector3 eu, Vector3 ev, float u0, float v0, float u1, float v1, Mat m)
{
    Vector3 n = Vector3Normalize(Vector3CrossProduct(eu, ev));
    quad_pn(part, o, Vector3Add(o, eu), Vector3Add(Vector3Add(o, eu), ev), Vector3Add(o, ev), n, u0, v0, u1, v1, m);
}

static void hq(int part, float x0, float z0, float x1, float z1, float y, Mat m)
{
    float s = 1.0f / lsz(m.layer);
    quadv(part, V3(x0, y, z1), V3(x1 - x0, 0, 0), V3(0, 0, z0 - z1), x0 * s, z1 * s, x1 * s, z0 * s, m);
}

// vertical face; side: 0 = faces -z, 1 = +z, 2 = -x, 3 = +x
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
    float s = 1.0f / lsz(m.layer);
    vf(part, side, a0, a1, c, y0, y1, m, a0 * s, a1 * s, y0 * s, y1 * s);
}
static float cell_w(int l) { return lsz(l) > 0 ? lsz(l) : 3; }
static void facade(int part, int side, float a0, float a1, float c, float y0, float y1, Mat m)
{
    int l = m.layer;
    float cx = layer_cells[l].x > 0 ? layer_cells[l].x : 1, cy = layer_cells[l].y > 0 ? layer_cells[l].y : 1;
    float cw = cx > 0 ? 3.6f : cell_w(l), ch = 3.4f;
    if (l == L_STOREFRONT) { cw = 3.6f; ch = 4.2f; }
    if (l == L_WIN_PLAIN) { cw = 6.0f; ch = 4.0f; }
    int nu = (int)fmaxf(1, roundf((a1 - a0) / cw)), nv = (int)fmaxf(1, roundf((y1 - y0) / ch));
    vf(part, side, a0, a1, c, y0, y1, m, 0, nu / cx, 0, nv / cy);
}

static void add_box(float x0, float y0, float z0, float x1, float y1, float z1)
{
    if (nbx == cbx) { cbx = cbx ? cbx * 2 : 512; bx = realloc(bx, cbx * sizeof(Box)); }
    Vector3 c = fr_p(V3((x0 + x1) * 0.5f, 0, (z0 + z1) * 0.5f));
    bx[nbx++] = (Box){ c.x, c.z, fabsf(x1 - x0) * 0.5f, fabsf(z1 - z0) * 0.5f, fr_o.y + fminf(y0, y1), fr_o.y + fmaxf(y0, y1), fr_c, fr_s };
}

static void box(int part, float x0, float y0, float z0, float x1, float y1, float z1, Mat side, Mat top)
{
    vfm(part, 1, x0, x1, z1, y0, y1, side);
    vfm(part, 0, x0, x1, z0, y0, y1, side);
    vfm(part, 3, z0, z1, x1, y0, y1, side);
    vfm(part, 2, z0, z1, x0, y0, y1, side);
    hq(part, x0, z0, x1, z1, y1, top);
}
static void sbox(float x0, float y0, float z0, float x1, float y1, float z1, Mat side, Mat top) { box(GEO_SOLID, x0, y0, z0, x1, y1, z1, side, top); }
static void solid(float x0, float y0, float z0, float x1, float y1, float z1, Mat side, Mat top)
{
    sbox(x0, y0, z0, x1, y1, z1, side, top);
    add_box(x0, y0, z0, x1, y1, z1);
}

static void tube(int part, Vector3 a, Vector3 b, float r0, float r1, int n, Mat m, float urep, float vrep)
{
    Vector3 w = Vector3Normalize(Vector3Subtract(b, a));
    Vector3 t = fabsf(w.y) < 0.9f ? V3(0, 1, 0) : V3(1, 0, 0);
    Vector3 s = Vector3Normalize(Vector3CrossProduct(w, t)), q = Vector3CrossProduct(w, s);
    Buf *bf = buf_at(part, 0, 0);
    uint32_t prev0 = 0, prev1 = 0;
    for (int i = 0; i <= n; i++) {
        float ang = i * 6.2832f / n;
        Vector3 d = Vector3Add(Vector3Scale(s, cosf(ang)), Vector3Scale(q, sinf(ang)));
        Vector3 nn = Vector3Normalize(Vector3Add(d, Vector3Scale(w, (r0 - r1) / fmaxf(Vector3Distance(a, b), 0.01f))));
        uint32_t v0 = vert(bf, Vector3Add(a, Vector3Scale(d, r0)), nn, urep * i / n, 0, m);
        uint32_t v1 = vert(bf, Vector3Add(b, Vector3Scale(d, r1)), nn, urep * i / n, vrep, m);
        if (i) { tri(bf, prev0, v0, v1); tri(bf, prev0, v1, prev1); }
        prev0 = v0; prev1 = v1;
    }
}

static void disc(int part, Vector3 c, Vector3 nrm, float r, int n, Mat m)
{
    Vector3 t = fabsf(nrm.y) < 0.9f ? V3(0, 1, 0) : V3(1, 0, 0);
    Vector3 s = Vector3Normalize(Vector3CrossProduct(nrm, t)), q = Vector3CrossProduct(nrm, s);
    Buf *bf = buf_at(part, 0, 0);
    uint32_t ctr = vert(bf, c, nrm, 0.5f, 0.5f, m), prev = 0;
    for (int i = 0; i <= n; i++) {
        float ang = i * 6.2832f / n;
        Vector3 d = Vector3Add(Vector3Scale(s, cosf(ang)), Vector3Scale(q, sinf(ang)));
        uint32_t v = vert(bf, Vector3Add(c, Vector3Scale(d, r)), nrm, 0.5f + 0.5f * cosf(ang), 0.5f + 0.5f * sinf(ang), m);
        if (i) tri(bf, ctr, prev, v);
        prev = v;
    }
}

static void cyl(int part, float x, float y0, float z, float y1, float r, int n, Mat side, Mat top)
{
    tube(part, V3(x, y0, z), V3(x, y1, z), r, r, n, side, 1, (y1 - y0) / lsz(side.layer));
    disc(part, V3(x, y1, z), V3(0, 1, 0), r, n, top);
}

static void cone(Vector3 base, float r, float h, int n, Mat m)
{
    tube(GEO_SOLID, base, Vector3Add(base, V3(0, h, 0)), r, 0.02f, n, m, 2, h * 0.5f);
    disc(GEO_SOLID, base, V3(0, -1, 0), r, n, tint(m, 0.6f));
}

static void leaf_card(Vector3 c, Vector3 right, Vector3 up, Vector3 n, float s, Mat m)
{
    Vector3 r = Vector3Scale(right, s), u = Vector3Scale(up, s);
    quad_pn(GEO_SOLID, Vector3Subtract(Vector3Subtract(c, r), u), Vector3Subtract(Vector3Add(c, r), u), Vector3Add(Vector3Add(c, r), u),
            Vector3Add(Vector3Subtract(c, r), u), n, 0, 0, 1, 1, m);
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
        Vector3 side = Vector3Scale(Vector3Normalize(V3(-d.z, 0, d.x)), 0.02f);
        quadv(GEO_SOLID, Vector3Subtract(pa, side), Vector3Scale(side, 2), d, 0, 0, 1, 1, m);
        quadv(GEO_SOLID, Vector3Subtract(pa, V3(0, 0.02f, 0)), V3(0, 0.04f, 0), d, 0, 0, 1, 1, m);
        pa = pb;
    }
}

// sign board using one row of the sign sheets; idx 0..31
static void sign_board(Vector3 c, float w, float h, int idx, float glow)
{
    int sheet = idx / 8, row = idx % 8;
    Mat m = M(L_SIGN0 + sheet, 0xffffff);
    if (glow > 0) m = mf(m, F_SELF, glow);
    float v0 = row / 8.0f, v1 = (row + 1) / 8.0f;
    quadv(GEO_SOLID, V3(c.x - w * 0.5f, c.y - h * 0.5f, c.z + 0.06f), V3(w, 0, 0), V3(0, h, 0), 0, v1, 1, v0, m);
    quadv(GEO_SOLID, V3(c.x + w * 0.5f, c.y - h * 0.5f, c.z - 0.06f), V3(-w, 0, 0), V3(0, h, 0), 0, v1, 1, v0, m);
    sbox(c.x - w * 0.5f - 0.05f, c.y - h * 0.5f - 0.05f, c.z - 0.05f, c.x + w * 0.5f + 0.05f, c.y + h * 0.5f + 0.05f, c.z + 0.05f, M_DARK, M_DARK);
}

static void lamp_add(Vector3 p)
{
    if (cur->nlamp < 12) cur->lamp[cur->nlamp++] = fr_p(p);
}

static void cont_add(int kind, Vector3 p, float yaw)
{
    if (cur->ncont >= 40) return;
    float wy = atan2f(fr_s, fr_c);
    cur->cont[cur->ncont] = (ContDef){ kind, fr_p(p), yaw + wy, cur->ncont };
    cur->ncont++;
}

static void car_add(int model, Vector3 p, float yaw, bool burnt)
{
    if (cur->ncar >= 10) return;
    float wy = atan2f(fr_s, fr_c);
    cur->car[cur->ncar] = (CarDef){ model, fr_p(p), yaw + wy, burnt, cur->ncar };
    cur->ncar++;
}

// ---------------------------------------------------------------- vegetation and props

static Mat ground_mat(double s, float d)
{
    if (world_kind == WORLD_CITY && fabsf(d) < 145) return M(L_CONCRETE, 0xa9a6a0);
    float grass = 0, r = 0, g = 0, b = 0;
    static const float gw[BIOME_COUNT] = { 0.05f, 0.78f, 0.95f, 0.02f, 0.25f };
    static const uint32_t col[BIOME_COUNT] = { 0xf2e2c4, 0xf0f0d8, 0xd8e8c8, 0xf0b090, 0xf4f8ff };
    for (int bi = 0; bi < BIOME_COUNT; bi++) {
        float w = biome_mix((float)s, bi);
        grass += w * gw[bi];
        r += w * (col[bi] >> 16 & 255); g += w * (col[bi] >> 8 & 255); b += w * (col[bi] & 255);
    }
    float snow = biome_mix((float)s, BIOME_SNOW);
    if (snow > 0.5f) grass *= 1 - sm(0.5f, 1, snow) * 0.8f;
    Mat m = M(L_SAND, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
    if (snow > 0.5f) m = M(L_SNOW, 0xffffff);
    return mf(m, F_BLEND, grass);
}

static void pine(Vector3 p, float h, bool snowy)
{
    Mat bark = M(L_BARK, 0x9a8878);
    tube(GEO_SOLID, p, Vector3Add(p, V3(0, h * 0.35f, 0)), 0.22f * h / 10, 0.14f * h / 10, 6, bark, 1, 2);
    uint32_t gc = snowy ? 0x9ab89a : 0x7aa860;
    float k = rf(0.85f, 1.15f);
    for (int i = 0; i < 4; i++) {
        float t = i / 4.0f;
        Mat m = tint(M(L_GRASS, gc), k * (0.9f + 0.1f * i));
        cone(Vector3Add(p, V3(0, h * (0.22f + 0.2f * t), 0)), h * (0.3f - 0.06f * i), h * (0.42f - 0.05f * i), 7, m);
        if (snowy) cone(Vector3Add(p, V3(0, h * (0.22f + 0.2f * t) + h * 0.2f, 0)), h * (0.16f - 0.03f * i), h * (0.24f - 0.03f * i), 7, M(L_SNOW, 0xffffff));
    }
    add_box(p.x - 0.3f, p.y, p.z - 0.3f, p.x + 0.3f, p.y + h * 0.6f, p.z + 0.3f);
}

static void tree_broad(Vector3 b, float h, float dry)
{
    Mat bark = M(L_BARK, 0xb0a090);
    float lean = rf(-0.4f, 0.4f);
    Vector3 mid = V3(b.x + lean, b.y + h * 0.42f, b.z + lean * 0.5f);
    tube(GEO_SOLID, b, mid, 0.24f, 0.16f, 7, bark, 1, h * 0.5f);
    Vector3 c = V3(b.x + lean, b.y + h * 0.72f, b.z + lean * 0.5f);
    float R = h * 0.36f;
    for (int i = 0; i < 4; i++) {
        float a = i * 1.57f + rf(0, 1);
        Vector3 e = V3(c.x + cosf(a) * R * 0.55f, c.y + R * 0.1f, c.z + sinf(a) * R * 0.55f);
        tube(GEO_SOLID, mid, e, 0.11f, 0.05f, 5, bark, 1, 2);
    }
    float hue = rf(0.8f, 1.15f), warm = rf(-0.1f, 0.2f) + dry * 0.5f;
    int n = 40;
    for (int i = 0; i < n; i++) {
        float a = rf(0, 6.2832f), bb = acosf(rf(-0.55f, 1.0f)), rr = R * powf(rf(0.45f, 1.0f), 0.5f);
        Vector3 p = V3(c.x + sinf(bb) * cosf(a) * rr * 1.1f, c.y + cosf(bb) * rr * 0.85f, c.z + sinf(bb) * sinf(a) * rr * 1.1f);
        Vector3 nn = Vector3Normalize(Vector3Subtract(p, Vector3Add(c, V3(0, -R * 0.2f, 0))));
        float ra = rf(0, 3.1416f), t = rf(0.35f, 1.0f);
        Vector3 right = V3(cosf(ra), rf(-0.3f, 0.3f), sinf(ra)), up = Vector3Normalize(Vector3CrossProduct(right, nn));
        right = Vector3Normalize(Vector3CrossProduct(nn, up));
        float g = hue * rf(0.85f, 1.1f);
        Mat m = mf(M(L_LEAF, ((int)fminf(255, 205 * g * (1 + warm)) << 16) | ((int)fminf(255, 230 * g) << 8) | (int)fmaxf(0, 170 * (1 - warm))),
                   F_CUT, sat((p.y - c.y) / R * 0.5f + 0.55f));
        leaf_card(p, right, up, nn, 1.3f * t + 0.6f, m);
    }
    add_box(b.x - 0.25f, b.y, b.z - 0.25f, b.x + 0.25f, b.y + h * 0.5f, b.z + 0.25f);
}

static void dead_tree(Vector3 b, float h)
{
    Mat bark = M(L_BARK, 0x8a7a6a);
    Vector3 top = Vector3Add(b, V3(rf(-0.5f, 0.5f), h, rf(-0.5f, 0.5f)));
    tube(GEO_SOLID, b, top, 0.2f, 0.06f, 6, bark, 1, h * 0.5f);
    for (int i = 0; i < 4; i++) {
        float t = rf(0.35f, 0.8f), a = rf(0, 6.28f);
        Vector3 s = Vector3Lerp(b, top, t);
        tube(GEO_SOLID, s, Vector3Add(s, V3(cosf(a) * h * 0.3f, h * rf(0.1f, 0.3f), sinf(a) * h * 0.3f)), 0.07f, 0.02f, 4, bark, 1, 1);
    }
    add_box(b.x - 0.2f, b.y, b.z - 0.2f, b.x + 0.2f, b.y + h * 0.6f, b.z + 0.2f);
}

static void cactus(Vector3 b, float h)
{
    Mat g = M(L_GRASS, 0x9ac088);
    tube(GEO_SOLID, b, Vector3Add(b, V3(0, h, 0)), 0.22f, 0.2f, 8, g, 2, h);
    disc(GEO_SOLID, Vector3Add(b, V3(0, h, 0)), V3(0, 1, 0), 0.2f, 8, g);
    int arms = 1 + ri(2);
    for (int i = 0; i < arms; i++) {
        float a = rf(0, 6.28f), y = h * rf(0.35f, 0.6f), len = rf(0.5f, 0.8f), up = h * rf(0.25f, 0.4f);
        Vector3 s = Vector3Add(b, V3(0, y, 0)), e = Vector3Add(s, V3(cosf(a) * len, 0.05f, sinf(a) * len));
        tube(GEO_SOLID, s, e, 0.13f, 0.13f, 6, g, 1, 1);
        tube(GEO_SOLID, e, Vector3Add(e, V3(0, up, 0)), 0.13f, 0.12f, 6, g, 1, 1);
        disc(GEO_SOLID, Vector3Add(e, V3(0, up, 0)), V3(0, 1, 0), 0.12f, 6, g);
    }
    add_box(b.x - 0.25f, b.y, b.z - 0.25f, b.x + 0.25f, b.y + h, b.z + 0.25f);
}

static void shrub(Vector3 b, float s, uint32_t col)
{
    for (int i = 0; i < 5; i++) {
        float a = rf(0, 6.28f), r = rf(0, 0.4f) * s;
        Vector3 c = Vector3Add(b, V3(cosf(a) * r, s * rf(0.35f, 0.6f), sinf(a) * r));
        float ra = rf(0, 3.14f);
        Vector3 right = V3(cosf(ra), 0, sinf(ra)), up = V3(0, 1, 0), n = V3(-sinf(ra), 0.3f, cosf(ra));
        Mat m = mf(tint(M(L_LEAF, col), rf(0.85f, 1.15f)), F_CUT, 0.4f);
        leaf_card(c, right, up, Vector3Normalize(n), s * 0.55f, m);
        leaf_card(c, V3(-sinf(ra), 0, cosf(ra)), up, Vector3Normalize(V3(cosf(ra), 0.3f, sinf(ra))), s * 0.5f, m);
    }
}

static void rock(Vector3 b, float s, uint32_t col)
{
    enum { SEG = 7, RING = 4 };
    Vector3 p[RING + 1][SEG];
    float sx = s * rf(0.8f, 1.3f), sy = s * rf(0.45f, 0.8f), sz = s * rf(0.8f, 1.3f), rot = rf(0, 6.28f);
    for (int j = 0; j <= RING; j++)
        for (int i = 0; i < SEG; i++) {
            float ph = -0.35f + (PI * 0.5f + 0.35f) * j / RING, th = rot + i * 6.2832f / SEG;
            float k = rf(0.78f, 1.12f);
            float r = cosf(ph) * k;
            p[j][i] = Vector3Add(b, V3(cosf(th) * r * sx, (sinf(ph) * k + 0.25f) * sy, sinf(th) * r * sz));
        }
    Mat m = tint(M(L_CONCRETE, col), rf(0.75f, 1.05f));
    Vector3 c = Vector3Add(b, V3(0, sy * 0.4f, 0));
    float sc = 1 / lsz(L_CONCRETE);
    for (int j = 0; j < RING; j++)
        for (int i = 0; i < SEG; i++) {
            int i2 = (i + 1) % SEG;
            Vector3 a = p[j][i], bb = p[j][i2], cc = p[j + 1][i2], d = p[j + 1][i];
            Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(bb, a), Vector3Subtract(d, a)));
            Vector3 mid = Vector3Scale(Vector3Add(Vector3Add(a, bb), Vector3Add(cc, d)), 0.25f);
            if (Vector3DotProduct(n, Vector3Subtract(mid, c)) < 0) n = Vector3Negate(n);
            quad_pn(GEO_SOLID, a, bb, cc, d, n, a.x * sc, a.y * sc, cc.x * sc + 0.3f, cc.y * sc + 0.3f, m);
        }
    // cap
    Buf *bf = buf_at(GEO_SOLID, 0, 0);
    Vector3 top = Vector3Add(b, V3(0, sy * 1.15f, 0));
    for (int i = 0; i < SEG; i++) {
        Vector3 a = p[RING][i], bb = p[RING][(i + 1) % SEG];
        Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(bb, a), Vector3Subtract(top, a)));
        if (n.y < 0) n = Vector3Negate(n);
        uint32_t i0 = vert(bf, a, n, a.x * sc, a.z * sc, m), i1 = vert(bf, bb, n, bb.x * sc, bb.z * sc, m), i2 = vert(bf, top, n, top.x * sc, top.z * sc, m);
        tri(bf, i0, i1, i2);
    }
    if (s > 0.5f) add_box(b.x - sx * 0.75f, b.y, b.z - sz * 0.75f, b.x + sx * 0.75f, b.y + sy, b.z + sz * 0.75f);
}

static void pole(Vector3 b, float yaw)
{
    Mat wd = M(L_BARK, 0x8a7a68);
    float h = 9.0f;
    tube(GEO_SOLID, b, Vector3Add(b, V3(0, h, 0)), 0.16f, 0.12f, 6, wd, 1, 4);
    Vector3 ax = V3(cosf(yaw), 0, -sinf(yaw));
    Vector3 c = Vector3Add(b, V3(0, h - 0.65f, 0));
    Vector3 a0 = Vector3Add(c, Vector3Scale(ax, -1.1f)), a1 = Vector3Add(c, Vector3Scale(ax, 1.1f));
    tube(GEO_SOLID, a0, a1, 0.07f, 0.07f, 4, wd, 1, 1);
    add_box(b.x - 0.2f, b.y, b.z - 0.2f, b.x + 0.2f, b.y + h, b.z + 0.2f);
}

// ---------------------------------------------------------------- buildings (lot frame: y = 0 is the lot, +z faces the road)

// a room with walls on four sides; door gap in the +z wall. inside faces lit as interior.
static void room(float x0, float z0, float x1, float z1, float h, Mat out, Mat in, Mat floor, float door_x, float door_w, bool windows)
{
    float T = 0.2f, dh = 2.25f;
    Mat wout = out;
    if (windows) wout.layer = out.layer == L_BRICK ? L_WIN_BRICK : L_WIN_STUCCO;
    Mat inm = mf(in, F_INTER, 0);
    // back wall (-z), sides
    solid(x0, 0, z0, x1, h, z0 + T, out, out);
    facade(GEO_SOLID, 0, x0, x1, z0 - 0.002f, 0, h, wout);
    vfm(GEO_SOLID, 1, x0 + T, x1 - T, z0 + T + 0.002f, 0, h, inm);
    solid(x0, 0, z0 + T, x0 + T, h, z1 - T, out, out);
    facade(GEO_SOLID, 2, z0, z1, x0 - 0.002f, 0, h, wout);
    vfm(GEO_SOLID, 3, z0 + T, z1 - T, x0 + T + 0.002f, 0, h, inm);
    solid(x1 - T, 0, z0 + T, x1, h, z1 - T, out, out);
    facade(GEO_SOLID, 3, z0, z1, x1 + 0.002f, 0, h, wout);
    vfm(GEO_SOLID, 2, z0 + T, z1 - T, x1 - T - 0.002f, 0, h, inm);
    // front wall with the door
    float da = door_x - door_w * 0.5f, db = door_x + door_w * 0.5f;
    solid(x0, 0, z1 - T, da, h, z1, out, out);
    solid(db, 0, z1 - T, x1, h, z1, out, out);
    solid(da, dh, z1 - T, db, h, z1, out, out);
    facade(GEO_SOLID, 1, x0, da, z1 + 0.002f, 0, h, wout);
    facade(GEO_SOLID, 1, db, x1, z1 + 0.002f, 0, h, wout);
    vfm(GEO_SOLID, 0, x0 + T, da, z1 - T - 0.002f, 0, h, inm);
    vfm(GEO_SOLID, 0, db, x1 - T, z1 - T - 0.002f, 0, h, inm);
    // door frame
    sbox(da - 0.06f, 0, z1 - 0.02f, da, dh + 0.06f, z1 + 0.04f, M_DARK, M_DARK);
    sbox(db, 0, z1 - 0.02f, db + 0.06f, dh + 0.06f, z1 + 0.04f, M_DARK, M_DARK);
    // floor and ceiling
    hq(GEO_SOLID, x0 + T, z0 + T, x1 - T, z1 - T, 0.03f, mf(floor, F_INTER, 0));
    Mat ceil = mf(in, F_INTER, 0);
    quadv(GEO_SOLID, V3(x0 + T, h - 0.01f, z0 + T), V3(x1 - x0 - 2 * T, 0, 0), V3(0, 0, z1 - z0 - 2 * T), 0, 0, 1, 1, ceil);
}

static void flat_roof(float x0, float z0, float x1, float z1, float h, Mat m)
{
    sbox(x0 - 0.15f, h, z0 - 0.15f, x1 + 0.15f, h + 0.25f, z1 + 0.15f, m, M(L_GRAVEL, 0xb0aca4));
    add_box(x0, h, z0, x1, h + 0.25f, z1);
}

static void gable_roof(float x0, float z0, float x1, float z1, float h, float rise, Mat m, Mat gable)
{
    float o = 0.35f, cx = (x0 + x1) * 0.5f;
    Vector3 a = V3(x0 - o, h - 0.1f, z0 - o), b = V3(x0 - o, h - 0.1f, z1 + o), c = V3(cx, h + rise, z1 + o), d = V3(cx, h + rise, z0 - o);
    Vector3 e = V3(x1 + o, h - 0.1f, z0 - o), f = V3(x1 + o, h - 0.1f, z1 + o);
    float sc = 1.0f / lsz(m.layer);
    float sl = Vector3Distance(a, d);
    Vector3 n1 = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(d, a)));
    if (n1.y < 0) n1 = Vector3Negate(n1);
    quad_pn(GEO_SOLID, a, b, c, d, n1, 0, 0, (z1 - z0 + 2 * o) * sc, sl * sc, m);
    Vector3 n2 = V3(-n1.x, n1.y, -n1.z);
    quad_pn(GEO_SOLID, f, e, d, c, n2, 0, 0, (z1 - z0 + 2 * o) * sc, sl * sc, m);
    // gable ends
    for (int s = 0; s < 2; s++) {
        float z = s ? z1 : z0;
        Vector3 n = V3(0, 0, s ? 1.0f : -1.0f);
        Buf *bf = buf_at(GEO_SOLID, 0, 0);
        float gs = 1.0f / lsz(gable.layer);
        uint32_t i0 = vert(bf, V3(x0, h, z), n, x0 * gs, 0, gable), i1 = vert(bf, V3(x1, h, z), n, x1 * gs, 0, gable);
        uint32_t i2 = vert(bf, V3(cx, h + rise - 0.1f, z), n, cx * gs, rise * gs, gable);
        tri(bf, i0, i1, i2);
    }
    add_box(x0, h, z0, x1, h + rise * 0.6f, z1);
}

static void shelf(Vector3 p, float yaw, float w)
{
    Mat wd = M(L_WOOD, 0xc0a080);
    float c = cosf(yaw), s = sinf(yaw);
    (void)c; (void)s;
    sbox(p.x - w * 0.5f, 0, p.z - 0.25f, p.x + w * 0.5f, 1.8f, p.z - 0.2f, wd, wd);
    for (int k = 0; k < 4; k++) sbox(p.x - w * 0.5f, 0.1f + k * 0.55f, p.z - 0.25f, p.x + w * 0.5f, 0.14f + k * 0.55f, p.z + 0.25f, wd, wd);
    for (int k = -1; k <= 1; k += 2) sbox(p.x + k * w * 0.5f - 0.04f, 0, p.z - 0.25f, p.x + k * w * 0.5f, 1.8f, p.z + 0.25f, wd, wd);
    // a few goods on the shelves
    for (int k = 0; k < 6; k++) {
        float x = p.x + rf(-w * 0.4f, w * 0.4f), y = 0.14f + ri(3) * 0.55f;
        uint32_t col = (uint32_t[]){ 0xc03020, 0x2050a0, 0xe0c040, 0x40a050, 0xd0d0d0 }[ri(5)];
        sbox(x - 0.08f, y, p.z - 0.12f, x + 0.08f, y + rf(0.12f, 0.28f), p.z + 0.1f, M(L_PLAIN, col), M(L_PLAIN, col));
    }
    add_box(p.x - w * 0.5f, 0, p.z - 0.25f, p.x + w * 0.5f, 1.8f, p.z + 0.25f);
}

static void furniture(int kind, Vector3 p, float yaw)
{
    // p is the front face centre of the furniture; it is built facing +z then the container is registered
    Mat wd = M(L_WOOD, 0xb89470), met = M(L_METAL, 0x8a9098), wh = M(L_PLAIN, 0xe8e8e4);
    switch (kind) {
    case CT_FRIDGE: solid(p.x - 0.4f, 0, p.z - 0.75f, p.x + 0.4f, 1.9f, p.z, wh, wh);
        sbox(p.x + 0.25f, 0.9f, p.z, p.x + 0.3f, 1.5f, p.z + 0.05f, M_DARK, M_DARK); break;
    case CT_SHELF: shelf(V3(p.x, 0, p.z - 0.25f), yaw, 1.8f); break;
    case CT_CRATE: solid(p.x - 0.5f, 0, p.z - 0.8f, p.x + 0.5f, 0.75f, p.z, M(L_WOOD, 0xa08058), M(L_WOOD, 0xa08058)); break;
    case CT_LOCKER: solid(p.x - 0.45f, 0, p.z - 0.5f, p.x + 0.45f, 1.95f, p.z, met, met);
        sbox(p.x - 0.01f, 0.1f, p.z, p.x + 0.01f, 1.85f, p.z + 0.02f, M_DARK, M_DARK); break;
    case CT_DESK: solid(p.x - 0.7f, 0, p.z - 0.7f, p.x + 0.7f, 0.78f, p.z, wd, wd); break;
    case CT_TOOLBOX: solid(p.x - 0.4f, 0, p.z - 0.5f, p.x + 0.4f, 0.95f, p.z, M(L_PLAIN, 0xb02420), M(L_PLAIN, 0xb02420));
        for (int k = 0; k < 3; k++) sbox(p.x - 0.36f, 0.15f + k * 0.27f, p.z, p.x + 0.36f, 0.17f + k * 0.27f, p.z + 0.01f, M_DARK, M_DARK); break;
    case CT_BED: solid(p.x - 0.75f, 0, p.z - 2.0f, p.x + 0.75f, 0.5f, p.z, wd, M(L_PLAIN, 0xd0c8b8));
        sbox(p.x - 0.6f, 0.5f, p.z - 1.95f, p.x + 0.6f, 0.62f, p.z - 1.55f, wh, wh); break;
    case CT_BENCH: solid(p.x - 1.0f, 0, p.z - 0.8f, p.x + 1.0f, 0.95f, p.z, met, M(L_WOOD, 0x8a6a48)); break;
    case CT_WELL: {
        Mat st = M(L_CONCRETE, 0xb0a898);
        for (int k = 0; k < 8; k++) {
            float a = k * 0.785f;
            sbox(p.x + cosf(a) * 0.75f - 0.22f, 0, p.z - 0.9f + sinf(a) * 0.75f - 0.22f, p.x + cosf(a) * 0.75f + 0.22f, 0.8f, p.z - 0.9f + sinf(a) * 0.75f + 0.22f, st, st);
        }
        hq(GEO_SOLID, p.x - 0.6f, p.z - 1.5f, p.x + 0.6f, p.z - 0.3f, 0.3f, M(L_PLAIN, 0x101418));
        for (int s = -1; s <= 1; s += 2) sbox(p.x + s * 0.9f - 0.06f, 0, p.z - 0.96f, p.x + s * 0.9f + 0.06f, 2.0f, p.z - 0.84f, wd, wd);
        sbox(p.x - 1.1f, 2.0f, p.z - 1.4f, p.x + 1.1f, 2.1f, p.z - 0.4f, M(L_SHINGLE, 0x8a5a40), M(L_SHINGLE, 0x8a5a40));
        add_box(p.x - 0.95f, 0, p.z - 1.85f, p.x + 0.95f, 0.8f, p.z + 0.05f);
        break;
    }
    case CT_PUMP: {
        Mat r = M(L_PLAIN, 0xc02a20);
        solid(p.x - 0.4f, 0, p.z - 0.3f, p.x + 0.4f, 1.7f, p.z + 0.3f, r, M(L_PLAIN, 0xe8e4dc));
        sbox(p.x - 0.32f, 1.05f, p.z + 0.3f, p.x + 0.32f, 1.5f, p.z + 0.32f, M(L_PLAIN, 0xe8e4dc), M_WHITE);
        sbox(p.x - 0.2f, 1.15f, p.z + 0.32f, p.x + 0.2f, 1.35f, p.z + 0.33f, M(L_PLAIN, 0x101418), M_DARK);
        sbox(p.x + 0.4f, 0.75f, p.z - 0.06f, p.x + 0.48f, 1.0f, p.z + 0.06f, M_DARK, M_DARK);
        sbox(p.x - 0.4f, 0, p.z - 0.3f, p.x + 0.4f, 1.7f, p.z - 0.29f, r, r);
        break;
    }
    default: break;
    }
    cont_add(kind, V3(p.x, 0, p.z + 0.6f), yaw);
}

static void canopy(float x0, float z0, float x1, float z1, float h)
{
    Mat pm = M(L_PLAIN, 0xe0e0dc), red = M(L_PLAIN, 0xb02420);
    for (int i = 0; i < 4; i++) {
        float x = i & 1 ? x1 - 0.6f : x0 + 0.6f, z = i & 2 ? z1 - 0.6f : z0 + 0.6f;
        solid(x - 0.18f, 0, z - 0.18f, x + 0.18f, h, z + 0.18f, pm, pm);
    }
    sbox(x0, h, z0, x1, h + 0.7f, z1, red, M(L_METAL, 0xd0d0d0));
    // the underside faces down, with the strip lights in it
    quadv(GEO_SOLID, V3(x0, h, z0), V3(x1 - x0, 0, 0), V3(0, 0, z1 - z0), 0, 0, (x1 - x0) / 2, (z1 - z0) / 2, M(L_METAL, 0xe8e8e4));
    for (int k = 0; k < 3; k++) {
        float z = z0 + (z1 - z0) * (k + 1) / 4;
        quadv(GEO_SOLID, V3(x0 + 1, h - 0.03f, z - 0.15f), V3(x1 - x0 - 2, 0, 0), V3(0, 0, 0.3f), 0, 0, 1, 1, mf(M(L_PLAIN, 0xfff4e0), F_SELF, 2.5f));
    }
    lamp_add(V3((x0 + x1) * 0.5f, h - 0.2f, (z0 + z1) * 0.5f));
    lamp_add(V3(x0 + 1, h - 0.2f, (z0 + z1) * 0.5f));
}

static void lot_surface(float x0, float z0, float x1, float z1, Mat m)
{
    hq(GEO_GROUND, x0, z0, x1, z1, 0.02f, m);
}

static void fence_line(Vector3 a, Vector3 b, float h)
{
    Mat wd = M(L_BARK, 0x9a8a78), wr = M(L_METAL, 0x6a6a6a);
    float len = Vector3Distance(a, b);
    int n = (int)fmaxf(1, len / 3);
    for (int i = 0; i <= n; i++) {
        Vector3 p = Vector3Lerp(a, b, (float)i / n);
        tube(GEO_SOLID, p, Vector3Add(p, V3(0, h, 0)), 0.06f, 0.05f, 4, wd, 1, 1);
    }
    for (int k = 1; k <= 2; k++) wire(Vector3Add(a, V3(0, h * k / 2.6f, 0)), Vector3Add(b, V3(0, h * k / 2.6f, 0)), 0.1f, wr);
}

// a small house with a gable roof, door facing +z, interior with containers
static void house(float cx, float cz, float w, float d, uint32_t col, bool search)
{
    Mat out = M(L_STUCCO, col), in = M(L_PLAIN, 0xe8e0d0), fl = M(L_WOOD, 0xc0a080);
    float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f, z0 = cz - d * 0.5f, z1 = cz + d * 0.5f, h = 2.9f;
    room(x0, z0, x1, z1, h, out, in, fl, cx + w * 0.2f, 1.1f, true);
    gable_roof(x0, z0, x1, z1, h, 1.6f, M(L_SHINGLE, PICK(0x6a4a3a, 0x4a4a52, 0x7a3a2a)), out);
    sbox(cx + w * 0.2f - 0.9f, 0, z1, cx + w * 0.2f + 0.9f, 0.18f, z1 + 1.2f, M(L_CONCRETE, 0xc0c0b8), M(L_CONCRETE, 0xc0c0b8));
    if (!search) return;
    furniture(CT_FRIDGE, V3(x0 + 0.9f, 0, z0 + 1.2f), PI);
    furniture(CT_DESK, V3(x1 - 1.2f, 0, z0 + 1.1f), PI);
    furniture(ri(2) ? CT_BED : CT_LOCKER, V3(x0 + 1.0f, 0, z1 - 0.4f), 0);
}

static void store(float cx, float cz, float w, float d, int sign, bool full)
{
    Mat out = M(L_STUCCO, PICK(0xe8dcc0, 0xd8c8a8, 0xc8d0d8, 0xe0c8b0)), in = M(L_PLAIN, 0xf0ece4), fl = M(L_SIDEWALK, 0xd8d4cc);
    float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f, z0 = cz - d * 0.5f, z1 = cz + d * 0.5f, h = 3.4f;
    room(x0, z0, x1, z1, h, out, in, fl, cx, 1.6f, true);
    flat_roof(x0, z0, x1, z1, h, out);
    sbox(x0 - 0.3f, h - 0.7f, z1, x1 + 0.3f, h - 0.6f, z1 + 1.6f, M(L_AWNING, 0xffffff), M(L_AWNING, 0xffffff));
    if (sign >= 0) sign_board(V3(cx, h + 0.75f, z1 - 0.1f), fminf(w * 0.8f, 7), 0.9f, sign, 1.5f);
    lamp_add(V3(cx, h - 0.5f, z1 + 0.8f));
    furniture(CT_SHELF, V3(x0 + 1.4f, 0, z0 + 1.0f), PI);
    if (w > 7) furniture(CT_SHELF, V3(cx + 0.5f, 0, z0 + 1.0f), PI);
    furniture(CT_FRIDGE, V3(x1 - 0.7f, 0, z0 + 1.4f), PI);
    furniture(CT_DESK, V3(x1 - 1.4f, 0, z1 - 1.0f), 0);
    if (full) furniture(CT_SHELF, V3(x0 + 1.4f, 0, cz + 0.5f), PI);
}

static int name_sign(int kind)
{
    switch (kind) {
    case STOP_GAS: return 0; case STOP_DINER: return 1; case STOP_MOTEL: return 2; case STOP_TRADER: return 4;
    case STOP_GARAGE: return 5; case STOP_RANGER: return 6; case STOP_FARM: return 7; case STOP_CAMP: return 15;
    default: return 13;
    }
}

static void stop_build(StopDesc *d)
{
    double s = d->s;
    (void)s;
    frame_set(V3((float)(d->px - cax), d->y, (float)(d->pz - caz)), d->yaw);
    brng = h3(d->k, 33, 1) | 1;
    Mat gravel = M(L_GRAVEL, 0xc8c0b0), asph = M(L_ASPHALT, 0xd8d8d8);
    int sign = name_sign(d->kind);
    switch (d->kind) {
    case STOP_GAS: {
        lot_surface(-24, -16, 24, 18, asph);
        canopy(-9, 1, 9, 12, 4.8f);
        furniture(CT_PUMP, V3(-4, 0, 6.5f), 0);
        furniture(CT_PUMP, V3(4, 0, 6.5f), 0);
        store(0, -9, 11, 8, sign, d->k != START_K);
        // tall sign by the road
        for (int k = -1; k <= 1; k += 2) solid(17 + k * 0.6f - 0.12f, 0, 14 - 0.12f, 17 + k * 0.6f + 0.12f, 7, 14 + 0.12f, M_STEEL, M_STEEL);
        sign_board(V3(17, 7.8f, 14), 4.5f, 1.6f, d->k == START_K ? 14 : 0, 2.0f);
        lamp_add(V3(17, 7, 14));
        // a tank fill and an air pump
        sbox(12, 0, -2, 12.6f, 1.2f, -1.4f, M(L_PLAIN, 0x3060a0), M(L_PLAIN, 0x3060a0));
        if (ri(3) == 0 || d->k == START_K) car_add(ri(2) ? CAR_SEDAN : CAR_PICKUP, V3(-15, 0, 4), rf(-0.4f, 0.4f) + PI * 0.5f, false);
        break;
    }
    case STOP_DINER: {
        lot_surface(-22, -14, 22, 16, asph);
        Mat out = M(L_METAL, 0xd8dce0), in = M(L_PLAIN, 0xf0e8e0), fl = M(L_SIDEWALK, 0xe0d0d0);
        room(-9, -6, 9, 3, 3.3f, out, in, fl, 0, 1.6f, true);
        flat_roof(-9, -6, 9, 3, 3.3f, M(L_PLAIN, 0xb02420));
        sbox(-9.2f, 1.0f, 3.0f, 9.2f, 1.25f, 3.1f, M(L_PLAIN, 0xb02420), M(L_PLAIN, 0xb02420));
        sign_board(V3(0, 4.6f, 2.8f), 6, 1.2f, (d->name & 1) ? 1 : 19, 2.0f);
        furniture(CT_DESK, V3(-3, 0, -1.5f), 0);
        furniture(CT_FRIDGE, V3(6.5f, 0, -5.5f + 0.75f), PI);
        furniture(CT_SHELF, V3(2.5f, 0, -5.0f), PI);
        furniture(CT_LOCKER, V3(-7.8f, 0, -5.2f), PI);
        for (int k = 0; k < 3; k++) sbox(-7 + k * 2.2f - 0.4f, 0, 1.4f, -7 + k * 2.2f + 0.4f, 0.75f, 2.2f, M(L_WOOD, 0x8a5a3a), M(L_WOOD, 0x8a5a3a));
        lamp_add(V3(0, 3, 4));
        if (ri(2)) car_add(CAR_WAGON, V3(12, 0, 8), PI * 0.5f, false);
        break;
    }
    case STOP_MOTEL: {
        lot_surface(-28, -12, 28, 14, asph);
        int rooms = 4 + ri(3);
        float w = 4.2f, x = -rooms * w * 0.5f;
        Mat out = M(L_STUCCO, 0xe8c8a0), in = M(L_PLAIN, 0xe8e0d0), fl = M(L_TILE, 0xb0a090);
        for (int r = 0; r < rooms; r++) {
            room(x + r * w, -8, x + (r + 1) * w, -2, 2.9f, out, in, fl, x + r * w + w * 0.3f, 1.0f, true);
            furniture(CT_BED, V3(x + r * w + w * 0.65f, 0, -3.0f), 0);
            if (r % 2 == 0) furniture(ri(2) ? CT_LOCKER : CT_DESK, V3(x + r * w + 1.0f, 0, -7.4f), PI);
            sbox(x + r * w + w * 0.3f - 0.5f, 2.3f, -2, x + r * w + w * 0.3f + 0.5f, 2.32f, -1.98f, M(L_PLAIN, 0x203040), M_DARK);
        }
        flat_roof(x, -8, x + rooms * w, -2, 2.9f, out);
        sbox(x - 0.2f, 2.8f, -2, x + rooms * w + 0.2f, 2.95f, 0.2f, M(L_PLAIN, 0x5a6a4a), M(L_PLAIN, 0x5a6a4a));
        for (int k = -1; k <= 1; k += 2) solid(x - 4 + k * 0.4f - 0.1f, 0, 8 - 0.1f, x - 4 + k * 0.4f + 0.1f, 6, 8 + 0.1f, M_STEEL, M_STEEL);
        sign_board(V3(x - 4, 6.6f, 8), 4.5f, 1.2f, (d->name & 2) ? 2 : 17, 2.0f);
        sign_board(V3(x - 4, 5.6f, 8), 3.5f, 0.6f, ri(2) ? 8 : 9, 2.5f);
        lamp_add(V3(0, 2.5f, -1));
        if (ri(2)) car_add(CAR_HATCH, V3(x + 2, 0, 1.5f), PI, false);
        if (ri(3) == 0) car_add(CAR_VAN, V3(x + 10, 0, 1.5f), PI, false);
        break;
    }
    case STOP_FARM: {
        lot_surface(-12, -2, 6, 22, gravel);
        house(-12, -14, 8, 7, 0xe8e4d8, true);
        // barn: big open shed
        Mat red = M(L_WOOD, 0xd06048), in = M(L_WOOD, 0xa07050);
        room(4, -18, 16, -6, 4.2f, red, in, M(L_GRAVEL, 0xb0a890), 10, 3.4f, false);
        gable_roof(4, -18, 16, -6, 4.2f, 3.2f, M(L_METAL, 0x8a8a8a), red);
        furniture(CT_CRATE, V3(6.5f, 0, -16.2f), PI);
        furniture(CT_TOOLBOX, V3(13.5f, 0, -16.6f), PI);
        furniture(CT_CRATE, V3(14.4f, 0, -9.5f), -PI * 0.5f);
        if (!d->pond) furniture(CT_WELL, V3(0, 0, -2), 0);
        fence_line(V3(-26, 0, 14), V3(26, 0, 14), 1.2f);
        fence_line(V3(-26, 0, -28), V3(-26, 0, 14), 1.2f);
        fence_line(V3(26, 0, -28), V3(26, 0, 14), 1.2f);
        sign_board(V3(-4, 1.6f, 15), 3, 0.5f, 7, 0);
        for (int k = 0; k < 3; k++) tree_broad(V3(rf(-24, 24), 0, rf(-26, -20)), rf(7, 10), 0);
        if (ri(2)) car_add(CAR_PICKUP, V3(1, 0, 6), rf(0, 6.28f), false);
        break;
    }
    case STOP_WRECKS: {
        // on the road itself: a pileup across the lanes and the shoulder
        frame_reset();
        double x, z; float h, th;
        int n = 3 + ri(3);
        for (int i = 0; i < n; i++) {
            double ss = d->s + rf(-30, 30);
            line_at(ss, &x, &z, &h, &th);
            Vector3 l = left_of(th);
            float off = rf(-7, 7);
            Vector3 p = V3((float)(x - cax) + l.x * off, 0, (float)(z - caz) + l.z * off);
            p.y = height_abs(p.x + cax, p.z + caz);
            car_add(ri(10), p, th + rf(-1.2f, 1.2f), ri(3) == 0);
        }
        for (int i = 0; i < 3; i++) {
            line_at(d->s + rf(-25, 25), &x, &z, &h, &th);
            Vector3 l = left_of(th);
            float off = rf(5.5f, 9) * (ri(2) ? 1 : -1);
            Vector3 p = V3((float)(x - cax) + l.x * off, 0, (float)(z - caz) + l.z * off);
            p.y = height_abs(p.x + cax, p.z + caz);
            fr_o = V3(0, 0, 0);
            cont_add(i == 0 ? CT_CORPSE : CT_BAG, p, rf(0, 6.28f));
        }
        break;
    }
    case STOP_CAMP: {
        lot_surface(-10, -10, 10, 10, M(L_GRAVEL, 0xa8a090));
        cont_add(CT_BENCH, V3(0, 0, 0), 0);   // the fire pit, used for camping
        sbox(-0.6f, 0, -0.6f, 0.6f, 0.25f, 0.6f, M(L_CONCRETE, 0x7a7670), M(L_PLAIN, 0x202020));
        for (int i = 0; i < 2 + ri(2); i++) {
            float a = i * 2.2f + rf(-0.3f, 0.3f), r = rf(4.5f, 7);
            float tx = cosf(a) * r, tz = sinf(a) * r;
            Mat tm = M(L_PLAIN, PICK(0x3a6a8a, 0x8a6a3a, 0x5a7a3a, 0xa04030));
            float ty = -a + PI * 0.5f;
            (void)ty;
            Vector3 in = V3(tx, 0.6f, tz);
            quad_pn(GEO_SOLID, V3(tx - 1.1f, 0, tz - 1.2f), V3(tx - 1.1f, 0, tz + 1.2f), V3(tx, 1.3f, tz + 1.2f), V3(tx, 1.3f, tz - 1.2f), Vector3Normalize(V3(-1.3f, 1.1f, 0)), 0, 0, 1, 1, tm);
            quad_pn(GEO_SOLID, V3(tx + 1.1f, 0, tz + 1.2f), V3(tx + 1.1f, 0, tz - 1.2f), V3(tx, 1.3f, tz - 1.2f), V3(tx, 1.3f, tz + 1.2f), Vector3Normalize(V3(1.3f, 1.1f, 0)), 0, 0, 1, 1, tm);
            (void)in;
            add_box(tx - 1.1f, 0, tz - 1.2f, tx + 1.1f, 1.2f, tz + 1.2f);
            cont_add(CT_BAG, V3(tx, 0, tz + 1.8f), 0);
        }
        cont_add(CT_CRATE, V3(-6, 0, -6), 0);
        sbox(-6.5f, 0, -7.0f, -5.5f, 0.7f, -6.2f, M(L_WOOD, 0x9a7a50), M(L_WOOD, 0x9a7a50));
        for (int k = 0; k < 6; k++) pine(V3(rf(-20, 20), 0, rf(-22, -12)), rf(9, 14), d->biome == BIOME_SNOW);
        for (int k = -1; k <= 1; k += 2) solid(8 + k * 0.4f - 0.08f, 0, 9.5f - 0.08f, 8 + k * 0.4f + 0.08f, 2.4f, 9.5f + 0.08f, M(L_WOOD, 0x6a4a30), M(L_WOOD, 0x6a4a30));
        sign_board(V3(8, 2.7f, 9.5f), 3.2f, 0.8f, 15, 0);
        if (ri(2)) car_add(CAR_JEEP, V3(-7, 0, 5), rf(0, 6.28f), false);
        break;
    }
    case STOP_TOWN: {
        lot_surface(-50, -6, 50, 6, asph);
        for (float x = -48; x < 48; x += 4) {
            hq(GEO_GROUND, x, -0.08f, x + 2.2f, 0.08f, 0.035f, mf(M(L_EDGE, 0xffd040), F_CUT | F_DECAL, 0));
        }
        int n = 4 + ri(3);
        for (int i = 0; i < n; i++) {
            float x = -42 + i * (84.0f / (n - 1));
            bool north = i & 1;
            frame_set(V3((float)(d->px - cax), d->y, (float)(d->pz - caz)), d->yaw);
            // buildings alternate sides of the main street, facing it
            Vector3 o = fr_p(V3(x, 0, north ? -12.0f : 12.0f));
            frame_set(o, d->yaw + (north ? 0 : PI));
            int pick = ri(5);
            if (pick < 2) store(0, 0, 9 + rf(0, 3), 8, i == 0 ? 13 : (int)(rf(10, 14)), true);
            else if (pick < 4) house(0, 0, 8, 7, (uint32_t[]){ 0xe8e4d8, 0xd0d8c0, 0xe0d0b0, 0xc8d0e0 }[ri(4)], true);
            else {
                Mat out = M(L_BRICK, 0xc89070), in = M(L_PLAIN, 0xd8d0c4);
                room(-6, -5, 6, 4, 4.0f, out, in, M(L_CONCRETE, 0xa8a8a0), 0, 3.0f, false);
                flat_roof(-6, -5, 6, 4, 4.0f, out);
                sign_board(V3(0, 4.7f, 3.9f), 5, 1.0f, 12, 1.0f);
                furniture(CT_TOOLBOX, V3(-4.5f, 0, -4.2f), PI);
                furniture(CT_SHELF, V3(3.5f, 0, -4.0f), PI);
            }
            if (ri(3) == 0) car_add(ri(8), V3(rf(-3, 3), 0, 6.5f), PI * 0.5f, ri(4) == 0);
            frame_set(V3((float)(d->px - cax), d->y, (float)(d->pz - caz)), d->yaw);
        }
        frame_set(V3((float)(d->px - cax), d->y, (float)(d->pz - caz)), d->yaw);
        for (int k = 0; k < 4; k++) lamp_add(V3(-36 + k * 24.0f, 5, 6));
        for (int k = 0; k < 4; k++) {
            float x = -36 + k * 24.0f;
            tube(GEO_SOLID, V3(x, 0, 6.5f), V3(x, 6, 6.5f), 0.1f, 0.07f, 6, M_STEEL, 1, 3);
            sbox(x - 0.3f, 5.8f, 5.6f, x + 0.3f, 6.0f, 6.6f, M_STEEL, mf(M(L_PLAIN, 0xfff2d0), F_SELF, 3));
            add_box(x - 0.15f, 0, 6.35f, x + 0.15f, 6, 6.65f);
        }
        break;
    }
    case STOP_TRADER: {
        lot_surface(-12, -10, 12, 12, gravel);
        Mat out = M(L_WOOD, 0xb09070), in = M(L_WOOD, 0x8a6a50);
        room(-6, -7, 6, 1, 3.0f, out, in, M(L_WOOD, 0x8a6a4a), 0, 2.4f, false);
        gable_roof(-6, -7, 6, 1, 3.0f, 1.8f, M(L_METAL, 0x9a8a7a), out);
        sbox(-6.3f, 2.4f, 1, 6.3f, 2.5f, 3.2f, M(L_WOOD, 0x6a4a30), M(L_METAL, 0x8a8a8a));
        for (int k = -1; k <= 1; k += 2) solid(k * 6 - 0.1f, 0, 3.0f, k * 6 + 0.1f, 2.4f, 3.2f, M(L_WOOD, 0x6a4a30), M(L_WOOD, 0x6a4a30));
        sign_board(V3(0, 3.6f, 1.1f), 5.5f, 1.0f, 4, 1.5f);
        sbox(-3, 0, -2.4f, 3, 1.0f, -1.6f, M(L_WOOD, 0x7a5a3a), M(L_WOOD, 0x9a7a5a));
        add_box(-3, 0, -2.4f, 3, 1.0f, -1.6f);
        cont_add(CT_TRADER, V3(0, 0, -1.0f), 0);
        furniture(CT_CRATE, V3(-4.5f, 0, -5.4f), PI);
        furniture(CT_SHELF, V3(3.0f, 0, -6.0f), PI);
        solid(7.5f, 0, -3, 9.5f, 0.9f, -1, M(L_PLAIN, 0x2a5a7a), M(L_PLAIN, 0x244a62));
        lamp_add(V3(0, 2.3f, 2));
        break;
    }
    case STOP_RANGER: {
        lot_surface(-12, -10, 12, 12, gravel);
        Mat out = M(L_WOOD, 0x8a6a48), in = M(L_WOOD, 0xb08a60);
        room(-5, -6, 5, 1, 3.0f, out, in, M(L_WOOD, 0x9a7a5a), 0, 1.2f, true);
        gable_roof(-5, -6, 5, 1, 3.0f, 2.2f, M(L_SHINGLE, 0x3a4a3a), out);
        furniture(CT_LOCKER, V3(-3.8f, 0, -5.3f), PI);
        furniture(CT_LOCKER, V3(-2.8f, 0, -5.3f), PI);
        furniture(CT_DESK, V3(2.5f, 0, -4.8f), PI);
        furniture(CT_BED, V3(3.6f, 0, 0.4f), 0);
        // lookout tower
        for (int i = 0; i < 4; i++) {
            float x = 8 + (i & 1) * 3.0f, z = -8 + (i >> 1) * 3.0f;
            tube(GEO_SOLID, V3(x, 0, z), V3(x + ((i & 1) ? -0.5f : 0.5f), 9, z + ((i >> 1) ? -0.5f : 0.5f)), 0.12f, 0.1f, 5, M(L_BARK, 0x8a7a68), 1, 3);
            add_box(x - 0.15f, 0, z - 0.15f, x + 0.15f, 9, z + 0.15f);
        }
        sbox(7.8f, 9, -8.2f, 11.2f, 9.2f, -4.8f, M(L_WOOD, 0x8a6a48), M(L_WOOD, 0x8a6a48));
        sbox(7.6f, 11.4f, -8.4f, 11.4f, 11.6f, -4.6f, M(L_SHINGLE, 0x3a4a3a), M(L_SHINGLE, 0x3a4a3a));
        for (int k = -1; k <= 1; k += 2) solid(10 + k * 0.4f - 0.08f, 0, 10 - 0.08f, 10 + k * 0.4f + 0.08f, 2.2f, 10 + 0.08f, M(L_WOOD, 0x6a4a30), M(L_WOOD, 0x6a4a30));
        sign_board(V3(10, 2.6f, 10), 4.4f, 0.8f, 6, 0);
        car_add(CAR_JEEP, V3(-9, 0, 5), PI * 0.5f, false);
        break;
    }
    case STOP_GARAGE: {
        lot_surface(-22, -14, 22, 16, asph);
        Mat out = M(L_METAL, 0xb8bcc0), in = M(L_CONCRETE, 0xa8a8a4);
        room(-9, -10, 9, 0, 5.0f, out, in, M(L_CONCRETE, 0x9a9a96), 0, 6.0f, false);
        flat_roof(-9, -10, 9, 0, 5.0f, out);
        sign_board(V3(0, 5.8f, -0.1f), 8, 1.2f, (d->name & 1) ? 5 : 12, 1.5f);
        furniture(CT_TOOLBOX, V3(-7.5f, 0, -9.2f), PI);
        furniture(CT_TOOLBOX, V3(-6.5f, 0, -9.2f), PI);
        furniture(CT_SHELF, V3(5.5f, 0, -9.0f), PI);
        furniture(CT_LOCKER, V3(7.8f, 0, -6), -PI * 0.5f);
        // tyre stack
        for (int k = 0; k < 4; k++) cyl(GEO_SOLID, 10.5f, k * 0.25f, 2, k * 0.25f + 0.24f, 0.36f, 10, M(L_PLAIN, 0x1a1a1a), M(L_PLAIN, 0x2a2a2a));
        add_box(10.1f, 0, 1.6f, 10.9f, 1, 2.4f);
        car_add(ri(10), V3(-3, 0, -5), PI * 0.5f, false);
        if (ri(2)) car_add(ri(8), V3(14, 0, 8), rf(0, 6.28f), false);
        lamp_add(V3(0, 4.5f, -5));
        break;
    }
    }
    if (d->kind != STOP_WRECKS) {
        frame_set(V3((float)(d->px - cax), d->y, (float)(d->pz - caz)), d->yaw);
        float zf = d->kind == STOP_TOWN ? 6 : d->kind == STOP_CAMP ? 10 : d->kind == STOP_FARM ? 22 : 12;
        float zr = fabsf(d->d) - ASPH + 0.4f;
        if (zr > zf) lot_surface(-3.5f, zf - 0.5f, 3.5f, zr, d->kind == STOP_FARM || d->kind == STOP_CAMP || d->kind == STOP_RANGER || d->kind == STOP_TRADER ? gravel : asph);
    }
    frame_reset();
}

// ---------------------------------------------------------------- segment geometry

static const float cols[] = { 0, 0.9f, 2.5f, 5, 8, 12, 17, 23, 31, 41, 54, 70, 90, 115, 145, 185, 235, 300, 380, 480, 600, 760 };
#define NCOL ((int)(sizeof cols / sizeof cols[0]))

static void build_terrain(int k)
{
    Buf *b = &bufs[GEO_GROUND];
    double s0 = k * SEG_LEN;
    float us = 1.0f / 6.0f;
    double ou = fmod(cax, 600.0), ov = fmod(caz, 600.0);
    enum { ROWS = (int)(SEG_LEN / 4) + 3 };
    static Vector3 P[ROWS][NCOL];
    static double PX[ROWS][NCOL], PZ[ROWS][NCOL];
    for (int side = -1; side <= 1; side += 2) {
        // one extra row each end, for normals
        for (int r = 0; r < ROWS; r++) {
            double s = s0 + (r - 1) * 4.0;
            double x, z; float h, th;
            line_at(s < 0 ? 0 : s, &x, &z, &h, &th);
            Vector3 l = left_of(th);
            for (int c = 0; c < NCOL; c++) {
                float d = side * (ASPH + cols[c]);
                double X = x + l.x * d, Z = z + l.z * d;
                PX[r][c] = X; PZ[r][c] = Z;
                P[r][c] = V3((float)(X - cax), height_sd(s < 0 ? 0 : s, d, X, Z), (float)(Z - caz));
            }
        }
        uint32_t base = (uint32_t)b->nv;
        for (int r = 1; r < ROWS - 1; r++) {
            double s = s0 + (r - 1) * 4.0;
            for (int c = 0; c < NCOL; c++) {
                Vector3 ds = Vector3Subtract(P[r + 1][c], P[r - 1][c]);
                Vector3 dd = Vector3Subtract(P[r][c < NCOL - 1 ? c + 1 : c], P[r][c > 0 ? c - 1 : c]);
                Vector3 n = Vector3Normalize(Vector3CrossProduct(ds, dd));
                if (n.y < 0) n = Vector3Negate(n);
                if (c == 0) n = V3(0, 1, 0);
                float d = side * (ASPH + cols[c]);
                Mat m = ground_mat(s, d);
                vert(b, P[r][c], n, (float)((PX[r][c] - cax + ou) * us), (float)((PZ[r][c] - caz + ov) * us), m);
            }
        }
        int rows = ROWS - 2;
        for (int r = 0; r + 1 < rows; r++)
            for (int c = 0; c + 1 < NCOL; c++) {
                uint32_t a = base + r * NCOL + c, bb = a + 1, cc = a + NCOL, dd = cc + 1;
                if (side > 0) { tri(b, a, cc, dd); tri(b, a, dd, bb); }
                else { tri(b, a, dd, cc); tri(b, a, bb, dd); }
            }
    }
}

static void build_road_surface(int k)
{
    Buf *b = &bufs[GEO_GROUND];
    double s0 = k * SEG_LEN;
    uint32_t base = (uint32_t)b->nv;
    int rows = (int)(SEG_LEN / 4) + 1;
    Mat asph = M(L_ASPHALT, 0xd4d4d4);
    for (int r = 0; r < rows; r++) {
        double s = s0 + r * 4.0;
        double x, z; float h, th;
        line_at(s, &x, &z, &h, &th);
        Vector3 l = left_of(th);
        for (int c = 0; c < 3; c++) {
            float d = (c - 1) * ASPH;
            vert(b, V3((float)(x + l.x * d - cax), h + 0.03f + (c == 1 ? 0.03f : 0), (float)(z + l.z * d - caz)), V3(0, 1, 0),
                 d / 8.0f, (float)((s - floor(s0 / 1280.0) * 1280.0) / 8.0), asph);
        }
    }
    for (int r = 0; r + 1 < rows; r++)
        for (int c = 0; c < 2; c++) {
            uint32_t a = base + r * 3 + c;
            tri(b, a, a + 3, a + 4); tri(b, a, a + 4, a + 1);
        }
    // gravel shoulders, laid over the terrain
    Mat grav = mf(M(L_GRAVEL, 0xd8d0c0), F_DECAL, 0);
    for (int side = -1; side <= 1; side += 2) {
        uint32_t gb = (uint32_t)b->nv;
        for (int r = 0; r < rows; r++) {
            double s = s0 + r * 4.0;
            double x, z; float h, th;
            line_at(s, &x, &z, &h, &th);
            Vector3 l = left_of(th);
            for (int c = 0; c < 2; c++) {
                float d = side * (ASPH - 0.05f + c * 1.35f);
                double X = x + l.x * d, Z = z + l.z * d;
                float y = c == 0 ? h + 0.03f : height_sd(s, d, X, Z) + 0.02f;
                vert(b, V3((float)(X - cax), y, (float)(Z - caz)), V3(0, 1, 0), (float)(d / 2.0), (float)((s - floor(s0 / 1280.0) * 1280.0) / 2.0), grav);
            }
        }
        for (int r = 0; r + 1 < rows; r++) { uint32_t a = gb + r * 2; tri(b, a, a + 2, a + 3); tri(b, a, a + 3, a + 1); }
    }
    // paint: a yellow centre dash, white edge lines
    Mat dash = mf(M(L_DASH, 0xffc828), F_CUT | F_DECAL, 0), edge = mf(M(L_EDGE, 0xffffff), F_CUT | F_DECAL, 0);
    for (int line = 0; line < 3; line++) {
        float d = line == 0 ? 0 : (line == 1 ? ASPH - 0.35f : -(ASPH - 0.35f)), hw = 0.08f;
        uint32_t lb = (uint32_t)b->nv;
        for (int r = 0; r < rows; r++) {
            double s = s0 + r * 4.0;
            double x, z; float h, th;
            line_at(s, &x, &z, &h, &th);
            Vector3 l = left_of(th);
            float y = h + 0.03f + (line == 0 ? 0.035f : 0.012f);
            float v = (float)(line == 0 ? (s - floor(s0 / 1152.0) * 1152.0) / 9.0 : (s - floor(s0 / 1280.0) * 1280.0) / 4.0);
            Mat m = line == 0 ? dash : edge;
            vert(b, V3((float)(x + l.x * (d - hw) - cax), y, (float)(z + l.z * (d - hw) - caz)), V3(0, 1, 0), 0, v, m);
            vert(b, V3((float)(x + l.x * (d + hw) - cax), y, (float)(z + l.z * (d + hw) - caz)), V3(0, 1, 0), 1, v, m);
        }
        for (int r = 0; r + 1 < rows; r++) { uint32_t a = lb + r * 2; tri(b, a, a + 2, a + 3); tri(b, a, a + 3, a + 1); }
    }
}

static Vector3 rel_at(double s, float d, float *th_out)
{
    double x, z; float h, th;
    line_at(s, &x, &z, &h, &th);
    Vector3 l = left_of(th);
    double X = x + l.x * d, Z = z + l.z * d;
    if (th_out) *th_out = th;
    return V3((float)(X - cax), height_sd(s, d, X, Z), (float)(Z - caz));
}

static bool near_lot(double X, double Z, int k, float pad)
{
    for (int kk = k - 1; kk <= k + 1; kk++) {
        if (kk < 0) continue;
        StopDesc *d = stop_desc(kk);
        if (d->kind == STOP_NONE) continue;
        if (d->R > 0 && hypot(X - d->px, Z - d->pz) < d->R + pad) return true;
        if (d->pond && hypot(X - d->wx, Z - d->wz) < d->wr + 3) return true;
    }
    return false;
}

static void build_district(int k)
{
    // Cross streets link the highway with the parallel streets on either side.
    float z = cross_street_z(k);
    Mat asphalt = M(L_ASPHALT, 0x909497);
    Mat paint = mf(M(L_PLAIN, 0xe2dcc3), F_DECAL, 0);
    Mat curb_mat = M(L_CURB, 0xb8b4ae);
    lot_surface(-136, z - 4, -ASPH, z + 4, asphalt);
    lot_surface(ASPH, z - 4, 136, z + 4, asphalt);
    for (int side = -1; side <= 1; side += 2) {
        float x = side * 112.0f;
        lot_surface(x - 4, 0, x + 4, SEG_LEN, asphalt);
        for (float lane_z = 3; lane_z < SEG_LEN - 3; lane_z += 9) {
            if (fabsf(lane_z - z) < 7) continue;
            hq(GEO_GROUND, x - 0.07f, lane_z, x + 0.07f, lane_z + 3.5f, 0.033f, paint);
        }
        float crossing = side * (ASPH + 2.6f);
        for (int stripe = -3; stripe <= 3; stripe++) {
            float sz = z + stripe * 0.87f;
            hq(GEO_GROUND, crossing - 1.0f, sz - 0.18f, crossing + 1.0f, sz + 0.18f, 0.033f, paint);
        }
        // Fill the open blocks with small parks and parking courts.
        for (int i = 0; i < 2; i++) {
            float bz = k % 2 == 0 ? 28 + i * 48 : 53 + i * 48;
            float bx = side * (i == 0 ? 19.0f : 82.0f);
            if (near_lot(cax + bx, caz + bz, k, 4)) continue;
            if ((i + k) % 2 == 0) {
                solid(bx - 4.9f, -0.08f, bz - 7, bx + 4.9f, 0.16f, bz + 7, curb_mat,
                      M(L_GRASS, 0x7e9b65));
                tree_broad(V3(bx, 0.16f, bz), rf(4.6f, 6.7f), 0);
                tree_broad(V3(bx, 0.16f, bz - 4.5f), rf(3.8f, 4.7f), 0);
            } else {
                lot_surface(bx - 6, bz - 9, bx + 6, bz + 9, asphalt);
                for (int mark = -3; mark <= 3; mark++) {
                    float zz = bz + mark * 2.7f;
                    hq(GEO_GROUND, bx - 5.3f, zz - 0.04f, bx - 1.2f, zz + 0.04f, 0.033f, paint);
                }
            }
        }
        for (int i = 0; i < 3; i++) {
            float bz = (k % 2 == 0 ? 20 : 40) + i * 30;
            float bx = side * rf(126, 132);
            float h = rf(12, 38), hw = rf(6, 9), hd = rf(7, 10);
            Mat wall = M(i == 0 ? L_BRICK : L_PANEL, i == 0 ? 0xb58069 : i == 1 ? 0x9da7b1 : 0xbeb4a1);
            solid(bx - hw, 0, bz - hd, bx + hw, h, bz + hd, wall, M(L_CONCRETE, 0x777c81));
            Mat glass = mf(M(L_PANEL, 0x253e4e), F_SELF, (k + i) % 3 == 0 ? 0.35f : 0.05f);
            for (float floor = 3; floor + 1.3f < h; floor += 3.1f) {
                for (int bay = -2; bay <= 2; bay++) {
                    float in = bx - side * (hw + 0.02f);
                    float wz = bz + bay * hd * 0.35f;
                    quadv(GEO_SOLID, V3(in, floor, wz - side * 0.8f),
                          V3(0, 0, side * 1.6f), V3(0, 1.3f, 0), 0, 0, 1, 1, glass);
                }
            }
        }
        float curb = side * (ASPH + 0.9f);
        lot_surface(curb - 0.75f, 0, curb + 0.75f, z - 4, M(L_SIDEWALK, 0xd2d0c8));
        lot_surface(curb - 0.75f, z + 4, curb + 0.75f, SEG_LEN, M(L_SIDEWALK, 0xd2d0c8));
        float lz = z + (k % 2 == 0 ? -7 : 7);
        solid(curb - 0.07f, 0, lz - 0.07f, curb + 0.07f, 5, lz + 0.07f, M_STEEL, M_STEEL);
        sbox(curb - 0.3f, 4.94f, lz - 0.16f, curb + 0.3f, 5.10f, lz + 0.16f, M_STEEL,
             mf(M(L_PLAIN, 0xffe4bd), F_SELF, 2.0f));
        lamp_add(V3(curb, 4.85f, lz));
    }
}

static void build_roadside(int k)
{
    double s0 = k * SEG_LEN;
    float mid = (float)(s0 + 64);
    // telephone poles on one side, wires to the next pole
    int pside = (k / 40) % 2 ? 1 : -1;
    for (int i = 0; i < 3; i++) {
        double s = s0 + i * 45.0 - fmod(s0, 45.0);
        if (s < s0) s += 45;
        if (s >= s0 + SEG_LEN) continue;
        float th;
        Vector3 p = rel_at(s, pside * 11.5f, &th);
        double X = p.x + cax, Z = p.z + caz;
        if (near_lot(X, Z, k, 14)) continue;
        pole(p, th);
        float th2;
        Vector3 q = rel_at(s + 45, pside * 11.5f, &th2);
        Mat wr = M(L_PLAIN, 0x202020);
        Vector3 ax = left_of(th), ax2 = left_of(th2);
        for (int w = -1; w <= 1; w += 2)
            wire(Vector3Add(p, V3(ax.x * w * 0.95f, 8.45f, ax.z * w * 0.95f)), Vector3Add(q, V3(ax2.x * w * 0.95f, 8.45f, ax2.z * w * 0.95f)), 0.6f, wr);
    }
    // reflector posts
    for (int i = 0; i < 3; i++) {
        double s = s0 + 20 + i * 42;
        for (int sd = -1; sd <= 1; sd += 2) {
            float th;
            Vector3 p = rel_at(s, sd * (ASPH + 1.3f), &th);
            if (near_lot(p.x + cax, p.z + caz, k, 0)) continue;
            Vector3 d = dir_of(th);
            sbox(p.x - 0.05f, p.y, p.z - 0.05f, p.x + 0.05f, p.y + 1.0f, p.z + 0.05f, M_WHITE, M_WHITE);
            Mat refl = mf(M(L_PLAIN, sd > 0 ? 0xffa020 : 0xff3020), F_SELF, 1.2f);
            quadv(GEO_SOLID, Vector3Add(p, V3(-d.x * 0.051f - 0.04f * d.z, 0.82f, -d.z * 0.051f + 0.04f * d.x)), V3(0.08f * d.z, 0, -0.08f * d.x), V3(0, 0.12f, 0), 0, 0, 1, 1, refl);
        }
    }
    // a km sign every 1024 m
    if (k % 8 == 0 && k > 0) {
        float th;
        Vector3 p = rel_at(s0 + 10, -(ASPH + 2.5f), &th);
        if (!near_lot(p.x + cax, p.z + caz, k, 2)) {
            frame_set(p, th + PI);
            sbox(-0.04f, 0, -0.04f, 0.04f, 1.2f, 0.04f, M_STEEL, M_STEEL);
            sbox(-0.3f, 1.2f, -0.03f, 0.3f, 1.7f, 0.03f, M(L_PLAIN, 0x1e6a3a), M(L_PLAIN, 0x1e6a3a));
            frame_reset();
        }
    }
    // land cover by biome
    int nb[BIOME_COUNT];
    for (int b = 0; b < BIOME_COUNT; b++) nb[b] = 0;
    float wsum[BIOME_COUNT];
    for (int b = 0; b < BIOME_COUNT; b++) wsum[b] = biome_mix(mid, b);
    int count = (int)(wsum[0] * 45 + wsum[1] * 40 + wsum[2] * 70 + wsum[3] * 40 + wsum[4] * 55);
    if (quality == 0) count = count * 2 / 3;
    for (int i = 0; i < count; i++) {
        double s = s0 + rf(0, (float)SEG_LEN);
        float r = rf(0, 1);
        float dist = ASPH + 6 + 320 * r * r;
        float d = (ri(2) ? 1 : -1) * dist;
        float th;
        Vector3 p = rel_at(s, d, &th);
        double X = p.x + cax, Z = p.z + caz;
        if (near_lot(X, Z, k, 6)) continue;
        // pick a biome by weight, then a plant
        float pick = rf(0, 1), acc = 0;
        int b = 0;
        for (; b < BIOME_COUNT - 1; b++) { acc += wsum[b]; if (pick < acc) break; }
        float u = rf(0, 1);
        switch (b) {
        case BIOME_DESERT:
            if (u < 0.28f) cactus(p, rf(2.5f, 5));
            else if (u < 0.8f) shrub(p, rf(0.5f, 1.1f), 0xa8b088);
            else if (u < 0.95f) rock(p, rf(0.4f, 1.6f), 0xe0c8a8);
            else dead_tree(p, rf(3, 5));
            break;
        case BIOME_PLAINS:
            if (u < 0.12f) tree_broad(p, rf(6, 10), 0.1f);
            else if (u < 0.85f) shrub(p, rf(0.5f, 1.0f), 0xc8d090);
            else rock(p, rf(0.3f, 1.0f), 0xc0c0b8);
            break;
        case BIOME_FOREST:
            if (u < 0.72f) pine(p, rf(10, 18), false);
            else if (u < 0.86f) tree_broad(p, rf(8, 12), 0);
            else if (u < 0.95f) shrub(p, rf(0.6f, 1.2f), 0x90b070);
            else rock(p, rf(0.5f, 1.5f), 0xa8a8a0);
            break;
        case BIOME_BADLANDS:
            if (u < 0.45f) rock(p, rf(0.6f, 3.2f), 0xd89070);
            else if (u < 0.85f) shrub(p, rf(0.4f, 0.9f), 0xa0a078);
            else dead_tree(p, rf(2.5f, 4.5f));
            break;
        default:
            if (u < 0.7f) pine(p, rf(9, 16), true);
            else rock(p, rf(0.6f, 2.0f), 0xd8d8e0);
            break;
        }
    }
    // the road begins at a barrier
    if (k == 0) {
        float th;
        Vector3 p = rel_at(30, 0, &th);
        frame_set(p, th);
        solid(-6, 0, -0.3f, 6, 1.1f, 0.3f, M(L_PLAIN, 0xd8d8d0), M(L_PLAIN, 0xd8d8d0));
        for (int i = 0; i < 6; i++) sbox(-6 + i * 2.0f, 0.4f, 0.3f, -5 + i * 2.0f, 0.8f, 0.31f, M(L_PLAIN, 0xd02020), M(L_PLAIN, 0xd02020));
        sign_board(V3(0, 2.4f, 0.4f), 4, 0.9f, 29, 0);
        frame_reset();
    }
}

static void build_pond(StopDesc *d)
{
    Buf *b = &bufs[GEO_WATER];
    int n = 24;
    float wy = d->y + WATER_DY;
    Vector3 c = V3((float)(d->wx - cax), wy, (float)(d->wz - caz));
    uint32_t ci = vert(b, c, V3(0, 1, 0), 0, 0, (Mat){ 0, 255, 255, 255, 0, 2.2f });
    for (int ring = 1; ring <= 2; ring++) {
        for (int i = 0; i <= n; i++) {
            float a = i * 6.2832f / n, r = (d->wr + 1.5f) * ring / 2.0f;
            Vector3 p = V3(c.x + cosf(a) * r, wy, c.z + sinf(a) * r);
            float dep = d->y + WATER_DY - height_abs(p.x + cax, p.z + caz);
            vert(b, p, V3(0, 1, 0), 0, 0, (Mat){ 0, 255, 255, 255, 0, fmaxf(dep, 0) });
        }
    }
    for (int i = 0; i < n; i++) {
        tri(b, ci, ci + 1 + i, ci + 2 + i);
        uint32_t a = ci + 1 + i, bb = a + n + 1;
        tri(b, a, bb, bb + 1); tri(b, a, bb + 1, a + 1);
    }
}

// ---------------------------------------------------------------- segments: build, upload, free

static void seg_free(Seg *g)
{
    if (!g->used) return;
    if (g->built) {
        glDeleteVertexArrays(GEO_PARTS, g->vao);
        glDeleteBuffers(GEO_PARTS, g->vbo);
        glDeleteBuffers(GEO_PARTS, g->ebo);
    }
    free(g->box); free(g->cell_start); free(g->cell_items);
    memset(g, 0, sizeof *g);
}

static void seg_grid(Seg *g)
{
    float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
    for (int i = 0; i < g->nbox; i++) {
        Box *b = &g->box[i];
        float r = b->hx + b->hz;
        x0 = fminf(x0, b->cx - r); z0 = fminf(z0, b->cz - r); x1 = fmaxf(x1, b->cx + r); z1 = fmaxf(z1, b->cz + r);
    }
    if (g->nbox == 0) { x0 = z0 = 0; x1 = z1 = 1; }
    g->gx0 = x0; g->gz0 = z0;
    g->gnx = (int)Clamp(ceilf((x1 - x0) / 16), 1, 64); g->gnz = (int)Clamp(ceilf((z1 - z0) / 16), 1, 64);
    int nc = g->gnx * g->gnz;
    int *cnt = calloc(nc + 1, sizeof(int));
    #define CELLS(b, BODY) { float r = (b)->hx + (b)->hz; \
        int cx0 = (int)Clamp(floorf(((b)->cx - r - x0) / 16), 0, g->gnx - 1), cx1 = (int)Clamp(floorf(((b)->cx + r - x0) / 16), 0, g->gnx - 1); \
        int cz0 = (int)Clamp(floorf(((b)->cz - r - z0) / 16), 0, g->gnz - 1), cz1 = (int)Clamp(floorf(((b)->cz + r - z0) / 16), 0, g->gnz - 1); \
        for (int cz = cz0; cz <= cz1; cz++) for (int cx = cx0; cx <= cx1; cx++) { int cell = cz * g->gnx + cx; BODY; } }
    for (int i = 0; i < g->nbox; i++) CELLS(&g->box[i], cnt[cell]++);
    g->cell_start = malloc((nc + 1) * sizeof(int));
    int tot = 0;
    for (int c = 0; c < nc; c++) { g->cell_start[c] = tot; tot += cnt[c]; cnt[c] = 0; }
    g->cell_start[nc] = tot;
    g->cell_items = malloc((tot + 1) * sizeof(int));
    for (int i = 0; i < g->nbox; i++) CELLS(&g->box[i], g->cell_items[g->cell_start[cell] + cnt[cell]++] = i);
    free(cnt);
    #undef CELLS
}

static void upload(Seg *g)
{
    g->mn = V3(1e9f, 1e9f, 1e9f); g->mx = V3(-1e9f, -1e9f, -1e9f);
    for (int part = 0; part < GEO_PARTS; part++) {
        Buf *b = &bufs[part];
        g->count[part] = b->ni;
        for (int k = 0; k < b->nv; k++) {
            const float *p = b->v[k].p;
            g->mn = V3(fminf(g->mn.x, p[0]), fminf(g->mn.y, p[1]), fminf(g->mn.z, p[2]));
            g->mx = V3(fmaxf(g->mx.x, p[0]), fmaxf(g->mx.y, p[1]), fmaxf(g->mx.z, p[2]));
        }
        glGenVertexArrays(1, &g->vao[part]);
        glGenBuffers(1, &g->vbo[part]);
        glGenBuffers(1, &g->ebo[part]);
        glBindVertexArray(g->vao[part]);
        glBindBuffer(GL_ARRAY_BUFFER, g->vbo[part]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(b->nv + 1) * (GLsizeiptr)sizeof(Vtx), b->v, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo[part]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(b->ni + 1) * 4, b->i, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)12);
        glVertexAttribPointer(2, 3, GL_BYTE, GL_TRUE, sizeof(Vtx), (void *)20);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void *)24);
        glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)28);
        for (int a = 0; a < 6; a++) if (a != 4) glEnableVertexAttribArray(a);
        glBindVertexArray(0);
        b->nv = b->ni = 0;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g->box = malloc((nbx + 1) * sizeof(Box));
    memcpy(g->box, bx, nbx * sizeof(Box));
    g->nbox = nbx;
    nbx = 0;
    seg_grid(g);
    g->built = true;
}

static Seg *seg_find(int k)
{
    for (int i = 0; i < NSEG; i++) if (segs[i].used && segs[i].k == k) return &segs[i];
    return NULL;
}

static void seg_build(int k)
{
    Seg *g = NULL;
    for (int i = 0; i < NSEG; i++) if (!segs[i].used) { g = &segs[i]; break; }
    if (!g) return;
    memset(g, 0, sizeof *g);
    g->used = true; g->k = k; g->stop_slot = -1;
    double x, z; float h, th;
    line_at(k * SEG_LEN, &x, &z, &h, &th);
    g->ax = x; g->az = z;
    cur = g; cax = x; caz = z;
    brng = h3(k, 31, 7) | 1;
    frame_reset();
    build_terrain(k);
    build_road_surface(k);
    if (world_kind == WORLD_CITY) build_district(k);
    else build_roadside(k);
    StopDesc *d = stop_desc(k);
    if (d->kind != STOP_NONE) {
        stop_build(d);
        if (d->pond) build_pond(d);
    }
    // loose wrecks along the shoulders
    brng = h3(k, 41, 9) | 1;
    if (k > START_K + 1 && d->kind == STOP_NONE && rf(0, 1) < 0.24f) {
        int n = 1 + (rf(0, 1) < 0.3f);
        for (int i = 0; i < n; i++) {
            double s = k * SEG_LEN + rf(10, 118);
            float off = (rf(0, 1) < 0.5f ? 1 : -1) * rf(ASPH + 0.5f, ASPH + 9);
            float th2;
            Vector3 p = rel_at(s, off, &th2);
            car_add(ri(CAR_RAIDER), p, th2 + rf(-0.6f, 0.6f) + (rf(0, 1) < 0.5f ? PI : 0), rf(0, 1) < 0.35f);
            if (rf(0, 1) < 0.4f) { fr_o = V3(0, 0, 0); cont_add(CT_BAG, Vector3Add(p, V3(rf(-3, 3), 0, rf(-3, 3))), 0); }
        }
    }
    upload(g);
    cur = NULL;
}

// ---------------------------------------------------------------- activation: stops, containers, wrecks become live

static uint32_t *flags; static int nflags, cflags;   // sorted keys of things already used up

bool world_flag(uint32_t key)
{
    int lo = 0, hi = nflags;
    while (lo < hi) { int m = (lo + hi) / 2; if (flags[m] < key) lo = m + 1; else hi = m; }
    return lo < nflags && flags[lo] == key;
}

void world_set_flag(uint32_t key)
{
    if (world_flag(key)) return;
    if (nflags == cflags) { cflags = cflags ? cflags * 2 : 256; flags = realloc(flags, cflags * 4); }
    int i = nflags++;
    while (i > 0 && flags[i - 1] > key) { flags[i] = flags[i - 1]; i--; }
    flags[i] = key;
}

int world_flags_get(uint32_t **out) { *out = flags; return nflags; }
void world_flags_clear(void) { nflags = 0; }

uint32_t world_key(int k, int kind, int slot) { return ((uint32_t)k << 12) | ((uint32_t)(kind & 15) << 8) | (uint32_t)(slot & 255); }

static void seg_activate(Seg *g)
{
    g->live = true;
    StopDesc *d = stop_desc(g->k);
    if (d->kind != STOP_NONE) {
        for (int i = 0; i < MAX_STOPS; i++)
            if (!stops[i].used) {
                Stop *st = &stops[i];
                memset(st, 0, sizeof *st);
                st->used = true; st->kind = d->kind; st->biome = d->biome; st->seg = g->k;
                st->p = V3((float)(d->px - world_ox), d->y, (float)(d->pz - world_oz));
                st->yaw = d->yaw; st->side = (float)d->side;
                st->km = (float)((d->s - START_S) / 1000.0);
                stop_name(d, st->name, sizeof st->name);
                st->visited = world_flag(world_key(g->k, 15, 0));
                g->stop_slot = i;
                break;
            }
    }
    for (int c = 0; c < g->ncont; c++) {
        ContDef *cd = &g->cont[c];
        for (int i = 0; i < MAX_CONTS; i++)
            if (!conts[i].used) {
                Cont *ct = &conts[i];
                ct->used = true; ct->kind = cd->kind; ct->stop = g->stop_slot; ct->yaw = cd->yaw;
                ct->p = V3((float)(cd->p.x + g->ax - world_ox), cd->p.y, (float)(cd->p.z + g->az - world_oz));
                ct->p.y = fmaxf(ct->p.y, ground_y(ct->p.x, ct->p.z));
                ct->key = world_key(g->k, 1, cd->slot);
                ct->searched = world_flag(ct->key);
                ct->car = -1;
                break;
            }
    }
    for (int c = 0; c < g->ncar; c++) {
        CarDef *cd = &g->car[c];
        uint32_t key = world_key(g->k, 2, cd->slot);
        if (world_flag(world_key(g->k, 4, cd->slot))) continue;   // taken away by the player
        Vector3 p = V3((float)(cd->p.x + g->ax - world_ox), 0, (float)(cd->p.z + g->az - world_oz));
        p.y = ground_y(p.x, p.z) + 0.05f;
        int ci = car_spawn(cd->model, p, cd->yaw, cd->burnt ? AI_WRECK : AI_PARKED);
        if (ci < 0) continue;
        Car *car = &cars[ci];
        car->key = key;
        uint32_t h = h3(g->k, 51, cd->slot);
        // abandoned cars: low on everything, some fixable
        car->fuel = world_flag(world_key(g->k, 3, cd->slot)) ? 0 : (h & 255) / 255.0f * car->fuel_max * 0.35f;
        car->engine = 10 + ((h >> 8) & 63);
        car->battery = ((h >> 14) & 3) ? 20 + ((h >> 16) & 63) : 0;
        for (int w = 0; w < 4; w++) car->tire[w] = ((h >> (22 + w)) & 1) ? 0 : 30 + ((h >> (18 + w * 2)) & 63);
        if (world_flag(world_key(g->k, 5, cd->slot))) { car->engine = 0; car->battery = 0; for (int w = 0; w < 4; w++) car->tire[w] = 0; }
        if (cd->burnt) {
            car->health = 0; car->paint = (Color){ 30, 28, 26, 255 }; car->fuel = 0; car->engine = 0; car->battery = 0;
            for (int w = 0; w < 4; w++) car->tire[w] = 0;
        }
        // a trunk to search
        for (int i = 0; i < MAX_CONTS; i++)
            if (!conts[i].used) {
                Cont *ct = &conts[i];
                ct->used = true; ct->kind = CT_TRUNK; ct->stop = g->stop_slot; ct->car = ci;
                ct->p = p; ct->key = world_key(g->k, 6, cd->slot); ct->searched = world_flag(ct->key);
                break;
            }
    }
}

static void seg_deactivate(Seg *g)
{
    g->live = false;
    if (g->stop_slot >= 0) stops[g->stop_slot].used = false;
    for (int i = 0; i < MAX_CONTS; i++) {
        Cont *ct = &conts[i];
        if (!ct->used) continue;
        if ((ct->key >> 12) != (uint32_t)g->k) continue;
        ct->used = false;
    }
    for (int i = 0; i < MAX_CARS; i++) {
        Car *c = &cars[i];
        if (!c->used || !c->key || (c->key >> 12) != (uint32_t)g->k) continue;
        if (c->owned || c->driver == PLAYER || i == sv.home_car) continue;
        car_remove(i);
    }
    g->stop_slot = -1;
}

// ---------------------------------------------------------------- lanes

static int lane_k[LANE_RING];
static bool lanes_init;

static void lanes_fill(int k)
{
    int slot = ((k % LANE_RING) + LANE_RING) % LANE_RING;
    lane_k[slot] = k;
    for (int dir = 0; dir < 2; dir++)
        for (int j = 0; j < 8; j++) {
            Lane *l = &lanes[slot * 16 + dir * 8 + j];
            double s0 = k * SEG_LEN + j * 16.0, s1 = s0 + 16;
            float off = dir == 0 ? -LANE_OFF : LANE_OFF;
            Vector3 a = road_at((float)s0, NULL), b = road_at((float)s1, NULL);
            Vector3 da, db;
            road_at((float)s0, &da); road_at((float)s1, &db);
            float tha = atan2f(da.x, da.z), thb = atan2f(db.x, db.z);
            Vector3 la = left_of(tha), lb = left_of(thb);
            a = Vector3Add(a, Vector3Scale(la, off)); b = Vector3Add(b, Vector3Scale(lb, off));
            if (dir == 0) { l->a = a; l->b = b; } else { l->a = b; l->b = a; }
            l->len = Vector3Distance(l->a, l->b);
            l->speed = 24;
            l->light = -1;
            l->nnext = 1;
            int nk = k, nj = j;
            if (dir == 0) { nj = j + 1; if (nj == 8) { nj = 0; nk = k + 1; } }
            else { nj = j - 1; if (nj < 0) { nj = 7; nk = k - 1; } }
            if (nk < 0) { l->nnext = 0; continue; }
            int ns = ((nk % LANE_RING) + LANE_RING) % LANE_RING;
            l->next[0] = ns * 16 + dir * 8 + nj;
        }
}

static void lanes_update(int kp)
{
    if (!lanes_init) { for (int i = 0; i < LANE_RING; i++) lane_k[i] = -999999; lanes_init = true; }
    for (int k = kp - 50; k <= kp + 50; k++) {
        if (k < 0) continue;
        int slot = ((k % LANE_RING) + LANE_RING) % LANE_RING;
        if (lane_k[slot] != k) lanes_fill(k);
    }
}

int nearest_lane(Vector3 p, float maxd)
{
    float s;
    road_offset(p.x, p.z, &s);
    int k = (int)floorf(s / (float)SEG_LEN);
    if (k < 0) return -1;
    int slot = ((k % LANE_RING) + LANE_RING) % LANE_RING;
    if (lane_k[slot] != k) return -1;
    int j = (int)Clamp(floorf((s - k * (float)SEG_LEN) / 16), 0, 7);
    int best = -1;
    float bd = maxd;
    for (int dir = 0; dir < 2; dir++) {
        int li = slot * 16 + dir * 8 + j;
        Lane *l = &lanes[li];
        Vector3 ab = Vector3Subtract(l->b, l->a), ap = Vector3Subtract(p, l->a);
        float t = Clamp((ab.x * ap.x + ab.z * ap.z) / fmaxf(ab.x * ab.x + ab.z * ab.z, 1e-4f), 0, 1);
        float dx = p.x - (l->a.x + ab.x * t), dz = p.z - (l->a.z + ab.z * t), d = sqrtf(dx * dx + dz * dz);
        if (d < bd) { bd = d; best = li; }
    }
    return best;
}

int nearest_walk(Vector3 p, float maxd) { return -1; }
int light_state(int light) { return 0; }
bool walk_light_ok(int light) { return true; }

// ---------------------------------------------------------------- collision queries

static void seg_off(const Seg *g, float *ox, float *oz) { *ox = (float)(g->ax - world_ox); *oz = (float)(g->az - world_oz); }

typedef void (*BoxVisit)(const Box *b, float ox, float oz, void *ud);

static void boxes_near(Vector3 c, float r, BoxVisit fn, void *ud)
{
    float s;
    road_offset(c.x, c.z, &s);
    int k = (int)floorf(s / (float)SEG_LEN);
    for (int kk = k - 1; kk <= k + 1; kk++) {
        Seg *g = seg_find(kk);
        if (!g || !g->built) continue;
        float ox, oz;
        seg_off(g, &ox, &oz);
        float qx = c.x - ox, qz = c.z - oz;
        int cx0 = (int)floorf((qx - r - g->gx0) / 16), cx1 = (int)floorf((qx + r - g->gx0) / 16);
        int cz0 = (int)floorf((qz - r - g->gz0) / 16), cz1 = (int)floorf((qz + r - g->gz0) / 16);
        if (cx1 < 0 || cz1 < 0 || cx0 >= g->gnx || cz0 >= g->gnz) continue;
        cx0 = cx0 < 0 ? 0 : cx0; cz0 = cz0 < 0 ? 0 : cz0;
        cx1 = cx1 >= g->gnx ? g->gnx - 1 : cx1; cz1 = cz1 >= g->gnz ? g->gnz - 1 : cz1;
        static int stamp, mark[4096];
        stamp++;
        for (int cz = cz0; cz <= cz1; cz++)
            for (int cx = cx0; cx <= cx1; cx++) {
                int cell = cz * g->gnx + cx;
                for (int q = g->cell_start[cell]; q < g->cell_start[cell + 1]; q++) {
                    int id = g->cell_items[q];
                    if (id < 4096) { if (mark[id] == stamp) continue; mark[id] = stamp; }
                    fn(&g->box[id], ox, oz, ud);
                }
            }
    }
}

typedef struct { Vector3 *p; float r, h; bool hit; } PushQ;

static void push_visit(const Box *b, float ox, float oz, void *ud)
{
    PushQ *q = ud;
    Vector3 *p = q->p;
    if (b->y1 < p->y + 0.3f || b->y0 > p->y + q->h) return;
    float dx = p->x - (b->cx + ox), dz = p->z - (b->cz + oz);
    // into box space: inverse of the frame rotation
    float lx = dx * b->c - dz * b->s, lz = dx * b->s + dz * b->c;
    float qx = Clamp(lx, -b->hx, b->hx), qz = Clamp(lz, -b->hz, b->hz);
    float ex = lx - qx, ez = lz - qz, d2 = ex * ex + ez * ez;
    if (d2 >= q->r * q->r) return;
    if (d2 > 1e-8f) {
        float d = sqrtf(d2), k = (q->r - d) / d;
        lx += ex * k; lz += ez * k;
    } else {
        float l = lx + b->hx, r = b->hx - lx, t = lz + b->hz, bt = b->hz - lz;
        float m = fminf(fminf(l, r), fminf(t, bt));
        if (m == l) lx = -b->hx - q->r; else if (m == r) lx = b->hx + q->r;
        else if (m == t) lz = -b->hz - q->r; else lz = b->hz + q->r;
    }
    p->x = b->cx + ox + lx * b->c + lz * b->s;
    p->z = b->cz + oz - lx * b->s + lz * b->c;
    q->hit = true;
}

bool world_push(Vector3 *p, float radius, float height)
{
    PushQ q = { p, radius, height, false };
    for (int it = 0; it < 2; it++) boxes_near(*p, radius + 1, push_visit, &q);
    return q.hit;
}

typedef struct { WBox *out; int n, max; Vector3 c; float r; } BoxQ;

static void obox_visit(const Box *b, float ox, float oz, void *ud)
{
    BoxQ *q = ud;
    if (q->n >= q->max) return;
    float dx = q->c.x - (b->cx + ox), dz = q->c.z - (b->cz + oz);
    float rr = q->r + b->hx + b->hz;
    if (dx * dx + dz * dz > rr * rr) return;
    WBox *w = &q->out[q->n++];
    w->c = V3(b->cx + ox, (b->y0 + b->y1) * 0.5f, b->cz + oz);
    w->hx = b->hx; w->hz = b->hz; w->y0 = b->y0; w->y1 = b->y1;
    w->ax = V3(b->c, 0, -b->s); w->az = V3(b->s, 0, b->c);
}

int world_obox(Vector3 c, float r, WBox *out, int max)
{
    BoxQ q = { out, 0, max, c, r };
    boxes_near(c, r, obox_visit, &q);
    return q.n;
}

int world_boxes(Vector3 c, float r, BoundingBox *out, int max)
{
    WBox tmp[64];
    int n = world_obox(c, r, tmp, max < 64 ? max : 64);
    for (int i = 0; i < n; i++) {
        float ex = fabsf(tmp[i].ax.x) * tmp[i].hx + fabsf(tmp[i].az.x) * tmp[i].hz;
        float ez = fabsf(tmp[i].ax.z) * tmp[i].hx + fabsf(tmp[i].az.z) * tmp[i].hz;
        out[i] = (BoundingBox){ { tmp[i].c.x - ex, tmp[i].y0, tmp[i].c.z - ez }, { tmp[i].c.x + ex, tmp[i].y1, tmp[i].c.z + ez } };
    }
    return n;
}

typedef struct { Vector3 o, d; float best; Vector3 n; bool found; } RayQ;

static void ray_visit(const Box *b, float ox, float oz, void *ud)
{
    RayQ *q = ud;
    float dx = q->o.x - (b->cx + ox), dz = q->o.z - (b->cz + oz);
    float lo[3] = { -b->hx, b->y0, -b->hz }, hi[3] = { b->hx, b->y1, b->hz };
    float oo[3] = { dx * b->c - dz * b->s, q->o.y, dx * b->s + dz * b->c };
    float dd[3] = { q->d.x * b->c - q->d.z * b->s, q->d.y, q->d.x * b->s + q->d.z * b->c };
    float t0 = 0, t1 = q->best;
    int axis = -1, sgn = 0;
    for (int a = 0; a < 3; a++) {
        if (fabsf(dd[a]) < 1e-9f) { if (oo[a] < lo[a] || oo[a] > hi[a]) return; continue; }
        float ta = (lo[a] - oo[a]) / dd[a], tb = (hi[a] - oo[a]) / dd[a];
        int sg = -1;
        if (ta > tb) { float t = ta; ta = tb; tb = t; sg = 1; }
        if (ta > t0) { t0 = ta; axis = a; sgn = sg; }
        if (tb < t1) t1 = tb;
        if (t0 > t1) return;
    }
    if (axis < 0 || t0 >= q->best) return;
    q->best = t0;
    float nl[3] = { 0, 0, 0 };
    nl[axis] = (float)sgn;
    q->n = V3(nl[0] * b->c + nl[2] * b->s, nl[1], -nl[0] * b->s + nl[2] * b->c);
    q->found = true;
}

bool world_ray(Vector3 o, Vector3 dir, float maxd, Hit *h)
{
    RayQ q = { o, dir, maxd, V3(0, 1, 0), false };
    // terrain: march, then refine
    float t = 0, step = 0.75f, prev_t = 0;
    while (t < q.best) {
        t = fminf(t + step, q.best);
        Vector3 p = Vector3Add(o, Vector3Scale(dir, t));
        if (p.y < ground_y(p.x, p.z)) {
            float a = prev_t, b = t;
            for (int i = 0; i < 6; i++) {
                float m = (a + b) * 0.5f;
                Vector3 pm = Vector3Add(o, Vector3Scale(dir, m));
                if (pm.y < ground_y(pm.x, pm.z)) b = m; else a = m;
            }
            q.best = b;
            Vector3 ph2 = Vector3Add(o, Vector3Scale(dir, b));
            float e = 0.5f;
            float hx = ground_y(ph2.x + e, ph2.z) - ground_y(ph2.x - e, ph2.z), hz = ground_y(ph2.x, ph2.z + e) - ground_y(ph2.x, ph2.z - e);
            q.n = Vector3Normalize(V3(-hx, 2 * e, -hz));
            q.found = true;
            break;
        }
        prev_t = t;
        step = fminf(step * 1.25f, 4.0f);
    }
    // boxes along the ray, sampled every 20 m
    for (float s = 0; s <= q.best + 10; s += 20) {
        Vector3 c = Vector3Add(o, Vector3Scale(dir, fminf(s + 10, q.best)));
        boxes_near(c, 15, ray_visit, &q);
    }
    if (q.found && h) { h->t = q.best; h->p = Vector3Add(o, Vector3Scale(dir, q.best)); h->n = q.n; h->kind = HIT_WORLD; h->id = 0; }
    return q.found;
}

// ---------------------------------------------------------------- per tick: streaming, weather

int stop_near(Vector3 p, float r)
{
    int best = -1;
    float bd = r;
    for (int i = 0; i < MAX_STOPS; i++) {
        if (!stops[i].used) continue;
        float d = Vector3Distance(p, stops[i].p) - stop_R[stops[i].kind] * 0.6f;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

int cont_near(Vector3 p, float r)
{
    int best = -1;
    float bd = r;
    for (int i = 0; i < MAX_CONTS; i++) {
        if (!conts[i].used) continue;
        Vector3 cp = conts[i].p;
        if (conts[i].kind == CT_TRUNK && conts[i].car >= 0 && cars[conts[i].car].used) {
            Car *c = &cars[conts[i].car];
            cp = Vector3Subtract(c->pos, Vector3Scale(car_fwd(c), car_models[c->model].size.z * 0.5f + 0.3f));
        }
        float dy = fabsf(p.y - cp.y);
        if (dy > 2.2f) continue;
        float d = hypotf(p.x - cp.x, p.z - cp.z);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static int focus_k(void)
{
    Vector3 f = peds[PLAYER].car >= 0 ? cars[peds[PLAYER].car].pos : peds[PLAYER].pos;
    return (int)floorf(road_s(f) / (float)SEG_LEN);
}

static void stream(bool all)
{
    int kp = focus_k();
    int lo = kp - 3, hi = kp + (quality == 0 ? 9 : 12);
    // free far segments
    for (int i = 0; i < NSEG; i++) {
        Seg *g = &segs[i];
        if (!g->used) continue;
        if (g->k < lo - 2 || g->k > hi + 2) { if (g->live) seg_deactivate(g); seg_free(g); }
    }
    // build the nearest missing ones
    int built = 0;
    for (int dk = 0; dk <= hi - kp + 3; dk++) {
        for (int sgn = 0; sgn < 2; sgn++) {
            int k = sgn ? kp - dk : kp + dk;
            if (k < 0 || k < lo || k > hi || (sgn && dk == 0)) continue;
            if (seg_find(k)) continue;
            seg_build(k);
            if (!all && ++built >= 1) goto done;
        }
    }
done:
    for (int i = 0; i < NSEG; i++) {
        Seg *g = &segs[i];
        if (!g->used || !g->built) continue;
        bool want = g->k >= kp - 3 && g->k <= kp + 6;
        if (want && !g->live) seg_activate(g);
        else if (!want && g->live && (g->k < kp - 4 || g->k > kp + 8)) seg_deactivate(g);
    }
    lanes_update(kp);
}

static float wt_rain, wt_fog, wt_dust, wt_snow, wt_wind, weather_t;

static void weather_pick(void)
{
    float s = road_s(peds[PLAYER].pos);
    int b = biome_at(s);
    float u = frand(0, 1);
    wt_rain = wt_fog = wt_dust = wt_snow = 0;
    wt_wind = frand(0.1f, 0.4f);
    switch (b) {
    case BIOME_DESERT: if (u < 0.25f) { wt_dust = frand(0.5f, 1); wt_wind = 0.9f; } else if (u < 0.3f) wt_rain = 0.5f; break;
    case BIOME_PLAINS: if (u < 0.35f) wt_rain = frand(0.4f, 1); else if (u < 0.5f) wt_fog = frand(0.4f, 0.9f); break;
    case BIOME_FOREST: if (u < 0.4f) wt_rain = frand(0.4f, 1); else if (u < 0.6f) wt_fog = frand(0.5f, 1); break;
    case BIOME_BADLANDS: if (u < 0.3f) { wt_dust = frand(0.4f, 1); wt_wind = 0.8f; } else if (u < 0.38f) wt_rain = 0.6f; break;
    default: if (u < 0.55f) { wt_snow = frand(0.4f, 1); wt_wind = 0.6f; } else if (u < 0.7f) wt_fog = 0.7f; break;
    }
    if (wt_rain > 0.7f) wt_wind = fmaxf(wt_wind, 0.6f);
    weather_t = frand(240, 600);
}

void world_tick(float dt)
{
    stream(false);
    weather_t -= dt;
    if (weather_t <= 0) weather_pick();
    float k = fminf(1, dt * 0.05f);
    weather_rain += (wt_rain - weather_rain) * k;
    weather_fog += (wt_fog - weather_fog) * k;
    weather_dust += (wt_dust - weather_dust) * k;
    weather_snow += (wt_snow - weather_snow) * k;
    weather_wind += (wt_wind - weather_wind) * k;
    // snow and rain don't fall in the wrong place
    float s = road_s(peds[PLAYER].pos);
    float snowy = biome_mix(s, BIOME_SNOW);
    if (snowy < 0.3f && weather_snow > 0.01f) { weather_rain = fmaxf(weather_rain, weather_snow); weather_snow *= 0.98f; }
    // keep stop positions in step after a rebase
    for (int i = 0; i < NSEG; i++) {
        Seg *g = &segs[i];
        if (!g->used || !g->live || g->stop_slot < 0) continue;
        StopDesc *d = stop_desc(g->k);
        stops[g->stop_slot].p = V3((float)(d->px - world_ox), d->y, (float)(d->pz - world_oz));
    }
}

float world_temp(void)
{
    float s = road_s(peds[PLAYER].pos);
    static const float day_t[BIOME_COUNT] = { 33, 24, 17, 30, 2 }, night_t[BIOME_COUNT] = { 9, 10, 5, 8, -12 };
    float dt = 0, nt = 0;
    for (int b = 0; b < BIOME_COUNT; b++) { float w = biome_mix(s, b); dt += w * day_t[b]; nt += w * night_t[b]; }
    float h = day_hours;
    float day = sm(5, 9, h) * (1 - sm(17, 22, h));
    float t = mixf(nt, dt, day);
    t -= weather_rain * 6 + weather_snow * 5 + weather_wind * 3 + weather_fog * 2;
    float y = peds[PLAYER].pos.y;
    t -= fmaxf(0, y) * 0.0065f;
    return t;
}

void world_rebase(Vector3 d)
{
    world_ox += d.x; world_oz += d.z;
    for (int i = 0; i < MAX_LANES; i++) { lanes[i].a = Vector3Subtract(lanes[i].a, d); lanes[i].b = Vector3Subtract(lanes[i].b, d); }
    for (int i = 0; i < MAX_STOPS; i++) if (stops[i].used) stops[i].p = Vector3Subtract(stops[i].p, d);
    for (int i = 0; i < MAX_CONTS; i++) if (conts[i].used) conts[i].p = Vector3Subtract(conts[i].p, d);
    cars_shift(d);
    peds_shift(d);
    fx_shift(d);
    combat_shift(d);
    raiders_shift(d);
    camera_shift(d);
    if (sv.camp_on) sv.camp_p = Vector3Subtract(sv.camp_p, d);
}

// ---------------------------------------------------------------- drawing

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

static bool box_visible(Vector3 mn, Vector3 mx)
{
    if (mn.x > mx.x) return false;
    for (int i = 0; i < 6; i++) {
        float px = planes[i][0] > 0 ? mx.x : mn.x, py = planes[i][1] > 0 ? mx.y : mn.y, pz = planes[i][2] > 0 ? mx.z : mn.z;
        if (planes[i][0] * px + planes[i][1] * py + planes[i][2] * pz + planes[i][3] < 0) return false;
    }
    if (cull_r < 1e8f) {
        float dx = fmaxf(fmaxf(mn.x - cull_c.x, cull_c.x - mx.x), 0), dz = fmaxf(fmaxf(mn.z - cull_c.z, cull_c.z - mx.z), 0);
        if (dx * dx + dz * dz > cull_r * cull_r) return false;
    }
    return true;
}

void world_draw_part(int part)
{
    for (int i = 0; i < NSEG; i++) {
        Seg *g = &segs[i];
        if (!g->used || !g->built || !g->count[part]) continue;
        float ox, oz;
        seg_off(g, &ox, &oz);
        Vector3 off = V3(ox, 0, oz);
        if (!box_visible(Vector3Add(g->mn, off), Vector3Add(g->mx, off))) continue;
        r_set_offset(off);
        glBindVertexArray(g->vao[part]);
        glDrawElements(GL_TRIANGLES, g->count[part], GL_UNSIGNED_INT, 0);
    }
    glBindVertexArray(0);
    r_set_offset(V3(0, 0, 0));
}

void world_draw(int pass)
{
    world_draw_part(GEO_GROUND);
    world_draw_part(GEO_SOLID);
    if (pass == PASS_MAIN) { world_draw_part(GEO_WATER); world_draw_part(GEO_GLASS); }
}

void world_frame(void) {}

int world_lamps(Vector3 near, Vector4 *out, int max)
{
    float dist[32];
    int n = 0;
    for (int i = 0; i < NSEG; i++) {
        Seg *g = &segs[i];
        if (!g->used || !g->built) continue;
        float ox, oz;
        seg_off(g, &ox, &oz);
        for (int j = 0; j < g->nlamp; j++) {
            Vector3 lp = V3(g->lamp[j].x + ox, g->lamp[j].y, g->lamp[j].z + oz);
            float dx = lp.x - near.x, dz = lp.z - near.z, d = dx * dx + dz * dz;
            if (d > 140.0f * 140.0f) continue;
            if (n == max && d >= dist[max - 1]) continue;
            int k = n < max ? n++ : max - 1;
            while (k > 0 && dist[k - 1] > d) { dist[k] = dist[k - 1]; out[k] = out[k - 1]; k--; }
            dist[k] = d;
            out[k] = (Vector4){ lp.x, lp.y - 0.3f, lp.z, 18.0f };
        }
    }
    return n;
}

// ---------------------------------------------------------------- build

float world_start_s(void) { return (float)START_S; }

void world_build(uint32_t seed)
{
    static bool tex_done;
    wseed = seed * 2654435761u + 0x51ed27u;
    for (int i = 0; i < 8; i++) ph[i] = hu(i, 3, 99) * 6.2832f;
    nrt = 0;
    last_k = 0;
    for (int i = 0; i < NSEG; i++) seg_free(&segs[i]);
    memset(stops, 0, sizeof stops);
    memset(conts, 0, sizeof conts);
    lanes_init = false;
    world_ox = world_oz = 0;
    sd_init = false; rb_init = false;
    M_STEEL = M(L_METAL, 0x9aa0a6); M_BLACK = M(L_PLAIN, 0x202224); M_DARK = M(L_PLAIN, 0x3a3e42); M_WHITE = M(L_PLAIN, 0xf0f0ec);
    if (!tex_done) { texture_build(quality == 0 ? 256 : 512); tex_done = true; }
    road_ensure(1024);
    weather_rain = weather_fog = weather_dust = weather_snow = 0;
    wt_rain = wt_fog = wt_dust = wt_snow = 0;
    wt_wind = weather_wind = 0.2f;
    if (world_kind == WORLD_SNOW) wt_snow = weather_snow = 0.6f;
    if (world_kind == WORLD_FOREST) wt_fog = weather_fog = 0.27f;
    weather_t = 300;
}

void world_stream_all(void) { stream(true); }

void world_reload_active(void)
{
    for (int i = 0; i < NSEG; i++) if (segs[i].live) seg_deactivate(&segs[i]);
    stream(true);
}

void weather_force(const char *w)
{
    wt_rain = wt_fog = wt_dust = wt_snow = 0;
    if (!strcmp(w, "rain")) wt_rain = 1;
    else if (!strcmp(w, "fog")) wt_fog = 1;
    else if (!strcmp(w, "dust")) wt_dust = 1;
    else if (!strcmp(w, "snow")) wt_snow = 1;
    weather_rain = wt_rain; weather_fog = wt_fog; weather_dust = wt_dust; weather_snow = wt_snow;
    weather_wind = wt_dust > 0 ? 0.9f : 0.5f; wt_wind = weather_wind;
    weather_t = 1e9f;
}
