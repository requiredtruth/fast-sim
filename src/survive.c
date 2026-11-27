#include "estate.h"
// staying alive: the pack and the trunk, food water sleep and warmth, searching, using things,
// crafting, trading, camping, fishing, and saving the trip.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ped.h"
#include "city.h"

Slot pack[PACK_SLOTS], cargo[CARGO_SLOTS];
Survivor sv;
int trade_with = -1;
float search_t; int search_ci = -1;
Slot loot_slots[8]; int loot_ci = -1;
Slot trade_stock[12];
int car_view = -1;
float sleep_fade;

// container contents, rolled once when first opened
static Slot cont_items[MAX_CONTS][8];
static uint32_t cont_rolled[MAX_CONTS];   // key the contents were rolled for, 0 = not yet
#define DROP_BASE 0xF0000000u
#define MAX_GROUND_DROPS 96
static uint32_t drop_key = DROP_BASE;
static int ground_drop(Vector3 p, int kind);
static int place_slot = -1;
static float place_yaw;

static float msg_cd, save_cd = 60, fish_t = -1, act_t = -1;
static int act_kind, act_car;
static bool camp_tent;

#define HOURS (1.0f / 60.0f)   // game hours per real second

static float dist_xz(Vector3 a, Vector3 b) { return hypotf(a.x - b.x, a.z - b.z); }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static uint32_t hsh(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }

float km_now(void) { return (float)sv.km; }

// ---------------------------------------------------------------- the pack

int pack_count(int it)
{
    int n = 0;
    for (int i = 0; i < PACK_SLOTS; i++) if (pack[i].n > 0 && pack[i].item == it) n += pack[i].n;
    return n;
}

static int inv_count(Slot *inv, int ns, int it)
{
    int n = 0;
    for (int i = 0; i < ns; i++) if (inv[i].n > 0 && inv[i].item == it) n += inv[i].n;
    return n;
}

int pack_add(Slot *inv, int ns, int it, int n)
{
    if (it < 0 || it >= nitems || n <= 0) return n;
    int st = items[it].stack;
    for (int i = 0; i < ns && n > 0; i++)
        if (inv[i].n > 0 && inv[i].item == it && inv[i].n < st) { int k = st - inv[i].n < n ? st - inv[i].n : n; inv[i].n += k; n -= k; }
    for (int i = 0; i < ns && n > 0; i++)
        if (inv[i].n <= 0) { int k = st < n ? st : n; inv[i].item = it; inv[i].n = k; n -= k; }
    return n;
}

bool pack_take(Slot *inv, int ns, int it, int n)
{
    if (inv_count(inv, ns, it) < n) return false;
    for (int i = ns - 1; i >= 0 && n > 0; i--)
        if (inv[i].n > 0 && inv[i].item == it) { int k = inv[i].n < n ? inv[i].n : n; inv[i].n -= k; n -= k; }
    return true;
}

float pack_weight(void)
{
    float w = 0;
    for (int i = 0; i < PACK_SLOTS; i++) if (pack[i].n > 0) w += items[pack[i].item].weight * pack[i].n;
    return w;
}

static int find_id(const char *id) { return item_find(id); }

float pack_capacity(void) { return 24 + (pack_count(find_id("backpack")) > 0 ? 15 : 0); }

int cur_weapon_ammo_item(int w)
{
    for (int i = 0; i < nitems; i++) if (items[i].kind == IK_AMMO && items[i].weapon == w) return i;
    return -1;
}

static void sync_weapons(void)
{
    bool have[W_COUNT] = { 0 };
    for (int i = 0; i < PACK_SLOTS; i++)
        if (pack[i].n > 0 && items[pack[i].item].effect == E_WEAPON && items[pack[i].item].weapon > 0) have[items[pack[i].item].weapon] = true;
    for (int w = 1; w < W_COUNT; w++) { if (have[w] && !weapon_owned(w)) weapon_give(w); else if (!have[w] && weapon_owned(w)) weapon_take(w); }
}

static void say(const char *fmt, const char *a)
{
    char b[96];
    snprintf(b, sizeof b, fmt, a);
    hud_msg(b, 2.5f);
}

// gives n of an item: into the pack, then the trunk if the car is close, else left on the ground
static void give(int it, int n)
{
    if (it < 0 || n <= 0) return;
    int left = pack_add(pack, PACK_SLOTS, it, n);
    if (left > 0 && sv.home_car >= 0 && cars[sv.home_car].used && dist_xz(cars[sv.home_car].pos, peds[PLAYER].pos) < 12)
        left = pack_add(cargo, CARGO_SLOTS, it, left);
    if (left > 0) {
        int ci = ground_drop(peds[PLAYER].pos, CT_BAG);
        if (ci >= 0) {
            left = pack_add(cont_items[ci], 8, it, left);
            hud_msg(left ? "Some supplies would not fit" : "Pack full: left it on the ground", 2.5f);
        } else hud_msg("No room for more supplies here", 2.5f);
    }
}

// ---------------------------------------------------------------- the car to work on

static int car_target(void)
{
    Ped *p = &peds[PLAYER];
    if (p->car >= 0 && cars[p->car].used) return p->car;
    if (car_view >= 0 && cars[car_view].used && dist_xz(cars[car_view].pos, p->pos) < 6) return car_view;
    if (sv.home_car >= 0 && cars[sv.home_car].used && dist_xz(cars[sv.home_car].pos, p->pos) < 6) return sv.home_car;
    int ci = car_near(p->pos, 5);
    return ci;
}

static bool need_tool(const char *id, const char *what)
{
    int t = find_id(id);
    if (t >= 0 && pack_count(t) > 0) return true;
    char b[80];
    snprintf(b, sizeof b, "Need a %s to %s", t >= 0 ? items[t].name : id, what);
    hud_msg(b, 2.5f);
    return false;
}

// ---------------------------------------------------------------- using things

static bool near_fire(void) { return sv.camp_fire > 0 && dist_xz(sv.camp_p, peds[PLAYER].pos) < 5; }

