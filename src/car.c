#include "city.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CAR_TEST
#include <time.h>
#endif

// ped.c has the real one; this keeps the build alive without it
__attribute__((weak)) void ped_exit_car(int i, bool panic)
{
    peds[i].car = -1;
    peds[i].state = PS_FLEE;
}

Car cars[MAX_CARS];

#define G 9.81f
#define S_REST 0.25f     // hardpoint to wheel centre at rest
#define S_MAX 0.42f      // fully extended
#define SUBSTEPS 2
#define TRAFFIC_TARGET 48
#define PARKED_TARGET 12

typedef struct { float vmax, accel, grip; } Tune;

static const Tune tune[CAR_MODELS] = {
    [CAR_SEDAN] = { 52, 5.2f, 1.10f }, [CAR_COMPACT] = { 42, 4.4f, 1.10f }, [CAR_HATCH] = { 45, 4.8f, 1.10f },
    [CAR_MINIVAN] = { 38, 3.6f, 1.00f }, [CAR_PICKUP] = { 41, 4.0f, 1.00f }, [CAR_SUV] = { 44, 4.2f, 1.05f },
    [CAR_WAGON] = { 48, 4.6f, 1.10f }, [CAR_OFFROAD] = { 40, 4.0f, 1.05f }, [CAR_COP] = { 58, 6.0f, 1.15f },
    [CAR_TAXI] = { 47, 4.8f, 1.10f },
};

static const float gear_frac[5] = { 0.22f, 0.40f, 0.58f, 0.78f, 1.02f };

// per-car state that doesn't need to be in the shared struct
typedef struct {
    float steer_s;               // eased steering input, +1 is right
    float spin_w[4];
    float prev_c[4];
    float slip[4];
    int gear; float shift_s;
    float crash_cd;
    bool asleep; float still_s;
    bool water, exploded, no_rev;
    float burn_s, trip_cd; int attacker;
    float smoke_t;
    // ai
    int route[4];
    float speed_mul, go_wait, blocked_s, honk_s, honk_cd, rev_s, repick_s;
    bool light_hold, flee_init;
    float yield_free_s;
    Vector3 tgt_last, tgt_vel;
    uint64_t drive_tick, tgt_tick;
    bool test_ctl;
    float nomove_s;
} Ex;

static Ex ex[MAX_CARS];
static float ped_hit_cd[MAX_PEDS];
static bool popped;

// ---------------------------------------------------------------- small helpers

static Vector3 V(float x, float y, float z) { return (Vector3){ x, y, z }; }
static Vector3 qrot(Quaternion q, Vector3 v) { return Vector3RotateByQuaternion(v, q); }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float dot2(Vector3 a, Vector3 b) { return a.x * b.x + a.z * b.z; }
static float len2(Vector3 a) { return sqrtf(a.x * a.x + a.z * a.z); }

static Vector3 norm2(Vector3 a)
{
    float l = len2(a);
    return l > 1e-5f ? V(a.x / l, 0, a.z / l) : V(0, 0, 1);
}

static Vector3 left_h(Vector3 f) { return V(f.z, 0, -f.x); }

Vector3 car_fwd(const Car *c) { return qrot(c->rot, V(0, 0, 1)); }
float car_speed(const Car *c) { return Vector3DotProduct(c->vel, car_fwd(c)); }

static float com_y(const Car *c) { return car_models[c->model].size.y * 0.35f; }

static Vector3 origin_of(const Car *c) { return Vector3Subtract(c->pos, qrot(c->rot, V(0, com_y(c), 0))); }

Matrix car_matrix(const Car *c)
{
    Vector3 o = origin_of(c);
    if (!isfinite(o.x + o.y + o.z + c->rot.x + c->rot.y + c->rot.z + c->rot.w)) return MatrixIdentity();
    return MatrixMultiply(QuaternionToMatrix(c->rot), MatrixTranslate(o.x, o.y, o.z));
}

static float half_w(const Car *c) { return car_models[c->model].size.x * 0.5f * 0.96f; }
static float half_l(const Car *c) { return car_models[c->model].size.z * 0.5f; }

static void inertia(const Car *c, float *ix, float *iy, float *iz)
{
    const CarModel *m = &car_models[c->model];
    float w = m->size.x, h = m->size.y, l = m->size.z, k = m->mass_kg / 12;
    *ix = k * (h * h + l * l);
    *iy = k * (w * w + l * l) * 0.75f;
    *iz = k * (w * w + h * h);
}

static Vector3 focus(void)
{
    const Ped *p = &peds[PLAYER];
    if (p->car >= 0 && cars[p->car].used) return cars[p->car].pos;
    return p->pos;
}

// rough frustum test with some margin, so spawning and despawning happen off screen
static bool in_view(Vector3 p, float far_m)
{
    Vector3 d = Vector3Subtract(p, cam.position);
    float l = Vector3Length(d);
    if (l < 12) return true;
    if (l > far_m) return false;
    Vector3 f = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, cam.up)), u = Vector3CrossProduct(r, f);
    float z = Vector3DotProduct(d, f);
    if (z < -6) return false;
    z = fmaxf(z, 1);
    float th = tanf(cam.fovy * DEG2RAD * 0.5f);
    return fabsf(Vector3DotProduct(d, r)) < z * th * 1.9f + 8 && fabsf(Vector3DotProduct(d, u)) < z * th * 1.3f + 8;
}

static bool night(void) { return day_hours < 6.0f || day_hours > 18.7f; }

// ---------------------------------------------------------------- paint

static const struct { Color c; int w; } paints[] = {
    { { 236, 236, 236, 255 }, 22 }, { { 24, 24, 26, 255 }, 20 }, { { 168, 172, 178, 255 }, 18 },
    { { 88, 92, 98, 255 }, 14 }, { { 22, 36, 76, 255 }, 8 }, { { 150, 24, 28, 255 }, 8 },
    { { 28, 70, 48, 255 }, 3 }, { { 196, 178, 140, 255 }, 2 }, { { 220, 176, 30, 255 }, 2 },
    { { 214, 98, 24, 255 }, 1 }, { { 70, 130, 190, 255 }, 1 }, { { 92, 60, 40, 255 }, 1 },
};

static Color random_paint(void)
{
    int total = 0;
    for (int i = 0; i < 12; i++) total += paints[i].w;
    int r = (int)(rnd() % total);
    for (int i = 0; i < 12; i++) {
        if (r < paints[i].w) {
            Color c = paints[i].c;
            int j = (int)(rnd() % 9) - 4;
            c.r = (unsigned char)clampf(c.r + j, 0, 255); c.g = (unsigned char)clampf(c.g + j, 0, 255); c.b = (unsigned char)clampf(c.b + j, 0, 255);
            return c;
        }
        r -= paints[i].w;
    }
    return WHITE;
}

static int random_model(void)
{
    static const int tab[] = { CAR_SEDAN, CAR_SEDAN, CAR_SEDAN, CAR_COMPACT, CAR_COMPACT, CAR_HATCH, CAR_HATCH, CAR_HATCH,
                               CAR_WAGON, CAR_WAGON, CAR_MINIVAN, CAR_PICKUP, CAR_PICKUP, CAR_SUV, CAR_SUV, CAR_OFFROAD,
                               CAR_TAXI, CAR_SEDAN, CAR_HATCH, CAR_COMPACT };
    return tab[rnd() % (sizeof tab / sizeof *tab)];
}

// ---------------------------------------------------------------- spawn / remove

int car_spawn(int model, Vector3 p, float yaw, int ai)
{
    int id = -1;
    for (int i = 0; i < MAX_CARS; i++) if (!cars[i].used) { id = i; break; }
    if (id < 0 || model < 0 || model >= CAR_MODELS) return -1;
    Car *c = &cars[id];
    memset(c, 0, sizeof *c);
    memset(&ex[id], 0, sizeof ex[id]);
    c->used = true;
    c->model = model;
    c->ai = ai;
    c->driver = -1;
    c->lane = -1;
    c->target = -1;
    c->health = 1000;
    c->rot = QuaternionFromAxisAngle(V(0, 1, 0), yaw);
    float gy = ground_y(p.x, p.z);
    Vector3 o = V(p.x, fmaxf(p.y, gy), p.z);
    c->pos = Vector3Add(o, qrot(c->rot, V(0, com_y(c), 0)));
    c->paint = (model == CAR_COP || model == CAR_TAXI) ? WHITE : random_paint();
    for (int w = 0; w < 4; w++) { c->wheel_ground[w] = true; ex[id].prev_c[w] = S_MAX - S_REST; }
    ex[id].asleep = ai == AI_PARKED;
    ex[id].speed_mul = frand(0.88f, 1.12f);
    for (int i = 0; i < 4; i++) ex[id].route[i] = -1;
    c->handbrake = ai == AI_PARKED;
    return id;
}

void car_remove(int ci)
{
    if (ci < 0 || ci >= MAX_CARS || !cars[ci].used) return;
    Car *c = &cars[ci];
    if (c->driver == PLAYER) peds[PLAYER].car = -1;
    else if (c->driver >= 0 && peds[c->driver].used && peds[c->driver].car == ci) ped_remove(c->driver);
    for (int w = 0; w < 4; w++) fx_skid(ci * 4 + w, c->pos, 0.2f, false);
    c->used = false;
}

