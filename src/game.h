#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>
#include "raylib.h"
#include "raymath.h"

// Long Road: an endless highway survival game, forked from Out on Parole.
// units: metres, seconds, radians. y is up.

#ifndef GAME_VERSION
#define GAME_VERSION "dev"
#endif

#define TICK_HZ 60
#define DT (1.0f / TICK_HZ)

// ---- input (all game code reads this, never raylib input, so replays work)

enum {
    BTN_FIRE = 1 << 0, BTN_AIM = 1 << 1, BTN_JUMP = 1 << 2, BTN_SPRINT = 1 << 3,
    BTN_ENTER = 1 << 4, BTN_RELOAD = 1 << 5, BTN_NEXT = 1 << 6, BTN_PREV = 1 << 7,
    BTN_HORN = 1 << 8, BTN_MAP = 1 << 9, BTN_PAUSE = 1 << 10, BTN_CAM = 1 << 11,
    BTN_BAG = 1 << 12, BTN_CROUCH = 1 << 13, BTN_USE = 1 << 14, BTN_LIGHTS = 1 << 15,
};

typedef struct {
    float mx, my;      // move: x right, y forward. in a car: steer, gas(+)/brake(-)
    float lx, ly;      // look delta this tick, radians
    uint16_t held;     // BTN_* down
    uint16_t hit;      // BTN_* went down this tick
} Input;

// ---- assets (procgen.c builds every mesh and clip from code)

enum { CAR_SEDAN, CAR_HATCH, CAR_WAGON, CAR_PICKUP, CAR_SUV, CAR_VAN, CAR_JEEP, CAR_MUSCLE, CAR_TRUCK, CAR_RAIDER,
       CAR_MODELS };

typedef struct {
    Mesh body;            // colors.rgb albedo, colors.a = CM_* material
    Mesh wheel;           // one wheel, centred on its axle, axle along x, outer face +x
    Vector3 wheel_pos[4]; // FL FR RL RR, model space
    float wheel_r;
    Vector3 size;         // full width, height, length
    Vector3 seat;         // driver seat, model space
    float mass_kg;
} CarModel;
// model space for cars: origin on the ground under the middle of the wheelbase, +z forward, +x is the car's left

enum { CM_PLAIN = 0, CM_TAIL = 32, CM_HEAD = 64, CM_GLASS = 128, CM_SIREN = 192, CM_PAINT = 255 };

enum { BODY_MALE, BODY_FEMALE, BODY_COUNT };
enum { HAIR_NONE = -1, HAIR_BUZZ, HAIR_PARTED, HAIR_LONG, HAIR_BUNS, HAIR_BEARD, HAIR_CAP, HAIR_COUNT };
enum { W_FIST, W_PISTOL, W_RIFLE, W_SHOTGUN, W_BAT, W_COUNT };
enum { PROP_CRATE, PROP_BAG, PROP_JERRY, PROP_FIRE, PROP_TENT, PROP_DEER, PROP_CORPSE, PROP_BARREL, PROP_PUMP, PROP_SIGN,
       PROP_WOLF, PROP_COUNT };

enum { R_SKIN, R_SHIRT, R_PANTS, R_SHOES, R_HAIR, R_EYES, R_COUNT };

enum {
    A_IDLE, A_WALK, A_JOG, A_SPRINT, A_JUMP_START, A_JUMP_LOOP, A_JUMP_LAND, A_DEATH, A_DRIVE,
    A_HIT_CHEST, A_HIT_HEAD, A_PISTOL_IDLE, A_PISTOL_AIM, A_PISTOL_SHOOT, A_PISTOL_RELOAD,
    A_PUNCH_JAB, A_PUNCH_CROSS, A_COWER, A_ROLL, A_INTERACT, A_TALK, A_CROUCH_WALK, A_COUNT
};

#define MAX_BONES 32

