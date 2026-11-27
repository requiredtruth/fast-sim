#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "combat.h"
#include "jobs.h"

#define NMISSION 5
enum { SL_START, SL_TARGET };

static const char *const title[NMISSION] = { "DAY THREE", "DALE'S WAGON", "SPIN CYCLE", "EXPORT QUALITY", "RAY RUNS" };
static const int pay[NMISSION] = { 300, 450, 600, 800, 1200 };

static int cur;                // missions done
static int step;               // 0 waiting at the lot, then 1.. per mission
static float t0;               // game_s when the step began
static int mcar = -1;
static Vector3 mspot;
static bool taken, went, was_hot;
static float far_s, pin_s;

static const char *queue[8];
static int qn, qi;
static float q_s;

#define SAY(...) say((const char *[]){ __VA_ARGS__ }, (int)(sizeof((const char *[]){ __VA_ARGS__ }) / sizeof(char *)))

static void say(const char *const *lines, int n)
{
    qn = n;
    qi = 0;
    q_s = 0;
    for (int i = 0; i < n && i < 8; i++) queue[i] = lines[i];
}

static float dxz(Vector3 a, Vector3 b) { return hypotf(a.x - b.x, a.z - b.z); }

bool mission_owns_car(int ci) { return step > 0 && ci == mcar; }

static bool in_mcar(void)
{
    return mcar >= 0 && cars[mcar].used && peds[PLAYER].car == mcar && cars[mcar].driver == PLAYER;
}

static void cleanup(void)
{
    if (mcar >= 0 && cars[mcar].used && cars[mcar].driver != PLAYER) car_remove(mcar);
    mcar = -1;
    taken = went = was_hot = false;
    marker_set(SL_TARGET, (Vector3){ 0 }, -1);
    hud_objective("");
}

static void fail(const char *why)
{
    hud_msg(why, 4.5f);
    hud_big("JOB BLOWN", 3);
    cleanup();
    step = 0;
    qn = qi = 0;
}

static void finish(const char *const *lines, int n)
{
    cash += pay[cur];
    char m[56];
    snprintf(m, sizeof m, "+$%d", pay[cur]);
    hud_msg(m, 3);
    hud_big("JOB DONE", 3);
    sfx(S_CASH, peds[PLAYER].pos, 0.8f, 1);
    cur++;
    step = 0;
    mcar = -1;     // the car stays where it is
    cleanup();
    say(lines, n);
}
#define FINISH(...) finish((const char *[]){ __VA_ARGS__ }, (int)(sizeof((const char *[]){ __VA_ARGS__ }) / sizeof(char *)))

static void spawn_mcar(int model, Color paint)
{
    float yaw;
    Vector3 p;
    if (!curb_spot(mspot, &p, &yaw)) return;
    mcar = car_spawn(model, p, yaw, AI_PARKED);
    if (mcar >= 0) cars[mcar].paint = paint;
}

// the car this mission is about: get in, drive it to dest. true on arrival
static bool car_step(int model, Color paint, Vector3 dest, float rad, const char *get_in, const char *go)
{
    if (mcar < 0 || !cars[mcar].used) {
        if (taken) { fail("The car is gone. So is Walt's patience."); return false; }
        if (dxz(peds[PLAYER].pos, mspot) < 220) spawn_mcar(model, paint);
    }
    if (mcar >= 0 && cars[mcar].used && (cars[mcar].health <= 0 || cars[mcar].ai == AI_WRECK)) {
        fail("You wrecked it. Walt wanted it in one piece.");
        return false;
    }
    bool used = mcar >= 0 && cars[mcar].used;
    if (!in_mcar()) {
        hud_objective(went ? "Get back in the car" : get_in);
        marker_set(SL_TARGET, used ? cars[mcar].pos : mspot, MK_TARGET);
        return false;
    }
    taken = true;
    hud_objective(go);
    marker_set(SL_TARGET, dest, MK_TARGET);
    return dxz(cars[mcar].pos, dest) < rad && fabsf(car_speed(&cars[mcar])) < 4;
}

static void begin(void)
{
    step = 1;
    t0 = game_s;
    taken = went = was_hot = false;
    far_s = pin_s = 0;
    hud_big(title[cur], 2.5f);
    Vector3 w = walt_pos();
    switch (cur) {
    case 0:
        mspot = w;
        SAY("Walt: Three days out and you came straight here.", "Walt: Good. The board thinks you work for me.",
            "Walt: Pickup's at the kerb. Take it to Harbor Row.", "Walt: Look busy. It's the whole job.");
        break;
    case 1:
        mspot = (Vector3){ -86, 0, 178 };
        SAY("Walt: Dale stopped paying me. His wagon didn't.", "Walt: Green, parked in Market Row. Never locked.",
            "Walt: Bring it back here before Dale notices.");
        break;
    case 2:
        SAY("Walt: The laundromat on Market Row owes me rent.", "Walt: It's a laundromat. It has cash.",
            "Walt: Be polite. Loud, but polite.");
        break;
    case 3:
        mspot = (Vector3){ 255, 0, 299 };
        SAY("Walt: A ship leaves the docks tonight.", "Walt: It needs one more SUV. A red one.",
            "Walt: Nobody's. Nobody will miss it.");
        break;
    default:
        SAY("Walt: Ray has my envelope and a blue hatchback.", "Walt: He's a coward, so he drives fast.",
            "Walt: Stop the car. Gently. Not that gently.");
        float yaw;
        Vector3 p;
        if (curb_spot((Vector3){ -328, 0, 30 }, &p, &yaw)) {
            int l = nearest_lane(p, 20);
            if (l >= 0) {
                Vector3 d = Vector3Normalize(Vector3Subtract(lanes[l].b, lanes[l].a));
                p = Vector3Add(lanes[l].a, Vector3Scale(d, fminf(lanes[l].len * 0.5f, 30)));
                yaw = atan2f(d.x, d.z);
            }
            mcar = car_spawn(CAR_HATCH, p, yaw, AI_FLEE);
            if (mcar >= 0) {
                cars[mcar].paint = (Color){ 40, 80, 190, 255 };
                cars[mcar].ai_timer = 40;
                int d = ped_spawn(PED_CIV, cars[mcar].pos, yaw);
                if (d >= 0) {
                    peds[d].car = mcar;
                    peds[d].seat = 0;
                    peds[d].state = PS_IN_CAR;
                    cars[mcar].driver = d;
                }
            }
        }
    }
}

