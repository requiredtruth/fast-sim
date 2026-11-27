#ifndef CITY_H
#define CITY_H

// internals shared by world.c, render.c and fx.c. other modules only need light_add.

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include "external/glad.h"   // raylib's own loader, it fills the pointers in InitWindow
#endif
#include "game.h"

// raylib stores matrices field-by-field; MatrixToFloatV gives the column-major order GL wants
static inline void gl_mat(GLint loc, Matrix m) { glUniformMatrix4fv(loc, 1, GL_FALSE, MatrixToFloatV(m).v); }

typedef struct { float p[3], uv[2]; int8_t n[4]; uint8_t c[4]; float layer, seed; } Vtx;

enum { GEO_GROUND, GEO_SOLID, GEO_GLASS, GEO_WATER, GEO_PARTS };

enum {
    L_DETAIL, L_ASPHALT, L_SIDEWALK, L_CURB, L_CONCRETE, L_PANEL, L_BRICK, L_STUCCO, L_GRASS, L_SAND,
    L_GRAVEL, L_TILE, L_SHINGLE, L_WOOD, L_METAL, L_CONTAINER, L_PLAIN, L_QUAY,
    L_WIN_STUCCO, L_WIN_BRICK, L_WIN_CONC, L_WIN_GLASS, L_STOREFRONT, L_WIN_PLAIN,
    L_DOOR, L_GARAGE, L_ROLLER, L_AWNING, L_CHAIN, L_FENCE, L_PALM, L_LEAF, L_BARK, L_PALMBARK,
    L_DASH, L_YELLOW, L_EDGE, L_ZEBRA, L_STOP, L_MANHOLE, L_PARKING, L_CROSS, L_SOLAR,
    L_SIGN0, L_SIGN1, L_SIGN2, L_SIGN3, L_COUNT
};

// vertex flags, stored in c[3]
enum {
    F_WIN = 1,      // texture alpha is the glass mask: lit windows at night, sky reflection by day
    F_CUT = 2,      // alpha tested
    F_SELF = 4,     // glows at night, seed is the strength
    F_GLASSY = 8,   // reflective curtain wall
    F_BLEND = 16,   // sand into grass by seed
    F_DECAL = 32,   // pulled toward the camera so paint sits on the road
    F_INTER = 64,   // shop interior, lit from within
    F_SIGNAL = 128  // traffic lamp, seed = group * 4 + lamp
};

// world.c
extern GLuint world_tex, sig_tex;
extern Vector2 layer_cells[L_COUNT];
void world_cull(Matrix vp, Vector3 center, float radius);
void world_draw_part(int part);
void world_frame(void);
int  world_lamps(Vector3 near, Vector4 *out, int max);
float world_coast(float x, float z);   // metres inland of the shoreline, negative at sea

// render.c
#define MAX_LIGHTS 24
void light_add(Vector3 p, Vector3 rgb, float radius);  // call every frame while the light is wanted
GLuint r_program(const char *vs, const char *fs);
extern Matrix r_view, r_proj, r_vp;
extern Vector3 r_cam_right, r_cam_up;
extern Vector3 r_sun_col, r_amb_col, r_fog_col;
extern float r_night;

// fx.c
float fx_shake_amount(void);
int  fx_lights(Vector3 *p, Vector3 *rgb, float *radius, int max);

#endif
