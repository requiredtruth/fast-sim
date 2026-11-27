#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ped.h"
#include "combat.h"
#include "cops.h"
#include "city.h"

int heat;
float heat_search;
Vector3 last_seen;

#define MAX_COP_PEDS 22
#define MAX_WIT 4
#define WITNESS_CALL_S 6.0f
#define LOST_S 6.0f   // radio keeps last_seen on the real player this long after sight is lost

static const float level_at[6] = { 0, 12, 38, 75, 120, 180 };
static const float crime_pts[11] = { 14, 30, 5, 16, 6, 14, 35, 70, 40, 12, 0 };
static const float crime_gap[11] = { 1.5f, 1.0f, 3.0f, 1.5f, 4.0f, 1.0f, 1.5f, 1.0f, 5.0f, 1.0f, 1.0f };
static const int want_units[6] = { 0, 1, 3, 4, 6, 8 };
static const float spawn_gap[6] = { 0, 5, 3.5f, 3, 2.5f, 1.8f };

static int spawn_fails;
static float score, seen_s = 99, resist_s = 99, arrest_t, spawn_cd, block_cd;
static int seen_vehicle = -1;
static bool was_in_car;
static uint32_t crime_id;

typedef struct { int ped; float t, pts; Vector3 at; uint32_t id; } Witness;
static Witness wit[MAX_WIT];

typedef struct {
    bool swat, block, sees, los_ok, partner_done;
    float last_x, last_z, stall_t, jink_t, jink_dir, shoot_cd, aim_t, repath_t, pause_t, los_t, side_t, side_dir, lost_los_s, stuck_t;
    int burst, home_car;
    Vector3 goal;
    bool has_goal;
    int path[48], path_n, path_i;
} Cop;
static Cop cop[MAX_PEDS];

typedef struct { bool mine, block; float age; } CCar;
static CCar ccar[MAX_CARS];

static Vector3 v3(float x, float y, float z) { return (Vector3){ x, y, z }; }
static float dist_xz(Vector3 a, Vector3 b) { return hypotf(a.x - b.x, a.z - b.z); }

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

static bool los(Vector3 a, Vector3 b)
{
    Vector3 d = Vector3Subtract(b, a);
    float len = Vector3Length(d);
    if (len < 0.5f) return true;
    Hit h;
    return !world_ray(a, Vector3Scale(d, 1 / len), len - 0.3f, &h);
}

static bool is_cop(int i) { return i > 0 && i < MAX_PEDS && peds[i].used && peds[i].kind == PED_COP; }
static bool cop_alive(int i) { return is_cop(i) && peds[i].state != PS_DEAD; }

static void shout(int i, int line) { voice(line, peds[i].pos, 1, -1); }