bool item_use(int it)
{
    if (it < 0 || it >= nitems || pack_count(it) <= 0) return false;
    if (wardrobe_item(it)) return wardrobe_equip(it);
    Item *t = &items[it];
    Ped *p = &peds[PLAYER];
    bool used = true, consume = true;
    switch (t->effect) {
    case E_EAT:
        sv.food = clampf(sv.food + t->value, 0, 100);
        sv.water = clampf(sv.water + t->value2, 0, 100);
        sv.rest = clampf(sv.rest + item_extra_rest(it), 0, 100);
        if (t->sick > 0 && frand(0, 1) < t->sick) { sv.sick = clampf(sv.sick + 35, 0, 100); hud_msg("Your stomach turns...", 3); }
        sfx(S_PICKUP, p->pos, 0.5f, 0.7f);
        break;
    case E_DRINK:
        sv.water = clampf(sv.water + t->value, 0, 100);
        sv.food = clampf(sv.food + t->value2, 0, 100);
        sv.rest = clampf(sv.rest + item_extra_rest(it), 0, 100);
        sv.warmth = clampf(sv.warmth + item_extra_warm(it), 0, 100);
        if (t->sick > 0 && frand(0, 1) < t->sick) { sv.sick = clampf(sv.sick + 40, 0, 100); hud_msg("That water was bad...", 3); }
        sfx(S_PICKUP, p->pos, 0.5f, 0.6f);
        break;
    case E_HEAL:
        if (p->health >= 100 && sv.bleed <= 0) { hud_msg("You're not hurt", 1.5f); return false; }
        p->health = fminf(100, p->health + t->value);
        sv.rest = clampf(sv.rest + item_extra_rest(it), 0, 100);
        sv.bleed = 0;
        break;
    case E_BANDAGE:
        if (p->health >= 100 && sv.bleed <= 0) { hud_msg("You're not hurt", 1.5f); return false; }
        p->health = fminf(100, p->health + t->value);
        sv.bleed = 0;
        hud_msg("Bleeding stopped", 2);
        break;
    case E_REST:
        sv.rest = clampf(sv.rest + t->value, 0, 100);
        sv.water = clampf(sv.water + t->value2, 0, 100);
        break;
    case E_WARM:
        sv.warmth = clampf(sv.warmth + t->value, 0, 100);
        sv.water = clampf(sv.water + t->value2, 0, 100);
        sv.rest = clampf(sv.rest + item_extra_rest(it), 0, 100);
        break;
    case E_CURE:
        if (sv.sick <= 0 && p->health >= 100) { hud_msg("You feel fine already", 1.5f); return false; }
        sv.sick = clampf(sv.sick - t->value, 0, 100);
        p->health = fminf(100, p->health + item_extra_heal(it));
        break;
    case E_FUEL: {
        int ci = car_target();
        if (ci < 0) { hud_msg("Stand next to a car to pour fuel in", 2.5f); return false; }
        Car *c = &cars[ci];
        if (c->fuel >= c->fuel_max - 0.5f) { hud_msg("The tank is full", 2); return false; }
        c->fuel = fminf(c->fuel_max, c->fuel + t->value);
        say("Poured fuel into the %s", car_name(c->model));
        sfx(S_PICKUP, c->pos, 0.6f, 0.5f);
        break;
    }
    case E_ENGINE: case E_TIRE: case E_BATTERY: case E_RADIATOR: case E_ARMOR: {
        int ci = car_target();
        if (ci < 0) { hud_msg("Stand next to a car to fit that", 2.5f); return false; }
        Car *c = &cars[ci];
        if (c->ai == AI_WRECK) { hud_msg("That car is burnt out", 2); return false; }
        if (t->effect == E_ENGINE) {
            if (t->value >= 30 && !need_tool("toolkit", "work on the engine")) return false;
            if (c->engine >= 99) { hud_msg("The engine is fine", 2); return false; }
            c->engine = fminf(100, c->engine + t->value);
        } else if (t->effect == E_TIRE) {
            int worst = 0;
            for (int w = 1; w < 4; w++) if (c->tire[w] < c->tire[worst]) worst = w;
            if (c->tire[worst] >= 98) { hud_msg("The tyres are fine", 2); return false; }
            if (t->value >= 90 && !need_tool("jack", "change a tyre")) return false;
            if (t->value < 90 && c->tire[worst] <= 0) c->tire[worst] = t->value * 0.6f;
            else c->tire[worst] = fminf(100, t->value >= 90 ? 100 : c->tire[worst] + t->value);
        } else if (t->effect == E_BATTERY) {
            if (c->battery >= 95) { hud_msg("The battery is fine", 2); return false; }
            c->battery = 100;
        } else if (t->effect == E_RADIATOR) {
            if (c->radiator >= 99) { hud_msg("The radiator is full", 2); return false; }
            c->radiator = fminf(100, c->radiator + t->value);
            c->heat = fminf(c->heat, 0.6f);
        } else {
            if (c->armor) { hud_msg("Already armoured", 2); return false; }
            if (!need_tool("toolkit", "bolt on armour")) return false;
            c->armor = true;
            c->health = fminf(1500, c->health + 500);
        }
        say("Fixed up the %s", car_name(c->model));
        sfx(S_RELOAD, c->pos, 0.7f, 0.6f);
        break;
    }
    case E_LIGHT:
        if (t->value <= 1) {   // the flashlight itself: a switch
            sv.torch_on = !sv.torch_on;
            if (sv.torch_on && sv.flashlight <= 0) { hud_msg("The flashlight is dead. Find batteries", 2.5f); sv.torch_on = false; }
            consume = false;
        } else {
            if (pack_count(find_id("flashlight")) <= 0) { hud_msg("You need a flashlight for these", 2); return false; }
            sv.flashlight = fminf(1200, sv.flashlight + t->value);
            hud_msg("Flashlight recharged", 2);
        }
        sfx(S_UI, p->pos, 0.4f, 1.4f);
        break;
    case E_TENT:
        if (p->car >= 0) { hud_msg("Get out of the car first", 2); return false; }
        if (sv.tent) { hud_msg("Your tent is already up", 2); return false; }
        sv.tent = true;
        sv.camp_on = true;
        sv.camp_p = Vector3Add(p->pos, (Vector3){ sinf(p->yaw) * 2.2f, 0, cosf(p->yaw) * 2.2f });
        sv.camp_p.y = ground_y(sv.camp_p.x, sv.camp_p.z);
        camp_tent = true;
        hud_msg("Camp pitched. Use it to sleep", 2.5f);
        break;
    case E_WEAPON:
        hud_msg("Weapons are ready to draw. Swap with Q or the mouse wheel", 2.5f);
        return false;
    default:
        if (t->id[0] && !strcmp(t->id, "wood")) {
            if (p->car >= 0) { hud_msg("Get out of the car first", 2); return false; }
            int lighter = find_id("lighter"), matches = find_id("matches");
            if (pack_count(it) < 2) { hud_msg("Need two pieces of firewood", 2); return false; }
            bool can = (lighter >= 0 && pack_count(lighter) > 0) || (matches >= 0 && pack_count(matches) > 0);
            if (!can) { hud_msg("Need a lighter or matches", 2); return false; }
            if (!(lighter >= 0 && pack_count(lighter) > 0)) pack_take(pack, PACK_SLOTS, matches, 1);
            pack_take(pack, PACK_SLOTS, it, 2);
            sv.camp_fire = 360;
            if (!sv.camp_on || dist_xz(sv.camp_p, p->pos) > 6) {
                sv.camp_on = true;
                sv.camp_p = Vector3Add(p->pos, (Vector3){ sinf(p->yaw) * 1.5f, 0, cosf(p->yaw) * 1.5f });
                sv.camp_p.y = ground_y(sv.camp_p.x, sv.camp_p.z);
            }
            hud_msg("The fire catches. Cook and boil water here", 3);
            return true;
        }
        if (!strcmp(t->id, "rod")) { survive_interact(); return true; }
        hud_msg("You can't use that on its own", 2);
        return false;
    }
    if (used && consume) {
        pack_take(pack, PACK_SLOTS, it, 1);
        int e = item_empty(it);
        if (e >= 0) give(e, 1);
    }
    return used;
}

// ---------------------------------------------------------------- crafting

bool recipe_can(int r)
{
    if (r < 0 || r >= nrecipes) return false;
    Recipe *rc = &recipes[r];
    for (int i = 0; i < rc->nin; i++) if (pack_count(rc->in[i]) < rc->in_n[i]) return false;
    if (rc->tool >= 0 && pack_count(rc->tool) <= 0) return false;
    if (rc->fire && !near_fire()) return false;
    return true;
}

bool recipe_make(int r)
{
    if (!recipe_can(r)) {
        if (r >= 0 && r < nrecipes && recipes[r].fire && !near_fire()) hud_msg("Needs a campfire nearby", 2);
        return false;
    }
    Recipe *rc = &recipes[r];
    for (int i = 0; i < rc->nin; i++) pack_take(pack, PACK_SLOTS, rc->in[i], rc->in_n[i]);
    give(rc->out, rc->out_n);
    sv.crafted++;
    say("Made %s", items[rc->out].name);
    sfx(S_PICKUP, peds[PLAYER].pos, 0.6f, 1.1f);
    return true;
}

// ---------------------------------------------------------------- loot

static int roll_count(int kind)
{
    switch (kind) {
    case CT_FRIDGE: return 1 + (int)frand(0, 2.6f);
    case CT_SHELF: return 1 + (int)frand(0, 3.4f);
    case CT_CRATE: return 2 + (int)frand(0, 2.8f);
    case CT_LOCKER: return 1 + (int)frand(0, 2.6f);
    case CT_TRUNK: return (int)frand(0.6f, 3.6f);
    case CT_DESK: return 1 + (int)frand(0, 2.2f);
    case CT_TOOLBOX: return 1 + (int)frand(0, 3);
    case CT_BAG: return 1 + (int)frand(0, 2.6f);
    case CT_CORPSE: return 1 + (int)frand(0, 3);
    case CT_BED: return (int)frand(0, 1.8f);
    default: return 0;
    }
}

