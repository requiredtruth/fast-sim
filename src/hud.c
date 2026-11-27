#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ped.h"
#include "rlgl.h"
#include "combat.h"
#include "cops.h"

#define MAP_PX 2048
#define MAP_M 1280.0f

static const Color INK = { 238, 240, 244, 255 };
static const Color DIM = { 150, 156, 168, 255 };
static const Color ACCENT = { 255, 153, 51, 255 };
static const Color DANGER = { 232, 64, 56, 255 };
static const Color COPBLUE = { 72, 132, 255, 255 };
static const Color STEEL = { 140, 176, 214, 255 };

static Color alpha(Color c, float a) { c.a = (unsigned char)(c.a * Clamp(a, 0, 1)); return c; }

// ---- a stroke font, drawn with lines so it scales cleanly. glyph points are x y on a 5 by 9 grid, y up

typedef struct { const char *s; int w; } Glyph;

static const Glyph glyphs[128] = {
    ['A'] = { "00 28 40/13 33", 4 },
    ['B'] = { "00 08 38 47 45 34 04/34 43 41 30 00", 4 },
    ['C'] = { "46 38 18 07 01 10 30 42", 4 },
    ['D'] = { "00 08 28 46 42 20 00", 4 },
    ['E'] = { "48 08 00 40/04 34", 4 },
    ['F'] = { "48 08 00/04 34", 4 },
    ['G'] = { "46 38 18 07 01 10 30 41 44 24", 4 },
    ['H'] = { "00 08/40 48/04 44", 4 },
    ['I'] = { "00 20/10 18/08 28", 2 },
    ['J'] = { "48 41 30 10 02", 4 },
    ['K'] = { "00 08/48 04 40", 4 },
    ['L'] = { "08 00 40", 4 },
    ['M'] = { "00 08 24 48 40", 4 },
    ['N'] = { "00 08 40 48", 4 },
    ['O'] = { "10 01 07 18 38 47 41 30 10", 4 },
    ['P'] = { "00 08 38 47 45 34 04", 4 },
    ['Q'] = { "10 01 07 18 38 47 41 30 10/22 40", 4 },
    ['R'] = { "00 08 38 47 45 34 04/24 40", 4 },
    ['S'] = { "47 38 18 07 05 14 34 43 41 30 10 01", 4 },
    ['T'] = { "08 48/28 20", 4 },
    ['U'] = { "08 01 10 30 41 48", 4 },
    ['V'] = { "08 20 48", 4 },
    ['W'] = { "08 10 25 30 48", 4 },
    ['X'] = { "00 48/08 40", 4 },
    ['Y'] = { "08 24 48/24 20", 4 },
    ['Z'] = { "08 48 00 40", 4 },
    ['0'] = { "10 01 07 18 38 47 41 30 10/13 35", 4 },
    ['1'] = { "16 28 20/00 40", 4 },
    ['2'] = { "07 18 38 47 45 00 40", 4 },
    ['3'] = { "07 18 38 47 45 34 14/34 43 41 30 10 01", 4 },
    ['4'] = { "30 38 03 43", 4 },
    ['5'] = { "48 08 05 35 44 41 30 10 01", 4 },
    ['6'] = { "47 38 18 07 01 10 30 41 43 34 04", 4 },
    ['7'] = { "08 48 20", 4 },
    ['8'] = { "14 03 01 10 30 41 43 34 14 05 07 18 38 47 45 34", 4 },
    ['9'] = { "01 10 30 41 47 38 18 07 05 14 44", 4 },
    ['.'] = { "00 00", 1 },
    [','] = { "10 01", 2 },
    [':'] = { "01 01/06 06", 1 },
    ['!'] = { "00 00/02 08", 1 },
    ['?'] = { "07 18 38 47 45 24 23/20 20", 4 },
    ['-'] = { "03 33", 3 },
    ['+'] = { "04 44/22 26", 4 },
    ['/'] = { "00 48", 4 },
    ['%'] = { "06 07 17 16 06/33 34 44 43 33/00 48", 4 },
    ['$'] = { "46 37 17 06 05 14 34 43 42 31 11 02/20 21/27 28", 4 },
    ['('] = { "20 11 17 28", 2 },
    [')'] = { "00 11 17 08", 2 },
    ['\''] = { "06 08", 1 },
    ['"'] = { "06 08/26 28", 3 },
    ['='] = { "03 43/05 45", 4 },
    ['<'] = { "42 04 46", 4 },
    ['>'] = { "02 44 06", 4 },
    ['_'] = { "00 40", 4 },
    [' '] = { "", 3 },
};

static float glyph_adv(unsigned char ch, float sc)
{
    const Glyph *g = &glyphs[ch & 127];
    return ((g->s ? g->w : 3) + 2.0f) * sc;
}

static float text_w(const char *s, float size)
{
    float sc = size / 8, w = 0;
    for (; *s; s++) w += glyph_adv(toupper((unsigned char)*s), sc);
    return w - 2.0f * sc;
}

static void draw_glyph(const Glyph *g, float x, float y, float sc, float th, Color c)
{
    if (!g->s) return;
    bool round = th >= 3 && c.a == 255;
    const char *p = g->s;
    Vector2 prev = { 0 };
    bool have = false;
    while (*p) {
        if (*p == '/') { have = false; p++; continue; }
        if (*p == ' ') { p++; continue; }
        float gx = p[0] - '0', gy = p[1] - '0';
        p += 2;
        Vector2 q = { x + gx * sc, y + (8 - gy) * sc };
        if (have) {
            if (q.x == prev.x && q.y == prev.y) DrawPoly(q, 8, th * 0.55f, 0, c);
            else DrawLineEx(prev, q, th, c);
            if (round) DrawPoly(q, 8, th * 0.5f, 0, c);
        } else if (round) DrawPoly(q, 8, th * 0.5f, 0, c);
        prev = q;
        have = true;
    }
}