static int cop_near(Vector3 p)
{
    int best = -1;
    float bd = 1e9f;
    for (int i = 1; i < MAX_PEDS; i++) {
        float d = cop_alive(i) ? Vector3Distance(peds[i].pos, p) : 1e9f;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static bool visible(Vector3 p)
{
    Vector3 d = Vector3Subtract(p, cam.position), f = Vector3Subtract(cam.target, cam.position);
    float dl = Vector3Length(d), fl = Vector3Length(f);
    if (dl < 1 || fl < 0.01f) return true;
    return Vector3DotProduct(d, f) / (dl * fl) > 0.4f;
}

static Vector3 player_pos(void)
{
    Ped *p = &peds[PLAYER];
    return p->car >= 0 && cars[p->car].used ? cars[p->car].pos : p->pos;
}

static Vector3 player_point(void)
{
    Ped *p = &peds[PLAYER];
    if (p->car >= 0 && cars[p->car].used) return Vector3Add(cars[p->car].pos, v3(0, 0.5f, 0));
    return Vector3Add(p->pos, v3(0, 1.2f, 0));
}

static Vector3 player_vel(void)
{
    Ped *p = &peds[PLAYER];
    return p->car >= 0 && cars[p->car].used ? cars[p->car].vel : p->vel;
}

static float player_speed(void)
{
    Vector3 v = player_vel();
    return hypotf(v.x, v.z);
}

static Vector3 eye_of(int i)
{
    Ped *p = &peds[i];
    return Vector3Add(p->pos, v3(0, p->state == PS_IN_CAR ? 1.0f : 1.55f, 0));
}

// ---- the helicopter

#define HELI_ALT_M 40
#define HELI_SPEED_MS 30
#define HELI_ACCEL_MS2 9
#define HELI_ORBIT_M 45
#define HELI_SPOT_LAG 2.5f

static struct {
    bool on, sees;
    Vector3 pos, vel, spot;
    float yaw, yaw_rate, bank, pitch, rotor, orbit;
} heli;

#ifdef COPS_TEST
static bool no_cars(void) { return getenv("OOP_NOCARS") != NULL; }
#else
static bool no_cars(void) { return false; }
#endif

static void heli_spawn(void)
{
    Vector3 pp = player_pos();
    float a = frand(0, 2 * PI);
    heli.pos = v3(Clamp(pp.x + 260 * sinf(a), -620, 620), HELI_ALT_M, Clamp(pp.z + 260 * cosf(a), -620, 620));
    heli.vel = Vector3Zero();
    heli.yaw = atan2f(pp.x - heli.pos.x, pp.z - heli.pos.z);
    heli.spot = pp;
    heli.bank = heli.pitch = heli.yaw_rate = 0;
    heli.sees = false;
    heli.on = true;
}

static void heli_tick(float dt)
{
    if (!heli.on) {
        if (heat >= 5) heli_spawn();
        return;
    }
    Vector3 pp = player_pos();
    bool leaving = heat < 4;
    bool orbiting = !heli.sees && heat_search > 0;
    Vector3 goal = pp;
    heli.orbit += 0.3f * dt;
    if (orbiting) goal = v3(last_seen.x + HELI_ORBIT_M * cosf(heli.orbit), 0, last_seen.z + HELI_ORBIT_M * sinf(heli.orbit));
    Vector3 want = Vector3Scale(Vector3Subtract(goal, heli.pos), 0.35f);
    want.y = 0;
    if (leaving) {
        Vector3 out = Vector3Subtract(heli.pos, pp);
        out.y = 0;
        want = Vector3Scale(Vector3Normalize(out), HELI_SPEED_MS);
    }
    float sp = Vector3Length(want);
    if (sp > HELI_SPEED_MS) want = Vector3Scale(want, HELI_SPEED_MS / sp);
    Vector3 dv = Vector3Subtract(want, heli.vel);
    float dl = Vector3Length(dv);
    if (dl > HELI_ACCEL_MS2 * dt) dv = Vector3Scale(dv, HELI_ACCEL_MS2 * dt / dl);
    heli.vel = Vector3Add(heli.vel, dv);
    heli.pos = Vector3Add(heli.pos, Vector3Scale(heli.vel, dt));
    float alt = HELI_ALT_M + 4 * sinf(game_s * 0.4f) + (leaving ? 15 : 0);
    heli.pos.y += (alt - heli.pos.y) * Clamp(dt * 0.8f, 0, 1);

    float speed = hypotf(heli.vel.x, heli.vel.z);
    Vector3 look = speed > 4 ? heli.vel : Vector3Subtract(goal, heli.pos);
    float old = heli.yaw;
    heli.yaw = turn_to(heli.yaw, atan2f(look.x, look.z), 1.1f, dt);
    heli.yaw_rate += (angle_diff(old, heli.yaw) / dt - heli.yaw_rate) * 0.1f;
    heli.bank += (Clamp(-heli.yaw_rate * speed * 0.035f, -0.45f, 0.45f) - heli.bank) * 0.08f;
    heli.pitch += (speed * 0.011f - heli.pitch) * 0.08f;
    heli.rotor = fmodf(heli.rotor + (2 * PI * 3.7f) * dt, 2 * PI);

    Vector3 aim = heli.sees || seen_s < LOST_S ? pp : last_seen;
    aim.x += 5 * sinf(game_s * 0.9f);
    aim.z += 5 * cosf(game_s * 0.7f);
    aim.y = ground_y(aim.x, aim.z);
    heli.spot = Vector3Lerp(heli.spot, aim, Clamp(HELI_SPOT_LAG * dt, 0, 1));

    if (leaving && dist_xz(heli.pos, pp) > 450) { heli.on = false; heli.sees = false; }
}

bool cops_heli(Vector3 *pos, Vector3 *vel)
{
    if (!heli.on) return false;
    *pos = heli.pos;
    *vel = heli.vel;
    return true;
}

void cops_draw(int pass)
{
    if (!heli.on) return;
    Matrix m = MatrixMultiply(MatrixMultiply(MatrixRotateZ(heli.bank), MatrixRotateX(heli.pitch)), MatrixRotateY(heli.yaw));
    m = MatrixMultiply(m, MatrixTranslate(heli.pos.x, heli.pos.y, heli.pos.z));
    draw_mesh(heli_mesh[0], m, WHITE, MAT_PROP);
    draw_mesh(heli_mesh[1], MatrixMultiply(MatrixRotateY(heli.rotor), m), WHITE, MAT_PROP);
    if (pass != PASS_MAIN) return;
    light_add(Vector3Add(heli.spot, v3(0, 6, 0)), v3(0.75f, Lerp(0.72f, 0.52f, r_night), Lerp(0.62f, 0.31f, r_night)), 16);
    light_add(Vector3Add(heli.spot, v3(0, 9, 0)), v3(0.14f, Lerp(0.13f, 0.10f, r_night), Lerp(0.11f, 0.057f, r_night)), 34);
}

// ---- the heat score

static int level_for(float s)
{
    int l = 0;
    for (int k = 1; k < 6; k++) if (s >= level_at[k]) l = k;
    return l;
}

static void add_heat(float pts, Vector3 at, bool cop_saw)
{
    int before = heat;
    score += pts;
    int lv = level_for(score);
    if (lv > heat) {
        heat = lv;
        voice(S_V_PURSUIT, at, 1, -1);
        if (before == 0) spawn_cd = 0.5f;
    }
    if (cop_saw) {
        last_seen = player_point();
        seen_s = 0;
        seen_vehicle = peds[PLAYER].car;
        heat_search = 0;
        if (before == 0) hud_msg("Police spotted you", 2.5f);
    } else {
        last_seen = at;
        seen_s = fminf(seen_s, 2);
        heat_search = 0;
    }
}

static void start_witness(Vector3 at, float pts, int victim)
{
    int slot = -1;
    for (int w = 0; w < MAX_WIT; w++) if (wit[w].ped <= 0) { slot = w; break; }
    if (slot < 0) return;
    Vector3 eye = Vector3Add(at, v3(0, 1.3f, 0));
    int best = -1;
    float bd = 35;
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        if (!p->used || p->kind != PED_CIV || i == victim) continue;
        if (p->state != PS_WALK && p->state != PS_IDLE && p->state != PS_FLEE && p->state != PS_COWER) continue;
        bool busy = false;
        for (int w = 0; w < MAX_WIT; w++) if (wit[w].ped == i) busy = true;
        float d = dist_xz(p->pos, at);
        if (busy || d >= bd) continue;
        Vector3 to = Vector3Subtract(at, p->pos);
        if (fabsf(angle_diff(p->yaw, atan2f(to.x, to.z))) > 2.3f) continue;
        if (!los(Vector3Add(p->pos, v3(0, 1.55f, 0)), eye)) continue;
        bd = d;
        best = i;
    }
    if (best < 0) return;
    Ped *p = &peds[best];
    p->state = PS_CALLING;
    p->vel = v3(0, 0, 0);
    p->node = p->next_node = -1;
    wit[slot] = (Witness){ best, WITNESS_CALL_S, pts, at, ++crime_id };
    voice(S_V_PHONE, p->pos, 1, -1);
}

void crime(int kind, Vector3 at, int victim)
{
    static float next_ok[11];
    if (kind < 0 || kind > CR_EVADE || crime_pts[kind] <= 0 || game_s < next_ok[kind]) return;
    next_ok[kind] = game_s + crime_gap[kind];
    float pts = crime_pts[kind];
    if (kind == CR_COP_ASSAULT || kind == CR_COP_MURDER) resist_s = 0;
    // cop crimes and shop alarms get radioed in; the rest needs eyes on it
    bool seen = kind == CR_ROBBERY || kind == CR_COP_ASSAULT || kind == CR_COP_MURDER;
    Vector3 eye = Vector3Add(at, v3(0, 1.3f, 0));
    for (int i = 1; i < MAX_PEDS && !seen; i++)
        if (cop_alive(i) && dist_xz(peds[i].pos, at) < 45 && los(eye_of(i), eye)) seen = true;
    if (seen) add_heat(pts, at, kind != CR_ROBBERY);
    else start_witness(at, pts, victim);
    if (kind == CR_ROBBERY) hud_msg("Silent alarm", 2.5f);
}

static void witness_tick(float dt)
{
    Ped *pl = &peds[PLAYER];
    for (int w = 0; w < MAX_WIT; w++) {
        Witness *x = &wit[w];
        if (x->ped <= 0) continue;
        Ped *p = &peds[x->ped];
        if (!p->used || p->state != PS_CALLING) { x->ped = 0; continue; }
        // a gun in their face makes them drop the phone
        if (aim_live && pl->weapon != W_FIST && dist_xz(p->pos, pl->pos) < 16) {
            Vector3 to = Vector3Subtract(Vector3Add(p->pos, v3(0, 1.3f, 0)), aim_from);
            float t = Vector3DotProduct(to, aim_dir);
            if (t > 0 && Vector3Length(Vector3Subtract(to, Vector3Scale(aim_dir, t))) < 1.2f) {
                p->state = PS_FLEE;
                p->panic = 1;
                pv[x->ped].src = pl->pos;
                x->ped = 0;
                continue;
            }
        }
        if ((x->t -= dt) > 0) continue;
        add_heat(x->pts, x->at, false);
        hud_msg("A witness called the cops", 3);
        p->state = PS_WALK;
        p->node = p->next_node = -1;
        x->ped = 0;
    }
}

// ---- walking paths for cops on foot

static float *a_g;
static int *a_from, *a_seen, *a_closed;
static int a_stamp;

static int plan_path(Vector3 from, Vector3 to, int *out, int maxout)
{
    int a = nearest_walk(from, 40), b = nearest_walk(to, 40);
    if (a < 0 || b < 0) return 0;
    if (!a_g) {
        a_g = calloc(nwalk, sizeof *a_g);
        a_from = calloc(nwalk, sizeof *a_from);
        a_seen = calloc(nwalk, sizeof *a_seen);
        a_closed = calloc(nwalk, sizeof *a_closed);
    }
    a_stamp++;
    int open[512], nopen = 0, best = a;
    float open_f[512], best_h = 1e9f;
    a_g[a] = 0; a_from[a] = -1; a_seen[a] = a_stamp;
    open[nopen] = a; open_f[nopen++] = 0;
    for (int it = 0; it < 900 && nopen > 0; it++) {
        int bi = 0;
        for (int k = 1; k < nopen; k++) if (open_f[k] < open_f[bi]) bi = k;
        int n = open[bi];
        open[bi] = open[--nopen]; open_f[bi] = open_f[nopen];
        if (a_closed[n] == a_stamp) continue;
        a_closed[n] = a_stamp;
        float h = dist_xz(walk[n].p, walk[b].p);
        if (h < best_h) { best_h = h; best = n; }
        if (n == b) break;
        for (int k = 0; k < walk[n].n; k++) {
            int m = walk[n].nb[k];
            if (m < 0 || m >= nwalk || a_closed[m] == a_stamp) continue;
            float g = a_g[n] + dist_xz(walk[n].p, walk[m].p);
            if (a_seen[m] == a_stamp && g >= a_g[m]) continue;
            a_g[m] = g; a_from[m] = n; a_seen[m] = a_stamp;
            if (nopen < 512) { open[nopen] = m; open_f[nopen++] = g + dist_xz(walk[m].p, walk[b].p); }
        }
    }
    int chain[400], nc = 0;
    for (int n = best; n >= 0 && nc < 400; n = a_from[n]) chain[nc++] = n;
    int cnt = 0;
    for (int k = nc - 1; k >= 0 && cnt < maxout; k--) out[cnt++] = chain[k];
    return cnt;
}

static Vector3 approach_v(Vector3 v, Vector3 to, float step)
{
    Vector3 d = Vector3Subtract(to, v);
    float len = Vector3Length(d);
    if (len <= step) return to;
    return Vector3Add(v, Vector3Scale(d, step / len));
}

static void move_to(int i, Vector3 goal, float speed, float dt)
{
    Ped *c = &peds[i];
    Cop *k = &cop[i];
    Vector3 wp = goal;
    float dd = dist_xz(c->pos, goal);
    Vector3 from = Vector3Add(c->pos, v3(0, 0.9f, 0));
    Vector3 to = v3(goal.x, from.y, goal.z);
    bool direct = dd < 5 || los(from, to);
    if (!direct) {
        if ((k->repath_t -= dt) <= 0 || k->path_n == 0) {
            k->path_n = plan_path(c->pos, goal, k->path, 48);
            k->path_i = 0;
            k->repath_t = 0.9f;
        }
        while (k->path_i < k->path_n && dist_xz(c->pos, walk[k->path[k->path_i]].p) < 1.6f) k->path_i++;
        if (k->path_i < k->path_n) wp = walk[k->path[k->path_i]].p;
    } else k->path_n = 0;
    Vector3 dir = v3(wp.x - c->pos.x, 0, wp.z - c->pos.z);
    float len = Vector3Length(dir);
    // a tree or a parked car in the way: sidestep it for a moment
    if (k->jink_t > 0) {
        k->jink_t -= dt;
        dir = v3(dir.x + dir.z * k->jink_dir * 1.6f, 0, dir.z - dir.x * k->jink_dir * 1.6f);
        len = Vector3Length(dir);
    }
    if (Vector3Length(c->vel) > 2.5f && len > 3) k->stall_t += dt * (fabsf(c->pos.x - k->last_x) + fabsf(c->pos.z - k->last_z) < 0.03f ? 1 : -2);
    k->stall_t = fmaxf(k->stall_t, 0);
    k->last_x = c->pos.x; k->last_z = c->pos.z;
    if (k->stall_t > 0.35f && k->jink_t <= 0) { k->jink_t = 0.9f; k->jink_dir = rnd() & 1 ? 1 : -1; k->stall_t = 0; }
    Vector3 want = len > 0.1f ? Vector3Scale(dir, speed / len) : v3(0, 0, 0);
    Vector3 hv = approach_v(v3(c->vel.x, 0, c->vel.z), want, 26 * dt);
    c->vel = hv;
    c->pos.x += hv.x * dt;
    c->pos.z += hv.z * dt;
    if (Vector3Length(hv) > 0.3f) c->yaw = turn_to(c->yaw, atan2f(hv.x, hv.z), 11, dt);
}

static void stop_and_face(int i, Vector3 at, float dt)
{
    Ped *c = &peds[i];
    c->vel = approach_v(c->vel, v3(0, 0, 0), 30 * dt);
    c->pos.x += c->vel.x * dt;
    c->pos.z += c->vel.z * dt;
    c->yaw = turn_to(c->yaw, atan2f(at.x - c->pos.x, at.z - c->pos.z), 14, dt);
}

// ---- making cops

static void make_swat(int i)
{
    Ped *p = &peds[i];
    p->armor = 60;
    p->health = 130;
    p->weapon = W_SMG;
    p->pal[R_SHIRT] = (Color){ 20, 22, 26, 255 };
    p->pal[R_PANTS] = (Color){ 26, 28, 32, 255 };
    p->pal[R_HAIR] = (Color){ 18, 18, 20, 255 };
    cop[i].swat = true;
}

static int new_cop(Vector3 at, float yaw, bool swat, int home)
{
    int i = ped_spawn(PED_COP, at, yaw);
    if (i < 0) return -1;
    memset(&cop[i], 0, sizeof cop[i]);
    cop[i].home_car = home;
    peds[i].weapon = W_PISTOL;
    if (swat) make_swat(i);
    return i;
}

static int cop_count(void)
{
    int n = 0;
    for (int i = 1; i < MAX_PEDS; i++) if (is_cop(i)) n++;
    return n;
}

static int units(void)
{
    int n = 0;
    for (int ci = 0; ci < MAX_CARS; ci++) {
        if (!cars[ci].used || !ccar[ci].mine || ccar[ci].block || cars[ci].ai != AI_COP) continue;
        bool active = cars[ci].driver >= 0;
        for (int i = 1; i < MAX_PEDS && !active; i++) active = cop_alive(i) && cop[i].home_car == ci;
        if (active) n++;
    }
    return n;
}

static bool spawn_cop_car(void)
{
    Vector3 pp = player_pos();
    float rmax = 140 + spawn_fails * 15;
    float rmin = fmaxf(50, 70 - spawn_fails * 5);
    float want_dot = fmaxf(0, 0.3f - spawn_fails * 0.05f);
    Vector3 pv_ = player_vel();
    float psp = hypotf(pv_.x, pv_.z);
    Vector3 pdir = psp > 0.1f ? v3(pv_.x / psp, 0, pv_.z / psp) : v3(0, 0, 0);
    int why[6] = { 0 };
    for (int tries = 0; tries < 160; tries++) {
        float ang = frand(0, 2 * PI), rr = frand(rmin, rmax);
        Vector3 q = v3(pp.x + sinf(ang) * rr, 0, pp.z + cosf(ang) * rr);
        static int spawn_n;
        bool tail = psp > 8 && spawn_fails < 4 && !(heat >= 2 && (spawn_n & 1));
        if (psp > 8 && spawn_fails < 4 && !tail) q = v3(pp.x + pdir.x * rr - pdir.z * frand(-5, 5), 0, pp.z + pdir.z * rr + pdir.x * frand(-5, 5));
        if (tail) q = v3(pp.x - pdir.x * rr - pdir.z * frand(-5, 5), 0, pp.z - pdir.z * rr + pdir.x * frand(-5, 5));
        int l = nearest_lane(q, 25);
        if (l < 0) { why[0]++; continue; }
        Lane *L = &lanes[l];
        if (L->len < 16 || L->nnext == 0) { why[0]++; continue; }
        Vector3 d = Vector3Normalize(v3(L->b.x - L->a.x, 0, L->b.z - L->a.z));
        if (tail && Vector3DotProduct(d, pdir) < 0.7f) { why[3]++; continue; }
        float t = Clamp(Vector3DotProduct(Vector3Subtract(q, L->a), d), 4, L->len - 6);
        Vector3 p = Vector3Add(L->a, Vector3Scale(d, t));
        float dist = dist_xz(p, pp);
        if (dist < rmin || dist > rmax) { why[1]++; continue; }
        if (visible(p) || ground_y(p.x, p.z) > 0.05f) { why[2]++; continue; }
        Vector3 to_pl = v3(pp.x - p.x, 0, pp.z - p.z);
        if (Vector3DotProduct(d, Vector3Scale(to_pl, 1 / dist)) < want_dot) { why[3]++; continue; }
        // a fast player gets chased from behind
        if (tail && Vector3DotProduct(to_pl, pdir) > 0) { why[3]++; continue; }
        bool clash = false;
        for (int c = 0; c < MAX_CARS && !clash; c++) clash = cars[c].used && dist_xz(cars[c].pos, p) < 8;
        if (clash) continue;
        float yaw = atan2f(d.x, d.z);
        int ci = car_spawn(CAR_COP, p, yaw, AI_COP);
        if (ci < 0) return false;
        Car *c = &cars[ci];
        int di = new_cop(c->pos, yaw, heat >= 4 && rnd() % 100 < 60, ci);
        if (di < 0) { car_remove(ci); return false; }
        Ped *dp = &peds[di];
        dp->car = ci;
        dp->seat = 0;
        dp->state = PS_IN_CAR;
        c->driver = di;
        c->siren = true;
        c->target = PLAYER;
        c->vel = Vector3Scale(d, 14);
        ccar[ci] = (CCar){ true, false, 0 };
        spawn_fails = 0;
        spawn_n++;
        return true;
    }
    if (spawn_fails < 8) spawn_fails++;
#ifdef COPS_TEST
    if (getenv("OOP_COPLOG")) fprintf(stderr, "spawn fail: lane %d range %d vis/ground %d dir %d\n", why[0], why[1], why[2], why[3]);
#endif
    return false;
}

static void spawn_roadblock(void)
{
    Ped *pl = &peds[PLAYER];
    Vector3 pv_ = player_vel();
    float sp = hypotf(pv_.x, pv_.z);
    if (pl->car < 0 || sp < 7) return;
    Vector3 ahead = v3(pv_.x / sp, 0, pv_.z / sp);
    Vector3 pp = player_pos();
    Vector3 target = Vector3Add(pp, Vector3Scale(ahead, 170));
    int l = nearest_lane(target, 45);
    if (l < 0) return;
    Lane *L = &lanes[l];
    Vector3 d = Vector3Normalize(v3(L->b.x - L->a.x, 0, L->b.z - L->a.z));
    float t = Clamp(Vector3DotProduct(Vector3Subtract(target, L->a), d), 8, L->len - 8);
    Vector3 p = Vector3Add(L->a, Vector3Scale(d, t));
    float dist = dist_xz(p, pp);
    if (dist < 110 || dist > 230 || visible(p) || ground_y(p.x, p.z) > 0.05f) return;
    if (Vector3DotProduct(Vector3Subtract(p, pp), ahead) < 60) return;
    for (int c = 0; c < MAX_CARS; c++) if (cars[c].used && dist_xz(cars[c].pos, p) < 12) return;
    if (cop_count() > MAX_COP_PEDS - 3) return;
    Vector3 fwd = Vector3DotProduct(d, ahead) >= 0 ? d : Vector3Scale(d, -1);
    Vector3 side = v3(-fwd.z, 0, fwd.x);
    float yaw = atan2f(side.x, side.z);
    for (int s = -1; s <= 1; s += 2) {
        Vector3 q = Vector3Add(p, Vector3Scale(side, s * 2.5f));
        int ci = car_spawn(CAR_COP, q, yaw + s * 0.1f, AI_PARKED);
        if (ci < 0) return;
        cars[ci].siren = true;
        ccar[ci] = (CCar){ true, true, 0 };
        bool swat = rnd() % 100 < 60;
        Vector3 at = Vector3Add(Vector3Add(p, Vector3Scale(fwd, 3.6f)), Vector3Scale(side, s * 1.3f));
        at.y = ground_y(at.x, at.z);
        int j = new_cop(at, atan2f(-fwd.x, -fwd.z), swat, -1);
        if (j < 0) continue;
        peds[j].state = PS_CHASE;
        cop[j].block = true;
    }
}

// ---- cars with cops in them

static void exit_car(int i)
{
    Ped *c = &peds[i];
    Cop *k = &cop[i];
    int ci = c->car;
    if (ci < 0 || !cars[ci].used) return;
    ped_exit_car(i, false);
    if (c->car >= 0) return;
    c->state = PS_CHASE;
    c->node = c->next_node = -1;
    k->home_car = ci;
    if (heat >= 2 && !k->partner_done && cop_count() < MAX_COP_PEDS) {
        k->partner_done = true;
        const CarModel *m = &car_models[cars[ci].model];
        Vector3 at = Vector3Transform(v3(-(m->size.x * 0.5f + 0.9f), 0, m->seat.z - 0.3f), car_matrix(&cars[ci]));
        at.y = ground_y(at.x, at.z);
        int j = new_cop(at, c->yaw, k->swat, ci);
        if (j >= 0) { peds[j].state = PS_CHASE; cop[j].partner_done = true; }
    }
}

// 1 fired, 0 not ready, -1 a bystander is in the way
static int cop_fire(int i, Vector3 from, float err)
{
    Ped *c = &peds[i];
    Vector3 pt = Vector3Add(player_point(), Vector3Scale(player_vel(), 0.15f));
    Vector3 dir = Vector3Normalize(Vector3Subtract(pt, from));
    dir = Vector3Normalize(Vector3Add(dir, v3(frand(-1, 1) * err, frand(-1, 1) * err * 0.6f, frand(-1, 1) * err)));
    Hit h;
    if (shoot_ray(from, dir, 60, i, c->car, &h) && h.kind == HIT_PED && h.id != PLAYER) return -1;
    return fire_weapon(i, from, dir) ? 1 : 0;
}

static void drive_cop(int i, float dt)
{
    Ped *c = &peds[i];
    Cop *k = &cop[i];
    int ci = c->car;
    Car *car = &cars[ci];
    if (car->driver != i || mode != MODE_PLAY) return;
    Vector3 pp = player_pos();
    bool know = seen_s < LOST_S;
    bool on_foot = peds[PLAYER].car < 0;
    float dist = dist_xz(car->pos, pp), mine_v = fabsf(car_speed(car));
    if (know && !on_foot && dist < 45) voice(S_V_PULLOVER, car->pos, 1, -1);
    float psp = player_speed();
    if ((k->los_t -= dt) <= 0) {
        k->los_ok = dist < 70 && los(Vector3Add(car->pos, v3(0, 0.9f, 0)), player_point());
        k->los_t = 0.2f;
    }
    // sirens never wait, so slow and far from the player and out of view means a real wall
    if (mine_v < 2.5f && dist > 60 && !visible(car->pos)) {
        if ((k->stuck_t += dt) > 4) { car_remove(ci); return; }
    } else k->stuck_t = 0;
    // a car that fell far behind is replaced by one spawned nearer, out of view
    if (know && (dist > 220 || (dist > 150 && !visible(car->pos)))) { car_remove(ci); return; }
    if (know) {
        if (on_foot) {
            bool ram = heat >= 3 && i % 2 == 0;   // ram cars keep driving at the player
            if (!ram && (dist < 14 || (dist < 22 && mine_v < 4))) { exit_car(i); return; }
            car_drive_to(ci, pp, dist < 30 ? 12 : Clamp(dist * 0.4f + 14, 24, 38), true);
        } else {
            if (psp < 2.2f && dist < 15) { exit_car(i); return; }
            float sp = Clamp(psp * 1.15f + 4 + heat * 1.5f, 14, 42);
            if (dist > 60) sp += 10;
            if (dist < 45) sp = fminf(sp, psp + 11);   // a ram, not an execution
#ifdef COPS_TEST
            if (getenv("OOP_COPLOG") && ticks % 60 == 0) fprintf(stderr, "   drive cop %d: speed %.1f target %.1f psp %.1f dist %.0f throttle %.2f brake %.2f\n", i, mine_v, sp, psp, dist, car->throttle, car->brake);
#endif
            car_drive_to(ci, pp, sp, true);
            // car_drive_to feels for walls and backs off in the open; when the way is clear, floor it
            if (heat >= 3 && k->los_ok && fabsf(car->steer) < 0.6f && mine_v < psp + 11) { car->throttle = fmaxf(car->throttle, 0.95f); car->brake = 0; }
        }
        if (heat >= 3 && k->sees && dist < 35 && k->shoot_cd <= 0) {
            Vector3 win = Vector3Transform(v3(car_models[car->model].size.x * 0.4f, 0.9f, 0.6f), car_matrix(car));
            int r = cop_fire(i, win, 0.075f);
            k->shoot_cd = r < 0 ? 0.3f : frand(0.9f, 1.7f);
        }
    } else {
        float dl = dist_xz(car->pos, last_seen);
        if (heat_search > 0 && dl < 32) {
            if (i % 2 == 0) { exit_car(i); return; }
            if (!k->has_goal || dist_xz(car->pos, k->goal) < 14) {
                Vector3 q = Vector3Add(last_seen, v3(frand(-70, 70), 0, frand(-70, 70)));
                int l = nearest_lane(q, 60);
                k->goal = l >= 0 ? lanes[l].a : last_seen;
                k->has_goal = true;
            }
            car_drive_to(ci, k->goal, 15, true);
        } else car_drive_to(ci, last_seen, 22, true);
    }
}

// ---- cops on foot

static void foot_cop(int i, float dt)
{
    Ped *c = &peds[i];
    Cop *k = &cop[i];
    Ped *pl = &peds[PLAYER];
    Vector3 pp = player_pos(), pt = player_point();
    float d = dist_xz(c->pos, pp);
    bool know = seen_s < LOST_S && heat > 0;
    c->aiming = false;
    if (staggering(i)) return;
    if (mode != MODE_PLAY) { stop_and_face(i, pp, dt); return; }

    if ((k->los_t -= dt) <= 0) {
        k->los_ok = d < 60 && los(eye_of(i), pt);
        k->los_t = 0.15f;
    }
    if (!k->los_ok) k->lost_los_s += dt; else k->lost_los_s = 0;
    if (k->lost_los_s > 1.2f) k->aim_t = 0;

    float speed = (d > 8 ? 5.6f : 4.7f) + (heat >= 5 ? 0.6f : 0);
    bool shoot_ok = heat >= 2 || resist_s < 4 || shot_age < 2;
    bool compliant = player_speed() < 2.4f && shot_age > 2 && !pl->aiming;

    if (k->block) {
        if (know && shoot_ok && k->los_ok && d < 60) goto shoot;
        stop_and_face(i, know ? pp : v3(c->pos.x + sinf(c->yaw), 0, c->pos.z + cosf(c->yaw)), dt);
        return;
    }
    if (!know) {
        if (dist_xz(c->pos, last_seen) > 38) { move_to(i, last_seen, speed, dt); return; }
        if (k->pause_t > 0) {
            k->pause_t -= dt;
            c->vel = v3(0, 0, 0);
            c->yaw += sinf(game_s * 1.4f + i) * 1.1f * dt;
            return;
        }
        if (!k->has_goal) {
            Vector3 q = Vector3Add(last_seen, v3(frand(-30, 30), 0, frand(-30, 30)));
            int n = nearest_walk(q, 25);
            k->goal = n >= 0 ? walk[n].p : last_seen;
            k->has_goal = true;
            k->path_n = 0;
        }
        move_to(i, k->goal, 2.9f, dt);
        if (dist_xz(c->pos, k->goal) < 2) { k->has_goal = false; k->pause_t = frand(1, 2.5f); }
        return;
    }
    k->has_goal = false;
    if (pl->car < 0 && d < 16) shout(i, pl->weapon != W_FIST ? S_V_DROP : i & 1 ? S_V_FREEZE : S_V_GROUND);
    if (d < 2.0f && compliant) { stop_and_face(i, pp, dt); return; }
    if (shoot_ok && k->los_ok && d <= 28 && d >= 2.5f) goto shoot;
    if (d < 1.3f) { stop_and_face(i, pp, dt); return; }
#ifdef COPS_TEST
    if (getenv("OOP_COPLOG") && ticks % 60 == 0) fprintf(stderr, "foot %d d %.1f vel %.2f los %d pathn %d\n", i, d, Vector3Length(c->vel), k->los_ok, k->path_n);
#endif
    move_to(i, pp, speed, dt);
    return;

shoot:
    c->aiming = true;
    Vector3 hand = ped_hand(c);
    c->aim_pitch = atan2f(pt.y - hand.y, fmaxf(d, 1));
    if (k->side_t > 0) {
        k->side_t -= dt;
        Vector3 f = v3(sinf(c->yaw), 0, cosf(c->yaw));
        Vector3 want = Vector3Scale(v3(f.z, 0, -f.x), k->side_dir * 2.3f);
        c->vel = approach_v(c->vel, want, 30 * dt);
        c->pos.x += c->vel.x * dt;
        c->pos.z += c->vel.z * dt;
        c->yaw = turn_to(c->yaw, atan2f(pp.x - c->pos.x, pp.z - c->pos.z), 14, dt);
    } else if (heat >= 2 && d > 8) move_to(i, pp, speed * 0.9f, dt);   // shoot while closing in
    else stop_and_face(i, pp, dt);
    k->aim_t += dt;
    if (k->shoot_cd > 0) return;
    float err = Lerp(0.10f, 0.025f, fminf(k->aim_t / 5, 1)) * (k->swat ? 0.8f : 1) * (k->block ? 1.3f : 1) * (heat >= 2 && d > 8 ? 0.85f : 1);
    Vector3 dir = Vector3Normalize(Vector3Subtract(pt, hand));
    int r = cop_fire(i, Vector3Add(hand, Vector3Scale(dir, 0.45f)), err);
    if (r < 0) { k->side_t = 0.7f; k->side_dir = rnd() & 1 ? 1 : -1; k->shoot_cd = 0.2f; }
    else if (r > 0) {
        if (k->swat && ++k->burst < 4) k->shoot_cd = 0.1f;
        else { k->burst = 0; k->shoot_cd = k->swat ? frand(0.6f, 0.9f) : heat >= 3 ? frand(0.45f, 0.8f) : frand(0.6f, 1.0f); }
    }
}

// ---- standing down, busted, per-tick

static void stand_down(void)
{
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *c = &peds[i];
        if (!is_cop(i)) continue;
        Cop *k = &cop[i];
        if (c->state == PS_DEAD) continue;
        if (c->state == PS_IN_CAR && c->car >= 0 && ccar[c->car].mine) {
            Car *car = &cars[c->car];
            car->ai = AI_TRAFFIC;
            car->siren = false;
            ccar[c->car].mine = false;
            continue;
        }
        if (!visible(c->pos) || dist_xz(c->pos, player_pos()) > 70) { ped_remove(i); continue; }
        c->kind = PED_CIV;
        c->state = PS_WALK;
        c->node = c->next_node = -1;
        c->weapon = W_FIST;
        c->aiming = false;
        memset(k, 0, sizeof *k);
    }
}

