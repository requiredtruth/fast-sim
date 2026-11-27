#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ped.h"

Ped peds[MAX_PEDS];

#define WALKERS 45
#define SPEED_WALK 1.5f
#define SPEED_JOG 3.5f
#define SPEED_SPRINT 5.8f
#define SPEED_CROUCH 1.6f
#define SPEED_AIM 2.0f
#define JUMP_MS 5.6f
#define FALL_G 22.0f
#define STEP_UP_M 0.45f
#define DOOR_REACH_M 5.0f

static bool stolen[MAX_CARS];
static int boot_ticks;
static float scream_cd;

// ---- looks

static Color rgb(int r, int g, int b) { return (Color){ r, g, b, 255 }; }

static const Color skins[] = {
    { 255, 224, 200, 255 }, { 241, 200, 168, 255 }, { 224, 172, 130, 255 }, { 198, 142, 100, 255 },
    { 160, 106, 72, 255 }, { 124, 80, 56, 255 }, { 92, 58, 42, 255 }, { 70, 44, 34, 255 },
};
static const Color shirts[] = {
    { 235, 235, 230, 255 }, { 30, 30, 34, 255 }, { 120, 124, 130, 255 }, { 28, 44, 84, 255 }, { 172, 38, 40, 255 },
    { 214, 168, 52, 255 }, { 36, 120, 124, 255 }, { 52, 110, 60, 255 }, { 214, 112, 40, 255 }, { 220, 140, 160, 255 },
    { 80, 120, 176, 255 }, { 110, 30, 48, 255 }, { 120, 120, 70, 255 }, { 100, 60, 140, 255 }, { 224, 210, 180, 255 },
    { 150, 190, 220, 255 },
};
static const Color pants[] = {
    { 52, 72, 112, 255 }, { 70, 92, 132, 255 }, { 38, 52, 84, 255 }, { 152, 132, 98, 255 }, { 28, 28, 32, 255 },
    { 84, 86, 92, 255 }, { 96, 98, 66, 255 }, { 88, 62, 44, 255 }, { 170, 160, 140, 255 },
};
static const Color shoes[] = { { 226, 226, 222, 255 }, { 24, 24, 26, 255 }, { 82, 52, 32, 255 }, { 110, 110, 114, 255 } };
static const Color hairs[] = {
    { 24, 20, 18, 255 }, { 46, 32, 24, 255 }, { 88, 58, 36, 255 }, { 188, 152, 92, 255 }, { 128, 58, 34, 255 },
    { 150, 150, 152, 255 }, { 16, 14, 14, 255 },
};
#define PICK(a) (a[rnd() % (sizeof a / sizeof a[0])])

static void make_look(Ped *p)
{
    static const int male_hair[] = { HAIR_BUZZ, HAIR_PARTED, HAIR_BEARD, HAIR_BUZZ, HAIR_PARTED };
    static const int female_hair[] = { HAIR_LONG, HAIR_BUNS, HAIR_PARTED, HAIR_LONG, HAIR_BUNS };
    p->body = rnd() & 1;
    p->hair = p->body == BODY_MALE ? male_hair[rnd() % 5] : female_hair[rnd() % 5];
    p->pal[R_SKIN] = PICK(skins);
    p->pal[R_SHIRT] = PICK(shirts);
    p->pal[R_PANTS] = PICK(pants);
    p->pal[R_SHOES] = PICK(shoes);
    p->pal[R_HAIR] = PICK(hairs);
    p->pal[R_EYES] = rgb(34, 24, 20);
}

static void cop_look(Ped *p)
{
    p->body = BODY_MALE;
    p->hair = HAIR_CAP;
    p->pal[R_SKIN] = PICK(skins);
    p->pal[R_SHIRT] = rgb(24, 34, 62);
    p->pal[R_PANTS] = rgb(20, 26, 44);
    p->pal[R_SHOES] = rgb(18, 18, 20);
    p->pal[R_HAIR] = rgb(22, 30, 54);
    p->pal[R_EYES] = rgb(34, 24, 20);
}

// ---- small helpers

static float angle_diff(float from, float to)
{
    float d = fmodf(to - from + PI, 2 * PI);
    if (d < 0) d += 2 * PI;
    return d - PI;
}

static float turn_to(float yaw, float want, float rate, float dt)
{
    float m = rate * dt;
    return yaw + Clamp(angle_diff(yaw, want), -m, m);
}

static float dist_xz(Vector3 a, Vector3 b) { return hypotf(a.x - b.x, a.z - b.z); }

static Vector3 approach(Vector3 v, Vector3 to, float step)
{
    Vector3 d = Vector3Subtract(to, v);
    float len = Vector3Length(d);
    if (len <= step) return to;
    return Vector3Add(v, Vector3Scale(d, step / len));
}

static bool in_view(Vector3 p)
{
    Vector3 d = Vector3Subtract(p, cam.position), f = Vector3Subtract(cam.target, cam.position);
    float dl = Vector3Length(d), fl = Vector3Length(f);
    if (dl < 1 || fl < 0.01f) return true;
    return Vector3DotProduct(d, f) / (dl * fl) > 0.45f;
}

static bool afoot(const Ped *p)
{
    return p->used && p->state != PS_IN_CAR && p->state != PS_RAGDOLL && p->state != PS_DEAD && p->state != PS_ENTERING;
}

static Vector3 chest(const Ped *p)
{
    if (pv[p - peds].rag_on) return peds[p - peds].rag[1];
    return Vector3Add(p->pos, (Vector3){ 0, 1.2f, 0 });
}

// ---- cars: door spots

static Vector3 door_world(int c, float out)
{
    const CarModel *m = &car_models[cars[c].model];
    float sx = m->seat.x >= 0 ? 1 : -1;
    return Vector3Transform((Vector3){ sx * (m->size.x * 0.5f + out), 0, m->seat.z }, car_matrix(&cars[c]));
}

