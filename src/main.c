#include "estate.h"
// Long Road: the frame loop, input, starting and ending trips.
#include "game.h"
#include "ped.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef PLATFORM_WEB
#include <emscripten.h>
#endif

int mode = MODE_TITLE;
bool audio_offline;
float game_s;
uint64_t ticks;
uint32_t world_seed;
WorldKind world_kind, next_world;
int difficulty = DIFF_PEACEFUL;
bool demo_active, want_demo;
const char *const world_names[WORLD_COUNT] = { "LONG ROAD", "METRO GRID", "PINE COUNTRY", "FROSTLINE", "RED MESA" };
const char *const world_ids[WORLD_COUNT] = { "road", "city", "forest", "snow", "mesa" };
bool want_new_game, want_continue, want_quit;
float audio_volume = 1;

static uint32_t rng = 0x9e3779b9;

uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

float frand(float a, float b) { return a + (b - a) * (rnd() >> 8) * (1.0f / 16777216.0f); }

void hud_log_stop(const char *name, float km);
void player_init(void);
void render_set_quality(int q);

// ---------------------------------------------------------------- settings

void settings_save(void)
{
    char b[256];
    snprintf(b, sizeof b, "quality=%d\ntilt_steer=%d\ntilt_walk=%d\ntilt_invert=%d\ntilt_sens=%.2f\nvolume=%.2f\ndifficulty=%d\nambience=%d\nvehicle_audio=%d\nfemale_avatar=%d\n",
             quality, tilt_steer, tilt_walk, tilt_invert, tilt_sens, audio_volume, difficulty, ambience_enabled, vehicle_audio_enabled, female_avatar);
    plat_put("settings", b);
}

static void settings_load(void)
{
    char *t = plat_get("settings");
    if (!t) return;
    int q, a, b, c;
    float s, v;
    char *p;
    if ((p = strstr(t, "quality=")) && sscanf(p, "quality=%d", &q) == 1) quality = q < 0 ? 0 : q > 2 ? 2 : q;
    if ((p = strstr(t, "tilt_steer=")) && sscanf(p, "tilt_steer=%d", &a) == 1) tilt_steer = a;
    if ((p = strstr(t, "tilt_walk=")) && sscanf(p, "tilt_walk=%d", &b) == 1) tilt_walk = b;
    if ((p = strstr(t, "tilt_invert=")) && sscanf(p, "tilt_invert=%d", &c) == 1) tilt_invert = c;
    if ((p = strstr(t, "tilt_sens=")) && sscanf(p, "tilt_sens=%f", &s) == 1) tilt_sens = s;
    if ((p = strstr(t, "volume=")) && sscanf(p, "volume=%f", &v) == 1) audio_volume = v;
    if ((p = strstr(t, "difficulty=")) && sscanf(p, "difficulty=%d", &q) == 1 && q >= 0 && q < DIFF_COUNT) difficulty = q;
    if ((p = strstr(t, "\nambience=")) && sscanf(p, "\nambience=%d", &q) == 1) ambience_enabled = q != 0;
    if ((p = strstr(t, "\nvehicle_audio=")) && sscanf(p, "\nvehicle_audio=%d", &q) == 1) vehicle_audio_enabled = q != 0;
    if ((p = strstr(t, "\nfemale_avatar=")) && sscanf(p, "\nfemale_avatar=%d", &q) == 1) female_avatar = q != 0;
    free(t);
}

// ---------------------------------------------------------------- input

static uint16_t last_held;

