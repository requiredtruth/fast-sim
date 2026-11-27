#include "game.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int mode = MODE_PLAY;
bool audio_offline;
float game_s;
uint64_t ticks;

static uint32_t rng = 0x9e3779b9;
static float mode_s;

uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

float frand(float a, float b) { return a + (b - a) * (rnd() >> 8) * (1.0f / 16777216.0f); }

// scripted input: "<seconds> <cmd> [args]" per line, for tests and footage
typedef struct { float t; char cmd[16]; char arg[48]; float x, y, v[7]; } Cue;
static Cue *cues;
static int ncues, next_cue;
static Input script_in;
static float script_look_x, script_look_y;
static uint16_t script_tap;

static const char *btn_names[] = { "fire", "aim", "jump", "sprint", "enter", "reload", "next", "prev",
                                   "horn", "map", "pause", "cam", "radio", "crouch" };

static uint16_t btn_bit(const char *name)
{
    for (int i = 0; i < 14; i++)
        if (!strcmp(name, btn_names[i])) return 1 << i;
    fprintf(stderr, "script: unknown button %s\n", name);
    return 0;
}

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "can't open %s\n", path); exit(1); }
    char line[128];
    cues = calloc(4096, sizeof *cues);
    while (fgets(line, sizeof line, f) && ncues < 4096) {
        Cue c = {0};
        if (line[0] == '#' || sscanf(line, "%f %15s", &c.t, c.cmd) < 2) continue;
        sscanf(line, "%*f %*s %47s", c.arg);
        sscanf(line, "%*f %*s %f %f", &c.x, &c.y);
        sscanf(line, "%*f %*s %f %f %f %f %f %f %f", &c.v[0], &c.v[1], &c.v[2], &c.v[3], &c.v[4], &c.v[5], &c.v[6]);
        cues[ncues++] = c;
    }
    fclose(f);
}

static const char *shot_dir = "runs";
static int shot_w, shot_h;
static bool want_shot;
static char shot_name[64];

// script time keeps running while paused, game_s doesn't
static float script_s;

static bool script_step(Input *in)
{
    script_tap = 0;
    while (next_cue < ncues && cues[next_cue].t <= script_s + 1e-4f) {
        Cue *c = &cues[next_cue++];
        if (!strcmp(c->cmd, "move")) { script_in.mx = c->x; script_in.my = c->y; }
        else if (!strcmp(c->cmd, "look")) { script_look_x = c->x; script_look_y = c->y; }
        else if (!strcmp(c->cmd, "hold")) script_in.held |= btn_bit(c->arg);
        else if (!strcmp(c->cmd, "release")) script_in.held &= ~btn_bit(c->arg);
        else if (!strcmp(c->cmd, "tap")) script_tap |= btn_bit(c->arg);
        else if (!strcmp(c->cmd, "hours")) day_hours = c->x;
        else if (!strcmp(c->cmd, "shot")) { want_shot = true; snprintf(shot_name, sizeof shot_name, "%s", c->arg); }
        else if (!strcmp(c->cmd, "teleport")) { peds[PLAYER].pos = (Vector3){ c->x, 0, c->y }; }
        else if (!strncmp(c->cmd, "drone", 5)) drone_cmd(c->cmd, c->v);
        else if (!strcmp(c->cmd, "end")) return false;
    }
    *in = script_in;
    in->lx = script_look_x * DT;
    in->ly = script_look_y * DT;
    in->held |= script_tap;
    in->hit = script_tap;
    script_s += DT;
    return true;
}

static uint16_t last_held;

