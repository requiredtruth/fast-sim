#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ped.h"
#include "city.h"
#include "combat.h"

const Weapon weapons[W_COUNT] = {
    [W_FIST]    = { "Fists",   12, 2.4f,  0,      1.7f, 0,  1, 0 },
    [W_PISTOL]  = { "Pistol",  25, 3.8f,  0.012f, 90,   12, 1, 1.5f },
    [W_SMG]     = { "SMG",     14, 11,    0.04f,  80,   30, 1, 1.9f },
    [W_SHOTGUN] = { "Shotgun", 14, 1.15f, 0.075f, 45,   6,  8, 2.8f },
};

int cash;
float shot_age = 99;
float mode_since;
bool map_held;
int aim_target = -1;
bool aim_live;
Vector3 aim_from, aim_dir;

#define CAR_DMG_MUL 5.0f
#define COP_DMG_MUL 0.38f
#define PICK_RESPAWN_S 120
#define ROB_COOLDOWN_S 300

static bool ready;
static bool owned[W_COUNT];
static int stored_clip[W_COUNT], wanted_weapon;
float weapon_reach;
static float alarm_cd, shots_cd;
static float punch_t = -1, combo_s;
static int punch_n, shot_count;
static bool head_hit;

static Vector3 v3(float x, float y, float z) { return (Vector3){ x, y, z }; }
static float dist_xz(Vector3 a, Vector3 b) { return hypotf(a.x - b.x, a.z - b.z); }

static float angle_diff(float from, float to)
{
    float d = fmodf(to - from + PI, 2 * PI);
    if (d < 0) d += 2 * PI;
    return d - PI;
}

static Vector3 chest_of(int i)
{
    if (pv[i].rag_on) return peds[i].rag[1];
    return Vector3Add(peds[i].pos, v3(0, 1.2f, 0));
}

// ---- ray against people and cars

static float ray_sphere(Vector3 o, Vector3 d, Vector3 c, float r)
{
    Vector3 oc = Vector3Subtract(o, c);
    float b = Vector3DotProduct(oc, d), cc = Vector3DotProduct(oc, oc) - r * r;
    float disc = b * b - cc;
    if (disc < 0) return -1;
    float t = -b - sqrtf(disc);
    return t;
}

static float ray_capsule(Vector3 ro, Vector3 rd, Vector3 pa, Vector3 pb, float ra)
{
    Vector3 ba = Vector3Subtract(pb, pa), oa = Vector3Subtract(ro, pa);
    float baba = Vector3DotProduct(ba, ba), bard = Vector3DotProduct(ba, rd), baoa = Vector3DotProduct(ba, oa);
    float rdoa = Vector3DotProduct(rd, oa), oaoa = Vector3DotProduct(oa, oa);
    float a = baba - bard * bard;
    float b = baba * rdoa - baoa * bard;
    float c = baba * oaoa - baoa * baoa - ra * ra * baba;
    float h = b * b - a * c;
    if (h < 0) return -1;
    if (fabsf(a) > 1e-7f) {
        float t = (-b - sqrtf(h)) / a;
        float y = baoa + t * bard;
        if (y > 0 && y < baba && t >= 0) return t;
        Vector3 oc = y <= 0 ? oa : Vector3Subtract(ro, pb);
        float bb = Vector3DotProduct(rd, oc), cc = Vector3DotProduct(oc, oc) - ra * ra;
        float hh = bb * bb - cc;
        if (hh > 0) return -bb - sqrtf(hh);
        return -1;
    }
    return ray_sphere(ro, rd, pa, ra);
}

static float ped_ray(int i, Vector3 o, Vector3 d, bool *head)
{
    Ped *p = &peds[i];
    float best = -1;
    *head = false;
    Vector3 mid = pv[i].rag_on ? chest_of(i) : Vector3Add(p->pos, v3(0, 1, 0));
    Vector3 to = Vector3Subtract(mid, o);
    float along = Vector3DotProduct(to, d);
    if (along < -1.5f) return -1;
    if (Vector3LengthSqr(to) - along * along > 2.6f * 2.6f) return -1;
    if (pv[i].rag_on) {
        for (int k = 0; k < 11; k++) {
            float r = k == 2 ? 0.17f : k < 2 ? 0.2f : 0.1f;
            float t = ray_sphere(o, d, p->rag[k], r);
            if (t >= 0 && (best < 0 || t < best)) { best = t; *head = k == 2; }
        }
        float t = ray_capsule(o, d, p->rag[0], p->rag[1], 0.2f);
        if (t >= 0 && (best < 0 || t < best)) { best = t; *head = false; }
        return best;
    }
    float top = pv[i].crouch ? 1.2f : 1.72f;
    float tb = ray_capsule(o, d, Vector3Add(p->pos, v3(0, 0.3f, 0)), Vector3Add(p->pos, v3(0, top - 0.5f, 0)), 0.27f);
    float th = ray_sphere(o, d, Vector3Add(p->pos, v3(0, top - 0.14f, 0)), 0.17f);
    if (th >= 0 && (tb < 0 || th <= tb + 0.06f)) { *head = true; return th; }
    return tb;
}

static float car_ray(int ci, Vector3 o, Vector3 d, float maxd, Vector3 *n_out, float *local_y)
{
    Car *c = &cars[ci];
    const CarModel *m = &car_models[c->model];
    Vector3 to = Vector3Subtract(c->pos, o);
    float along = Vector3DotProduct(to, d);
    float rad = m->size.z * 0.62f;
    if (along < -rad || along > maxd + rad) return -1;
    if (Vector3LengthSqr(to) - along * along > rad * rad) return -1;
    Matrix mx = car_matrix(c);
    Vector3 org = v3(mx.m12, mx.m13, mx.m14);
    Quaternion inv = QuaternionInvert(c->rot);
    Vector3 lo = Vector3RotateByQuaternion(Vector3Subtract(o, org), inv);
    Vector3 ld = Vector3RotateByQuaternion(d, inv);
    float lo_a[3] = { lo.x, lo.y, lo.z }, ld_a[3] = { ld.x, ld.y, ld.z };
    float mn[3] = { -m->size.x * 0.5f, 0.2f, -m->size.z * 0.5f }, mxv[3] = { m->size.x * 0.5f, m->size.y, m->size.z * 0.5f };
    float t0 = 0, t1 = maxd;
    int axis = -1, sgn = 0;
    for (int a = 0; a < 3; a++) {
        if (fabsf(ld_a[a]) < 1e-9f) { if (lo_a[a] < mn[a] || lo_a[a] > mxv[a]) return -1; continue; }
        float ta = (mn[a] - lo_a[a]) / ld_a[a], tb = (mxv[a] - lo_a[a]) / ld_a[a];
        int sg = -1;
        if (ta > tb) { float t = ta; ta = tb; tb = t; sg = 1; }
        if (ta > t0) { t0 = ta; axis = a; sgn = sg; }
        if (tb < t1) t1 = tb;
        if (t0 > t1) return -1;
    }
    if (axis < 0) return -1;
    Vector3 ln = v3(axis == 0 ? sgn : 0, axis == 1 ? sgn : 0, axis == 2 ? sgn : 0);
    *n_out = Vector3RotateByQuaternion(ln, c->rot);
    *local_y = lo.y + ld.y * t0;
    return t0;
}