static void roll_loot(int ci)
{
    Cont *c = &conts[ci];
    memset(cont_items[ci], 0, sizeof cont_items[ci]);
    cont_rolled[ci] = c->key ? c->key : 1;
    int n = (int)roundf(roll_count(c->kind) * tune_loot);
    if (c->kind == CT_CORPSE && c->key >= DROP_BASE) n += 1;
    float total = 0;
    for (int i = 0; i < nitems; i++) total += items[i].loot[c->kind];
    if (total <= 0) return;
    for (int k = 0; k < n; k++) {
        float r = frand(0, total);
        int it = 0;
        for (; it < nitems - 1; it++) { r -= items[it].loot[c->kind]; if (r <= 0) break; }
        if (items[it].loot[c->kind] <= 0) continue;
        int cnt = 1;
        if (items[it].kind == IK_AMMO) cnt = 3 + (int)frand(0, items[it].stack > 30 ? 10 : 6);
        else if (!strcmp(items[it].id, "cash")) cnt = 5 + (int)frand(0, 40);
        else if (items[it].stack >= 10 && frand(0, 1) < 0.4f) cnt = 2;
        pack_add(cont_items[ci], 8, it, cnt);
    }
}

static int ground_drop(Vector3 p, int kind)
{
    if (kind == CT_BAG) {
        int count = 0;
        for (int i = 0; i < MAX_CONTS; i++)
            if (conts[i].used && conts[i].key >= DROP_BASE && conts[i].kind == CT_BAG) count++;
        if (count >= MAX_GROUND_DROPS) return -1;
    }
    for (int i = 0; i < MAX_CONTS; i++) {
        if (conts[i].used) continue;
        Cont *c = &conts[i];
        memset(c, 0, sizeof *c);
        c->used = true; c->kind = kind; c->p = p;
        c->p.y = fmaxf(p.y, ground_y(p.x, p.z));
        c->stop = -1; c->car = -1;
        c->key = ++drop_key;
        memset(cont_items[i], 0, sizeof cont_items[i]);
        if (kind == CT_CORPSE) cont_rolled[i] = 0;
        else {
            c->searched = true;
            cont_rolled[i] = c->key;
        }
        return i;
    }
    return -1;
}

void cont_drop(Vector3 p, int kind) { ground_drop(p, kind); }

static uint32_t exhausted_key(uint32_t key) { return (key & ~0xF00u) | ((key & 0xF00u) + 0x800u); }

static void finish_loot(void)
{
    Cont *c = &conts[loot_ci];
    for (int i = 0; i < 8; i++) if (loot_slots[i].n > 0) return;
    if (c->key < DROP_BASE) {
        if (c->key) world_set_flag(exhausted_key(c->key));
    } else if (c->kind == CT_BAG) {
        c->used = false;
        loot_ci = -1;
        ui_screen = UI_NONE;
    }
}

static void loot_open(int ci)
{
    Cont *c = &conts[ci];
    if (c->key && c->key < DROP_BASE && world_flag(exhausted_key(c->key))) {
        memset(cont_items[ci], 0, sizeof cont_items[ci]);
        cont_rolled[ci] = c->key;
    }
    if (cont_rolled[ci] != (c->key ? c->key : 1)) roll_loot(ci);
    for (int i = 0; i < 8; i++) loot_slots[i] = cont_items[ci][i];
    loot_ci = ci;
    ui_screen = UI_LOOT;
    if (!c->searched) {
        c->searched = true;
        if (c->key && c->key < DROP_BASE) world_set_flag(c->key);
    }
    finish_loot();
}

static void loot_sync(void)
{
    if (loot_ci < 0) return;
    for (int i = 0; i < 8; i++) cont_items[loot_ci][i] = loot_slots[i];
}

bool loot_take(int slot)
{
    if (loot_ci < 0 || slot < 0 || slot >= 8 || loot_slots[slot].n <= 0) return false;
    Slot s = loot_slots[slot];
    int left = pack_add(pack, PACK_SLOTS, s.item, s.n);
    if (left > 0 && sv.home_car >= 0 && cars[sv.home_car].used && dist_xz(cars[sv.home_car].pos, peds[PLAYER].pos) < 12)
        left = pack_add(cargo, CARGO_SLOTS, s.item, left);
    if (left == s.n) { hud_msg("No room in the pack", 2); return false; }
    loot_slots[slot].n = left;
    loot_sync();
    finish_loot();
    sfx(S_PICKUP, peds[PLAYER].pos, 0.6f, frand(0.95f, 1.1f));
    return true;
}

void loot_take_all(void)
{
    for (int i = 0; i < 8; i++) if (loot_slots[i].n > 0) loot_take(i);
}

bool pack_drop(int slot)
{
    if (slot < 0 || slot >= PACK_SLOTS || pack[slot].n <= 0) return false;
    Slot s = pack[slot];
    // add to a bag at our feet, or make one
    Vector3 p = peds[PLAYER].pos;
    int left = s.n;
    for (int i = 0; i < MAX_CONTS && left > 0; i++) {
        Cont *c = &conts[i];
        if (c->used && c->key >= DROP_BASE && c->kind == CT_BAG && !c->placed &&
            dist_xz(c->p, p) < 1.4f && fabsf(c->p.y - p.y) < 0.75f)
            left = pack_add(cont_items[i], 8, s.item, left);
    }
    if (left > 0) {
        int bag = ground_drop(p, CT_BAG);
        if (bag >= 0) left = pack_add(cont_items[bag], 8, s.item, left);
        else hud_msg("No space for another ground cache", 2);
    }
    pack[slot].n = left;
    sfx(S_DOOR, peds[PLAYER].pos, 0.3f, 1.5f);
    return left < s.n;
}

bool placement_active(void) { return place_slot >= 0; }
void placement_cancel(void) { place_slot = -1; }
void placement_rotate(void) { place_yaw = fmodf(place_yaw + PI / 6, 2 * PI); }

void placement_begin(int slot)
{
    Ped *p = &peds[PLAYER];
    if (p->car >= 0 || slot < 0 || slot >= PACK_SLOTS || pack[slot].n <= 0) return;
    place_slot = slot;
    place_yaw = p->yaw;
    ui_screen = UI_NONE;
}

static bool placement_target(Vector3 *out)
{
    Ped *p = &peds[PLAYER];
    if (!placement_active() || p->car >= 0) return false;
    Vector3 dir = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    Hit hit;
    if (!world_ray(cam.position, dir, 15, &hit) || hit.n.y < .62f ||
        dist_xz(hit.p, p->pos) > 3.6f || fabsf(hit.p.y - p->pos.y) > 1.7f ||
        is_water(hit.p.x, hit.p.z)) return false;
    for (int ci = 0; ci < MAX_CONTS; ci++) {
        Cont *c = &conts[ci];
        if (c->used && c->placed && Vector3Distance(c->p, hit.p) < .35f) return false;
    }
    *out = Vector3Add(hit.p, (Vector3){ 0, .025f, 0 });
    return true;
}

static void placement_confirm(void)
{
    Slot *slot = &pack[place_slot];
    Vector3 p;
    if (slot->n <= 0) { placement_cancel(); return; }
    if (!placement_target(&p)) { hud_msg("Aim at dry ground or a flat surface in reach", 2); return; }
    if (estate_piece(slot->item)) {
        if (estate_place(slot->item,p,place_yaw)) { slot->n--; placement_cancel(); }
        else hud_msg("Part will not fit here",2);
        return;
    }
    int ci = ground_drop(p, CT_BAG);
    if (ci < 0) { hud_msg("No space for another placed item", 2); return; }
    Cont *c = &conts[ci];
    c->placed = true;
    c->yaw = place_yaw;
    cont_items[ci][0] = (Slot){ .item = slot->item, .n = 1 };
    slot->n--;
    placement_cancel();
    sfx(S_PICKUP, p, .6f, .85f);
}

// ---------------------------------------------------------------- trunk and other cars

bool cargo_move(bool to_cargo, int slot)
{
    Slot *from = to_cargo ? pack : cargo, *to = to_cargo ? cargo : pack;
    int nf = to_cargo ? PACK_SLOTS : CARGO_SLOTS, nt = to_cargo ? CARGO_SLOTS : PACK_SLOTS;
    if (slot < 0 || slot >= nf || from[slot].n <= 0) return false;
    int left = pack_add(to, nt, from[slot].item, from[slot].n);
    if (left == from[slot].n) { hud_msg(to_cargo ? "The trunk is full" : "No room in the pack", 2); return false; }
    from[slot].n = left;
    sfx(S_DOOR, peds[PLAYER].pos, 0.3f, 1.2f);
    return true;
}