int car_near(Vector3 p, float r)
{
    int best = -1;
    float bd = r * r;
    for (int i = 0; i < MAX_CARS; i++) {
        if (!cars[i].used) continue;
        float d = Vector3DistanceSqr(cars[i].pos, p);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// ---------------------------------------------------------------- 2d box overlap

typedef struct { Vector3 c, f, l; float hl, hw; } Foot;

static Foot foot_of(const Car *c)
{
    Vector3 f = norm2(car_fwd(c));
    return (Foot){ c->pos, f, left_h(f), half_l(c), half_w(c) };
}

// overlap of two oriented rectangles in xz. n points from b to a. false when apart
static bool sat(const Foot *a, const Foot *b, Vector3 *n, float *depth)
{
    Vector3 axes[4] = { a->f, a->l, b->f, b->l };
    Vector3 d = Vector3Subtract(a->c, b->c);
    float best = 1e9f;
    for (int i = 0; i < 4; i++) {
        Vector3 ax = axes[i];
        float ra = a->hl * fabsf(dot2(a->f, ax)) + a->hw * fabsf(dot2(a->l, ax));
        float rb = b->hl * fabsf(dot2(b->f, ax)) + b->hw * fabsf(dot2(b->l, ax));
        float dist = dot2(d, ax);
        float ov = ra + rb - fabsf(dist);
        if (ov <= 0) return false;
        if (ov < best) { best = ov; *n = dist < 0 ? V(-ax.x, 0, -ax.z) : ax; }
    }
    *depth = best;
    return true;
}

static bool inside(const Foot *f, Vector3 p)
{
    Vector3 d = Vector3Subtract(p, f->c);
    return fabsf(dot2(d, f->f)) < f->hl && fabsf(dot2(d, f->l)) < f->hw;
}

static void corners(const Foot *f, Vector3 *out)
{
    int k = 0;
    for (int i = -1; i <= 1; i += 2)
        for (int j = -1; j <= 1; j += 2)
            out[k++] = V(f->c.x + f->f.x * f->hl * i + f->l.x * f->hw * j, 0, f->c.z + f->f.z * f->hl * i + f->l.z * f->hw * j);
}

// ---------------------------------------------------------------- effects and damage

static void crash_fx(Vector3 at, Vector3 n, float speed)
{
    if (speed < 2.5f) return;
    int big = speed > 11;
    sfx(big ? S_CRASH_BIG : S_CRASH_SMALL, at, clampf(speed / 18, 0.3f, 1), frand(0.9f, 1.1f));
    fx_burst(FX_SPARK, at, n, (int)clampf(speed * 1.6f, 4, 34));
    if (speed > 7) fx_burst(FX_DEBRIS, at, n, (int)(speed * 0.5f));
    if (speed > 10) { fx_burst(FX_GLASS, at, n, 10); fx_shake(clampf(speed / 35, 0.1f, 0.8f)); }
    if (speed > 6) ped_alarm(at, 18 + speed, -1);
}

static void explode_car(int ci)
{
    Car *c = &cars[ci];
    Ex *x = &ex[ci];
    if (x->exploded) return;
    x->exploded = true;
    Vector3 p = Vector3Add(c->pos, V(0, 0.6f, 0));
    explode(p, 8, x->attacker);
    fx_burst(FX_EXPLOSION, p, V(0, 1, 0), 1);
    float d = Vector3Distance(p, cam.position);
    fx_shake(clampf(1.4f - d / 60, 0.1f, 1.2f));
    sfx(S_EXPLOSION, p, 1, 1);
    if (c->driver >= 0 && c->driver != PLAYER) {
        if (peds[c->driver].used && peds[c->driver].car == ci) ped_damage(c->driver, 1000, V(0, 4, 0), x->attacker, -1);
        c->driver = -1;
    } else if (c->driver == PLAYER) ped_damage(PLAYER, 1000, V(0, 4, 0), x->attacker, -1);
    c->ai = AI_WRECK;
    c->health = 0;
    c->siren = false; c->horn = false; c->throttle = c->brake = 0;
    c->handbrake = false;
    c->paint = (Color){ 22, 21, 20, 255 };
    c->vel.y += 6.5f;
    c->vel.x += frand(-2, 2); c->vel.z += frand(-2, 2);
    c->angvel = Vector3Add(c->angvel, V(frand(-1.5f, 1.5f), frand(-1, 1), frand(-2, 2)));
    x->asleep = false;
    x->burn_s = 0;
}

void car_damage(int ci, float dmg, Vector3 at, int attacker)
{
    if (ci < 0 || ci >= MAX_CARS || !cars[ci].used || dmg <= 0) return;
    Car *c = &cars[ci];
    if (attacker >= 0) ex[ci].attacker = attacker;
    ex[ci].asleep = false;
    if (c->ai == AI_WRECK) return;
    c->health -= dmg;
    if (c->health <= 0) { c->health = 0; explode_car(ci); }
}

static void bail_out(int ci)
{
    Car *c = &cars[ci];
    int d = c->driver;
    if (d < 0 || d == PLAYER) return;
    if (peds[d].used && peds[d].car == ci) {
        ped_exit_car(d, true);
    }
    c->driver = -1;
}

// ---------------------------------------------------------------- physics

typedef struct { bool hit; float s; } Gnd;

static Gnd ground_hit(Vector3 hp, Vector3 up, float wheel_r)
{
    Gnd r = { false, 0 };
    if (up.y < 0.15f) return r;
    float gy = ground_y(hp.x, hp.z);
    float t = (hp.y - gy) / up.y;
    Vector3 q = Vector3Subtract(hp, Vector3Scale(up, t));
    if (is_water(q.x, q.z)) return r;
    gy = ground_y(q.x, q.z);
    t = (hp.y - gy) / up.y;
    r.s = t - wheel_r;
    r.hit = r.s < S_MAX && r.s > -0.6f;
    return r;
}

static float max_steer(float v, float grip)
{
    float a = grip * G * 1.35f * 2.8f / fmaxf(v * v, 1);
    return clampf(a, 0.05f, 0.55f);
}

static float lock_of(const Car *c, float v)
{
    if (c->driver != PLAYER) return max_steer(v, tune[c->model].grip);
    const CarModel *m = &car_models[c->model];
    float wb = fabsf(m->wheel_pos[0].z - m->wheel_pos[2].z);
    // a slide shouldn't bring back parking-speed steering lock
    float speed2 = dot2(c->vel, c->vel);
    return atanf(wb / (3.75f + speed2 / (0.93f * G * tune[c->model].grip)));
}

static float curve(float x) { return sinf(1.5f * atanf(1.8f * x)); }

static void apply(Vector3 *f, Vector3 *t, Vector3 force, Vector3 r)
{
    *f = Vector3Add(*f, force);
    *t = Vector3Add(*t, Vector3CrossProduct(r, force));
}

static void drive_gear(Car *c, Ex *x, float v, float thr, bool spinning, float dt)
{
    const Tune *tn = &tune[c->model];
    float gv = tn->vmax * gear_frac[x->gear];
    if (x->shift_s > 0) x->shift_s -= dt;
    if (x->shift_s <= 0) {
        if (x->gear < 4 && v > gv * 0.93f && thr > 0.05f) { x->gear++; x->shift_s = 0.2f; }
        else if (x->gear > 0 && v < tn->vmax * gear_frac[x->gear - 1] * 0.74f) x->gear--;
    }
    gv = tn->vmax * gear_frac[x->gear];
    float u = fmaxf(v / gv, x->gear == 0 ? thr * 0.4f : 0);
    float target = 0.16f + 0.84f * clampf(u, 0, 1.05f);
    if (thr < 0.05f) target = 0.16f + (target - 0.16f) * 0.85f;
    if (spinning) target = fmaxf(target, 0.8f);
    if (x->shift_s > 0) target *= 0.8f;
    float rate = target > c->rpm ? 9.0f : 13.0f;
    c->rpm += (target - c->rpm) * fminf(1, rate * dt);
    c->rpm = clampf(c->rpm, 0.1f, 1);
}

static void step_car(int ci, float dt)
{
    Car *c = &cars[ci];
    Ex *x = &ex[ci];
    const CarModel *m = &car_models[c->model];
    const Tune *tn = &tune[c->model];
    float mass = m->mass_kg, mw = mass / 4;
    Quaternion q = c->rot;
    Vector3 up = qrot(q, V(0, 1, 0)), fwd = qrot(q, V(0, 0, 1));
    Vector3 force = V(0, -mass * G, 0), torque = V(0, 0, 0);
    float cy = com_y(c);

    float vlong_body = Vector3DotProduct(c->vel, fwd);
    float speed = len2(c->vel);
    bool player = c->driver == PLAYER;

    // inputs
    float thr = clampf(c->throttle, 0, 1), brk = clampf(c->brake, 0, 1);
    if (x->water || c->ai == AI_WRECK) thr = brk = 0;
    float power = c->health < 150 ? 0.5f : c->health < 400 ? 0.8f : 1;
    bool reverse = false;
    if (brk > 0.05f && thr < 0.05f && vlong_body < 0.6f && !c->handbrake && !x->no_rev) reverse = true;
    float steer_in = clampf(c->steer, -1, 1);
    if (player) {
        steer_in *= 0.5f + 0.5f * fabsf(steer_in);
        float fast = clampf((speed - 5) / 25, 0, 1);
        float rate = steer_in == 0 ? 12 - 3 * fast : 17 - 8 * fast;
        x->steer_s += (steer_in - x->steer_s) * fminf(rate * dt, 1);
    } else {
        bool turning_in = fabsf(steer_in) > fabsf(x->steer_s) || steer_in * x->steer_s < 0;
        float step = (turning_in ? 5.5f : 8.0f) * dt;
        x->steer_s += clampf(steer_in - x->steer_s, -step, step);
    }
    Vector3 right = qrot(q, V(1, 0, 0));
    float side_speed = Vector3DotProduct(c->vel, right);
    float slide = player ? clampf((fabsf(side_speed) / fmaxf(speed, 3) - 0.08f) * 3, 0, 1) : 0;
    float delta = -x->steer_s * lock_of(c, fabsf(vlong_body));
    if (vlong_body > 3 && !c->handbrake) {
        float beta = atan2f(side_speed, vlong_body);
        delta += clampf(beta * (player ? 0.45f + 0.1f * slide : 0.55f), -0.22f, 0.22f);
        delta = clampf(delta, -0.6f, 0.6f);
    }

    // engine force along the car
    float fdrive = 0;
    float vabs = fabsf(vlong_body);
    bool braking_now = false;
    float vm = tn->vmax * 1.05f;
    if (reverse) fdrive = -0.38f * tn->accel * mass * brk * clampf(1 - vabs * vabs / 110, 0, 1);
    else if (thr > 0.05f && vlong_body > -0.6f && x->shift_s <= 0.1f) {
        float f = tn->accel * mass * thr * clampf(1 - vlong_body * vlong_body / (vm * vm), 0, 1);
        float boost = 1 + 0.35f * (1 - x->gear / 4.0f);
        fdrive = f * boost * power;
    } else if (thr > 0.05f && vlong_body <= -0.6f) braking_now = true;
    if (player && !c->handbrake && fdrive > 0) {
        float corner = fabsf(x->steer_s) * clampf((speed - 4) / 8, 0, 1);
        fdrive *= 1 - 0.34f * corner - 0.18f * slide;
    }
    if (brk > 0.05f && vlong_body >= 0.6f) braking_now = true;
    if (brk > 0.05f && vlong_body <= -0.6f && !reverse) braking_now = true;   // rolling back with nobody reversing
    float bforce = braking_now ? (vlong_body < 0 ? fmaxf(thr, reverse ? 0 : brk) : brk) * 10.0f * mass : 0;
    if (c->handbrake && !braking_now) bforce = 0;

    // suspension geometry
    Vector3 hp[4], hv[4];
    float cl[4], N[4];
    bool ground[4];
    for (int w = 0; w < 4; w++) {
        Vector3 ml = V(m->wheel_pos[w].x, m->wheel_pos[w].y + S_REST - cy, m->wheel_pos[w].z);
        Vector3 rw = qrot(q, ml);
        hp[w] = Vector3Add(c->pos, rw);
        hv[w] = Vector3Add(c->vel, Vector3CrossProduct(c->angvel, rw));
        Gnd r = ground_hit(hp[w], up, m->wheel_r);
        ground[w] = r.hit;
        cl[w] = r.hit ? S_MAX - r.s : 0;
        N[w] = 0;
    }
    float k = mass * G / 4 / (S_MAX - S_REST);
    float cd = 0.55f * 2 * sqrtf(k * mw);
    for (int w = 0; w < 4; w++) {
        if (!ground[w]) continue;
        float cdot = c->wheel_ground[w] ? (cl[w] - x->prev_c[w]) / dt : 0;
        float cc = fminf(cl[w], S_MAX);
        float f = k * cc + (cl[w] > S_MAX ? k * 9 * (cl[w] - S_MAX) : 0) + cd * cdot;
        N[w] = f;
    }
    // anti-roll bars
    for (int a = 0; a < 4; a += 2) {
        if (!ground[a] || !ground[a + 1]) continue;
        float d = (cl[a] - cl[a + 1]) * k * 0.6f;
        N[a] += d; N[a + 1] -= d;
    }

    float side_load = 0;
    bool spinning = false;
    float maxskid = 0;
    for (int w = 0; w < 4; w++) {
        bool front = w < 2;
        c->wheel_ground[w] = ground[w];
        // wheel_comp: how far the wheel has moved up from its rest position
        float s_now = ground[w] ? S_MAX - fminf(cl[w], S_MAX) : S_MAX;
        float comp = S_REST - s_now;
        c->wheel_comp[w] += (comp - c->wheel_comp[w]) * fminf(1, 30 * dt);
        x->prev_c[w] = cl[w];
        if (!ground[w] || N[w] <= 0) {
            x->spin_w[w] *= 1 - 0.3f * dt;
            if (c->handbrake && w >= 2) x->spin_w[w] = 0;
            x->slip[w] = 0;
            continue;
        }
        float Nw = N[w];
        side_load += Nw;
        Vector3 rl = Vector3Subtract(hp[w], c->pos);
        force.y += Nw;
        torque = Vector3Add(torque, Vector3CrossProduct(rl, V(0, Nw, 0)));

        // wheel frame on the ground plane
        Vector3 fh = norm2(fwd);
        if (front) {
            float cs = cosf(delta), sn = sinf(delta);
            fh = V(fh.x * cs + left_h(fh).x * sn, 0, fh.z * cs + left_h(fh).z * sn);
        }
        Vector3 lh = left_h(fh);
        float vl = dot2(hv[w], fh), vs = dot2(hv[w], lh);
        float mu = tn->grip * (front ? 1.05f : 1.0f);
        float fmax = mu * Nw;
        Vector3 f2 = V(0, 0, 0);
        Vector3 rlow = V(rl.x, rl.y * 0.2f, rl.z);
        bool locked = c->handbrake && !front;
        float vh = sqrtf(vl * vl + vs * vs);
        if (locked) {
            float fk = fminf(0.72f * fmax, mw * vh / dt);
            if (vh > 1e-3f) f2 = V(-(fh.x * vl + lh.x * vs) / vh * fk, 0, -(fh.z * vl + lh.z * vs) / vh * fk);
            x->spin_w[w] = 0;
            x->slip[w] = vh > 3 ? 1.5f : 0;
        } else {
            float fx = 0;
            if (!front) fx += fdrive * 0.5f;
            if (bforce > 0) {
                float share = front ? 0.325f : 0.175f;
                float fb = bforce * share;
                float cap = mw * fabsf(vl) / dt;
                fx -= (vl > 0 ? 1 : -1) * fminf(fb, cap);
            }
            bool sat_l = fabsf(fx) > fmax * 0.98f;
            if (sat_l) { fx = fx > 0 ? fmax * 0.98f : -fmax * 0.98f; }
            if (sat_l && fdrive != 0 && !front && vabs < 18) spinning = true;
            float lat_cap = fmax * sqrtf(fmaxf(0.1f, 1 - (fx / fmax) * (fx / fmax)));
            float slip_t = fabsf(vs) / fmaxf(fabsf(vl), 2.0f);
            float xs = slip_t / (front ? 0.17f : 0.2f);
            float fy = lat_cap * curve(xs);
            float stop = mw * fabsf(vs) / dt;
            fy = fminf(fy, stop);
            fy = vs > 0 ? -fy : fy;
            f2 = V(fh.x * fx + lh.x * fy, 0, fh.z * fx + lh.z * fy);
            x->slip[w] = sqrtf(xs * xs + (sat_l ? 1.4f * 1.4f : 0));
            if (!sat_l && x->slip[w] < 0.02f) x->slip[w] = 0;
            float roll = vl / m->wheel_r;
            x->spin_w[w] = (sat_l && fdrive != 0 && !front) ? (vl + 7) / m->wheel_r : (sat_l && bforce > 0 ? 0 : roll);
        }
        apply(&force, &torque, f2, rlow);
        // a sliding tyre that meets a kerb digs in and can flip the car
        if (fabsf(vs) > 6) {
            float sgn = vs > 0 ? 1 : -1;
            Vector3 q = V(hp[w].x + lh.x * sgn * 0.5f, 0, hp[w].z + lh.z * sgn * 0.5f);
            if (ground_y(q.x, q.z) - ground_y(hp[w].x, hp[w].z) > 0.06f && ground_y(q.x, q.z) < 1) {
                float ft = fminf(mw * (fabsf(vs) - 4) / dt * 0.25f, 0.9f * mw * G * (fabsf(vs) - 4));
                Vector3 tf = V(-lh.x * sgn * ft, 0, -lh.z * sgn * ft);
                Vector3 rtrip = V(rl.x, -cy, rl.z);
                apply(&force, &torque, tf, rtrip);
                if (x->trip_cd <= 0) {
                    x->trip_cd = 0.5f;
                    float k = clampf((fabsf(vs) - 4) * 0.28f, 0, 6);
                    c->angvel = Vector3Add(c->angvel, Vector3Scale(fwd, -sgn * k));
                }
#ifdef CAR_TEST
                if (getenv("CAR_DBG6")) printf("trip w%d vs %.1f ft %.0f angvel %.2f %.2f %.2f up %.2f\n", w, vs, ft, c->angvel.x, c->angvel.y, c->angvel.z, up.y);
#endif
                if (x->crash_cd <= 0 && fabsf(vs) > 8) { x->crash_cd = 0.3f; crash_fx(V(hp[w].x, 0.2f, hp[w].z), V(0, 1, 0), fabsf(vs) * 0.5f); }
            }
        }
        float sk = (x->slip[w] - 1.0f) / 0.7f;
        if (vh < 2.5f && !(locked || spinning)) sk = 0;
        if (sk > maxskid) maxskid = sk;
    }
    (void)side_load;
    if (player && (ground[0] || ground[1]) && (ground[2] || ground[3])) {
        // let the tail swing with the handbrake and settle once it's released
        float damp = c->handbrake ? 0.55f : 1.15f + 4 * slide;
        torque.y -= c->angvel.y * damp * mass;
        if (!c->handbrake && vlong_body > 0)
            force = Vector3Subtract(force, Vector3Scale(left_h(norm2(fwd)), mass * side_speed * slide * 1.35f));
    }
    c->skid += (clampf(maxskid, 0, 1) - c->skid) * fminf(1, 14 * dt);

    // drag, rolling resistance, engine braking
    float vh2 = len2(c->vel);
    if (vh2 > 0.01f) {
        float drag = 0.32f * vh2 * vh2 + 90 + (thr < 0.05f && !reverse ? mass * 0.05f : 0);
        drag = fminf(drag, mass * vh2 / dt * 0.5f);
        Vector3 d = Vector3Scale(V(c->vel.x, 0, c->vel.z), -drag / vh2);
        if (c->wheel_ground[0] || c->wheel_ground[1] || c->wheel_ground[2] || c->wheel_ground[3]) force = Vector3Add(force, d);
        else force = Vector3Add(force, Vector3Scale(d, 0.3f));
    }

    // body against the ground: floor and roof corners
    Vector3 ctr = V(0, m->size.y * 0.5f - cy, 0);
    for (int i = 0; i < 8; i++) {
        Vector3 l = V(((i & 1) ? 1 : -1) * m->size.x * 0.46f, ctr.y + ((i & 2) ? 1 : -1) * m->size.y * 0.5f * ((i & 2) ? 1.0f : 0.78f),
                      ((i & 4) ? 1 : -1) * m->size.z * 0.5f);
        Vector3 rl = qrot(q, l);
        Vector3 p = Vector3Add(c->pos, rl);
        float gy = ground_y(p.x, p.z);
        if (gy < -1) continue;
        float depth = gy - p.y;
        if (depth <= 0) continue;
        Vector3 v = Vector3Add(c->vel, Vector3CrossProduct(c->angvel, rl));
        float n = fminf(45000 * depth - 2500 * v.y, 120000);
        if (n < 0) n = 0;
        float vt = len2(v);
        Vector3 ft = vt > 1e-3f ? Vector3Scale(V(v.x, 0, v.z), -fminf(0.6f * n, mass * 0.25f * vt / dt) / vt) : V(0, 0, 0);
        apply(&force, &torque, V(ft.x, n, ft.z), rl);
        if (-v.y > 4 && c->health > 0 && x->crash_cd <= 0) {
            x->crash_cd = 0.3f;
            crash_fx(p, V(0, 1, 0), -v.y);
            car_damage(ci, (-v.y - 4) * (-v.y - 4) * 0.9f, p, -1);
        }
    }

    // water: engine out, sinks slowly
    Vector3 o = origin_of(c);
    if (is_water(c->pos.x, c->pos.z) && o.y < 0.1f) {
        if (!x->water) {
            x->water = true;
            fx_burst(FX_WATER, c->pos, V(0, 1, 0), 30);
            sfx(S_CRASH_SMALL, c->pos, 0.7f, 0.5f);
            c->fire_s = 0;
        }
        force.y += mass * G * 0.9f;
        force = Vector3Add(force, Vector3Scale(c->vel, -mass * 2.2f));
        torque = Vector3Add(torque, Vector3Scale(c->angvel, -mass * 4));
        if (c->pos.y < -4.2f) { force.y = 0; c->vel.y = fmaxf(c->vel.y, 0); c->pos.y = -4.2f; }
    }

    // integrate
#ifdef CAR_TEST
    if (getenv("CAR_DBG") && ci == 0 && ticks > 840 && ticks < 880) printf("g%d sh%.2f thr%.2f v %.2f F %.0f %.0f %.0f fdrive %.0f N %.0f %.0f %.0f %.0f\n", x->gear, x->shift_s, thr, vlong_body, force.x, force.y, force.z, fdrive, N[0], N[1], N[2], N[3]);
#endif
    c->vel = Vector3Add(c->vel, Vector3Scale(force, dt / mass));
    float ix, iy, iz;
    inertia(c, &ix, &iy, &iz);
    Vector3 tb = qrot(QuaternionInvert(q), torque);
    Vector3 wb = qrot(QuaternionInvert(q), c->angvel);
    Vector3 gy_ = V((iz - iy) * wb.y * wb.z / ix, (ix - iz) * wb.z * wb.x / iy, (iy - ix) * wb.x * wb.y / iz);
    Vector3 al = V((tb.x) / ix - gy_.x, (tb.y) / iy - gy_.y, (tb.z) / iz - gy_.z);
    wb = Vector3Add(wb, Vector3Scale(al, dt));
    c->angvel = qrot(q, wb);
    c->angvel = Vector3Scale(c->angvel, 1 - 0.35f * dt);
    float wl = Vector3Length(c->angvel);
    if (wl > 12) c->angvel = Vector3Scale(c->angvel, 12 / wl);
    c->pos = Vector3Add(c->pos, Vector3Scale(c->vel, dt));
    Quaternion dq = QuaternionMultiply((Quaternion){ c->angvel.x, c->angvel.y, c->angvel.z, 0 }, q);
    c->rot = QuaternionNormalize((Quaternion){ q.x + 0.5f * dt * dq.x, q.y + 0.5f * dt * dq.y, q.z + 0.5f * dt * dq.z, q.w + 0.5f * dt * dq.w });

    drive_gear(c, x, reverse ? vabs * 4.6f : fmaxf(vlong_body, 0), reverse ? brk : thr, spinning, dt);
    if (x->water) c->rpm *= 0.9f;
}

// wheel visuals and skid marks, once per tick
static void wheel_fx(int ci, float dt)
{
    Car *c = &cars[ci];
    Ex *x = &ex[ci];
    const CarModel *m = &car_models[c->model];
    for (int w = 0; w < 4; w++) {
        c->wheel_spin[w] = fmodf(c->wheel_spin[w] + x->spin_w[w] * dt, 6.2831853f);
        bool on = c->wheel_ground[w] && x->slip[w] > 1.05f && !x->water && c->health > 0 + 0;
        Vector3 rw = qrot(c->rot, V(m->wheel_pos[w].x, m->wheel_pos[w].y + S_REST - com_y(c), m->wheel_pos[w].z));
        Vector3 p = Vector3Add(c->pos, rw);
        p.y = ground_y(p.x, p.z);
        bool road = p.y < 0.05f;
        float fast = len2(c->vel);
        on = on && (fast > 2.5f || x->spin_w[w] == 0 || x->slip[w] > 1.3f);
        fx_skid(ci * 4 + w, p, m->wheel_r * 0.7f, on && road);
        if (on && c->skid > 0.55f && ((ticks + (uint64_t)ci * 3 + (uint64_t)w) % 5 == 0))
            fx_burst(road ? FX_SMOKE : FX_DUST, V(p.x, p.y + 0.1f, p.z), V(0, 1, 0), 1);
    }
}

// ---------------------------------------------------------------- collisions

static void wake(int ci) { ex[ci].asleep = false; ex[ci].still_s = 0; }

static void hit_driver(int ci, float speed)
{
    Car *c = &cars[ci];
    if (c->driver >= 0 && speed > 14 && peds[c->driver].used && peds[c->driver].car == ci)
        ped_damage(c->driver, (speed - 14) * 3.2f, V(0, 0, 0), -1, -1);
}

static void impact(int ci, Vector3 at, Vector3 n, float vn)
{
    Ex *x = &ex[ci];
    if (vn < 2.5f || x->crash_cd > 0) return;
    x->crash_cd = 0.25f;
    crash_fx(at, n, vn);
    if (vn > 3) car_damage(ci, (vn - 3) * (vn - 3) * 0.8f, at, -1);
    hit_driver(ci, vn);
}

static void world_collide(int ci)
{
    Car *c = &cars[ci];
    Foot f = foot_of(c);
    BoundingBox bb[48];
    int nb = world_boxes(c->pos, f.hl + f.hw + 1.5f, bb, 48);
    float bottom = origin_of(c).y + 0.28f, top = origin_of(c).y + car_models[c->model].size.y;
    float mass = car_models[c->model].mass_kg, ix, iy, iz;
    inertia(c, &ix, &iy, &iz);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nb; i++) {
            const BoundingBox *b = &bb[i];
            if (b->max.y < bottom || b->min.y > top) continue;
            Foot bf = { V((b->min.x + b->max.x) * 0.5f, 0, (b->min.z + b->max.z) * 0.5f), V(1, 0, 0), V(0, 0, 1),
                        (b->max.x - b->min.x) * 0.5f, (b->max.z - b->min.z) * 0.5f };
            f = foot_of(c);
            Vector3 n; float d;
            if (!sat(&f, &bf, &n, &d)) continue;
            Vector3 cp[4]; corners(&f, cp);
            Vector3 pt = cp[0];
            float bestd = 1e9f;
            for (int k = 0; k < 4; k++) {
                float dd = dot2(Vector3Subtract(cp[k], bf.c), n);
                if (dd < bestd) { bestd = dd; pt = cp[k]; }
            }
            // contact point for the car's own faces: nearest point of the box
            pt.x = clampf(pt.x, b->min.x, b->max.x); pt.z = clampf(pt.z, b->min.z, b->max.z);
            if (bestd > 0) { pt = V(clampf(c->pos.x, b->min.x, b->max.x), 0, clampf(c->pos.z, b->min.z, b->max.z)); }
            Vector3 r = V(pt.x - c->pos.x, 0, pt.z - c->pos.z);
            Vector3 vp = Vector3Add(c->vel, Vector3CrossProduct(c->angvel, r));
            float vn = dot2(vp, n);
            c->pos.x += n.x * (d + 0.01f); c->pos.z += n.z * (d + 0.01f);
            if (vn >= 0) continue;
            float rxn = r.z * n.x - r.x * n.z;
            float inv = 1 / mass + rxn * rxn / iy;
            float e = 0.18f;
            float j = -(1 + e) * vn / inv;
            c->vel.x += n.x * j / mass; c->vel.z += n.z * j / mass;
            c->angvel.y += rxn * j / iy;
            Vector3 t = V(-n.z, 0, n.x);
            float vt = dot2(vp, t);
            float rxt = r.z * t.x - r.x * t.z;
            float invt = 1 / mass + rxt * rxt / iy;
            float jt = clampf(-vt / invt, -0.45f * j, 0.45f * j);
            c->vel.x += t.x * jt / mass; c->vel.z += t.z * jt / mass;
            c->angvel.y += rxt * jt / iy;
            Vector3 at = V(pt.x, c->pos.y, pt.z);
            if (pass == 0) impact(ci, at, n, -vn);
        }
}

static void car_vs_car(int a, int b)
{
    Car *A = &cars[a], *B = &cars[b];
    if (fabsf(A->pos.y - B->pos.y) > 1.1f) return;
    Foot fa = foot_of(A), fb = foot_of(B);
    Vector3 n; float d;
    if (!sat(&fa, &fb, &n, &d)) return;
    float ma = car_models[A->model].mass_kg, mb = car_models[B->model].mass_kg;
    bool sa = ex[a].asleep, sb = ex[b].asleep;
    if (sa) wake(a);
    if (sb) wake(b);
    // contact: corners of each inside the other
    Vector3 ca[4], cb[4], pt = V(0, 0, 0);
    corners(&fa, ca); corners(&fb, cb);
    int cnt = 0;
    for (int i = 0; i < 4; i++) {
        if (inside(&fb, ca[i])) { pt = Vector3Add(pt, ca[i]); cnt++; }
        if (inside(&fa, cb[i])) { pt = Vector3Add(pt, cb[i]); cnt++; }
    }
    if (cnt) pt = Vector3Scale(pt, 1.0f / cnt);
    else pt = V((A->pos.x + B->pos.x) * 0.5f, 0, (A->pos.z + B->pos.z) * 0.5f);
    float ia[3], ib[3];
    inertia(A, &ia[0], &ia[1], &ia[2]); inertia(B, &ib[0], &ib[1], &ib[2]);
    Vector3 ra = V(pt.x - A->pos.x, 0.2f, pt.z - A->pos.z), rb = V(pt.x - B->pos.x, 0.2f, pt.z - B->pos.z);
    Vector3 va = Vector3Add(A->vel, Vector3CrossProduct(A->angvel, ra));
    Vector3 vb = Vector3Add(B->vel, Vector3CrossProduct(B->angvel, rb));
    Vector3 vr = Vector3Subtract(va, vb);
    float vn = dot2(vr, n);
    float tot = ma + mb;
    A->pos.x += n.x * (d + 0.02f) * mb / tot; A->pos.z += n.z * (d + 0.02f) * mb / tot;
    B->pos.x -= n.x * (d + 0.02f) * ma / tot; B->pos.z -= n.z * (d + 0.02f) * ma / tot;
    if (vn >= 0) return;
    Vector3 N3 = V(n.x, 0, n.z);
    Vector3 ta = Vector3CrossProduct(ra, N3), tb = Vector3CrossProduct(rb, N3);
    float inv = 1 / ma + 1 / mb + ta.y * ta.y / ia[1] + tb.y * tb.y / ib[1] + (ta.x * ta.x) / ia[0] * 0.25f + (ta.z * ta.z) / ia[2] * 0.25f
                + (tb.x * tb.x) / ib[0] * 0.25f + (tb.z * tb.z) / ib[2] * 0.25f;
    float j = -(1.22f) * vn / inv;
#ifdef CAR_TEST
    if (getenv("CAR_DBG2")) printf("cvc %d %d vn %.2f d %.2f at %.1f %.1f\n", a, b, vn, d, pt.x, pt.z);
#endif
    Vector3 J = Vector3Scale(N3, j);
    A->vel = Vector3Add(A->vel, Vector3Scale(J, 1 / ma));
    B->vel = Vector3Subtract(B->vel, Vector3Scale(J, 1 / mb));
    Vector3 dwa = Vector3CrossProduct(ra, J), dwb = Vector3CrossProduct(rb, J);
    Vector3 qa = qrot(QuaternionInvert(A->rot), dwa), qb = qrot(QuaternionInvert(B->rot), dwb);
    qa = V(qa.x / ia[0] * 0.5f, qa.y / ia[1], qa.z / ia[2] * 0.5f);
    qb = V(qb.x / ib[0] * 0.5f, qb.y / ib[1], qb.z / ib[2] * 0.5f);
    A->angvel = Vector3Add(A->angvel, qrot(A->rot, qa));
    B->angvel = Vector3Subtract(B->angvel, qrot(B->rot, qb));
    Vector3 t = V(-n.z, 0, n.x);
    float vt = dot2(vr, t);
    float jt = clampf(-vt / (1 / ma + 1 / mb), -0.35f * j, 0.35f * j);
    A->vel = Vector3Add(A->vel, Vector3Scale(t, jt / ma));
    B->vel = Vector3Subtract(B->vel, Vector3Scale(t, jt / mb));
    float speed = -vn;
    Vector3 at = V(pt.x, A->pos.y, pt.z);
    if (ex[a].crash_cd <= 0 && ex[b].crash_cd <= 0 && speed > 2.5f) {
        ex[a].crash_cd = ex[b].crash_cd = 0.25f;
        crash_fx(at, n, speed);
        float base = speed > 3 ? (speed - 3) * (speed - 3) * 0.8f : 0;
        int atk_a = B->driver >= 0 ? B->driver : -1, atk_b = A->driver >= 0 ? A->driver : -1;
        car_damage(a, base * 2 * mb / tot, at, atk_a);
        car_damage(b, base * 2 * ma / tot, at, atk_b);
        hit_driver(a, speed); hit_driver(b, speed);
    }
}

static void car_vs_peds(int ci)
{
    Car *c = &cars[ci];
    float sp = len2(c->vel);
    if (sp < 2 || c->ai == AI_WRECK) return;
    Foot f = foot_of(c);
    f.hl += 0.3f; f.hw += 0.3f;
    float bottom = origin_of(c).y, top = bottom + car_models[c->model].size.y;
    for (int i = 0; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used || i == c->driver || p->car == ci || ped_hit_cd[i] > 0) continue;
        if (p->state == PS_DEAD || p->state == PS_IN_CAR || p->state == PS_ENTERING || p->state == PS_EXITING) continue;
        if (fabsf(p->pos.x - c->pos.x) > 6 || fabsf(p->pos.z - c->pos.z) > 6) continue;
        if (p->pos.y + 1.7f < bottom || p->pos.y > top - 0.1f) continue;
        if (!inside(&f, p->pos)) continue;
        // only the front half and sides take the hit; slow rolling over a person isn't modelled
        float dmg = clampf(sp * 9, 10, 400);
        Vector3 dir = Vector3Normalize(V(c->vel.x, 0, c->vel.z));
        Vector3 imp = Vector3Add(Vector3Scale(dir, sp * 0.9f), V(0, 2.5f + sp * 0.18f, 0));
        int attacker = c->driver >= 0 ? c->driver : -1;
        ped_hit_cd[i] = 0.6f;
        ped_damage(i, dmg, imp, attacker, -1);
        c->vel = Vector3Scale(c->vel, 0.97f);
        sfx(S_BODYFALL, p->pos, 0.8f, 1);
        fx_burst(FX_BLOOD, V(p->pos.x, p->pos.y + 1, p->pos.z), dir, 6);
        if (c->driver == PLAYER) crime(CR_HIT_PED, p->pos, i);
    }
}