bool shoot_ray(Vector3 o, Vector3 dir, float maxd, int ignore_ped, int ignore_car, Hit *h)
{
    Hit best = { .t = maxd };
    bool found = false;
    head_hit = false;
    Hit w;
    if (world_ray(o, dir, maxd, &w)) { best = w; found = true; }
    for (int ci = 0; ci < MAX_CARS; ci++) {
        if (!cars[ci].used || ci == ignore_car) continue;
        Vector3 n;
        float ly;
        float t = car_ray(ci, o, dir, best.t, &n, &ly);
        if (t >= 0 && t < best.t) {
            best = (Hit){ Vector3Add(o, Vector3Scale(dir, t)), n, t, HIT_CAR, ci };
            found = true;
            head_hit = false;
        }
    }
    for (int i = 0; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used || i == ignore_ped || p->state == PS_DEAD || p->state == PS_IN_CAR) continue;
        bool hd;
        float t = ped_ray(i, o, dir, &hd);
        if (t >= 0 && t < best.t) {
            Vector3 pt = Vector3Add(o, Vector3Scale(dir, t));
            best = (Hit){ pt, Vector3Normalize(Vector3Subtract(pt, chest_of(i))), t, HIT_PED, i };
            found = true;
            head_hit = hd;
        }
    }
    if (found && h) *h = best;
    return found;
}

// ---- hurting people, with the right crime attached

static bool hurt_ped(int i, float dmg, Vector3 imp, int by, int weapon)
{
    Ped *p = &peds[i];
    bool was_alive = p->used && p->state != PS_DEAD;
    ped_damage(i, dmg, imp, by, weapon);
    bool dead = was_alive && peds[i].state == PS_DEAD;
    if (by == PLAYER && i != PLAYER && was_alive) {
        bool cop = p->kind == PED_COP;
        crime(dead ? (cop ? CR_COP_MURDER : CR_MURDER) : (cop ? CR_COP_ASSAULT : CR_ASSAULT), p->pos, i);
    }
    return dead;
}

static bool people_near(Vector3 at, float r, int ignore)
{
    for (int i = 0; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used || i == ignore || p->state == PS_DEAD || p->state == PS_IN_CAR) continue;
        if (dist_xz(p->pos, at) < r) return true;
    }
    return false;
}

// ---- bullets

static void impact_fx(const Hit *h, Vector3 d, bool me)
{
    if (h->kind == HIT_WORLD) {
        if (is_water(h->p.x, h->p.z) && h->n.y > 0.9f) {
            fx_burst(FX_WATER, h->p, v3(0, 1, 0), 6);
            return;
        }
        fx_decal(DECAL_BULLET, h->p, h->n, 0.5f);
        fx_burst(FX_SPARK, h->p, h->n, h->n.y > 0.8f ? 2 : 4);
        if (rnd() % 3 == 0) fx_burst(FX_DUST, h->p, h->n, 1);
        if (me && rnd() % 5 == 0) sfx(S_RICOCHET, h->p, 0.5f, frand(0.9f, 1.2f));
    } else if (h->kind == HIT_CAR) {
        fx_burst(FX_SPARK, h->p, h->n, 5);
        if (rnd() % 4 == 0) sfx(S_RICOCHET, h->p, 0.6f, frand(0.8f, 1.1f));
    }
    (void)d;
}

static void blood_splat(int i, Vector3 dir, bool kill)
{
    Vector3 g = peds[i].pos;
    g.y = ground_y(g.x, g.z);
    if (kill) {
        fx_decal(DECAL_BLOOD, Vector3Add(g, Vector3Scale(v3(dir.x, 0, dir.z), 0.3f)), v3(0, 1, 0), frand(1.8f, 2.6f));
        fx_decal(DECAL_BLOOD, Vector3Add(g, Vector3Scale(v3(dir.x, 0, dir.z), 1.1f)), v3(0, 1, 0), frand(0.8f, 1.2f));
    } else if (rnd() % 100 < 40) {
        fx_decal(DECAL_BLOOD, Vector3Add(g, Vector3Scale(v3(dir.x, 0, dir.z), frand(0.6f, 1.4f))), v3(0, 1, 0), frand(0.6f, 1.0f));
    }
}

bool fire_weapon(int pi, Vector3 from, Vector3 dir)
{
    Ped *p = &peds[pi];
    if (!p->used || p->weapon == W_FIST || p->fire_cd > 0 || p->reload_s > 0) return false;
    bool me = pi == PLAYER;
    const Weapon *w = &weapons[p->weapon];
    if (me) {
        if (weapon_reach > 0 || p->clip_ammo <= 0) return false;
        p->clip_ammo--;
        shot_age = 0;
    }
    p->fire_cd = 1 / w->rate;
    dir = Vector3Normalize(dir);
    shot_count++;

    ped_play(pi, A_PISTOL_SHOOT);
    fx_burst(FX_MUZZLE, from, dir, 1);
    static const int snd[W_COUNT] = { S_PUNCH, S_PISTOL, S_SMG, S_SHOTGUN };
    sfx(snd[p->weapon], from, me ? 1.0f : 0.85f, frand(0.96f, 1.04f));
    if (me) fx_shake(p->weapon == W_SHOTGUN ? 0.22f : p->weapon == W_SMG ? 0.04f : 0.06f);
    if (alarm_cd <= 0) { ped_alarm(from, me ? 60 : 45, pi); alarm_cd = 0.5f; }
    if (me && shots_cd <= 0 && people_near(from, 45, PLAYER)) { crime(CR_SHOTS, p->pos, -1); shots_cd = 2.5f; }

    Vector3 right = Vector3CrossProduct(dir, v3(0, 1, 0));
    if (Vector3LengthSqr(right) < 1e-4f) right = v3(1, 0, 0);
    right = Vector3Normalize(right);
    Vector3 up = Vector3CrossProduct(right, dir);

    float dmg_acc[MAX_PEDS] = { 0 };
    Vector3 imp_acc[MAX_PEDS] = { 0 };
    bool head_acc[MAX_PEDS] = { 0 };
    bool tracer_done = false;
    for (int k = 0; k < w->pellets; k++) {
        float a = frand(0, 2 * PI), r = w->spread * sqrtf(frand(0, 1));
        Vector3 d = Vector3Normalize(Vector3Add(dir, Vector3Add(Vector3Scale(right, cosf(a) * r), Vector3Scale(up, sinf(a) * r))));
        Hit h;
        bool hit = shoot_ray(from, d, w->range, pi, p->car, &h);
        bool hd = head_hit;
        Vector3 end = hit ? h.p : Vector3Add(from, Vector3Scale(d, w->range));
        if (!tracer_done && (p->weapon == W_SMG ? shot_count % 3 == 0 : rnd() % 100 < 45)) {
            fx_tracer(from, end);
            tracer_done = true;
        }
        if (!hit) continue;
        float fall = w->pellets > 1 ? Clamp(1 - powf(h.t / w->range, 1.2f), 0.1f, 1) : 1;
        float dmg = w->dmg * fall;
        if (h.kind == HIT_PED) {
            if (hd) dmg *= 2.5f;
            if (h.id == PLAYER && !me) dmg *= COP_DMG_MUL;
            dmg_acc[h.id] += dmg;
            head_acc[h.id] |= hd;
            imp_acc[h.id] = Vector3Add(imp_acc[h.id], Vector3Scale(d, p->weapon == W_SHOTGUN ? 1.8f : 3.2f + dmg * 0.04f));
            fx_burst(FX_BLOOD, h.p, h.n, 3);
        } else if (h.kind == HIT_CAR) {
            Car *c = &cars[h.id];
            car_damage(h.id, dmg * (me ? CAR_DMG_MUL : 2.2f), h.p, pi);
            if (me && (c->ai == AI_COP || (c->driver >= 0 && peds[c->driver].kind == PED_COP))) crime(CR_COP_ASSAULT, h.p, c->driver);
            if (c->driver >= 0 && peds[c->driver].used && rnd() % 100 < 22) {
                Matrix mx = car_matrix(c);
                if (h.p.y > mx.m13 + car_models[c->model].size.y * 0.5f) {
                    float dd = c->driver == PLAYER && !me ? dmg * COP_DMG_MUL : dmg;
                    int d_i = c->driver;
                    if (me) hurt_ped(d_i, dd, d, pi, p->weapon); else ped_damage(d_i, dd, d, pi, p->weapon);
                }
            }
            impact_fx(&h, d, me);
        } else impact_fx(&h, d, me);
    }
    for (int i = 0; i < MAX_PEDS; i++) {
        if (dmg_acc[i] <= 0) continue;
        Vector3 imp = imp_acc[i];
        bool dead;
        if (me) dead = hurt_ped(i, dmg_acc[i], imp, pi, p->weapon);
        else { bool was = peds[i].state != PS_DEAD; ped_damage(i, dmg_acc[i], imp, pi, p->weapon); dead = was && peds[i].state == PS_DEAD; }
        blood_splat(i, Vector3Normalize(imp), dead);
    }
    return true;
}

