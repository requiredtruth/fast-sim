#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ped.h"
#include "rlgl.h"
#include "combat.h"





static const Color INK = { 238, 240, 244, 255 };
static const Color DIM = { 150, 156, 168, 255 };
static const Color ACCENT = { 255, 153, 51, 255 };
static const Color DANGER = { 232, 64, 56, 255 };

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

typedef struct { char t[80]; float born, until; } Msg;
static Msg msgs[4];
static int nmsg;
static char big_t[48];
static float big_until, big_born;

void hud_msg(const char *t, float secs)
{
    if (!t || !*t) return;
    if (nmsg > 0 && strncmp(msgs[nmsg - 1].t, t, 6) == 0 && (float)GetTime() - msgs[nmsg - 1].born < 2.5f) {
        snprintf(msgs[nmsg - 1].t, sizeof msgs[0].t, "%s", t);
        msgs[nmsg - 1].until = (float)GetTime() + secs;
        return;
    }
    if (nmsg == 4) { memmove(msgs, msgs + 1, 3 * sizeof msgs[0]); nmsg = 3; }
    snprintf(msgs[nmsg].t, sizeof msgs[0].t, "%s", t);
    msgs[nmsg].born = (float)GetTime();
    msgs[nmsg].until = (float)GetTime() + secs;
    nmsg++;
}

void hud_big(const char *t, float secs)
{
    snprintf(big_t, sizeof big_t, "%s", t);
    big_born = (float)GetTime();
    big_until = (float)GetTime() + secs;
}


// ================================================================ Long Road HUD, menus and touch controls

int ui_screen = UI_TITLE;
bool ui_blocks_game;

static float S;        // ui scale: 1 at 720 px tall
static int W, H;
static int sel_slot = -1, sel_cargo = -1, sel_recipe = -1;
static float scroll_y[16];
static bool touch_ui;  // touch controls are on screen

static const Color PANEL = { 20, 22, 26, 225 };
static const Color PANEL2 = { 34, 37, 43, 235 };
static const Color LINE = { 255, 255, 255, 40 };
static const Color GOOD = { 110, 200, 120, 255 };
static const Color WARN = { 240, 190, 70, 255 };
static const Color SAND = { 232, 196, 140, 255 };

static Color hexc(int rgb) { return (Color){ (unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8), (unsigned char)rgb, 255 }; }
static Color mixc(Color a, Color b, float t)
{
    t = Clamp(t, 0, 1);
    return (Color){ (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t), (unsigned char)(a.b + (b.b - a.b) * t), (unsigned char)(a.a + (b.a - a.a) * t) };
}

// ---------------------------------------------------------------- immediate mode widgets

static Vector2 press_at;
static bool press_live, press_used;

static bool hover(Rectangle r) { return CheckCollisionPointRec(GetMousePosition(), r); }

// a click: pressed and released inside the same rectangle without dragging far
static bool clicked(Rectangle r)
{
    if (press_used || !IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return false;
    Vector2 m = GetMousePosition();
    if (!CheckCollisionPointRec(m, r) || !CheckCollisionPointRec(press_at, r)) return false;
    if (Vector2Distance(m, press_at) > 24 * S) return false;
    press_used = true;
    sfx(S_UI, cam.position, 0.35f, 1.1f);
    return true;
}

static void panel(Rectangle r, Color c) { DrawRectangleRounded(r, 0.06f, 6, c); DrawRectangleRoundedLinesEx(r, 0.06f, 6, fmaxf(1, S), LINE); }

static bool button(Rectangle r, const char *label, bool on)
{
    bool h = on && hover(r) && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    Color bg = !on ? (Color){ 40, 42, 46, 200 } : h ? (Color){ 255, 153, 51, 255 } : (Color){ 58, 62, 70, 240 };
    DrawRectangleRounded(r, 0.25f, 6, bg);
    DrawRectangleRoundedLinesEx(r, 0.25f, 6, fmaxf(1, S * 1.2f), on ? (Color){ 255, 255, 255, 60 } : LINE);
    float ts = fminf(r.height * 0.42f, 22 * S);
    while (ts > 8 * S && text_w(label, ts) > r.width - 14 * S) ts *= 0.9f;
    text(label, r.x + r.width / 2, r.y + (r.height - ts) / 2, ts, on ? (h ? (Color){ 20, 20, 20, 255 } : INK) : DIM, 1);
    return on && clicked(r);
}

static void bar(Rectangle r, float v, Color fill)
{
    DrawRectangleRounded(r, 0.5f, 4, (Color){ 0, 0, 0, 140 });
    Rectangle f = r;
    f.width = r.width * Clamp(v, 0, 1);
    if (f.width > 1) DrawRectangleRounded(f, 0.5f, 4, fill);
}

static Rectangle rect(float x, float y, float w, float h) { return (Rectangle){ x, y, w, h }; }

// text that wraps inside a width; returns the height used
static float wrap(const char *s, float x, float y, float wmax, float size, Color c)
{
    char line[160] = { 0 }, word[64];
    float yy = y;
    const char *p = s;
    while (*p) {
        int n = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && n < 63) word[n++] = *p++;
        word[n] = 0;
        if (!n) break;
        char trial[200];
        snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word);
        if (text_w(trial, size) > wmax && line[0]) {
            text(line, x, yy, size, c, 0);
            yy += size * 1.6f;
            snprintf(line, sizeof line, "%s", word);
        } else snprintf(line, sizeof line, "%s", trial);
    }
    if (line[0]) { text(line, x, yy, size, c, 0); yy += size * 1.6f; }
    return yy - y;
}

// ---------------------------------------------------------------- icons, drawn from shapes