typedef struct {
    int count;
    int parent[MAX_BONES];
    Vector3 bind_pos[MAX_BONES];
    Quaternion bind_rot[MAX_BONES];
    Matrix inv_bind[MAX_BONES];
    int head, hand_r, hand_l, hips, spine;
} Skeleton;

typedef struct {
    int frames;
    float fps;
    bool loop;
    Quaternion *rot;   // frames * bones, local
    Vector3 *hips;     // frames, local translation of the hips bone
} Clip;

extern CarModel car_models[CAR_MODELS];
extern Mesh body_mesh[BODY_COUNT];
extern Mesh hair_mesh[HAIR_COUNT];
extern Mesh gun_mesh[W_COUNT];
extern Mesh prop_mesh[PROP_COUNT];
extern Skeleton skel;
extern Clip clips[A_COUNT];

void assets_load(void);
void pose_sample(const Clip *c, float t, Quaternion *rot, Vector3 *hips);
void pose_matrices(const Quaternion *rot, Vector3 hips, Matrix *out);

// ---- world (road.c): the endless road, the land around it, the stops along it

typedef struct { Vector3 p, n; float t; int kind, id; } Hit;
enum { HIT_NONE, HIT_WORLD, HIT_CAR, HIT_PED };

typedef struct {
    Vector3 a, b;       // centreline of one lane, driving a -> b
    int next[3], nnext;
    int light;          // always -1 out here
    float len, speed;
} Lane;

typedef struct { Vector3 p; int nb[4], n; int light; } WalkNode;

enum { BIOME_DESERT, BIOME_PLAINS, BIOME_FOREST, BIOME_BADLANDS, BIOME_SNOW, BIOME_COUNT };
enum { STOP_NONE, STOP_GAS, STOP_DINER, STOP_MOTEL, STOP_FARM, STOP_WRECKS, STOP_CAMP, STOP_TOWN, STOP_TRADER,
       STOP_RANGER, STOP_GARAGE, STOP_COUNT };

// something to search or use at a stop
enum { CT_SHELF, CT_FRIDGE, CT_CRATE, CT_LOCKER, CT_TRUNK, CT_DESK, CT_TOOLBOX, CT_BAG, CT_PUMP, CT_WELL, CT_BED,
       CT_BENCH, CT_TRADER, CT_CORPSE, CT_COUNT };

typedef struct {
    bool used;
    Vector3 p;
    float yaw;
    int kind, stop;     // CT_*, index into stops
    uint32_t key;       // stable id: segment and slot, so search state survives streaming and saves
    bool searched;
    int car;            // CT_TRUNK: the wreck it belongs to, or -1
} Cont;

typedef struct {
    bool used;
    int kind, biome;
    int seg;            // road segment it belongs to
    Vector3 p;          // centre of the lot, current world space
    float yaw, side;    // facing the road; side of the road (+1 left, -1 right of travel)
    char name[32];
    bool visited, hostile;
    float km;
} Stop;

#define MAX_LANES 2048
#define MAX_CONTS 256
#define MAX_STOPS 24

extern Lane lanes[MAX_LANES]; extern int nlanes;
extern WalkNode *walk; extern int nwalk;
extern Cont conts[MAX_CONTS];
extern Stop stops[MAX_STOPS];
extern double world_ox, world_oz;     // world space origin offset after rebasing, metres

void world_build(uint32_t seed);
void world_tick(float dt);
int  light_state(int light);
bool walk_light_ok(int light);
float ground_y(float x, float z);
bool is_water(float x, float z);
bool world_ray(Vector3 o, Vector3 dir, float maxd, Hit *h);
bool world_push(Vector3 *p, float radius, float height);
int  world_boxes(Vector3 c, float r, BoundingBox *out, int max);
int  nearest_lane(Vector3 p, float maxd);
int  nearest_walk(Vector3 p, float maxd);
void world_draw(int pass);
float road_offset(float x, float z, float *s_out);   // signed distance from the centreline (+ is left of travel)
Vector3 road_at(float s, Vector3 *dir);              // point and heading at road distance s (metres from start)
float road_s(Vector3 p);                             // road distance nearest p
bool on_road(float x, float z);
int  biome_at(float s);
float biome_mix(float s, int biome);                 // 0..1 how much of a biome is here
const char *biome_name(int b);
int  stop_near(Vector3 p, float r);
int  cont_near(Vector3 p, float r);
void world_rebase(Vector3 d);                        // shift everything by -d; called by main.c far from origin
float world_temp(void);                              // air temperature, C, with weather and time of day
extern float weather_rain, weather_fog, weather_dust, weather_snow, weather_wind;