static Input read_input(void)
{
    Input in = {0};
    if (IsKeyDown(KEY_D)) in.mx += 1;
    if (IsKeyDown(KEY_A)) in.mx -= 1;
    if (IsKeyDown(KEY_W)) in.my += 1;
    if (IsKeyDown(KEY_S)) in.my -= 1;
    // the first frames after the cursor is captured report one big jump, which spun the camera
    static int settle;
    static bool was_hidden;
    if (IsCursorHidden() != was_hidden) { was_hidden = IsCursorHidden(); settle = 3; }
    Vector2 md = settle > 0 ? (Vector2){0} : GetMouseDelta();
    if (settle > 0) settle--;
    in.lx = md.x * 0.0025f;
    in.ly = md.y * 0.0025f;

    uint16_t h = 0;
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) h |= BTN_FIRE;
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) h |= BTN_AIM;
    if (IsKeyDown(KEY_SPACE)) h |= BTN_JUMP;
    if (IsKeyDown(KEY_LEFT_SHIFT)) h |= BTN_SPRINT;
    if (IsKeyDown(KEY_F) || IsKeyDown(KEY_ENTER)) h |= BTN_ENTER;
    if (IsKeyDown(KEY_R)) h |= BTN_RELOAD;
    if (IsKeyDown(KEY_E) || GetMouseWheelMove() < 0) h |= BTN_NEXT;
    if (IsKeyDown(KEY_Q) || GetMouseWheelMove() > 0) h |= BTN_PREV;
    if (IsKeyDown(KEY_H)) h |= BTN_HORN;
    if (IsKeyDown(KEY_M)) h |= BTN_MAP;
    if (IsKeyDown(KEY_ESCAPE) || IsKeyDown(KEY_P)) h |= BTN_PAUSE;
    if (IsKeyDown(KEY_V)) h |= BTN_CAM;
    if (IsKeyDown(KEY_TAB)) h |= BTN_RADIO;
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_C)) h |= BTN_CROUCH;

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
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)) h |= BTN_SPRINT;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) h |= BTN_NEXT;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_TRIGGER_1)) h |= BTN_PREV;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_THUMB)) h |= BTN_HORN;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_MIDDLE_LEFT)) h |= BTN_MAP;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_MIDDLE_RIGHT)) h |= BTN_PAUSE;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_THUMB)) h |= BTN_CAM;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) h |= BTN_RADIO;
    }
    in.held = h;
    in.hit = h & ~last_held;
    last_held = h;
    return in;
}

void respawn(Vector3 at)
{
    Ped *p = &peds[PLAYER];
    if (p->car >= 0) { cars[p->car].driver = -1; p->car = -1; }
    p->pos = at;
    p->vel = (Vector3){0};
    p->health = 100;
    p->armor = 0;
    p->state = PS_IDLE;
    weapon_stow();
    cops_reset();
    mode = MODE_PLAY;
}

static void tick(const Input *in)
{
    if (mode == MODE_PAUSE) {
        if (in->hit & BTN_PAUSE) mode = MODE_PLAY;
        return;
    }
    if (mode == MODE_PLAY && (in->hit & BTN_PAUSE)) { mode = MODE_PAUSE; return; }

    world_tick(DT);
    peds_tick(in, DT);
    cars_tick(DT);
    combat_tick(in, DT);
    jobs_tick(in, DT);
    missions_tick(in, DT);
    cops_tick(DT);
    fx_tick(DT);
    audio_tick(DT);
    camera_update(in, DT);

    if (mode == MODE_DEAD || mode == MODE_BUSTED) {
        mode_s += DT;
        if (mode_s > 4.5f) {
            int fee = mode == MODE_DEAD ? 500 : 250;
            cash = cash > fee ? cash - fee : 0;
            respawn(mode == MODE_DEAD ? hospital_pos : police_pos);
        }
    } else mode_s = 0;

    game_s += DT;
    ticks++;
}

// pipes raw frames to ffmpeg, writes the mixer's output alongside, muxes at the end
typedef struct { FILE *video, *audio; char path[256], wav[256]; int w, h, fps; long samples; } Recorder;