static void icon(int ic, Vector2 c, float s, Color col)
{
    Color dk = mixc(col, BLACK, 0.45f), lt = mixc(col, WHITE, 0.35f);
    float h = s * 0.5f;
    switch (ic) {
    case ICON_CAN:
        DrawRectangleRounded(rect(c.x - h * 0.6f, c.y - h * 0.8f, h * 1.2f, h * 1.6f), 0.2f, 4, col);
        DrawRectangle((int)(c.x - h * 0.6f), (int)(c.y - h * 0.25f), (int)(h * 1.2f), (int)(h * 0.5f), lt);
        DrawLineEx((Vector2){ c.x - h * 0.6f, c.y - h * 0.68f }, (Vector2){ c.x + h * 0.6f, c.y - h * 0.68f }, s * 0.05f, dk);
        break;
    case ICON_BOTTLE:
        DrawRectangleRounded(rect(c.x - h * 0.42f, c.y - h * 0.35f, h * 0.84f, h * 1.2f), 0.3f, 4, col);
        DrawRectangle((int)(c.x - h * 0.18f), (int)(c.y - h * 0.85f), (int)(h * 0.36f), (int)(h * 0.55f), col);
        DrawRectangle((int)(c.x - h * 0.22f), (int)(c.y - h * 0.95f), (int)(h * 0.44f), (int)(h * 0.16f), dk);
        DrawRectangle((int)(c.x - h * 0.3f), (int)(c.y - h * 0.1f), (int)(h * 0.18f), (int)(h * 0.7f), alpha(WHITE, 0.4f));
        break;
    case ICON_CROSS:
        DrawRectangleRounded(rect(c.x - h * 0.8f, c.y - h * 0.65f, h * 1.6f, h * 1.3f), 0.2f, 4, WHITE);
        DrawRectangle((int)(c.x - h * 0.15f), (int)(c.y - h * 0.5f), (int)(h * 0.3f), (int)(h * 1.0f), (Color){ 220, 40, 40, 255 });
        DrawRectangle((int)(c.x - h * 0.5f), (int)(c.y - h * 0.15f), (int)(h * 1.0f), (int)(h * 0.3f), (Color){ 220, 40, 40, 255 });
        break;
    case ICON_JERRY:
        DrawRectangleRounded(rect(c.x - h * 0.6f, c.y - h * 0.6f, h * 1.2f, h * 1.45f), 0.15f, 4, col);
        DrawRectangle((int)(c.x - h * 0.4f), (int)(c.y - h * 0.85f), (int)(h * 0.5f), (int)(h * 0.28f), dk);
        DrawLineEx((Vector2){ c.x - h * 0.5f, c.y - h * 0.4f }, (Vector2){ c.x + h * 0.5f, c.y + h * 0.75f }, s * 0.06f, dk);
        DrawLineEx((Vector2){ c.x + h * 0.5f, c.y - h * 0.4f }, (Vector2){ c.x - h * 0.5f, c.y + h * 0.75f }, s * 0.06f, dk);
        break;
    case ICON_GEAR:
        for (int i = 0; i < 8; i++) {
            float a = i * PI / 4;
            DrawLineEx(c, (Vector2){ c.x + cosf(a) * h * 0.9f, c.y + sinf(a) * h * 0.9f }, s * 0.16f, col);
        }
        DrawCircleV(c, h * 0.62f, col);
        DrawCircleV(c, h * 0.25f, dk);
        break;
    case ICON_WRENCH:
        DrawLineEx((Vector2){ c.x - h * 0.6f, c.y + h * 0.6f }, (Vector2){ c.x + h * 0.35f, c.y - h * 0.35f }, s * 0.16f, col);
        DrawRing((Vector2){ c.x + h * 0.45f, c.y - h * 0.45f }, h * 0.18f, h * 0.38f, -60, 230, 10, col);
        break;
    case ICON_BULLET:
        for (int k = -1; k <= 1; k++) {
            float x = c.x + k * h * 0.45f;
            DrawRectangle((int)(x - h * 0.14f), (int)(c.y - h * 0.2f), (int)(h * 0.28f), (int)(h * 0.9f), col);
            DrawCircleV((Vector2){ x, c.y - h * 0.2f }, h * 0.14f, lt);
            DrawRectangle((int)(x - h * 0.14f), (int)(c.y - h * 0.45f), (int)(h * 0.28f), (int)(h * 0.25f), lt);
        }
        break;
    case ICON_GUN:
        DrawRectangle((int)(c.x - h * 0.9f), (int)(c.y - h * 0.3f), (int)(h * 1.8f), (int)(h * 0.28f), col);
        DrawRectangle((int)(c.x - h * 0.8f), (int)(c.y - h * 0.1f), (int)(h * 0.45f), (int)(h * 0.7f), dk);
        DrawRectangle((int)(c.x - h * 0.2f), (int)(c.y - h * 0.05f), (int)(h * 0.12f), (int)(h * 0.3f), dk);
        break;
    case ICON_BAG:
        DrawRectangleRounded(rect(c.x - h * 0.7f, c.y - h * 0.45f, h * 1.4f, h * 1.25f), 0.35f, 6, col);
        DrawRing((Vector2){ c.x, c.y - h * 0.45f }, h * 0.22f, h * 0.36f, 180, 360, 10, dk);
        DrawRectangle((int)(c.x - h * 0.5f), (int)(c.y), (int)(h * 1.0f), (int)(h * 0.12f), dk);
        break;
    case ICON_PLANK:
        for (int k = 0; k < 3; k++) DrawRectangleRounded(rect(c.x - h * 0.85f + k * h * 0.12f, c.y - h * 0.6f + k * h * 0.42f, h * 1.6f, h * 0.32f), 0.4f, 4, k & 1 ? dk : col);
        break;
    case ICON_COIN:
        DrawRectangleRounded(rect(c.x - h * 0.85f, c.y - h * 0.5f, h * 1.7f, h * 1.0f), 0.15f, 4, col);
        DrawCircleV(c, h * 0.3f, lt);
        DrawCircleV(c, h * 0.18f, col);
        break;
    case ICON_TIRE:
        DrawRing(c, h * 0.45f, h * 0.9f, 0, 360, 20, (Color){ 30, 30, 30, 255 });
        DrawRing(c, h * 0.3f, h * 0.45f, 0, 360, 16, (Color){ 160, 160, 164, 255 });
        break;
    case ICON_BATTERY:
        DrawRectangleRounded(rect(c.x - h * 0.8f, c.y - h * 0.5f, h * 1.6f, h * 1.1f), 0.1f, 4, col);
        DrawRectangle((int)(c.x - h * 0.55f), (int)(c.y - h * 0.7f), (int)(h * 0.3f), (int)(h * 0.2f), dk);
        DrawRectangle((int)(c.x + h * 0.25f), (int)(c.y - h * 0.7f), (int)(h * 0.3f), (int)(h * 0.2f), dk);
        text("+", c.x - h * 0.4f, c.y - h * 0.25f, h * 0.6f, WHITE, 1);
        break;
    case ICON_PILL:
        DrawRectangleRounded(rect(c.x - h * 0.8f, c.y - h * 0.3f, h * 1.6f, h * 0.6f), 1, 8, col);
        DrawRectangleRounded(rect(c.x, c.y - h * 0.3f, h * 0.8f, h * 0.6f), 1, 8, WHITE);
        break;
    case ICON_FLAME:
        DrawCircleV((Vector2){ c.x, c.y + h * 0.2f }, h * 0.5f, col);
        DrawTriangle((Vector2){ c.x, c.y - h * 0.9f }, (Vector2){ c.x - h * 0.5f, c.y + h * 0.1f }, (Vector2){ c.x + h * 0.5f, c.y + h * 0.1f }, col);
        DrawCircleV((Vector2){ c.x, c.y + h * 0.3f }, h * 0.25f, (Color){ 255, 240, 160, 255 });
        break;
    case ICON_MEAT:
        DrawEllipse((int)c.x, (int)c.y, h * 0.75f, h * 0.55f, col);
        DrawEllipse((int)(c.x - h * 0.1f), (int)(c.y - h * 0.05f), h * 0.4f, h * 0.28f, lt);
        DrawLineEx((Vector2){ c.x + h * 0.5f, c.y + h * 0.3f }, (Vector2){ c.x + h * 0.9f, c.y + h * 0.7f }, s * 0.08f, (Color){ 240, 230, 210, 255 });
        break;
    case ICON_LEAF:
        DrawCircleV(c, h * 0.62f, col);
        DrawLineEx((Vector2){ c.x, c.y - h * 0.6f }, (Vector2){ c.x + h * 0.15f, c.y - h * 0.95f }, s * 0.06f, (Color){ 90, 60, 30, 255 });
        DrawEllipse((int)(c.x + h * 0.3f), (int)(c.y - h * 0.8f), h * 0.25f, h * 0.12f, (Color){ 80, 150, 60, 255 });
        break;
    default:
        DrawRectangleRounded(rect(c.x - h * 0.75f, c.y - h * 0.6f, h * 1.5f, h * 1.2f), 0.15f, 4, col);
        DrawLineEx((Vector2){ c.x - h * 0.75f, c.y - h * 0.2f }, (Vector2){ c.x + h * 0.75f, c.y - h * 0.2f }, s * 0.05f, dk);
        break;
    }
}

static void item_tile(Rectangle r, Slot s, bool selected, bool dim)
{
    Color bg = selected ? (Color){ 255, 153, 51, 90 } : (Color){ 255, 255, 255, 14 };
    DrawRectangleRounded(r, 0.12f, 4, bg);
    DrawRectangleRoundedLinesEx(r, 0.12f, 4, fmaxf(1, S), selected ? ACCENT : LINE);
    if (s.n <= 0) return;
    Item *it = &items[s.item];
    icon(it->icon, (Vector2){ r.x + r.width / 2, r.y + r.height * 0.44f }, r.height * 0.55f, alpha(hexc(it->color), dim ? 0.4f : 1));
    if (s.n > 1) {
        char b[16];
        snprintf(b, sizeof b, "%d", s.n);
        text(b, r.x + r.width - 5 * S, r.y + r.height - 15 * S, 11 * S, INK, 2);
    }
}

// ---------------------------------------------------------------- the in-game HUD

static float smooth_fuel = -1;

static void stat_row(float x, float y, int ic, Color col, float v, const char *label)
{
    float sz = 22 * S;
    icon(ic, (Vector2){ x + sz / 2, y + sz / 2 }, sz, v < 0.2f ? (fmodf((float)GetTime(), 0.8f) < 0.4f ? DANGER : col) : col);
    bar(rect(x + sz + 8 * S, y + sz * 0.3f, 120 * S, sz * 0.42f), v, v < 0.2f ? DANGER : v < 0.4f ? WARN : col);
    (void)label;
}

static const char *clock_str(void)
{
    static char b[16];
    int h = (int)day_hours, m = (int)((day_hours - h) * 60);
    snprintf(b, sizeof b, "%02d:%02d", h, m);
    return b;
}

static void survival_panel(void)
{
    Ped *p = &peds[PLAYER];
    float x = 16 * S, y = 14 * S;
    panel(rect(x - 8 * S, y - 6 * S, 172 * S, 152 * S), (Color){ 10, 12, 14, 150 });
    stat_row(x, y, ICON_CROSS, (Color){ 230, 70, 60, 255 }, p->health / 100, "HEALTH");
    stat_row(x, y + 28 * S, ICON_CAN, (Color){ 220, 170, 80, 255 }, sv.food / 100, "FOOD");
    stat_row(x, y + 56 * S, ICON_BOTTLE, (Color){ 90, 170, 240, 255 }, sv.water / 100, "WATER");
    stat_row(x, y + 84 * S, ICON_PILL, (Color){ 170, 150, 230, 255 }, sv.rest / 100, "REST");
    stat_row(x, y + 112 * S, ICON_FLAME, (Color){ 240, 120, 60, 255 }, sv.warmth / 100, "WARMTH");
    float yy = y + 146 * S;
    if (sv.bleed > 0) { text("BLEEDING", x, yy, 13 * S, DANGER, 0); yy += 18 * S; }
    if (sv.sick > 30) { text("SICK", x, yy, 13 * S, (Color){ 160, 220, 90, 255 }, 0); yy += 18 * S; }
    if (pack_weight() > pack_capacity()) text("OVERLOADED", x, yy, 13 * S, WARN, 0);
}

static void world_panel(void)
{
    float s = road_s(peds[PLAYER].car >= 0 ? cars[peds[PLAYER].car].pos : peds[PLAYER].pos);
    char b[64];
    float x = W - 16 * S;
    panel(rect(x - 200 * S, 8 * S, 208 * S, 86 * S), (Color){ 10, 12, 14, 150 });
    text(clock_str(), x - 8 * S, 16 * S, 22 * S, INK, 2);
    snprintf(b, sizeof b, "DAY %d", (int)sv.days + 1);
    text(b, x - 192 * S, 20 * S, 14 * S, DIM, 0);
    float t = world_temp();
    const char *wx = weather_snow > 0.3f ? "SNOW" : weather_rain > 0.3f ? "RAIN" : weather_dust > 0.3f ? "DUST STORM" : weather_fog > 0.3f ? "FOG" : "CLEAR";
    snprintf(b, sizeof b, "%.0f C  %s", t, wx);
    text(b, x - 8 * S, 46 * S, 13 * S, t < 5 ? STEEL : t > 30 ? WARN : DIM, 2);
    snprintf(b, sizeof b, "KM %.1f  %s", (s - world_start_s()) / 1000, biome_name(biome_at(s)));
    text(b, x - 8 * S, 68 * S, 13 * S, SAND, 2);
}