// ---- cars (car.c)

#define MAX_CARS 64
enum { AI_NONE, AI_PARKED, AI_TRAFFIC, AI_COP, AI_FLEE, AI_WRECK };   // AI_COP is a raider chasing its target

typedef struct {
    bool used;
    int model, ai;
    Vector3 pos, vel;
    Quaternion rot;
    Vector3 angvel;
    float steer, throttle, brake;
    bool handbrake;
    float wheel_spin[4], wheel_comp[4];
    bool wheel_ground[4];
    float health;          // 1000 new, smoke < 400, fire < 150, explodes at 0
    float fire_s;
    Color paint;
    int driver;
    int lane; float lane_t;
    int target;
    float ai_timer, stuck_s;
    bool siren, horn, lights;
    float rpm, skid;
    // condition, out on the road
    float fuel, fuel_max;  // litres
    float engine;          // 0..100, under 15 it stalls
    float tire[4];         // 0..100, flat at 0
    float battery;         // 0..100, no start at 0
    float radiator;        // 0..100, overheats when low
    float heat;            // engine temperature 0..1.3, > 1 damages
    bool armor, stalled, owned;
    int cargo;             // inventory slot block for this car, -1 none
    uint32_t key;          // stable id for wrecks placed by the world
} Car;

extern Car cars[MAX_CARS];

int  car_spawn(int model, Vector3 p, float yaw, int ai);
void car_remove(int c);
void car_damage(int c, float dmg, Vector3 at, int attacker);
Matrix car_matrix(const Car *c);
Vector3 car_fwd(const Car *c);
float car_speed(const Car *c);
void cars_tick(float dt);
void cars_draw(int pass);
int  car_near(Vector3 p, float r);
bool cars_push(Vector3 *p, float radius);
void car_drive_to(int c, Vector3 target, float speed_ms, bool reckless);
void cars_shift(Vector3 d);
float car_kmh(const Car *c);
const char *car_name(int model);

// ---- people (ped.c). peds[0] is the player.

#define MAX_PEDS 48
#define PLAYER 0
enum { PED_CIV, PED_COP, PED_PLAYER };      // PED_COP: a raider
enum { PS_WALK, PS_IDLE, PS_FLEE, PS_COWER, PS_FIGHT, PS_CHASE, PS_DEAD, PS_IN_CAR, PS_ENTERING, PS_EXITING,
       PS_RAGDOLL, PS_CALLING, PS_ARREST, PS_HANDS_UP };

typedef struct {
    bool used;
    int kind, state;
    Vector3 pos, vel;
    float yaw, aim_pitch;
    bool aiming;
    float health, armor;
    int car, seat;
    int body, hair;
    Color pal[R_COUNT];
    int weapon; int ammo[W_COUNT]; int clip_ammo;
    float fire_cd, reload_s;
    int anim, prev_anim; float anim_t, prev_t, blend;
    int node, next_node;
    int target; float timer;
    float panic;
    Vector3 rag[11], rag_prev[11];
    int cash;
    Vector3 home;          // npc: where they hang around
    int role;              // npc: ROLE_*
} Ped;
enum { ROLE_DRIFTER, ROLE_TRADER, ROLE_RAIDER, ROLE_HITCHER };