static void lose_them(void)
{
    heat = 0;
    score = 0;
    heat_search = 0;
    arrest_t = 0;
    hud_msg("Lost them", 3);
    voice(S_V_NOTODAY, player_pos(), 1, -1);
    stand_down();
}

static void bust(void)
{
    Ped *p = &peds[PLAYER];
    mode = MODE_BUSTED;
#ifdef COPS_TEST
    fprintf(stderr, "busted at %.2f\n", game_s);
#endif
    weapon_stow();
    int who = cop_near(p->pos);
    if (who > 0) shout(who, S_V_ARREST);
    if (p->car < 0) { p->state = PS_ARREST; p->vel = v3(0, 0, 0); }
    arrest_t = 0;
}

static void arrest_tick(float dt)
{
    Ped *pl = &peds[PLAYER];
    bool slow = player_speed() < 2.4f && shot_age > 2 && !pl->aiming;
    Vector3 pp = player_pos();
    bool near = false;
    for (int i = 1; i < MAX_PEDS && !near; i++) {
        Ped *c = &peds[i];
        if (!cop_alive(i) || cop[i].block || c->state == PS_IN_CAR || c->state == PS_RAGDOLL) continue;
        near = dist_xz(c->pos, pp) < (pl->car >= 0 ? 3.6f : 2.7f);
        if (near && slow && arrest_t > 0.2f) shout(i, S_V_HANDS);
    }
    if (near && slow && pl->state != PS_RAGDOLL) arrest_t += dt;
    else arrest_t = fmaxf(0, arrest_t - dt * 1.5f);
#ifdef COPS_TEST
    if (getenv("OOP_COPLOG") && ticks % 30 == 0) fprintf(stderr, "arrest: near %d slow %d speed %.2f shot_age %.1f aiming %d t %.2f state %d\n", near, slow, player_speed(), shot_age, pl->aiming, arrest_t, pl->state);
#endif
    if (arrest_t > 1.5f) bust();
}