static float car_yaw(const Car *c)
{
    Vector3 f = car_fwd(c);
    return atan2f(f.x, f.z);
}

static void seat_update(int i, float dt)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    Car *c = &cars[p->car];
    const CarModel *m = &car_models[c->model];
    Vector3 seat = Vector3Transform((Vector3){ m->seat.x, m->seat.y - SEAT_DROP, m->seat.z }, car_matrix(c));
    v->sit = fminf(1, v->sit + dt / 0.22f);
    float k = v->sit * v->sit * (3 - 2 * v->sit);
    p->pos = Vector3Lerp(v->sit_from, seat, k);
    p->vel = c->vel;
    p->yaw = car_yaw(c);
}

// ---- spawn, remove, find

int ped_spawn(int kind, Vector3 pos, float yaw)
{
    int i = 1;
    while (i < MAX_PEDS && peds[i].used) i++;
    if (i == MAX_PEDS) return -1;
    Ped *p = &peds[i];
    memset(p, 0, sizeof *p);
    memset(&pv[i], 0, sizeof pv[i]);
    p->used = true;
    p->kind = kind;
    p->state = PS_IDLE;
    p->pos = pos;
    p->yaw = yaw;
    p->health = 100;
    p->car = -1; p->node = p->next_node = -1; p->target = -1;
    if (kind == PED_COP) cop_look(p); else make_look(p);
    Priv *v = &pv[i];
    v->scale = frand(0.94f, 1.04f);
    v->walk_speed = frand(1.15f, 1.65f);
    v->side_off = frand(0.3f, 0.9f);
    v->grounded = true;
    v->sit = 1;
    v->shot = -1; v->getup = -1; v->enter_car = -1;
    v->last_pos = pos;
    p->anim = p->prev_anim = A_IDLE;
    p->blend = 1;
    v->rate = v->prev_rate = 1;
    p->anim_t = frand(0, 2);
    return i;
}

void ped_remove(int i)
{
    if (i <= 0 || i >= MAX_PEDS || !peds[i].used) return;
    Ped *p = &peds[i];
    if (p->car >= 0 && cars[p->car].driver == i) cars[p->car].driver = -1;
    p->used = false;
    memset(&pv[i], 0, sizeof pv[i]);
}

int ped_at(Vector3 at, float r, int ignore)
{
    int best = -1;
    float bd = r;
    for (int i = 0; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used || i == ignore || p->state == PS_DEAD || p->health <= 0) continue;
        float d = Vector3Distance(at, chest(p));
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// ---- hurting people

static void die(int i, Vector3 impulse, int attacker)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    if (p->state == PS_DEAD) return;
    if (p->car >= 0) {
        if (cars[p->car].driver == i) cars[p->car].driver = -1;
        p->car = -1;
    }
    p->health = 0;
    p->state = PS_DEAD;
    p->aiming = false;
    v->dead_s = 0;
    rag_begin(i, impulse, (Vector3){0});
    if (i == PLAYER) mode = MODE_DEAD;
    else if (frand(0, 1) < 0.5f) {
        if (ticks & 1) voice(S_V_SCREAM, p->pos, 1, i);
        else sfx(S_SCREAM, p->pos, 0.7f, frand(0.8f, 1.2f));
    }
}

static void knock_down(int i, Vector3 impulse, float secs)
{
    Ped *p = &peds[i];
    p->state = PS_RAGDOLL;
    pv[i].down_s = secs;
    p->panic = 1;
    rag_begin(i, impulse, (Vector3){0});
}

void ped_damage(int i, float dmg, Vector3 impulse, int attacker, int weapon)
{
    if (i < 0 || i >= MAX_PEDS) return;
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    if (!p->used || p->state == PS_DEAD || (dmg <= 0 && Vector3Length(impulse) < 0.1f)) return;
    if (p->armor > 0) {
        float a = fminf(p->armor, dmg * 0.7f);
        p->armor -= a;
        dmg -= a;
    }
    p->health -= dmg;
    Vector3 dir = Vector3Length(impulse) > 0.1f ? Vector3Normalize(impulse) : (Vector3){ 0, 1, 0 };
    if (dmg > 0) fx_burst(FX_BLOOD, chest(p), dir, 3 + (int)(dmg * 0.15f));
    float imp = Vector3Length(impulse);
    if (p->health <= 0) { die(i, impulse, attacker); return; }
    if (dmg > 2) voice(i != PLAYER && p->body == BODY_FEMALE ? S_V_SCREAM : S_V_GRUNT, p->pos, 1, i == PLAYER ? -1 : i);
    if (p->car >= 0 && p->state == PS_IN_CAR) { p->panic = 1; return; }
    if (imp > 7.5f && p->state != PS_RAGDOLL) {
        knock_down(i, impulse, 1.3f + 0.5f * frand(0, 1) + imp * 0.04f);
        return;
    }
    if (p->state == PS_RAGDOLL) return;
    if (i != PLAYER && weapon > W_FIST && afoot(p)) {
        if (v->flinch_s <= 0) {
            v->flinch_s = 0.60f;
            p->vel = (Vector3){ 1.8f * dir.x, 0, 1.8f * dir.z };
            p->aiming = false;
            ped_play(i, A_HIT_CHEST);
            v->shot_full = true;
        }
    } else ped_play(i, frand(0, 1) < 0.2f ? A_HIT_HEAD : A_HIT_CHEST);
    if (p->kind == PED_CIV && attacker >= 0) {
        p->panic = 1;
        v->src = peds[attacker].pos;
        if (frand(0, 1) < 0.15f && weapon == W_FIST) { p->state = PS_FIGHT; p->target = attacker; v->state_s = 0; }
        else if (p->state != PS_FLEE) { p->state = PS_FLEE; p->node = p->next_node = -1; v->state_s = 0; }
    }
}