extern Ped peds[MAX_PEDS];
extern Camera3D cam;
void drone_cmd(const char *cmd, const float *v);
void debug_parse(const char *list);
void debug_draw(void);

int  ped_spawn(int kind, Vector3 p, float yaw);
void ped_damage(int i, float dmg, Vector3 impulse, int attacker, int weapon);
void peds_tick(const Input *in, float dt);
void peds_draw(int pass);
void camera_update(const Input *in, float dt);
void ped_pose(const Ped *p, Matrix *bones_out);
Vector3 ped_hand(const Ped *p);
void ped_alarm(Vector3 p, float radius, int source);
void ped_play(int i, int anim);
void ped_remove(int i);
int  ped_at(Vector3 p, float r, int ignore);
void peds_shift(Vector3 d);
void camera_shift(Vector3 d);
extern float cam_touch_yaw, cam_touch_pitch;   // look input from touch, added by main.c

// ---- combat (combat.c)

typedef struct { const char *name; float dmg, rate, spread, range; int clip, pellets; float reload_s; } Weapon;
extern const Weapon weapons[W_COUNT];

void combat_tick(const Input *in, float dt);
void weapon_stow(void);
bool fire_weapon(int ped, Vector3 from, Vector3 dir);
void explode(Vector3 p, float radius, int attacker);
void pickups_draw(int pass);
bool shoot_ray(Vector3 o, Vector3 dir, float maxd, int ignore_ped, int ignore_car, Hit *h);
void combat_shift(Vector3 d);
void weapon_give(int w);          // the player now owns w (from the pack)
void weapon_take(int w);
bool weapon_owned(int w);
extern int cur_weapon_ammo_item(int w);

// ---- raiders and wildlife (raiders.c)

void raiders_tick(float dt);
void raiders_reset(void);
void raiders_draw(int pass);
void raiders_shift(Vector3 d);
int  raiders_active(void);       // how many hostiles are hunting the player
bool hunt_hit(Vector3 o, Vector3 d, float maxd, float dmg, float *t_out);   // shots at animals
extern float threat;             // 0..1 how dangerous this stretch is

// ---- survival (survive.c): the pack, stats, crafting, loot, saving

#define MAX_ITEMS 160
#define MAX_RECIPES 64
#define PACK_SLOTS 24
#define CARGO_SLOTS 40

enum { IK_FOOD, IK_DRINK, IK_MED, IK_FUEL, IK_PART, IK_TOOL, IK_AMMO, IK_WEAPON, IK_GEAR, IK_MAT, IK_TRADE, IK_COUNT };
// what using an item does
enum { E_NONE, E_EAT, E_DRINK, E_HEAL, E_REST, E_WARM, E_FUEL, E_ENGINE, E_TIRE, E_BATTERY, E_RADIATOR,
       E_ARMOR, E_WEAPON, E_LIGHT, E_CAMP, E_CURE, E_BANDAGE, E_TENT, E_COUNT };

typedef struct {
    char id[24], name[32], desc[80];
    int kind, effect;
    float value, value2;      // effect strength; value2: secondary (water from food, sickness chance...)
    float weight;
    int stack, price;
    int weapon;               // W_* for IK_WEAPON, ammo kind for IK_AMMO
    float loot[CT_COUNT];     // chance weight per container kind
    float sick;               // chance using it makes you ill
    int color;                // icon tint, 0xRRGGBB
    int icon;                 // ICON_*
    bool mod;                 // came from a mod
} Item;

enum { ICON_CAN, ICON_BOTTLE, ICON_BOX, ICON_CROSS, ICON_JERRY, ICON_GEAR, ICON_WRENCH, ICON_BULLET, ICON_GUN, ICON_BAG,
       ICON_PLANK, ICON_COIN, ICON_TIRE, ICON_BATTERY, ICON_PILL, ICON_FLAME, ICON_MEAT, ICON_LEAF, ICON_COUNT };

typedef struct { int item, n; } Slot;