// the next stop up the road, like a highway sign
static void next_stop(void)
{
    Vector3 f = peds[PLAYER].car >= 0 ? cars[peds[PLAYER].car].pos : peds[PLAYER].pos;
    float s0 = road_s(f);
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < MAX_STOPS; i++) {
        if (!stops[i].used) continue;
        float ss = road_s(stops[i].p);
        float d = ss - s0;
        if (d < -40 || d > 2500) continue;
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return;
    char b[64];
    if (bd < 60) snprintf(b, sizeof b, "%s", stops[best].name);
    else snprintf(b, sizeof b, "%s  %.1f KM", stops[best].name, bd / 1000);
    float ts = 15 * S, w = text_w(b, ts) + 30 * S;
    Rectangle r = rect(W / 2 - w / 2, 12 * S, w, 30 * S);
    DrawRectangleRounded(r, 0.25f, 6, (Color){ 22, 90, 50, 230 });
    DrawRectangleRoundedLinesEx(r, 0.25f, 6, 2 * S, (Color){ 230, 240, 230, 200 });
    text(b, W / 2, r.y + 8 * S, ts, WHITE, 1);
    if (stops[best].hostile && stops[best].visited) text("HOSTILE", W / 2, r.y + 36 * S, 12 * S, DANGER, 1);
}

static void car_gauges(void)
{
    Ped *p = &peds[PLAYER];
    if (p->car < 0 || !cars[p->car].used) return;
    Car *c = &cars[p->car];
    float cx = W / 2, by = H - (touch_ui ? 112 : 24) * S;
    char b[32];
    snprintf(b, sizeof b, "%.0f", car_kmh(c));
    text(b, cx, by - 54 * S, 40 * S, INK, 1);
    text("KM/H", cx, by - 8 * S, 11 * S, DIM, 1);
    if (smooth_fuel < 0) smooth_fuel = c->fuel;
    smooth_fuel += (c->fuel - smooth_fuel) * 0.1f;
    float fx = cx - 210 * S;
    icon(ICON_JERRY, (Vector2){ fx, by - 30 * S }, 22 * S, (Color){ 220, 60, 50, 255 });
    bar(rect(fx + 16 * S, by - 36 * S, 120 * S, 12 * S), smooth_fuel / c->fuel_max, c->fuel < c->fuel_max * 0.15f ? DANGER : SAND);
    snprintf(b, sizeof b, "%.0f L", c->fuel);
    text(b, fx + 16 * S, by - 18 * S, 11 * S, DIM, 0);
    float tx = cx + 90 * S;
    text("TEMP", tx, by - 38 * S, 11 * S, DIM, 0);
    bar(rect(tx + 44 * S, by - 38 * S, 76 * S, 10 * S), c->heat / 1.3f, c->heat > 1 ? DANGER : c->heat > 0.85f ? WARN : STEEL);
    // warning lamps
    float lx = tx;
    int flat = 0;
    for (int w = 0; w < 4; w++) if (c->tire[w] <= 0) flat++;
    struct { bool on; const char *t; } lamps[5] = {
        { c->engine < 30, "ENGINE" }, { flat > 0, "TYRE" }, { c->battery < 15, "BATT" }, { c->radiator < 25, "COOL" }, { c->lights, "LIGHTS" },
    };
    for (int k = 0; k < 5; k++) {
        if (!lamps[k].on) continue;
        text(lamps[k].t, lx, by - 18 * S, 11 * S, k == 4 ? GOOD : DANGER, 0);
        lx += text_w(lamps[k].t, 11 * S) + 10 * S;
    }
    if (c->stalled) text(c->fuel <= 0 ? "OUT OF FUEL" : c->battery <= 0 ? "DEAD BATTERY" : "ENGINE DEAD", cx, by - 82 * S, 16 * S, DANGER, 1);
}

static void weapon_box(void)
{
    Ped *p = &peds[PLAYER];
    if (p->car >= 0) return;
    float x = W - 18 * S, y = H - (touch_ui ? 250 : 40) * S;
    if (p->weapon == W_FIST) return;
    char b[48];
    if (p->weapon == W_BAT) snprintf(b, sizeof b, "%s", weapons[p->weapon].name);
    else snprintf(b, sizeof b, "%s  %d / %d", weapons[p->weapon].name, p->clip_ammo, p->ammo[p->weapon]);
    text(b, x, y, 16 * S, p->clip_ammo == 0 && p->weapon != W_BAT ? DANGER : INK, 2);
    if (p->reload_s > 0) text("RELOADING", x, y + 22 * S, 12 * S, WARN, 2);
}

static void crosshair(void)
{
    Ped *p = &peds[PLAYER];
    if (p->car >= 0 || !p->aiming) return;
    Vector2 c = { W / 2.0f, H / 2.0f };
    Color col = aim_target >= 0 ? DANGER : WHITE;
    float r = 9 * S, g = 4 * S;
    DrawLineEx((Vector2){ c.x - r, c.y }, (Vector2){ c.x - g, c.y }, 2 * S, col);
    DrawLineEx((Vector2){ c.x + g, c.y }, (Vector2){ c.x + r, c.y }, 2 * S, col);
    DrawLineEx((Vector2){ c.x, c.y - r }, (Vector2){ c.x, c.y - g }, 2 * S, col);
    DrawLineEx((Vector2){ c.x, c.y + g }, (Vector2){ c.x, c.y + r }, 2 * S, col);
}

static void messages(void)
{
    float now = (float)GetTime();
    float y = H * 0.62f;
    for (int i = nmsg - 1; i >= 0; i--) {
        if (now > msgs[i].until) continue;
        float a = Clamp((msgs[i].until - now) * 2, 0, 1) * Clamp((now - msgs[i].born) * 6, 0, 1);
        text(msgs[i].t, W / 2, y, 17 * S, alpha(INK, a), 1);
        y -= 28 * S;
    }
    if (now < big_until) {
        float a = Clamp((big_until - now) * 1.5f, 0, 1) * Clamp((now - big_born) * 3, 0, 1);
        text(big_t, W / 2, H * 0.28f, 34 * S, alpha(SAND, a), 1);
    }
}

static void prompt_line(void)
{
    const char *pr = survive_prompt();
    if (!pr || ui_screen != UI_NONE) return;
    char b[96];
    if (touch_ui) snprintf(b, sizeof b, "%s", pr);
    else snprintf(b, sizeof b, "E  %s", pr);
    float ts = 17 * S, w = text_w(b, ts) + 34 * S;
    Rectangle r = rect(W / 2 - w / 2, H * 0.7f, w, 34 * S);
    DrawRectangleRounded(r, 0.4f, 6, (Color){ 0, 0, 0, 150 });
    text(b, W / 2, r.y + 9 * S, ts, ACCENT, 1);
}

static void search_ring(void)
{
    if (search_ci < 0) return;
    Vector2 c = { W / 2.0f, H * 0.58f };
    float k = Clamp(search_t / 1.4f, 0, 1);
    DrawRing(c, 18 * S, 24 * S, -90, -90 + 360 * k, 32, ACCENT);
    text("SEARCHING", c.x, c.y + 32 * S, 13 * S, INK, 1);
}

static void danger_overlay(void)
{
    Ped *p = &peds[PLAYER];
    float k = p->health < 35 ? (35 - p->health) / 35 : 0;
    if (sv.warmth < 15) k = fmaxf(k, 0.3f);
    if (k > 0) {
        Color edge = sv.warmth < 15 && p->health >= 35 ? (Color){ 120, 180, 255, (unsigned char)(110 * k) } : (Color){ 180, 0, 0, (unsigned char)(140 * k) };
        float e = 120 * S;
        DrawRectangleGradientV(0, 0, W, (int)e, edge, BLANK);
        DrawRectangleGradientV(0, H - (int)e, W, (int)e, BLANK, edge);
        DrawRectangleGradientH(0, 0, (int)e, H, edge, BLANK);
        DrawRectangleGradientH(W - (int)e, 0, (int)e, H, BLANK, edge);
    }
    int act = raiders_active();
    if (act > 0) text("RAIDERS", W / 2, 50 * S, 14 * S, alpha(DANGER, 0.6f + 0.4f * sinf((float)GetTime() * 6)), 1);
}

// ---------------------------------------------------------------- touch controls

typedef struct { int id; int ctl; Vector2 start, last; bool live; } Touch;
static Touch touches[10];
static bool aim_toggle;
enum { TC_NONE, TC_JOY, TC_LOOK, TC_FIRE, TC_AIM, TC_JUMP, TC_USE, TC_CAR, TC_BAG, TC_SWAP, TC_PAUSE, TC_GAS, TC_BRAKE,
       TC_LEFT, TC_RIGHT, TC_HORN, TC_LIGHTS, TC_HAND, TC_CAM, TC_CROUCH, TC_RELOAD, TC_COUNT };
static Vector2 tc_pos[TC_COUNT];
static float tc_r[TC_COUNT];
static bool tc_down[TC_COUNT];
static Vector2 joy_c;
static Vector2 joy_v;