void ped_alarm(Vector3 at, float radius, int source)
{
    int screams = 0;
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used || p->kind != PED_CIV) continue;
        float d = dist_xz(p->pos, at);
        if (d > radius) continue;
        float k = 1 - d / radius;
        p->panic = fmaxf(p->panic, 0.5f + 0.5f * k);
        pv[i].src = at;
        if (p->state == PS_IN_CAR || p->state == PS_COWER || p->state == PS_CALLING) continue;
        if (p->state == PS_WALK || p->state == PS_IDLE || p->state == PS_FLEE || p->state == PS_FIGHT) {
            int r = rnd() % 100;
            if (p->state == PS_FLEE) continue;
            pv[i].wait = false;
            pv[i].state_s = 0;
            if (k > 0.35f && r < 28) { p->state = PS_COWER; p->timer = frand(1.2f, 2.8f); }
            else { p->state = PS_FLEE; p->node = p->next_node = -1; }
            if (screams < 2 && scream_cd <= 0 && r > 50) {
                if (r % 3 == 0) sfx(S_SCREAM, p->pos, 0.8f, frand(0.85f, 1.3f));
                else voice(r % 3 == 1 ? S_V_HELP : S_V_COPS, p->pos, 1, i);
                screams++;
                scream_cd = 0.3f;
            }
        }
    }
}

// ---- getting into and out of cars

static void leave_car(int i, bool panic)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    int ci = p->car;
    Car *c = &cars[ci];
    Vector3 d = door_world(ci, 0.7f);
    d.y = ground_y(d.x, d.z);
    world_push(&d, 0.32f, 1.75f);
    float spd = fabsf(car_speed(c));
    p->pos = d;
    p->vel = (Vector3){ c->vel.x * (spd > 7 ? 0.55f : 0.25f), 0, c->vel.z * (spd > 7 ? 0.55f : 0.25f) };
    p->yaw = car_yaw(c);
    if (c->driver == i) c->driver = -1;
    p->car = -1;
    v->exit_cd = 0.5f;
    v->grounded = true; v->vy = 0; v->sit = 0;
    v->last_pos = d;
    anim_to(i, A_IDLE, 0);
    if (i == PLAYER) {
        c->throttle = c->brake = c->steer = 0;
        c->handbrake = spd < 3;
        c->horn = false;
        p->state = PS_IDLE;
        if (spd > 7) {
            p->state = PS_EXITING;
            v->state_s = 0.75f;
            ped_play(i, A_ROLL);
            if (spd > 12) ped_damage(i, (spd - 12) * 2.5f, (Vector3){0}, -1, -1);
        }
    } else {
        p->state = panic ? PS_FLEE : PS_WALK;
        p->node = p->next_node = -1;
        if (panic) { p->panic = 1; v->src = c->pos; }
    }
    sfx(S_DOOR, d, 0.6f, frand(0.95f, 1.05f));
}

void ped_exit_car(int i, bool panic)
{
    if (i > 0 && i < MAX_PEDS && peds[i].used && peds[i].car >= 0 && cars[peds[i].car].used) leave_car(i, panic);
}

static void jack(int d, int ci)
{
    Ped *vp = &peds[d];
    Car *c = &cars[ci];
    Vector3 door = door_world(ci, 0.7f);
    door.y = ground_y(door.x, door.z);
    Vector3 out = Vector3Subtract(door, c->pos);
    out.y = 0;
    out = Vector3Normalize(out);
    Vector3 shift = Vector3Subtract(door, vp->pos);
    shift.y = 0;
    c->driver = -1;
    vp->car = -1;
    vp->state = PS_RAGDOLL;
    vp->vel = (Vector3){ c->vel.x * 0.4f, 0, c->vel.z * 0.4f };
    rag_begin(d, (Vector3){ out.x * 3.2f, 1.0f, out.z * 3.2f }, shift);
    pv[d].down_s = 1.5f + frand(0, 1);
    vp->panic = 1;
    vp->target = frand(0, 1) < 0.2f ? PLAYER : -1;
    pv[d].src = peds[PLAYER].pos;
    voice(S_V_MYCAR, vp->pos, 1, d);
}

static void board(int i)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    int ci = v->enter_car;
    if (ci < 0 || !cars[ci].used || cars[ci].health <= 0) { p->state = PS_IDLE; return; }
    Car *c = &cars[ci];
    int d = c->driver;
    if (d >= 0 && d != i && peds[d].used) {
        jack(d, ci);
        if (i == PLAYER) crime(CR_CARJACK, c->pos, d);
    } else if (i == PLAYER && !stolen[ci] && c->ai != AI_NONE) crime(CR_CAR_THEFT, c->pos, -1);
    c->driver = i;
    p->car = ci;
    p->seat = 0;
    p->state = PS_IN_CAR;
    v->sit = 0;
    v->sit_from = p->pos;
    v->enter_car = -1;
    v->state_s = 0;
    if (i == PLAYER) { c->ai = AI_NONE; stolen[ci] = true; }
    sfx(S_DOOR, p->pos, 0.6f, 1);
}

