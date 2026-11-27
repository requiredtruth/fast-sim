#ifndef PED_H
#define PED_H

// internals shared by ped.c, ped_anim.c and camera.c

#include "game.h"

#define SEAT_DROP 0.62f   // seat height in the model to ped origin

typedef struct {
    Vector3 last_pos;
    float speed;                  // measured ground speed, m/s
    float scale;
    float gait, rate, prev_rate;  // locomotion phase in cycles, clip playback rates
    int up_anim; float up_t, up_w;          // upper body layer (gun pose)
    int shot; float shot_t, shot_len; bool shot_full;  // one-shot action, -1 when none
    float pitch;                  // spine pitch while aiming, + is up
    bool grounded, crouch;
    float vy, air_s, land_s, drown_s, hit_cd;
    float flinch_s;               // starts at 0.60, last 0.35 allows free movement
    float walk_speed, side_off;
    int idle_kind;                // 0 stand, 1 talk, 2 phone
    bool wait;                    // standing at a red crosswalk
    Vector3 src;                  // what we are running from
    // cars
    int enter_car; float state_s;
    Vector3 wp[3]; int nwp, wpi;  // entering: waypoints in car space
    float sit; Vector3 sit_from;
    float exit_cd;
    int step_ph;
    // ragdoll
    bool rag_on, rag_sleep;
    int rag_still;
    float rag_s, down_s, dead_s;
    Vector3 side;
    float getup;                  // < 0 when not getting up, else 0..1
    Quaternion gu_rot[MAX_BONES]; Vector3 gu_hips;
    // draw
    Matrix xf, bones[MAX_BONES];
    bool ready;
} Priv;

extern Priv pv[MAX_PEDS];
static inline bool staggering(int i) { return pv[i].flinch_s > 0.35f; }
extern float cam_yaw, cam_pitch;
extern float weapon_reach;         // player hand: 0 ready, 1 at the hip

// ped_anim.c
void anim_to(int i, int a, float t0);
void anim_step(int i, float dt);
void locomote(int i, float speed, bool crouch, bool back);
void ped_build(int i);
void rag_begin(int i, Vector3 impulse, Vector3 shift);
void rag_tick(int i);
void getup_begin(int i);
Vector3 rag_centre(int i);
float rag_yaw(int i);

// for the car module: a driver climbs out and runs
void ped_exit_car(int i, bool panic);

#endif