static void layout_touch(bool in_car)
{
    memset(tc_r, 0, sizeof tc_r);
    float m = 26 * S, big = 52 * S, med = 38 * S, sm = 30 * S;
    tc_pos[TC_PAUSE] = (Vector2){ W - 210 * S - 40 * S, 30 * S }; tc_r[TC_PAUSE] = 22 * S;
    tc_pos[TC_BAG] = (Vector2){ W - 210 * S - 92 * S, 30 * S }; tc_r[TC_BAG] = 22 * S;
    if (in_car) {
        tc_pos[TC_GAS] = (Vector2){ W - m - big, H - m - big * 1.2f }; tc_r[TC_GAS] = big * 1.15f;
        tc_pos[TC_BRAKE] = (Vector2){ W - m - big * 3.4f, H - m - big * 0.9f }; tc_r[TC_BRAKE] = big * 0.95f;
        if (!tilt_steer || !tilt_available()) {
            tc_pos[TC_LEFT] = (Vector2){ m + big, H - m - big }; tc_r[TC_LEFT] = big;
            tc_pos[TC_RIGHT] = (Vector2){ m + big * 3.3f, H - m - big }; tc_r[TC_RIGHT] = big;
        }
        tc_pos[TC_CAR] = (Vector2){ W - m - sm, H * 0.42f }; tc_r[TC_CAR] = sm;
        tc_pos[TC_USE] = (Vector2){ W - m - sm, H * 0.42f - 2.4f * sm }; tc_r[TC_USE] = sm;
        tc_pos[TC_LIGHTS] = (Vector2){ m + sm, H * 0.42f - 2.4f * sm }; tc_r[TC_LIGHTS] = sm;
        tc_pos[TC_HORN] = (Vector2){ m + sm, H * 0.42f }; tc_r[TC_HORN] = sm;
        tc_pos[TC_HAND] = (Vector2){ W - m - big * 3.4f, H - m - big * 2.8f }; tc_r[TC_HAND] = sm;
        tc_pos[TC_CAM] = (Vector2){ m + sm, H * 0.42f + 2.4f * sm }; tc_r[TC_CAM] = sm;
    } else {
        bool armed = peds[PLAYER].weapon != W_FIST;
        tc_pos[TC_FIRE] = (Vector2){ W - m - big, H - m - big }; tc_r[TC_FIRE] = big;
        tc_pos[TC_JUMP] = (Vector2){ W - m - big * 3.1f, H - m - med * 0.8f }; tc_r[TC_JUMP] = med;
        tc_pos[TC_USE] = (Vector2){ W - m - big * 1.0f, H - m - big * 3.1f }; tc_r[TC_USE] = med;
        tc_pos[TC_CAR] = (Vector2){ W - m - big * 3.1f, H - m - big * 2.7f }; tc_r[TC_CAR] = med;
        tc_pos[TC_SWAP] = (Vector2){ W - m - sm, H * 0.34f }; tc_r[TC_SWAP] = sm;
        tc_pos[TC_CROUCH] = (Vector2){ W - m - big * 3.1f - med * 2.4f, H - m - med * 0.8f }; tc_r[TC_CROUCH] = sm;
        if (armed && peds[PLAYER].weapon != W_BAT) {
            tc_pos[TC_AIM] = (Vector2){ W - m - big * 2.2f, H - m - big * 2.0f }; tc_r[TC_AIM] = sm;
            tc_pos[TC_RELOAD] = (Vector2){ W - m - sm - 2.4f * sm, H * 0.34f }; tc_r[TC_RELOAD] = sm;
        }
    }
}

static int touch_hit(Vector2 p)
{
    for (int k = TC_FIRE; k < TC_COUNT; k++)
        if (tc_r[k] > 0 && Vector2Distance(p, tc_pos[k]) < tc_r[k] * 1.15f) return k;
    return TC_NONE;
}

bool hud_touch_update(Input *in, int w, int h)
{
    W = w; H = h; S = fminf(h / 720.0f, w / 1280.0f) * (is_touch_device() ? 1.15f : 1.0f);
    int n = GetTouchPointCount();
    if (n > 0) touch_ui = true;
    if (!touch_ui) return false;
    Ped *p = &peds[PLAYER];
    bool in_car = p->car >= 0 && p->state == PS_IN_CAR;
    layout_touch(in_car);
    memset(tc_down, 0, sizeof tc_down);
    // match touches by id
    bool seen[10] = { 0 };
    for (int i = 0; i < n && i < 10; i++) {
        int id = GetTouchPointId(i);
        Vector2 pos = GetTouchPosition(i);
        int slot = -1;
        for (int k = 0; k < 10; k++) if (touches[k].live && touches[k].id == id) slot = k;
        if (slot < 0) {
            for (int k = 0; k < 10; k++) if (!touches[k].live) { slot = k; break; }
            if (slot < 0) continue;
            Touch *t = &touches[slot];
            t->live = true; t->id = id; t->start = pos; t->last = pos;
            t->ctl = ui_screen != UI_NONE ? TC_NONE : touch_hit(pos);
            if (t->ctl == TC_NONE && ui_screen == UI_NONE) {
                if (!in_car && pos.x < w * 0.42f && pos.y > h * 0.3f) { t->ctl = TC_JOY; joy_c = pos; }
                else if (pos.y > 60 * S) t->ctl = TC_LOOK;
            }
            if (t->ctl == TC_AIM) aim_toggle = !aim_toggle;
            if (t->ctl == TC_PAUSE) { ui_screen = UI_PAUSE; }
            if (t->ctl == TC_BAG) { ui_screen = UI_PACK; }
            if (t->ctl == TC_USE) survive_interact();
        }
        seen[slot] = true;
        Touch *t = &touches[slot];
        if (t->ctl > TC_LOOK) tc_down[t->ctl] = true;
        if (t->ctl == TC_LOOK) {
            Vector2 d = Vector2Subtract(pos, t->last);
            in->lx += d.x * 0.0058f / S;
            in->ly += d.y * 0.0048f / S;
        }
        if (t->ctl == TC_JOY) {
            Vector2 d = Vector2Subtract(pos, joy_c);
            float r = 70 * S, l = Vector2Length(d);
            if (l > r) { joy_c = Vector2Add(joy_c, Vector2Scale(d, (l - r) / l)); d = Vector2Scale(d, r / l); }
            joy_v = Vector2Scale(d, 1 / r);
            in->mx = joy_v.x; in->my = -joy_v.y;
            if (Vector2Length(joy_v) > 0.97f) in->held |= BTN_SPRINT;
        }
        t->last = pos;
    }
    for (int k = 0; k < 10; k++) if (touches[k].live && !seen[k]) { touches[k].live = false; if (touches[k].ctl == TC_JOY) joy_v = (Vector2){ 0 }; }
    bool joy = false;
    for (int k = 0; k < 10; k++) if (touches[k].live && touches[k].ctl == TC_JOY) joy = true;
    // tilt
    Vector2 tl = tilt_read();
    if (in_car) {
        if (tilt_steer && tilt_available()) in->mx = tl.x;
        else in->mx = (tc_down[TC_RIGHT] ? 1.0f : 0) - (tc_down[TC_LEFT] ? 1.0f : 0);
        float gas = tc_down[TC_GAS] ? 1.0f : 0, brake = tc_down[TC_BRAKE] ? 1.0f : 0;
        if (!gas && !brake && tilt_steer && tilt_available() && tilt_walk) { if (tl.y > 0.1f) gas = tl.y; else if (tl.y < -0.25f) brake = -tl.y; }
        in->my = gas - brake;
        if (tc_down[TC_HORN]) in->held |= BTN_HORN;
        if (tc_down[TC_HAND]) in->held |= BTN_JUMP;
        if (tc_down[TC_LIGHTS]) in->held |= BTN_LIGHTS;
        if (tc_down[TC_CAM]) in->held |= BTN_CAM;
        if (tc_down[TC_CAR]) in->held |= BTN_ENTER;
    } else {
        if (!joy && tilt_walk && tilt_available()) {
            in->my = tl.y;
            in->lx += tl.x * 2.2f * GetFrameTime();
            if (tl.y > 0.95f) in->held |= BTN_SPRINT;
        }
        if (tc_down[TC_FIRE]) in->held |= BTN_FIRE;
        if (aim_toggle && p->weapon != W_FIST && p->weapon != W_BAT) in->held |= BTN_AIM;
        if (p->weapon == W_FIST || p->weapon == W_BAT) aim_toggle = false;
        if (tc_down[TC_JUMP]) in->held |= BTN_JUMP;
        if (tc_down[TC_CAR]) in->held |= BTN_ENTER;
        if (tc_down[TC_SWAP]) in->held |= BTN_NEXT;
        if (tc_down[TC_CROUCH]) in->held |= BTN_CROUCH;
        if (tc_down[TC_RELOAD]) in->held |= BTN_RELOAD;
    }
    return true;
}

static void touch_button(int k, const char *label, int ic)
{
    if (tc_r[k] <= 0) return;
    Vector2 c = tc_pos[k];
    bool dn = tc_down[k] || (k == TC_AIM && aim_toggle);
    DrawCircleV(c, tc_r[k], dn ? (Color){ 255, 153, 51, 150 } : (Color){ 0, 0, 0, 95 });
    DrawRing(c, tc_r[k] - 2.5f * S, tc_r[k], 0, 360, 36, (Color){ 255, 255, 255, dn ? 200 : 90 });
    if (ic >= 0) icon(ic, (Vector2){ c.x, c.y - tc_r[k] * 0.12f }, tc_r[k] * 0.7f, (Color){ 255, 255, 255, 200 });
    float ts = fminf(tc_r[k] * 0.34f, 15 * S);
    text(label, c.x, ic >= 0 ? c.y + tc_r[k] * 0.38f : c.y - ts / 2, ts, (Color){ 255, 255, 255, 220 }, 1);
}

