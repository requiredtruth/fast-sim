#include <math.h>
#include <string.h>
#include "ped.h"

Camera3D cam;
float cam_yaw, cam_pitch = -0.12f;

static const float foot_dist[3] = { 2.6f, 3.6f, 5.2f };
static const float car_scale[3] = { 0.8f, 1.0f, 1.4f };

static int dist_mode = 1;
static float look_idle_s;
static float aim_k, car_k, clip_d = 4;
static Vector3 pivot_s;
static float car_yaw_s, car_off, car_poff;
static bool was_car, back_view, started;

static float angle_diff(float from, float to)
{
    float d = fmodf(to - from + PI, 2 * PI);
    if (d < 0) d += 2 * PI;
    return d - PI;
}

static Vector3 look_dir(float yaw, float pitch)
{
    return (Vector3){ sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch) };
}

// pull the camera in so it stays out of walls and the ground; returns the clear distance
static float clear_dist(Vector3 from, Vector3 dir, float want)
{
    Hit h;
    if (world_ray(from, dir, want + 0.3f, &h)) return fmaxf(0.4f, h.t - 0.3f);
    return want;
}

static void foot_cam(const Input *in, float dt, Vector3 *pos, Vector3 *tgt, float *fov)
{
    Ped *p = &peds[PLAYER];
    Priv *v = &pv[PLAYER];
    Vector3 head = v->rag_on ? Vector3Add(rag_centre(PLAYER), (Vector3){ 0, 0.5f, 0 })
                             : Vector3Add(p->pos, (Vector3){ 0, v->crouch ? 1.1f : 1.55f, 0 });
    if (!started || Vector3Distance(head, pivot_s) > 10) pivot_s = head;
    float kx = 1 - expf(-16 * dt), ky = 1 - expf(-6 * dt);
    pivot_s.x += (head.x - pivot_s.x) * kx;
    pivot_s.z += (head.z - pivot_s.z) * kx;
    pivot_s.y += (head.y - pivot_s.y) * ky;

    aim_k += ((p->aiming ? 1 : 0) - aim_k) * (1 - expf(-9 * dt));
    float dist = Lerp(foot_dist[dist_mode], 1.9f, aim_k);
    float shoulder = Lerp(0.35f, 0.7f, aim_k);
    Vector3 dir = look_dir(cam_yaw, cam_pitch), right = { -cosf(cam_yaw), 0, sinf(cam_yaw) };
    Vector3 want = Vector3Add(Vector3Add(pivot_s, Vector3Scale(right, shoulder)), Vector3Scale(dir, -dist));
    Vector3 ray = Vector3Subtract(want, pivot_s);
    float len = Vector3Length(ray);
    ray = Vector3Scale(ray, 1 / len);
    float cd = clear_dist(pivot_s, ray, len);
    if (cd < clip_d) clip_d = cd;
    else clip_d = fminf(cd, clip_d + 5 * dt);
    if (!started) clip_d = cd;
    *pos = Vector3Add(pivot_s, Vector3Scale(ray, clip_d));
    float floor_y = ground_y(pos->x, pos->z);
    if (floor_y < -1) floor_y = 0;
    pos->y = fmaxf(pos->y, floor_y + 0.3f);
    *tgt = Vector3Add(*pos, Vector3Scale(dir, 20));
    *fov = Lerp(62, 46, aim_k) + (p->state == PS_IDLE && v->speed > 5 ? 4 : 0);
}

static void car_cam(const Input *in, float dt, Vector3 *pos, Vector3 *tgt, float *fov)
{
    Ped *p = &peds[PLAYER];
    Car *c = &cars[p->car];
    const CarModel *m = &car_models[c->model];
    float spd = car_speed(c);
    float heading = atan2f(car_fwd(c).x, car_fwd(c).z);
    if (!was_car) { car_yaw_s = cam_yaw; car_off = 0; car_poff = 0; back_view = false; }

    // lean toward where the car is really going when it slides
    float hdg = heading;
    float vh = hypotf(c->vel.x, c->vel.z);
    if (fabsf(spd) > 4 && vh > 4) hdg += 0.45f * angle_diff(heading, atan2f(c->vel.x, c->vel.z));
    if (spd < -5) back_view = true;
    else if (spd > -1) back_view = false;
    float want = hdg + (back_view ? PI : 0);
    car_yaw_s += angle_diff(car_yaw_s, want) * (1 - expf(-(back_view ? 2.4f : 3.4f) * dt));

    if (in->lx != 0 || in->ly != 0) {
        car_off -= in->lx;
        car_poff = Clamp(car_poff - in->ly, -0.6f, 0.9f);
    } else if (look_idle_s > 1.5f) {
        float k = expf(-3 * dt);
        car_off *= k;
        car_poff *= k;
    }
    float yaw = car_yaw_s + car_off;
    cam_yaw = yaw;
    float pitch = -0.2f - fminf(fabsf(spd) * 0.002f, 0.06f) + car_poff;
    cam_pitch = pitch;

    float dist = (m->size.z * 0.5f + 3.2f) * car_scale[dist_mode] + fminf(fabsf(spd) * 0.04f, 1.8f);
    Vector3 centre = Vector3Add(c->pos, (Vector3){ 0, m->size.y * 0.5f + 0.15f, 0 });
    Vector3 dir = look_dir(yaw, pitch);
    float cd = clear_dist(centre, Vector3Scale(dir, -1), dist);
    *pos = Vector3Add(centre, Vector3Scale(dir, -cd));
    float floor_y = ground_y(pos->x, pos->z);
    if (floor_y < -1) floor_y = 0;
    pos->y = fmaxf(pos->y, floor_y + 0.35f);
    *tgt = Vector3Add(centre, Vector3Scale((Vector3){ dir.x, 0, dir.z }, 2.0f));
    *fov = 60 + Clamp(fabsf(spd) / 45, 0, 1) * 16;
}