// ---- explosions

typedef struct { int car; float t, dmg; int by; } Delayed;
static Delayed pend[32];
static int npend;

void explode(Vector3 p, float radius, int attacker)
{
    bool from_car = false;
    for (int ci = 0; ci < MAX_CARS; ci++)
        if (cars[ci].used && cars[ci].health <= 0 && cars[ci].ai != AI_WRECK && Vector3Distance(cars[ci].pos, p) < 3) from_car = true;
    if (!from_car) {
        fx_burst(FX_EXPLOSION, p, v3(0, 1, 0), 1);
        sfx(S_EXPLOSION, p, 1, 1);
        fx_shake(Clamp(1.4f - Vector3Distance(p, cam.position) / 60, 0.1f, 1.2f));
    }
    fx_burst(FX_DEBRIS, p, v3(0, 1, 0), 16);
    fx_burst(FX_SMOKE, Vector3Add(p, v3(0, 0.5f, 0)), v3(0, 1, 0), 6);
    Vector3 g = { p.x, ground_y(p.x, p.z), p.z };
    fx_decal(DECAL_SCORCH, g, v3(0, 1, 0), radius * 1.15f);
    fx_light(Vector3Add(p, v3(0, 1.5f, 0)), v3(6, 3.2f, 1.1f), radius * 4, 0.8f);
    ped_alarm(p, 120, attacker);

    float reach = radius * 1.4f;
    for (int i = 0; i < MAX_PEDS; i++) {
        Ped *q = &peds[i];
        if (!q->used || q->state == PS_DEAD) continue;
        Vector3 c = q->state == PS_IN_CAR && q->car >= 0 ? cars[q->car].pos : chest_of(i);
        float d = Vector3Distance(c, p);
        if (d > reach) continue;
        float k = 1 - d / reach;
        Hit h;
        Vector3 dir = Vector3Normalize(Vector3Subtract(c, p));
        if (world_ray(Vector3Add(p, v3(0, 0.5f, 0)), dir, d, &h)) k *= 0.25f;
        float dmg = 340 * k * k;
        Vector3 imp = Vector3Add(Vector3Scale(v3(dir.x, 0, dir.z), 14 * k + 2), v3(0, 5 * k + 1, 0));
        if (q->state == PS_IN_CAR) continue;
        hurt_ped(i, dmg, imp, attacker, -1);
    }

    bool destroyed = false;
    for (int ci = 0; ci < MAX_CARS; ci++) {
        Car *c = &cars[ci];
        if (!c->used || c->ai == AI_WRECK) continue;
        float d = Vector3Distance(c->pos, p);
        if (d < 1.6f && c->health <= 0) continue;
        if (d > radius * 1.25f) continue;
        float k = 1 - d / (radius * 1.25f);
        car_damage(ci, 1, p, -1);   // wakes it
        Vector3 dir = Vector3Normalize(Vector3Subtract(c->pos, p));
        c->vel = Vector3Add(c->vel, Vector3Add(Vector3Scale(v3(dir.x, 0, dir.z), 9 * k), v3(0, 3.5f * k, 0)));
        c->angvel = Vector3Add(c->angvel, v3(frand(-1, 1) * k * 2, frand(-1, 1) * k, frand(-1, 1) * k * 2));
        if (npend < 32) pend[npend++] = (Delayed){ ci, 0.15f + d * 0.06f + frand(0, 0.2f), 1500 * powf(k, 0.8f), attacker };
        destroyed = true;
    }
    (void)destroyed;
    if (attacker == PLAYER && from_car) crime(CR_DESTROY_CAR, p, -1);
}

static void pending_tick(float dt)
{
    for (int i = 0; i < npend; i++) {
        pend[i].t -= dt;
        if (pend[i].t > 0) continue;
        Delayed d = pend[i];
        pend[i] = pend[--npend];
        i--;
        if (cars[d.car].used) car_damage(d.car, d.dmg, cars[d.car].pos, d.by);
    }
}

// ---- pickups

typedef struct { Vector3 pos; int type, amount; float respawn, life; bool active, temp; } Pickup;
#define MAX_PICK 80
static Pickup picks[MAX_PICK];
static int npicks;
static Mesh pk_mesh[PK_TYPES], pad_mesh, ammo_mesh;
static bool pk_meshes;

static uint32_t hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

static void pick_add(Vector3 at, int type, int amount, bool temp, float life)
{
    int slot = -1;
    for (int i = 0; i < npicks; i++) if (!picks[i].active && picks[i].temp && picks[i].respawn <= 0) { slot = i; break; }
    if (slot < 0 && npicks < MAX_PICK) slot = npicks++;
    if (slot < 0) return;
    picks[slot] = (Pickup){ at, type, amount, 0, life, true, temp };
}

void pickup_drop(Vector3 at, int type, int amount)
{
    at.y = ground_y(at.x, at.z);
    pick_add(at, type, amount, true, 90);
}

