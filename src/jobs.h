#ifndef JOBS_H
#define JOBS_H

// shared between jobs.c and missions.c

#include "game.h"

Vector3 export_pos(void);
Vector3 walt_pos(void);
bool curb_spot(Vector3 near, Vector3 *spot, float *yaw);   // a parking spot on the kerb of the nearest street
bool mission_owns_car(int ci);                              // missions.c: the export yard leaves these alone

#endif