bool cars_push(Vector3 *p, float radius)
{
    bool hit = false;
    for (int i = 0; i < MAX_CARS; i++) {
        Car *c = &cars[i];
        if (!c->used) continue;
        if (fabsf(c->pos.x - p->x) > 4 || fabsf(c->pos.z - p->z) > 4) continue;
        float bottom = origin_of(c).y, top = bottom + car_models[c->model].size.y;
        if (p->y + 1.7f < bottom + 0.2f || p->y > top) continue;
        Foot f = foot_of(c);
        Vector3 d = Vector3Subtract(*p, f.c);
        float lx = dot2(d, f.l), lz = dot2(d, f.f);
        float qx = clampf(lx, -f.hw, f.hw), qz = clampf(lz, -f.hl, f.hl);
        float dx = lx - qx, dz = lz - qz, d2 = dx * dx + dz * dz;
        if (d2 >= radius * radius) continue;
        if (d2 > 1e-8f) {
            float dd = sqrtf(d2), s = (radius - dd) / dd;
            lx += dx * s; lz += dz * s;
        } else {
            float gx = f.hw - fabsf(lx), gz = f.hl - fabsf(lz);
            if (gx < gz) lx = (lx < 0 ? -1 : 1) * (f.hw + radius);
            else lz = (lz < 0 ? -1 : 1) * (f.hl + radius);
        }
        p->x = f.c.x + f.l.x * lx + f.f.x * lz;
        p->z = f.c.z + f.l.z * lx + f.f.z * lz;
        hit = true;
    }
    return hit;
}