static void try_enter(int i)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    int ci = car_near(p->pos, DOOR_REACH_M);
    if (ci < 0 || !cars[ci].used || cars[ci].health <= 0 || fabsf(car_speed(&cars[ci])) > 9) return;
    const CarModel *m = &car_models[cars[ci].model];
    float hw = m->size.x * 0.5f, hl = m->size.z * 0.5f, sx = m->seat.x >= 0 ? 1 : -1;
    Vector3 lp = Vector3Transform(p->pos, MatrixInvert(car_matrix(&cars[ci])));
    v->nwp = 0; v->wpi = 0;
    Vector3 door = { sx * (hw + 0.55f), 0, m->seat.z };
    if (lp.x * sx < hw && fabsf(lp.z) < hl + 0.5f) {
        float ez = lp.z >= 0 ? 1 : -1, side = lp.x >= 0 ? 1 : -1;
        v->wp[v->nwp++] = (Vector3){ side * (hw + 0.7f), 0, ez * (hl + 0.8f) };
        v->wp[v->nwp++] = (Vector3){ sx * (hw + 0.7f), 0, ez * (hl + 0.8f) };
    }
    v->wp[v->nwp++] = door;
    v->enter_car = ci;
    v->state_s = 0;
    p->state = PS_ENTERING;
    p->aiming = false;
}

static void entering(int i, float dt)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    int ci = v->enter_car;
    if (ci < 0 || !cars[ci].used) { p->state = PS_IDLE; return; }
    v->state_s += dt;
    Vector3 t = Vector3Transform(v->wp[v->wpi], car_matrix(&cars[ci]));
    t.y = p->pos.y;
    float d = dist_xz(p->pos, t);
    if (d < 0.45f) {
        if (++v->wpi >= v->nwp) { board(i); return; }
    } else {
        float step = fminf(d, 6.5f * dt);
        p->pos.x += (t.x - p->pos.x) / d * step;
        p->pos.z += (t.z - p->pos.z) / d * step;
        p->yaw = turn_to(p->yaw, atan2f(t.x - p->pos.x, t.z - p->pos.z), 18, dt);
    }
    p->vel = Vector3Scale(Vector3Normalize(Vector3Subtract(t, p->pos)), d > 0.45f ? 6.5f : 0);
    p->pos.y = Lerp(p->pos.y, ground_y(p->pos.x, p->pos.z), fminf(1, 12 * dt));
    world_push(&p->pos, 0.32f, 1.75f);
    if (v->state_s > 1.8f) board(i);   // wedged somewhere, just get in
}

// ---- the player

static void foot_steps(int i)
{
    Priv *v = &pv[i];
    int ph = (int)floorf(v->gait * 2);
    if (ph != v->step_ph && v->grounded && v->speed > 0.8f)
        sfx(S_STEP, peds[i].pos, i == PLAYER ? 0.22f : 0.1f, frand(0.85f, 1.2f) + v->speed * 0.03f);
    v->step_ph = ph;
}

static void land(Ped *p, Priv *v)
{
    v->grounded = true;
    if (v->vy < -11) ped_damage(PLAYER, (-v->vy - 11) * 9, (Vector3){0}, -1, -1);
    if (v->vy < -2 && !is_water(p->pos.x, p->pos.z)) { fx_burst(FX_DUST, p->pos, (Vector3){ 0, 1, 0 }, 4); sfx(S_STEP, p->pos, 0.4f, 0.7f); }
    if (v->speed < 1.8f && v->vy < -3) v->land_s = 0.3f;
    v->vy = 0;
}