static Input read_input(void)
{
    Input in = { 0 };
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) in.mx += 1;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) in.mx -= 1;
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) in.my += 1;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) in.my -= 1;
    uint16_t h = 0;
    bool touch = is_touch_device() || GetTouchPointCount() > 0;
    if (!touch) {
        static int settle;
        static bool was_hidden;
        if (IsCursorHidden() != was_hidden) { was_hidden = IsCursorHidden(); settle = 3; }
        Vector2 md = settle > 0 || !IsCursorHidden() ? (Vector2){ 0 } : GetMouseDelta();
        if (settle > 0) settle--;
        in.lx = md.x * 0.0025f;
        in.ly = md.y * 0.0025f;
        if (IsCursorHidden()) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) h |= BTN_FIRE;
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) h |= BTN_AIM;
            if (GetMouseWheelMove() < 0) h |= BTN_NEXT;
            if (GetMouseWheelMove() > 0) h |= BTN_PREV;
        }
    }
    if (IsKeyDown(KEY_SPACE)) h |= BTN_JUMP;
    if (IsKeyDown(KEY_LEFT_SHIFT)) h |= BTN_SPRINT;
    if (IsKeyDown(KEY_F) || IsKeyDown(KEY_ENTER)) h |= BTN_ENTER;
    if (IsKeyDown(KEY_R)) h |= BTN_RELOAD;
    if (IsKeyDown(KEY_Q)) h |= BTN_NEXT;
    if (IsKeyDown(KEY_E)) h |= BTN_USE;
    if (IsKeyDown(KEY_H)) h |= BTN_HORN;
    if (IsKeyDown(KEY_M)) h |= BTN_MAP;
    if (IsKeyDown(KEY_ESCAPE) || IsKeyDown(KEY_P) || IsKeyDown(KEY_BACK)) h |= BTN_PAUSE;
    if (IsKeyDown(KEY_V)) h |= BTN_CAM;
    if (IsKeyDown(KEY_TAB) || IsKeyDown(KEY_I)) h |= BTN_BAG;
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_C)) h |= BTN_CROUCH;
    if (IsKeyDown(KEY_L)) h |= BTN_LIGHTS;

    if (IsGamepadAvailable(0)) {
        float sx = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X), sy = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        if (fabsf(sx) > 0.15f) in.mx = sx;
        if (fabsf(sy) > 0.15f) in.my = -sy;
        float rx = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_X), ry = GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_Y);
        if (fabsf(rx) > 0.12f) in.lx += rx * 3.0f * DT;
        if (fabsf(ry) > 0.12f) in.ly += ry * 2.0f * DT;
        float rt = (GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_TRIGGER) + 1) * 0.5f;
        float lt = (GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_TRIGGER) + 1) * 0.5f;
        if (peds[PLAYER].car >= 0) { if (rt > 0.05f || lt > 0.05f) in.my = rt - lt; }
        else {
            if (rt > 0.5f) h |= BTN_FIRE;
            if (lt > 0.5f) h |= BTN_AIM;
        }
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) h |= BTN_JUMP;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_UP)) h |= BTN_ENTER;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_LEFT)) h |= BTN_RELOAD;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)) h |= BTN_USE;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) h |= BTN_NEXT;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_TRIGGER_1)) h |= BTN_PREV;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_THUMB)) h |= BTN_SPRINT;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_MIDDLE_LEFT)) h |= BTN_BAG;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_MIDDLE_RIGHT)) h |= BTN_PAUSE;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_THUMB)) h |= BTN_CAM;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_UP)) h |= BTN_LIGHTS;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) h |= BTN_HORN;
    }
    in.held = h;
    return in;
}

// ---------------------------------------------------------------- trips

void respawn(Vector3 at)
{
    Ped *p = &peds[PLAYER];
    p->pos = at;
    p->vel = (Vector3){ 0 };
    p->health = 100;
    p->state = PS_IDLE;
    weapon_stow();
    mode = MODE_PLAY;
}

static Vector3 stop_local(const Stop *s, float x, float z)
{
    float c = cosf(s->yaw), sn = sinf(s->yaw);
    Vector3 p = { s->p.x + x * c + z * sn, 0, s->p.z - x * sn + z * c };
    p.y = ground_y(p.x, p.z);
    return p;
}

