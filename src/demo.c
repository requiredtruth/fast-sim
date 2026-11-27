// The demo drives the same controls and uses the same inventory rules as the player.
#include "ped.h"
#include "city.h"
#include <math.h>
#include <stddef.h>

extern float cam_yaw;
static float road_until, wait_until, stuck, reverse_until;
static float walk_best, walk_deadline;
static int stops_left, walking_to, city_lane;
static bool returning, at_shop_door;
static Vector3 trail[128];
static int trail_n;
static const char *status;
static uint32_t tried[24];
static int ntried;

const char *demo_status(void) { return status ? status : "DEMO  /  GETTING READY"; }

void demo_reset(void)
{
    road_until = game_s + 7;
    wait_until = 0;
    stuck = reverse_until = 0;
    stops_left = 3;
    walking_to = city_lane = -1;
    returning = false;
    at_shop_door = false;
    trail_n = 0;
    ntried = 0;
    walk_best = 1e9f;
    walk_deadline = 0;
    status = "DEMO  /  PREPARING SUPPLIES";
}

static float distance(Vector3 a, Vector3 b) { return hypotf(a.x - b.x, a.z - b.z); }
static float angle_delta(float a, float b) { return remainderf(a - b, 2 * PI); }

static bool use_slot(int effect)
{
    for (int i = 0; i < PACK_SLOTS; i++) {
        if (pack[i].n > 0 && items[pack[i].item].effect == effect && item_use(pack[i].item)) return true;
    }
    int home = sv.home_car;
    if (home < 0 || !cars[home].used || distance(cars[home].pos, peds[PLAYER].pos) > 10) return false;
    for (int i = 0; i < CARGO_SLOTS; i++) {
        if (cargo[i].n <= 0 || items[cargo[i].item].effect != effect) continue;
        int item = cargo[i].item;
        if (cargo_move(false, i) && item_use(item)) return true;
    }
    return false;
}

static bool supplies(Ped *p, Car *car)
{
    if (game_s < wait_until || sleeping()) return true;
    if ((sv.water < 66 && use_slot(E_DRINK)) ||
        (sv.food < 65 && use_slot(E_EAT)) ||
        ((sv.bleed > .01f || p->health < 67) && (use_slot(E_BANDAGE) || use_slot(E_HEAL))) ||
        (sv.sick > 25 && use_slot(E_CURE))) {
        status = "DEMO  /  USING SUPPLIES";
    } else if (car && ((car->fuel < car->fuel_max * .42f && use_slot(E_FUEL)) ||
        (car->engine < 78 && use_slot(E_ENGINE)) ||
        (car->battery < 50 && use_slot(E_BATTERY)) ||
        (car->radiator < 60 && use_slot(E_RADIATOR)))) {
        status = "DEMO  /  REFUELING OR REPAIRING";
    } else if (!car && sv.warmth < 57 && !sv.camp_fire && pack_count(item_find("wood")) >= 2 &&
               item_use(item_find("wood"))) {
        status = "DEMO  /  LIGHTING A CAMPFIRE";
    } else if (sv.rest < 43) {
        if (!car && !sv.tent && pack_count(item_find("tent"))) item_use(item_find("tent"));
        survive_sleep(7);
        status = "DEMO  /  RESTING";
    } else {
        int selected = -1;
        for (int i = 0; i < nrecipes; i++) {
            if (recipe_can(i)) { selected = i; break; }
        }
        if (selected < 0 || !recipe_make(selected)) return false;
        status = "DEMO  /  CRAFTING";
    }
    wait_until = game_s + (car ? .65f : 1.35f);
    return true;
}

static int nearest_aligned_lane(const Car *car)
{
    Vector3 f = car_fwd(car);
    float score = 160;
    int best = -1;
    for (int i = 0; i < nlanes; i++) {
        Lane *l = &lanes[i];
        if (l->len < .5f || l->nnext <= 0) continue;
        Vector3 d = Vector3Normalize(Vector3Subtract(l->b, l->a));
        float along = Vector3DotProduct(d, Vector3Subtract(car->pos, l->a));
        Vector3 p = Vector3Add(l->a, Vector3Scale(d, Clamp(along, 0, l->len)));
        float cost = distance(car->pos, p) + (1 - Vector3DotProduct(f, d)) * 14;
        if (cost < score) { best = i; score = cost; }
    }
    return best;
}