static void place_pickups(void)
{
    static const int plan[] = {
        PK_SMG, PK_HEALTH, PK_SHOTGUN, PK_ARMOR,
        PK_PISTOL, PK_AMMO, PK_AMMO, PK_HEALTH, PK_SMG, PK_ARMOR, PK_AMMO, PK_SHOTGUN, PK_HEALTH,
        PK_SMG, PK_AMMO, PK_ARMOR, PK_HEALTH, PK_PISTOL, PK_SHOTGUN, PK_AMMO, PK_SMG, PK_HEALTH, PK_ARMOR, PK_AMMO, PK_SHOTGUN,
    };
    static const float near_lo[4] = { 18, 30, 55, 80 }, near_hi[4] = { 34, 48, 80, 120 };
    int n = (int)(sizeof plan / sizeof plan[0]);
    if (nwalk <= 0) return;
    for (int k = 0; k < n; k++) {
        int best = -1;
        uint32_t best_key = ~0u;
        for (int j = 0; j < nwalk; j++) {
            Vector3 q = walk[j].p;
            if (walk[j].light >= 0 || fabsf(q.x) > 600 || fabsf(q.z) > 600) continue;
            float d = dist_xz(q, police_pos);
            if (k < 4 ? (d < near_lo[k] || d > near_hi[k]) : d < 40) continue;
            bool clash = false;
            for (int m = 0; m < npicks && !clash; m++) clash = dist_xz(picks[m].pos, q) < (k < 4 ? 20 : 60);
            if (clash) continue;
            uint32_t key = hash(j * 2654435761u + k * 97 + 5);
            if (key < best_key) { best_key = key; best = j; }
        }
        if (best < 0) continue;
        Vector3 q = walk[best].p;
        q.y = ground_y(q.x, q.z);
        int t = plan[k];
        int amount = t == PK_HEALTH ? 50 : t == PK_ARMOR ? 100 : 0;
        pick_add(q, t, amount, false, 0);
    }
}

static void give_weapon(int w, int rounds)
{
    Ped *p = &peds[PLAYER];
    bool had = owned[w];
    owned[w] = true;
    if (!had) {
        p->ammo[w] += rounds;
        stored_clip[w] = fminf(weapons[w].clip, p->ammo[w]);
        p->ammo[w] -= stored_clip[w];
    } else p->ammo[w] += rounds;
}

bool shop_open;

bool give_item(int type)
{
    Ped *p = &peds[PLAYER];
    switch (type) {
    case PK_PISTOL: give_weapon(W_PISTOL, 24); break;
    case PK_SMG: give_weapon(W_SMG, 60); break;
    case PK_SHOTGUN: give_weapon(W_SHOTGUN, 18); break;
    case PK_AMMO: {
        bool any = false;
        for (int w = W_PISTOL; w < W_COUNT; w++) if (owned[w]) { p->ammo[w] += weapons[w].clip * 2; any = true; }
        if (!any) return false;
        break; }
    case PK_ARMOR:
        if (p->armor >= 100) return false;
        p->armor = 100;
        break;
    default: return false;
    }
    sfx(S_PICKUP, p->pos, 0.8f, 1);
    return true;
}

static void pick_up(Pickup *k)
{
    Ped *p = &peds[PLAYER];
    char msg[48];
    switch (k->type) {
    case PK_PISTOL: give_weapon(W_PISTOL, 24); hud_msg("Pistol", 2); break;
    case PK_SMG: give_weapon(W_SMG, 60); hud_msg("SMG", 2); break;
    case PK_SHOTGUN: give_weapon(W_SHOTGUN, 18); hud_msg("Shotgun", 2); break;
    case PK_AMMO: {
        bool any = false;
        for (int w = W_PISTOL; w < W_COUNT; w++) if (owned[w]) { p->ammo[w] += weapons[w].clip * 2; any = true; }
        if (!any) return;
        hud_msg("Ammo", 2);
        break; }
    case PK_HEALTH:
        if (p->health >= 100) return;
        p->health = fminf(100, p->health + k->amount);
        hud_msg("Health", 2);
        break;
    case PK_ARMOR:
        if (p->armor >= 100) return;
        p->armor = 100;
        hud_msg("Body armor", 2);
        break;
    default:
        cash += k->amount;
        snprintf(msg, sizeof msg, "+$%d", k->amount);
        hud_msg(msg, 1.5f);
        sfx(S_CASH, p->pos, 0.7f, 1);
        k->active = false;
        return;
    }
    sfx(S_PICKUP, p->pos, 0.8f, 1);
    k->active = false;
    k->respawn = PICK_RESPAWN_S;
}

static void pickups_tick(float dt)
{
    Ped *p = &peds[PLAYER];
    bool afoot = p->car < 0 && (p->state == PS_IDLE || p->state == PS_WALK);
    for (int i = 0; i < npicks; i++) {
        Pickup *k = &picks[i];
        if (k->active) {
            if (k->temp && (k->life -= dt) <= 0) { k->active = false; k->respawn = 0; continue; }
            if (afoot && dist_xz(k->pos, p->pos) < 1.4f && fabsf(k->pos.y - p->pos.y) < 2) pick_up(k);
        } else if (!k->temp && (k->respawn -= dt) <= 0) k->active = true;
    }
}

int pickups_view(PickView *out, int max)
{
    int n = 0;
    for (int i = 0; i < npicks && n < max; i++)
        if (picks[i].active) out[n++] = (PickView){ picks[i].pos, picks[i].type };
    return n;
}

typedef struct { float cx, cy, cz, hx, hy, hz; unsigned char r, g, b; } BoxDef;

static Mesh build_mesh(const BoxDef *b, int n)
{
    Mesh m = { 0 };
    m.vertexCount = n * 24;
    m.triangleCount = n * 12;
    m.vertices = MemAlloc(m.vertexCount * 3 * sizeof(float));
    m.normals = MemAlloc(m.vertexCount * 3 * sizeof(float));
    m.colors = MemAlloc(m.vertexCount * 4);
    m.indices = MemAlloc(m.triangleCount * 3 * sizeof(unsigned short));
    int vi = 0, ii = 0;
    for (int i = 0; i < n; i++) {
        float c[3] = { b[i].cx, b[i].cy, b[i].cz }, h[3] = { b[i].hx, b[i].hy, b[i].hz };
        for (int f = 0; f < 6; f++) {
            int a = f / 2, t1 = (a + 1) % 3, t2 = (a + 2) % 3;
            float s = f % 2 ? -1 : 1;
            for (int q = 0; q < 4; q++) {
                float pos[3];
                pos[a] = c[a] + s * h[a];
                pos[t1] = c[t1] + ((q & 1) ? 1 : -1) * h[t1];
                pos[t2] = c[t2] + ((q & 2) ? 1 : -1) * h[t2];
                float nrm[3] = { 0, 0, 0 };
                nrm[a] = s;
                for (int k = 0; k < 3; k++) { m.vertices[vi * 3 + k] = pos[k]; m.normals[vi * 3 + k] = nrm[k]; }
                m.colors[vi * 4] = b[i].r; m.colors[vi * 4 + 1] = b[i].g; m.colors[vi * 4 + 2] = b[i].b; m.colors[vi * 4 + 3] = 255;
                vi++;
            }
            int base = vi - 4;
            static const int quad[6] = { 0, 1, 3, 0, 3, 2 };
            for (int k = 0; k < 6; k++) m.indices[ii++] = base + quad[k];
        }
    }
    UploadMesh(&m, false);
    return m;
}