static void run(void)
{
    Ped *pl = &peds[PLAYER];
    Vector3 w = walt_pos();
    switch (cur) {
    case 0:
        if (car_step(CAR_PICKUP, (Color){ 210, 205, 190, 255 }, (Vector3){ -434, 0, 299 }, 11, "Get in Walt's pickup", "Take the pickup to Harbor Row")) {
            FINISH("The dock man signs without reading. Concerning.", "Walt: Your half. The other half is interest.");
        }
        if (in_mcar() && !went) { went = true; hud_msg("It smells like air freshener and regret.", 3.5f); }
        break;
    case 1:
        if (car_step(CAR_WAGON, (Color){ 52, 110, 70, 255 }, w, 14, "Find Dale's green wagon in Market Row", "Take the wagon back to Walt's")) {
            FINISH("Walt: Dale owes me less now. Good.", "Walt: Don't tell me what was in the glovebox.");
        }
        if (in_mcar() && !went) { went = true; hud_msg("It smells like dog. Dale has a dog.", 3.5f); }
        break;
    case 2: {
        int s = 0;
        for (int i = 0; i < nshops; i++) if (shops[i].kind == 23) s = i;
        if (shops[s].robbed_at < t0) {
            hud_objective("Rob the laundromat. Point a gun at the clerk");
            marker_set(SL_TARGET, shops[s].door, MK_TARGET);
            break;
        }
        if (step == 1) { step = 2; t0 = game_s; hud_msg("Walt: Cops, I assume. Lose them.", 4); }
        marker_set(SL_TARGET, (Vector3){ 0 }, -1);
        hud_objective("Lose the cops");
        if (heat > 0) was_hot = true;
        if ((was_hot && heat == 0) || game_s - t0 > 45) {
            FINISH("Walt: Clean getaway? Good. The quarters are yours.", "Walt: Keep the quarters. I'll take the bills.");
        }
        break; }
    case 3:
        if (car_step(CAR_SUV, (Color){ 190, 30, 36, 255 }, export_pos(), 10, "Steal the red SUV in Sunset Terrace", "Take the SUV to the docks")) {
            FINISH("The crane operator doesn't look at the plates.", "Walt: Almost even. Almost is a good word.");
        }
        if (in_mcar() && !went) { went = true; hud_msg("Still has the new-car smell. Someone's.", 3.5f); }
        break;
    default:
        if (mcar < 0 || !cars[mcar].used) { fail("Ray got away. He'll be back."); break; }
        Car *c = &cars[mcar];
        c->ai_timer = 30;
        hud_objective("Stop Ray's blue hatchback");
        marker_set(SL_TARGET, c->pos, MK_TARGET);
        far_s = dxz(c->pos, pl->pos) > 260 ? far_s + DT : 0;
        if (far_s > 12) { fail("You lost Ray. He's thrilled."); break; }
        pin_s = fabsf(car_speed(c)) < 1.5f && dxz(c->pos, pl->pos) < 30 ? pin_s + DT : 0;
        if (c->health < 700 || pin_s > 3) {
            FINISH("Ray hands over the envelope through the window.", "Walt: We're square. Don't look so surprised.",
                   "Walt: Wash something. The officer drops by Friday.");
        }
    }
}

void missions_tick(const Input *in, float dt)
{
    if (nlanes <= 0 || nwalk <= 0) return;
    if (ticks == 2 && getenv("OOP_MISSION")) { cur = atoi(getenv("OOP_MISSION")); begin(); }
    if (qi < qn && (q_s -= dt) <= 0) { hud_msg(queue[qi++], 4.2f); q_s = 3.3f; }
    if (step > 0 && (mode == MODE_DEAD || mode == MODE_BUSTED)) fail(mode == MODE_DEAD ? "You died. Walt pretends he didn't see." : "Busted. Walt can't help with that.");
    if (mode != MODE_PLAY) return;
    if (cur >= NMISSION) { marker_set(SL_START, (Vector3){ 0 }, -1); return; }
    Ped *pl = &peds[PLAYER];
    if (step == 0) {
        Vector3 w = walt_pos();
        marker_set(SL_START, w, MK_MISSION);
        if (pl->car < 0 && dxz(pl->pos, w) < 2.5f) begin();
        return;
    }
    marker_set(SL_START, (Vector3){ 0 }, -1);
    run();
}