bool car_siphon(int ci)
{
    if (ci < 0 || !cars[ci].used) return false;
    Car *c = &cars[ci];
    if (c->fuel < 0.5f) { hud_msg("The tank is dry", 2); return false; }
    if (!need_tool("siphon", "siphon fuel")) return false;
    // straight into our own car if it's close, else into empty jerry cans
    int home = sv.home_car;
    float got = 0;
    if (home >= 0 && home != ci && cars[home].used && dist_xz(cars[home].pos, c->pos) < 12) {
        float room = cars[home].fuel_max - cars[home].fuel;
        got = fminf(room, c->fuel);
        cars[home].fuel += got; c->fuel -= got;
    }
    int empty = find_id("gas_empty"), full = find_id("gas_can");
    while (c->fuel >= 4 && pack_count(empty) > 0) {
        float take = fminf(12, c->fuel);
        pack_take(pack, PACK_SLOTS, empty, 1);
        give(full, 1);
        c->fuel -= take; got += take;
    }
    if (got <= 0) { hud_msg("Nothing to put it in: bring empty jerry cans or park your car close", 3); return false; }
    char b[64];
    snprintf(b, sizeof b, "Siphoned %.0f litres", got);
    hud_msg(b, 2.5f);
    if (c->key) world_set_flag(world_key(c->key >> 12, 3, c->key & 255));
    sfx(S_PICKUP, c->pos, 0.5f, 0.5f);
    return true;
}

bool car_strip(int ci)
{
    if (ci < 0 || !cars[ci].used || ci == sv.home_car) return false;
    Car *c = &cars[ci];
    if (!need_tool("toolkit", "strip parts")) return false;
    int got = 0;
    if (c->battery > 30) { give(find_id("battery"), 1); c->battery = 0; got++; }
    int best = 0;
    for (int w = 1; w < 4; w++) if (c->tire[w] > c->tire[best]) best = w;
    if (c->tire[best] > 50 && pack_count(find_id("jack")) > 0) { give(find_id("tire"), 1); c->tire[best] = 0; got++; }
    if (c->engine > 45) { give(find_id("engine_parts"), 1); c->engine = 5; got++; }
    if (c->radiator > 40) { give(find_id("coolant"), 1); c->radiator = 0; got++; }
    give(find_id("scrap"), 2 + (int)frand(0, 3)); got++;
    if (c->key) world_set_flag(world_key(c->key >> 12, 5, c->key & 255));
    c->engine = fminf(c->engine, 5);
    hud_msg(got > 1 ? "Stripped what was worth taking" : "Only scrap left on this one", 2.5f);
    sfx(S_RELOAD, c->pos, 0.8f, 0.5f);
    return true;
}

bool car_claim(int ci)
{
    if (ci < 0 || !cars[ci].used || ci == sv.home_car || cars[ci].ai == AI_WRECK) return false;
    Car *c = &cars[ci];
    if (c->driver >= 0 && c->driver != PLAYER) return false;
    if (sv.home_car >= 0 && cars[sv.home_car].used) cars[sv.home_car].owned = false;
    sv.home_car = ci;
    c->owned = true;
    if (c->key) { world_set_flag(world_key(c->key >> 12, 4, c->key & 255)); c->key = 0; }
    c->ai = AI_NONE;
    say("The %s is yours now. Your trunk comes along", car_name(c->model));
    return true;
}

// ---------------------------------------------------------------- trading

static int trade_price(int it, bool selling)
{
    int p = items[it].price;
    if (p < 1) p = 1;
    return selling ? (p + 1) / 2 : p;
}
int item_price(int it, bool selling) { return trade_price(it, selling); }

void trade_open(int ped)
{
    trade_with = ped;
    memset(trade_stock, 0, sizeof trade_stock);
    uint32_t h = hsh((uint32_t)(peds[ped].home.x * 13 + peds[ped].home.z * 7) ^ world_seed);
    int n = 8 + (int)(h % 4);
    for (int k = 0; k < n; k++) {
        h = hsh(h + k);
        int it = (int)(h % (uint32_t)nitems);
        if (!strcmp(items[it].id, "cash")) continue;
        int cnt = items[it].kind == IK_AMMO ? 12 : items[it].stack > 4 ? 1 + (int)((h >> 8) % 3) : 1;
        pack_add(trade_stock, 12, it, cnt);
    }
    // traders always carry the essentials
    pack_add(trade_stock, 12, find_id("gas_can"), 1 + (int)(h >> 28) % 2);
    pack_add(trade_stock, 12, find_id("water"), 3);
    ui_screen = UI_TRADE;
    voice(S_V_WHAT, peds[ped].pos, 1, ped);
}

bool trade_buy(int slot)
{
    if (slot < 0 || slot >= 12 || trade_stock[slot].n <= 0) return false;
    int it = trade_stock[slot].item, price = trade_price(it, false), cash = find_id("cash");
    if (pack_count(cash) < price) { hud_msg("Not enough dollars. Sell something first", 2); return false; }
    if (pack_add(pack, PACK_SLOTS, it, 1) > 0) { hud_msg("No room in the pack", 2); return false; }
    pack_take(pack, PACK_SLOTS, cash, price);
    trade_stock[slot].n--;
    sfx(S_CASH, peds[PLAYER].pos, 0.6f, 1);
    return true;
}

bool trade_sell(int slot)
{
    if (slot < 0 || slot >= PACK_SLOTS || pack[slot].n <= 0) return false;
    int it = pack[slot].item, cash = find_id("cash");
    if (it == cash) return false;
    int price = trade_price(it, true);
    pack[slot].n--;
    pack_add(trade_stock, 12, it, 1);
    pack_add(pack, PACK_SLOTS, cash, price);
    sfx(S_CASH, peds[PLAYER].pos, 0.6f, 1.2f);
    return true;
}

// ---------------------------------------------------------------- sleeping

bool sleeping(void) { return sleep_fade > 0; }

void survive_sleep(float hours)
{
    Ped *p = &peds[PLAYER];
    bool in_car = p->car >= 0;
    bool tent = sv.tent && dist_xz(sv.camp_p, p->pos) < 5;
    bool bed = false;
    int ci = cont_near(p->pos, 2.5f);
    if (ci >= 0 && conts[ci].kind == CT_BED) bed = true;
    bool bag = pack_count(find_id("sleeping_bag")) > 0;
    float quality_k = bed ? 1.0f : tent ? 0.95f : in_car ? 0.75f : bag ? 0.8f : 0.55f;
    float temp = world_temp();
    sv.rest = clampf(sv.rest + 12.5f * hours * quality_k, 0, 100);
    sv.food = clampf(sv.food - 2.2f * hours * tune_hunger, 0, 100);
    sv.water = clampf(sv.water - 3.5f * hours * tune_thirst, 0, 100);
    float cold = temp < 12 ? (12 - temp) * 0.9f : 0;
    if (bed || tent || bag) cold *= 0.25f;
    if (in_car) cold *= 0.5f;
    if (near_fire()) cold = 0;
    sv.warmth = clampf(sv.warmth + (cold > 0 ? -cold * hours * 0.6f * tune_cold : 3 * hours), 0, 100);
    if (sv.sick > 0) sv.sick = clampf(sv.sick - 4 * hours, 0, 100);
    if (sv.food > 20 && sv.water > 20 && sv.warmth > 20) p->health = fminf(100, p->health + 3 * hours);
    day_hours = fmodf(day_hours + hours, 24);
    sv.days += hours / 24;
    if (sv.camp_fire > 0) sv.camp_fire = fmaxf(0, sv.camp_fire - hours * 60);
    sleep_fade = 2.0f;
    // out in the open, someone might find you
    bool safe = bed || tent;
    float risk = difficulty == DIFF_PEACEFUL ? 0 : (safe ? 0.05f : in_car ? 0.18f : 0.3f) * threat * tune_raiders;
    if (frand(0, 1) < risk) { raiders_ambush(); hud_big("You wake to voices", 3); }
    else hud_big(bed ? "You slept well" : tent ? "A night in the tent" : in_car ? "A cramped night in the car" : "You slept rough", 3);
    game_save();
}

// ---------------------------------------------------------------- what the use button does here

static int trader_near(void)
{
    for (int i = 1; i < MAX_PEDS; i++) {
        Ped *q = &peds[i];
        if (!q->used || q->state == PS_DEAD || q->kind != PED_CIV || q->role != ROLE_TRADER) continue;
        if (dist_xz(q->pos, peds[PLAYER].pos) < 2.8f) return i;
    }
    return -1;
}

