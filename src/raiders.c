// the people and animals out on the road: drifters and traders at the stops, raider gangs that hold
// some of them, raider cars that hunt you down the highway, and deer and wolves in the wild.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ped.h"
#include "city.h"

float threat;

static float dist_xz(Vector3 a, Vector3 b) { return hypotf(a.x - b.x, a.z - b.z); }
static Vector3 v3(float x, float y, float z) { return (Vector3){ x, y, z }; }
static float angle_diff(float from, float to)
{
    float d = fmodf(to - from + PI, 2 * PI);
    if (d < 0) d += 2 * PI;
    return d - PI;
}
static float turn_to(float yaw, float want, float rate, float dt) { float m = rate * dt; return yaw + Clamp(angle_diff(yaw, want), -m, m); }
static uint32_t hsh(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }

static Vector3 player_pos(void)
{
    Ped *p = &peds[PLAYER];
    if (p->car >= 0 && cars[p->car].used) return cars[p->car].pos;
    return p->pos;
}

static bool los(Vector3 a, Vector3 b)
{
    Vector3 d = Vector3Subtract(b, a);
    float len = Vector3Length(d);
    if (len < 0.5f) return true;
    Hit h;
    return !world_ray(a, Vector3Scale(d, 1 / len), len - 0.4f, &h);
}

// ---------------------------------------------------------------- people at the stops

static int populated[MAX_STOPS];   // segment a stop slot was populated for, -1 when not yet

static int spawn_npc(int kind, int role, Vector3 at, float yaw)
{
    at.y = ground_y(at.x, at.z);
    int i = ped_spawn(kind, at, yaw);
    if (i < 0) return -1;
    Ped *p = &peds[i];
    p->home = at;
    p->role = role;
    p->state = PS_IDLE;
    p->timer = frand(3, 20);
    if (kind == PED_COP) {
        int r = rnd() % 100;
        p->weapon = r < 55 ? W_PISTOL : r < 80 ? W_SHOTGUN : r < 90 ? W_RIFLE : W_BAT;
        p->clip_ammo = 99;
        p->health = 70 + frand(0, 40);
    }
    if (role == ROLE_TRADER) { p->pal[R_SHIRT] = (Color){ 60, 90, 120, 255 }; p->hair = HAIR_CAP; p->pal[R_HAIR] = (Color){ 150, 120, 60, 255 }; }
    return i;
}

static Vector3 stop_spot(const Stop *s, float x, float z)
{
    float c = cosf(s->yaw), sn = sinf(s->yaw);
    return v3(s->p.x + x * c + z * sn, s->p.y, s->p.z - x * sn + z * c);
}

