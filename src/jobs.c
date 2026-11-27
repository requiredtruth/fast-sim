#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "combat.h"
#include "cops.h"
#include "jobs.h"

enum { SL_MISSION, SL_TARGET, SL_TARGET2, SL_SHOP, SL_RESPRAY, SL_EXPORT, SL_TAXI, SL_STASH = 15 };

static float dxz(Vector3 a, Vector3 b) { return hypotf(a.x - b.x, a.z - b.z); }

static bool driving(int *ci)
{
    Ped *p = &peds[PLAYER];
    if (p->car < 0 || !cars[p->car].used || cars[p->car].driver != PLAYER) return false;
    *ci = p->car;
    return true;
}

static Vector3 lane_mid(float x, float z)
{
    Vector3 q = { x, 0, z };
    int l = nearest_lane(q, 120);
    if (l < 0) return q;
    return (Vector3){ (lanes[l].a.x + lanes[l].b.x) / 2, 0, (lanes[l].a.z + lanes[l].b.z) / 2 };
}

static Vector3 walk_at(float x, float z)
{
    int n = nearest_walk((Vector3){ x, 0, z }, 120);
    return n < 0 ? (Vector3){ x, 0, z } : walk[n].p;
}

Vector3 export_pos(void) { return lane_mid(-202, 406); }
Vector3 walt_pos(void) { return walk_at(-328, -71); }
static Vector3 respray_pos(void) { return lane_mid(145, -65); }

bool curb_spot(Vector3 near, Vector3 *spot, float *yaw)
{
    int l = nearest_lane(near, 60);
    if (l < 0) return false;
    Lane *L = &lanes[l];
    Vector3 d = Vector3Normalize(Vector3Subtract(L->b, L->a));
    float t = Clamp((near.x - L->a.x) * d.x + (near.z - L->a.z) * d.z, 8, fmaxf(L->len - 8, 8));
    Vector3 p = Vector3Add(L->a, Vector3Scale(d, t));
    Vector3 r = { -d.z, 0, d.x };
    float e = 0;
    while (e < 8 && ground_y(p.x + r.x * e, p.z + r.z * e) < 0.05f) e += 0.25f;
    *spot = e >= 3.3f && e < 8 ? Vector3Add(p, Vector3Scale(r, e - 1.2f)) : p;
    *yaw = atan2f(d.x, d.z);
    return true;
}

// ---- the gun counter

static const struct { const char *name; int item, price; } stock[] = {
    { "Pistol", PK_PISTOL, 350 }, { "SMG", PK_SMG, 1200 }, { "Shotgun", PK_SHOTGUN, 900 },
    { "Ammo", PK_AMMO, 120 }, { "Body armor", PK_ARMOR, 400 },
};
#define NSTOCK 5
static int sel;
static Vector3 shop_spot;
static bool shop_ready;

int shop_view(int *s, const char **name, int *price, int max)
{
    if (!shop_open) return 0;
    *s = sel;
    for (int i = 0; i < NSTOCK && i < max; i++) { name[i] = stock[i].name; price[i] = stock[i].price; }
    return NSTOCK;
}

static void shop_init(void)
{
    shop_spot = (Vector3){ 262.5f, 0.1f, 73.6f };   // the pawn shop front, found by ray from a screenshot
    marker_set(SL_SHOP, shop_spot, MK_SHOP);
    shop_ready = true;
}

static void shop_tick(const Input *in)
{
    Ped *p = &peds[PLAYER];
    bool here = mode == MODE_PLAY && p->car < 0 && dxz(p->pos, shop_spot) < 4;
    if (here && !shop_open) { sel = 0; hud_msg("Pawn Kings. Guns are in the back.", 3); }
    shop_open = here;
    if (!here) return;
    if (in->hit & BTN_NEXT) sel = (sel + 1) % NSTOCK;
    if (in->hit & BTN_PREV) sel = (sel + NSTOCK - 1) % NSTOCK;
    if (!(in->hit & BTN_ENTER)) return;
    char m[56];
    if (cash < stock[sel].price) {
        snprintf(m, sizeof m, "$%d. You have $%d.", stock[sel].price, cash);
        hud_msg(m, 2.5f);
        sfx(S_EMPTY, p->pos, 0.6f, 1);
    } else if (give_item(stock[sel].item)) {
        cash -= stock[sel].price;
        snprintf(m, sizeof m, "%s. $%d.", stock[sel].name, stock[sel].price);
        hud_msg(m, 2.5f);
        sfx(S_CASH, p->pos, 0.6f, 0.8f);
    } else hud_msg(stock[sel].item == PK_ARMOR ? "You are wearing it." : "Nothing to put them in.", 2.5f);
}