static void draw_touch(void)
{
    if (!touch_ui || ui_screen != UI_NONE) return;
    Ped *p = &peds[PLAYER];
    bool in_car = p->car >= 0 && p->state == PS_IN_CAR;
    touch_button(TC_PAUSE, "II", -1);
    touch_button(TC_BAG, "", ICON_BAG);
    if (in_car) {
        touch_button(TC_GAS, "GAS", -1);
        touch_button(TC_BRAKE, "BRAKE", -1);
        touch_button(TC_LEFT, "<", -1);
        touch_button(TC_RIGHT, ">", -1);
        touch_button(TC_CAR, "EXIT", -1);
        touch_button(TC_USE, "CAR", ICON_WRENCH);
        touch_button(TC_LIGHTS, "LIGHTS", -1);
        touch_button(TC_HORN, "HORN", -1);
        touch_button(TC_HAND, "HAND", -1);
        touch_button(TC_CAM, "CAM", -1);
        if (tilt_steer && tilt_available()) {
            Vector2 tl = tilt_read();
            float cx = W * 0.25f, cy = H - 70 * S;
            DrawRing((Vector2){ cx, cy }, 36 * S, 40 * S, 200, 340, 20, (Color){ 255, 255, 255, 70 });
            float a = (270 + tl.x * 60) * DEG2RAD;
            DrawCircleV((Vector2){ cx + cosf(a) * 38 * S, cy + sinf(a) * 38 * S }, 6 * S, ACCENT);
        }
    } else {
        touch_button(TC_FIRE, p->weapon == W_FIST ? "PUNCH" : p->weapon == W_BAT ? "SWING" : "FIRE", p->weapon == W_FIST ? -1 : ICON_GUN);
        touch_button(TC_AIM, "AIM", -1);
        touch_button(TC_JUMP, "JUMP", -1);
        touch_button(TC_USE, survive_prompt() ? "USE" : "USE", ICON_WRENCH);
        touch_button(TC_CAR, "CAR", -1);
        touch_button(TC_SWAP, "SWAP", -1);
        touch_button(TC_CROUCH, "DUCK", -1);
        touch_button(TC_RELOAD, "RELOAD", -1);
        bool joy = false;
        for (int k = 0; k < 10; k++) if (touches[k].live && touches[k].ctl == TC_JOY) joy = true;
        if (joy) {
            DrawCircleV(joy_c, 70 * S, (Color){ 0, 0, 0, 70 });
            DrawCircleV(Vector2Add(joy_c, Vector2Scale(joy_v, 70 * S)), 30 * S, (Color){ 255, 255, 255, 120 });
        } else if (!(tilt_walk && tilt_available())) {
            DrawCircleV((Vector2){ 130 * S, H - 140 * S }, 70 * S, (Color){ 0, 0, 0, 40 });
            text("MOVE", 130 * S, H - 146 * S, 13 * S, (Color){ 255, 255, 255, 90 }, 1);
        }
    }
}

// ---------------------------------------------------------------- screens

static Rectangle frame_rect(void) { return rect(W * 0.05f, H * 0.07f, W * 0.9f, H * 0.86f); }

static bool close_button(Rectangle f)
{
    Rectangle r = rect(f.x + f.width - 54 * S, f.y + 10 * S, 44 * S, 38 * S);
    return button(r, "X", true) || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACK);
}

static void tabs(Rectangle f)
{
    static const char *names[4] = { "PACK", "CRAFT", "CAR", "TRIP" };
    static const int scr[4] = { UI_PACK, UI_CRAFT, UI_CAR, UI_MAP };
    float x = f.x + 16 * S;
    for (int i = 0; i < 4; i++) {
        Rectangle r = rect(x, f.y + 10 * S, 120 * S, 38 * S);
        bool on = ui_screen == scr[i];
        if (on) { DrawRectangleRounded(r, 0.25f, 6, ACCENT); text(names[i], r.x + r.width / 2, r.y + 11 * S, 16 * S, (Color){ 20, 20, 20, 255 }, 1); }
        else if (button(r, names[i], true)) {
            ui_screen = scr[i];
            if (scr[i] == UI_CAR && (car_view < 0 || !cars[car_view].used)) car_view = sv.home_car;
            sel_slot = sel_cargo = -1;
        }
        x += 130 * S;
    }
}

static void details(Rectangle r, int item, int count)
{
    if (item < 0) { text("Tap an item", r.x + r.width / 2, r.y + r.height * 0.4f, 15 * S, DIM, 1); return; }
    Item *it = &items[item];
    icon(it->icon, (Vector2){ r.x + 50 * S, r.y + 50 * S }, 64 * S, hexc(it->color));
    text(it->name, r.x + 96 * S, r.y + 24 * S, 18 * S, INK, 0);
    char b[96];
    snprintf(b, sizeof b, "%s  %.2f KG  x%d", item_kind_name(it->kind), it->weight, count);
    text(b, r.x + 96 * S, r.y + 52 * S, 12 * S, DIM, 0);
    float y = r.y + 100 * S;
    y += wrap(it->desc, r.x + 16 * S, y, r.width - 32 * S, 14 * S, INK);
    char e[96] = { 0 };
    switch (it->effect) {
    case E_EAT: snprintf(e, sizeof e, "FOOD +%.0f%s", it->value, it->value2 ? (it->value2 > 0 ? "  WATER +" : "  WATER ") : ""); if (it->value2) snprintf(e + strlen(e), 20, "%.0f", it->value2); break;
    case E_DRINK: snprintf(e, sizeof e, "WATER +%.0f", it->value); break;
    case E_HEAL: snprintf(e, sizeof e, "HEALTH +%.0f", it->value); break;
    case E_BANDAGE: snprintf(e, sizeof e, "STOPS BLEEDING, HEALTH +%.0f", it->value); break;
    case E_REST: snprintf(e, sizeof e, "REST +%.0f", it->value); break;
    case E_WARM: snprintf(e, sizeof e, "WARMTH +%.0f", it->value); break;
    case E_FUEL: snprintf(e, sizeof e, "FUEL +%.0f L", it->value); break;
    case E_ENGINE: snprintf(e, sizeof e, "ENGINE +%.0f", it->value); break;
    case E_TIRE: snprintf(e, sizeof e, "TYRE +%.0f", it->value); break;
    case E_BATTERY: snprintf(e, sizeof e, "NEW BATTERY"); break;
    case E_RADIATOR: snprintf(e, sizeof e, "COOLANT +%.0f", it->value); break;
    case E_CURE: snprintf(e, sizeof e, "SICKNESS -%.0f", it->value); break;
    default: break;
    }
    if (e[0]) { text(e, r.x + 16 * S, y + 4 * S, 13 * S, GOOD, 0); y += 22 * S; }
    if (it->sick > 0) { snprintf(b, sizeof b, "MIGHT MAKE YOU SICK (%.0f%%)", it->sick * 100); text(b, r.x + 16 * S, y + 4 * S, 12 * S, DANGER, 0); }
}

static void pack_screen(void)
{
    Rectangle f = frame_rect();
    panel(f, PANEL);
    tabs(f);
    if (close_button(f)) { ui_screen = UI_NONE; return; }
    char b[64];
    float wt = pack_weight(), cap = pack_capacity();
    snprintf(b, sizeof b, "%.1f / %.0f KG", wt, cap);
    text(b, f.x + f.width - 70 * S, f.y + 22 * S, 14 * S, wt > cap ? DANGER : DIM, 2);
    float gx = f.x + 20 * S, gy = f.y + 66 * S, t = fminf((f.width * 0.56f) / 6, (f.height - 90 * S) / 4) - 8 * S;
    for (int i = 0; i < PACK_SLOTS; i++) {
        Rectangle r = rect(gx + (i % 6) * (t + 8 * S), gy + (i / 6) * (t + 8 * S), t, t);
        item_tile(r, pack[i], sel_slot == i, false);
        if (clicked(r)) sel_slot = pack[i].n > 0 ? i : -1;
    }
    Rectangle d = rect(gx + 6 * (t + 8 * S) + 8 * S, gy, f.x + f.width - (gx + 6 * (t + 8 * S) + 28 * S), f.height - 90 * S);
    panel(d, PANEL2);
    if (sel_slot >= 0 && pack[sel_slot].n <= 0) sel_slot = -1;
    int it = sel_slot >= 0 ? pack[sel_slot].item : -1;
    details(d, it, sel_slot >= 0 ? pack[sel_slot].n : 0);
    if (it >= 0) {
        float bw = (d.width - 40 * S) / 3, by = d.y + d.height - 56 * S;
        bool usable = items[it].effect != E_NONE && items[it].effect != E_WEAPON;
        if (!strcmp(items[it].id, "wood") || !strcmp(items[it].id, "rod")) usable = true;
        if (button(rect(d.x + 10 * S, by, bw, 44 * S), items[it].effect == E_LIGHT && items[it].value <= 1 ? (sv.torch_on ? "LIGHT OFF" : "LIGHT ON") : "USE", usable))
            item_use(it);
        if (button(rect(d.x + 20 * S + bw, by, bw, 44 * S), "DROP", true)) pack_drop(sel_slot);
        bool near_car = sv.home_car >= 0 && cars[sv.home_car].used && Vector2Distance((Vector2){ cars[sv.home_car].pos.x, cars[sv.home_car].pos.z }, (Vector2){ peds[PLAYER].pos.x, peds[PLAYER].pos.z }) < 8;
        if (button(rect(d.x + 30 * S + 2 * bw, by, bw, 44 * S), "TO TRUNK", near_car)) cargo_move(true, sel_slot);
    }
}