// ---------------------------------------------------------------- lane following ai

typedef struct { int id; Vector3 a, d; float len, start; } Seg;

static int route_segs(int ci, Seg *s, float t)
{
    Ex *x = &ex[ci];
    int n = 0;
    float st = -t;
    for (int i = 0; i < 4; i++) {
        int l = x->route[i];
        if (l < 0) break;
        Lane *L = &lanes[l];
        s[n++] = (Seg){ l, L->a, norm2(Vector3Subtract(L->b, L->a)), L->len, st };
        st += L->len;
    }
    return n;
}

static Vector3 seg_point(const Seg *s, int n, float dist)
{
    if (n == 0) return V(0, 0, 0);
    for (int i = 0; i < n; i++)
        if (dist <= s[i].start + s[i].len || i == n - 1) {
            float u = dist - s[i].start;
            return Vector3Add(s[i].a, Vector3Scale(s[i].d, u));
        }
    return s[n - 1].a;
}

// follows lane chains to a street long enough to compare against the goal
static Vector3 lane_far_end(int l)
{
    for (int i = 0; i < 6; i++) {
        if (lanes[l].len > 15 || lanes[l].nnext == 0) break;
        l = lanes[l].next[0];
    }
    return lanes[l].b;
}

// driving distance to the goal through the road beyond lane l (as the crow flies once the lanes run out)
static float lane_cost(int l, Vector3 goal)
{
    float acc = 0, best = 1e9f;
    Vector3 end = lanes[l].a;
    for (int i = 0; i < 6; i++) {
        const Lane *L = &lanes[l];
        Vector3 d = norm2(Vector3Subtract(L->b, L->a));
        float t = dot2(Vector3Subtract(goal, L->a), d);
        if (t > -1) {
            t = clampf(t, 0, L->len);
            Vector3 q = Vector3Add(L->a, Vector3Scale(d, t));
            best = fminf(best, acc + t + len2(Vector3Subtract(goal, q)));
        }
        acc += L->len;
        end = L->b;
        if (L->len > 30 || L->nnext == 0) break;
        l = L->next[0];
    }
    return fminf(best, acc + len2(Vector3Subtract(goal, end)));
}

static int pick_next(int l, int mode, Vector3 goal)
{
    const Lane *L = &lanes[l];
    if (L->nnext <= 0) return -1;
    if (mode == 0) {
        if (L->nnext == 1) return L->next[0];
        if (rnd() % 100 < 62) return L->next[0];
        return L->next[1 + rnd() % (L->nnext - 1)];
    }
    int best = L->next[0];
    float bs = mode == 1 ? 1e9f : -1e9f;
    for (int i = 0; i < L->nnext; i++) {
        float d = mode == 1 ? lane_cost(L->next[i], goal) : len2(Vector3Subtract(lane_far_end(L->next[i]), goal));
        if (mode == 1 ? d < bs : d > bs) { bs = d; best = L->next[i]; }
    }
    return best;
}

static void route_fill(int ci, int mode, Vector3 goal)
{
    Ex *x = &ex[ci];
    for (int i = 1; i < 4; i++)
        if (x->route[i] < 0 && x->route[i - 1] >= 0) x->route[i] = pick_next(x->route[i - 1], mode, goal);
}

static int lane_find(Vector3 p, Vector3 f)
{
    static const float offs[5] = { 0, -2.2f, 2.2f, -4.6f, 4.6f };
    Vector3 l = left_h(f);
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < 5; i++) {
        Vector3 q = V(p.x + l.x * offs[i], 0, p.z + l.z * offs[i]);
        int id = nearest_lane(q, 4);
        if (id < 0) continue;
        Lane *L = &lanes[id];
        Vector3 d = norm2(Vector3Subtract(L->b, L->a));
        if (dot2(d, f) < 0.6f) continue;
        Vector3 ab = Vector3Subtract(L->b, L->a);
        float t = clampf(dot2(Vector3Subtract(p, L->a), d), 0, L->len);
        Vector3 cp = Vector3Add(L->a, Vector3Scale(d, t));
        float dist = len2(Vector3Subtract(cp, p));
        (void)ab;
        if (dist < bd && dist < 7) { bd = dist; best = id; }
    }
    return best;
}

static bool attach_lane(int ci, int mode, Vector3 goal)
{
    Car *c = &cars[ci];
    Ex *x = &ex[ci];
    int l = lane_find(c->pos, norm2(car_fwd(c)));
    if (l < 0) return false;
    for (int i = 0; i < 4; i++) x->route[i] = -1;
    x->route[0] = l;
    route_fill(ci, mode, goal);
    c->lane = l;
    return true;
}

typedef struct { float gap, vl, s0; } Obst;

static float idm(float v, float v0, float amax, float gap, float vl, float s0, float tgap)
{
    float b = 3.0f;
    float sx = s0 + fmaxf(0, v * tgap + v * (v - vl) / (2 * sqrtf(amax * b)));
    float free = 1 - powf(v / fmaxf(v0, 0.5f), 4);
    float inter = gap > 0.05f ? (sx / gap) * (sx / gap) : 100;
    return amax * (free - inter);
}

static float seg_gap_to(const Seg *s, int n, Vector3 p, float lat_thr_base, float other_half, float *lat_out)
{
    (void)other_half;
    float best = 1e9f;
    for (int i = 0; i < n; i++) {
        Vector3 d = Vector3Subtract(p, s[i].a);
        float along = dot2(d, s[i].d);
        float lat = dot2(d, left_h(s[i].d));
        if (along < (i == 0 ? -3 : 0) || along > s[i].len + (i == n - 1 ? 4 : 0)) continue;
        if (fabsf(lat) > lat_thr_base) continue;
        float dist = s[i].start + along;
        if (dist < best) { best = dist; *lat_out = lat; }
    }
    return best;
}