// y is the top of the capitals. align: 0 left, 1 centre, 2 right
static void text(const char *s, float x, float y, float size, Color c, int align)
{
    float sc = size / 8, th = fmaxf(1.15f, size * 0.115f);
    float w = text_w(s, size);
    if (align == 1) x -= w / 2; else if (align == 2) x -= w;
    for (int pass = 0; pass < 2; pass++) {
        float ox = pass == 0 ? fmaxf(1, size * 0.07f) : 0, oy = ox;
        Color col = pass == 0 ? (Color){ 0, 0, 0, (unsigned char)(c.a * 0.55f) } : c;
        float cx = x + ox;
        for (const char *q = s; *q; q++) {
            unsigned char ch = (unsigned char)toupper((unsigned char)*q);
            draw_glyph(&glyphs[ch & 127], cx, y + oy, sc, th, col);
            cx += glyph_adv(ch, sc);
        }
    }
}

// ---- messages

typedef struct { char t[56]; float born, until; } Msg;
static Msg msgs[4];
static int nmsg;
static char big_t[40];
static float big_until, big_born;

void hud_msg(const char *t, float secs)
{
    if (!t || !*t) return;
    if (nmsg > 0 && strncmp(msgs[nmsg - 1].t, t, 6) == 0 && game_s - msgs[nmsg - 1].born < 2.5f) {
        snprintf(msgs[nmsg - 1].t, sizeof msgs[0].t, "%s", t);
        msgs[nmsg - 1].until = game_s + secs;
        return;
    }
    if (nmsg == 4) { memmove(msgs, msgs + 1, 3 * sizeof msgs[0]); nmsg = 3; }
    snprintf(msgs[nmsg].t, sizeof msgs[0].t, "%s", t);
    msgs[nmsg].born = game_s;
    msgs[nmsg].until = game_s + secs;
    nmsg++;
}

void hud_big(const char *t, float secs)
{
    snprintf(big_t, sizeof big_t, "%s", t);
    big_born = game_s;
    big_until = game_s + secs;
}

// ---- the map: roads, buildings and water baked once into a texture

static Texture2D map_tex;
static bool map_ok;

static void build_map(void)
{
    Color land = { 31, 35, 43, 255 }, water = { 17, 30, 46, 255 }, bld = { 54, 59, 71, 255 };
    Color road = { 92, 99, 112, 255 }, casing = { 14, 16, 21, 255 };
    Image img = GenImageColor(MAP_PX, MAP_PX, land);
    Color *px = img.data;
    float k = MAP_PX / MAP_M;
    for (int y = 0; y < MAP_PX; y += 2)
        for (int x = 0; x < MAP_PX; x += 2) {
            if (!is_water((x + 1) / k - MAP_M / 2, (y + 1) / k - MAP_M / 2)) continue;
            px[y * MAP_PX + x] = px[y * MAP_PX + x + 1] = px[(y + 1) * MAP_PX + x] = px[(y + 1) * MAP_PX + x + 1] = water;
        }
    static BoundingBox bb[200];
    for (float gz = -MAP_M / 2; gz < MAP_M / 2; gz += 28)
        for (float gx = -MAP_M / 2; gx < MAP_M / 2; gx += 28) {
            int n = world_boxes((Vector3){ gx + 14, 0, gz + 14 }, 22, bb, 200);
            for (int i = 0; i < n; i++) {
                float dx = bb[i].max.x - bb[i].min.x, dz = bb[i].max.z - bb[i].min.z;
                if (bb[i].max.y < 3.2f || dx * dz < 20) continue;
                ImageDrawRectangle(&img, (int)((bb[i].min.x + MAP_M / 2) * k), (int)((bb[i].min.z + MAP_M / 2) * k),
                                   (int)(dx * k) + 1, (int)(dz * k) + 1, bld);
            }
        }
    for (int pass = 0; pass < 2; pass++)
        for (int l = 0; l < nlanes; l++) {
            Vector2 a = { (lanes[l].a.x + MAP_M / 2) * k, (lanes[l].a.z + MAP_M / 2) * k };
            Vector2 b = { (lanes[l].b.x + MAP_M / 2) * k, (lanes[l].b.z + MAP_M / 2) * k };
            ImageDrawLineEx(&img, a, b, pass ? 8 : 12, pass ? road : casing);
        }
    map_tex = LoadTextureFromImage(img);
    UnloadImage(img);
    GenTextureMipmaps(&map_tex);
    SetTextureFilter(map_tex, TEXTURE_FILTER_TRILINEAR);
    map_ok = true;
}

typedef struct { Vector2 c; float s; Vector3 w0, fwd, right; } View;   // s is pixels per metre

static Vector2 to_screen(const View *v, Vector3 p)
{
    Vector3 d = Vector3Subtract(p, v->w0);
    return (Vector2){ v->c.x + (d.x * v->right.x + d.z * v->right.z) * v->s, v->c.y - (d.x * v->fwd.x + d.z * v->fwd.z) * v->s };
}

static Vector2 map_uv(const View *v, Vector2 s)
{
    float f = (v->c.y - s.y) / v->s, r = (s.x - v->c.x) / v->s;
    Vector3 w = Vector3Add(v->w0, Vector3Add(Vector3Scale(v->fwd, f), Vector3Scale(v->right, r)));
    return (Vector2){ (w.x + MAP_M / 2) / MAP_M, (w.z + MAP_M / 2) / MAP_M };
}

static void map_vertex(const View *v, Vector2 s)
{
    Vector2 uv = map_uv(v, s);
    rlTexCoord2f(uv.x, uv.y);
    rlVertex2f(s.x, s.y);
}

