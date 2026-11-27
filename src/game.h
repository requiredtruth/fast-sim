#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>
#include "raylib.h"
#include "raymath.h"

// units: metres, seconds, radians. y is up.

#define TICK_HZ 60
#define DT (1.0f / TICK_HZ)

// ---- input (all game code reads this, never raylib input, so replays work)

enum {
    BTN_FIRE = 1 << 0, BTN_AIM = 1 << 1, BTN_JUMP = 1 << 2, BTN_SPRINT = 1 << 3,
    BTN_ENTER = 1 << 4, BTN_RELOAD = 1 << 5, BTN_NEXT = 1 << 6, BTN_PREV = 1 << 7,
    BTN_HORN = 1 << 8, BTN_MAP = 1 << 9, BTN_PAUSE = 1 << 10, BTN_CAM = 1 << 11,
    BTN_RADIO = 1 << 12, BTN_CROUCH = 1 << 13,
};

typedef struct {
    float mx, my;      // move: x right, y forward. in a car: steer, gas(+)/brake(-)
    float lx, ly;      // look delta this tick, radians
    uint16_t held;     // BTN_* down
    uint16_t hit;      // BTN_* went down this tick
} Input;

// ---- assets (tools/pack.py -> build/assets.c, unpacked by pack.c)

enum { CAR_SEDAN, CAR_COMPACT, CAR_HATCH, CAR_MINIVAN, CAR_PICKUP, CAR_SUV, CAR_WAGON, CAR_OFFROAD,
       CAR_COP, CAR_TAXI, CAR_MODELS };

typedef struct {
    Mesh body;            // colors.rgb baked albedo, colors.a = CM_* material
    Mesh wheel;           // one wheel, centred on its axle, axle along x
    Vector3 wheel_pos[4]; // FL FR RL RR, model space
    float wheel_r;
    Vector3 size;         // bounding box extents (full width, height, length)
    Vector3 seat;         // driver seat, model space
    float mass_kg;
} CarModel;
// model space for cars: origin on the ground under the middle of the wheelbase, +z forward, +x is the car's left

// car vertex colors.a
enum { CM_PLAIN = 0, CM_TAIL = 32, CM_HEAD = 64, CM_GLASS = 128, CM_SIREN = 192, CM_PAINT = 255 };

enum { BODY_MALE, BODY_FEMALE, BODY_COUNT };
enum { HAIR_NONE = -1, HAIR_BUZZ, HAIR_PARTED, HAIR_LONG, HAIR_BUNS, HAIR_BEARD, HAIR_CAP, HAIR_COUNT };
enum { W_FIST, W_PISTOL, W_SMG, W_SHOTGUN, W_COUNT };

// character vertex colors.r = region, used to pick a colour from the ped's palette
enum { R_SKIN, R_SHIRT, R_PANTS, R_SHOES, R_HAIR, R_EYES, R_COUNT };

enum {
    A_IDLE, A_WALK, A_JOG, A_SPRINT, A_JUMP_START, A_JUMP_LOOP, A_JUMP_LAND, A_DEATH, A_DRIVE,
    A_HIT_CHEST, A_HIT_HEAD, A_PISTOL_IDLE, A_PISTOL_AIM, A_PISTOL_SHOOT, A_PISTOL_RELOAD,
    A_PUNCH_JAB, A_PUNCH_CROSS, A_COWER, A_ROLL, A_INTERACT, A_TALK, A_CROUCH_WALK, A_COUNT
};

#define MAX_BONES 32