void new_game(uint32_t seed)
{
    world_seed = seed;
    rng = seed * 2654435761u + 1;
    memset(cars, 0, sizeof cars);
    for (int i = 0; i < MAX_PEDS; i++) peds[i].used = false;
    peds[PLAYER].health = 100;
    raiders_reset();
    fx_reset();
    world_flags_clear();
    data_load();
    survive_reset(seed);
    world_build(seed);
    // start on the road, beside the first gas station
    Vector3 dir;
    Vector3 at = road_at(world_start_s(), &dir);
    peds[PLAYER].pos = at;
    player_init();
    wardrobe_apply();
    world_stream_all();
    int gas = -1;
    for (int i = 0; i < MAX_STOPS; i++) if (stops[i].used && stops[i].kind == STOP_GAS && stops[i].km < 0.6f) gas = i;
    float yaw = atan2f(dir.x, dir.z);
    Vector3 car_at = Vector3Add(at, (Vector3){ dir.z * (world_kind == WORLD_CITY ? 2 : -6), 0, dir.x * 6 });
    float car_yaw = yaw;
    if (gas >= 0) {
        Stop *s = &stops[gas];
        car_at = stop_local(s, -4.0f, 3.2f);
        car_yaw = s->yaw + PI * 0.5f;
        peds[PLAYER].pos = stop_local(s, 0.8f, 3.2f);
        peds[PLAYER].yaw = s->yaw - PI * 0.5f;
        hud_log_stop(s->name, 0);
        s->visited = true;
        world_set_flag(world_key(s->seg, 15, 0));
    }
    int ci = car_spawn(CAR_WAGON, car_at, car_yaw, AI_NONE);
    if (ci >= 0) {
        Car *c = &cars[ci];
        c->fuel = c->fuel_max * 0.55f;
        c->engine = 88; c->battery = 90; c->radiator = 80;
        for (int w = 0; w < 4; w++) c->tire[w] = 80 + w * 3;
        c->paint = (Color){ 176, 96, 52, 255 };
        c->owned = true;
        sv.home_car = ci;
    }
    peds[PLAYER].pos.y = ground_y(peds[PLAYER].pos.x, peds[PLAYER].pos.z);
    extern float cam_yaw;
    cam_yaw = peds[PLAYER].yaw;
    day_hours = 8.0f;
    game_s = 0;
    ticks = 0;
    weapon_stow();
}

static void tick(const Input *in)
{
    world_tick(DT);
    peds_tick(in, DT);
    cars_tick(DT);
    combat_tick(in, DT);
    raiders_tick(DT);
    survive_tick(DT);
    fx_tick(DT);
    audio_tick(DT);
    camera_update(in, DT);
    day_hours += DT / 60.0f;
    if (day_hours >= 24) day_hours -= 24;
    game_s += DT;
    ticks++;
    // far from the origin: shift everything back, the world keeps its true position
    Vector3 f = peds[PLAYER].car >= 0 ? cars[peds[PLAYER].car].pos : peds[PLAYER].pos;
    if (world_kind != WORLD_CITY && (fabsf(f.x) > 2500 || fabsf(f.z) > 2500)) {
        Vector3 d = { roundf(f.x / 256) * 256, 0, roundf(f.z / 256) * 256 };
        world_rebase(d);
    }
}

// ---------------------------------------------------------------- the frame

static Input pending;
static float acc;
static float title_t;
static bool dead_handled;

static void title_camera(float dt)
{
    title_t += dt;
    Ped *p = &peds[PLAYER];
    Vector3 c = sv.home_car >= 0 && cars[sv.home_car].used ? cars[sv.home_car].pos : p->pos;
    // swing slowly on the road side of the station, never through the buildings
    float base = 0;
    for (int i = 0; i < MAX_STOPS; i++) if (stops[i].used && stops[i].km < 0.6f) base = stops[i].yaw;
    float a = base + 0.75f * sinf(title_t * 0.05f);
    cam.position = (Vector3){ c.x + sinf(a) * 15, c.y + 4.2f, c.z + cosf(a) * 15 };
    cam.position.y = fmaxf(cam.position.y, ground_y(cam.position.x, cam.position.z) + 2.5f);
    cam.target = (Vector3){ c.x, c.y + 1.2f, c.z };
    cam.up = (Vector3){ 0, 1, 0 };
    cam.fovy = 52;
}