static bool near_pond(Vector3 p)
{
    for (int a = 0; a < 8; a++) {
        float ang = a * 0.785f;
        if (is_water(p.x + sinf(ang) * 2.5f, p.z + cosf(ang) * 2.5f)) return true;
    }
    return is_water(p.x, p.z);
}

static const char *ct_word[CT_COUNT] = { "shelf", "fridge", "crate", "locker", "trunk", "desk", "toolbox", "bag", "pump",
                                         "well", "bed", "fire pit", "trader", "body" };
static char prompt_buf[64];
static int prompt_kind, prompt_ci;   // what survive_interact will do
enum { PK_NONE, PK_TRADE, PK_SEARCH, PK_PUMP, PK_WELL, PK_BED, PK_PIT, PK_CAR, PK_POND, PK_CAMP, PK_DEER, PK_LOOT };

int deer_near(Vector3 p, float r);    // raiders.c: a dead deer to butcher
void deer_butcher(int d);

const char *survive_prompt(void)
{
    Ped *p = &peds[PLAYER];
    prompt_kind = PK_NONE;
    if (!p->used || p->state == PS_DEAD || p->state == PS_RAGDOLL || search_ci >= 0) return NULL;
    const char *edit = estate_prompt();
    if (edit) return edit;
    if (p->car >= 0) {
        if (fabsf(car_speed(&cars[p->car])) < 1) { prompt_kind = PK_CAR; prompt_ci = p->car; return "Car and trunk"; }
        return NULL;
    }
    int t = trader_near();
    if (t >= 0) { prompt_kind = PK_TRADE; prompt_ci = t; return "Trade"; }
    int ci = cont_near(p->pos, 1.9f);
    if (ci >= 0) {
        Cont *c = &conts[ci];
        prompt_ci = ci;
        if (c->placed) { prompt_kind = PK_LOOT; return "Pick up item"; }
        switch (c->kind) {
        case CT_PUMP: prompt_kind = PK_PUMP; return "Hand-pump the tank";
        case CT_WELL: prompt_kind = PK_WELL; return "Draw water";
        case CT_BED: if (c->searched) { prompt_kind = PK_BED; return "Sleep"; } break;
        case CT_BENCH: prompt_kind = PK_PIT; return sv.camp_fire > 0 && dist_xz(sv.camp_p, c->p) < 3 ? "Rest by the fire" : "Make camp here";
        case CT_TRADER: break;
        default: break;
        }
        if (c->kind != CT_TRADER && c->kind != CT_BENCH) {
            prompt_kind = c->searched ? PK_LOOT : PK_SEARCH;
            if (c->kind == CT_TRUNK && c->car >= 0 && c->car == sv.home_car) prompt_kind = PK_NONE;
            else {
                snprintf(prompt_buf, sizeof prompt_buf, "%s %s", c->searched ? "Open" : "Search", ct_word[c->kind]);
                return prompt_buf;
            }
        }
    }
    int d = deer_near(p->pos, 2.2f);
    if (d >= 0) { prompt_kind = PK_DEER; prompt_ci = d; return "Butcher the deer"; }
    if (sv.camp_on && dist_xz(sv.camp_p, p->pos) < 3.2f) { prompt_kind = PK_CAMP; return sv.tent ? "Sleep in the tent" : "Rest by the fire"; }
    int car = car_near(p->pos, 4.5f);
    if (car >= 0 && cars[car].ai != AI_COP) { prompt_kind = PK_CAR; prompt_ci = car; return car == sv.home_car ? "Car and trunk" : "Look over this car"; }
    if (near_pond(p->pos)) { prompt_kind = PK_POND; return pack_count(find_id("rod")) > 0 ? "Fish / fill bottles" : "Fill bottles"; }
    return NULL;
}

static void fill_bottles(bool clean)
{
    int b = find_id("bottle"), w = find_id(clean ? "water" : "dirty_water");
    int n = pack_count(b);
    if (n <= 0) { hud_msg("No empty bottles to fill", 2); return; }
    pack_take(pack, PACK_SLOTS, b, n);
    give(w, n);
    char m[64];
    snprintf(m, sizeof m, "Filled %d bottle%s", n, n > 1 ? "s" : "");
    hud_msg(m, 2);
    sfx(S_PICKUP, peds[PLAYER].pos, 0.5f, 0.6f);
}

bool survive_search(int ci)
{
    if (ci < 0 || ci >= MAX_CONTS || !conts[ci].used || peds[PLAYER].car >= 0 || search_ci >= 0 ||
        Vector3Distance(peds[PLAYER].pos, conts[ci].p) > 2.1f) return false;
    Cont *c = &conts[ci];
    if (c->key && c->key < DROP_BASE && world_flag(exhausted_key(c->key))) return false;
    if (c->searched) { loot_open(ci); return true; }
    search_ci = ci;
    search_t = 0;
    ped_play(PLAYER, A_INTERACT);
    sfx(S_DOOR, c->p, .5f, .9f);
    return true;
}

void survive_interact(void)
{
    if (placement_active()) { placement_confirm(); return; }
    if (estate_use(false)) return;
    Ped *p = &peds[PLAYER];
    survive_prompt();
    switch (prompt_kind) {
    case PK_TRADE: trade_open(prompt_ci); break;
    case PK_SEARCH:
        search_ci = prompt_ci;
        search_t = 0;
        ped_play(PLAYER, A_INTERACT);
        sfx(S_DOOR, conts[prompt_ci].p, 0.5f, 0.9f);
        break;
    case PK_LOOT: {
        bool placed = conts[prompt_ci].placed;
        loot_open(prompt_ci);
        if (placed) { loot_take_all(); ui_screen = UI_NONE; loot_ci = -1; }
        break;
    }
    case PK_PUMP: {
        Cont *c = &conts[prompt_ci];
        uint32_t k = c->key ^ 0x5a5a;
        float tank = world_flag(k) ? 0 : 15 + (hsh(c->key) % 50);
        if (tank <= 0) { hud_msg("The underground tank is dry", 2.5f); break; }
        int home = sv.home_car;
        float got = 0;
        if (home >= 0 && cars[home].used && dist_xz(cars[home].pos, c->p) < 10) {
            got = fminf(tank, cars[home].fuel_max - cars[home].fuel);
            cars[home].fuel += got;
        }
        int empty = find_id("gas_empty"), full = find_id("gas_can");
        while (tank - got >= 6 && pack_count(empty) > 0) { pack_take(pack, PACK_SLOTS, empty, 1); give(full, 1); got += 12; }
        if (got <= 0) { hud_msg("Park your car by the pump, or bring empty jerry cans", 3); break; }
        world_set_flag(k);
        char m[64];
        snprintf(m, sizeof m, "Pumped %.0f litres by hand", fminf(got, tank));
        hud_msg(m, 2.5f);
        ped_play(PLAYER, A_INTERACT);
        sfx(S_PICKUP, c->p, 0.6f, 0.45f);
        break;
    }
    case PK_WELL: fill_bottles(true); break;
    case PK_POND:
        if (pack_count(find_id("rod")) > 0 && pack_count(find_id("bottle")) == 0) {
            if (fish_t >= 0) break;
            fish_t = frand(5, 11);
            hud_msg("You cast a line...", 2);
            ped_play(PLAYER, A_INTERACT);
        } else fill_bottles(false);
        break;
    case PK_BED: case PK_CAMP: survive_sleep(sv.rest < 40 ? 8 : 4); break;
    case PK_PIT: {
        Cont *c = &conts[prompt_ci];
        if (sv.camp_fire > 0 && dist_xz(sv.camp_p, c->p) < 3) { survive_sleep(sv.rest < 40 ? 8 : 4); break; }
        int wood = find_id("wood");
        if (pack_count(wood) < 2) {
            // a fire pit always has a few sticks around it
            if (!world_flag(c->key ^ 0x7777)) { world_set_flag(c->key ^ 0x7777); give(wood, 2); hud_msg("You gather some firewood", 2); }
            else { hud_msg("Need two pieces of firewood", 2); break; }
        }
        sv.camp_on = true; sv.camp_p = c->p; sv.tent = sv.tent && dist_xz(sv.camp_p, c->p) < 5;
        item_use(wood);
        break;
    }
    case PK_CAR: car_view = prompt_ci; ui_screen = UI_CAR; break;
    case PK_DEER: deer_butcher(prompt_ci); give(find_id("raw_meat"), 3); give(find_id("rags"), 1); hud_msg("You butcher the deer", 2.5f); break;
    default:
        if (pack_count(find_id("tent")) > 0 && sv.tent && dist_xz(sv.camp_p, p->pos) < 4) { sv.tent = false; sv.camp_on = sv.camp_fire > 0; hud_msg("Tent packed away", 2); }
        break;
    }
}