static void map_disc(const View *v, float r)
{
    rlSetTexture(map_tex.id);
    rlBegin(RL_TRIANGLES);
    rlColor4ub(255, 255, 255, 255);
    int n = 72;
    for (int i = 0; i < n; i++) {
        float a0 = i * 2 * PI / n, a1 = (i + 1) * 2 * PI / n;
        map_vertex(v, v->c);
        map_vertex(v, (Vector2){ v->c.x + sinf(a1) * r, v->c.y - cosf(a1) * r });
        map_vertex(v, (Vector2){ v->c.x + sinf(a0) * r, v->c.y - cosf(a0) * r });
    }
    rlEnd();
    rlSetTexture(0);
}

static void map_rect(const View *v, Rectangle r)
{
    rlSetTexture(map_tex.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    map_vertex(v, (Vector2){ r.x, r.y });
    map_vertex(v, (Vector2){ r.x, r.y + r.height });
    map_vertex(v, (Vector2){ r.x + r.width, r.y + r.height });
    map_vertex(v, (Vector2){ r.x + r.width, r.y });
    rlEnd();
    rlSetTexture(0);
}

// ---- shapes

static void shield(Vector2 c, float w, float h, Color fill, Color line, bool filled)
{
    Vector2 p[5] = { { c.x - w, c.y - h }, { c.x + w, c.y - h }, { c.x + w, c.y + h * 0.1f }, { c.x, c.y + h }, { c.x - w, c.y + h * 0.1f } };
    if (filled) {
        for (int i = 0; i < 5; i++) DrawTriangle(c, p[(i + 1) % 5], p[i], fill);
        DrawTriangle(p[0], p[1], p[4], fill);
    }
    float th = fmaxf(1.2f, h * 0.1f);
    for (int i = 0; i < 5; i++) DrawLineEx(p[i], p[(i + 1) % 5], th, line);
}

static void cross(Vector2 c, float r, float th, Color col)
{
    DrawLineEx((Vector2){ c.x - r, c.y }, (Vector2){ c.x + r, c.y }, th, col);
    DrawLineEx((Vector2){ c.x, c.y - r }, (Vector2){ c.x, c.y + r }, th, col);
}

static void diamond(Vector2 c, float r, Color col)
{
    DrawTriangle((Vector2){ c.x - r, c.y }, (Vector2){ c.x, c.y + r }, (Vector2){ c.x, c.y - r }, col);
    DrawTriangle((Vector2){ c.x + r, c.y }, (Vector2){ c.x, c.y - r }, (Vector2){ c.x, c.y + r }, col);
}

static void bar(Rectangle r, float v, Color fill)
{
    DrawRectangleRec(r, (Color){ 8, 10, 14, 150 });
    float w = r.width * Clamp(v, 0, 1);
    if (w > 0) DrawRectangle((int)r.x, (int)r.y, (int)w, (int)r.height, fill);
    DrawRectangleLinesEx(r, 1, (Color){ 255, 255, 255, 50 });
}

static void thousands(int v, char *out, size_t n)
{
    char t[24];
    snprintf(t, sizeof t, "%d", v < 0 ? -v : v);
    int len = (int)strlen(t), o = 0;
    char buf[40];
    if (v < 0) buf[o++] = '-';
    for (int i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) buf[o++] = ',';
        buf[o++] = t[i];
    }
    buf[o] = 0;
    snprintf(out, n, "$%s", buf);
}

// ---- districts, by block of the city

static const char *district(Vector3 p)
{
    static const char *layout[9] = {
        "RRRRRRRRB", "RRRRRRRRB", "RPSSRRRRB", "RRSDDDHSB", "RRXDDDSRB", "SSSDDDSSB", "SSSSSSSSB", "IIRRPRRRB", "IIIIIIBBB",
    };
    if (is_water(p.x, p.z)) return "Palmera Bay";
    int i = (int)floorf((p.x + 608) / 114.7f), j = (int)floorf((p.z + 608) / 114.7f);
    i = i < 0 ? 0 : i > 8 ? 8 : i;
    j = j < 0 ? 0 : j > 8 ? 8 : j;
    switch (layout[j][i]) {
    case 'B': return "Palmera Beach";
    case 'I': return "Harbor Row";
    case 'D': return "Downtown";
    case 'H': return "Mercy Heights";
    case 'X': return "Civic Center";
    case 'P': return "Fairmount Park";
    case 'S': return i < 3 ? "Old Quarter" : "Market Row";
    default: return j < 2 ? "Cedar Hollow" : j >= 7 ? "Sunset Terrace" : i < 2 ? "Westgate" : "Orchard Hill";
    }
}


// ---- markers and the objective line

static struct { Vector3 p; int kind; } marks[MAX_MARKERS];   // kind is stored plus one, 0 is empty
static char objective[72];
static const char mk_glyph[MK_COUNT][2] = { "M", "", "G", "R", "E", "?" };
static const Color mk_col[MK_COUNT] = {
    { 255, 153, 51, 255 }, { 255, 214, 64, 255 }, { 120, 200, 140, 255 },
    { 190, 120, 230, 255 }, { 80, 190, 230, 255 }, { 238, 240, 244, 255 },
};

void marker_set(int slot, Vector3 p, int kind)
{
    if (slot < 0 || slot >= MAX_MARKERS) return;
    marks[slot].p = p;
    marks[slot].kind = kind >= 0 && kind < MK_COUNT ? kind + 1 : 0;
}

void hud_objective(const char *t)
{
    snprintf(objective, sizeof objective, "%s", t ? t : "");
}

static void blip(Vector2 p, int kind, float r, float S)
{
    DrawCircleV(p, r + 1.5f * S, (Color){ 8, 10, 14, 235 });
    if (kind == MK_TARGET) { diamond(p, r * 0.9f, mk_col[kind]); return; }
    DrawCircleV(p, r, mk_col[kind]);
    text(mk_glyph[kind], p.x, p.y - r * 0.55f, r * 1.1f, (Color){ 12, 14, 18, 255 }, 1);
}

