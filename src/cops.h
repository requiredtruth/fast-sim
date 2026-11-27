#ifndef COPS_H
#define COPS_H

// what the hud needs to see of cops.c

#include "game.h"

typedef struct { Vector3 pos; float yaw; bool searching, in_car; } CopView;

int cops_view(CopView *out, int max);

bool cops_heli(Vector3 *pos, Vector3 *vel);   // for the rotor sound
void cops_draw(int pass);                      // called by render.c

#endif