static void build_pickup_meshes(void)
{
    pk_meshes = true;
    BoxDef med[16];
    int n = 0;
    med[n++] = (BoxDef){ 0, 0.17f, 0, 0.26f, 0.17f, 0.17f, 238, 240, 242 };
    med[n++] = (BoxDef){ 0, 0.345f, 0, 0.15f, 0.012f, 0.045f, 210, 48, 44 };
    med[n++] = (BoxDef){ 0, 0.345f, 0, 0.045f, 0.012f, 0.15f, 210, 48, 44 };
    med[n++] = (BoxDef){ 0, 0.17f, 0.172f, 0.13f, 0.04f, 0.012f, 210, 48, 44 };
    med[n++] = (BoxDef){ 0, 0.17f, 0.172f, 0.04f, 0.13f, 0.012f, 210, 48, 44 };
    med[n++] = (BoxDef){ 0, 0.17f, -0.172f, 0.13f, 0.04f, 0.012f, 210, 48, 44 };
    med[n++] = (BoxDef){ 0, 0.17f, -0.172f, 0.04f, 0.13f, 0.012f, 210, 48, 44 };
    med[n++] = (BoxDef){ 0.262f, 0.17f, 0, 0.012f, 0.04f, 0.1f, 210, 48, 44 };
    med[n++] = (BoxDef){ 0.262f, 0.17f, 0, 0.012f, 0.1f, 0.04f, 210, 48, 44 };
    med[n++] = (BoxDef){ -0.262f, 0.17f, 0, 0.012f, 0.04f, 0.1f, 210, 48, 44 };
    med[n++] = (BoxDef){ -0.262f, 0.17f, 0, 0.012f, 0.1f, 0.04f, 210, 48, 44 };
    med[n++] = (BoxDef){ 0, 0.37f, 0, 0.09f, 0.02f, 0.025f, 120, 126, 132 };
    pk_mesh[PK_HEALTH] = build_mesh(med, n);

    BoxDef vest[16];
    n = 0;
    vest[n++] = (BoxDef){ 0, 0.3f, 0.07f, 0.24f, 0.28f, 0.05f, 52, 70, 104 };
    vest[n++] = (BoxDef){ 0, 0.3f, -0.07f, 0.24f, 0.28f, 0.05f, 44, 60, 92 };
    vest[n++] = (BoxDef){ 0.16f, 0.6f, 0, 0.065f, 0.035f, 0.12f, 64, 84, 122 };
    vest[n++] = (BoxDef){ -0.16f, 0.6f, 0, 0.065f, 0.035f, 0.12f, 64, 84, 122 };
    vest[n++] = (BoxDef){ 0.255f, 0.24f, 0, 0.018f, 0.2f, 0.12f, 30, 38, 56 };
    vest[n++] = (BoxDef){ -0.255f, 0.24f, 0, 0.018f, 0.2f, 0.12f, 30, 38, 56 };
    vest[n++] = (BoxDef){ 0, 0.17f, 0.124f, 0.16f, 0.07f, 0.012f, 78, 100, 140 };
    vest[n++] = (BoxDef){ 0, 0.4f, 0.124f, 0.09f, 0.1f, 0.012f, 130, 176, 232 };
    pk_mesh[PK_ARMOR] = build_mesh(vest, n);

    BoxDef cashb[16];
    n = 0;
    for (int s = 0; s < 3; s++) {
        float x = s == 2 ? 0 : (s ? 0.19f : -0.19f), y = s == 2 ? 0.145f : 0.05f;
        cashb[n++] = (BoxDef){ x, y, 0, 0.17f, 0.048f, 0.1f, 62, 140, 84 };
        cashb[n++] = (BoxDef){ x, y + 0.046f, 0, 0.165f, 0.004f, 0.095f, 126, 190, 128 };
        cashb[n++] = (BoxDef){ x, y, 0, 0.04f, 0.05f, 0.102f, 232, 228, 200 };
    }
    pk_mesh[PK_CASH] = build_mesh(cashb, n);

    BoxDef pad[2] = { { 0, 0.012f, 0, 0.38f, 0.012f, 0.38f, 40, 46, 56 }, { 0, 0.026f, 0, 0.3f, 0.008f, 0.3f, 255, 168, 48 } };
    pad_mesh = build_mesh(pad, 2);
    BoxDef box[8];
    n = 0;
    box[n++] = (BoxDef){ 0, 0.14f, 0, 0.26f, 0.14f, 0.15f, 92, 104, 72 };
    box[n++] = (BoxDef){ 0, 0.285f, 0, 0.27f, 0.012f, 0.16f, 74, 84, 58 };
    box[n++] = (BoxDef){ 0, 0.15f, 0.152f, 0.14f, 0.05f, 0.008f, 232, 190, 70 };
    box[n++] = (BoxDef){ 0, 0.15f, -0.152f, 0.14f, 0.05f, 0.008f, 232, 190, 70 };
    ammo_mesh = build_mesh(box, n);
}

void pickups_draw(int pass)
{
    if (pass != PASS_MAIN || npicks == 0) return;
    if (!pk_meshes) build_pickup_meshes();
    int lit = 0;
    for (int i = 0; i < npicks; i++) {
        Pickup *k = &picks[i];
        if (!k->active) continue;
        float d2 = Vector3DistanceSqr(k->pos, cam.position);
        if (d2 > 140 * 140) continue;
        float spin = game_s * 1.1f + i * 1.7f;
        float bob = sinf(game_s * 2 + i) * 0.06f;
        Matrix at = MatrixTranslate(k->pos.x, k->pos.y, k->pos.z);
        Color padc = WHITE;
        switch (k->type) {
        case PK_PISTOL: case PK_SMG: case PK_SHOTGUN: {
            Mesh g = gun_mesh[k->type == PK_PISTOL ? W_PISTOL : k->type == PK_SMG ? W_SMG : W_SHOTGUN];
            Matrix m = MatrixMultiply(MatrixMultiply(MatrixScale(1.5f, 1.5f, 1.5f), MatrixRotateY(spin)), MatrixTranslate(k->pos.x, k->pos.y + 0.9f + bob, k->pos.z));
            draw_mesh(g, m, WHITE, MAT_PROP);
            padc = (Color){ 255, 190, 90, 255 };
            break; }
        case PK_AMMO: {
            Matrix m = MatrixMultiply(MatrixMultiply(MatrixScale(1.2f, 1.2f, 1.2f), MatrixRotateY(spin)), MatrixTranslate(k->pos.x, k->pos.y + 0.62f + bob, k->pos.z));
            draw_mesh(ammo_mesh, m, WHITE, MAT_PROP);
            padc = (Color){ 255, 190, 90, 255 };
            break; }
        case PK_CASH: {
            Matrix m = MatrixMultiply(MatrixMultiply(MatrixScale(0.8f, 0.8f, 0.8f), MatrixRotateY(spin)), MatrixTranslate(k->pos.x, k->pos.y + 0.5f + bob, k->pos.z));
            draw_mesh(pk_mesh[PK_CASH], m, WHITE, MAT_PROP);
            padc = (Color){ 130, 220, 150, 255 };
            break; }
        default: {
            Matrix m = MatrixMultiply(MatrixMultiply(MatrixScale(1.3f, 1.3f, 1.3f), MatrixRotateY(spin)), MatrixTranslate(k->pos.x, k->pos.y + 0.55f + bob, k->pos.z));
            draw_mesh(pk_mesh[k->type], m, WHITE, MAT_PROP);
            padc = k->type == PK_HEALTH ? (Color){ 255, 120, 110, 255 } : (Color){ 130, 180, 255, 255 };
            break; }
        }
        if (k->type != PK_CASH) draw_mesh(pad_mesh, at, padc, MAT_PROP);
        if (r_night > 0.25f && lit < 3 && d2 < 50 * 50) {
            Vector3 col = k->type == PK_HEALTH ? v3(1.4f, 0.4f, 0.35f) : k->type == PK_ARMOR ? v3(0.5f, 0.8f, 1.6f)
                        : k->type == PK_CASH ? v3(0.5f, 1.3f, 0.6f) : v3(1.5f, 1.0f, 0.4f);
            light_add(Vector3Add(k->pos, v3(0, 0.6f, 0)), col, 4.5f);
            lit++;
        }
    }
}

