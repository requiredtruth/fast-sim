#include "game.h"
#include "rlgl.h"
#include "cops.h"
#include <math.h>
#include <string.h>

#ifdef RELEASE   // the overlays are for the dev build; the 1 MB builds leave them out
void debug_parse(const char *list) {}
void debug_draw(void) {}
#else
enum { D_LANES = 1, D_WALK = 2, D_CONES = 4, D_SEARCH = 8, D_BOXES = 16 };
static int dbg;

void debug_parse(const char *list)
{
    static const char *names[] = { "lanes", "walk", "cones", "search", "boxes" };
    for (int i = 0; i < 5; i++)
        if (strstr(list, names[i])) dbg |= 1 << i;
}

static Vector3 up(Vector3 p, float h) { p.y += h; return p; }

static bool near_cam(Vector3 p) { return Vector3DistanceSqr(p, cam.position) < 150 * 150; }

static void draw_lanes(void)
{
    static const Color col[3] = { { 60, 255, 90, 255 }, { 255, 220, 40, 255 }, { 255, 50, 40, 255 } };
    for (int i = 0; i < nlanes; i++) {
        Lane *l = &lanes[i];
        if (!near_cam(l->b)) continue;
        Color c = l->light >= 0 ? col[light_state(l->light)] : (Color){ 90, 200, 255, 255 };
        Vector3 a = up(l->a, 0.4f), b = up(l->b, 0.4f);
        DrawLine3D(a, b, c);
        Vector3 d = Vector3Normalize(Vector3Subtract(b, a)), s = { -d.z, 0, d.x };
        Vector3 back = Vector3Subtract(b, Vector3Scale(d, 2.5f));
        DrawLine3D(b, Vector3Add(back, Vector3Scale(s, 1.2f)), c);
        DrawLine3D(b, Vector3Subtract(back, Vector3Scale(s, 1.2f)), c);
    }
}

static void walkgraph(void)
{
    for (int i = 0; i < nwalk; i++) {
        WalkNode *n = &walk[i];
        if (!near_cam(n->p)) continue;
        Color c = n->light >= 0 ? (Color){ 255, 90, 220, 255 } : (Color){ 255, 170, 40, 255 };
        for (int k = 0; k < n->n; k++)
            if (n->nb[k] > i) DrawLine3D(up(n->p, 0.3f), up(walk[n->nb[k]].p, 0.3f), c);
        DrawCube(up(n->p, 0.3f), 0.5f, 0.5f, 0.5f, c);
    }
}

static void cones(void)
{
    CopView cv[32];
    int n = cops_view(cv, 32);
    for (int i = 0; i < n; i++) {
        Vector3 o = { cv[i].pos.x, fmaxf(ground_y(cv[i].pos.x, cv[i].pos.z), 0) + 0.2f, cv[i].pos.z };
        if (!near_cam(o)) continue;
        Color c = cv[i].searching ? (Color){ 255, 60, 40, 110 } : (Color){ 255, 220, 60, 60 };
        float a0 = cv[i].yaw - 1.3f, step = 2.6f / 16;
        for (int k = 0; k < 16; k++) {
            float a = a0 + k * step, b = a + step;
            Vector3 p = { o.x + sinf(a) * 70, o.y, o.z + cosf(a) * 70 };
            Vector3 q = { o.x + sinf(b) * 70, o.y, o.z + cosf(b) * 70 };
            DrawTriangle3D(o, q, p, c);
            DrawTriangle3D(o, p, q, c);
        }
    }
}

static void search(void)
{
    if (heat_search <= 0) return;
    float r = 30 + heat_search * 2.5f;
    Vector3 o = up(last_seen, 0.3f);
    for (int k = 0; k < 64; k++) {
        float a = k * 2 * PI / 64, b = (k + 1) * 2 * PI / 64;
        DrawLine3D((Vector3){ o.x + sinf(a) * r, o.y, o.z + cosf(a) * r },
                   (Vector3){ o.x + sinf(b) * r, o.y, o.z + cosf(b) * r }, RED);
    }
    DrawLine3D(o, up(o, 40), RED);
    DrawCube(up(o, 40), 2, 2, 2, RED);
}

static void box_lines(const Vector3 c[8], Color col)
{
    // corner index bits: x=1 y=2 z=4
    for (int i = 0; i < 8; i++)
        for (int b = 1; b < 8; b <<= 1)
            if (!(i & b)) DrawLine3D(c[i], c[i | b], col);
}

static void boxes(void)
{
    BoundingBox bb[256];
    int n = world_boxes(cam.position, 60, bb, 256);
    for (int i = 0; i < n; i++) {
        Vector3 c[8];
        for (int k = 0; k < 8; k++)
            c[k] = (Vector3){ k & 1 ? bb[i].max.x : bb[i].min.x, k & 2 ? bb[i].max.y : bb[i].min.y, k & 4 ? bb[i].max.z : bb[i].min.z };
        box_lines(c, (Color){ 255, 255, 255, 255 });
    }
    for (int i = 0; i < MAX_CARS; i++) {
        Car *car = &cars[i];
        if (!car->used || !near_cam(car->pos)) continue;
        Vector3 s = car_models[car->model].size;
        Matrix m = car_matrix(car);
        Vector3 c[8];
        for (int k = 0; k < 8; k++)
            c[k] = Vector3Transform((Vector3){ k & 1 ? s.x / 2 : -s.x / 2, k & 2 ? s.y : 0, k & 4 ? s.z / 2 : -s.z / 2 }, m);
        box_lines(c, (Color){ 255, 80, 80, 255 });
    }
}

void debug_draw(void)
{
    if (!dbg) return;
    rlDrawRenderBatchActive();
    rlSetClipPlanes(0.25, 2800);
    BeginMode3D(cam);
    rlDisableDepthTest();
    rlDisableBackfaceCulling();
    BeginBlendMode(BLEND_ALPHA);
    if (dbg & D_LANES) draw_lanes();
    if (dbg & D_WALK) walkgraph();
    if (dbg & D_CONES) cones();
    if (dbg & D_SEARCH) search();
    if (dbg & D_BOXES) boxes();
    EndBlendMode();
    EndMode3D();
    rlEnableDepthTest();
}
#endif