static void start_play(void)
{
    mode = MODE_PLAY;
    ui_screen = UI_NONE;
    dead_handled = false;
    tilt_calibrate();
    acc = 0;
    memset(&pending, 0, sizeof pending);
    // a few quiet ticks so the camera and the people settle
    Input none = { 0 };
    for (int i = 0; i < 4; i++) tick(&none);
}

static void frame(void)
{
    Input now = read_input();
    hud_touch_update(&now, GetScreenWidth(), GetScreenHeight());
    now.hit = now.held & ~last_held;
    last_held = now.held;
    hud_input(&now);
    if (demo_active && ui_screen == UI_LOOT) {
        loot_take_all();
        ui_screen = UI_NONE;
        loot_ci = -1;
    }
    if (demo_active && (ui_screen == UI_CAR || ui_screen == UI_TRADE)) ui_screen = UI_NONE;

    if (demo_active && mode == MODE_TITLE) demo_active = false;
    if (want_demo) {
        want_demo = false;
        demo_active = true;
        world_kind = next_world;
        new_game((uint32_t)time(NULL) ^ (uint32_t)(GetTime() * 1000003));
        demo_reset();
        start_play();
        hud_big("Autoplay demo", 3);
    }
    if (want_new_game) {
        want_new_game = false;
        demo_active = false;
        world_kind = next_world;
        new_game((uint32_t)time(NULL) ^ (uint32_t)(GetTime() * 1000003));
        start_play();
        if (!game_save()) hud_msg("Could not save this trip", 3);
        hud_big(world_names[world_kind], 3);
    }
    if (want_continue) {
        want_continue = false;
        demo_active = false;
        WorldKind previous = world_kind;
        world_kind = next_world;
        if (game_load()) start_play();
        else { world_kind = previous; hud_big("That save could not be read", 3); }
    }

    if (mode == MODE_TITLE) {
        title_camera(GetFrameTime());
        world_tick(0);
        fx_tick(GetFrameTime() * 0.5f);
        audio_tick(GetFrameTime());
        if (IsCursorHidden()) EnableCursor();
    } else {
        pending.mx = now.mx; pending.my = now.my; pending.held = now.held;
        pending.lx += now.lx; pending.ly += now.ly; pending.hit |= now.hit;
        acc += fminf(GetFrameTime(), 0.1f);
        bool paused = ui_screen != UI_NONE;
        if (paused) {
            acc = 0;
            pending.lx = pending.ly = 0;
            pending.hit = 0;
            audio_tick(0);
        }
        while (acc >= DT && ui_screen == UI_NONE) {
            Input bot = demo_active && mode == MODE_PLAY ? demo_control() : pending;
            tick(&bot);
            pending.lx = pending.ly = 0;
            pending.hit = 0;
            acc -= DT;
        }
        if (mode == MODE_DEAD && !dead_handled) {
            dead_handled = true;
            if (demo_active) { want_demo = true; next_world = world_kind; }
            else game_delete_save();
            int best = 0;
            char *t = plat_get("best_km");
            if (t) { best = atoi(t); free(t); }
            if (!demo_active && (int)(sv.km * 10) > best) { char b[32]; snprintf(b, sizeof b, "%d", (int)(sv.km * 10)); plat_put("best_km", b); }
        }
        // the mouse is captured while driving or walking, freed in menus
        bool want_cursor = paused || mode == MODE_DEAD || is_touch_device();
        if (want_cursor && IsCursorHidden()) EnableCursor();
#if defined(PLATFORM_DESKTOP)
        if (!want_cursor && !IsCursorHidden() && IsWindowFocused()) DisableCursor();
#else
        if (!want_cursor && !IsCursorHidden() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) DisableCursor();
#endif
    }
#if defined(PLATFORM_DESKTOP)
    if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();
#endif

    BeginDrawing();
    render_frame(GetScreenWidth(), GetScreenHeight());
    hud_draw(GetScreenWidth(), GetScreenHeight());
    EndDrawing();
}

