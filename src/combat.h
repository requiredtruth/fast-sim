#ifndef COMBAT_H
#define COMBAT_H

// what the hud needs to see of combat.c

#include "game.h"

extern float shot_age;          // seconds since the player last fired
extern int aim_target;          // ped under the crosshair, -1 if none
extern bool aim_live;           // the player is aiming a gun this tick
extern Vector3 aim_from, aim_dir;
extern bool map_held;           // BTN_MAP is down, for the hud
extern float mode_since;        // game_s when the current mode began

#endif
