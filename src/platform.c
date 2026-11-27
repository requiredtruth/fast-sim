// what differs between Android, the browser and the desktop: tilt, storage, screen flags.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

bool tilt_steer = true, tilt_walk = true;
float tilt_sens = 1.0f;
bool tilt_invert;

// raw gravity in device axes (x right, y up, z out of the screen, portrait), m/s^2
static volatile float gx, gy, gz;
static volatile bool have_tilt;
static float neutral_pitch = 0.75f;   // radians the phone is held back from vertical, set by calibration
static int rot = 90;                  // landscape rotation in use

#if defined(PLATFORM_ANDROID)
#include <android/sensor.h>
#include <android/looper.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <pthread.h>
struct android_app *GetAndroidApp(void);

// the sensor queue lives on its own looper thread so it never touches raylib's event loop
static void *sensor_thread(void *u)
{
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorManager *sm = ASensorManager_getInstance();
    const ASensor *acc = ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_ACCELEROMETER);
    if (!acc) return NULL;
    ASensorEventQueue *q = ASensorManager_createEventQueue(sm, looper, 1, NULL, NULL);
    ASensorEventQueue_enableSensor(q, acc);
    ASensorEventQueue_setEventRate(q, acc, 16667);
    have_tilt = true;
    for (;;) {
        int ev, id = ALooper_pollOnce(-1, NULL, &ev, NULL);
        if (id != 1) continue;
        ASensorEvent e;
        while (ASensorEventQueue_getEvents(q, &e, 1) > 0) {
            // light smoothing here; the game smooths again per tick
            gx += (e.acceleration.x - gx) * 0.35f;
            gy += (e.acceleration.y - gy) * 0.35f;
            gz += (e.acceleration.z - gz) * 0.35f;
        }
    }
    return NULL;
}

void platform_init(void)
{
    struct android_app *app = GetAndroidApp();
    if (app && app->activity) ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    pthread_t t;
    pthread_create(&t, NULL, sensor_thread, NULL);
}

const char *save_dir(void)
{
    static char dir[512];
    if (dir[0]) return dir;
    struct android_app *app = GetAndroidApp();
    const char *p = app && app->activity ? (app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath) : NULL;
    snprintf(dir, sizeof dir, "%s", p ? p : ".");
    MakeDirectory(dir);
    return dir;
}

bool is_touch_device(void) { return true; }
void platform_vibrate(int ms) { (void)ms; }

#elif defined(PLATFORM_WEB)
#include <emscripten.h>

EMSCRIPTEN_KEEPALIVE void web_tilt(float x, float y, float z, int angle)
{
    gx = x; gy = y; gz = z;
    rot = ((angle % 360) + 360) % 360;
    have_tilt = true;
}

EM_JS(void, web_tilt_listen, (void), {
    var attach = function () {
        window.addEventListener('devicemotion', function (e) {
            var a = e.accelerationIncludingGravity;
            if (!a || a.x === null) return;
            var ang = (screen.orientation && screen.orientation.angle !== undefined) ? screen.orientation.angle : (window.orientation || 0);
            var ios = /iPad|iPhone|iPod/.test(navigator.userAgent);
            var k = ios ? -1 : 1;
            Module._web_tilt(a.x * k, a.y * k, a.z * k, ang);
        });
    };
    if (typeof DeviceMotionEvent !== 'undefined' && typeof DeviceMotionEvent.requestPermission === 'function') {
        var ask = function () {
            DeviceMotionEvent.requestPermission().then(function (r) { if (r === 'granted') attach(); }).catch(function () {});
            window.removeEventListener('touchend', ask);
        };
        window.addEventListener('touchend', ask);
    } else attach();
});

EM_JS(int, web_is_touch, (void), { return ('ontouchstart' in window) || navigator.maxTouchPoints > 0 ? 1 : 0; });
EM_JS(void, web_vibrate, (int ms), { if (navigator.vibrate) navigator.vibrate(ms); });

