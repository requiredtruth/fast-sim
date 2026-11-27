#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ped.h"
#include "city.h"
#include "combat.h"

void cont_drop(Vector3 p, int kind);

const Weapon weapons[W_COUNT] = {
    [W_FIST]    = { "Fists",         12, 2.4f,  0,      1.7f, 0, 1, 0 },
    [W_PISTOL]  = { "Revolver",      30, 2.6f,  0.012f, 90,   6, 1, 2.2f },
    [W_RIFLE]   = { "Hunting rifle", 70, 0.9f,  0.003f, 180,  5, 1, 2.8f },
    [W_SHOTGUN] = { "Shotgun",       14, 1.1f,  0.075f, 45,   2, 8, 2.4f },
    [W_BAT]     = { "Lead pipe",     24, 1.6f,  0,      2.2f, 0, 1, 0 },
};
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
    if (by == PLAYER && i != PLAYER && dead) sv.kills++;
    if (by == PLAYER && i != PLAYER && !dead && peds[i].used && peds[i].kind == PED_COP && peds[i].state == PS_IDLE) { peds[i].state = PS_CHASE; peds[i].target = PLAYER; }
    if (by == PLAYER && i != PLAYER && was_alive && !dead && peds[i].kind == PED_CIV && peds[i].role == ROLE_TRADER) {
        peds[i].kind = PED_COP; peds[i].role = ROLE_RAIDER; peds[i].state = PS_CHASE; peds[i].target = PLAYER;   // they fight back
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
    static const int snd[W_COUNT] = { S_PUNCH, S_PISTOL, S_SMG, S_SHOTGUN, S_PUNCH };
    sfx(snd[p->weapon], from, me ? 1.0f : 0.85f, p->weapon == W_RIFLE ? frand(0.7f, 0.75f) : frand(0.96f, 1.04f));
    if (me) fx_shake(p->weapon == W_SHOTGUN ? 0.22f : p->weapon == W_RIFLE ? 0.16f : 0.08f);
    if (alarm_cd <= 0) { ped_alarm(from, me ? 60 : 45, pi); alarm_cd = 0.5f; }
    (void)people_near;

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
        if (me) {
            float ta;
            if (hunt_hit(from, d, hit ? h.t : w->range, w->dmg, &ta)) { hit = false; end = Vector3Add(from, Vector3Scale(d, ta)); }
        }
        if (!tracer_done && rnd() % 100 < 45) {
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
    (void)from_car;
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

// ---- weapons live in the pack

static int ammo_of(int w) { return cur_weapon_ammo_item(w); }

bool weapon_owned(int w) { return w == W_FIST || owned[w]; }
void weapon_give(int w) { if (w > W_FIST && w < W_COUNT) owned[w] = true; }
void weapon_take(int w)
{
    if (w <= W_FIST || w >= W_COUNT) return;
    owned[w] = false;
    stored_clip[w] = 0;
    if (peds[PLAYER].weapon == w) { weapon_stow(); }
}

void pickups_draw(int pass) { (void)pass; }
void combat_shift(Vector3 d) { aim_from = Vector3Subtract(aim_from, d); }

static bool aiming_at(Vector3 target_pt, float maxd)
{
    if (!aim_live) return false;
    Vector3 to = Vector3Subtract(target_pt, aim_from);
    float t = Vector3DotProduct(to, aim_dir);
    if (t < 0 || t > maxd) return false;
    Vector3 perp = Vector3Subtract(to, Vector3Scale(aim_dir, t));
    return Vector3Length(perp) < 1.1f;
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
        if (w == W_FIST || weapon_owned(w)) break;
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
    if (p->weapon == W_FIST || p->weapon == W_BAT || p->reload_s > 0 || p->clip_ammo >= w->clip || p->ammo[p->weapon] <= 0) return;
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
        bool pipe = p->weapon == W_BAT;
        int t = melee_target(p->yaw, pipe ? 2.5f : 2.1f, 1.15f);
        Vector3 f = v3(sinf(p->yaw), 0, cosf(p->yaw));
        if (t >= 0) {
            Vector3 imp = Vector3Add(Vector3Scale(f, heavy ? 9.5f : 3.6f), v3(0, heavy ? 1.6f : 0, 0));
            sfx(S_HIT, peds[t].pos, 0.9f, heavy ? 0.85f : frand(0.95f, 1.1f));
            fx_burst(FX_BLOOD, Vector3Add(peds[t].pos, v3(0, 1.45f, 0)), f, heavy ? 5 : 2);
            fx_shake(heavy ? 0.12f : 0.04f);
            hurt_ped(t, (heavy ? 26 : 11) * (pipe ? 2.1f : 1), imp, PLAYER, W_FIST);
            if (peds[t].kind == PED_CIV) voice(S_V_WHAT, peds[t].pos, 1, t);
        } else {
            sfx(S_PUNCH, p->pos, 0.35f, 1.3f);
            float ta;
            Vector3 f3 = v3(sinf(p->yaw), 0, cosf(p->yaw));
            if (pipe || heavy) hunt_hit(Vector3Add(p->pos, v3(0, 1.0f, 0)), f3, pipe ? 2.6f : 2.0f, pipe ? 45 : 20, &ta);
        }
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
        if (take > 0 && ammo_of(p->weapon) >= 0) pack_take(pack, PACK_SLOTS, ammo_of(p->weapon), take);
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

// ---- loot from the dead: the body can be searched

static bool dropped[MAX_PEDS];

static void drops_tick(void)
{
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used) { dropped[i] = false; continue; }
        if (p->state != PS_DEAD || dropped[i]) continue;
        if (pv[i].rag_s < 1.5f) continue;
        dropped[i] = true;
        cont_drop(rag_centre(i), CT_CORPSE);
    }
}

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
    ready = true;
    map_held = (in->held & BTN_MAP) != 0;
    // ammo counts come from the pack
    for (int w = W_PISTOL; w < W_COUNT; w++) {
        int it = ammo_of(w);
        p->ammo[w] = it >= 0 ? pack_count(it) : 0;
    }
    if (!weapon_owned(p->weapon)) weapon_stow();
    if (!weapon_owned(wanted_weapon)) wanted_weapon = W_FIST;
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

    bool free_hands = mode == MODE_PLAY && !ui_blocks_game && search_ci < 0 && p->car < 0 && (p->state == PS_IDLE || p->state == PS_WALK);
    if (free_hands) {
        if (in->hit & BTN_NEXT) cycle_weapon(1);
        if (in->hit & BTN_PREV) cycle_weapon(-1);
        // The hand travels to the belt, changes meshes there, then comes back.
        bool reaching = wanted_weapon != p->weapon;
        if (reaching || weapon_reach > 0) {
            weapon_reach = Clamp(weapon_reach + (reaching ? 6 : -6) * dt, 0, 1);
            if (reaching && weapon_reach >= 1) equip(wanted_weapon);
            p->aiming = false;
            p->fire_cd -= dt;
        } else if (p->weapon == W_FIST || p->weapon == W_BAT) { p->aiming = false; punch_tick(in, dt); }
        else gun_tick(in, dt);
    } else {
        wanted_weapon = p->weapon;
        weapon_reach = 0;
        p->aiming = false;
        p->reload_s = 0;
        punch_t = -1;
    }
}