// ---------------------------------------------------------------- per tick

static float last_s = -1;

void survive_tick(float dt)
{
    Ped *p = &peds[PLAYER];
    if (!p->used || mode == MODE_DEAD) return;
    sync_weapons();
    wardrobe_apply();
    if (sleep_fade > 0) sleep_fade -= dt;
    float h = dt * HOURS;
    bool in_car = p->car >= 0 && cars[p->car].used;
    Car *car = in_car ? &cars[p->car] : NULL;
    float temp = world_temp();
    // hunger and thirst
    float heat = temp > 28 ? 1 + (temp - 28) * 0.06f : 1;
    float exert = !in_car && pv[PLAYER].speed > 4 ? 1.5f : 1;
    sv.food = clampf(sv.food - 4.0f * h * tune_hunger * exert, 0, 100);
    sv.water = clampf(sv.water - 6.5f * h * tune_thirst * heat * exert, 0, 100);
    sv.rest = clampf(sv.rest - (sv.sick > 30 ? 6.0f : 4.0f) * h, 0, 100);
    // warmth: the air, the car heater, the fire, the jacket
    float target = clampf(50 + (temp - 8) * 3.2f, 0, 100);
    if (in_car && !car->stalled) target = fmaxf(target, 85);
    else if (in_car) target = fmaxf(target, clampf(target + 25, 0, 70));
    if (near_fire()) target = 100;
    if (weather_rain > 0.3f && !in_car) target -= 20 * weather_rain;
    bool jacket = pack_count(find_id("jacket")) > 0;
    float rate = target < sv.warmth ? (jacket ? 0.35f : 0.8f) * tune_cold : 2.5f;
    sv.warmth = clampf(sv.warmth + (target - sv.warmth) * fminf(1, rate * h * 0.6f), 0, 100);
    // what it does to you
    float hurt = 0;
    if (sv.food <= 0) hurt += 4;
    if (sv.water <= 0) hurt += 8;
    if (sv.warmth < 10) hurt += 6;
    if (sv.sick > 40) hurt += 2 + (sv.sick - 40) * 0.08f;
    if (sv.rest <= 0) hurt += 2;
    if (hurt > 0) p->health -= hurt * h;
    else if (sv.food > 40 && sv.water > 40 && sv.warmth > 35 && sv.sick < 20) p->health = fminf(100, p->health + 1.5f * h);
    if (sv.bleed > 0) {
        p->health -= sv.bleed * 1.2f * dt;
        if ((ticks % 40) == 0) fx_burst(FX_BLOOD, Vector3Add(p->pos, (Vector3){ 0, 0.9f, 0 }), (Vector3){ 0, -1, 0 }, 1);
        sv.bleed = fmaxf(0, sv.bleed - dt * 0.004f);
    }
    if (sv.sick > 0 && sv.sick < 40) sv.sick = fmaxf(0, sv.sick - 1.5f * h);
    if (p->health <= 0 && p->state != PS_DEAD) ped_damage(PLAYER, 1000, (Vector3){ 0 }, -1, -1);
    // warnings, rate limited
    msg_cd -= dt;
    if (msg_cd <= 0) {
        msg_cd = 25;
        if (sv.water < 15) hud_msg("You're parched", 3);
        else if (sv.food < 15) hud_msg("You're starving", 3);
        else if (sv.warmth < 20) hud_msg("You're freezing", 3);
        else if (sv.rest < 12) hud_msg("You can barely keep your eyes open", 3);
        else if (in_car && car->fuel < car->fuel_max * 0.12f && car->fuel > 0) hud_msg("Fuel is low", 3);
        else if (in_car && car->heat > 1) hud_msg("The engine is overheating", 3);
        else msg_cd = 5;
    }
    // distance along the road
    Vector3 f = in_car ? car->pos : p->pos;
    float s = road_s(f);
    float km = (s - world_start_s()) / 1000.0f;
    if (km > sv.km) sv.km = km;
    if (last_s >= 0 && fabsf(s - last_s) < 50) (void)0;
    last_s = s;
    sv.days += h / 24;
    // stops: the first visit is announced
    int st = stop_near(f, 25);
    if (st >= 0 && !stops[st].visited) {
        stops[st].visited = true;
        world_set_flag(world_key(stops[st].seg, 15, 0));
        sv.stops_seen++;
        hud_big(stops[st].name, 3);
        hud_log_stop(stops[st].name, sv.km);
    }
    // searching takes a moment
    if (search_ci >= 0) {
        search_t += dt;
        Cont *c = &conts[search_ci];
        if (!c->used || dist_xz(c->p, p->pos) > 3 || p->car >= 0) search_ci = -1;
        else if (search_t >= (c->kind == CT_TRUNK || c->kind == CT_CORPSE ? 1.6f : 1.2f)) { loot_open(search_ci); search_ci = -1; }
    }
    if (ui_screen == UI_LOOT && (loot_ci < 0 || !conts[loot_ci].used || dist_xz(conts[loot_ci].p, p->pos) > 4)) { ui_screen = UI_NONE; loot_ci = -1; }
    if (ui_screen == UI_TRADE && (trade_with < 0 || !peds[trade_with].used || dist_xz(peds[trade_with].pos, p->pos) > 5)) ui_screen = UI_NONE;
    // fishing
    if (fish_t >= 0) {
        fish_t -= dt;
        if (pv[PLAYER].speed > 0.5f) { fish_t = -1; hud_msg("You reel in", 1.5f); }
        else if (fish_t < 0) {
            if (frand(0, 1) < 0.65f) { give(find_id("raw_fish"), 1); hud_msg("Got a fish!", 2); sfx(S_PICKUP, p->pos, 0.6f, 1); }
            else hud_msg("Nothing bites", 2);
        }
    }
    (void)act_t; (void)act_kind; (void)act_car;
    // the campfire
    if (sv.camp_fire > 0) {
        sv.camp_fire -= dt;
        if ((ticks % 3) == 0) fx_burst(FX_FIRE, Vector3Add(sv.camp_p, (Vector3){ 0, 0.25f, 0 }), (Vector3){ 0, 1, 0 }, 1);
        if ((ticks % 9) == 0) fx_burst(FX_SMOKE, Vector3Add(sv.camp_p, (Vector3){ 0, 0.9f, 0 }), (Vector3){ 0, 1, 0 }, 1);
        if (sv.camp_fire <= 0 && !sv.tent) sv.camp_on = false;
    }
    // the flashlight
    if (sv.torch_on) {
        sv.flashlight -= dt;
        if (sv.flashlight <= 0) { sv.flashlight = 0; sv.torch_on = false; hud_msg("The flashlight dies", 2); }
    }
    // autosave
    save_cd -= dt;
    if (save_cd <= 0) {
        save_cd = 60;
        if (!game_save()) hud_msg("Autosave failed", 3);
    }
}

static void item_draw(int item, Vector3 pos, float angle, bool preview)
{
    Item *it = &items[item];
    Matrix at = MatrixMultiply(MatrixRotateY(angle), MatrixTranslate(pos.x, pos.y, pos.z));
    Color tint = preview ? (Color){ 151, 230, 180, 255 } : (Color){ it->color >> 16, it->color >> 8, it->color, 255 };
    if (it->kind == IK_WEAPON && it->weapon > W_FIST && it->weapon < W_COUNT) {
        at = MatrixMultiply(MatrixTranslate(0, .19f, 0), at);
        draw_mesh(gun_mesh[it->weapon], at, preview ? tint : WHITE, MAT_PROP);
    } else draw_mesh(item_mesh[it->icon], at, tint, MAT_PROP);
}