// ---- shops and robberies

typedef struct { int ped, state; float got, total, lost_t, alarm_t, dead_until, say_t; } Rob;
enum { R_IDLE, R_ROB, R_DONE };
static Rob *rob;

static void clerk_upkeep(int s, float dt)
{
    Rob *r = &rob[s];
    Shop *sh = &shops[s];
    Ped *pl = &peds[PLAYER];
    float d = dist_xz(pl->pos, sh->inside);
    if (r->ped >= 0) {
        Ped *c = &peds[r->ped];
        if (!c->used) r->ped = -1;
        else if (c->state == PS_DEAD) {
            if (r->state == R_ROB) pickup_drop(c->pos, PK_CASH, (int)r->got + 20 + rnd() % 80);
            r->ped = -1;
            r->dead_until = game_s + 180;
            r->state = R_IDLE;
        } else if (d > 130 && r->state != R_ROB) { ped_remove(r->ped); r->ped = -1; }
    }
    if (r->ped < 0 && d < 90 && game_s > r->dead_until) {
        int i = ped_spawn(PED_CIV, sh->inside, sh->yaw);
        if (i < 0) return;
        peds[i].state = PS_IDLE;
        peds[i].timer = 99;
        peds[i].target = -1;
        r->ped = i;
    }
    if (r->ped < 0) return;
    Ped *c = &peds[r->ped];
    if (c->state == PS_RAGDOLL || staggering(r->ped)) return;
    if (r->state == R_ROB) c->state = PS_HANDS_UP;
    else if (c->state == PS_FLEE || c->state == PS_WALK || c->state == PS_HANDS_UP) { c->state = PS_COWER; c->timer = 2; c->node = c->next_node = -1; }
    else if (c->state == PS_IDLE) c->timer = 99;
    if (c->state == PS_IDLE || c->state == PS_HANDS_UP || c->state == PS_COWER) {
        Vector3 look = r->state == R_ROB ? Vector3Subtract(pl->pos, c->pos) : v3(sinf(sh->yaw), 0, cosf(sh->yaw));
        float want = atan2f(look.x, look.z);
        c->yaw += Clamp(angle_diff(c->yaw, want), -8 * dt, 8 * dt);
        c->vel = v3(0, 0, 0);
    }
}

bool rob_view(float *frac, int *got_cash)
{
    for (int s = 0; rob && s < nshops; s++) {
        if (rob[s].state != R_ROB) continue;
        *frac = Clamp(rob[s].got / rob[s].total, 0, 1);
        *got_cash = (int)fminf(rob[s].got, rob[s].total);
        return true;
    }
    return false;
}

static bool aiming_at(Vector3 target_pt, float maxd)
{
    if (!aim_live) return false;
    Vector3 to = Vector3Subtract(target_pt, aim_from);
    float t = Vector3DotProduct(to, aim_dir);
    if (t < 0 || t > maxd) return false;
    Vector3 perp = Vector3Subtract(to, Vector3Scale(aim_dir, t));
    return Vector3Length(perp) < 1.1f;
}

static void robbery_tick(float dt)
{
    if (nshops <= 0) return;
    if (!rob) {
        rob = calloc(nshops, sizeof *rob);
        for (int s = 0; s < nshops; s++) rob[s].ped = -1;
    }
    Ped *pl = &peds[PLAYER];
    char msg[48];
    for (int s = 0; s < nshops; s++) {
        Rob *r = &rob[s];
        clerk_upkeep(s, dt);
        Shop *sh = &shops[s];
        if (r->alarm_t > 0 && (r->alarm_t -= dt) <= 0) {
            crime(CR_ROBBERY, sh->door, r->ped);
            if (r->ped >= 0) voice(S_V_OUTTA, peds[r->ped].pos, 1, -1);
            if (r->state == R_DONE) r->state = R_IDLE;
        }
        if (r->ped < 0 || peds[r->ped].state == PS_DEAD || staggering(r->ped)) continue;
        Ped *c = &peds[r->ped];
        float d = Vector3Distance(pl->pos, c->pos);
        bool armed_aim = aim_live && pl->weapon != W_FIST && d < 10;
        Vector3 cp = Vector3Add(c->pos, v3(0, 1.3f, 0));
        bool at = armed_aim && aiming_at(cp, 12);
        if (at) {
            Hit h;
            Vector3 to = Vector3Normalize(Vector3Subtract(cp, aim_from));
            if (world_ray(aim_from, to, Vector3Distance(aim_from, cp) - 0.4f, &h)) at = false;
        }
        if (r->state == R_IDLE && at && game_s - sh->robbed_at > ROB_COOLDOWN_S && r->alarm_t <= 0) {
            r->state = R_ROB;
            r->got = 0;
            r->total = 150 + (float)(rnd() % 751);
            r->lost_t = 0;
            r->say_t = 0;
            hud_msg("Hands up", 1.5f);
            voice(rnd() & 1 ? S_V_TAKEIT : S_V_DONTSHOOT, c->pos, 1, -1);
        } else if (r->state == R_ROB) {
            if (at) {
                r->lost_t = 0;
                r->got += r->total * dt / (d < 3.5f ? 4.5f : 7.5f);
                if (r->got >= r->total) {
                    cash += (int)r->total;
                    snprintf(msg, sizeof msg, "Got $%d. Go.", (int)r->total);
                    hud_msg(msg, 3);
                    sfx(S_CASH, c->pos, 1, 1);
                    sh->robbed_at = game_s;
                    r->state = R_DONE;
                    r->alarm_t = 4.5f;
                    c->state = PS_COWER;
                    c->timer = 6;
                }
            } else if ((r->lost_t += dt) > 0.7f) {
                c->state = PS_COWER;
                c->timer = 3;
                r->state = R_IDLE;
                if (rnd() % 100 < 70) { r->alarm_t = 3.5f; hud_msg("They hit the alarm", 2.5f); }
                else hud_msg("They calmed down", 2);
            }
        }
    }
}