static void craft_screen(void)
{
    Rectangle f = frame_rect();
    panel(f, PANEL);
    tabs(f);
    if (close_button(f)) { ui_screen = UI_NONE; return; }
    float y0 = f.y + 66 * S, rh = 64 * S;
    float *sy = &scroll_y[UI_CRAFT];
    Rectangle list = rect(f.x + 16 * S, y0, f.width - 32 * S, f.height - 82 * S);
    if (hover(list)) *sy -= GetMouseWheelMove() * 60 * S;
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && hover(list)) *sy -= GetMouseDelta().y;
    float maxs = fmaxf(0, nrecipes * (rh + 6 * S) - list.height);
    *sy = Clamp(*sy, 0, maxs);
    BeginScissorMode((int)list.x, (int)list.y, (int)list.width, (int)list.height);
    for (int r = 0; r < nrecipes; r++) {
        Recipe *rc = &recipes[r];
        float y = y0 + r * (rh + 6 * S) - *sy;
        if (y > list.y + list.height || y + rh < list.y) continue;
        Rectangle row = rect(list.x, y, list.width, rh);
        bool can = recipe_can(r);
        DrawRectangleRounded(row, 0.12f, 4, (Color){ 255, 255, 255, (unsigned char)(can ? 22 : 10) });
        icon(items[rc->out].icon, (Vector2){ row.x + 32 * S, y + rh / 2 }, 40 * S, hexc(items[rc->out].color));
        char b[64];
        snprintf(b, sizeof b, "%s%s", items[rc->out].name, rc->out_n > 1 ? "" : "");
        if (rc->out_n > 1) snprintf(b, sizeof b, "%s X%d", items[rc->out].name, rc->out_n);
        text(b, row.x + 64 * S, y + 12 * S, 15 * S, can ? INK : DIM, 0);
        float x = row.x + 64 * S;
        for (int i = 0; i < rc->nin; i++) {
            int have = pack_count(rc->in[i]);
            snprintf(b, sizeof b, "%d %s", rc->in_n[i], items[rc->in[i]].name);
            text(b, x, y + 38 * S, 11 * S, have >= rc->in_n[i] ? GOOD : DANGER, 0);
            x += text_w(b, 11 * S) + 16 * S;
        }
        if (rc->tool >= 0) { snprintf(b, sizeof b, "TOOL %s", items[rc->tool].name); text(b, x, y + 38 * S, 11 * S, pack_count(rc->tool) > 0 ? GOOD : DANGER, 0); x += text_w(b, 11 * S) + 16 * S; }
        if (rc->fire) text("AT A FIRE", x, y + 38 * S, 11 * S, sv.camp_fire > 0 ? GOOD : WARN, 0);
        if (button(rect(row.x + row.width - 130 * S, y + 12 * S, 118 * S, rh - 24 * S), "MAKE", can)) recipe_make(r);
    }
    EndScissorMode();
    if (nrecipes == 0) text("Nothing to make", W / 2, H / 2, 16 * S, DIM, 1);
}

static void cond_bar(float x, float y, float w, const char *label, float v, const char *val)
{
    text(label, x, y, 12 * S, DIM, 0);
    bar(rect(x + 110 * S, y, w - 170 * S, 12 * S), v, v < 0.2f ? DANGER : v < 0.5f ? WARN : GOOD);
    text(val, x + w, y, 12 * S, INK, 2);
}

static void car_screen(void)
{
    Rectangle f = frame_rect();
    panel(f, PANEL);
    tabs(f);
    if (close_button(f)) { ui_screen = UI_NONE; return; }
    int ci = car_view;
    if (ci < 0 || !cars[ci].used) {
        text("No car. Find one on the road, fix it up and claim it.", W / 2, H / 2, 16 * S, DIM, 1);
        return;
    }
    Car *c = &cars[ci];
    bool home = ci == sv.home_car;
    float x = f.x + 24 * S, y = f.y + 70 * S, w = f.width * 0.38f;
    char b[64];
    snprintf(b, sizeof b, "%s%s", car_name(c->model), home ? "  (YOURS)" : "");
    text(b, x, y, 20 * S, home ? SAND : INK, 0);
    y += 40 * S;
    snprintf(b, sizeof b, "%.0f / %.0f L", c->fuel, c->fuel_max); cond_bar(x, y, w, "FUEL", c->fuel / c->fuel_max, b); y += 26 * S;
    snprintf(b, sizeof b, "%.0f%%", c->engine); cond_bar(x, y, w, "ENGINE", c->engine / 100, b); y += 26 * S;
    snprintf(b, sizeof b, "%.0f%%", c->radiator); cond_bar(x, y, w, "COOLANT", c->radiator / 100, b); y += 26 * S;
    snprintf(b, sizeof b, "%.0f%%", c->battery); cond_bar(x, y, w, "BATTERY", c->battery / 100, b); y += 26 * S;
    static const char *tn[4] = { "TYRE FL", "TYRE FR", "TYRE RL", "TYRE RR" };
    for (int t = 0; t < 4; t++) { snprintf(b, sizeof b, c->tire[t] <= 0 ? "FLAT" : "%.0f%%", c->tire[t]); cond_bar(x, y, w, tn[t], c->tire[t] / 100, b); y += 26 * S; }
    snprintf(b, sizeof b, "%.0f", c->health); cond_bar(x, y, w, "BODY", c->health / 1000, b); y += 26 * S;
    text(c->armor ? "ARMOURED" : "NO ARMOUR", x, y, 12 * S, c->armor ? GOOD : DIM, 0); y += 30 * S;
    if (c->ai == AI_WRECK) text("BURNT OUT: SCRAP ONLY", x, y, 14 * S, DANGER, 0);
    float bw = w * 0.48f;
    float by = f.y + f.height - 112 * S;
    if (!home) {
        if (button(rect(x, by, bw, 44 * S), "SIPHON FUEL", c->fuel > 0.5f)) car_siphon(ci);
        if (button(rect(x + bw + 10 * S, by, bw, 44 * S), "STRIP PARTS", true)) car_strip(ci);
        if (button(rect(x, by + 52 * S, bw * 2 + 10 * S, 44 * S), "MAKE THIS MY CAR", c->ai != AI_WRECK)) car_claim(ci);
    } else {
        text("Use parts from the pack to repair it.", x, by + 8 * S, 13 * S, DIM, 0);
        text("Tap trunk items to take them, pack items to store them.", x, by + 30 * S, 13 * S, DIM, 0);
    }
    if (!home) return;
    // trunk and pack side by side
    float gx = f.x + f.width * 0.46f, gy = f.y + 70 * S, t = fminf((f.width * 0.5f) / 8 - 6 * S, 56 * S);
    text("TRUNK", gx, gy - 4 * S, 13 * S, SAND, 0);
    gy += 16 * S;
    for (int i = 0; i < CARGO_SLOTS; i++) {
        Rectangle r = rect(gx + (i % 8) * (t + 6 * S), gy + (i / 8) * (t + 6 * S), t, t);
        item_tile(r, cargo[i], false, false);
        if (clicked(r) && cargo[i].n > 0) cargo_move(false, i);
    }
    float py = gy + 5 * (t + 6 * S) + 22 * S;
    text("PACK", gx, py - 18 * S, 13 * S, SAND, 0);
    for (int i = 0; i < PACK_SLOTS; i++) {
        Rectangle r = rect(gx + (i % 8) * (t + 6 * S), py + (i / 8) * (t + 6 * S), t, t);
        item_tile(r, pack[i], false, false);
        if (clicked(r) && pack[i].n > 0) cargo_move(true, i);
    }
}

static void trade_screen(void)
{
    Rectangle f = frame_rect();
    panel(f, PANEL);
    if (close_button(f)) { ui_screen = UI_NONE; return; }
    text("TRADER", f.x + 24 * S, f.y + 20 * S, 20 * S, SAND, 0);
    char b[64];
    snprintf(b, sizeof b, "YOUR DOLLARS: %d", pack_count(item_find("cash")));
    text(b, f.x + f.width - 70 * S, f.y + 22 * S, 15 * S, GOOD, 2);
    float t = fminf(f.width * 0.42f / 4 - 8 * S, 86 * S);
    float gx = f.x + 24 * S, gy = f.y + 80 * S;
    text("BUY (TAP)", gx, gy - 22 * S, 13 * S, DIM, 0);
    for (int i = 0; i < 12; i++) {
        Rectangle r = rect(gx + (i % 4) * (t + 8 * S), gy + (i / 4) * (t + 26 * S), t, t);
        item_tile(r, trade_stock[i], false, false);
        if (trade_stock[i].n > 0) {
            snprintf(b, sizeof b, "$%d", item_price(trade_stock[i].item, false));
            text(b, r.x + t / 2, r.y + t + 4 * S, 11 * S, WARN, 1);
            if (hover(r)) text(items[trade_stock[i].item].name, f.x + f.width / 2, f.y + f.height - 40 * S, 15 * S, INK, 1);
        }
        if (clicked(r)) trade_buy(i);
    }
    float sx = f.x + f.width * 0.52f;
    text("SELL (TAP)", sx, gy - 22 * S, 13 * S, DIM, 0);
    float ts = fminf(f.width * 0.44f / 6 - 8 * S, 70 * S);
    for (int i = 0; i < PACK_SLOTS; i++) {
        Rectangle r = rect(sx + (i % 6) * (ts + 8 * S), gy + (i / 6) * (ts + 24 * S), ts, ts);
        item_tile(r, pack[i], false, false);
        if (pack[i].n > 0 && strcmp(items[pack[i].item].id, "cash")) {
            snprintf(b, sizeof b, "$%d", item_price(pack[i].item, true));
            text(b, r.x + ts / 2, r.y + ts + 4 * S, 11 * S, GOOD, 1);
            if (hover(r)) text(items[pack[i].item].name, f.x + f.width / 2, f.y + f.height - 40 * S, 15 * S, INK, 1);
        }
        if (clicked(r)) trade_sell(i);
    }
}