void survive_draw(int pass)
{
    estate_draw(pass);
    Vector3 target;
    if (pass == PASS_MAIN && placement_target(&target)) item_draw(pack[place_slot].item, target, place_yaw, true);
    for (int i = 0; i < MAX_CONTS; i++) {
        Cont *c = &conts[i];
        if (!c->used || c->kind != CT_BAG || dist_xz(c->p, cam.position) > 26) continue;
        if (c->key < DROP_BASE) {
            draw_mesh(prop_mesh[PROP_BAG], MatrixTranslate(c->p.x, c->p.y, c->p.z), WHITE, MAT_PROP);
            continue;
        }
        for (int j = 0; j < 8; j++) {
            Slot slot = cont_items[i][j];
            if (slot.n <= 0 || slot.item >= nitems) continue;
            float a = c->placed ? c->yaw : j * 2.39996f, spread = j == 0 ? 0 : 0.26f + 0.035f * j;
            Vector3 pos = Vector3Add(c->p, (Vector3){ cosf(a) * spread, 0.015f, sinf(a) * spread });
            item_draw(slot.item, pos, a, false);
        }
    }
    if (sv.camp_on) {
        Matrix at = MatrixTranslate(sv.camp_p.x, sv.camp_p.y, sv.camp_p.z);
        draw_mesh(prop_mesh[PROP_FIRE], at, WHITE, MAT_PROP);
    }
    if (sv.camp_on && sv.tent) {
        Matrix t = MatrixMultiply(MatrixRotateY(0.6f), MatrixTranslate(sv.camp_p.x + 2.8f, ground_y(sv.camp_p.x + 2.8f, sv.camp_p.z + 1), sv.camp_p.z + 1));
        draw_mesh(prop_mesh[PROP_TENT], t, WHITE, MAT_PROP);
    }
    if (pass == PASS_MAIN && sv.camp_on && sv.camp_fire > 0) {
        float fl = 0.85f + 0.15f * sinf(game_s * 13) * sinf(game_s * 7.3f);
        light_add(Vector3Add(sv.camp_p, (Vector3){ 0, 0.8f, 0 }), (Vector3){ 3.2f * fl, 1.5f * fl, 0.5f * fl }, 11);
    }
    // the flashlight beam lights what's ahead
    if (pass == PASS_MAIN && sv.torch_on && sv.flashlight > 0 && peds[PLAYER].car < 0) {
        Ped *p = &peds[PLAYER];
        Vector3 f = { sinf(p->yaw), 0, cosf(p->yaw) };
        light_add(Vector3Add(p->pos, (Vector3){ f.x * 4, 1.2f, f.z * 4 }), (Vector3){ 1.6f, 1.5f, 1.3f }, 9);
    }
}

// ---------------------------------------------------------------- new trip

void survive_reset(uint32_t seed)
{
    memset(&sv, 0, sizeof sv);
    sv.food = 85; sv.water = 80; sv.rest = 90; sv.warmth = 75;
    sv.home_car = -1;
    sv.flashlight = 0;
    memset(pack, 0, sizeof pack);
    memset(cargo, 0, sizeof cargo);
    memset(cont_rolled, 0, sizeof cont_rolled);
    for (int i = 0; i < nstart; i++) pack_add(pack, PACK_SLOTS, start_kit[i].item, start_kit[i].n);
    pack_add(cargo, CARGO_SLOTS, find_id("gas_can"), 1);
    pack_add(cargo, CARGO_SLOTS, find_id("jug"), 1);
    pack_add(cargo, CARGO_SLOTS, find_id("jerky"), 2);
    pack_add(cargo, CARGO_SLOTS, find_id("patch"), 1);
    search_ci = -1; loot_ci = -1; trade_with = -1; car_view = -1;
    fish_t = act_t = -1; last_s = -1; save_cd = 60;
    msg_cd = search_t = sleep_fade = 0;
    camp_tent = false;
    drop_key = DROP_BASE;
    placement_cancel();
    estate_reset();
    (void)seed;
}

// ---------------------------------------------------------------- saving: plain text, item ids not numbers

#ifdef PLATFORM_WEB
#include <emscripten.h>
EM_JS(int, web_save_put, (const char *k, const char *v), { try { localStorage.setItem(UTF8ToString(k), UTF8ToString(v)); return 1; } catch (e) { return 0; } });
EM_JS(char *, web_save_get, (const char *k), { var v = null; try { v = localStorage.getItem(UTF8ToString(k)); } catch (e) {} return v ? stringToNewUTF8(v) : 0; });
EM_JS(void, web_save_del, (const char *k), { try { localStorage.removeItem(UTF8ToString(k)); } catch (e) {} });
#endif

#ifdef PLATFORM_WEB
static const char *save_key(WorldKind kind)
{
    static char key[64];
    snprintf(key, sizeof key, kind == WORLD_ROAD ? "longroad_trip" : "longroad_trip_%s", kind == WORLD_CITY ? "original-city" : world_ids[kind]);
    return key;
}
#else
static void save_path(char *out, int n, WorldKind kind)
{
    if (kind == WORLD_ROAD) snprintf(out, n, "%s/trip.sav", save_dir());
    else snprintf(out, n, "%s/trip-%s.sav", save_dir(), kind == WORLD_CITY ? "original-city" : world_ids[kind]);
}
#endif

static void put_inv(char **w, char *end, const char *key, Slot *inv, int ns)
{
    *w += snprintf(*w, end - *w, "%s=", key);
    for (int i = 0; i < ns; i++) if (inv[i].n > 0) *w += snprintf(*w, end - *w, "%s:%d,", items[inv[i].item].id, inv[i].n);
    *w += snprintf(*w, end - *w, "\n");
}

bool game_save(void)
{
    if (demo_active) return true;
    Ped *p = &peds[PLAYER];
    if (!p->used || mode == MODE_DEAD || p->state == PS_DEAD) return false;
    static char buf[131072];
    char *w = buf, *end = buf + sizeof buf;
    double px = p->pos.x + world_ox, pz = p->pos.z + world_oz;
    w += snprintf(w, end - w, "longroad=2\nseed=%u\nhours=%.3f\n", world_seed, day_hours);
    w += snprintf(w, end - w, "pos=%.3f %.3f %.3f %.3f\n", px, (double)p->pos.y, pz, (double)p->yaw);
    w += snprintf(w, end - w, "stats=%.2f %.2f %.2f %.2f %.2f %.2f %.4f %.3f %d %d %d %.1f %d\n", sv.food, sv.water, sv.rest, sv.warmth,
                  sv.sick, p->health, sv.km, sv.days, sv.kills, sv.stops_seen, sv.crafted, sv.flashlight, sv.torch_on);
    if (sv.home_car >= 0 && cars[sv.home_car].used) {
        Car *c = &cars[sv.home_car];
        Vector3 f = car_fwd(c);
        w += snprintf(w, end - w, "car=%d %.3f %.3f %.3f %.4f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.1f %d %d %d %d %d\n", c->model,
                      c->pos.x + world_ox, (double)c->pos.y, c->pos.z + world_oz, (double)atan2f(f.x, f.z), c->fuel, c->engine,
                      c->tire[0], c->tire[1], c->tire[2], c->tire[3], c->battery, c->radiator, c->health, c->armor,
                      c->paint.r, c->paint.g, c->paint.b, p->car == sv.home_car);
    }
    if (sv.camp_on) w += snprintf(w, end - w, "camp=%.3f %.3f %.3f %.1f %d\n", sv.camp_p.x + world_ox, (double)sv.camp_p.y, sv.camp_p.z + world_oz, sv.camp_fire, sv.tent);
    w += snprintf(w, end - w, "bleed=%.3f\n", sv.bleed);
    put_inv(&w, end, "pack", pack, PACK_SLOTS);
    put_inv(&w, end, "cargo", cargo, CARGO_SLOTS);
    for (int i = 0; i < MAX_CONTS; i++) {
        Cont *c = &conts[i];
        if (!c->used || c->key < DROP_BASE || c->kind != CT_BAG) continue;
        w += snprintf(w, end - w, "drop=%.3f %.3f %.3f %.3f ", c->p.x + world_ox, (double)c->p.y, c->p.z + world_oz, c->placed ? c->yaw : -100.0f);
        put_inv(&w, end, "items", cont_items[i], 8);
    }
    if (!estate_save(&w,end)) return false;
    uint32_t *fl;
    int nf = world_flags_get(&fl);
    if (end - w < 16 || (size_t)nf > (size_t)(end - w - 16) / 9) return false;
    w += snprintf(w, end - w, "flags=");
    for (int i = 0; i < nf && w < end - 16; i++) w += snprintf(w, end - w, "%x,", fl[i]);
    w += snprintf(w, end - w, "\n");
#ifdef PLATFORM_WEB
    return web_save_put(save_key(world_kind), buf);
#else
    char path[300];
    save_path(path, sizeof path, world_kind);
    char tmp[sizeof path + 5];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (write_text(tmp, buf) && rename(tmp, path) == 0) return true;
    remove(tmp);
    return false;
#endif
}