static Vector3 city_aim(const Car *car)
{
    if (city_lane < 0 || city_lane >= nlanes || distance(car->pos, lanes[city_lane].b) > lanes[city_lane].len + 22)
        city_lane = nearest_aligned_lane(car);
    if (city_lane < 0) return Vector3Add(car->pos, Vector3Scale(car_fwd(car), 10));
    Lane *l = &lanes[city_lane];
    for (int hops = 0; hops < 5 && distance(car->pos, l->b) < 7 && l->nnext > 0; hops++) {
        int next = l->next[rnd() % (uint32_t)l->nnext];
        if (next < 0 || next >= nlanes || next == city_lane) break;
        city_lane = next;
        l = &lanes[city_lane];
    }
    return l->b;
}

static Vector3 highway_aim(const Car *car)
{
    Vector3 d;
    float s = road_s(car->pos);
    Vector3 aim = road_at(s + 16, &d);
    return Vector3Add(aim, (Vector3){ -d.z * 1.85f, 0, d.x * 1.85f });
}

static int nearby_supplies(Vector3 from, float max_range)
{
    float best = max_range;
    int chosen = -1;
    for (int i = 0; i < MAX_CONTS; i++) {
        Cont *c = &conts[i];
        if (!c->used || c->searched || c->placed || c->kind == CT_TRADER || c->kind == CT_BENCH) continue;
        bool used = false;
        for (int k = 0; k < ntried; k++) if (tried[k] == c->key) used = true;
        if (used) continue;
        float score = distance(c->p, from);
        if (world_kind == WORLD_CITY && c->kind != CT_BAG) score += 8;
        if (score < best) { best = score; chosen = i; }
    }
    return chosen;
}

static Input drive(Ped *p, Car *car)
{
    Input in = { 0 };
    bool servicing = supplies(p, car);
    if (car_kmh(car) < 2 && servicing) { in.held = BTN_JUMP; return in; }
    if (car->fuel < 7 && car_kmh(car) < 1) road_until = 0;
    bool stop = game_s >= road_until && nearby_supplies(car->pos, car->fuel < 8 ? 68 : 34) >= 0;
    if (stop) {
        if (fabsf(car_speed(car)) > 1.8f) { in.held = BTN_JUMP; return in; }
        in.hit = BTN_ENTER;
        status = "DEMO  /  STOPPING TO SCAVENGE";
        stops_left = 3;
        returning = false;
        walking_to = -1;
        trail_n = 0;
        walk_best = 1e9f;
        road_until = game_s + 36;
        return in;
    }
    if (!servicing) status = "DEMO  /  TRAVELING";
    Vector3 aim = world_kind == WORLD_CITY ? city_aim(car) : highway_aim(car);
    Vector3 d = Vector3Subtract(aim, car->pos), f = car_fwd(car);
    float err = angle_delta(atan2f(d.x, d.z), atan2f(f.x, f.z));
    float speed = fabsf(car_speed(car));
    float target_speed = (world_kind == WORLD_CITY ? 12 : 19) * (1 - fminf(.80f, fabsf(err) * .41f));
    if (speed < 1.2f) stuck += DT;
    else if (stuck > 0) stuck = fmaxf(0, stuck - DT * 2);
    if (stuck > 2.0f && reverse_until <= game_s) { reverse_until = game_s + 2.3f; stuck = 0; }
    if (reverse_until > game_s) {
        status = "DEMO  /  BACKING AWAY FROM AN OBSTACLE";
        in.my = -.65f;
        in.mx = err >= 0 ? .9f : -.9f;
        return in;
    }
    in.mx = Clamp(-err * 1.85f, -1, 1);
    in.my = Clamp((target_speed - speed) * .22f, -.8f, 1);
    return in;
}