static void beacons(int w, int h, float S)
{
    Ped *pl = &peds[PLAYER];
    Vector3 pp = pl->car >= 0 && cars[pl->car].used ? cars[pl->car].pos : pl->pos;
    Vector3 f = Vector3Subtract(cam.target, cam.position);
    for (int i = 0; i < MAX_MARKERS; i++) {
        int kind = marks[i].kind - 1;
        if (kind < 0) continue;
        float d = Vector3Distance(pp, marks[i].p);
        if (kind >= MK_SHOP && kind != MK_STASH && d > 220) continue;
        if (kind == MK_STASH && d > 12) continue;
        Vector3 top = Vector3Add(marks[i].p, (Vector3){ 0, 3.2f + 0.25f * sinf(game_s * 3 + i), 0 });
        if (Vector3DotProduct(Vector3Subtract(top, cam.position), f) < 1) continue;
        Vector2 s = GetWorldToScreenEx(top, cam, w, h);
        if (s.x < 0 || s.x > w || s.y < 0 || s.y > h) continue;
        float r = Clamp(14 * S * 60 / fmaxf(d, 30), 6 * S, 14 * S);
        Color c = mk_col[kind];
        diamond(s, r + 2 * S, (Color){ 8, 10, 14, 220 });
        diamond(s, r, c);
        char n[16];
        snprintf(n, sizeof n, "%d M", (int)d);
        text(n, s.x, s.y + r + 6 * S, 13 * S, c, 1);
    }
}

static void shop_panel(int w, int h, float S)
{
    int sel;
    const char *name[8];
    int price[8];
    int n = shop_view(&sel, name, price, 8);
    if (!n) return;
    float pw = 330 * S, row = 38 * S, ph = (n + 2) * row + 20 * S;
    float x = w * 0.5f - pw / 2, y = h * 0.5f - ph / 2;
    DrawRectangle((int)x, (int)y, (int)pw, (int)ph, (Color){ 6, 8, 12, 215 });
    DrawRectangle((int)x, (int)y, (int)(3 * S), (int)ph, ACCENT);
    text("PAWN KINGS", x + 18 * S, y + 14 * S, 20 * S, INK, 0);
    for (int i = 0; i < n; i++) {
        float ry = y + 10 * S + (i + 1) * row + 8 * S;
        bool on = i == sel, can = cash >= price[i];
        if (on) DrawRectangle((int)(x + 8 * S), (int)(ry - 6 * S), (int)(pw - 16 * S), (int)(row - 4 * S), (Color){ 255, 153, 51, 50 });
        char p[16];
        snprintf(p, sizeof p, "$%d", price[i]);
        text(name[i], x + 22 * S, ry, 17 * S, on ? INK : DIM, 0);
        text(p, x + pw - 20 * S, ry, 17 * S, can ? (on ? INK : DIM) : DANGER, 2);
    }
    text("NEXT PREV PICK. ENTER BUYS", x + pw / 2, y + ph - 30 * S, 11 * S, DIM, 1);
}

static void objective_line(int w, int h, float S)
{
    if (!objective[0]) return;
    float M = fmaxf(18, 30 * S), sz = 20 * S;
    float tw = text_w(objective, sz);
    DrawRectangle((int)(w / 2 - tw / 2 - 14 * S), (int)(M - 4 * S), (int)(tw + 28 * S), (int)(sz + 14 * S), (Color){ 6, 8, 12, 150 });
    text(objective, w / 2.0f, M + 2 * S, sz, INK, 1);
}

// ---- the radar

static float zoom_m = 90;