// ---------------------------------------------------------------- headless shots for testing: --shot file --at seconds

static void shot(const char *path)
{
    Image img = LoadImageFromScreen();
    ExportImage(img, path);
    UnloadImage(img);
}

int main(int argc, char **argv)
{
    uint32_t seed = 0;
    int w = 1280, h = 720;
    const char *shot_path = NULL, *script = NULL;
    float shot_at = 5, t_hours = -1, t_km = 0;
    const char *t_weather = NULL, *t_ui = NULL;
    int t_quality = -1;
    bool t_touch = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--seed")) { seed = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "--size")) { sscanf(v, "%dx%d", &w, &h); i++; }
        else if (!strcmp(a, "--shot")) { shot_path = v; i++; }
        else if (!strcmp(a, "--at")) { shot_at = (float)atof(v); i++; }
        else if (!strcmp(a, "--play")) script = v[0] && v[0] != '-' ? (i++, v) : "drive";
        else if (!strcmp(a, "--quality")) { t_quality = atoi(v); i++; }
        else if (!strcmp(a, "--hours")) { t_hours = (float)atof(v); i++; }
        else if (!strcmp(a, "--km")) { t_km = (float)atof(v); i++; }
        else if (!strcmp(a, "--world")) {
            for (int k = 0; k < WORLD_COUNT; k++) if (!strcmp(v, world_ids[k])) world_kind = k;
            i++;
        }
        else if (!strcmp(a, "--weather")) { t_weather = v; i++; }
        else if (!strcmp(a, "--ui")) { t_ui = v; i++; }
        else if (!strcmp(a, "--touch")) t_touch = true;
    }
    settings_load();
    if (t_quality >= 0) quality = t_quality;
    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT;
#if defined(PLATFORM_ANDROID)
    flags = FLAG_FULLSCREEN_MODE | FLAG_VSYNC_HINT;