static void player_foot(const Input *in, float dt)
{
    Ped *p = &peds[PLAYER];
    Priv *v = &pv[PLAYER];
    bool water = is_water(p->pos.x, p->pos.z);
    bool armed = p->weapon != W_FIST;
    p->aiming = armed && weapon_reach == 0 && (in->held & BTN_AIM);
    p->aim_pitch = cam_pitch;
    v->crouch = (in->held & BTN_CROUCH) != 0 && v->grounded;

    float mag = fminf(1, hypotf(in->mx, in->my));
    Vector3 fwd = { sinf(cam_yaw), 0, cosf(cam_yaw) }, right = { -cosf(cam_yaw), 0, sinf(cam_yaw) };
    Vector3 wish = Vector3Add(Vector3Scale(fwd, in->my), Vector3Scale(right, in->mx));
    if (Vector3Length(wish) > 1) wish = Vector3Normalize(wish);

    float want_speed = 0;
    bool sprint = false;
    if (mag > 0.1f) {
        if (v->crouch) want_speed = SPEED_CROUCH;
        else if (p->aiming) want_speed = SPEED_AIM;
        else if ((in->held & BTN_SPRINT) && mag > 0.5f) { want_speed = SPEED_SPRINT; sprint = true; }
        else if (mag < 0.6f) want_speed = SPEED_WALK * (mag / 0.6f);
        else want_speed = SPEED_JOG;
        if (water) want_speed = fminf(want_speed, 1.5f);
    }
    float accel = !v->grounded ? 7 : sprint ? 14 : want_speed > Vector3Length(p->vel) ? 22 : 30;
    Vector3 hv = { p->vel.x, 0, p->vel.z };
    Vector3 wd = Vector3Length(wish) > 0.01f ? Vector3Normalize(wish) : (Vector3){0};
    hv = approach(hv, Vector3Scale(wd, want_speed), accel * dt);
    p->vel.x = hv.x; p->vel.z = hv.z;
    float hs = Vector3Length(hv);

    float want_yaw = p->yaw;
    if (p->aiming) want_yaw = cam_yaw;
    else if (mag > 0.1f) want_yaw = atan2f(wd.x, wd.z);
    p->yaw = turn_to(p->yaw, want_yaw, p->aiming ? 16 : sprint ? 7 : 11, dt);

    p->pos.x += hv.x * dt;
    p->pos.z += hv.z * dt;
    p->pos.x = Clamp(p->pos.x, -WORLD_HALF + 2, WORLD_HALF - 2);
    p->pos.z = Clamp(p->pos.z, -WORLD_HALF + 2, WORLD_HALF - 2);
    world_push(&p->pos, 0.32f, 1.75f);
    cars_push(&p->pos, 0.35f);

    if ((in->hit & BTN_JUMP) && v->grounded && !v->crouch && !water) {
        v->vy = JUMP_MS;
        v->grounded = false;
        v->air_s = 0;
        anim_to(PLAYER, A_JUMP_START, 0.28f);
        v->rate = 1.8f;
    }

    float gy = ground_y(p->pos.x, p->pos.z);
    float reach = water ? 2.3f : STEP_UP_M;
    if (v->grounded) {
        if (gy > p->pos.y + 0.001f) {
            if (gy - p->pos.y <= reach) p->pos.y = fminf(gy, p->pos.y + 9 * dt);
        } else if (gy < p->pos.y - 0.001f) {
            if (p->pos.y - gy <= STEP_UP_M) p->pos.y = fmaxf(gy, p->pos.y - 10 * dt);
            else { v->grounded = false; v->vy = 0; v->air_s = 0.3f; }
        }
        if (gy > p->pos.y + 0.001f && gy - p->pos.y > reach) {   // a wall of ground: back out
            p->pos.x -= hv.x * dt; p->pos.z -= hv.z * dt;
        }
    } else {
        float before = p->pos.y;
        v->vy -= FALL_G * dt;
        p->pos.y += v->vy * dt;
        v->air_s += dt;
        if (before > -0.9f && p->pos.y <= -0.9f && water) {
            fx_burst(FX_WATER, (Vector3){ p->pos.x, -0.9f, p->pos.z }, (Vector3){ 0, 1, 0 }, 14);
            v->vy *= 0.5f;
        }
        if (p->pos.y <= gy && v->vy <= 0) { p->pos.y = gy; land(p, v); }
    }
    if (water && v->grounded && p->pos.y < -1) {
        v->drown_s += dt;
        if (v->drown_s > 5) ped_damage(PLAYER, 1000, (Vector3){0}, -1, -1);
    } else v->drown_s = fmaxf(0, v->drown_s - dt * 2);

    v->speed = hs;
    v->land_s = fmaxf(0, v->land_s - dt);
    if (!v->grounded) {
        anim_to(PLAYER, v->air_s < 0.22f && v->vy > 0 ? A_JUMP_START : A_JUMP_LOOP, 0);
        if (p->anim == A_JUMP_LOOP) v->rate = 1;
    } else if (v->land_s > 0) {
        anim_to(PLAYER, A_JUMP_LAND, 0.1f);
        v->rate = 2.2f;
    } else {
        bool back = p->aiming && hs > 0.5f && Vector3DotProduct(Vector3Normalize(hv), (Vector3){ sinf(p->yaw), 0, cosf(p->yaw) }) < -0.3f;
        locomote(PLAYER, hs, v->crouch, back);
    }
    foot_steps(PLAYER);
    if ((in->hit & BTN_ENTER) && v->grounded && v->exit_cd <= 0 && !water) try_enter(PLAYER);
}

static void player_car(const Input *in, float dt)
{
    Ped *p = &peds[PLAYER];
    Priv *v = &pv[PLAYER];
    Car *c = &cars[p->car];
    seat_update(PLAYER, dt);
    c->steer = in->mx;
    c->throttle = fmaxf(in->my, 0);
    c->brake = fmaxf(-in->my, 0);
    c->handbrake = (in->held & BTN_JUMP) != 0;
    c->horn = (in->held & BTN_HORN) != 0;
    p->aiming = false;
    v->speed = 0;
    anim_to(PLAYER, A_DRIVE, 0);
    v->rate = 0.35f;
    if ((in->hit & BTN_ENTER) && v->sit > 0.5f) leave_car(PLAYER, false);
}

static void handle_rag(int i, float dt);

static void player_tick(const Input *in0, float dt)
{
    Ped *p = &peds[PLAYER];
    Priv *v = &pv[PLAYER];
    Input none = {0};
    const Input *in = (mode == MODE_PLAY || mode == MODE_MAP) ? in0 : &none;
    v->hit_cd -= dt; v->exit_cd -= dt;

    if (v->rag_on && p->state != PS_RAGDOLL && p->state != PS_DEAD) { v->rag_on = false; v->getup = -1; v->grounded = true; }
    if (p->health <= 0 && p->state != PS_DEAD) die(PLAYER, (Vector3){0}, -1);
    if (p->car >= 0 && (!cars[p->car].used || cars[p->car].health <= 0) && p->state == PS_IN_CAR) {
        int ci = p->car;
        if (cars[ci].used) { Vector3 up = { 0, 5, 0 }; leave_car(PLAYER, false); ped_damage(PLAYER, 40, up, -1, -1); }
        else { p->car = -1; p->state = PS_IDLE; }
    }
    switch (p->state) {
    case PS_IDLE: case PS_WALK:
        player_foot(in, dt);
        break;
    case PS_ENTERING:
        entering(PLAYER, dt);
        v->speed = Vector3Length(p->vel);
        locomote(PLAYER, v->speed, false, false);
        break;
    case PS_IN_CAR:
        player_car(in, dt);
        break;
    case PS_EXITING:
        v->state_s -= dt;
        p->vel = Vector3Scale(p->vel, expf(-2.5f * dt));
        p->pos = Vector3Add(p->pos, Vector3Scale(p->vel, dt));
        world_push(&p->pos, 0.32f, 1.75f);
        p->pos.y = ground_y(p->pos.x, p->pos.z);
        v->speed = 0;
        if (v->state_s <= 0) p->state = PS_IDLE;
        break;
    case PS_RAGDOLL: case PS_DEAD:
        handle_rag(PLAYER, dt);
        break;
    default:   // arrested, hands up: the cops module moves us
        p->vel = (Vector3){0};
        v->speed = 0;
        anim_to(PLAYER, A_IDLE, 0);
        v->rate = 1;
        break;
    }
}

// ---- walking the sidewalk graph

