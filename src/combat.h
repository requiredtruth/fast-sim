#ifndef COMBAT_H
#define COMBAT_H

// what the hud and the cops need to see of combat.c

#include "game.h"

enum { PK_PISTOL, PK_SMG, PK_SHOTGUN, PK_AMMO, PK_HEALTH, PK_ARMOR, PK_CASH, PK_TYPES };

typedef struct { Vector3 pos; int type; } PickView;

int pickups_view(PickView *out, int max);
void pickup_drop(Vector3 at, int type, int amount);

bool rob_view(float *frac, int *got_cash);   // a robbery is running: register progress and dollars so far

bool give_item(int pk_type);   // the shop sells through this: false if it would do nothing
extern bool shop_open;         // the player is at the gun counter, so NEXT/PREV pick items
int shop_view(int *sel, const char **name, int *price, int max);   // jobs.c, for the hud: 0 when closed

extern float shot_age;          // seconds since the player last fired
extern int aim_target;          // ped under the crosshair, -1 if none
extern bool aim_live;           // the player is aiming a gun this tick
extern Vector3 aim_from, aim_dir;
extern bool map_held;           // BTN_MAP is down, for the hud
extern float mode_since;        // game_s when the current mode began

#endif