// ---- the player

static void equip(int w)
{
    Ped *p = &peds[PLAYER];
    stored_clip[p->weapon] = p->clip_ammo;
    p->weapon = w;
    p->clip_ammo = stored_clip[w];
}

void weapon_stow(void)
{
    Ped *p = &peds[PLAYER];
    equip(W_FIST);
    wanted_weapon = W_FIST;
    weapon_reach = 0;
    p->aiming = aim_live = false;
    p->reload_s = p->fire_cd = 0;
    pv[PLAYER].up_w = pv[PLAYER].pitch = 0;
    pv[PLAYER].shot = -1;
    punch_t = -1;
}

static void cycle_weapon(int dir)
{
    Ped *p = &peds[PLAYER];
    int w = wanted_weapon;
    for (int k = 0; k < W_COUNT; k++) {
        w = (w + dir + W_COUNT) % W_COUNT;
        if (w == W_FIST || owned[w]) break;
    }
    if (w == wanted_weapon) return;
    wanted_weapon = w;
    p->reload_s = 0;
    punch_t = -1;
    pv[PLAYER].shot = -1;
    sfx(S_UI, p->pos, 0.5f, 1.2f);
}

static void start_reload(void)
{
    Ped *p = &peds[PLAYER];
    const Weapon *w = &weapons[p->weapon];
    if (p->weapon == W_FIST || p->reload_s > 0 || p->clip_ammo >= w->clip || p->ammo[p->weapon] <= 0) return;
    p->reload_s = w->reload_s;
    ped_play(PLAYER, A_PISTOL_RELOAD);
    sfx(S_RELOAD, p->pos, 0.8f, 1);
}

static void aim_solve(bool lock)
{
    Ped *p = &peds[PLAYER];
    Vector3 co = cam.position, cd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 chest = Vector3Add(p->pos, v3(0, 1.3f, 0));
    float t0 = fmaxf(Vector3DotProduct(Vector3Subtract(chest, co), cd), 0.3f);
    Vector3 start = Vector3Add(co, Vector3Scale(cd, t0));
    Hit h;
    Vector3 point = Vector3Add(start, Vector3Scale(cd, 120));
    float hit_t = 120;
    int target = -1;
    if (shoot_ray(start, cd, 120, PLAYER, -1, &h)) {
        point = h.p;
        hit_t = h.t;
        if (h.kind == HIT_PED) target = h.id;
    }
    if (lock) {
        float best = 0.05f;
        for (int i = 1; i < MAX_PEDS; i++) {
            Ped *q = &peds[i];
            if (!q->used || q->state == PS_DEAD || q->state == PS_IN_CAR) continue;
            Vector3 c = chest_of(i), v = Vector3Subtract(c, start);
            float d = Vector3Length(v);
            if (d < 3 || d > 70 || d > hit_t + 1.5f) continue;
            float ang = acosf(Clamp(Vector3DotProduct(v, cd) / d, -1, 1));
            if (ang < best) { best = ang; point = Vector3Lerp(point, c, 0.7f); target = i; }
        }
    }
    Vector3 muz0 = ped_hand(p);
    Vector3 d = Vector3Subtract(point, muz0);
    float len = Vector3Length(d);
    d = len < 2 ? cd : Vector3Scale(d, 1 / len);
    aim_from = Vector3Add(muz0, Vector3Scale(d, 0.45f));
    aim_dir = d;
    aim_target = target;
}

static int melee_target(float yaw, float reach, float arc)
{
    Ped *p = &peds[PLAYER];
    int best = -1;
    float bd = reach;
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *q = &peds[i];
        if (!q->used || q->state == PS_DEAD || q->state == PS_IN_CAR || q->state == PS_ENTERING) continue;
        float d = dist_xz(q->pos, p->pos);
        if (d > bd) continue;
        float a = atan2f(q->pos.x - p->pos.x, q->pos.z - p->pos.z);
        if (fabsf(angle_diff(yaw, a)) > arc) continue;
        bd = d; best = i;
    }
    return best;
}

static void punch_tick(const Input *in, float dt)
{
    Ped *p = &peds[PLAYER];
    p->fire_cd -= dt;
    combo_s -= dt;
    if (punch_t > 0 && (punch_t -= dt) <= 0) {
        bool heavy = punch_n % 3 == 2;
        int t = melee_target(p->yaw, 2.1f, 1.15f);
        Vector3 f = v3(sinf(p->yaw), 0, cosf(p->yaw));
        if (t >= 0) {
            Vector3 imp = Vector3Add(Vector3Scale(f, heavy ? 9.5f : 3.6f), v3(0, heavy ? 1.6f : 0, 0));
            sfx(S_HIT, peds[t].pos, 0.9f, heavy ? 0.85f : frand(0.95f, 1.1f));
            fx_burst(FX_BLOOD, Vector3Add(peds[t].pos, v3(0, 1.45f, 0)), f, heavy ? 5 : 2);
            fx_shake(heavy ? 0.12f : 0.04f);
            hurt_ped(t, heavy ? 26 : 11, imp, PLAYER, W_FIST);
            if (peds[t].kind == PED_CIV) voice(S_V_WHAT, peds[t].pos, 1, t);
        } else sfx(S_PUNCH, p->pos, 0.35f, 1.3f);
    }
    if (!(in->held & BTN_FIRE) || p->fire_cd > 0 || punch_t > 0) return;
    if (combo_s > 0) punch_n++; else punch_n = 0;
    combo_s = 1.1f;
    int t = melee_target(cam_yaw, 2.6f, 1.3f);
    if (t >= 0) p->yaw = atan2f(peds[t].pos.x - p->pos.x, peds[t].pos.z - p->pos.z);
    else p->yaw = cam_yaw;
    ped_play(PLAYER, punch_n % 2 ? A_PUNCH_CROSS : A_PUNCH_JAB);
    p->fire_cd = punch_n % 3 == 2 ? 0.62f : 0.4f;
    punch_t = 0.14f;
    sfx(S_PUNCH, p->pos, 0.5f, 1);
}