#endif
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(w, h, "Long Road");
    if (!IsWindowReady()) return 1;
    SetExitKey(KEY_NULL);
    SetTargetFPS(shot_path ? 0 : 60);
    platform_init();
    assets_load();
    render_init();
    audio_init();
    data_load();
    new_game(seed ? seed : (uint32_t)time(NULL));
    mode = MODE_TITLE;

    if (shot_path) {
        // test mode: optionally play, then save a frame
        bool play = script != NULL;
        if (play) start_play();
        if (t_hours >= 0) day_hours = t_hours;
        if (t_km > 0 && sv.home_car >= 0) {
            Car *c = &cars[sv.home_car];
            Vector3 d;
            Vector3 at = road_at(world_start_s() + t_km * 1000, &d);
            at = Vector3Add(at, (Vector3){ -d.z * 1.85f, 1.0f, d.x * 1.85f });
            c->pos = at; c->rot = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, atan2f(d.x, d.z)); c->vel = Vector3Scale(d, 15);
            Ped *p = &peds[PLAYER];
            p->car = sv.home_car; p->state = PS_IN_CAR; c->driver = PLAYER; pv[PLAYER].sit = 1; p->pos = at;
            sv.km = t_km;
            world_stream_all();
        }
        if (t_weather) {
            extern void weather_force(const char *w);
            weather_force(t_weather);
        }
        if (t_touch) { extern bool touch_force; touch_force = true; }
        if (getenv("LONGROAD_AMBUSH")) { sv.km = 20; raiders_ambush(); weapon_give(W_SHOTGUN); }
        int frames = (int)(shot_at * 60);
        for (int f = 0; f < frames && !WindowShouldClose(); f++) {
            Input in = { 0 };
            if (play) {
                if (!strcmp(script, "drive")) {
                    if (peds[PLAYER].car < 0 && f % 30 == 0 && f < 300) in.held |= BTN_ENTER, in.hit |= BTN_ENTER;
                    if (peds[PLAYER].car >= 0) {
                        // drive back to the road, then follow it on the right-hand lane
                        Car *c = &cars[peds[PLAYER].car];
                        float s;
                        float off = road_offset(c->pos.x, c->pos.z, &s);
                        Vector3 d;
                        Vector3 aim = road_at(s + 18, &d);
                        aim = Vector3Add(aim, (Vector3){ d.z * -1.8f, 0, -d.x * -1.8f });
                        Vector3 to = Vector3Subtract(aim, c->pos);
                        Vector3 cf = car_fwd(c);
                        float err = atan2f(to.x, to.z) - atan2f(cf.x, cf.z);
                        while (err > PI) err -= 2 * PI;
                        while (err < -PI) err += 2 * PI;
                        in.mx = Clamp(-err * 2.0f, -1, 1);
                        in.my = car_kmh(c) < 70 ? 1 : 0.2f;
                        (void)off;
                    }
                } else if (!strcmp(script, "demo")) {
                    demo_active = true;
                    if (f == 0) demo_reset();
                    if (ui_screen == UI_LOOT) { loot_take_all(); ui_screen = UI_NONE; loot_ci = -1; }
                    in = demo_control();
                } else if (!strcmp(script, "walk")) {
                    in.my = f > 60 ? 0.8f : 0;
                }
                tick(&in);
            } else { title_camera(DT); world_tick(0); }
            if (t_touch) { Input dummy = { 0 }; hud_touch_update(&dummy, GetScreenWidth(), GetScreenHeight()); }
            if (f % 4 == 0 || f == frames - 1) {
                BeginDrawing();
                render_frame(GetScreenWidth(), GetScreenHeight());
                hud_draw(GetScreenWidth(), GetScreenHeight());
                EndDrawing();
            }
        }
        BeginDrawing();
        render_frame(GetScreenWidth(), GetScreenHeight());
        hud_draw(GetScreenWidth(), GetScreenHeight());
        if (t_ui) {
            static const char *names[] = { "none", "pack", "craft", "car", "trade", "map", "pause", "settings", "mods", "title", "dead", "help", "loot", "worlds" };
            for (int k = 0; k < (int)(sizeof names / sizeof *names); k++) if (!strcmp(t_ui, names[k])) ui_screen = k;
            if (ui_screen == UI_CAR) car_view = sv.home_car;
            if (ui_screen == UI_TRADE) { for (int k = 1; k < MAX_PEDS; k++) if (peds[k].used) { trade_open(k); break; } }
            if (ui_screen == UI_LOOT) { cont_drop(peds[PLAYER].pos, CT_CORPSE); for (int k = MAX_CONTS - 1; k >= 0; k--) if (conts[k].used && conts[k].kind == CT_CORPSE) { conts[k].searched = false; extern int loot_force(int); loot_force(k); break; } }
            hud_draw(GetScreenWidth(), GetScreenHeight());
            hud_draw(GetScreenWidth(), GetScreenHeight());
        }
        rlDrawRenderBatchActive();
        {
            Ped *p = &peds[PLAYER];
            fprintf(stderr, "player %.1f %.1f %.1f state %d car %d | cam %.1f %.1f %.1f -> %.1f %.1f %.1f | ground %.2f\n", p->pos.x, p->pos.y, p->pos.z, p->state, p->car,
                    cam.position.x, cam.position.y, cam.position.z, cam.target.x, cam.target.y, cam.target.z, ground_y(p->pos.x, p->pos.z));
            if (p->car >= 0) { Car *c = &cars[p->car]; Vector3 f = car_fwd(c); fprintf(stderr, "car %.1f %.1f %.1f fwd %.2f %.2f %.2f v %.1f fuel %.1f stalled %d\n", c->pos.x, c->pos.y, c->pos.z, f.x, f.y, f.z, car_speed(c), c->fuel, c->stalled); }
        }
        shot(shot_path);
        EndDrawing();
        CloseWindow();
        return 0;
    }

#ifdef PLATFORM_WEB
    emscripten_set_main_loop(frame, 0, 1);
#else
    while (!WindowShouldClose() && !want_quit) frame();
    if (mode == MODE_PLAY) game_save();
    CloseWindow();
#endif
    return 0;
}