static void radar(int w, int h, float S)
{
    if (!map_ok && nlanes > 0) build_map();
    Ped *pl = &peds[PLAYER];
    Vector3 pp = pl->car >= 0 && cars[pl->car].used ? cars[pl->car].pos : pl->pos;
    float R = 132 * S, M = fmaxf(18, 30 * S);
    Vector2 c = { M + R + 4 * S, h - M - R - 2 * S };
    Vector3 f = Vector3Subtract(cam.target, cam.position);
    f.y = 0;
    f = Vector3Length(f) > 0.01f ? Vector3Normalize(f) : (Vector3){ 0, 0, 1 };
    float spd = pl->car >= 0 ? fabsf(car_speed(&cars[pl->car])) : 0;
    zoom_m += (100 + Clamp(spd, 0, 36) * 1.6f - zoom_m) * 0.06f;
    View v = { c, R / zoom_m, pp, f, { -f.z, 0, f.x } };

    DrawCircleV(c, R + 3 * S, (Color){ 6, 8, 12, 200 });
    if (map_ok) map_disc(&v, R);
    else DrawCircleV(c, R, (Color){ 31, 35, 43, 255 });

    // everything below is clipped to the disc by hand
    #define INSIDE(p, slack) (Vector2Distance(p, c) < R - (slack))
    float t = game_s;

    for (int i = 0; i < nshops; i++) {
        Vector2 p = to_screen(&v, shops[i].door);
        if (!INSIDE(p, 8 * S)) continue;
        bool cool = game_s - shops[i].robbed_at < 300;
        text("$", p.x, p.y - 5 * S, 10 * S, cool ? alpha(DIM, 0.7f) : ACCENT, 1);
    }
    PickView pk[96];
    int np = pickups_view(pk, 96);
    for (int i = 0; i < np; i++) {
        Vector2 p = to_screen(&v, pk[i].pos);
        if (!INSIDE(p, 6 * S) || pk[i].type == PK_CASH) continue;
        if (pk[i].type == PK_HEALTH) cross(p, 3.2f * S, 1.8f * S, INK);
        else if (pk[i].type == PK_ARMOR) cross(p, 3.2f * S, 1.8f * S, STEEL);
        else diamond(p, 3.2f * S, alpha(INK, 0.85f));
    }
    // places that matter hug the rim when they are off the radar
    Vector3 places[2] = { police_pos, hospital_pos };
    for (int i = 0; i < 2; i++) {
        Vector2 p = to_screen(&v, places[i]);
        float d = Vector2Distance(p, c);
        if (d > R - 10 * S) p = Vector2Add(c, Vector2Scale(Vector2Subtract(p, c), (R - 10 * S) / d));
        DrawCircleV(p, 7 * S, (Color){ 8, 10, 14, 230 });
        if (i == 0) shield(p, 4 * S, 4.6f * S, COPBLUE, COPBLUE, true);
        else cross(p, 4 * S, 2.4f * S, INK);
    }

    for (int i = 0; i < MAX_MARKERS; i++) {
        int kind = marks[i].kind - 1;
        if (kind < 0 || (kind == MK_STASH && Vector3Distance(pp, marks[i].p) > 30)) continue;
        Vector2 p = to_screen(&v, marks[i].p);
        float d = Vector2Distance(p, c);
        if (d > R - 10 * S) p = Vector2Add(c, Vector2Scale(Vector2Subtract(p, c), (R - 10 * S) / d));
        blip(p, kind, 5.5f * S, S);
    }

    CopView cv[40];
    int nc = cops_view(cv, 40);
    if (heat_search > 0) {
        Vector2 sc = to_screen(&v, last_seen);
        float rad = (30 + heat_search * 2.5f) * v.s;
        float d = Vector2Distance(sc, c);
        if (d < R + rad) {
            // draw the zone as a ring of short arcs that stay inside the disc
            for (int i = 0; i < 48; i++) {
                float a = i * 2 * PI / 48;
                Vector2 p = { sc.x + sinf(a) * rad, sc.y - cosf(a) * rad };
                if (INSIDE(p, 3 * S)) DrawCircleV(p, 1.6f * S, alpha(ACCENT, 0.8f));
            }
        }
        for (int i = 0; i < nc; i++) {
            if (!cv[i].searching) continue;
            Vector2 o = to_screen(&v, cv[i].pos);
            Color col = alpha(i % 2 ? COPBLUE : DANGER, 0.22f);
            float yaw = cv[i].yaw;
            Vector2 pts[6];
            for (int k = 0; k < 6; k++) {
                float a = yaw + (k - 2.5f) / 2.5f * 0.9f;
                Vector3 wp = Vector3Add(cv[i].pos, (Vector3){ sinf(a) * 38, 0, cosf(a) * 38 });
                pts[k] = to_screen(&v, wp);
                float dd = Vector2Distance(pts[k], c);
                if (dd > R - 2 * S) pts[k] = Vector2Add(c, Vector2Scale(Vector2Subtract(pts[k], c), (R - 2 * S) / dd));
            }
            if (!INSIDE(o, 0)) continue;
            for (int k = 0; k < 5; k++) DrawTriangle(o, pts[k + 1], pts[k], col);
        }
    }
    for (int i = 0; i < nc; i++) {
        Vector2 p = to_screen(&v, cv[i].pos);
        if (!INSIDE(p, 3 * S)) continue;
        bool on = fmodf(t * 3 + i * 0.37f, 1) < 0.55f;
        Color col = (i + (int)(t * 3)) % 2 ? DANGER : COPBLUE;
        DrawCircleV(p, 4.6f * S, (Color){ 6, 8, 12, 220 });
        DrawCircleV(p, 3.2f * S, on ? col : alpha(col, 0.45f));
    }

    // the player, pointing where the body points
    float yaw = pl->car >= 0 && cars[pl->car].used ? atan2f(car_fwd(&cars[pl->car]).x, car_fwd(&cars[pl->car]).z) : pl->yaw;
    float rel = atan2f(f.x, f.z) - yaw;
    Vector2 a = { c.x + sinf(rel) * 9 * S, c.y - cosf(rel) * 9 * S };
    Vector2 b = { c.x + sinf(rel + 2.5f) * 7 * S, c.y - cosf(rel + 2.5f) * 7 * S };
    Vector2 d = { c.x + sinf(rel - 2.5f) * 7 * S, c.y - cosf(rel - 2.5f) * 7 * S };
    DrawTriangle(a, d, b, INK);
    DrawTriangleLines(a, d, b, (Color){ 0, 0, 0, 200 });

    DrawRing(c, R - 1, R + 2.5f * S, 0, 360, 72, (Color){ 238, 240, 244, 200 });
    // north
    Vector2 np_ = to_screen(&v, (Vector3){ pp.x, 0, pp.z + 1000 });
    Vector2 dir = Vector2Normalize(Vector2Subtract(np_, c));
    Vector2 npos = Vector2Add(c, Vector2Scale(dir, R + 11 * S));
    text("N", npos.x, npos.y - 5 * S, 10 * S, alpha(INK, 0.8f), 1);

    // health and armour to the right of the radar
    float bx = c.x + R + 18 * S, bw = 190 * S, bh = 9 * S;
    float hp = pl->health / 100, ar = pl->armor / 100;
    float hy = c.y + R * 0.18f, ay = hy + 38 * S;
    bool low = pl->health < 30;
    Color hc = low && fmodf(game_s * 3, 1) < 0.5f ? DANGER : ACCENT;
    char num[16];
    text("HEALTH", bx, hy - 17 * S, 11 * S, DIM, 0);
    snprintf(num, sizeof num, "%d", (int)ceilf(fmaxf(pl->health, 0)));
    text(num, bx + bw, hy - 17 * S, 11 * S, INK, 2);
    bar((Rectangle){ bx, hy, bw, bh }, hp, hc);
    text("ARMOR", bx, ay - 17 * S, 11 * S, DIM, 0);
    snprintf(num, sizeof num, "%d", (int)ceilf(pl->armor));
    text(num, bx + bw, ay - 17 * S, 11 * S, pl->armor > 0 ? INK : DIM, 2);
    bar((Rectangle){ bx, ay, bw, bh }, ar, STEEL);
    #undef INSIDE
}