typedef struct {
    char id[24];
    int out, out_n;
    int in[4], in_n[4], nin;
    int tool;                 // item that must be in the pack, not consumed, or -1
    bool fire;                // needs a campfire nearby
} Recipe;

extern Item items[MAX_ITEMS]; extern int nitems;
extern Recipe recipes[MAX_RECIPES]; extern int nrecipes;
extern Slot pack[PACK_SLOTS];
extern Slot cargo[CARGO_SLOTS];

typedef struct {
    float food, water, rest, warmth;   // 0..100, 100 is good
    float sick;                         // 0..100
    float bleed;                        // 0..1
    double km;                          // total distance driven or walked
    float days;                         // in-game days survived
    int kills, stops_seen, crafted, best_km;
    int home_car;                       // the car whose trunk is `cargo`, -1 when none
    bool camp_on; Vector3 camp_p;
    float camp_fire;                    // seconds of fire left
    bool tent;
    float flashlight;                   // battery seconds of torch
    bool torch_on;
} Survivor;
extern Survivor sv;

int  item_find(const char *id);
int  pack_count(int item);
int  pack_add(Slot *inv, int nslots, int item, int n);    // returns how many did not fit
bool pack_take(Slot *inv, int nslots, int item, int n);
float pack_weight(void);
float pack_capacity(void);
bool item_use(int item);                 // from the pack; false if it did nothing
bool recipe_can(int r);
bool recipe_make(int r);
void survive_tick(float dt);
void survive_reset(uint32_t seed);
void survive_interact(void);             // BTN_USE: search, refuel, repair, trade, camp
const char *survive_prompt(void);        // what BTN_USE would do now, or NULL
void loot_container(int ci);
bool game_save(void);
bool game_load(void);
bool game_has_save(void);
void game_delete_save(void);
void survive_sleep(float hours);
void trade_open(int ped);
void survive_draw(int pass);
void cont_drop(Vector3 p, int kind);
int  item_empty(int it);
float item_extra_rest(int it); float item_extra_heal(int it); float item_extra_warm(int it);
extern Slot loot_slots[8]; extern int loot_ci;        // the container open in the loot window
extern Slot trade_stock[12];
extern int car_view;                                  // the car shown in the car screen
bool loot_take(int slot);
void loot_take_all(void);
bool trade_buy(int slot);
bool trade_sell(int pack_slot);
bool cargo_move(bool to_cargo, int slot);
bool car_siphon(int ci);
bool car_strip(int ci);
bool car_claim(int ci);
bool pack_drop(int slot);
bool sleeping(void);
extern float sleep_fade;
extern float tune_hunger, tune_thirst, tune_fuel, tune_raiders, tune_loot, tune_cold;
extern Slot start_kit[16]; extern int nstart;
bool world_flag(uint32_t key);
void world_set_flag(uint32_t key);
uint32_t world_key(int k, int kind, int slot);
int  world_flags_get(uint32_t **out);
void world_flags_clear(void);
float world_start_s(void);
void world_stream_all(void);
void raiders_ambush(void);
float km_now(void);
extern int trade_with;                   // ped we are trading with, -1 when none
extern float search_t; extern int search_ci;   // a search in progress
const char *item_kind_name(int k);
int  item_price(int item, bool selling);

// ---- data and mods (data.c)

void data_load(void);                    // built-in content then every mod
int  mods_count(void);
const char *mod_name(int i);
bool mod_enabled(int i);
void mod_toggle(int i);
const char *mods_dir(void);
int  mod_import_text(const char *name, const char *text);   // adds a mod from text (web / android picker); 0 ok
extern char data_errors[512];

// ---- effects (fx.c)

enum { FX_SPARK, FX_BLOOD, FX_SMOKE, FX_FIRE, FX_DEBRIS, FX_GLASS, FX_MUZZLE, FX_DUST, FX_EXPLOSION, FX_WATER, FX_RAIN,
       FX_SNOW, FX_STEAM };