static bool ai_lane_drive(int ci, float dt, int mode, Vector3 goal, float v_goal, bool reckless)
{
    Car *c = &cars[ci];
    Ex *x = &ex[ci];
    const Tune *tn = &tune[c->model];
    Vector3 fwd = norm2(car_fwd(c));
    float v = fmaxf(car_speed(c), 0);
    float hl = half_l(c), hw = half_w(c);

    if (x->route[0] < 0 || !lanes) {
        if (!attach_lane(ci, mode, goal)) { c->throttle = 0; c->brake = 0.5f; c->handbrake = v < 0.5f; c->stuck_s += dt; return false; }
    }
    // position on the current lane, hop to the next when we pass its end
    for (int guard = 0; guard < 3; guard++) {
        Lane *L = &lanes[x->route[0]];
        Vector3 d = norm2(Vector3Subtract(L->b, L->a));
        float t = dot2(Vector3Subtract(c->pos, L->a), d);
        if (t >= L->len - 0.3f && x->route[1] >= 0) {
            for (int i = 0; i < 3; i++) x->route[i] = x->route[i + 1];
            x->route[3] = -1;
            route_fill(ci, mode, goal);
        } else break;
    }
    Lane *L = &lanes[x->route[0]];
    Vector3 ld = norm2(Vector3Subtract(L->b, L->a));
    float t = dot2(Vector3Subtract(c->pos, L->a), ld);
    float lat_err = dot2(Vector3Subtract(c->pos, L->a), left_h(ld));
    c->lane = x->route[0];
    c->lane_t = clampf(t, 0, L->len);
    if (mode != 0 && x->route[1] < 0 && L->len - t < 10) return false; // dead end, caller goes straight at the goal
    if (fabsf(lat_err) > 6 || t < -8 || (x->route[1] < 0 && t > L->len + 6)) {
        for (int i = 0; i < 4; i++) x->route[i] = -1;
        if (!attach_lane(ci, mode, goal)) { c->throttle = 0; c->brake = 0.4f; c->stuck_s += dt; return false; }
        return true;
    }
    if (mode != 0) {
        x->repick_s -= dt;
        if (x->repick_s <= 0 && L->len - t > 14 && x->route[1] >= 0) {
            x->repick_s = 0.4f;
            x->route[1] = pick_next(x->route[0], mode, goal);
            x->route[2] = x->route[3] = -1;
            route_fill(ci, mode, goal);
        }
    }

    Seg segs[4];
    int ns = route_segs(ci, segs, t);

    // speed we'd like
    float lim_mul = reckless ? 1.6f : x->speed_mul;
    float v0 = (v_goal > 0 ? v_goal : L->speed * lim_mul);
    float dec = reckless ? 6.5f : 3.0f, need_dec = 0;
    for (int i = 0; i < ns; i++) {
        float vs = lanes[segs[i].id].speed * (reckless ? 1.5f : lim_mul);
        if (reckless && v_goal > 0) {
            // sirens ignore the limit, but a corner still wants slowing for
            bool turn = i > 0 && (dot2(segs[i].d, segs[i - 1].d) < 0.985f || (i > 1 && dot2(segs[i].d, segs[i - 2].d) < 0.97f)
                                  || (i + 1 < ns && dot2(segs[i].d, segs[i + 1].d) < 0.985f));
            vs = turn ? fminf(v_goal, segs[i].len < 12 ? 7.5f : 14) : v_goal;
        }
        if (v_goal > 0 && vs > v_goal) vs = v_goal;
        float dist = fmaxf(0, segs[i].start - hl);
        float cap = sqrtf(vs * vs + 2 * dec * dist);
        if (cap < v0) v0 = cap;
        if (reckless && v > vs) need_dec = fmaxf(need_dec, (v * v - vs * vs) / (2 * fmaxf(dist, 1.5f)));
    }
    v0 = fminf(v0, tn->vmax * 0.9f);

    // things in the way
    Obst ob[8];
    int no = 0;
    bool light_stop = false, ped_stop = false;
    bool blocked_by_car = false;
    for (int i = 0; i < ns && !reckless; i++) {
        Lane *l = &lanes[segs[i].id];
        if (l->light >= 0) {
            float stop = segs[i].start + segs[i].len - hl - 0.4f;
            if (stop < -1.5f || stop > 90) continue;
            int st = light_state(l->light);
            bool halt = st == 2 || (st == 1 && v * v / (2 * 4.0f) < stop);
            if (st == 0 && x->light_hold && i == 0) { x->go_wait = frand(0.35f, 0.95f); x->light_hold = false; }
            if (st == 0 && x->go_wait > 0) halt = true;
            if (halt) {
                if (st == 2) x->light_hold = true;
                ob[no++] = (Obst){ fmaxf(stop, 0.05f), 0, 0.5f };
                light_stop = true;
                break;
            }
        }
    }
    if (x->go_wait > 0) x->go_wait -= dt;
    if (x->route[1] < 0 && ns > 0) {
        float stop = segs[ns - 1].start + segs[ns - 1].len - hl - 1.0f;
        ob[no++] = (Obst){ fmaxf(stop, 0.05f), 0, 1.0f };
        light_stop = true;
    }

    // leaders: cars whose bodies sit on our route
    float lead_gap = 1e9f, lead_v = 0, lead_lat = 0;
    int lead = -1;
    for (int j = 0; j < MAX_CARS; j++) {
        Car *o = &cars[j];
        if (j == ci || !o->used) continue;
        if (fabsf(o->pos.x - c->pos.x) > 70 || fabsf(o->pos.z - c->pos.z) > 70) continue;
        Vector3 of = norm2(car_fwd(o));
        float co = fabsf(dot2(of, fwd)), si = sqrtf(fmaxf(0, 1 - co * co));
        float ohl = half_l(o), ohw = half_w(o);
        float proj_half = ohw * co + ohl * si;
        float thr = hw + proj_half - 0.05f;
        float lat = 0;
        float dist = seg_gap_to(segs, ns, o->pos, thr, proj_half, &lat);
        if (dist > 1e8f) continue;
        float ovel = dot2(o->vel, segs[0].d);
        bool same_dir = dot2(of, fwd) > 0.5f;
        bool stationary = len2(o->vel) < 1.0f;
        bool prio = !same_dir && !stationary && o->ai != AI_NONE && o->ai != AI_PARKED && ci < j && x->yield_free_s <= 0;
        if (!same_dir && !stationary && o->ai != AI_NONE && o->ai != AI_PARKED && !prio && x->yield_free_s > 0) continue;
        if (!same_dir && !stationary && o->ai != AI_NONE && o->ai != AI_PARKED && ci > j) { /* yield */ }
        else if (!same_dir && !stationary && o->ai != AI_NONE && o->ai != AI_PARKED) continue;
        if (dist < -hl) continue;
        float gap = dist - hl - (ohl * co + ohw * si);
        if (gap < lead_gap) { lead_gap = gap; lead_v = fmaxf(ovel, 0); lead = j; lead_lat = lat; }
    }
    if (lead >= 0 && !reckless) {
        ob[no++] = (Obst){ fmaxf(lead_gap, 0.05f), lead_v, 2.0f };
        blocked_by_car = true;
    } else if (lead >= 0 && lead_gap < 4.5f) {
        ob[no++] = (Obst){ fmaxf(lead_gap, 0.05f), lead_v, 1.0f };   // only brake when swerving won't save us
    }
    // people on the road
    if (!reckless) {
        static const float probe[3] = { 5, 9, 15 };
        for (int i = 0; i < 3; i++) {
            float dd = probe[i] * (1 + v / 14);
            Vector3 pt = seg_point(segs, ns, dd);
            int pi = ped_at(pt, 1.5f, c->driver);
            if (pi >= 0 && peds[pi].state != PS_IN_CAR && peds[pi].state != PS_DEAD) {
                ob[no++] = (Obst){ fmaxf(dd - hl - 1, 0.05f), 0, 1.5f };
                ped_stop = true;
                if (dd < 12 && x->honk_cd <= 0) { x->honk_s = 0.35f; x->honk_cd = 4; }
                break;
            }
        }
    }

    float a = idm(v, v0, reckless ? 3.5f : 2.0f, 1e6f, 0, 0, 1);
    for (int i = 0; i < no; i++) {
        float ai = idm(v, v0, reckless ? 3.5f : 2.0f, ob[i].gap, ob[i].vl, ob[i].s0, reckless ? 0.6f : 1.15f);
        if (ai < a) a = ai;
    }
    if (need_dec > 0.4f * dec) a = fminf(a, -need_dec * 1.1f);   // sirens brake for a corner as late as the tyres allow
    a = clampf(a, -9, 3.5f);

    // pedals
    float drag = (0.32f * v * v + 90) / car_models[c->model].mass_kg;
    float avail = fmaxf(tn->accel * (1 - v * v / (tn->vmax * tn->vmax)) * 0.9f, 1.2f);
    c->handbrake = false;
    c->throttle = c->brake = 0;
    if (a > -0.25f) c->throttle = clampf((a + drag) / avail, 0, 1);
    else if (a > -0.9f && v > 1) { c->throttle = 0; c->brake = 0; }
    else c->brake = clampf(-a / 9.0f, 0.05f, 1);
    if (v < 0.4f && (a < 0.2f)) { c->handbrake = true; c->throttle = 0; c->brake = 0.6f; x->no_rev = true; } else x->no_rev = false;
    if (v < 0.4f && c->handbrake == false) x->no_rev = true;

    // pure pursuit on the lane centreline
    float ldist = reckless ? clampf(3.0f + 0.3f * v, 4.0f, 12) : clampf(3.5f + 0.55f * v, 4.5f, 18);
    Vector3 tp = seg_point(segs, ns, ldist);
    if (reckless && lead >= 0 && lead_gap < 30 && lead_v < fmaxf(v - 1.5f, 5)) {
        // sirens: go round slow traffic, a lane's width to the side the car isn't on
        float side = lead_lat > 0.3f ? -1 : lead_lat < -0.3f ? 1 : (ci & 1 ? 1 : -1);
        tp = Vector3Add(tp, Vector3Scale(left_h(segs[0].d), side * 3.2f));
    }
    Vector3 dv = V(tp.x - c->pos.x, 0, tp.z - c->pos.z);
    float dl = len2(dv);
    float alpha = atan2f(dot2(dv, left_h(fwd)), dot2(dv, fwd));
    float delta_l = atanf(2 * (half_l(c) * 1.2f) * sinf(alpha) / fmaxf(dl, 2));
    c->steer = clampf(-delta_l / max_steer(v, tn->grip), -1, 1);

    // honking and getting unstuck
    bool waiting = light_stop || ped_stop;
    if (blocked_by_car && v < 0.6f && !waiting) {
        x->blocked_s += dt;
        if (x->blocked_s > 2 && lead >= 0 && (cars[lead].driver == PLAYER || cars[lead].ai == AI_NONE || cars[lead].ai == AI_PARKED) && x->honk_cd <= 0) {
            x->honk_s = 0.5f; x->honk_cd = 3;
        }
        if (x->blocked_s > 7) x->yield_free_s = 4;
    } else x->blocked_s = fmaxf(0, x->blocked_s - dt * 2);
    if (x->yield_free_s > 0) x->yield_free_s -= dt;
    if (v < 0.5f && (c->throttle > 0.3f || reckless) && !waiting && !blocked_by_car) c->stuck_s += dt;
    else if (v > 2) c->stuck_s = 0;
    else if (waiting || blocked_by_car) c->stuck_s += dt * 0.35f;
    if (c->stuck_s > 2.5f && x->rev_s <= 0 && !waiting && !blocked_by_car) { x->rev_s = 1.2f; c->stuck_s = 2.6f; }
    return true;
}

static void ai_apply_reverse(Car *c, Ex *x, float dt)
{
    if (x->rev_s <= 0) return;
    x->rev_s -= dt;
    c->throttle = 0; c->handbrake = false;
    c->brake = 0.8f;
    c->steer = -c->steer;
    x->no_rev = false;
    if (x->rev_s <= 0) c->stuck_s = 0;
}

// ---------------------------------------------------------------- generic drive-to