static void resist_tick(float dt)
{
    Ped *pl = &peds[PLAYER];
    resist_s += dt;
    if (pl->weapon == W_FIST) return;
    for (int i = 1; i < MAX_PEDS; i++) {
        if (!cop_alive(i)) continue;
        Ped *c = &peds[i];
        float d = Vector3Distance(c->pos, pl->pos);
        if (d > 45) continue;
        if (shot_age < 0.4f && cop[i].sees) { resist_s = 0; shout(i, S_V_SHOTS); continue; }
        if (!aim_live) continue;
        Vector3 to = Vector3Subtract(Vector3Add(c->pos, v3(0, 1.3f, 0)), aim_from);
        float t = Vector3DotProduct(to, aim_dir);
        if (t > 0 && Vector3Length(Vector3Subtract(to, Vector3Scale(aim_dir, t))) < 1.5f) resist_s = 0;
    }
}

static void sense(float dt)
{
    int pveh = peds[PLAYER].car;
    bool disguised = (heat_search > 0 || seen_s > LOST_S) && pveh != seen_vehicle;
    bool night = day_hours < 5.5f || day_hours > 20.5f;
    float range = disguised ? 20 : night ? 70 : 100;
    Vector3 pt = player_point();
    bool any = false;
    for (int i = 1; i < MAX_PEDS; i++) {
        if (!cop_alive(i)) continue;
        Cop *k = &cop[i];
        Ped *c = &peds[i];
        if (c->state == PS_RAGDOLL) { k->sees = false; continue; }
        if ((ticks + i) % 3 == 0) {
            bool s = false;
            Vector3 e = eye_of(i);
            float d = Vector3Distance(e, pt);
            if (d <= range) {
                Vector3 to = Vector3Subtract(pt, c->pos);
                bool in_cone = d < 7 || fabsf(angle_diff(c->yaw, atan2f(to.x, to.z))) < 1.3f;
                s = in_cone && los(e, pt);
            }
            k->sees = s;
        }
        any |= k->sees;
    }
    if (heli.on) {
        if (ticks % 3 == 0) {
            Vector3 eye = Vector3Add(heli.pos, v3(0, -2, 0));
            heli.sees = Vector3Distance(eye, pt) <= range * 1.5f && los(eye, pt);
        }
        any |= heli.sees;
    }
    if (any) {
        last_seen = pt;
        seen_s = 0;
        seen_vehicle = pveh;
        heat_search = 0;
    } else {
        seen_s += dt;
        if (seen_s < LOST_S) last_seen = pt;
    }
}