static void loot_screen(void)
{
    float w = fminf(W * 0.7f, 760 * S), h = 300 * S;
    Rectangle f = rect(W / 2 - w / 2, H / 2 - h / 2, w, h);
    panel(f, PANEL);
    if (close_button(f)) { ui_screen = UI_NONE; loot_ci = -1; return; }
    text("FOUND", f.x + 24 * S, f.y + 20 * S, 20 * S, SAND, 0);
    float t = (w - 48 * S) / 8 - 8 * S;
    bool any = false;
    for (int i = 0; i < 8; i++) {
        Rectangle r = rect(f.x + 24 * S + i * (t + 8 * S), f.y + 70 * S, t, t);
        item_tile(r, loot_slots[i], false, false);
        if (loot_slots[i].n > 0) {
            any = true;
            if (hover(r)) text(items[loot_slots[i].item].name, f.x + w / 2, f.y + 80 * S + t, 15 * S, INK, 1);
        }
        if (clicked(r)) loot_take(i);
    }
    if (!any) text("Nothing useful left", f.x + w / 2, f.y + 90 * S + t, 15 * S, DIM, 1);
    if (button(rect(f.x + w / 2 - 110 * S, f.y + h - 64 * S, 220 * S, 48 * S), "TAKE ALL", any)) { loot_take_all(); bool left = false; for (int i = 0; i < 8; i++) if (loot_slots[i].n > 0) left = true; if (!left) { ui_screen = UI_NONE; loot_ci = -1; } }
}

typedef struct { char name[32]; float km; } LogEntry;
static LogEntry tlog[64];
static int ntlog;

static void map_screen(void)
{
    Rectangle f = frame_rect();
    panel(f, PANEL);
    tabs(f);
    if (close_button(f)) { ui_screen = UI_NONE; return; }
    // the road around us, from the road function itself
    Rectangle m = rect(f.x + 16 * S, f.y + 64 * S, f.width * 0.6f, f.height - 80 * S);
    DrawRectangleRounded(m, 0.04f, 4, (Color){ 30, 28, 24, 255 });
    Vector3 pp = peds[PLAYER].car >= 0 ? cars[peds[PLAYER].car].pos : peds[PLAYER].pos;
    float s0 = road_s(pp);
    float span = 5000;
    Vector3 c = road_at(s0, NULL);
    float sc = fminf(m.width, m.height) / span * 0.9f;
    Vector2 mc = { m.x + m.width / 2, m.y + m.height / 2 };
    Vector2 prev = { 0 };
    for (float s = fmaxf(0, s0 - span * 0.6f); s < s0 + span * 0.6f; s += 40) {
        Vector3 q = road_at(s, NULL);
        Vector2 sp = { mc.x + (q.x - c.x) * sc * -1, mc.y - (q.z - c.z) * sc };
        if (s > fmaxf(0, s0 - span * 0.6f)) DrawLineEx(prev, sp, 3 * S, s < s0 ? (Color){ 150, 140, 120, 255 } : (Color){ 220, 210, 190, 255 });
        prev = sp;
    }
    for (int i = 0; i < MAX_STOPS; i++) {
        if (!stops[i].used) continue;
        Vector2 sp = { mc.x + (stops[i].p.x - c.x) * sc * -1, mc.y - (stops[i].p.z - c.z) * sc };
        if (!CheckCollisionPointRec(sp, m)) continue;
        DrawCircleV(sp, 6 * S, stops[i].hostile && stops[i].visited ? DANGER : stops[i].visited ? GOOD : WARN);
        text(stops[i].name, sp.x + 10 * S, sp.y - 6 * S, 10 * S, INK, 0);
    }
    if (sv.home_car >= 0 && cars[sv.home_car].used) {
        Vector3 q = cars[sv.home_car].pos;
        Vector2 sp = { mc.x + (q.x - c.x) * sc * -1, mc.y - (q.z - c.z) * sc };
        DrawRectangle((int)(sp.x - 5 * S), (int)(sp.y - 4 * S), (int)(10 * S), (int)(8 * S), SAND);
    }
    DrawCircleV(mc, 7 * S, ACCENT);
    DrawRing(mc, 9 * S, 11 * S, 0, 360, 24, WHITE);
    // the log
    float x = m.x + m.width + 20 * S, y = m.y;
    char b[80];
    snprintf(b, sizeof b, "KM DRIVEN  %.1f", sv.km); text(b, x, y, 16 * S, SAND, 0); y += 28 * S;
    snprintf(b, sizeof b, "DAYS  %.1f", sv.days); text(b, x, y, 13 * S, INK, 0); y += 22 * S;
    snprintf(b, sizeof b, "STOPS VISITED  %d", sv.stops_seen); text(b, x, y, 13 * S, INK, 0); y += 22 * S;
    snprintf(b, sizeof b, "RAIDERS DOWN  %d", sv.kills); text(b, x, y, 13 * S, INK, 0); y += 22 * S;
    snprintf(b, sizeof b, "THINGS MADE  %d", sv.crafted); text(b, x, y, 13 * S, INK, 0); y += 22 * S;
    snprintf(b, sizeof b, "DANGER  %.0f%%", threat * 100); text(b, x, y, 13 * S, threat > 0.6f ? DANGER : WARN, 0); y += 34 * S;
    text("ALONG THE WAY", x, y, 13 * S, DIM, 0); y += 22 * S;
    for (int i = ntlog - 1; i >= 0 && y < f.y + f.height - 20 * S; i--) {
        snprintf(b, sizeof b, "%5.1f  %s", tlog[i].km, tlog[i].name);
        text(b, x, y, 11 * S, INK, 0);
        y += 18 * S;
    }
}

void hud_log_stop(const char *name, float km)
{
    if (ntlog > 0 && !strcmp(tlog[ntlog - 1].name, name)) return;
    if (ntlog == 64) { memmove(tlog, tlog + 1, 63 * sizeof tlog[0]); ntlog = 63; }
    snprintf(tlog[ntlog].name, sizeof tlog[0].name, "%s", name);
    tlog[ntlog].km = km;
    ntlog++;
}

static bool toggle_row(float x, float y, float w, const char *label, bool v)
{
    text(label, x, y + 10 * S, 15 * S, INK, 0);
    return button(rect(x + w - 130 * S, y, 130 * S, 40 * S), v ? "ON" : "OFF", true);
}

extern void render_set_quality(int q);
extern float audio_volume;

static void settings_screen(void)
{
    Rectangle f = frame_rect();
    panel(f, PANEL);
    if (close_button(f)) { ui_screen = mode == MODE_TITLE ? UI_TITLE : UI_PAUSE; settings_save(); return; }
    text("SETTINGS", f.x + 24 * S, f.y + 20 * S, 22 * S, SAND, 0);
    float x = f.x + 40 * S, y = f.y + 80 * S, w = f.width * 0.42f;
    static const char *qn[3] = { "LOW", "MEDIUM", "HIGH" };
    text("GRAPHICS", x, y + 10 * S, 15 * S, INK, 0);
    if (button(rect(x + w - 130 * S, y, 130 * S, 40 * S), qn[quality], true)) render_set_quality((quality + 1) % 3);
    y += 54 * S;
    if (toggle_row(x, y, w, "STEER BY TILTING", tilt_steer)) tilt_steer = !tilt_steer;
    y += 54 * S;
    if (toggle_row(x, y, w, "TILT FORWARD TO GO", tilt_walk)) tilt_walk = !tilt_walk;
    y += 54 * S;
    if (toggle_row(x, y, w, "INVERT TILT", tilt_invert)) tilt_invert = !tilt_invert;
    y += 54 * S;
    char b[32];
    snprintf(b, sizeof b, "%.1fX", tilt_sens);
    text("TILT SENSITIVITY", x, y + 10 * S, 15 * S, INK, 0);
    if (button(rect(x + w - 270 * S, y, 60 * S, 40 * S), "-", true)) tilt_sens = fmaxf(0.4f, tilt_sens - 0.1f);
    text(b, x + w - 170 * S, y + 11 * S, 15 * S, INK, 1);
    if (button(rect(x + w - 130 * S, y, 60 * S, 40 * S), "+", true)) tilt_sens = fminf(2.5f, tilt_sens + 0.1f);
    y += 54 * S;
    if (button(rect(x, y, w, 44 * S), tilt_available() ? "CALIBRATE TILT (HOLD THE PHONE HOW YOU LIKE IT)" : "NO TILT SENSOR ON THIS DEVICE", tilt_available())) { tilt_calibrate(); hud_msg("Tilt calibrated", 2); }
    float x2 = f.x + f.width * 0.55f, y2 = f.y + 80 * S;
    snprintf(b, sizeof b, "%.0f%%", audio_volume * 100);
    text("VOLUME", x2, y2 + 10 * S, 15 * S, INK, 0);
    if (button(rect(x2 + w - 270 * S, y2, 60 * S, 40 * S), "-", true)) audio_volume = fmaxf(0, audio_volume - 0.1f);
    text(b, x2 + w - 170 * S, y2 + 11 * S, 15 * S, INK, 1);
    if (button(rect(x2 + w - 130 * S, y2, 60 * S, 40 * S), "+", true)) audio_volume = fminf(1.5f, audio_volume + 0.1f);
    y2 += 54 * S;
    if (toggle_row(x2, y2, w, "TOUCH CONTROLS", touch_ui)) touch_ui = !touch_ui;
    y2 += 54 * S;
    snprintf(b, sizeof b, "LONG ROAD %s", GAME_VERSION);
    text(b, x2, y2 + 20 * S, 12 * S, DIM, 0);
}