enum { DECAL_BULLET, DECAL_BLOOD, DECAL_SCORCH, DECAL_OIL };

void fx_burst(int kind, Vector3 p, Vector3 dir, int n);
void fx_decal(int kind, Vector3 p, Vector3 n, float size);
void fx_skid(int key, Vector3 p, float width, bool on);
void fx_light(Vector3 p, Vector3 rgb, float radius, float life);
void fx_tracer(Vector3 a, Vector3 b);
void fx_shake(float amount);
void fx_tick(float dt);
void fx_draw(int pass);
void fx_shift(Vector3 d);

// ---- rendering (render.c)

enum { PASS_SHADOW, PASS_MAIN };
enum { MAT_WORLD, MAT_CAR, MAT_PROP };

void render_init(void);
void render_frame(int w, int h);
void draw_mesh(Mesh m, Matrix xf, Color tint, int mat);
void draw_char(Mesh m, Matrix xf, const Matrix *bones, const Color *pal);
extern float day_hours;
extern Vector3 sun_dir;
extern int quality;              // 0 low (phones), 1 medium, 2 high

// ---- sound (audio.c)

enum { S_PISTOL, S_SMG, S_SHOTGUN, S_EMPTY, S_RELOAD, S_PUNCH, S_HIT, S_CRASH_SMALL, S_CRASH_BIG, S_GLASS,
       S_EXPLOSION, S_RICOCHET, S_STEP, S_DOOR, S_SCREAM, S_PICKUP, S_CASH, S_BODYFALL, S_UI,
       S_V_DROP, S_V_HANDS, S_V_TAKEIT, S_V_DONTSHOOT, S_V_OUTTA, S_V_NOTODAY,
       S_V_SCREAM, S_V_GRUNT, S_V_MYCAR, S_V_HELP, S_V_WHAT, S_V_WATCH, S_V_GUN, S_COUNT };

extern bool audio_offline;
void audio_init(void);
void sfx(int id, Vector3 p, float vol, float pitch);
void voice(int id, Vector3 p, float vol, int ped);
void audio_tick(float dt);
void audio_render(float *out, int frames);
extern float audio_volume;

// ---- hud and menus (hud.c)

void hud_draw(int w, int h);
void hud_msg(const char *text, float secs);
void hud_big(const char *text, float secs);
bool hud_touch_update(Input *in, int w, int h);
void hud_log_stop(const char *name, float km);
void render_set_quality(int q);   // on-screen controls; true when touch is in use
void hud_input(const Input *in);                   // menus and the pack read input here
extern int ui_screen;
enum { UI_NONE, UI_PACK, UI_CRAFT, UI_CAR, UI_TRADE, UI_MAP, UI_PAUSE, UI_SETTINGS, UI_MODS, UI_TITLE, UI_DEAD, UI_HELP, UI_LOOT };
extern bool ui_blocks_game;                        // a menu has the input

// ---- platform (platform.c): tilt, touch, storage paths

void platform_init(void);
bool tilt_available(void);
Vector2 tilt_read(void);              // x: roll (steer), y: pitch (forward/back), -1..1 after calibration
void tilt_calibrate(void);
const char *save_dir(void);
bool is_touch_device(void);
void platform_vibrate(int ms);
extern bool tilt_steer, tilt_walk, tilt_invert;    // settings
void plat_put(const char *key, const char *val);
char *plat_get(const char *key);   // malloc'd, or NULL
bool write_text(const char *path, const char *text);
extern float tilt_sens;

// ---- main.c

enum { MODE_PLAY, MODE_DEAD, MODE_BUSTED, MODE_PAUSE, MODE_MAP, MODE_TITLE };
extern int mode;
extern float game_s;
extern uint64_t ticks;
extern uint32_t world_seed;

uint32_t rnd(void);
float frand(float a, float b);
void respawn(Vector3 at);
void new_game(uint32_t seed);
void settings_save(void);

#endif