// ---- top right: heat, money, weapon

static int shown_cash = -1, last_cash, gain;
static float shown_cash_f;
static float gain_t;

static void status(int w, int h, float S)
{
    Ped *pl = &peds[PLAYER];
    float M = fmaxf(18, 30 * S), x = w - M, y = M;
    // heat shields
    float sw = 12 * S, sh = 14 * S, gap = 8 * S;
    bool flash = heat_search > 0 && fmodf(game_s * 2.5f, 1) < 0.5f;
    for (int i = 0; i < 5; i++) {
        Vector2 c = { x - sw - (4 - i) * (2 * sw + gap), y + sh };
        bool on = i < heat;
        if (on && flash) shield(c, sw, sh, ACCENT, ACCENT, false);
        else if (on) shield(c, sw, sh, ACCENT, ACCENT, true);
        else shield(c, sw, sh, ACCENT, (Color){ 255, 255, 255, 70 }, false);
    }
    y += 2 * sh + 18 * S;

    // money
    if (shown_cash < 0) { shown_cash = last_cash = cash; shown_cash_f = cash; }
    if (cash != last_cash) {
        if (cash > last_cash) { gain = game_s - gain_t < 1.6f ? gain + cash - last_cash : cash - last_cash; gain_t = game_s; }
        last_cash = cash;
    }
    // exponential catch-up off game_s, about 0.8 s to settle
    static float cash_t = -1;
    float cdt = cash_t < 0 ? 0 : Clamp(game_s - cash_t, 0, 0.25f);
    cash_t = game_s;
    float cdiff = cash - shown_cash_f;
    shown_cash_f += cdiff * (1 - expf(-cdt * 6));
    if (fabsf(cash - shown_cash_f) < 0.5f) shown_cash_f = cash;
    shown_cash = (int)roundf(shown_cash_f);
    char buf[32];
    thousands(shown_cash, buf, sizeof buf);
    float msize = 38 * S;
    text(buf, x, y, msize, INK, 2);
    if (game_s - gain_t < 1.6f && gain > 0) {
        char g[32];
        thousands(gain, g + 1, sizeof g - 1);
        g[0] = '+';
        float a = Clamp((1.6f - (game_s - gain_t)) / 0.5f, 0, 1);
        text(g, x, y + msize + 6 * S, 17 * S, alpha(ACCENT, a), 2);
    }
    y += msize + 34 * S;

    // weapon
    const Weapon *wp = &weapons[pl->weapon];
    float tw = 0;
    char ammo[32] = "";
    if (pl->weapon != W_FIST) {
        snprintf(ammo, sizeof ammo, "%d", pl->clip_ammo);
        char res[16];
        snprintf(res, sizeof res, "/%d", pl->ammo[pl->weapon]);
        float rs = 17 * S;
        text(res, x, y + 8 * S, rs, DIM, 2);
        tw = text_w(res, rs) + 6 * S;
        text(ammo, x - tw, y, 26 * S, pl->clip_ammo == 0 ? DANGER : INK, 2);
        tw += text_w(ammo, 26 * S) + 14 * S;
    }
    text(wp->name, x - tw, y + 8 * S, 15 * S, DIM, 2);
    if (pl->reload_s > 0) {
        float k = 1 - pl->reload_s / wp->reload_s;
        bar((Rectangle){ x - 120 * S, y + 36 * S, 120 * S, 4 * S }, k, ACCENT);
    }
}

// ---- messages, district, crosshair, speedometer

static char dist_name[24];
static float dist_t = -10;

static void messages(int w, int h, float S)
{
    for (int i = nmsg - 1; i >= 0; i--) {
        Msg *m = &msgs[i];
        float left = m->until - game_s;
        if (left <= 0) { memmove(m, m + 1, (nmsg - i - 1) * sizeof *m); nmsg--; continue; }
    }
    float base = h - 190 * S;
    for (int i = nmsg - 1, k = 0; i >= 0; i--, k++) {
        Msg *m = &msgs[i];
        float left = m->until - game_s, age = game_s - m->born;
        float a = Clamp(fminf(left / 0.4f, age / 0.12f + 0.3f), 0, 1) * (k ? 0.65f : 1);
        float size = 26 * S;
        text(m->t, w * 0.5f, base - k * 34 * S + (1 - Clamp(age / 0.15f, 0, 1)) * 6 * S, size, alpha(INK, a), 1);
    }
}

static void district_banner(int w, int h, float S)
{
    Ped *pl = &peds[PLAYER];
    Vector3 pp = pl->car >= 0 && cars[pl->car].used ? cars[pl->car].pos : pl->pos;
    const char *n = district(pp);
    if (strcmp(n, dist_name)) {
        if (game_s - dist_t > 0.5f || !dist_name[0]) dist_t = game_s;
        if (dist_name[0] || game_s > 1) dist_t = game_s;
        snprintf(dist_name, sizeof dist_name, "%s", n);
    }
    float age = game_s - dist_t;
    if (age > 4.5f || age < 0) return;
    float a = Clamp(fminf(age / 0.35f, (4.5f - age) / 0.8f), 0, 1);
    float M = fmaxf(18, 30 * S);
    float xo = (1 - Clamp(age / 0.35f, 0, 1)) * -18 * S;
    DrawRectangle((int)(M + xo), (int)(M + 2 * S), (int)(3 * S), (int)(34 * S), alpha(ACCENT, a));
    text(dist_name, M + 14 * S + xo, M + 4 * S, 30 * S, alpha(INK, a), 0);
}