static struct { bool on, follow; Vector3 p0, t0, p1, t1, off, at; float secs, t, look_y; } drone;

void drone_cmd(const char *cmd, const float *v)
{
    if (!strcmp(cmd, "dronefree")) { drone.on = drone.follow = false; return; }
    if (!strcmp(cmd, "dronefollow")) {   // back, up, right of the player, for chases seen from behind
        drone.off = (Vector3){ v[0], v[1], v[2] };
        drone.look_y = v[3] != 0 ? v[3] : 1;   // aim height above the player
        drone.at = cam.position;
        drone.on = drone.follow = true;
        return;
    }
    drone.follow = false;
    bool move = !strcmp(cmd, "dronemove");
    if (!drone.on) { drone.p0 = cam.position; drone.t0 = cam.target; }
    else if (drone.t < drone.secs) {
        float k = drone.t / drone.secs;
        k = k * k * (3 - 2 * k);
        drone.p0 = Vector3Lerp(drone.p0, drone.p1, k);
        drone.t0 = Vector3Lerp(drone.t0, drone.t1, k);
    } else { drone.p0 = drone.p1; drone.t0 = drone.t1; }
    const float *q = move ? v + 1 : v;
    drone.p1 = (Vector3){ q[0], q[1], q[2] };
    drone.t1 = (Vector3){ q[3], q[4], q[5] };
    drone.secs = move ? fmaxf(v[0], 0.01f) : 0.01f;
    drone.t = 0;
    drone.on = true;
}

void camera_update(const Input *in, float dt)
{
    Ped *p = &peds[PLAYER];
    bool incar = p->used && p->state == PS_IN_CAR && p->car >= 0 && cars[p->car].used;
    if (in->hit & BTN_CAM) dist_mode = (dist_mode + 1) % 3;
    if (fabsf(in->lx) + fabsf(in->ly) > 1e-5f) look_idle_s = 0; else look_idle_s += dt;

    if (!incar) {
        cam_yaw -= in->lx;
        cam_pitch = Clamp(cam_pitch - in->ly, -1.15f, 0.85f);
        Priv *v = &pv[PLAYER];
        // behind the player again when running and the mouse is quiet
        if (p->used && look_idle_s > 1.0f && v->speed > 1.5f && !p->aiming && p->state != PS_DEAD) {
            float d = angle_diff(cam_yaw, p->yaw);
            if (fabsf(d) < 2.3f) {
                float gain = fabsf(d) < 0.8f ? 1.6f : 0.5f;
                cam_yaw += d * fminf(1, gain * dt) * Clamp(v->speed / 5, 0.3f, 1);
            }
        }
    }
    static Vector3 cp, ct;
    static float cf = 60;
    Vector3 fp, ft;
    float ff;
    foot_cam(in, dt, &fp, &ft, &ff);
    if (incar) car_cam(in, dt, &cp, &ct, &cf);
    was_car = incar;
    car_k += ((incar ? 1 : 0) - car_k) * (1 - expf(-6 * dt));
    if (!started) car_k = incar ? 1 : 0;
    float k = car_k < 0.01f ? 0 : car_k > 0.99f ? 1 : car_k;
    cam.position = Vector3Lerp(fp, cp, k);
    cam.target = Vector3Lerp(ft, ct, k);
    cam.up = (Vector3){ 0, 1, 0 };
    float fov = Lerp(ff, cf, k);
    cam.fovy = started ? cam.fovy + (fov - cam.fovy) * (1 - expf(-8 * dt)) : fov;
    cam.projection = CAMERA_PERSPECTIVE;
    started = true;

    if (drone.on && drone.follow) {
        Ped *p = &peds[PLAYER];
        bool in_car = p->car >= 0 && cars[p->car].used;
        Vector3 at = in_car ? cars[p->car].pos : p->pos;
        Vector3 f = in_car ? car_fwd(&cars[p->car]) : (Vector3){ sinf(p->yaw), 0, cosf(p->yaw) };
        f.y = 0;
        f = Vector3Normalize(f);
        Vector3 r = { -f.z, 0, f.x };
        Vector3 want = Vector3Add(at, Vector3Add(Vector3Scale(f, -drone.off.x), Vector3Add((Vector3){ 0, drone.off.y, 0 }, Vector3Scale(r, drone.off.z))));
        drone.at = Vector3Lerp(drone.at, want, 1 - expf(-2.5f * dt));
        cam.position = drone.at;
        cam.target = Vector3Add(at, Vector3Add(Vector3Scale(f, 4), (Vector3){ 0, drone.look_y, 0 }));
        cam.fovy = 60;
    } else if (drone.on) {
        drone.t = fminf(drone.t + dt, drone.secs);
        float k = drone.t / drone.secs;
        k = k * k * (3 - 2 * k);
        cam.position = Vector3Lerp(drone.p0, drone.p1, k);
        cam.target = Vector3Lerp(drone.t0, drone.t1, k);
        cam.fovy = 55;
    }
}