static void wav_header(FILE *f, long samples)
{
    int32_t data = (int32_t)(samples * 4), riff = data + 36, fmt = 16, rate = 44100, byterate = rate * 4;
    int16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byterate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static void rec_open(Recorder *r, const char *path, int w, int h, int fps)
{
    char cmd[512];
    snprintf(r->path, sizeof r->path, "%s", path);
    snprintf(r->wav, sizeof r->wav, "%s.wav", path);
    r->w = w; r->h = h; r->fps = fps;
    snprintf(cmd, sizeof cmd, "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - "
             "-vf vflip -c:v libx264 -preset medium -crf 16 -pix_fmt yuv420p '%s.noaudio.mp4'", w, h, fps, path);
    r->video = popen(cmd, "w");
    r->audio = fopen(r->wav, "wb");
    wav_header(r->audio, 0);
}

static void rec_frame(Recorder *r, unsigned int tex)
{
    unsigned char *px = rlReadTexturePixels(tex, r->w, r->h, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    fwrite(px, 4, (size_t)r->w * r->h, r->video);
    RL_FREE(px);

    static float buf[44100 * 2];
    long want = (long)(44100.0 * (r->samples / 44100.0 + 1.0 / r->fps) + 0.5) - r->samples;
    audio_render(buf, (int)want);
    static int16_t pcm[44100 * 2];
    for (long i = 0; i < want * 2; i++) pcm[i] = (int16_t)(Clamp(buf[i], -1, 1) * 32767);
    fwrite(pcm, 2, want * 2, r->audio);
    r->samples += want;
}

static void rec_close(Recorder *r)
{
    pclose(r->video);
    wav_header(r->audio, r->samples);
    fclose(r->audio);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "ffmpeg -y -loglevel error -i '%s.noaudio.mp4' -i '%s' -c:v copy -c:a aac -b:a 192k "
             "-shortest '%s' && rm '%s.noaudio.mp4' '%s'", r->path, r->wav, r->path, r->path, r->wav);
    if (system(cmd) != 0) fprintf(stderr, "mux failed for %s\n", r->path);
}

static void save_shot(RenderTexture2D rt)
{
    Image img = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&img);
    char path[256];
    snprintf(path, sizeof path, "%s/%s.png", shot_dir, shot_name);
    ExportImage(img, path);
    UnloadImage(img);
    printf("shot %s at %.2f s\n", path, game_s);
}