// ---- respray

static float spray_cd;

static void respray_tick(float dt)
{
    spray_cd -= dt;
    int ci;
    Vector3 g = respray_pos();
    if (!driving(&ci) || dxz(cars[ci].pos, g) > 6 || spray_cd > 0) return;
    spray_cd = 4;
    char m[56];
    if (heat == 0) { spray_cd = 12; hud_msg("Paint is fine. Come back wanted.", 3); return; }
    CopView cv[40];
    int nc = cops_view(cv, 40);
    for (int i = 0; i < nc; i++) if (dxz(cv[i].pos, cars[ci].pos) < 25) { hud_msg("Cops are right behind you. Not here.", 3); return; }
    int fee = 100 + 100 * heat;
    if (cash < fee) { snprintf(m, sizeof m, "Respray is $%d. You have $%d.", fee, cash); hud_msg(m, 3); return; }
    cash -= fee;
    static const Color paints[] = { { 40, 70, 150, 255 }, { 190, 190, 195, 255 }, { 30, 30, 34, 255 }, { 200, 150, 40, 255 },
                                    { 60, 120, 80, 255 }, { 150, 40, 50, 255 } };
    cars[ci].paint = paints[rnd() % 6];
    snprintf(m, sizeof m, "New paint, $%d. Nobody saw anything.", fee);
    hud_msg(m, 3.5f);
    sfx(S_CASH, cars[ci].pos, 0.7f, 1);
    heat_clear();
}

// ---- car export

static const int export_pay[CAR_MODELS] = { 400, 300, 330, 450, 550, 700, 420, 800, 0, 0 };
static const char *const model_name[CAR_MODELS] = { "SEDAN", "COMPACT", "HATCH", "MINIVAN", "PICKUP", "SUV", "WAGON", "OFFROAD", "", "" };
static int want[3] = { -1, -1, -1 };
static int sold_car = -1;
static float near_msg_cd;

static void new_list(void)
{
    for (int i = 0; i < 3; i++) {
        int m;
        bool dup;
        do {
            m = (int)(rnd() % CAR_COP);
            dup = false;
            for (int k = 0; k < i; k++) dup |= want[k] == m;
        } while (dup);
        want[i] = m;
    }
}

static bool in_want(int model)
{
    for (int i = 0; i < 3; i++) if (want[i] == model) return true;
    return false;
}

static void export_tick(float dt)
{
    if (want[0] < 0) new_list();
    near_msg_cd -= dt;
    Vector3 g = export_pos();
    if (sold_car >= 0 && (peds[PLAYER].car != sold_car || !cars[sold_car].used)) {
        if (cars[sold_car].used) car_remove(sold_car);
        sold_car = -1;
    }
    int ci;
    if (!driving(&ci) || sold_car >= 0 || dxz(cars[ci].pos, g) > 7) return;
    if (near_msg_cd <= 0) {
        near_msg_cd = 6;
        char m[56];
        snprintf(m, sizeof m, "Wanted: %s, %s, %s", model_name[want[0]], model_name[want[1]], model_name[want[2]]);
        hud_msg(m, 4);
    }
    if (mission_owns_car(ci) || !in_want(cars[ci].model) || fabsf(car_speed(&cars[ci])) > 2.5f) return;
    int pay = (int)(export_pay[cars[ci].model] * (0.35f + 0.65f * Clamp(cars[ci].health / 1000, 0, 1)));
    char m[56];
    cash += pay;
    snprintf(m, sizeof m, "Exported. +$%d. Step out, crane's here.", pay);
    hud_msg(m, 4);
    sfx(S_CASH, cars[ci].pos, 0.8f, 1);
    sold_car = ci;
    new_list();
}

static void export_hint(void)
{
    Vector3 pp = peds[PLAYER].pos;
    if (dxz(pp, export_pos()) > 140 || want[0] < 0) return;
    char m[72];
    snprintf(m, sizeof m, "Docks want %s, %s, %s", model_name[want[0]], model_name[want[1]], model_name[want[2]]);
    hud_objective(m);
}

// ---- taxi fares

enum { F_NONE, F_WAIT, F_RIDE };
typedef struct { int state, ped; Vector3 from, to; float t, wave_cd, away; } Fare;
static Fare fare;
static float fare_cd = 3;

static void fare_reset(void)
{
    if (fare.state == F_WAIT && fare.ped >= 0) ped_remove(fare.ped);
    fare.state = F_NONE;
    marker_set(SL_TAXI, (Vector3){ 0 }, -1);
}