static void mods_screen(void)
{
    Rectangle f = frame_rect();
    panel(f, PANEL);
    if (close_button(f)) { ui_screen = mode == MODE_TITLE ? UI_TITLE : UI_PAUSE; return; }
    text("MODS", f.x + 24 * S, f.y + 20 * S, 22 * S, SAND, 0);
    char b[300];
    snprintf(b, sizeof b, "FOLDER: %s", mods_dir());
    text(b, f.x + 24 * S, f.y + 58 * S, 11 * S, DIM, 0);
    text("Changes apply when you start a new trip or continue one.", f.x + 24 * S, f.y + 78 * S, 12 * S, DIM, 0);
    float y = f.y + 110 * S;
    for (int i = 0; i < mods_count() && y < f.y + f.height - 120 * S; i++) {
        text(mod_name(i), f.x + 40 * S, y + 12 * S, 15 * S, INK, 0);
        if (button(rect(f.x + f.width - 200 * S, y, 150 * S, 40 * S), mod_enabled(i) ? "ON" : "OFF", true)) mod_toggle(i);
        y += 50 * S;
    }
    if (mods_count() == 0) text("No mods found. Put .txt mod files in the folder above.", W / 2, y + 20 * S, 14 * S, DIM, 1);
#ifdef PLATFORM_WEB
    if (button(rect(f.x + 40 * S, f.y + f.height - 70 * S, 300 * S, 48 * S), "IMPORT A MOD FILE", true)) {
        extern void web_pick_mod(void);
        web_pick_mod();
    }
#endif
    if (data_errors[0]) wrap(data_errors, f.x + f.width * 0.45f, f.y + f.height - 100 * S, f.width * 0.5f, 10 * S, WARN);
}

static void help_screen(void)
{
    Rectangle f = frame_rect();
    panel(f, PANEL);
    if (close_button(f)) { ui_screen = mode == MODE_TITLE ? UI_TITLE : UI_PAUSE; return; }
    text("HOW TO SURVIVE THE LONG ROAD", f.x + 24 * S, f.y + 20 * S, 20 * S, SAND, 0);
    const char *lines[] = {
        "Drive as far as you can. Every stop has something: fuel, food, parts, people.",
        "Keep food, water, rest and warmth up. Empty bars hurt. Cold nights are dangerous.",
        "Fuel runs out. Hand-pump station tanks, siphon wrecks, carry jerry cans in the trunk.",
        "Cars wear out. Patch tyres, top up coolant, swap batteries, rebuild engines.",
        "Search shelves, fridges, lockers, trunks and bodies. Trade with traders for what you need.",
        "Build a fire with two firewood and a lighter. Cook meat, boil dirty water, sleep warm.",
        "Raiders hold some stops and hunt the highway. Watch the mirror. Wolves come out at night.",
        "",
        "PHONE: tilt like a wheel to steer, tilt forward to drive or walk. Drag the right side to look.",
        "KEYBOARD: WASD move, mouse look, E use, F car, TAB pack, Q/E weapon, R reload,",
        "SPACE jump or handbrake, L lights, H horn, V camera, M trip map, ESC pause.",
        "GAMEPAD: sticks move and look, triggers drive, Y car, X reload, A jump.",
    };
    float y = f.y + 70 * S;
    for (int i = 0; i < (int)(sizeof lines / sizeof *lines); i++) { text(lines[i], f.x + 30 * S, y, 13 * S, i >= 8 ? DIM : INK, 0); y += 26 * S; }
}

static void pause_screen(void)
{
    DrawRectangle(0, 0, W, H, (Color){ 0, 0, 0, 120 });
    float w = 340 * S, x = W / 2 - w / 2, y = H * 0.22f;
    text("PAUSED", W / 2, y - 60 * S, 34 * S, SAND, 1);
    if (button(rect(x, y, w, 52 * S), "RESUME", true) || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACK)) ui_screen = UI_NONE;
    if (button(rect(x, y + 64 * S, w, 52 * S), "PACK", true)) ui_screen = UI_PACK;
    if (button(rect(x, y + 128 * S, w, 52 * S), "SETTINGS", true)) ui_screen = UI_SETTINGS;
    if (button(rect(x, y + 192 * S, w, 52 * S), "MODS", true)) ui_screen = UI_MODS;
    if (button(rect(x, y + 256 * S, w, 52 * S), "HOW TO PLAY", true)) ui_screen = UI_HELP;
    if (button(rect(x, y + 320 * S, w, 52 * S), "SAVE AND QUIT", true)) { game_save(); mode = MODE_TITLE; ui_screen = UI_TITLE; }
}

extern bool want_new_game, want_continue, want_quit;

static void title_screen(void)
{
    DrawRectangleGradientV(0, 0, W, H, (Color){ 0, 0, 0, 40 }, (Color){ 0, 0, 0, 170 });
    float y = H * 0.16f;
    text("LONG ROAD", W * 0.08f, y, 64 * S, SAND, 0);
    text("A HIGHWAY SURVIVAL GAME", W * 0.08f + 4 * S, y + 80 * S, 16 * S, INK, 0);
    float w = 340 * S, x = W * 0.08f, by = H * 0.42f;
    bool save = game_has_save();
    if (button(rect(x, by, w, 54 * S), "CONTINUE THE TRIP", save)) want_continue = true;
    if (button(rect(x, by + 64 * S, w, 54 * S), "NEW TRIP", true)) want_new_game = true;
    if (button(rect(x, by + 128 * S, w, 54 * S), "SETTINGS", true)) ui_screen = UI_SETTINGS;
    if (button(rect(x, by + 192 * S, w, 54 * S), "MODS", true)) ui_screen = UI_MODS;
    if (button(rect(x, by + 256 * S, w, 54 * S), "HOW TO PLAY", true)) ui_screen = UI_HELP;
#if defined(PLATFORM_DESKTOP)
    if (button(rect(x, by + 320 * S, w, 54 * S), "QUIT", true)) want_quit = true;
#endif
    char b[64];
    snprintf(b, sizeof b, "%s  %d MODS ON", GAME_VERSION, ({ int n = 0; for (int i = 0; i < mods_count(); i++) n += mod_enabled(i); n; }));
    text(b, W - 20 * S, H - 30 * S, 11 * S, DIM, 2);
}

static void dead_screen(void)
{
    DrawRectangle(0, 0, W, H, (Color){ 30, 0, 0, 150 });
    text("THE ROAD ENDS HERE", W / 2, H * 0.2f, 40 * S, (Color){ 240, 90, 70, 255 }, 1);
    char b[96];
    float y = H * 0.36f;
    snprintf(b, sizeof b, "YOU MADE IT %.1f KM IN %.1f DAYS", sv.km, sv.days); text(b, W / 2, y, 20 * S, INK, 1); y += 40 * S;
    snprintf(b, sizeof b, "%d STOPS  %d RAIDERS  %d THINGS MADE", sv.stops_seen, sv.kills, sv.crafted); text(b, W / 2, y, 15 * S, DIM, 1); y += 30 * S;
    int best = 0;
    char *t = plat_get("best_km");
    if (t) { best = atoi(t); free(t); }
    snprintf(b, sizeof b, "BEST TRIP %.1f KM", fmaxf(best / 10.0f, sv.km)); text(b, W / 2, y, 15 * S, SAND, 1);
    float w = 320 * S;
    if (button(rect(W / 2 - w / 2, H * 0.62f, w, 54 * S), "NEW TRIP", true)) want_new_game = true;
    if (button(rect(W / 2 - w / 2, H * 0.62f + 64 * S, w, 54 * S), "TITLE", true)) { mode = MODE_TITLE; ui_screen = UI_TITLE; }
}

// ---------------------------------------------------------------- frame

void hud_input(const Input *in)
{
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { press_at = GetMousePosition(); press_live = true; }
    press_used = false;
    if (mode == MODE_TITLE || mode == MODE_DEAD) return;
    if (in->hit & BTN_PAUSE) {
        if (ui_screen == UI_NONE) ui_screen = UI_PAUSE;
        else if (ui_screen != UI_PAUSE) { ui_screen = UI_NONE; loot_ci = -1; }
    }
    if (in->hit & BTN_BAG) ui_screen = ui_screen == UI_PACK ? UI_NONE : UI_PACK;
    if (in->hit & BTN_MAP) ui_screen = ui_screen == UI_MAP ? UI_NONE : UI_MAP;
    if ((in->hit & BTN_USE) && ui_screen == UI_NONE) survive_interact();
    else if ((in->hit & BTN_USE) && ui_screen == UI_LOOT) loot_take_all();
}

void hud_draw(int w, int h)
{
    W = w; H = h;
    S = fminf(h / 720.0f, w / 1280.0f) * (is_touch_device() ? 1.15f : 1.0f);
    ui_blocks_game = ui_screen != UI_NONE;
    if (mode == MODE_TITLE) {
        if (ui_screen == UI_SETTINGS) settings_screen();
        else if (ui_screen == UI_MODS) mods_screen();
        else if (ui_screen == UI_HELP) help_screen();
        else { ui_screen = UI_TITLE; title_screen(); }
        return;
    }
    if (sleep_fade > 0) DrawRectangle(0, 0, W, H, (Color){ 0, 0, 0, (unsigned char)(255 * Clamp(sleep_fade, 0, 1)) });
    if (mode == MODE_DEAD) {
        static float dead_t;
        dead_t += GetFrameTime();
        if (peds[PLAYER].state == PS_DEAD && pv[PLAYER].dead_s > 2.5f) { ui_screen = UI_DEAD; dead_screen(); }
        messages();
        return;
    }
    if (ui_screen == UI_NONE || ui_screen == UI_PAUSE) {
        survival_panel();
        world_panel();
        next_stop();
        car_gauges();
        weapon_box();
        crosshair();
        search_ring();
        danger_overlay();
        prompt_line();
        messages();
        draw_touch();
    }
    switch (ui_screen) {
    case UI_PACK: pack_screen(); break;
    case UI_CRAFT: craft_screen(); break;
    case UI_CAR: car_screen(); break;
    case UI_TRADE: trade_screen(); break;
    case UI_LOOT: loot_screen(); break;
    case UI_MAP: map_screen(); break;
    case UI_PAUSE: pause_screen(); break;
    case UI_SETTINGS: settings_screen(); break;
    case UI_MODS: mods_screen(); break;
    case UI_HELP: help_screen(); break;
    default: break;
    }
    if (ui_screen != UI_NONE && ui_screen != UI_PAUSE) messages();
}