void cops_tick(float dt)
{
    spawn_cd -= dt;
    block_cd -= dt;
    witness_tick(dt);
#ifdef COPS_TEST
    if (getenv("OOP_COPLOG") && ticks % 60 == 0 && heat > 0) {
        fprintf(stderr, "t=%.0f heat %d search %.1f seen_s %.1f cops %d units %d pl %.0f %.0f sp %.1f car %d hp %.0f\n", game_s, heat, heat_search, seen_s, cop_count(), units(), player_pos().x, player_pos().z, player_speed(), peds[PLAYER].car, peds[PLAYER].health);
        { int nc = -1; float nd = 1e9f; for (int c = 0; c < MAX_CARS; c++) if (cars[c].used && c != peds[PLAYER].car && dist_xz(cars[c].pos, player_pos()) < nd) { nd = dist_xz(cars[c].pos, player_pos()); nc = c; }
          if (nc >= 0) fprintf(stderr, "  nearest car %d ai %d dist %.1f speed %.1f\n", nc, cars[nc].ai, nd, car_speed(&cars[nc])); }
        if (heli.on) fprintf(stderr, "  heli %.0f %.0f %.0f sees %d spot %.0f %.0f\n", heli.pos.x, heli.pos.y, heli.pos.z, heli.sees, heli.spot.x, heli.spot.z);
        for (int i = 1; i < MAX_PEDS; i++) if (cop_alive(i)) fprintf(stderr, "  cop %d state %d pos %.0f %.0f car %d dist %.0f sees %d\n", i, peds[i].state, peds[i].pos.x, peds[i].pos.z, peds[i].car, dist_xz(peds[i].pos, player_pos()), cop[i].sees);
    }
    if (getenv("OOP_GOD")) peds[PLAYER].health = 100;
    if (ticks == 60 && getenv("OOP_HEAT")) {
        heat = atoi(getenv("OOP_HEAT"));
        score = level_at[heat];
        last_seen = player_pos();
        seen_s = 0;
    }
#endif
    if (mode != MODE_PLAY) {
        for (int i = 1; i < MAX_PEDS; i++) if (cop_alive(i) && peds[i].state == PS_CHASE) { peds[i].aiming = false; foot_cop(i, dt); }
        return;
    }
    resist_tick(dt);
    heli_tick(dt);
    if (heat > 0) {
        sense(dt);
        if (seen_s > LOST_S && heat_search <= 0) {
            heat_search = 14 + 7 * heat;
            voice(S_V_LOST, player_pos(), 1, -1);
        }
        if (ticks % 600 == 300 && seen_s < LOST_S) voice(S_V_NORTH, player_pos(), 1, -1);
        if (was_in_car && peds[PLAYER].car < 0 && seen_s < LOST_S) {
            int who = cop_near(player_pos());
            if (who > 0) shout(who, S_V_ONFOOT);
        }
        was_in_car = peds[PLAYER].car >= 0;
        if (heat_search > 0) {
            bool near = false;
            for (int i = 1; i < MAX_PEDS && !near; i++) near = cop_alive(i) && dist_xz(peds[i].pos, last_seen) < 50;
            heat_search -= dt * (near ? 1 : 0.6f);
            if (heat_search <= 0) lose_them();
        }
        if (heat > 0 && nlanes > 0 && spawn_cd <= 0 && !no_cars() && units() < want_units[heat] && cop_count() < MAX_COP_PEDS) {
            if (spawn_cop_car()) spawn_cd = spawn_gap[heat];
            else spawn_cd = 0.5f;
        }
        if (heat >= 4 && block_cd <= 0) { spawn_roadblock(); block_cd = 28; }
        arrest_tick(dt);
    }

    Vector3 pp = player_pos();
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *c = &peds[i];
        if (!is_cop(i)) continue;
        Cop *k = &cop[i];
        c->fire_cd -= dt;
        k->shoot_cd -= dt;
        float d = dist_xz(c->pos, pp);
        if (c->state == PS_DEAD) {
            if (pv[i].dead_s > 90 || (pv[i].dead_s > 30 && !visible(c->pos))) ped_remove(i);
            continue;
        }
        if (c->state == PS_IN_CAR) {
            if (c->car >= 0 && heat > 0) drive_cop(i, dt);
            if (c->car >= 0 && d > 280 && !visible(c->pos)) car_remove(c->car);
            continue;
        }
        if (c->state == PS_RAGDOLL || c->state == PS_EXITING || c->state == PS_ENTERING) continue;
        if (heat == 0) continue;
        if (d > 220 && !visible(c->pos)) { ped_remove(i); continue; }
        if (c->state != PS_CHASE) { c->state = PS_CHASE; c->node = c->next_node = -1; }
        foot_cop(i, dt);
    }
    for (int ci = 0; ci < MAX_CARS; ci++) {
        if (!ccar[ci].mine) continue;
        Car *car = &cars[ci];
        if (!car->used || (car->ai != AI_COP && !ccar[ci].block)) { ccar[ci].mine = false; continue; }
        ccar[ci].age += dt;
        if (ccar[ci].block && !visible(car->pos) && (ccar[ci].age > 100 || dist_xz(car->pos, pp) > 230)) { car_remove(ci); continue; }
        if (heat == 0 && car->driver < 0 && !visible(car->pos) && dist_xz(car->pos, pp) > 50) car_remove(ci);
    }
}