static void fare_find(Vector3 pp)
{
    for (int k = 0; k < 24; k++) {
        int n = (int)(rnd() % (uint32_t)nwalk);
        float d = dxz(walk[n].p, pp);
        if (d < 45 || d > 120 || nearest_lane(walk[n].p, 9) < 0) continue;
        int i = ped_spawn(PED_CIV, walk[n].p, 0);
        if (i < 0) return;
        peds[i].state = PS_IDLE;
        peds[i].timer = 99;
        peds[i].target = -1;
        fare = (Fare){ F_WAIT, i, walk[n].p, { 0 }, 0, 0, 0 };
        hud_msg("Someone is waving you down.", 3);
        return;
    }
}

static const char *const fare_lines[] = {
    "Drive. I'm late, and it's not my fault.", "Anywhere but here. Go.",
    "Don't talk. I had a day.", "Take the long way. I like cars.",
};

static void fare_tick(float dt)
{
    int ci;
    bool taxi = driving(&ci) && cars[ci].model == CAR_TAXI;
    if (!taxi || heat > 0 || mode != MODE_PLAY) {
        if (fare.state == F_RIDE && taxi && heat > 0) hud_msg("Fare: Cops? I'm walking.", 3);
        if (fare.state != F_NONE) fare.away += dt; else fare.away = 0;
        if (fare.state == F_RIDE && fare.away > 15) { hud_msg("Your fare gave up on you.", 3); fare_reset(); }
        else if (fare.state == F_WAIT && !taxi) fare_reset();
        if (fare.state == F_NONE) fare_cd = 4;
        return;
    }
    Vector3 pp = cars[ci].pos;
    fare.away = 0;
    if (fare.state == F_NONE) {
        if ((fare_cd -= dt) <= 0) fare_find(pp);
        return;
    }
    if (fare.state == F_WAIT) {
        Ped *w = &peds[fare.ped];
        if (!w->used || dxz(w->pos, pp) > 200) { fare_reset(); return; }
        marker_set(SL_TAXI, w->pos, MK_TARGET);
        hud_objective("Pick up your fare");
        w->state = PS_IDLE;
        w->timer = 99;
        w->vel = (Vector3){ 0 };
        w->yaw = atan2f(pp.x - w->pos.x, pp.z - w->pos.z);
        if ((fare.wave_cd -= dt) <= 0) { ped_play(fare.ped, A_INTERACT); fare.wave_cd = 2.2f; }
        if (dxz(w->pos, pp) < 7 && fabsf(car_speed(&cars[ci])) < 2) {
            for (int k = 0; k < 40; k++) {
                int n = (int)(rnd() % (uint32_t)nwalk);
                float d = dxz(walk[n].p, pp);
                if (d < 250 || d > 650 || nearest_lane(walk[n].p, 9) < 0) continue;
                fare.to = walk[n].p;
                break;
            }
            if (fare.to.x == 0 && fare.to.z == 0) return;
            ped_remove(fare.ped);
            fare.state = F_RIDE;
            fare.from = pp;
            fare.t = 0;
            hud_msg(fare_lines[rnd() % 4], 4);
        }
        return;
    }
    fare.t += dt;
    marker_set(SL_TAXI, fare.to, MK_TARGET);
    hud_objective("Take your fare to the marker");
    if (dxz(pp, fare.to) < 9 && fabsf(car_speed(&cars[ci])) < 3) {
        float dist = dxz(fare.from, fare.to);
        float par = dist / 11;
        int base = 15 + (int)(dist * 0.1f);
        int tip = (int)(Clamp((par - fare.t) / par, 0, 1) * dist * 0.06f);
        char m[56];
        cash += base + tip;
        snprintf(m, sizeof m, "Fare $%d, tip $%d.", base, tip);
        hud_msg(m, 3.5f);
        sfx(S_CASH, pp, 0.7f, 1);
        Vector3 right = Vector3Transform((Vector3){ 2.2f, 0, 0 }, QuaternionToMatrix(cars[ci].rot));
        int i = ped_spawn(PED_CIV, Vector3Add(pp, right), frand(0, 6));
        if (i >= 0) {
            int n = nearest_walk(fare.to, 60);
            peds[i].pos.y = ground_y(peds[i].pos.x, peds[i].pos.z);
            peds[i].state = PS_WALK;
            peds[i].next_node = n;
        }
        fare_reset();
        fare_cd = 8;
    }
}

// ---- stashes

#define NSTASH 20
static struct { Vector3 p; bool got; } stash[NSTASH];
static int nstash, stash_found;

static uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; return x ^ (x >> 16); }