void platform_init(void) { web_tilt_listen(); }
const char *save_dir(void) { MakeDirectory("/save"); return "/save"; }
bool is_touch_device(void) { return web_is_touch(); }
void platform_vibrate(int ms) { web_vibrate(ms); }

#else   // desktop

void platform_init(void) {}

const char *save_dir(void)
{
    static char dir[512];
    if (dir[0]) return dir;
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    snprintf(dir, sizeof dir, "%s\\LongRoad", base ? base : ".");
#else
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/LongRoad", xdg);
    else if (home) {
        char share[480];
        snprintf(share, sizeof share, "%s/.local/share", home);
        MakeDirectory(share);
        snprintf(dir, sizeof dir, "%s/LongRoad", share);
    } else snprintf(dir, sizeof dir, "./LongRoad");
#endif
    MakeDirectory(dir);
    return dir;
}

bool is_touch_device(void) { return false; }
void platform_vibrate(int ms) { (void)ms; }
#endif

bool tilt_available(void) { return have_tilt; }

// screen-space gravity for a landscape game
static void screen_gravity(float *sx, float *sy, float *sz)
{
    float x = gx, y = gy, z = gz;
#if defined(PLATFORM_ANDROID)
    // sensorLandscape can be either way round: the long axis tells which
    if (x > 2.5f) rot = 90; else if (x < -2.5f) rot = 270;
#endif
    switch (rot) {
    case 90: *sx = -y; *sy = x; break;
    case 180: *sx = -x; *sy = -y; break;
    case 270: *sx = y; *sy = -x; break;
    default: *sx = x; *sy = y; break;
    }
    *sz = z;
}

void tilt_calibrate(void)
{
    float sx, sy, sz;
    screen_gravity(&sx, &sy, &sz);
    neutral_pitch = atan2f(sz, fmaxf(sy, 0.1f));
}

Vector2 tilt_read(void)
{
    if (!have_tilt) return (Vector2){ 0, 0 };
    float sx, sy, sz;
    screen_gravity(&sx, &sy, &sz);
    float g = sqrtf(sx * sx + sy * sy + sz * sz);
    if (g < 3) return (Vector2){ 0, 0 };
    // steering: rotating the phone like a wheel. 26 degrees is full lock
    float roll = atan2f(-sx, fmaxf(sy, 0.5f));
    float pitch = atan2f(sz, fmaxf(sy, 0.1f)) - neutral_pitch;
    float k = tilt_sens;
    float s = roll / 0.45f * k, f = pitch / 0.42f * k;
    if (tilt_invert) { s = -s; f = -f; }
    // a little dead zone keeps the car straight when held still
    s = fabsf(s) < 0.05f ? 0 : (s - copysignf(0.05f, s)) / 0.95f;
    f = fabsf(f) < 0.12f ? 0 : (f - copysignf(0.12f, f)) / 0.88f;
    return (Vector2){ Clamp(s, -1, 1), Clamp(f, -1, 1) };
}

// ---- a small key/value store for settings and mod switches

#ifdef PLATFORM_WEB
EM_JS(void, kv_put_js, (const char *k, const char *v), { try { localStorage.setItem("longroad_" + UTF8ToString(k), UTF8ToString(v)); } catch (e) {} });
EM_JS(char *, kv_get_js, (const char *k), { var v = null; try { v = localStorage.getItem("longroad_" + UTF8ToString(k)); } catch (e) {} return v ? stringToNewUTF8(v) : 0; });
void plat_put(const char *key, const char *val) { kv_put_js(key, val); }
char *plat_get(const char *key) { return kv_get_js(key); }
#else
void plat_put(const char *key, const char *val)
{
    char path[600];
    snprintf(path, sizeof path, "%s/%s.txt", save_dir(), key);
    SaveFileText(path, (char *)val);
}
char *plat_get(const char *key)
{
    char path[600];
    snprintf(path, sizeof path, "%s/%s.txt", save_dir(), key);
    if (!FileExists(path)) return NULL;
    char *t = LoadFileText(path);
    if (!t) return NULL;
    char *r = strdup(t);
    UnloadFileText(t);
    return r;
}
#endif