void car_drive_to(int ci, Vector3 target, float speed_ms, bool reckless)
{
    if (ci < 0 || ci >= MAX_CARS || !cars[ci].used) return;
    Car *c = &cars[ci];
    Ex *x = &ex[ci];
    const Tune *tn = &tune[c->model];
    x->drive_tick = ticks;
    Vector3 fwd = norm2(car_fwd(c));
    float v = car_speed(c);
    float hl = half_l(c);

    // target velocity from successive calls, to aim ahead of a moving target
    if (x->tgt_tick + 2 >= ticks && x->tgt_tick != 0) {
        Vector3 nv = Vector3Scale(Vector3Subtract(target, x->tgt_last), TICK_HZ);
        float l = Vector3Length(nv);
        if (l > 40) nv = Vector3Scale(nv, 40 / l);
        x->tgt_vel = Vector3Lerp(x->tgt_vel, nv, 0.15f);
    } else x->tgt_vel = V(0, 0, 0);
    x->tgt_last = target;
    x->tgt_tick = ticks + 1;

    Vector3 to = V(target.x - c->pos.x, 0, target.z - c->pos.z);
    float dist = len2(to);
    Vector3 aim = target;
    if (dist > 8) {
        float tl = clampf(dist / fmaxf(fabsf(v), 10), 0, 1.4f);
        aim = Vector3Add(target, Vector3Scale(x->tgt_vel, tl));
    }
    to = V(aim.x - c->pos.x, 0, aim.z - c->pos.z);
    dist = len2(to);

    bool direct = false;
    if (dist < 50) {
        Hit h;
        Vector3 o = V(c->pos.x, 0.9f, c->pos.z);
        Vector3 d = Vector3Normalize(V(to.x, 0, to.z));
        direct = !world_ray(o, d, dist, &h);
    }
    if (dist < 14) direct = true;

    // wedged on a corner or a prop: whatever the lane logic thinks, back out
    if (fabsf(v) < 0.6f && speed_ms > 3 && dist > 8) x->nomove_s += DT;
    else if (fabsf(v) > 2) x->nomove_s = 0;
    if (x->nomove_s > 1.5f && x->rev_s <= 0) { x->rev_s = 1.3f; x->nomove_s = 0; }

    if (x->rev_s > 0) {
        // back out of whatever we're stuck on, turning away
        x->rev_s -= DT;
        c->throttle = 0; c->handbrake = false; c->brake = 0.9f; x->no_rev = false;
        float side = dot2(to, left_h(fwd)) > 0 ? 1 : -1;
        c->steer = side * 0.8f;
        if (x->rev_s <= 0) c->stuck_s = 0;
        return;
    }

    if (!direct) {
        if (ai_lane_drive(ci, DT, 1, aim, speed_ms, reckless)) {
            if (c->stuck_s > 2.5f && x->rev_s <= 0) { x->rev_s = 1.3f; c->stuck_s = 0; }
            return;
        }
        c->stuck_s = 0;
        if (dist > 20) {
            // off every lane and facing the wrong way: head for the nearest road first
            int l = nearest_lane(c->pos, 14);
            if (l >= 0) {
                Lane *L = &lanes[l];
                Vector3 ld = norm2(Vector3Subtract(L->b, L->a));
                float t = clampf(dot2(Vector3Subtract(c->pos, L->a), ld) + 10, 0, L->len);
                Vector3 q = Vector3Add(L->a, Vector3Scale(ld, t));
                to = V(q.x - c->pos.x, 0, q.z - c->pos.z);
                dist = len2(to);
            }
        }
    }
    // straight at it, feeling for walls ahead
    float alpha = atan2f(dot2(to, left_h(fwd)), dot2(to, fwd));
    float want = speed_ms;
    if (fabsf(alpha) > 0.5f) want *= clampf(1.4f - fabsf(alpha) * 0.55f, 0.35f, 1);
    if (!reckless && dist < 25) want = fminf(want, dist * 0.7f);
    float steer_l = alpha;
    Hit h;
    Vector3 o = V(c->pos.x, 0.9f, c->pos.z);
    float look = fmaxf(8, v * 0.9f) + hl;
    bool blocked = false;
    for (int s = -1; s <= 1; s++) {
        float ang = s * 0.35f;
        Vector3 d = V(fwd.x * cosf(ang) + left_h(fwd).x * sinf(ang), 0, fwd.z * cosf(ang) + left_h(fwd).z * sinf(ang));
        if (world_ray(o, d, look, &h)) {
            if (s == 0) blocked = true;
            steer_l -= s * (1 - h.t / look) * 0.9f;
            want = fminf(want, fmaxf(5, h.t * 0.9f));
        }
    }
    float dl = clampf(dist, 5, 14);
    float delta = atanf(2 * half_l(c) * 1.2f * sinf(clampf(steer_l, -1.4f, 1.4f)) / dl);
    c->steer = clampf(-delta / max_steer(fmaxf(v, 0), tn->grip), -1, 1);
    float err = want - v;
    c->handbrake = false;
    x->no_rev = true;
    if (err > 0.5f) { c->throttle = clampf(err / 4, 0.25f, 1); c->brake = 0; }
    else if (err < -1.5f) { c->throttle = 0; c->brake = clampf(-err / 8, 0.1f, 1); }
    else { c->throttle = 0.15f; c->brake = 0; }
    if (v < 0.4f && want < 1) { c->throttle = 0; c->brake = 0.6f; c->handbrake = true; }
    if (blocked && v < 2) x->no_rev = false;
    if (v < 1.2f && want > 3 && c->throttle > 0.2f) c->stuck_s += DT; else if (v > 3) c->stuck_s = 0;
    if (c->stuck_s > 1.6f) { x->rev_s = 1.2f; c->stuck_s = 0; }
}

// ---------------------------------------------------------------- population

static void sync_driver(int ci)
{
    Car *c = &cars[ci];
    int d = c->driver;
    if (d < 0) return;
    Ped *p = &peds[d];
    if (!p->used || (d != PLAYER && (p->car != ci))) { c->driver = -1; return; }
    if (d == PLAYER) return;
    if (p->state != PS_IN_CAR) return;
    const CarModel *m = &car_models[c->model];
    p->pos = Vector3Add(origin_of(c), qrot(c->rot, m->seat));
    p->vel = c->vel;
    Vector3 f = car_fwd(c);
    p->yaw = atan2f(f.x, f.z);
}

static int spawn_driver(int ci)
{
    Car *c = &cars[ci];
    Vector3 f = car_fwd(c);
    int d = ped_spawn(PED_CIV, c->pos, atan2f(f.x, f.z));
    if (d < 0) return -1;
    peds[d].car = ci;
    peds[d].seat = 0;
    peds[d].state = PS_IN_CAR;
    c->driver = d;
    return d;
}

#ifdef CAR_TEST
static int tcar;
static Vector3 dbgv;
static void dbg(const char *tag)
{
    if (!getenv("CAR_DBG4") || tcar < 0) return;
    if (Vector3Distance(dbgv, cars[tcar].vel) > 0.3f) printf("%s: %.2f -> %.2f tick %llu\n", tag, Vector3Length(dbgv), Vector3Length(cars[tcar].vel), (unsigned long long)ticks);
    dbgv = cars[tcar].vel;
}
static bool test_no_peds;
#else
#define test_no_peds 0
#endif

static bool spot_free(Vector3 p, float yaw, int model, float clear)
{
    Car tmp = { .model = model, .pos = V(p.x, 0.6f, p.z), .rot = QuaternionFromAxisAngle(V(0, 1, 0), yaw) };
    Foot f = foot_of(&tmp);
    f.hl += clear; f.hw += clear;
    for (int i = 0; i < MAX_CARS; i++) {
        if (!cars[i].used) continue;
        if (len2(Vector3Subtract(cars[i].pos, p)) > 10) continue;
        Foot o = foot_of(&cars[i]);
        Vector3 n; float d;
        if (sat(&f, &o, &n, &d)) return false;
    }
    BoundingBox bb[16];
    int nb = world_boxes(p, 6, bb, 16);
    for (int i = 0; i < nb; i++) {
        if (bb[i].max.y < 0.4f) continue;
        Foot bf = { V((bb[i].min.x + bb[i].max.x) * 0.5f, 0, (bb[i].min.z + bb[i].max.z) * 0.5f), V(1, 0, 0), V(0, 0, 1),
                    (bb[i].max.x - bb[i].min.x) * 0.5f, (bb[i].max.z - bb[i].min.z) * 0.5f };
        Vector3 n; float d;
        if (sat(&f, &bf, &n, &d)) return false;
    }
    return true;
}

static int count_ai(int kind)
{
    int n = 0;
    for (int i = 0; i < MAX_CARS; i++) {
        if (!cars[i].used) continue;
        if (kind == AI_TRAFFIC ? (cars[i].ai == AI_TRAFFIC || cars[i].ai == AI_FLEE) : cars[i].ai == kind) n++;
    }
    return n;
}

static bool try_spawn_traffic(Vector3 around, float rmin)
{
    if (nlanes <= 0) return false;
    for (int tries = 0; tries < 24; tries++) {
        // a point on the ring around the player, then the street under it
        float a = frand(0, 2 * PI), r = frand(rmin, 150);
        int l = nearest_lane(V(around.x + sinf(a) * r, 0, around.z + cosf(a) * r), 25);
        if (l < 0) continue;
        Lane *L = &lanes[l];
        if (L->light < 0 || L->len < 25 || L->nnext == 0) continue;
        float t = frand(5, L->len - 12);
        Vector3 d = norm2(Vector3Subtract(L->b, L->a));
        Vector3 p = Vector3Add(L->a, Vector3Scale(d, t));
        float dist = len2(Vector3Subtract(p, around));
        if (dist < rmin || dist > 200) continue;
        if (in_view(p, 320)) continue;
        if (ground_y(p.x, p.z) > 0.05f) continue;
        int model = random_model();
        float yaw = atan2f(d.x, d.z);
        if (!spot_free(p, yaw, model, 5.0f)) continue;
        int ci = car_spawn(model, p, yaw, AI_TRAFFIC);
        if (ci < 0) return false;
        Car *c = &cars[ci];
        ex[ci].asleep = false;
        float v = L->speed * ex[ci].speed_mul * 0.8f;
        c->vel = Vector3Scale(d, v);
        for (int w = 0; w < 4; w++) ex[ci].spin_w[w] = v / car_models[model].wheel_r;
        ex[ci].route[0] = l;
        route_fill(ci, 0, V(0, 0, 0));
        c->lane = l;
        c->lane_t = t;
        if (spawn_driver(ci) < 0 && !test_no_peds) { c->used = false; return false; }
        return true;
    }
    return false;
}

static bool try_spawn_parked(Vector3 around, float rmin, float rmax)
{
    if (nlanes <= 0) return false;
    for (int tries = 0; tries < 30; tries++) {
        int l = (int)(rnd() % (uint32_t)nlanes);
        Lane *L = &lanes[l];
        if (L->light < 0 || L->len < 34) continue;
        float t = frand(10, L->len - 16);
        Vector3 d = norm2(Vector3Subtract(L->b, L->a));
        Vector3 r = V(-d.z, 0, d.x);
        Vector3 p = Vector3Add(L->a, Vector3Scale(d, t));
        float dist = len2(Vector3Subtract(p, around));
        if (dist < rmin || dist > rmax) continue;
        float e = 0;
        while (e < 8 && ground_y(p.x + r.x * e, p.z + r.z * e) < 0.05f) e += 0.25f;
        if (e < 3.3f || e > 8) continue;
        int model = random_model();
        if (model == CAR_TAXI) model = CAR_SEDAN;
        float hw = car_models[model].size.x * 0.5f;
        float off = e - hw - 0.25f;
        Vector3 spot = Vector3Add(p, Vector3Scale(r, off));
        if (nearest_lane(spot, 2.3f) >= 0) continue;
        float yaw = atan2f(d.x, d.z);
        if (!spot_free(spot, yaw, model, 0.6f)) continue;
        if (ground_y(spot.x, spot.z) > 0.05f) continue;
        if (!test_no_peds && in_view(spot, 90) && dist > 25) continue;
        int ci = car_spawn(model, spot, yaw + frand(-0.03f, 0.03f), AI_PARKED);
        return ci >= 0;
    }
    return false;
}

static void manage_population(void)
{
    if (!nlanes) return;
    Vector3 pf = focus();
    bool initial = !popped || ticks < 90;
    int nt = count_ai(AI_TRAFFIC);
    int budget = initial ? 5 : (ticks % 4 == 0 ? 1 : 0);
    for (int k = 0; k < budget && nt < TRAFFIC_TARGET; k++)
        if (try_spawn_traffic(pf, initial ? 25 : 40)) nt++;
    int np = 0;
    for (int i = 0; i < MAX_CARS; i++)
        if (cars[i].used && cars[i].ai == AI_PARKED && len2(Vector3Subtract(cars[i].pos, pf)) < 200) np++;
    int pb = initial ? 3 : (ticks % 8 == 0 ? 1 : 0);
    for (int k = 0; k < pb && np < PARKED_TARGET; k++)
        if (try_spawn_parked(pf, initial ? 10 : 70, initial ? 100 : 190)) np++;
    popped = true;

    // despawn far or wedged cars when nobody is looking
    int i = (int)(ticks % MAX_CARS);
    Car *c = &cars[i];
    if (!c->used || c->driver == PLAYER || i == peds[PLAYER].car) return;
    float d = len2(Vector3Subtract(c->pos, pf));
    bool far = d > 180 && !in_view(c->pos, 330);   // recycled into the ring near the player
    bool wedged = (c->ai == AI_TRAFFIC || c->ai == AI_FLEE) && c->stuck_s > 8 && !in_view(c->pos, 400);
    bool junk = (c->ai == AI_NONE || c->ai == AI_WRECK || c->ai == AI_PARKED) && d > 300 && !in_view(c->pos, 500);
    bool off_world = fabsf(c->pos.x) > WORLD_HALF + 60 || fabsf(c->pos.z) > WORLD_HALF + 60;
    if ((far && (c->ai == AI_TRAFFIC || c->ai == AI_FLEE)) || wedged || junk || off_world || c->pos.y < -5.5f) car_remove(i);
}

// ---------------------------------------------------------------- per-car update