static bool stash_try(float cx, float cz, float r, uint32_t seed, float lat_min)
{
    for (int a = 0; a < 400; a++) {
        uint32_t h = mix(seed * 7919 + a);
        int n = (int)(h % (uint32_t)nwalk);
        if (dxz(walk[n].p, (Vector3){ cx, 0, cz }) > r) continue;
        float ang = (mix(h) % 628) / 100.0f, lat = lat_min + (mix(h + 1) % 400) / 100.0f;
        Vector3 q = { walk[n].p.x + sinf(ang) * lat, 0, walk[n].p.z + cosf(ang) * lat };
        if (is_water(q.x, q.z) || ground_y(q.x, q.z) < 0 || nearest_lane(q, 3.5f) >= 0) continue;
        q.y = ground_y(q.x, q.z);
        Vector3 pushed = q;
        world_push(&pushed, 0.6f, 1.8f);
        if (dxz(pushed, q) > 0.05f) continue;
        Hit hit;
        Vector3 eye = Vector3Add(walk[n].p, (Vector3){ 0, 1, 0 }), to = Vector3Subtract(Vector3Add(q, (Vector3){ 0, 1, 0 }), eye);
        if (world_ray(eye, Vector3Normalize(to), Vector3Length(to), &hit)) continue;
        bool close = false;
        for (int k = 0; k < nstash; k++) close |= dxz(stash[k].p, q) < 30;
        if (close) continue;
        stash[nstash++].p = q;
        return true;
    }
    return false;
}

static void stash_init(void)
{
    // park, beach, docks, then the backs of blocks
    static const float spots[NSTASH][3] = {
        { -436, -321, 70 }, { -436, -321, 70 }, { -92, 252, 70 }, { -92, 252, 70 },
        { 420, -433, 80 }, { 420, -205, 80 }, { 420, 52, 80 }, { 420, 258, 80 },
        { -480, 440, 60 }, { -330, 440, 60 }, { -180, 440, 60 }, { -30, 440, 60 },
        { -321, -436, 70 }, { -207, -207, 70 }, { 23, -321, 70 }, { 138, -436, 70 },
        { 252, -92, 70 }, { -92, 138, 70 }, { -551, 23, 70 }, { 252, 252, 70 },
    };
    for (int i = 0; i < NSTASH; i++) {
        if (stash_try(spots[i][0], spots[i][1], spots[i][2], i + 1, i < 12 ? 2 : 5)) continue;
        stash_try(spots[i][0], spots[i][1], spots[i][2] * 2, i + 101, 2);
    }
}

static void stash_tick(void)
{
    Ped *p = &peds[PLAYER];
    marker_set(SL_STASH, (Vector3){ 0 }, -1);
    if (mode != MODE_PLAY || p->car >= 0) return;
    for (int i = 0; i < nstash; i++) {
        if (stash[i].got) continue;
        float d = dxz(p->pos, stash[i].p);
        if (d < 30) marker_set(SL_STASH, stash[i].p, MK_STASH);
        if (d > 1.8f) continue;
        stash[i].got = true;
        stash_found++;
        char m[56];
        snprintf(m, sizeof m, "Stash %d/%d", stash_found, NSTASH);
        hud_msg(m, 3);
        uint32_t h = mix(i * 31 + 7);
        if (i % 5 == 3) {
            static const int w[3] = { PK_PISTOL, PK_SMG, PK_SHOTGUN };
            static const char *const wn[3] = { "A pistol, wrapped in a sock.", "An SMG in a gym bag.", "A shotgun in a rolled carpet." };
            int k = (i / 5 + i) % 3;
            give_item(w[k]);
            hud_msg(wn[k], 3);
        } else if (i % 9 == 4) {
            give_item(PK_ARMOR);
            hud_msg("Body armor under a tarp.", 3);
        } else {
            int amt = 150 + (int)(h % 351);
            cash += amt;
            snprintf(m, sizeof m, "+$%d in a coffee can.", amt);
            hud_msg(m, 3);
        }
        sfx(S_PICKUP, p->pos, 0.8f, 1.2f);
        if (stash_found == nstash) { cash += 1000; hud_msg("That's all of them. +$1000.", 4); }
        break;
    }
}

// ---- the lot

void jobs_tick(const Input *in, float dt)
{
    if (nlanes <= 0 || nwalk <= 0 || nshops <= 0) return;
    static bool ready;
    if (!ready) {
        ready = true;
        shop_init();
        marker_set(SL_RESPRAY, respray_pos(), MK_RESPRAY);
        marker_set(SL_EXPORT, export_pos(), MK_EXPORT);
        stash_init();
    }
    hud_objective("");
    if (mode != MODE_PLAY) { shop_open = false; return; }
    shop_tick(in);
    respray_tick(dt);
    export_tick(dt);
    export_hint();
    fare_tick(dt);
    stash_tick();
}