static void gun_tick(const Input *in, float dt)
{
    Ped *p = &peds[PLAYER];
    p->fire_cd -= dt;
    if (p->reload_s > 0 && (p->reload_s -= dt) <= 0) {
        int need = weapons[p->weapon].clip - p->clip_ammo, take = (int)fminf(need, p->ammo[p->weapon]);
        p->clip_ammo += take;
        p->ammo[p->weapon] -= take;
        p->reload_s = 0;
    }
    aim_solve(p->aiming);
    aim_live = p->aiming;
    if (in->hit & BTN_RELOAD) start_reload();
    if (!(in->held & BTN_FIRE) || p->reload_s > 0) return;
    if (p->clip_ammo <= 0) {
        if (p->ammo[p->weapon] > 0) start_reload();
        else if (in->hit & BTN_FIRE) sfx(S_EMPTY, p->pos, 0.7f, 1);
        return;
    }
    if (p->fire_cd > 0) return;
    Vector3 d = aim_dir;
    if (!p->aiming) {
        // from the hip the body turns to the camera and the shot wanders
        p->yaw += Clamp(angle_diff(p->yaw, cam_yaw), -0.6f, 0.6f);
        float a = frand(0, 2 * PI), r = 0.045f * sqrtf(frand(0, 1));
        Vector3 right = Vector3Normalize(Vector3CrossProduct(d, v3(0, 1, 0)));
        d = Vector3Normalize(Vector3Add(d, Vector3Add(Vector3Scale(right, cosf(a) * r), v3(0, sinf(a) * r, 0))));
    } else if (pv[PLAYER].speed > 1) {
        float a = frand(0, 2 * PI), r = 0.012f * sqrtf(frand(0, 1));
        d = Vector3Normalize(Vector3Add(d, v3(cosf(a) * r, sinf(a) * r, 0)));
    }
    fire_weapon(PLAYER, aim_from, d);
}

// ---- loot from the dead

static bool dropped[MAX_PEDS];

static void drops_tick(void)
{
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used) { dropped[i] = false; continue; }
        if (p->state != PS_DEAD || dropped[i]) continue;
        dropped[i] = true;
        if (p->kind == PED_COP) {
            pickup_drop(p->pos, rnd() & 1 ? PK_PISTOL : PK_AMMO, 0);
        } else if (rnd() % 100 < 45) {
            pickup_drop(p->pos, PK_CASH, 8 + rnd() % 55);
        }
    }
}

static void init_loadout(void)
{
    ready = true;
    Ped *p = &peds[PLAYER];
    owned[W_PISTOL] = true;
    stored_clip[W_PISTOL] = p->ammo[W_PISTOL] = weapons[W_PISTOL].clip;
    place_pickups();
}


#ifdef COMBAT_TEST
// OOP_SPAWN="c6 c8:1.5 a12 k10": civilian, car, cop on foot at that many metres ahead, optional :lateral
static void test_spawn(void)
{
    const char *env = getenv("OOP_SPAWN");
    Ped *p = &peds[PLAYER];
    if (getenv("OOP_GIVE") && ticks == 5) { give_weapon(W_SMG, 150); give_weapon(W_SHOTGUN, 60); give_weapon(W_PISTOL, 60); }
    if (getenv("OOP_SHOP") && ticks == 3) {
        Shop *sh = &shops[atoi(getenv("OOP_SHOP")) % nshops];
        Vector3 out = v3(sinf(sh->yaw), 0, cosf(sh->yaw));
        p->pos = Vector3Add(sh->door, Vector3Scale(out, 0.4f));
        p->pos.y = ground_y(p->pos.x, p->pos.z);
        cam_yaw = sh->yaw + PI;
        fprintf(stderr, "shop %d door %.1f %.1f\n", atoi(getenv("OOP_SHOP")) % nshops, sh->door.x, sh->door.z);
    }
    if (getenv("OOP_ANIM") && ticks > 40 && ticks % 90 == 0) {
        static const int list[] = { A_INTERACT, A_TALK, A_COWER, A_PISTOL_AIM, A_HIT_HEAD, A_PISTOL_IDLE };
        int k = (int)(ticks / 90 - 1) % 6;
        for (int i = 1; i < MAX_PEDS; i++) if (peds[i].used && peds[i].kind == PED_CIV && peds[i].state == PS_IDLE) { peds[i].timer = 600; ped_play(i, list[k]); }
        fprintf(stderr, "anim %d\n", list[k]);
    }
    if (!env || ticks != 30) return;
    Vector3 f = v3(sinf(cam_yaw), 0, cosf(cam_yaw)), l = v3(-f.z, 0, f.x);
    char buf[160];
    snprintf(buf, sizeof buf, "%s", env);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        float d = atof(tok + 1), lat = strchr(tok, ':') ? atof(strchr(tok, ':') + 1) : 0;
        Vector3 at = Vector3Add(p->pos, Vector3Add(Vector3Scale(f, d), Vector3Scale(l, lat)));
        at.y = ground_y(at.x, at.z);
        if (tok[0] == 'c' || tok[0] == 'k') {
            int i = ped_spawn(tok[0] == 'k' ? PED_COP : PED_CIV, at, cam_yaw + PI);
            if (i < 0) continue;
            peds[i].state = PS_IDLE;
            peds[i].timer = 600;
            if (tok[0] == 'k') peds[i].weapon = W_PISTOL;
        } else if (tok[0] == 'a') car_spawn(CAR_SEDAN, at, cam_yaw + PI / 2, AI_PARKED);
        else if (tok[0] == 'b') car_spawn(CAR_SEDAN, at, cam_yaw, AI_PARKED);
    }
}
#endif

static void gun_gasp(void)
{
    if (!aim_live || peds[PLAYER].weapon == W_FIST || ticks % 10) return;
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *c = &peds[i];
        if (!c->used || c->kind != PED_CIV || c->state == PS_DEAD || c->car >= 0) continue;
        if (Vector3Distance(c->pos, peds[PLAYER].pos) < 18 && aiming_at(Vector3Add(c->pos, v3(0, 1.3f, 0)), 18)) voice(S_V_GUN, c->pos, 1, i);
    }
}

void combat_tick(const Input *in, float dt)
{
    Ped *p = &peds[PLAYER];
    if (!p->used) return;
    if (!ready) init_loadout();
#ifdef COMBAT_TEST
    test_spawn();
#endif
    map_held = (in->held & BTN_MAP) != 0;
    static int prev_mode;
    if (mode != prev_mode) { mode_since = game_s; prev_mode = mode; }
    shot_age += dt;
    gun_gasp();
    alarm_cd -= dt;
    shots_cd -= dt;
    aim_live = false;
    aim_target = -1;
    pending_tick(dt);
    drops_tick();
    pickups_tick(dt);

    bool free_hands = mode == MODE_PLAY && p->car < 0 && (p->state == PS_IDLE || p->state == PS_WALK);
    if (free_hands) {
        if (!shop_open && (in->hit & BTN_NEXT)) cycle_weapon(1);
        if (!shop_open && (in->hit & BTN_PREV)) cycle_weapon(-1);
        // The hand travels to the belt, changes meshes there, then comes back.
        bool reaching = wanted_weapon != p->weapon;
        if (reaching || weapon_reach > 0) {
            weapon_reach = Clamp(weapon_reach + (reaching ? 6 : -6) * dt, 0, 1);
            if (reaching && weapon_reach >= 1) equip(wanted_weapon);
            p->aiming = false;
            p->fire_cd -= dt;
        } else if (p->weapon == W_FIST) { p->aiming = false; punch_tick(in, dt); }
        else gun_tick(in, dt);
    } else {
        wanted_weapon = p->weapon;
        weapon_reach = 0;
        p->aiming = false;
        p->reload_s = 0;
        punch_t = -1;
    }
    robbery_tick(dt);
}