static void fire_and_smoke(int ci, float dt)
{
    Car *c = &cars[ci];
    Ex *x = &ex[ci];
    if (x->water) { c->fire_s = 0; return; }
    Vector3 f = car_fwd(c), up = qrot(c->rot, V(0, 1, 0));
    Vector3 hood = Vector3Add(Vector3Add(c->pos, Vector3Scale(f, half_l(c) - 0.7f)), Vector3Scale(up, 0.45f));
    if (c->ai == AI_WRECK) {
        x->burn_s += dt;
        if (x->burn_s < 25) {
            c->fire_s = x->burn_s + 0.01f;
            if ((ticks + ci) % 2 == 0) fx_burst(FX_FIRE, Vector3Add(c->pos, Vector3Scale(up, 0.5f)), up, 1);
            if ((ticks + ci) % 4 == 0) fx_burst(FX_SMOKE, Vector3Add(c->pos, Vector3Scale(up, 0.7f)), up, 1);
        } else {
            c->fire_s = 0;
            if (x->burn_s < 90 && (ticks + ci) % 8 == 0) fx_burst(FX_SMOKE, Vector3Add(c->pos, Vector3Scale(up, 0.7f)), up, 1);
        }
        return;
    }
    if (c->health < 150) {
        c->fire_s += dt;
        c->health -= 25 * dt;
        if ((ticks + ci) % 2 == 0) fx_burst(FX_FIRE, hood, up, 1);
        if ((ticks + ci) % 3 == 0) fx_burst(FX_SMOKE, hood, up, 1);
        if (c->fire_s > 1.2f) bail_out(ci);
        if (c->fire_s > 5 || c->health <= 0) { c->health = 0; explode_car(ci); }
    } else {
        c->fire_s = 0;
        if (c->health < 400 && (ticks + ci) % (c->health < 250 ? 3 : 6) == 0) fx_burst(FX_SMOKE, hood, up, 1);
    }
}

static void lights_flags(Car *c)
{
    c->lights = night() && c->ai != AI_WRECK;
}

static void control(int ci, float dt)
{
    Car *c = &cars[ci];
    Ex *x = &ex[ci];
    bool ai_ours = (c->ai == AI_TRAFFIC || c->ai == AI_FLEE) && c->driver >= 0 && c->driver != PLAYER && !x->exploded;
#ifdef COPS_TEST
    if (c->driver == PLAYER && getenv("OOP_AUTO") && ticks > 100) {
        ai_lane_drive(ci, dt, 0, V(0, 0, 0), 24, getenv("OOP_AUTO")[0] == 'r');
        ai_apply_reverse(c, x, dt);
        return;
    }
#endif
    if (c->driver == PLAYER) { x->no_rev = false; return; }   // parked cars keep no_rev set: S never reversed
    if (x->test_ctl) return;
    if (ai_ours && x->drive_tick + 1 >= ticks && x->drive_tick != 0) { return; } // someone else is driving it
    if (c->ai == AI_FLEE && c->driver >= 0) {
        if (!x->flee_init) { x->flee_init = true; if (c->ai_timer <= 0) c->ai_timer = 25; }
        c->ai_timer -= dt;
        if (c->ai_timer <= 0) { c->ai = AI_TRAFFIC; x->flee_init = false; for (int i = 0; i < 4; i++) x->route[i] = -1; }
    }
    if (c->ai == AI_FLEE && c->driver >= 0) {
        Vector3 threat = peds[PLAYER].car >= 0 ? cars[peds[PLAYER].car].pos : peds[PLAYER].pos;
        ai_lane_drive(ci, dt, 2, threat, 0, true);
        ai_apply_reverse(c, x, dt);
    } else if (c->ai == AI_TRAFFIC && c->driver >= 0) {
        ai_lane_drive(ci, dt, 0, V(0, 0, 0), 0, false);
        ai_apply_reverse(c, x, dt);
    } else if (c->ai == AI_COP && c->driver >= 0) {
        // cops module writes the controls
        if (x->drive_tick + 2 < ticks) { c->throttle = 0; c->brake = 0.3f; }
    } else {
        // nobody at the wheel: roll to a stop and park (unless something is steering it from outside)
        if (x->drive_tick + 2 >= ticks && x->drive_tick) return;
        c->throttle = 0; c->steer = 0;
        float v = len2(c->vel);
        c->brake = v > 1 ? 0.35f : 0;
        c->handbrake = v < 1.2f;
        x->no_rev = true;
        if (c->ai == AI_WRECK) c->handbrake = false;
    }
    if (x->honk_s > 0) { c->horn = true; x->honk_s -= dt; } else if (c->ai == AI_TRAFFIC || c->ai == AI_FLEE) c->horn = false;
    if (x->honk_cd > 0) x->honk_cd -= dt;
}

static void maybe_sleep(int ci, float dt)
{
    Car *c = &cars[ci];
    Ex *x = &ex[ci];
    if (c->ai != AI_PARKED || c->driver >= 0) { x->still_s = 0; return; }
    float v = Vector3Length(c->vel), w = Vector3Length(c->angvel);
    if (v < 0.08f && w < 0.05f && c->wheel_ground[0] && c->wheel_ground[1] && c->wheel_ground[2] && c->wheel_ground[3]) {
        x->still_s += dt;
        if (x->still_s > 1.0f) { x->asleep = true; c->vel = V(0, 0, 0); c->angvel = V(0, 0, 0); }
    } else x->still_s = 0;
}

#ifdef CAR_TEST
static void test_tick(float dt);
static void test_timing(struct timespec *t0);
#endif

void cars_tick(float dt)
{
#ifdef CAR_TEST
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    test_tick(dt);
#endif
    manage_population();
#ifdef CAR_TEST
    dbg("start");
#endif
    for (int i = 0; i < MAX_PEDS; i++) if (ped_hit_cd[i] > 0) ped_hit_cd[i] -= dt;
    for (int i = 0; i < MAX_CARS; i++) {
        Car *c = &cars[i];
        if (!c->used) continue;
        Ex *x = &ex[i];
        sync_driver(i);
        if (c->driver >= 0 && x->asleep) wake(i);
        if (x->crash_cd > 0) x->crash_cd -= dt;
        if (x->trip_cd > 0) x->trip_cd -= dt;
        control(i, dt);
        lights_flags(c);
        if (c->ai == AI_PARKED && c->driver < 0 && x->asleep) {
            for (int w = 0; w < 4; w++) c->wheel_comp[w] = 0;
            c->skid = 0; c->rpm = 0;
            continue;
        }
#ifdef CAR_TEST
        Vector3 vb = c->vel;
#endif
        for (int s = 0; s < SUBSTEPS; s++) step_car(i, dt / SUBSTEPS);
#ifdef CAR_TEST
        if (getenv("CAR_DBG3") && i == tcar && ticks % 30 == 0) printf("dv/dt %.2f speed %.1f\n", (c->vel.z - vb.z) / dt + (c->vel.x - vb.x) / dt, car_speed(c));
#endif
        wheel_fx(i, dt);
        maybe_sleep(i, dt);
    }
#ifdef CAR_TEST
    dbg("phys");
#endif
    for (int i = 0; i < MAX_CARS; i++) {
        Car *c = &cars[i];
        if (!c->used) continue;
        if (ex[i].asleep && c->ai == AI_PARKED && c->driver < 0) {
            // sleeping cars still collide with awake ones, handled in the pair loop
        } else {
#ifdef CAR_TEST
            Vector3 v0 = cars[i].vel;
            world_collide(i);
            if (getenv("CAR_DBG2") && i == tcar && Vector3Distance(v0, cars[i].vel) > 0.01f) printf("wc dv %.3f at %.0f %.0f\n", Vector3Distance(v0, cars[i].vel), cars[i].pos.x, cars[i].pos.z);
#else
            world_collide(i);
#endif
        }
    }
#ifdef CAR_TEST
    dbg("world");
#endif
    for (int i = 0; i < MAX_CARS; i++) {
        if (!cars[i].used) continue;
        for (int j = i + 1; j < MAX_CARS; j++) {
            if (!cars[j].used) continue;
            if (fabsf(cars[i].pos.x - cars[j].pos.x) > 6 || fabsf(cars[i].pos.z - cars[j].pos.z) > 6) continue;
            if (ex[i].asleep && ex[j].asleep) continue;
            car_vs_car(i, j);
        }
    }
#ifdef CAR_TEST
    dbg("pairs");
#endif
    for (int i = 0; i < MAX_CARS; i++) {
        if (!cars[i].used) continue;
        car_vs_peds(i);
        fire_and_smoke(i, dt);
    }
#ifdef CAR_TEST
    for (int i = 0; i < MAX_CARS; i++) if (cars[i].used && (!isfinite(cars[i].pos.x + cars[i].pos.y + cars[i].pos.z + cars[i].rot.w + cars[i].rot.x + cars[i].vel.x + cars[i].angvel.x))) { printf("NaN car %d ai %d tick %llu\n", i, cars[i].ai, (unsigned long long)ticks); cars[i].used = false; }
    dbg("end");
    test_timing(&t0);
#endif
}

// ---------------------------------------------------------------- drawing

static void headlight(Car *c, float reach, float radius, float k)
{
    Vector3 f = car_fwd(c), l = qrot(c->rot, V(1, 0, 0));
    Vector3 base = Vector3Add(Vector3Add(c->pos, Vector3Scale(f, half_l(c) + reach)), V(0, 0.1f, 0));
    Vector3 color = V(1.45f * k, 0.95f * k, 0.48f * k);
    light_add(Vector3Add(base, Vector3Scale(l, 0.5f)), color, radius);
    light_add(Vector3Subtract(base, Vector3Scale(l, 0.5f)), color, radius);
}

static bool strobe_on(float phase)
{
    phase -= floorf(phase);
    return phase < 0.12f || (phase > 0.2f && phase < 0.32f);
}

static void car_lights(void)
{
    int budget = 8;
    int pc = peds[PLAYER].car;
    float ph = (float)game_s * 2.6f;
    if (night()) {
        bool done[MAX_CARS] = { 0 };
        if (pc >= 0 && cars[pc].used && cars[pc].ai != AI_WRECK) { headlight(&cars[pc], 4, 14, 1.0f); done[pc] = true; budget -= 2; }
        for (; budget > 0; budget--) {
            int best = -1;
            float bd = 40 * 40;
            for (int i = 0; i < MAX_CARS; i++) {
                Car *c = &cars[i];
                if (!c->used || done[i] || !c->lights || c->ai == AI_PARKED) continue;
                float d = Vector3DistanceSqr(c->pos, cam.position);
                if (d < bd) { bd = d; best = i; }
            }
            if (best < 0) break;
            done[best] = true;
            headlight(&cars[best], 5, 12, 0.8f);
        }
    }
    // sirens light up the street
    int sirens = 0;
    for (int i = 0; i < MAX_CARS && sirens < 4; i++) {
        Car *c = &cars[i];
        if (!c->used || !c->siren) continue;
        if (Vector3DistanceSqr(c->pos, cam.position) > 90 * 90) continue;
        sirens++;
        Vector3 roof = Vector3Add(c->pos, V(0, car_models[c->model].size.y * 0.65f, 0));
        Vector3 l = qrot(c->rot, V(1, 0, 0));
        if (strobe_on(ph)) light_add(Vector3Add(roof, Vector3Scale(l, 0.3f)), V(7, 0.3f, 0.2f), 24);
        if (strobe_on(ph + 0.5f)) light_add(Vector3Subtract(roof, Vector3Scale(l, 0.3f)), V(0.4f, 1.0f, 7), 24);
    }
}

void cars_draw(int pass)
{
#ifdef CAR_TEST
    if (getenv("CAR_LIST") && pass == PASS_MAIN) for (int i = 0; i < MAX_CARS; i++) if (cars[i].used) printf("car %d ai %d model %d pos %.1f %.1f %.1f cam %.1f %.1f %.1f\n", i, cars[i].ai, cars[i].model, cars[i].pos.x, cars[i].pos.y, cars[i].pos.z, cam.position.x, cam.position.y, cam.position.z);
#endif
    if (pass == PASS_MAIN) car_lights();
    for (int i = 0; i < MAX_CARS; i++) {
        Car *c = &cars[i];
        if (!c->used) continue;
        float d2 = Vector3DistanceSqr(c->pos, cam.position);
        if (d2 > 450 * 450) continue;
        const CarModel *m = &car_models[c->model];
        Matrix M = car_matrix(c);
        int flags = 0;
        if (c->lights) flags |= 1;
        if (c->brake > 0.1f && car_speed(c) > -0.5f && c->ai != AI_WRECK) flags |= 2;
        if (c->throttle < 0.05f && c->brake > 0.05f && car_speed(c) < 0.6f) flags |= 2;
        if (c->siren) flags |= 4;
        Color tint = c->paint;
        if (c->model == CAR_COP || c->model == CAR_TAXI) tint = c->ai == AI_WRECK ? (Color){ 40, 38, 36, 255 } : WHITE;
        tint.a = (unsigned char)flags;
        if (getenv("CAR_T")) tint = (Color){ 200, 200, 200, 0 };
        if (getenv("CAR_BS")) M = MatrixMultiply(MatrixScale(atof(getenv("CAR_BS")), atof(getenv("CAR_BS")), atof(getenv("CAR_BS"))), M);
        if (getenv("CAR_SKIP") && i == atoi(getenv("CAR_SKIP"))) continue;
        if (getenv("CAR_ONLY") && i != atoi(getenv("CAR_ONLY"))) continue;
        if (!getenv("CAR_NOBODY")) draw_mesh(m->body, M, tint, MAT_CAR);
        Color wt = c->ai == AI_WRECK ? (Color){ 60, 60, 60, 255 } : WHITE;
        wt.a = 0;
        for (int w = 0; w < 4; w++) {
            bool left = m->wheel_pos[w].x > 0;
            float steer = 0;
            if (w < 2) steer = -ex[i].steer_s * lock_of(c, fabsf(car_speed(c)));
            Matrix spin = MatrixRotateX(left ? c->wheel_spin[w] : -c->wheel_spin[w]);
            Matrix yaw = MatrixRotateY(steer + (left ? 0 : PI));
            Matrix t = MatrixTranslate(m->wheel_pos[w].x, m->wheel_r + c->wheel_comp[w], m->wheel_pos[w].z);
            Matrix W = MatrixMultiply(MatrixMultiply(MatrixMultiply(spin, yaw), t), M);
            if (!getenv("CAR_NOWHEEL")) draw_mesh(m->wheel, W, wt, MAT_CAR);
        }
    }
}