static int pick_node(int cur, int prev, bool flee, Vector3 src)
{
    const WalkNode *w = &walk[cur];
    int cand[4], n = 0;
    for (int k = 0; k < w->n; k++) {
        if (w->nb[k] == prev && w->n > 1) continue;
        cand[n++] = w->nb[k];
    }
    if (n == 0) return prev;
    if (flee && rnd() % 4 != 0) {
        int best = cand[0];
        for (int k = 1; k < n; k++)
            if (dist_xz(walk[cand[k]].p, src) > dist_xz(walk[best].p, src)) best = cand[k];
        return best;
    }
    return cand[rnd() % n];
}

static bool crossing(int a, int b) { return walk[a].light >= 0 && walk[a].light == walk[b].light; }

static void ground_follow(Ped *p, float dt)
{
    float gy = ground_y(p->pos.x, p->pos.z);
    p->pos.y = fabsf(gy - p->pos.y) < 0.6f ? Lerp(p->pos.y, gy, fminf(1, 14 * dt)) : gy;
}

static void foot_clamp(Ped *p, float dt)
{
    world_push(&p->pos, 0.3f, 1.7f);
    cars_push(&p->pos, 0.32f);
    ground_follow(p, dt);
}

static void graph_move(int i, float dt, float speed, bool flee)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    if (p->next_node < 0 || p->next_node >= nwalk) {
        int n = nearest_walk(p->pos, 60);
        if (n < 0) { if (i > 0) ped_remove(i); return; }
        p->next_node = n; p->node = -1;
        v->wait = false;
    }
    if (v->wait) {
        if (walk_light_ok(walk[p->node].light)) v->wait = false;
        else {
            Vector3 dir = Vector3Subtract(walk[p->next_node].p, walk[p->node].p);
            p->yaw = turn_to(p->yaw, atan2f(dir.x, dir.z), 6, dt);
            p->vel = (Vector3){0};
            return;
        }
    }
    Vector3 np = walk[p->next_node].p, a = p->node >= 0 ? walk[p->node].p : p->pos;
    Vector3 dir = { np.x - a.x, 0, np.z - a.z };
    if (Vector3Length(dir) < 0.01f) dir = (Vector3){ 0, 0, 1 };
    dir = Vector3Normalize(dir);
    Vector3 tp = { np.x - dir.z * v->side_off, 0, np.z + dir.x * v->side_off };
    float d = dist_xz(p->pos, tp);
    if (dist_xz(p->pos, np) < 1.2f || d < 0.4f) {
        int here = p->next_node, nxt = pick_node(here, p->node, flee, v->src);
        p->node = here;
        p->next_node = nxt;
        if (!flee) {
            if (crossing(here, nxt) && !walk_light_ok(walk[here].light)) v->wait = true;
            else if (rnd() % 100 < 5 && p->state == PS_WALK) {
                p->state = PS_IDLE;
                p->timer = frand(3, 9);
                v->idle_kind = rnd() % 3 == 0 ? 2 : 0;
                p->vel = (Vector3){0};
            }
        }
        return;
    }
    Vector3 want = { (tp.x - p->pos.x) / d * speed, 0, (tp.z - p->pos.z) / d * speed };
    Vector3 hv = approach((Vector3){ p->vel.x, 0, p->vel.z }, want, (flee ? 16 : 7) * dt);
    p->vel = hv;
    p->pos.x += hv.x * dt;
    p->pos.z += hv.z * dt;
    if (Vector3Length(hv) > 0.2f) p->yaw = turn_to(p->yaw, atan2f(hv.x, hv.z), flee ? 12 : 7, dt);
    foot_clamp(p, dt);
    // wedged against something: hop to the next node
    if (v->speed < 0.2f * speed) { v->state_s += dt; if (v->state_s > 3) { p->pos.x = np.x; p->pos.z = np.z; v->state_s = 0; } }
    else if (!flee) v->state_s = 0;
}

static void after_getup(int i)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    v->rag_on = false; v->getup = -1; v->grounded = true; v->state_s = 0;
    p->node = p->next_node = -1;
    if (i == PLAYER) { p->state = PS_IDLE; return; }
    if (p->kind == PED_COP) { p->state = PS_CHASE; return; }
    if (p->target == PLAYER) { p->state = PS_FIGHT; return; }
    p->state = PS_FLEE;
    p->panic = 1;
}

static void handle_rag(int i, float dt)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    if (!v->rag_on) rag_begin(i, (Vector3){0}, (Vector3){0});
    v->hit_cd -= dt;
    if (v->getup >= 0) {
        v->getup += dt / 0.9f;
        if (v->getup >= 1) after_getup(i);
        return;
    }
    rag_tick(i);
    if (p->state == PS_DEAD) { v->dead_s += dt; return; }
    v->down_s -= dt;
    if (v->down_s <= 0 && v->rag_s > 0.6f && Vector3Length(p->vel) < 2.5f) getup_begin(i);
}

static void idle_civ(int i, float dt)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    p->timer -= dt;
    p->vel = (Vector3){0};
    if (v->idle_kind == 1 && p->target >= 0 && peds[p->target].used && peds[p->target].state == PS_IDLE) {
        Vector3 d = Vector3Subtract(peds[p->target].pos, p->pos);
        p->yaw = turn_to(p->yaw, atan2f(d.x, d.z), 6, dt);
        anim_to(i, A_TALK, frand(0, 1));
    } else {
        if (v->idle_kind == 1) v->idle_kind = 0;
        anim_to(i, A_IDLE, 0);
    }
    v->rate = 1;
    if (p->timer <= 0) { p->state = PS_WALK; v->idle_kind = 0; }
    ground_follow(p, dt);
}