void heat_clear(void)
{
    if (heat > 0) lose_them();
}

void cops_reset(void)
{
    for (int i = 1; i < MAX_PEDS; i++) if (is_cop(i)) ped_remove(i);
    for (int ci = 0; ci < MAX_CARS; ci++) {
        if (ccar[ci].mine && cars[ci].used) car_remove(ci);
        ccar[ci] = (CCar){ 0 };
    }
    for (int w = 0; w < MAX_WIT; w++) {
        if (wit[w].ped > 0 && peds[wit[w].ped].used && peds[wit[w].ped].state == PS_CALLING) peds[wit[w].ped].state = PS_WALK;
        wit[w].ped = 0;
    }
    heat = 0;
    score = 0;
    heat_search = 0;
    seen_s = 99;
    resist_s = 99;
    arrest_t = 0;
    spawn_cd = 0;
    spawn_fails = 0;
    block_cd = 0;
    seen_vehicle = -1;
    heli.on = heli.sees = false;
}

int cops_view(CopView *out, int max)
{
    int n = 0;
    for (int i = 1; i < MAX_PEDS && n < max; i++) {
        if (!cop_alive(i)) continue;
        out[n++] = (CopView){ peds[i].pos, peds[i].yaw, heat_search > 0 && !cop[i].sees, peds[i].state == PS_IN_CAR };
    }
    if (heli.on && n < max) out[n++] = (CopView){ heli.pos, heli.yaw, heat_search > 0 && !heli.sees, false };
    return n;
}