static void populate(int si)
{
    Stop *s = &stops[si];
    populated[si] = s->seg;
    uint32_t h = hsh((uint32_t)s->seg * 2654435761u ^ world_seed);
    float u = (h & 0xffff) / 65535.0f, u2 = ((h >> 16) & 0xffff) / 65535.0f;
    float hostile_k = threat * tune_raiders;
    switch (s->kind) {
    case STOP_TRADER: {
        for (int i = 0; i < MAX_CONTS; i++)
            if (conts[i].used && conts[i].kind == CT_TRADER && conts[i].stop == si) {
                Vector3 back = Vector3Subtract(conts[i].p, v3(sinf(s->yaw + conts[i].yaw) * 0.0f, 0, 0));
                int t = spawn_npc(PED_CIV, ROLE_TRADER, stop_spot(s, 0, -3.6f), s->yaw);
                (void)back;
                if (t >= 0) peds[t].timer = 1e9f;
            }
        if (u < 0.5f) spawn_npc(PED_CIV, ROLE_DRIFTER, stop_spot(s, frand(-6, 6), frand(3, 8)), frand(0, 6.28f));
        return;
    }
    case STOP_CAMP: case STOP_TOWN: case STOP_WRECKS:
        if (difficulty != DIFF_PEACEFUL && u < 0.25f + 0.55f * hostile_k) {
            s->hostile = true;
            int n = 2 + (int)(u2 * (2 + 3 * hostile_k));
            for (int k = 0; k < n; k++) spawn_npc(PED_COP, ROLE_RAIDER, stop_spot(s, frand(-14, 14), frand(-8, 8)), frand(0, 6.28f));
        } else {
            int n = 1 + (u2 < 0.4f);
            for (int k = 0; k < n; k++) spawn_npc(PED_CIV, k == 0 && u2 < 0.5f ? ROLE_TRADER : ROLE_DRIFTER, stop_spot(s, frand(-8, 8), frand(-4, 6)), frand(0, 6.28f));
        }
        return;
    case STOP_GAS: case STOP_DINER: case STOP_MOTEL: case STOP_GARAGE:
        if (s->km < 0.5f) return;   // the first station is quiet
        if (u < 0.12f + 0.3f * hostile_k) {
            s->hostile = true;
            int n = 1 + (int)(u2 * (1 + 2.5f * hostile_k));
            for (int k = 0; k < n; k++) spawn_npc(PED_COP, ROLE_RAIDER, stop_spot(s, frand(-12, 12), frand(-6, 10)), frand(0, 6.28f));
        } else if (u < 0.6f) {
            spawn_npc(PED_CIV, u2 < 0.45f ? ROLE_TRADER : ROLE_DRIFTER, stop_spot(s, frand(-6, 6), frand(2, 9)), frand(0, 6.28f));
        }
        return;
    default:
        if (u < 0.35f) spawn_npc(PED_CIV, u2 < 0.35f ? ROLE_TRADER : ROLE_DRIFTER, stop_spot(s, frand(-5, 5), frand(2, 8)), frand(0, 6.28f));
        return;
    }
}

// ---------------------------------------------------------------- raiders on foot

static float shout_cd;

static float range_of(int w) { return w == W_SHOTGUN ? 10 : w == W_BAT ? 1.6f : w == W_RIFLE ? 40 : 20; }

static void raider_move(int i, Vector3 goal, float speed, float dt)
{
    Ped *p = &peds[i];
    Vector3 d = v3(goal.x - p->pos.x, 0, goal.z - p->pos.z);
    float l = Vector3Length(d);
    Vector3 want = l > 0.3f ? Vector3Scale(d, speed / l) : v3(0, 0, 0);
    p->vel.x += (want.x - p->vel.x) * fminf(1, 8 * dt);
    p->vel.z += (want.z - p->vel.z) * fminf(1, 8 * dt);
    p->pos.x += p->vel.x * dt;
    p->pos.z += p->vel.z * dt;
    world_push(&p->pos, 0.3f, 1.7f);
    cars_push(&p->pos, 0.32f);
    float gy = ground_y(p->pos.x, p->pos.z);
    p->pos.y = fabsf(gy - p->pos.y) < 0.6f ? Lerp(p->pos.y, gy, fminf(1, 14 * dt)) : gy;
    if (l > 0.3f && !p->aiming) p->yaw = turn_to(p->yaw, atan2f(d.x, d.z), 9, dt);
}