static void fight_civ(int i, float dt)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    v->state_s += dt;
    p->fire_cd -= dt;
    Ped *t = p->target >= 0 ? &peds[p->target] : NULL;
    if (!t || !t->used || t->state == PS_DEAD || v->state_s > 12) { p->state = PS_FLEE; p->node = p->next_node = -1; return; }
    Vector3 d = Vector3Subtract(t->pos, p->pos);
    float dist = hypotf(d.x, d.z);
    p->yaw = turn_to(p->yaw, atan2f(d.x, d.z), 10, dt);
    if (dist > 1.1f && v->shot < 0) {
        Vector3 want = Vector3Scale(Vector3Normalize((Vector3){ d.x, 0, d.z }), 3.2f);
        p->vel = approach(p->vel, want, 14 * dt);
        p->pos = Vector3Add(p->pos, Vector3Scale(p->vel, dt));
        foot_clamp(p, dt);
    } else {
        p->vel = (Vector3){0};
        if (dist <= 1.4f && p->fire_cd <= 0) {
            ped_play(i, rnd() & 1 ? A_PUNCH_JAB : A_PUNCH_CROSS);
            p->fire_cd = 1.2f;
            sfx(S_PUNCH, t->pos, 0.7f, frand(0.9f, 1.1f));
            ped_damage(p->target, 5, (Vector3){ d.x, 0, d.z }, i, W_FIST);
        }
    }
}

static void npc_tick(int i, float dt)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    v->hit_cd -= dt; v->exit_cd -= dt;
    p->panic = fmaxf(0, p->panic - 0.07f * dt);
    if (p->health <= 0 && p->state != PS_DEAD) die(i, (Vector3){0}, -1);
    v->flinch_s = fmaxf(0, v->flinch_s - dt);
    if (staggering(i)) {
        p->pos = Vector3Add(p->pos, Vector3Scale(p->vel, dt));
        p->vel = Vector3Scale(p->vel, fmaxf(0, 1 - 16 * dt));
        foot_clamp(p, dt);
        return;
    }
    switch (p->state) {
    case PS_IN_CAR:
        if (p->car < 0 || !cars[p->car].used || cars[p->car].driver != i) {
            if (p->car >= 0 && cars[p->car].used) leave_car(i, true);
            else { p->car = -1; p->state = PS_WALK; }
            break;
        }
        seat_update(i, dt);
        if (cars[p->car].health <= 0) { ped_damage(i, 300, (Vector3){ 0, 6, 0 }, -1, -1); break; }
        if (p->kind == PED_CIV && p->panic > 0.6f && fabsf(car_speed(&cars[p->car])) < 5) {
            v->state_s += dt;
            if (v->state_s > 0.5f + 0.4f * (i % 4)) { ped_exit_car(i, true); v->state_s = 0; }
        } else v->state_s = 0;
        break;
    case PS_RAGDOLL: case PS_DEAD:
        handle_rag(i, dt);
        break;
    case PS_WALK:
        if (p->kind != PED_CIV) { foot_clamp(p, dt); break; }
        graph_move(i, dt, v->walk_speed, false);
        break;
    case PS_IDLE:
        if (p->kind == PED_CIV) idle_civ(i, dt); else foot_clamp(p, dt);
        break;
    case PS_FLEE:
        v->state_s += dt;
        if (p->kind != PED_CIV) { foot_clamp(p, dt); break; }
        graph_move(i, dt, v->walk_speed * (p->panic > 0.5f ? 3.6f : 2.2f), true);
        if (p->panic < 0.12f || v->state_s > 16) { p->state = PS_WALK; p->node = p->next_node = -1; v->state_s = 0; }
        break;
    case PS_COWER:
        p->timer -= dt;
        p->vel = (Vector3){0};
        if (p->timer <= 0) { p->state = PS_FLEE; p->node = p->next_node = -1; v->state_s = 0; }
        break;
    case PS_FIGHT:
        if (p->kind == PED_CIV) fight_civ(i, dt); else foot_clamp(p, dt);
        break;
    default:   // calling, chase, arrest, hands up: somebody else moves them
        foot_clamp(p, dt);
        break;
    }
}

// pick the clip for whoever isn't the player
static void auto_anim(int i)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    if (v->rag_on) return;
    switch (p->state) {
    case PS_IN_CAR: anim_to(i, A_DRIVE, 0); v->rate = 0.35f; return;
    case PS_COWER: anim_to(i, A_COWER, 0); v->rate = 1; return;
    case PS_HANDS_UP: anim_to(i, A_IDLE, 0); v->rate = 1; return;
    case PS_CALLING: anim_to(i, A_TALK, 0); v->rate = 1; return;
    case PS_ARREST: anim_to(i, A_IDLE, 0); v->rate = 1; return;
    case PS_IDLE: if (p->kind == PED_CIV) return; break;
    default: break;
    }
    if (p->state == PS_WALK && v->wait) { anim_to(i, A_IDLE, 0); v->rate = 1; return; }
    locomote(i, v->speed, false, false);
    foot_steps(i);
}

// ---- people against people, people against cars

static void separate(void)
{
    int ids[MAX_PEDS], n = 0;
    for (int i = 0; i < MAX_PEDS; i++)
        if (afoot(&peds[i]) && peds[i].state != PS_EXITING) ids[n++] = i;
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++) {
            Ped *p = &peds[ids[a]], *q = &peds[ids[b]];
            float dx = q->pos.x - p->pos.x, dz = q->pos.z - p->pos.z, d2 = dx * dx + dz * dz;
            if (d2 > 0.5f * 0.5f || fabsf(p->pos.y - q->pos.y) > 1) continue;
            float d = sqrtf(d2);
            if (d < 0.001f) { dx = 0.01f; dz = 0; d = 0.01f; }
            float push = (0.5f - d) * 0.35f;
            float pa = 0.5f, pb = 0.5f;
            if (ids[a] == PLAYER) { pa = 0; pb = 1; }
            q->pos.x += dx / d * push * pb * 2; q->pos.z += dz / d * push * pb * 2;
            p->pos.x -= dx / d * push * pa * 2; p->pos.z -= dz / d * push * pa * 2;
        }
}