typedef struct {
    int count;
    int parent[MAX_BONES];        // -1 for root
    Vector3 bind_pos[MAX_BONES];  // local bind pose
    Quaternion bind_rot[MAX_BONES];
    Matrix inv_bind[MAX_BONES];   // model space -> bone space
    int head, hand_r, hand_l, hips, spine; // handy bone indices
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
extern Mesh hair_mesh[HAIR_COUNT];   // skinned to the head bone like the body
extern Mesh heli_mesh[2];            // police helicopter: body, rotor. origin at the rotor hub, nose +z
extern Mesh gun_mesh[W_COUNT];       // W_FIST is empty; grip at origin, barrel along +z
extern Skeleton skel;
extern Clip clips[A_COUNT];

void assets_load(void);
void pose_sample(const Clip *c, float t, Quaternion *rot, Vector3 *hips); // fills skel.count rotations
void pose_matrices(const Quaternion *rot, Vector3 hips, Matrix *out);    // local pose -> skinning matrices
// characters: bind pose stands on y = 0, faces +z, about 1.8 m tall

// ---- world (world.c): the city, its graphs and static collision

#define WORLD_HALF 640.0f

typedef struct { Vector3 p, n; float t; int kind, id; } Hit;
enum { HIT_NONE, HIT_WORLD, HIT_CAR, HIT_PED };

typedef struct {
    Vector3 a, b;       // centreline of one lane, driving a -> b
    int next[3], nnext; // lanes that continue from b
    int light;          // traffic light group guarding b, -1 if none
    float len, speed;   // m, m/s limit
} Lane;

typedef struct {
    Vector3 p;
    int nb[4], n;
    int light;          // >= 0 when this node starts a crosswalk
} WalkNode;

typedef struct { Vector3 door, inside; float yaw; int kind; float robbed_at; } Shop;

extern Lane *lanes; extern int nlanes;
extern WalkNode *walk; extern int nwalk;
extern Shop *shops; extern int nshops;
extern Vector3 hospital_pos, police_pos;

void world_build(uint32_t seed);
void world_tick(float dt);                      // traffic lights
int  light_state(int light);                    // 0 green, 1 yellow, 2 red, for traffic on that light
bool walk_light_ok(int light);                  // pedestrians may cross
float ground_y(float x, float z);               // street level 0; water returns -2
bool is_water(float x, float z);
bool world_ray(Vector3 o, Vector3 dir, float maxd, Hit *h);   // static geometry only
bool world_push(Vector3 *p, float radius, float height);        // push a vertical capsule out of buildings/props
int  world_boxes(Vector3 c, float r, BoundingBox *out, int max); // solid boxes near c, for car collision
int  nearest_lane(Vector3 p, float maxd);
int  nearest_walk(Vector3 p, float maxd);
void world_draw(int pass);                      // called by render.c

// ---- cars (car.c)

#define MAX_CARS 96
enum { AI_NONE, AI_PARKED, AI_TRAFFIC, AI_COP, AI_FLEE, AI_WRECK };

typedef struct {
    bool used;
    int model, ai;
    Vector3 pos, vel;      // centre of mass, world
    Quaternion rot;
    Vector3 angvel;
    float steer, throttle, brake;
    bool handbrake;
    float wheel_spin[4], wheel_comp[4];
    bool wheel_ground[4];
    float health;          // 1000 new, smoke < 400, fire < 150, explodes at 0
    float fire_s;
    Color paint;
    int driver;            // ped index or -1
    int lane; float lane_t;
    int target;            // cop: ped they chase
    float ai_timer, stuck_s;
    bool siren, horn, lights;
    float rpm;             // 0..1 for the engine sound
    float skid;            // 0..1 for tyre squeal
} Car;

extern Car cars[MAX_CARS];

int  car_spawn(int model, Vector3 p, float yaw, int ai);
void car_remove(int c);
void car_damage(int c, float dmg, Vector3 at, int attacker);
Matrix car_matrix(const Car *c);
Vector3 car_fwd(const Car *c);
float car_speed(const Car *c);          // signed, m/s along forward
void cars_tick(float dt);
void cars_draw(int pass);
int  car_near(Vector3 p, float r);      // closest car within r, or -1
bool cars_push(Vector3 *p, float radius);  // keep a person out of car bodies, true on contact
void car_drive_to(int c, Vector3 target, float speed_ms, bool reckless); // sets pedals/steer toward a point (cops, fleeing)

// ---- people (ped.c). peds[0] is the player.

#define MAX_PEDS 110
#define PLAYER 0
enum { PED_CIV, PED_COP, PED_PLAYER };
enum { PS_WALK, PS_IDLE, PS_FLEE, PS_COWER, PS_FIGHT, PS_CHASE, PS_DEAD, PS_IN_CAR, PS_ENTERING, PS_EXITING,
       PS_RAGDOLL, PS_CALLING, PS_ARREST, PS_HANDS_UP };

typedef struct {
    bool used;
    int kind, state;
    Vector3 pos, vel;
    float yaw, aim_pitch;
    bool aiming;
    float health, armor;
    int car, seat;               // -1 when on foot
    int body, hair;
    Color pal[R_COUNT];
    int weapon; int ammo[W_COUNT]; int clip_ammo;
    float fire_cd, reload_s;
    int anim, prev_anim; float anim_t, prev_t, blend;  // blend 0..1 into anim
    int node, next_node;         // walking graph
    int target; float timer;
    float panic;
    Vector3 rag[11], rag_prev[11]; // verlet ragdoll points when PS_RAGDOLL / dead
    int cash;
} Ped;

extern Ped peds[MAX_PEDS];
extern Camera3D cam;
void drone_cmd(const char *cmd, const float *v); // fixed or gliding camera for b-roll
void debug_parse(const char *list);
void debug_draw(void);

int  ped_spawn(int kind, Vector3 p, float yaw);
void ped_damage(int i, float dmg, Vector3 impulse, int attacker, int weapon);
void peds_tick(const Input *in, float dt);
void peds_draw(int pass);
void camera_update(const Input *in, float dt);
void ped_pose(const Ped *p, Matrix *bones_out);     // final skinning matrices
Vector3 ped_hand(const Ped *p);                      // right hand, world
void ped_alarm(Vector3 p, float radius, int source); // gunshots, crashes, explosions: people nearby react
void ped_play(int i, int anim);                      // one-shot action (punch, shoot, reload) over the current state
void ped_remove(int i);
int  ped_at(Vector3 p, float r, int ignore);         // closest live ped within r, or -1

// ---- combat (combat.c)

typedef struct { const char *name; float dmg, rate, spread, range; int clip, pellets; float reload_s; } Weapon;
extern const Weapon weapons[W_COUNT];
extern int cash;

void combat_tick(const Input *in, float dt);
void weapon_stow(void);
bool fire_weapon(int ped, Vector3 from, Vector3 dir);  // false if not ready
void explode(Vector3 p, float radius, int attacker);
void pickups_draw(int pass);                           // world items, called by render.c
bool shoot_ray(Vector3 o, Vector3 dir, float maxd, int ignore_ped, int ignore_car, Hit *h); // world + cars + peds

// ---- wanted level and police (cops.c)

enum { CR_ASSAULT, CR_MURDER, CR_SHOTS, CR_CARJACK, CR_CAR_THEFT, CR_HIT_PED, CR_COP_ASSAULT, CR_COP_MURDER,
       CR_ROBBERY, CR_DESTROY_CAR, CR_EVADE };

extern int heat;            // 0..5
extern float heat_search;   // > 0 while cops have lost sight, counts down to losing a level
extern Vector3 last_seen;

void crime(int kind, Vector3 p, int victim);
void cops_tick(float dt);
void cops_reset(void);

// ---- jobs and missions (jobs.c, missions.c)

enum { MK_MISSION, MK_TARGET, MK_SHOP, MK_RESPRAY, MK_EXPORT, MK_STASH, MK_COUNT };
#define MAX_MARKERS 16

void marker_set(int slot, Vector3 p, int kind);   // hud.c shows it on the radar, the map and as a beacon; kind < 0 clears
void hud_objective(const char *text);            // one line of mission text, "" clears
void jobs_tick(const Input *in, float dt);
void missions_tick(const Input *in, float dt);
void heat_clear(void);                           // cops.c: lose the wanted level now (respray)

// ---- effects (fx.c)

enum { FX_SPARK, FX_BLOOD, FX_SMOKE, FX_FIRE, FX_DEBRIS, FX_GLASS, FX_MUZZLE, FX_DUST, FX_EXPLOSION, FX_WATER };
enum { DECAL_BULLET, DECAL_BLOOD, DECAL_SCORCH, DECAL_OIL };

void fx_burst(int kind, Vector3 p, Vector3 dir, int n);
void fx_decal(int kind, Vector3 p, Vector3 n, float size);
void fx_skid(int key, Vector3 p, float width, bool on);    // key = car*4 + wheel
void fx_light(Vector3 p, Vector3 rgb, float radius, float life);
void fx_tracer(Vector3 a, Vector3 b);
void fx_shake(float amount);
void fx_tick(float dt);
void fx_draw(int pass);

// ---- rendering (render.c)

enum { PASS_SHADOW, PASS_MAIN };
enum { MAT_WORLD, MAT_CAR, MAT_PROP };

void render_init(void);
void render_frame(int w, int h);   // scene + post into the current framebuffer
void draw_mesh(Mesh m, Matrix xf, Color tint, int mat);
void draw_char(Mesh m, Matrix xf, const Matrix *bones, const Color *pal);
extern float day_hours;            // 0..24
extern Vector3 sun_dir;

// ---- sound (audio.c)

enum { S_PISTOL, S_SMG, S_SHOTGUN, S_EMPTY, S_RELOAD, S_PUNCH, S_HIT, S_CRASH_SMALL, S_CRASH_BIG, S_GLASS,
       S_EXPLOSION, S_RICOCHET, S_STEP, S_DOOR, S_SCREAM, S_PICKUP, S_CASH, S_BODYFALL, S_UI,
       S_V_FREEZE, S_V_GROUND, S_V_DROP, S_V_PULLOVER, S_V_HANDS, S_V_SHOTS, S_V_ONFOOT, S_V_ARREST,
       S_V_NORTH, S_V_PURSUIT, S_V_LOST, S_V_TAKEIT, S_V_DONTSHOOT, S_V_OUTTA, S_V_PHONE, S_V_NOTODAY,
       S_V_SCREAM, S_V_GRUNT, S_V_MYCAR, S_V_HELP, S_V_COPS, S_V_WHAT, S_V_WATCH, S_V_GUN, S_COUNT };

extern bool audio_offline;            // main.c sets it when recording: then only audio_render produces sound
void audio_init(void);
void sfx(int id, Vector3 p, float vol, float pitch);
void voice(int id, Vector3 p, float vol, int ped);  // a spoken line; ped picks the voice (-1: the player's own)
void audio_tick(float dt);                 // engines, sirens, fire, radio from game state
void audio_render(float *out, int frames); // stereo interleaved, 44100 Hz; used live and by --record

// ---- hud (hud.c)

void hud_draw(int w, int h);
void hud_msg(const char *text, float secs);
void hud_big(const char *text, float secs);  // centre screen, e.g. on death

// ---- main.c

enum { MODE_PLAY, MODE_DEAD, MODE_BUSTED, MODE_PAUSE, MODE_MAP };
extern int mode;
extern float game_s;        // seconds since start
extern uint64_t ticks;

uint32_t rnd(void);
float frand(float a, float b);
void respawn(Vector3 at);

#endif