#ifdef CAR_TEST
// scenario runner for runs/car, built with -DCAR_TEST, scenario picked by the CAR_TEST env var
float cam_yaw, cam_pitch;
static int tcar = -1, pursuer = -1;
static Input tin;
static int cam_mode;
static const char *scen;
static double t_sum; static long t_n;

static void test_timing(struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    t_sum += (t1.tv_sec - t0->tv_sec) * 1e6 + (t1.tv_nsec - t0->tv_nsec) / 1e3;
    if (++t_n % 600 == 0) {
        int n = 0;
        for (int i = 0; i < MAX_CARS; i++) n += cars[i].used;
        printf("cars_tick avg %.0f us over %ld ticks, %d cars\n", t_sum / t_n, t_n, n);
    }
}

static bool avenue_lane(int skip, Vector3 *p, Vector3 *d)
{
    int seen = 0;
    for (int l = 0; l < nlanes; l++)
        if (lanes[l].speed > 16 && lanes[l].len > 50 && lanes[l].light >= 0 && seen++ >= skip) {
            *d = norm2(Vector3Subtract(lanes[l].b, lanes[l].a));
            *p = lanes[l].a;
            return true;
        }
    return false;
}

static void test_setup(void)
{
    scen = getenv("CAR_TEST");
    Vector3 p = { 0 }, d = { 0, 0, 1 };
    test_no_peds = getenv("CAR_NOPEDS") != NULL;
    int skip = getenv("CAR_LANE") ? atoi(getenv("CAR_LANE")) : 3;
    avenue_lane(skip, &p, &d);
    float yaw = atan2f(d.x, d.z);
    if (getenv("CAR_AT_CURB")) {
        Vector3 r = V(-d.z, 0, d.x);
        float e = 0;
        while (e < 12 && ground_y(p.x + r.x * e, p.z + r.z * e) < 0.05f) e += 0.1f;
        p = Vector3Add(p, Vector3Scale(r, e - 2.2f));
    }
    if (!strcmp(scen, "wall")) {
        // face a building straight across an avenue
        for (int l = 0, seen = 0; l < nlanes; l++) {
            if (lanes[l].speed < 16 || lanes[l].len < 50 || lanes[l].light < 0) continue;
            Vector3 dd = norm2(Vector3Subtract(lanes[l].b, lanes[l].a));
            Vector3 mid = Vector3Lerp(lanes[l].a, lanes[l].b, 0.5f);
            bool done = false;
            for (int side = -1; side <= 1 && !done; side += 2) {
                Vector3 nd = Vector3Scale(left_h(dd), (float)side);
                Hit h;
                Vector3 o = V(mid.x, 0.9f, mid.z);
                if (world_ray(o, nd, 80, &h) && h.t > 22 && h.t < 34 && seen++ >= skip) {
                    p = mid; d = nd; yaw = atan2f(d.x, d.z);
                    printf("wall %.0f m ahead at %.0f %.0f\n", h.t, p.x, p.z);
                    done = true;
                }
            }
            if (done) break;
        }
    }
    if (!strcmp(scen, "water")) {
        for (float z = -600; z < 600; z += 3)
            for (float x = -600; x < 600; x += 3) {
                if (fabsf(ground_y(x, z) - 0.05f) > 0.001f) continue;
                Vector3 dirs[4] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
                for (int k = 0; k < 4; k++) {
                    float e = 0;
                    while (e < 200 && fabsf(ground_y(x + dirs[k].x * e, z + dirs[k].z * e) - 0.05f) < 0.001f) e += 1;
                    if (e > 30 && is_water(x + dirs[k].x * (e + 2), z + dirs[k].z * (e + 2)) && ground_y(x - dirs[k].x * 6, z - dirs[k].z * 6) < 0.06f) {
                        p = V(x, 0.05f, z); d = dirs[k]; yaw = atan2f(d.x, d.z);
                        printf("pier at %.0f %.0f dir %.0f %.0f len %.0f\n", x, z, d.x, d.z, e);
                        goto found;
                    }
                }
            }
    }
found:
    if (strcmp(scen, "traffic")) {
        int model = getenv("CAR_MODEL") ? atoi(getenv("CAR_MODEL")) : CAR_SEDAN;
        tcar = car_spawn(model, Vector3Subtract(p, Vector3Scale(d, 0)), yaw, AI_NONE);
        cars[tcar].paint = (Color){ 150, 24, 28, 255 };
        ex[tcar].test_ctl = true;
        ex[tcar].asleep = false;
        float v0 = getenv("CAR_V0") ? (float)atof(getenv("CAR_V0")) : 0;
        cars[tcar].vel = Vector3Scale(d, v0);
        if (getenv("CAR_VSIDE")) cars[tcar].vel = Vector3Add(cars[tcar].vel, Vector3Scale(V(-d.z, 0, d.x), (float)atof(getenv("CAR_VSIDE"))));
        if (!strcmp(scen, "fire")) cars[tcar].health = 140;
        if (getenv("CAR_SIREN")) cars[tcar].siren = true;
        if (!strcmp(scen, "cars")) {
            Vector3 q = Vector3Add(p, Vector3Scale(d, 70));
            int b = car_spawn(CAR_SUV, q, yaw + 1.5708f, AI_NONE);
            cars[b].handbrake = true;
            ex[b].test_ctl = true;
        }
        if (!strcmp(scen, "chase")) {
            int b = car_spawn(CAR_COP, Vector3Subtract(p, Vector3Scale(d, 45)), yaw, AI_COP);
            pursuer = b;
            cars[b].siren = true;
        }
        if (!strcmp(scen, "cars2")) { // head-on-ish: second car comes the other way
            Vector3 q = Vector3Add(p, Vector3Scale(d, 90));
            int b = car_spawn(CAR_COMPACT, q, yaw + 3.14159f, AI_NONE);
            cars[b].vel = Vector3Scale(d, -20);
            cars[b].throttle = 0.5f;
            ex[b].test_ctl = true;
        }
        peds[PLAYER].pos = p;
    } else {
        Vector3 q = police_pos;
        peds[PLAYER].pos = q;
    }
}

static void test_tick(float dt)
{
    static int calls;
    peds[PLAYER].used = true;
    if (++calls == 41 && nlanes) test_setup();
    if (tcar >= 0 && cars[tcar].used) {
        Car *c = &cars[tcar];
        c->steer = tin.mx;
        c->throttle = tin.my > 0 ? tin.my : 0;
        c->brake = tin.my < 0 ? -tin.my : 0;
        c->handbrake = tin.held & BTN_JUMP;
        c->horn = tin.held & BTN_HORN;
        if (ex[tcar].exploded) { c->throttle = c->brake = 0; }
        ex[tcar].no_rev = false;
        peds[PLAYER].pos = V(c->pos.x, 80, c->pos.z);
        { static float minup = 2; float u = qrot(c->rot, V(0, 1, 0)).y; if (u < minup) { minup = u; if (u < 0.95f) printf("minup %.2f at t %.2f\n", u, game_s); } }
        if (ticks % 15 == 0 && getenv("CAR_YAW"))
            printf("yaw %.0f ", atan2f(car_fwd(c).x, car_fwd(c).z) * 57.3f);
        if (ticks % 30 == 0)
            printf("t %.1f v %.1f m/s %.0f km/h rpm %.2f gear %d skid %.2f hp %.0f fire %.1f pos %.0f %.0f y %.2f up %.2f\n", game_s,
                   car_speed(c), car_speed(c) * 3.6f, c->rpm, ex[tcar].gear + 1, c->skid, c->health, c->fire_s, c->pos.x, c->pos.z, c->pos.y,
                   qrot(c->rot, V(0, 1, 0)).y);
    }
    static int prev_lane[MAX_CARS]; static int crossed, ran_red, ran_yellow;
    for (int i = 0; i < MAX_CARS; i++) {
        Car *c = &cars[i];
        if (!c->used || c->ai != AI_TRAFFIC) { prev_lane[i] = -1; continue; }
        int pl = prev_lane[i];
        if (pl >= 0 && pl != c->lane && lanes[pl].light >= 0) {
            crossed++;
            int st = light_state(lanes[pl].light);
            if (st == 2) ran_red++; else if (st == 1) ran_yellow++;
        }
        prev_lane[i] = c->lane;
    }
    if (ticks % 600 == 0) printf("crossed %d ran_red %d ran_yellow %d\n", crossed, ran_red, ran_yellow);
    if (ticks % 120 == 0) {
        int n = 0, moving = 0, drivers = 0, stuck = 0, parked = 0, asleep = 0;
        for (int i = 0; i < MAX_CARS; i++) {
            Car *c = &cars[i];
            if (!c->used) continue;
            if (c->ai == AI_PARKED) { parked++; asleep += ex[i].asleep; continue; }
            if (c->ai != AI_TRAFFIC && c->ai != AI_FLEE) continue;
            n++; moving += len2(c->vel) > 1.5f; drivers += c->driver >= 0; stuck += c->stuck_s > 3;
        }
        printf("t %.0f traffic %d moving %d drivers %d stuck %d parked %d asleep %d\n", game_s, n, moving, drivers, stuck, parked, asleep);
    }
    static int fleer = -1;
    if (scen && !strcmp(scen, "traffic") && game_s > 3 && fleer < 0)
        for (int i = 0; i < MAX_CARS; i++) if (cars[i].used && cars[i].ai == AI_TRAFFIC && cars[i].driver >= 0 && len2(cars[i].vel) > 5) { fleer = i; cars[i].ai = AI_FLEE; break; }
    if (fleer >= 0 && cars[fleer].used && ticks % 60 == 0) printf("flee car %d ai %d v %.1f stuck %.1f\n", fleer, cars[fleer].ai, car_speed(&cars[fleer]), cars[fleer].stuck_s);
    if (pursuer >= 0 && cars[pursuer].used && tcar >= 0) {
        car_drive_to(pursuer, cars[tcar].pos, 45, true);
        if (ticks % 60 == 0) printf("thr %.2f brk %.2f hb %d lane %d route %d  ", cars[pursuer].throttle, cars[pursuer].brake, cars[pursuer].handbrake, cars[pursuer].lane, ex[pursuer].route[0]);
        if (ticks % 60 == 0) printf("chase dist %.1f pursuer v %.1f target v %.1f hp %.0f\n", Vector3Distance(cars[pursuer].pos, cars[tcar].pos), car_speed(&cars[pursuer]), car_speed(&cars[tcar]), cars[pursuer].health);
    }
    (void)dt;
}

void camera_update(const Input *in, float dt)
{
    tin = *in;
    if (in->hit & BTN_CAM) cam_mode = (cam_mode + 1) % 6;
    Vector3 at = tcar >= 0 && cars[tcar].used ? cars[tcar].pos : peds[PLAYER].pos;
    Vector3 f = tcar >= 0 && cars[tcar].used ? norm2(car_fwd(&cars[tcar])) : V(0, 0, 1);
    Vector3 l = left_h(f);
    static Vector3 sm;
    sm = Vector3Lerp(sm, at, 0.25f);
    Vector3 pos, tgt = Vector3Add(at, V(0, 0.8f, 0));
    switch (cam_mode) {
    case 0: pos = Vector3Add(at, Vector3Add(Vector3Scale(f, -9), V(0, 3.2f, 0))); break;
    case 1: pos = Vector3Add(at, Vector3Add(Vector3Scale(l, 8), Vector3Add(Vector3Scale(f, 4), V(0, 1.5f, 0)))); break;
    case 2: pos = Vector3Add(sm, V(0, 55, -12)); tgt = sm; break;
    case 3: pos = Vector3Add(sm, V(0, 150, -30)); tgt = sm; break;
    case 4: pos = Vector3Add(at, Vector3Add(Vector3Scale(f, 12), Vector3Add(Vector3Scale(l, -3), V(0, 1.6f, 0)))); break;
    default: pos = Vector3Add(at, Vector3Add(Vector3Scale(f, -22), V(0, 9, 0))); break;
    }
    cam = (Camera3D){ pos, tgt, { 0, 1, 0 }, 60, CAMERA_PERSPECTIVE };
    (void)dt;
}
#endif