int main(int argc, char **argv)
{
    uint32_t seed = 1;
    const char *script = NULL, *replay = NULL, *record = NULL, *save_inputs = NULL;
    float seconds = 0, hours = 17.5f;
    int w = 1600, h = 900;
    bool fast = false, hidden = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--seed")) { seed = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "--script")) { script = v; i++; }
        else if (!strcmp(a, "--replay")) { replay = v; i++; }
        else if (!strcmp(a, "--save-inputs")) { save_inputs = v; i++; }
        else if (!strcmp(a, "--record")) { record = v; i++; }
        else if (!strcmp(a, "--seconds")) { seconds = (float)atof(v); i++; }
        else if (!strcmp(a, "--size")) { sscanf(v, "%dx%d", &w, &h); i++; }
        else if (!strcmp(a, "--hours")) { hours = (float)atof(v); i++; }
        else if (!strcmp(a, "--shots")) { shot_dir = v; i++; }
        else if (!strcmp(a, "--debug")) { debug_parse(v); i++; }
        else if (!strcmp(a, "--fast")) fast = true;
        else if (!strcmp(a, "--hidden")) hidden = true;
        else { fprintf(stderr, "unknown option %s\n", a); return 1; }
    }
    bool offscreen = record || script || replay;
    shot_w = w; shot_h = h;
    audio_offline = offscreen;

    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT;
    if (!fast && !record) flags |= FLAG_VSYNC_HINT;
    if (hidden) flags |= FLAG_WINDOW_HIDDEN;
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(offscreen ? 960 : w, offscreen ? 540 : h, "Out on Parole");
    SetExitKey(KEY_NULL);
    if (!offscreen) DisableCursor();   // macOS shrinks a too-big window itself; SetWindowSize on retina broke raylib's sizes

    rng = seed * 2654435761u + 1;
    assets_load();
    render_init();
    audio_init();
    world_build(seed);
    day_hours = hours;
    respawn(police_pos);
    for (int i = 0; i < 40; i++) tick(&(Input){0}); // let traffic and people spread out
    game_s = 0;
    ticks = 0;

    FILE *inputs = NULL;
    if (replay) {
        inputs = fopen(replay, "rb");
        if (!inputs) { fprintf(stderr, "can't open %s\n", replay); return 1; }
    } else if (!script) {
        // every live session is kept next to the game, so it can be replayed and filmed later
        char path[512];
        if (!save_inputs) {
            const char *dir = TextFormat("%ssessions", GetApplicationDirectory());
            MakeDirectory(dir);
            time_t now = time(NULL);
            char stamp[32];
            strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
            snprintf(path, sizeof path, "%s/%s.inp", dir, stamp);
            save_inputs = path;
        }
        inputs = fopen(save_inputs, "wb");
    }
    if (script) load_script(script);
    if (script && save_inputs) inputs = fopen(save_inputs, "wb");   // turns a script into a replay, for testing

    Recorder rec = {0};
    RenderTexture2D rt = {0};
    if (offscreen) rt = LoadRenderTexture(shot_w, shot_h);
    if (record) rec_open(&rec, record, shot_w, shot_h, TICK_HZ);

    float acc = 0;
    while (!WindowShouldClose()) {
        if (offscreen) {
            Input in = {0};
            if (script && !script_step(&in)) break;
            if (script && inputs) fwrite(&in, sizeof in, 1, inputs);
            if (replay && fread(&in, sizeof in, 1, inputs) != 1) break;
            tick(&in);
            if (seconds > 0 && game_s >= seconds) break;

            if (record || want_shot) {
                BeginTextureMode(rt);
                render_frame(shot_w, shot_h);
                debug_draw();
                hud_draw(shot_w, shot_h);
                EndTextureMode();
                if (record) rec_frame(&rec, rt.texture.id);
                if (want_shot) { save_shot(rt); want_shot = false; }
            }
            if (!fast || ticks % 30 == 0) {   // keep the window alive, show progress
                BeginDrawing();
                ClearBackground(BLACK);
                if (rt.id) DrawTexturePro(rt.texture, (Rectangle){ 0, 0, shot_w, -shot_h },
                                          (Rectangle){ 0, 0, GetScreenWidth(), GetScreenHeight() }, (Vector2){0}, 0, WHITE);
                DrawText(TextFormat("%.1f s", game_s), 10, 10, 20, YELLOW);
                EndDrawing();
            }
            continue;
        }

        // frames and ticks don't line up, so input piles up until a tick eats it
        static Input pending;
        Input now = read_input();
        pending.mx = now.mx; pending.my = now.my; pending.held = now.held;
        pending.lx += now.lx; pending.ly += now.ly; pending.hit |= now.hit;
        acc += fminf(GetFrameTime(), 0.1f);
        while (acc >= DT) {
            if (inputs) fwrite(&pending, sizeof pending, 1, inputs);
            tick(&pending);
            pending.lx = pending.ly = 0;
            pending.hit = 0;
            acc -= DT;
        }
        if (mode == MODE_PAUSE) EnableCursor(); else if (IsCursorHidden() == false) DisableCursor();
        if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();

        BeginDrawing();
        render_frame(GetScreenWidth(), GetScreenHeight());
        debug_draw();
        hud_draw(GetScreenWidth(), GetScreenHeight());
        EndDrawing();
    }

    if (record) rec_close(&rec);
    if (inputs) fclose(inputs);
    CloseWindow();
    return 0;
}