bool game_has_save(WorldKind kind)
{
#ifdef PLATFORM_WEB
    char *t = web_save_get(save_key(kind));
    bool ok = t != NULL;
    free(t);
    return ok;
#else
    char path[300];
    save_path(path, sizeof path, kind);
    return FileExists(path);
#endif
}

void game_delete_save(void)
{
#ifdef PLATFORM_WEB
    web_save_del(save_key(world_kind));
#else
    char path[300];
    save_path(path, sizeof path, world_kind);
    remove(path);
#endif
}

static void get_inv(const char *v, Slot *inv, int ns)
{
    memset(inv, 0, sizeof(Slot) * ns);
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", v);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char *c = strchr(tok, ':');
        if (!c) continue;
        *c = 0;
        int it = item_find(tok);
        if (it >= 0) pack_add(inv, ns, it, atoi(c + 1));
    }
}

// the trip is rebuilt from the seed; this puts the player, the car and the pack back
bool game_load(void)
{
    char *text;
#ifdef PLATFORM_WEB
    text = web_save_get(save_key(world_kind));
    if (!text) return false;
#else
    char path[300];
    save_path(path, sizeof path, world_kind);
    if (!FileExists(path)) return false;
    text = LoadFileText(path);
    if (!text) return false;
#endif
    uint32_t seed = 0;
    double px, pz;
    float py, yaw;
    const char *q = strstr(text, "\npos=");
    if (sscanf(text, "longroad=2\nseed=%u", &seed) != 1 || !q ||
        sscanf(q, "\npos=%lf %f %lf %f", &px, &py, &pz, &yaw) != 4 ||
        !isfinite(px) || !isfinite(py) || !isfinite(pz) || !isfinite(yaw) ||
        fabs(px) > 2e7 || fabs(pz) > 2e7 || fabsf(py) > 10000) {
#ifdef PLATFORM_WEB
        free(text);
#else
        UnloadFileText(text);
#endif
        return false;
    }
    new_game(seed);
    // the save holds absolute positions: move the origin there before anything streams
    // throw away the fresh trip's car, then rebase to the saved spot
    if (sv.home_car >= 0) { car_remove(sv.home_car); sv.home_car = -1; }
    Ped *p = &peds[PLAYER];
    if (p->car >= 0) { p->car = -1; p->state = PS_IDLE; }
    Vector3 d = { (float)(px - world_ox), 0, (float)(pz - world_oz) };
    if (world_kind != WORLD_CITY) world_rebase(d);
    p->pos = (Vector3){ (float)(px - world_ox), py, (float)(pz - world_oz) };
    p->yaw = yaw;
    extern float cam_yaw;
    cam_yaw = yaw;
    if ((q = strstr(text, "\nhours="))) sscanf(q, "\nhours=%f", &day_hours);
    if ((q = strstr(text, "\nstats="))) {
        int torch = 0;
        sscanf(q, "\nstats=%f %f %f %f %f %f %lf %f %d %d %d %f %d", &sv.food, &sv.water, &sv.rest, &sv.warmth, &sv.sick, &p->health,
               &sv.km, &sv.days, &sv.kills, &sv.stops_seen, &sv.crafted, &sv.flashlight, &torch);
        sv.torch_on = torch;
    }
    if ((q = strstr(text, "\nflags="))) {
        world_flags_clear();
        const char *s = q + 7;
        while (*s && *s != '\n') {
            unsigned v;
            if (sscanf(s, "%x", &v) == 1) world_set_flag(v);
            while (*s && *s != ',' && *s != '\n') s++;
            if (*s == ',') s++;
        }
    }
    // Refresh streamed objects with the restored flags before rebuilding the owned car.
    world_reload_active();
    if ((q = strstr(text, "\nbleed="))) sscanf(q, "\nbleed=%f", &sv.bleed);
    if ((q = strstr(text, "\ncar="))) {
        int model, armor, r, g, b, inside; double cx, cz; float cy, cyaw;
        float fuel, eng, t0, t1, t2, t3, bat, rad, hp;
        if (sscanf(q, "\ncar=%d %lf %f %lf %f %f %f %f %f %f %f %f %f %f %d %d %d %d %d", &model, &cx, &cy, &cz, &cyaw, &fuel, &eng,
                   &t0, &t1, &t2, &t3, &bat, &rad, &hp, &armor, &r, &g, &b, &inside) == 19 && model >= 0 && model < CAR_MODELS &&
            isfinite(cx) && isfinite(cy) && isfinite(cz) && isfinite(cyaw)) {
            Vector3 at = { (float)(cx - world_ox), cy, (float)(cz - world_oz) };
            int ci = car_spawn(model, at, cyaw, AI_NONE);
            if (ci >= 0) {
                Car *c = &cars[ci];
                c->fuel = fuel; c->engine = eng; c->tire[0] = t0; c->tire[1] = t1; c->tire[2] = t2; c->tire[3] = t3;
                c->battery = bat; c->radiator = rad; c->health = hp; c->armor = armor; c->owned = true;
                c->paint = (Color){ (unsigned char)r, (unsigned char)g, (unsigned char)b, 255 };
                sv.home_car = ci;
                if (inside) { c->driver = PLAYER; p->car = ci; p->state = PS_IN_CAR; pv[PLAYER].sit = 1; p->pos = c->pos; }
            }
        }
    }
    if ((q = strstr(text, "\ncamp="))) {
        double cx, cz; float cy, fire; int tent;
        if (sscanf(q, "\ncamp=%lf %f %lf %f %d", &cx, &cy, &cz, &fire, &tent) == 5) {
            sv.camp_on = true; sv.camp_p = (Vector3){ (float)(cx - world_ox), cy, (float)(cz - world_oz) }; sv.camp_fire = fire; sv.tent = tent;
        }
    }
    if ((q = strstr(text, "\npack="))) { char line[4096] = { 0 }; sscanf(q, "\npack=%4095[^\n]", line); get_inv(line, pack, PACK_SLOTS); }
    else memset(pack, 0, sizeof pack);
    if ((q = strstr(text, "\ncargo="))) { char line[4096]; line[0] = 0; sscanf(q, "\ncargo=%4095[^\n]", line); get_inv(line, cargo, CARGO_SLOTS); }
    else memset(cargo, 0, sizeof cargo);
    estate_load(text);
    q = text;
    while ((q = strstr(q, "\ndrop="))) {
        double x, z;
        float y, angle = -100;
        char line[512];
        q++;
        bool parsed = sscanf(q, "drop=%lf %f %lf %f items=%511[^\n]", &x, &y, &z, &angle, line) == 5;
        if (!parsed) {
            angle = -100;
            parsed = sscanf(q, "drop=%lf %f %lf items=%511[^\n]", &x, &y, &z, line) == 4;
        }
        if (!parsed || !isfinite(angle) || !isfinite(x) || !isfinite(y) || !isfinite(z) ||
            fabs(x) > 2e7 || fabs(z) > 2e7) continue;
        int ci = ground_drop((Vector3){ (float)(x - world_ox), y, (float)(z - world_oz) }, CT_BAG);
        if (ci >= 0) {
            conts[ci].placed = angle != -100;
            conts[ci].yaw = conts[ci].placed ? angle : 0;
            get_inv(line, cont_items[ci], 8);
            bool empty = true;
            for (int i = 0; i < 8; i++) if (cont_items[ci][i].n > 0) empty = false;
            if (empty) conts[ci].used = false;
        }
    }
#ifdef PLATFORM_WEB
    free(text);
#else
    UnloadFileText(text);
#endif
    hud_big(world_names[world_kind], 2.5f);
    return true;
}

int loot_force(int ci) { loot_open(ci); return 0; }