static Input walk_to(Ped *p, Vector3 target)
{
    float yaw = atan2f(target.x - p->pos.x, target.z - p->pos.z);
    Vector3 eye = Vector3Add(p->pos, (Vector3){ 0, .75f, 0 });
    float obstruction = fminf(2.4f, distance(target, p->pos));
    Hit hit;
    static const float offsets[] = { 0, -0.65f, 0.65f, -1.25f, 1.25f, -1.9f, 1.9f };
    for (int i = 0; i < (int)(sizeof offsets / sizeof *offsets); i++) {
        float a = yaw + offsets[i];
        Vector3 dir = { sinf(a), 0, cosf(a) };
        if (world_ray(eye, dir, obstruction, &hit)) continue;
        float turn = angle_delta(a, cam_yaw);
        return (Input){ .mx = -sinf(turn)*.82f, .my = cosf(turn)*.82f, .lx = -turn*.12f };
    }
    return (Input){ .mx = .7f, .my = -.32f };
}

Input demo_control(void)
{
    Input in = { 0 };
    Ped *p = &peds[PLAYER];
    Car *car = p->car >= 0 && p->state == PS_IN_CAR ? &cars[p->car] : NULL;
    if (car) return drive(p, car);
    if (p->state == PS_ENTERING || p->state == PS_EXITING || p->state == PS_RAGDOLL) return in;
    if (search_ci >= 0) return in;
    if (supplies(p, NULL)) return in;

    if (!returning) {
        int ci = nearby_supplies(p->pos, 56);
        if (ci >= 0 && stops_left > 0) {
            if (walking_to != ci) {
                walking_to = ci;
                walk_best = 1e9f;
                walk_deadline = game_s + 10;
                at_shop_door = false;
            }
            Cont *c = &conts[ci];
            Vector3 target = c->p, door;
            if (world_kind == WORLD_CITY && !at_shop_door && city_shop_door(ci, &door)) {
                if (distance(p->pos, door) < 1.1f) { at_shop_door = true; walk_best = 1e9f; }
                else target = door;
            }
            if (world_kind == WORLD_CITY && trail_n < 128 &&
                (trail_n == 0 || distance(trail[trail_n - 1], p->pos) > 1.8f)) trail[trail_n++] = p->pos;
            float d = distance(p->pos, target);
            if (d < walk_best - .6f) { walk_best = d; walk_deadline = game_s + 9; }
            status = "DEMO  /  SEARCHING FOR SUPPLIES";
            if (distance(p->pos, c->p) < 1.7f || walk_deadline < game_s) {
                if (ntried == 24) ntried = 0;
                tried[ntried++] = c->key;
                stops_left--;
                if (distance(p->pos, c->p) < 1.7f) {
                    if (c->kind == CT_PUMP || c->kind == CT_WELL) survive_interact();
                    else survive_search(ci);
                }
                walking_to = -1;
                return in;
            }
            return walk_to(p, target);
        }
        returning = true;
    }
    int home = sv.home_car;
    if (home >= 0 && cars[home].used) {
        Car *c = &cars[home];
        status = "DEMO  /  RETURNING TO THE CAR";
        if (distance(p->pos, c->pos) < 3.0f) in.hit = BTN_ENTER;
        else if (trail_n > 0) {
            while (trail_n > 1 && distance(p->pos, trail[trail_n - 1]) < 1.5f) trail_n--;
            if (distance(p->pos, trail[trail_n - 1]) < 1.5f) trail_n = 0;
            in = walk_to(p, trail_n > 0 ? trail[trail_n - 1] : c->pos);
        } else in = walk_to(p, c->pos);
        return in;
    }
    int replacement = car_near(p->pos, 20);
    if (replacement >= 0 && cars[replacement].driver < 0 && car_claim(replacement)) return in;
    status = "DEMO  /  TRAVELING ON FOOT";
    return walk_to(p, road_at(road_s(p->pos) + 22, NULL));
}