// ---- keeping a crowd around

static bool spawn_walker(bool boot)
{
    Vector3 pp = peds[PLAYER].pos;
    for (int tries = 0; tries < 6; tries++) {
        float a = frand(0, 2 * PI), r = boot ? frand(10, 120) : frand(70, 140);
        int n = nearest_walk((Vector3){ pp.x + sinf(a) * r, 0, pp.z + cosf(a) * r }, 22);
        if (n < 0) continue;
        Vector3 np = walk[n].p;
        float d = dist_xz(np, pp);
        if (d < (boot ? 12 : 60) || d > 150) continue;
        if (!boot && d < 110 && in_view(np)) continue;
        bool crowded = false;
        for (int k = 1; k < MAX_PEDS && !crowded; k++)
            if (peds[k].used && dist_xz(peds[k].pos, np) < 1.5f) crowded = true;
        if (crowded) continue;
        int i = ped_spawn(PED_CIV, (Vector3){ np.x + frand(-0.6f, 0.6f), np.y, np.z + frand(-0.6f, 0.6f) }, frand(0, 2 * PI));
        if (i < 0) return false;
        Ped *p = &peds[i];
        p->state = PS_WALK;
        p->node = -1;
        p->next_node = n;
        if (rnd() % 100 < 8) {   // a pair stood talking
            int j = ped_spawn(PED_CIV, (Vector3){ np.x + 0.9f, np.y, np.z }, 0);
            if (j >= 0) {
                float t = frand(25, 70);
                peds[i].state = peds[j].state = PS_IDLE;
                peds[i].timer = peds[j].timer = t;
                peds[i].target = j; peds[j].target = i;
                pv[i].idle_kind = pv[j].idle_kind = 1;
                peds[i].yaw = PI * 0.5f; peds[j].yaw = -PI * 0.5f;
                peds[j].node = peds[i].node; peds[j].next_node = n;
            }
        }
        return true;
    }
    return false;
}

static void crowd(void)
{
    Vector3 pp = peds[PLAYER].pos;
    int count = 0;
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used || p->kind != PED_CIV) continue;
        Priv *v = &pv[i];
        if (p->state == PS_IN_CAR || p->state == PS_ENTERING) continue;
        float d = dist_xz(p->pos, pp);
        bool seen = in_view(p->pos);
        bool dead = p->state == PS_DEAD;
        if (d > 170 || (d > 120 && !seen) || (dead && v->dead_s > (seen && d < 40 ? 45 : 30))) { ped_remove(i); continue; }
        if (!dead) count++;
    }
    int want = boot_ticks < 100 ? 8 : 2;
    for (int k = 0; k < want && count < WALKERS; k++)
        if (spawn_walker(boot_ticks < 100)) count++;
}

static void player_init(void)
{
    Ped *p = &peds[PLAYER];
    Vector3 pos = p->pos;
    float hp = p->health;
    memset(p, 0, sizeof *p);
    memset(&pv[PLAYER], 0, sizeof pv[PLAYER]);
    p->used = true;
    p->kind = PED_PLAYER;
    p->state = PS_IDLE;
    p->pos = pos;
    p->health = hp > 0 ? hp : 100;
    p->car = -1; p->node = p->next_node = -1; p->target = -1;
    p->body = BODY_MALE;
    p->hair = HAIR_BUZZ;
    p->pal[R_SKIN] = skins[3];
    p->pal[R_SHIRT] = rgb(200, 66, 44);
    p->pal[R_PANTS] = rgb(44, 58, 92);
    p->pal[R_SHOES] = rgb(226, 226, 222);
    p->pal[R_HAIR] = hairs[6];
    p->pal[R_EYES] = rgb(34, 24, 20);
    Priv *v = &pv[PLAYER];
    v->scale = 1;
    v->grounded = true;
    v->shot = -1; v->getup = -1; v->enter_car = -1;
    v->last_pos = pos;
    p->anim = p->prev_anim = A_IDLE;
    p->blend = 1;
    v->rate = v->prev_rate = 1;
}



static void near_misses(void)
{
    for (int ci = 0; ci < MAX_CARS; ci++) {
        Car *c = &cars[ci];
        if (!c->used || Vector3Length(c->vel) < 8) continue;
        for (int i = 1; i < MAX_PEDS; i++) {
            Ped *p = &peds[i];
            if (!p->used || p->kind != PED_CIV || (p->state != PS_WALK && p->state != PS_IDLE)) continue;
            if (dist_xz(p->pos, c->pos) < 2.4f) voice(S_V_WATCH, p->pos, 1, i);
        }
    }
}

void peds_tick(const Input *in, float dt)
{
    if (!peds[PLAYER].used) player_init();
    scream_cd -= dt;
    boot_ticks++;
    player_tick(in, dt);
    for (int i = 1; i < MAX_PEDS; i++)
        if (peds[i].used) npc_tick(i, dt);
    for (int ci = 0; ci < MAX_CARS; ci++)
        if (!cars[ci].used) stolen[ci] = false;
    separate();
    crowd();
    if (ticks % 6 == 0) near_misses();
    for (int i = 0; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used) continue;
        Priv *v = &pv[i];
        if (i != PLAYER) {
            v->speed = fminf(9, dist_xz(p->pos, v->last_pos) / dt) * 0.5f + v->speed * 0.5f;
            auto_anim(i);
        }
        v->last_pos = p->pos;
        anim_step(i, dt);
        ped_build(i);
    }
}