static void crosshair(int w, int h, float S)
{
    Ped *pl = &peds[PLAYER];
    if (mode != MODE_PLAY || !pl->aiming || pl->car >= 0) return;
    float rf; int rcash;
    if (rob_view(&rf, &rcash)) {
        float bw = 90 * S, by = h / 2.0f + 36 * S;
        char b[24];
        snprintf(b, sizeof b, "$%d", rcash);
        bar((Rectangle){ w / 2.0f - bw / 2, by, bw, 6 * S }, rf, ACCENT);
        text(b, w / 2.0f, by + 12 * S, 16 * S, INK, 1);
    }
    bool over = aim_target >= 0 && peds[aim_target].used && peds[aim_target].state != PS_DEAD;
    Color c = over ? DANGER : INK;
    float cx = w / 2.0f, cy = h / 2.0f, g = 7 * S, l = 8 * S, th = fmaxf(1.5f, 2 * S);
    Color shadow = { 0, 0, 0, 120 };
    for (int pass = 0; pass < 2; pass++) {
        float o = pass == 0 ? 1.2f : 0;
        Color col = pass == 0 ? shadow : c;
        DrawLineEx((Vector2){ cx - g - l + o, cy + o }, (Vector2){ cx - g + o, cy + o }, th, col);
        DrawLineEx((Vector2){ cx + g + o, cy + o }, (Vector2){ cx + g + l + o, cy + o }, th, col);
        DrawLineEx((Vector2){ cx + o, cy - g - l + o }, (Vector2){ cx + o, cy - g + o }, th, col);
        DrawLineEx((Vector2){ cx + o, cy + g + o }, (Vector2){ cx + o, cy + g + l + o }, th, col);
        DrawCircleV((Vector2){ cx + o, cy + o }, fmaxf(1.4f, 1.6f * S), col);
    }
}

static void speedo(int w, int h, float S)
{
    Ped *pl = &peds[PLAYER];
    if (pl->car < 0 || !cars[pl->car].used) return;
    Car *c = &cars[pl->car];
    float M = fmaxf(18, 30 * S), x = w - M, y = h - M;
    int kmh = (int)(fabsf(car_speed(c)) * 3.6f + 0.5f);
    char b[16];
    snprintf(b, sizeof b, "%d", kmh);
    float size = 64 * S;
    text("KM/H", x, y - 22 * S, 14 * S, DIM, 2);
    text(b, x - text_w("KM/H", 14 * S) - 14 * S, y - size - 2 * S, size, INK, 2);
    float hk = Clamp(c->health / 1000, 0, 1);
    Color col = hk < 0.35f ? (fmodf(game_s * 4, 1) < 0.5f ? DANGER : alpha(DANGER, 0.5f)) : ACCENT;
    bar((Rectangle){ x - 150 * S, y - 12 * S, 150 * S, 6 * S }, hk, col);
    if (c->health < 150) text("ENGINE FIRE", x, y - size - 28 * S, 13 * S, fmodf(game_s * 4, 1) < 0.5f ? DANGER : DIM, 2);
}

// ---- full-screen states

static float mode_t;

static void end_card(int w, int h, float S)
{
    float age = game_s - mode_t;
    float a = Clamp(age / 1.2f, 0, 1);
    DrawRectangle(0, 0, w, h, (Color){ 4, 5, 8, (unsigned char)(170 * a) });
    bool dead = mode == MODE_DEAD;
    const char *title = dead ? "FLATLINED" : "ARRESTED";
    float k = Clamp((age - 0.35f) / 0.5f, 0, 1);
    float size = 120 * S;
    float ty = h * 0.42f - size / 2;
    text(title, w / 2.0f, ty + (1 - k) * 14 * S, size, alpha(INK, k), 1);
    float lw = text_w(title, size);
    DrawRectangle((int)(w / 2 - lw / 2), (int)(ty + size + 18 * S), (int)(lw * k), (int)(4 * S), alpha(ACCENT, k));
    char fee[32];
    snprintf(fee, sizeof fee, "-$%d", dead ? 500 : 250);
    float k2 = Clamp((age - 0.9f) / 0.5f, 0, 1);
    text(fee, w / 2.0f, ty + size + 44 * S, 38 * S, alpha(ACCENT, k2), 1);
    text(dead ? "Patched up at Mercy General" : "Released from the Civic Center", w / 2.0f, ty + size + 96 * S, 17 * S, alpha(DIM, k2), 1);
}

static void pause_screen(int w, int h, float S)
{
    DrawRectangle(0, 0, w, h, (Color){ 4, 5, 8, 190 });
    float cx = w / 2.0f, y = h * 0.14f;
    text("PAUSED", cx, y, 64 * S, INK, 1);
    DrawRectangle((int)(cx - 40 * S), (int)(y + 82 * S), (int)(80 * S), (int)(4 * S), ACCENT);
    static const char *rows[][2] = {
        { "W A S D", "Move" }, { "Mouse", "Look" }, { "Left mouse", "Fire / punch" }, { "Right mouse", "Aim" },
        { "Shift", "Sprint" }, { "Space", "Jump / handbrake" }, { "C or Ctrl", "Crouch" }, { "F or Enter", "Get in / out of a car" },
        { "R", "Reload" }, { "E and Q / wheel", "Switch weapon" }, { "H", "Horn" }, { "V", "Camera distance" },
        { "F11", "Fullscreen" }, { "M (hold)", "Map" }, { "Esc or P", "Resume" },
    };
    int n = (int)(sizeof rows / sizeof rows[0]);
    int per = (n + 1) / 2;
    float keyw = 0;
    for (int i = 0; i < n; i++) keyw = fmaxf(keyw, text_w(rows[i][0], 17 * S));
    keyw += 24 * S;
    float colw = fmaxf(400 * S, keyw + 310 * S), top = y + 130 * S, rowh = 38 * S;
    for (int i = 0; i < n; i++) {
        int col = i / per, row = i % per;
        float x0 = cx - colw + col * colw + 10 * S;
        text(rows[i][0], x0, top + row * rowh, 17 * S, ACCENT, 0);
        text(rows[i][1], x0 + keyw, top + row * rowh, 17 * S, INK, 0);
    }
    text("Gamepad: sticks move and look, triggers aim and fire, A jump, Y get in, X reload, back for the map", cx, top + per * rowh + 40 * S, 13 * S, DIM, 1);
}