static void raider_foot(int i, float dt)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    if (p->state == PS_DEAD || p->state == PS_RAGDOLL || p->state == PS_IN_CAR || p->state == PS_ENTERING || staggering(i)) return;
    Ped *pl = &peds[PLAYER];
    Vector3 tp = player_pos();
    float d = dist_xz(p->pos, tp);
    Vector3 eye = Vector3Add(p->pos, v3(0, 1.5f, 0)), aim = Vector3Add(tp, v3(0, pl->car >= 0 ? 0.9f : 1.2f, 0));
    p->fire_cd -= dt;
    v->state_s += dt;
    bool alive_target = pl->used && pl->state != PS_DEAD;
    switch (p->state) {
    case PS_IDLE: case PS_WALK:
        p->aiming = false;
        if (dist_xz(p->pos, p->home) > 4) raider_move(i, p->home, 1.6f, dt);
        else { p->vel = v3(0, 0, 0); p->timer -= dt; if (p->timer < 0) { p->yaw += frand(-1.5f, 1.5f); p->timer = frand(3, 9); } }
        if (alive_target && d < 34 && (d < 9 || (ticks + i) % 12 == 0) && los(eye, aim)) {
            p->state = PS_CHASE; p->target = PLAYER; v->state_s = 0;
            if (shout_cd <= 0) { voice(rnd() & 1 ? S_V_OUTTA : S_V_TAKEIT, p->pos, 1, i); shout_cd = 4; }
            for (int k = 1; k < MAX_PEDS; k++)   // the gang wakes up together
                if (peds[k].used && peds[k].kind == PED_COP && peds[k].state == PS_IDLE && dist_xz(peds[k].pos, p->pos) < 30) { peds[k].state = PS_CHASE; peds[k].target = PLAYER; }
        }
        break;
    case PS_CHASE: {
        if (!alive_target) { p->state = PS_IDLE; p->aiming = false; break; }
        if (p->health < 25 && frand(0, 1) < 0.004f) { p->state = PS_FLEE; v->src = tp; p->aiming = false; voice(S_V_DONTSHOOT, p->pos, 1, i); break; }
        if (d > 90) { p->state = PS_IDLE; p->aiming = false; break; }
        float r = range_of(p->weapon);
        bool see = (ticks + i) % 6 == 0 ? los(eye, aim) : v->wait == false;
        if ((ticks + i) % 6 == 0) v->wait = !see;
        if (d > r * 0.85f || !see) {
            p->aiming = false;
            // close in, sidestepping a little
            Vector3 side = v3(-(tp.z - p->pos.z), 0, tp.x - p->pos.x);
            side = Vector3Scale(Vector3Normalize(side), sinf(game_s * 0.7f + i) * 3);
            raider_move(i, Vector3Add(tp, side), p->weapon == W_BAT ? 5.2f : 3.6f, dt);
        } else {
            p->vel = Vector3Scale(p->vel, 0.85f);
            p->aiming = p->weapon != W_BAT;
            p->yaw = turn_to(p->yaw, atan2f(tp.x - p->pos.x, tp.z - p->pos.z), 7, dt);
            Vector3 to = Vector3Subtract(aim, eye);
            p->aim_pitch = atan2f(to.y, hypotf(to.x, to.z));
            if (p->weapon == W_BAT) {
                if (d < 1.7f && p->fire_cd <= 0) {
                    ped_play(i, rnd() & 1 ? A_PUNCH_JAB : A_PUNCH_CROSS);
                    p->fire_cd = 1.1f;
                    sfx(S_HIT, tp, 0.9f, frand(0.8f, 1));
                    if (pl->car < 0) { ped_damage(PLAYER, 14, Vector3Scale(Vector3Normalize(to), 3), i, W_BAT); if (frand(0, 1) < 0.2f) sv.bleed = fmaxf(sv.bleed, 0.5f); }
                    else car_damage(pl->car, 40, tp, i);
                }
            } else if (p->fire_cd <= 0 && v->state_s > 0.6f) {
                // they miss more at range, against a moving target, and early on
                float spread = 0.02f + d * 0.0022f + Vector3Length(pl->vel) * 0.01f + (sv.km < 3 ? 0.03f : 0);
                Vector3 dir = Vector3Normalize(Vector3Add(Vector3Normalize(to), v3(frand(-spread, spread), frand(-spread, spread) * 0.6f, frand(-spread, spread))));
                Vector3 from = Vector3Add(ped_hand(p), Vector3Scale(dir, 0.4f));
                p->fire_cd = 0;
                p->reload_s = 0;
                if (fire_weapon(i, from, dir)) {
                    if (sv.km > 0 && pl->car < 0 && frand(0, 1) < 0.08f) sv.bleed = fmaxf(sv.bleed, 0.6f);
                }
                p->fire_cd = (p->weapon == W_SHOTGUN ? 1.6f : p->weapon == W_RIFLE ? 2.2f : 0.9f) * frand(0.8f, 1.5f);
            }
        }
        break;
    }
    case PS_FLEE:
        raider_move(i, Vector3Add(p->pos, Vector3Scale(Vector3Normalize(Vector3Subtract(p->pos, v->src)), 6)), 4.5f, dt);
        if (d > 70) ped_remove(i);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- raider cars

static float event_t = 90;

static int raider_car_spawn(bool ahead)
{
    Vector3 pp = player_pos();
    float s = road_s(pp) + (ahead ? frand(260, 360) : -frand(160, 240));
    if (s < 300) return -1;
    Vector3 dir;
    Vector3 at = road_at(s, &dir);
    Vector3 l = v3(dir.z, 0, -dir.x);
    at = Vector3Add(at, Vector3Scale(l, ahead ? 1.85f : -1.85f));
    int model = rnd() % 3 == 0 ? CAR_PICKUP : CAR_RAIDER;
    if (rnd() % 5 == 0) model = CAR_MUSCLE;
    float yaw = atan2f(dir.x, dir.z) + (ahead ? PI : 0);
    int ci = car_spawn(model, at, yaw, AI_COP);
    if (ci < 0) return -1;
    Car *c = &cars[ci];
    c->fuel = c->fuel_max;
    c->target = PLAYER;
    c->paint = model == CAR_RAIDER ? (Color){ 70, 58, 44, 255 } : (Color){ (unsigned char)(40 + rnd() % 60), 30, 28, 255 };
    c->armor = rnd() % 3 == 0;
    c->vel = Vector3Scale(car_fwd(c), 22);
    int d = ped_spawn(PED_COP, c->pos, yaw);
    if (d < 0) { car_remove(ci); return -1; }
    Ped *p = &peds[d];
    p->car = ci; p->seat = 0; p->state = PS_IN_CAR; p->role = ROLE_RAIDER;
    p->weapon = rnd() % 3 == 0 ? W_SHOTGUN : W_PISTOL;
    p->clip_ammo = 99;
    p->home = c->pos;
    pv[d].sit = 1;
    c->driver = d;
    return ci;
}

static void raider_cars(float dt)
{
    Ped *pl = &peds[PLAYER];
    Vector3 tp = player_pos();
    for (int ci = 0; ci < MAX_CARS; ci++) {
        Car *c = &cars[ci];
        if (!c->used || c->ai != AI_COP) continue;
        int d = c->driver;
        float dist = dist_xz(c->pos, tp);
        if (d < 0 || !peds[d].used || peds[d].car != ci) { c->ai = AI_NONE; continue; }
        Ped *p = &peds[d];
        bool player_car = pl->car >= 0;
        if (c->health < 300 || pl->state == PS_DEAD) {
            // beaten: drive off
            c->ai = AI_FLEE;
            c->ai_timer = 30;
            continue;
        }
        if (!player_car && dist < 25 && fabsf(car_speed(c)) < 6) {
            // the player is on foot: get out and fight
            ped_exit_car(d, false);
            p->state = PS_CHASE; p->target = PLAYER;
            c->ai = AI_NONE;
            continue;
        }
        float spd = player_car ? fmaxf(fabsf(car_speed(&cars[pl->car])) + 8, 18) : (dist > 30 ? 24 : 6);
        Vector3 goal = tp;
        if (player_car && dist < 40) {
            // swing alongside, then ram
            Vector3 f = car_fwd(&cars[pl->car]);
            Vector3 side = v3(f.z, 0, -f.x);
            float k = sinf(game_s * 0.4f + ci) > 0.3f ? 0 : 1;
            goal = Vector3Add(tp, Vector3Add(Vector3Scale(side, 2.8f * k * (ci & 1 ? 1 : -1)), Vector3Scale(f, 3)));
        }
        car_drive_to(ci, goal, spd, true);
        // the passenger side window: shots at the player
        p->fire_cd -= dt;
        if (dist < 32 && p->fire_cd <= 0 && p->weapon != W_BAT) {
            Vector3 from = Vector3Add(c->pos, v3(0, car_models[c->model].size.y * 0.6f, 0));
            Vector3 aim = Vector3Add(tp, v3(0, 1.0f, 0));
            Vector3 to = Vector3Normalize(Vector3Subtract(aim, from));
            float spread = 0.05f + dist * 0.003f;
            to = Vector3Normalize(Vector3Add(to, v3(frand(-spread, spread), frand(-spread, spread) * 0.5f, frand(-spread, spread))));
            p->reload_s = 0; p->fire_cd = 0;
            fire_weapon(d, Vector3Add(from, Vector3Scale(to, 1.4f)), to);
            p->fire_cd = frand(1.0f, 2.2f);
        }
        if (dist > 600) car_remove(ci);
    }
}

void raiders_ambush(void)
{
    if (difficulty == DIFF_PEACEFUL) return;
    Vector3 pp = peds[PLAYER].pos;
    int n = 2 + (int)(threat * 2.5f);
    for (int k = 0; k < n; k++) {
        float a = frand(0, 6.28f), r = frand(18, 28);
        int i = spawn_npc(PED_COP, ROLE_RAIDER, v3(pp.x + sinf(a) * r, pp.y, pp.z + cosf(a) * r), a + PI);
        if (i >= 0) { peds[i].state = PS_CHASE; peds[i].target = PLAYER; }
    }
}

int raiders_active(void)
{
    int n = 0;
    for (int i = 1; i < MAX_PEDS; i++) if (peds[i].used && peds[i].kind == PED_COP && peds[i].state == PS_CHASE) n++;
    for (int i = 0; i < MAX_CARS; i++) if (cars[i].used && cars[i].ai == AI_COP) n++;
    return n;
}

// ---------------------------------------------------------------- wildlife

enum { AN_DEER, AN_WOLF };
typedef struct { bool used, dead, butchered; int kind; Vector3 pos, vel; float yaw, health, timer, step, dead_s; int state; } Animal;
#define MAX_ANIM 10
static Animal an[MAX_ANIM];
static float anim_spawn_t = 5;

static bool night_now(void) { return day_hours < 5.5f || day_hours > 20.5f; }

static void animals_spawn(void)
{
    Vector3 pp = player_pos();
    float s = road_s(pp);
    int b = biome_at(s);
    int deer = 0, wolves = 0;
    for (int i = 0; i < MAX_ANIM; i++) if (an[i].used && !an[i].dead) { if (an[i].kind == AN_DEER) deer++; else wolves++; }
    bool wolfy = difficulty != DIFF_PEACEFUL && (b == BIOME_FOREST || b == BIOME_SNOW || b == BIOME_PLAINS) && night_now();
    int kind = -1;
    if (wolfy && wolves < 3 && frand(0, 1) < 0.5f * (0.5f + threat)) kind = AN_WOLF;
    else if (deer < 4 && b != BIOME_BADLANDS && frand(0, 1) < (b == BIOME_DESERT ? 0.3f : 0.7f)) kind = AN_DEER;
    if (kind < 0) return;
    for (int i = 0; i < MAX_ANIM; i++) {
        if (an[i].used) continue;
        float side = frand(0, 1) < 0.5f ? 1 : -1, ahead = frand(-80, 220);
        Vector3 dir;
        Vector3 at = road_at(s + ahead, &dir);
        Vector3 l = v3(dir.z, 0, -dir.x);
        at = Vector3Add(at, Vector3Scale(l, side * frand(30, 110)));
        at.y = ground_y(at.x, at.z);
        if (is_water(at.x, at.z)) return;
        int n = kind == AN_WOLF ? 2 + (int)frand(0, 2) : 1 + (int)frand(0, 2.5f);
        for (int k = 0; k < n; k++) {
            int j = -1;
            for (int q = 0; q < MAX_ANIM; q++) if (!an[q].used) { j = q; break; }
            if (j < 0) break;
            Animal *a = &an[j];
            memset(a, 0, sizeof *a);
            a->used = true; a->kind = kind;
            a->pos = Vector3Add(at, v3(frand(-4, 4), 0, frand(-4, 4)));
            a->pos.y = ground_y(a->pos.x, a->pos.z);
            a->yaw = frand(0, 6.28f);
            a->health = kind == AN_WOLF ? 45 : 60;
            a->timer = frand(1, 5);
        }
        return;
    }
}

static void animals_tick(float dt)
{
    anim_spawn_t -= dt;
    if (anim_spawn_t <= 0) { anim_spawn_t = frand(20, 45); animals_spawn(); }
    Vector3 pp = player_pos();
    Ped *pl = &peds[PLAYER];
    for (int i = 0; i < MAX_ANIM; i++) {
        Animal *a = &an[i];
        if (!a->used) continue;
        float d = dist_xz(a->pos, pp);
        if (d > 320) { a->used = false; continue; }
        if (a->dead) { a->dead_s += dt; if (a->dead_s > 400) a->used = false; continue; }
        Vector3 want = v3(0, 0, 0);
        float speed = 0;
        if (a->kind == AN_DEER) {
            bool spooked = d < 22 || (d < 60 && pl->car >= 0 && car_kmh(&cars[pl->car]) > 30) || a->state == 1;
            if (spooked) {
                a->state = 1;
                Vector3 away = Vector3Normalize(v3(a->pos.x - pp.x, 0, a->pos.z - pp.z));
                want = away; speed = 9;
                a->timer -= dt;
                if (d > 70) { a->state = 0; a->timer = frand(2, 6); }
            } else {
                a->timer -= dt;
                if (a->timer < 0) { a->yaw += frand(-1.2f, 1.2f); a->timer = frand(2, 8); a->step = frand(0, 1) < 0.5f ? 1 : 0; }
                want = v3(sinf(a->yaw), 0, cosf(a->yaw)); speed = a->step ? 0.8f : 0;
            }
        } else {
            // wolves: circle, then rush
            if (pl->state != PS_DEAD && d < 60) {
                Vector3 to = Vector3Normalize(v3(pp.x - a->pos.x, 0, pp.z - a->pos.z));
                Vector3 side = v3(-to.z, 0, to.x);
                bool rush = d < 14 || a->state == 2;
                if (rush) a->state = 2;
                want = rush ? to : Vector3Normalize(Vector3Add(Vector3Scale(side, 0.8f), Vector3Scale(to, 0.4f)));
                speed = rush ? 8.5f : 4;
                a->timer -= dt;
                if (d < 1.6f && a->timer <= 0 && pl->car < 0) {
                    a->timer = 1.1f;
                    ped_damage(PLAYER, 9, Vector3Scale(to, 2), -1, W_FIST);
                    sfx(S_HIT, pp, 0.8f, 1.4f);
                    if (frand(0, 1) < 0.3f) sv.bleed = fmaxf(sv.bleed, 0.5f);
                    a->state = 1;
                }
                if (a->state == 1 && d > 8) a->state = 2;
            } else { want = v3(sinf(a->yaw), 0, cosf(a->yaw)); speed = 1.5f; a->yaw += frand(-0.05f, 0.05f); }
        }
        a->vel.x += (want.x * speed - a->vel.x) * fminf(1, 4 * dt);
        a->vel.z += (want.z * speed - a->vel.z) * fminf(1, 4 * dt);
        a->pos.x += a->vel.x * dt;
        a->pos.z += a->vel.z * dt;
        world_push(&a->pos, 0.5f, 1.4f);
        a->pos.y = ground_y(a->pos.x, a->pos.z);
        float v = hypotf(a->vel.x, a->vel.z);
        if (v > 0.2f) a->yaw = turn_to(a->yaw, atan2f(a->vel.x, a->vel.z), 5, dt);
        a->step += v * dt;
        // cars hit animals
        for (int ci = 0; ci < MAX_CARS; ci++) {
            Car *c = &cars[ci];
            if (!c->used || Vector3Length(c->vel) < 6) continue;
            if (dist_xz(c->pos, a->pos) < 2.2f) {
                a->health = 0; a->dead = true;
                fx_burst(FX_BLOOD, Vector3Add(a->pos, v3(0, 0.8f, 0)), c->vel, 12);
                sfx(S_CRASH_SMALL, a->pos, 0.8f, 0.8f);
                car_damage(ci, a->kind == AN_DEER ? 70 : 25, a->pos, -1);
                if (a->kind == AN_DEER) { c->radiator = fmaxf(0, c->radiator - 15); c->engine = fmaxf(0, c->engine - 6); }
            }
        }
    }
}

bool hunt_hit(Vector3 o, Vector3 d, float maxd, float dmg, float *t_out)
{
    float best = maxd;
    int hit = -1;
    for (int i = 0; i < MAX_ANIM; i++) {
        Animal *a = &an[i];
        if (!a->used || a->dead) continue;
        float h = a->kind == AN_DEER ? 0.95f : 0.6f, r = a->kind == AN_DEER ? 0.62f : 0.42f;
        Vector3 c = Vector3Add(a->pos, v3(0, h, 0));
        Vector3 oc = Vector3Subtract(o, c);
        float b = Vector3DotProduct(oc, d), cc = Vector3DotProduct(oc, oc) - r * r, disc = b * b - cc;
        if (disc < 0) continue;
        float t = -b - sqrtf(disc);
        if (t > 0 && t < best) { best = t; hit = i; }
    }
    if (hit < 0) return false;
    Animal *a = &an[hit];
    a->health -= dmg;
    a->state = a->kind == AN_DEER ? 1 : 2;
    Vector3 p = Vector3Add(o, Vector3Scale(d, best));
    fx_burst(FX_BLOOD, p, Vector3Negate(d), 6);
    if (a->health <= 0) { a->dead = true; sfx(S_BODYFALL, a->pos, 0.7f, 0.8f); }
    if (t_out) *t_out = best;
    return true;
}

int deer_near(Vector3 p, float r)
{
    for (int i = 0; i < MAX_ANIM; i++)
        if (an[i].used && an[i].dead && !an[i].butchered && an[i].kind == AN_DEER && dist_xz(an[i].pos, p) < r) return i;
    return -1;
}

void deer_butcher(int i)
{
    if (i < 0 || i >= MAX_ANIM) return;
    an[i].butchered = true;
    ped_play(PLAYER, A_INTERACT);
    fx_burst(FX_BLOOD, an[i].pos, v3(0, 1, 0), 10);
}

void raiders_draw(int pass)
{
    for (int i = 0; i < MAX_ANIM; i++) {
        Animal *a = &an[i];
        if (!a->used) continue;
        if (Vector3DistanceSqr(a->pos, cam.position) > (pass == PASS_SHADOW ? 60 * 60 : 200 * 200)) continue;
        Matrix m;
        if (a->dead) m = MatrixMultiply(MatrixMultiply(MatrixRotateZ(PI * 0.5f), MatrixRotateY(a->yaw)), MatrixTranslate(a->pos.x, a->pos.y + 0.3f, a->pos.z));
        else {
            float bob = fabsf(sinf(a->step * 3.0f)) * 0.06f * fminf(1, hypotf(a->vel.x, a->vel.z));
            m = MatrixMultiply(MatrixRotateY(a->yaw), MatrixTranslate(a->pos.x, a->pos.y + bob, a->pos.z));
        }
        if (a->butchered) m = MatrixMultiply(MatrixScale(0.7f, 0.45f, 0.7f), m);
        draw_mesh(prop_mesh[a->kind == AN_DEER ? PROP_DEER : PROP_WOLF], m, WHITE, MAT_PROP);
    }
    // bodies of the dead people's things are containers; nothing else to draw
}

void raiders_shift(Vector3 d)
{
    for (int i = 0; i < MAX_ANIM; i++) an[i].pos = Vector3Subtract(an[i].pos, d);
}

void raiders_reset(void)
{
    memset(an, 0, sizeof an);
    for (int i = 0; i < MAX_STOPS; i++) populated[i] = -1;
    event_t = 120;
    anim_spawn_t = 8;
    threat = 0;
}

// ---------------------------------------------------------------- per tick

void raiders_tick(float dt)
{
    shout_cd -= dt;
    // how dangerous the road is here and now
    Vector3 pp = player_pos();
    float s = road_s(pp);
    float km = sv.km;
    threat = Clamp(0.08f + km * 0.014f + (night_now() ? 0.18f : 0) + biome_mix(s, BIOME_BADLANDS) * 0.15f + biome_mix(s, BIOME_DESERT) * 0.05f, 0, 1);
    if (difficulty == DIFF_PEACEFUL) threat = 0;
    if (difficulty == DIFF_HARD) threat = fminf(1, threat * 1.55f + 0.12f);
    // people at the stops
    for (int i = 0; i < MAX_STOPS; i++) {
        if (!stops[i].used) { populated[i] = -1; continue; }
        if (populated[i] != stops[i].seg && dist_xz(stops[i].p, pp) < 260) populate(i);
    }
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used) continue;
        float d = dist_xz(p->pos, pp);
        if (p->kind == PED_COP && difficulty == DIFF_PEACEFUL) { ped_remove(i); continue; }
        if (p->kind == PED_COP) raider_foot(i, dt);
        // people who wandered out of range are let go
        if (d > 300 && p->state != PS_IN_CAR) ped_remove(i);
        else if (p->state == PS_DEAD && pv[i].dead_s > 240) ped_remove(i);
    }
    if (difficulty == DIFF_PEACEFUL) {
        for (int i = 0; i < MAX_CARS; i++) if (cars[i].used && cars[i].ai == AI_COP && cars[i].driver != PLAYER) car_remove(i);
    } else raider_cars(dt);
    // highway encounters
    event_t -= dt;
    if (event_t <= 0) {
        event_t = frand(70, 160) / (0.5f + threat * tune_raiders);
        Ped *pl = &peds[PLAYER];
        bool driving = pl->car >= 0 && car_kmh(&cars[pl->car]) > 25;
        int active = 0;
        for (int i = 0; i < MAX_CARS; i++) if (cars[i].used && cars[i].ai == AI_COP) active++;
        if (difficulty != DIFF_PEACEFUL && world_kind != WORLD_CITY && driving && active < 2 && sv.km > 2.5f && frand(0, 1) < (0.25f + threat * 0.6f) * tune_raiders) {
            if (raider_car_spawn(frand(0, 1) < 0.3f) >= 0) {
                hud_msg("Headlights in the mirror. Raiders!", 3);
                if (threat > 0.55f && frand(0, 1) < 0.4f) raider_car_spawn(false);
            }
        }
    }
    if (difficulty == DIFF_PEACEFUL) {
        for (int i = 0; i < MAX_ANIM; i++) if (an[i].kind == AN_WOLF) an[i].used = false;
    }
    if (world_kind != WORLD_CITY) animals_tick(dt);
}