static void full_map(int w, int h, float S)
{
    if (!map_ok) return;
    Ped *pl = &peds[PLAYER];
    DrawRectangle(0, 0, w, h, (Color){ 4, 5, 8, 215 });
    float side = h * 0.88f;
    Vector3 f = { 0, 0, 1 };
    View v = { { w / 2.0f, h / 2.0f }, side / MAP_M, { 0, 0, 0 }, f, { -f.z, 0, f.x } };
    Rectangle r = { w / 2.0f - side / 2, h / 2.0f - side / 2, side, side };
    map_rect(&v, r);
    DrawRectangleLinesEx(r, 2 * S, alpha(INK, 0.5f));
    for (int i = 0; i < nshops; i++) {
        Vector2 p = to_screen(&v, shops[i].door);
        bool cool = game_s - shops[i].robbed_at < 300;
        text("$", p.x, p.y - 6 * S, 12 * S, cool ? alpha(DIM, 0.7f) : ACCENT, 1);
    }
    PickView pk[96];
    int np = pickups_view(pk, 96);
    for (int i = 0; i < np; i++) {
        Vector2 p = to_screen(&v, pk[i].pos);
        if (pk[i].type == PK_CASH) continue;
        if (pk[i].type == PK_HEALTH) cross(p, 4 * S, 2 * S, INK);
        else if (pk[i].type == PK_ARMOR) cross(p, 4 * S, 2 * S, STEEL);
        else diamond(p, 3.6f * S, alpha(INK, 0.85f));
    }
    Vector3 places[2] = { police_pos, hospital_pos };
    for (int i = 0; i < 2; i++) {
        Vector2 p = to_screen(&v, places[i]);
        DrawCircleV(p, 10 * S, (Color){ 8, 10, 14, 235 });
        if (i == 0) shield(p, 5.5f * S, 6.2f * S, COPBLUE, COPBLUE, true);
        else cross(p, 5.5f * S, 3 * S, INK);
        text(i == 0 ? "Police" : "Hospital", p.x, p.y + 14 * S, 12 * S, INK, 1);
    }
    for (int i = 0; i < MAX_MARKERS; i++) {
        int kind = marks[i].kind - 1;
        if (kind >= 0 && kind != MK_STASH) blip(to_screen(&v, marks[i].p), kind, 8 * S, S);
    }
    CopView cv[40];
    int nc = cops_view(cv, 40);
    for (int i = 0; i < nc; i++) DrawCircleV(to_screen(&v, cv[i].pos), 4 * S, i % 2 ? COPBLUE : DANGER);
    if (heat_search > 0) {
        Vector2 sc = to_screen(&v, last_seen);
        DrawRing(sc, (30 + heat_search * 2.5f) * v.s - 1, (30 + heat_search * 2.5f) * v.s + 1, 0, 360, 48, alpha(ACCENT, 0.8f));
    }
    Vector3 pp = pl->car >= 0 && cars[pl->car].used ? cars[pl->car].pos : pl->pos;
    float yaw = pl->car >= 0 && cars[pl->car].used ? atan2f(car_fwd(&cars[pl->car]).x, car_fwd(&cars[pl->car]).z) : pl->yaw;
    Vector2 c = to_screen(&v, pp);
    // the map has +x to the left, so a heading of yaw points at screen angle -yaw from up
    Vector3 hd = { sinf(yaw), 0, cosf(yaw) };
    Vector2 tip = Vector2Add(c, Vector2Scale(Vector2Subtract(to_screen(&v, Vector3Add(pp, hd)), c), 1));
    Vector2 dir = Vector2Normalize(Vector2Subtract(tip, c)), nrm = { -dir.y, dir.x };
    Vector2 a = Vector2Add(c, Vector2Scale(dir, 11 * S));
    Vector2 b = Vector2Add(Vector2Subtract(c, Vector2Scale(dir, 7 * S)), Vector2Scale(nrm, 7 * S));
    Vector2 d = Vector2Subtract(Vector2Subtract(c, Vector2Scale(dir, 7 * S)), Vector2Scale(nrm, 7 * S));
    DrawTriangle(a, d, b, INK);
    DrawTriangleLines(a, d, b, (Color){ 0, 0, 0, 220 });
    text("MAP", r.x, r.y - 30 * S, 20 * S, INK, 0);
    text(district(pp), r.x + r.width, r.y - 28 * S, 17 * S, DIM, 2);
}

void hud_draw(int w, int h)
{
    float S = h / 1080.0f;
    if (S < 0.5f) S = 0.5f;
    mode_t = mode_since;
    rlDisableBackfaceCulling();
    rlDisableDepthTest();
    BeginBlendMode(BLEND_ALPHA);

    if (mode == MODE_PAUSE) {
        radar(w, h, S);
        status(w, h, S);
        pause_screen(w, h, S);
    } else {
        radar(w, h, S);
        status(w, h, S);
        messages(w, h, S);
        beacons(w, h, S);
        objective_line(w, h, S);
        shop_panel(w, h, S);
        district_banner(w, h, S);
        crosshair(w, h, S);
        speedo(w, h, S);
        if (big_until > game_s) {
            float age = game_s - big_born, a = Clamp(fminf(age / 0.2f, (big_until - game_s) / 0.5f), 0, 1);
            text(big_t, w / 2.0f, h * 0.3f, 72 * S, alpha(INK, a), 1);
        }
        if (map_held && mode == MODE_PLAY) full_map(w, h, S);
        if (mode == MODE_DEAD || mode == MODE_BUSTED) end_card(w, h, S);
    }
    EndBlendMode();
    rlEnableBackfaceCulling();
}
