#include <math.h>
#include <string.h>
#include "ped.h"

#define CLIP_WALK_MS 1.2f
#define CLIP_JOG_MS 3.0f
#define CLIP_SPRINT_MS 4.4f
#define CLIP_CROUCH_MS 1.0f
#define FADE_S 0.2f
#define GRAVITY 18.0f

Priv pv[MAX_PEDS];

static Vector3 bind_p[MAX_BONES];
static Quaternion bind_q[MAX_BONES];
static float gait_off[A_COUNT];
static bool ready;
static float hu_w[MAX_PEDS];   // 0..1 how far the hands are up

// pelvis chest head elbowL elbowR handL handR kneeL kneeR footL footR
static const int rb[11] = { 0, 3, 5, 8, 12, 9, 13, 15, 19, 16, 20 };
static const float rr[11] = { 0.13f, 0.13f, 0.11f, 0.06f, 0.06f, 0.05f, 0.05f, 0.08f, 0.08f, 0.07f, 0.07f };
static const float rag_w[11] = { 0.8f, 1.2f, 1.1f, 0.9f, 0.9f, 0.9f, 0.9f, 0.8f, 0.8f, 0.7f, 0.7f };

typedef struct { int a, b; float lo, hi; } Con;   // negative lo is metres, else a fraction of the bind length
static const Con cons[] = {
    { 0, 1, 1, 1 }, { 1, 2, 1, 1 }, { 1, 3, 1, 1 }, { 1, 4, 1, 1 }, { 3, 5, 1, 1 }, { 4, 6, 1, 1 },
    { 0, 7, 1, 1 }, { 0, 8, 1, 1 }, { 7, 9, 1, 1 }, { 8, 10, 1, 1 },
    { 0, 2, 0.8f, 1.02f }, { 0, 3, 0.5f, 1.25f }, { 0, 4, 0.5f, 1.25f },
    { 1, 7, 0.5f, 1.02f }, { 1, 8, 0.5f, 1.02f }, { 1, 5, 0, 1.05f }, { 1, 6, 0, 1.05f },
    { 7, 8, -0.2f, 9 }, { 9, 10, -0.16f, 9 }, { 5, 6, -0.08f, 9 }, { 0, 9, 0.35f, 1.0f }, { 0, 10, 0.35f, 1.0f },
};
#define NCON ((int)(sizeof cons / sizeof cons[0]))
static float con_lo[NCON], con_hi[NCON];

static const float upper_mask[MAX_BONES] = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 };

static bool is_gait(int a) { return a == A_WALK || a == A_JOG || a == A_SPRINT || a == A_CROUCH_WALK; }

static Quaternion nlerp(Quaternion a, Quaternion b, float k)
{
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = (Quaternion){ -b.x, -b.y, -b.z, -b.w };
    return QuaternionNormalize(QuaternionLerp(a, b, k));
}

static float smooth(float k) { k = Clamp(k, 0, 1); return k * k * (3 - 2 * k); }

static Matrix basis(Vector3 x, Vector3 y, Vector3 z)
{
    return (Matrix){ x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, 0, 0, 0, 1 };
}

static void anim_init(void)
{
    if (ready || skel.count == 0) return;
    ready = true;
    Matrix w[MAX_BONES];
    for (int i = 0; i < skel.count; i++) {
        Vector3 p = skel.bind_pos[i];
        Matrix l = MatrixMultiply(QuaternionToMatrix(skel.bind_rot[i]), MatrixTranslate(p.x, p.y, p.z));
        int par = skel.parent[i];
        w[i] = par < 0 ? l : MatrixMultiply(l, w[par]);
        bind_p[i] = Vector3Transform((Vector3){0}, w[i]);
        bind_q[i] = par < 0 ? skel.bind_rot[i] : QuaternionMultiply(bind_q[par], skel.bind_rot[i]);
    }
    // line the cycles up: phase 0 is the left foot at its furthest reach
    static const int cyc[] = { A_WALK, A_JOG, A_SPRINT, A_CROUCH_WALK };
    for (int k = 0; k < 4; k++) {
        const Clip *c = &clips[cyc[k]];
        int n = c->frames - 1, best = 0;
        float bz = -1e9f;
        for (int f = 0; f < n; f++) {
            Quaternion r[MAX_BONES]; Vector3 h; Matrix b[MAX_BONES];
            pose_sample(c, f / c->fps, r, &h);
            pose_matrices(r, h, b);
            float z = Vector3Transform(bind_p[16], b[16]).z - h.z;
            if (z > bz) { bz = z; best = f; }
        }
        gait_off[cyc[k]] = (float)best / n;
    }
    for (int k = 0; k < NCON; k++) {
        float d = Vector3Distance(bind_p[rb[cons[k].a]], bind_p[rb[cons[k].b]]);
        con_lo[k] = cons[k].lo < 0 ? -cons[k].lo : cons[k].lo * d;
        con_hi[k] = cons[k].hi > 5 ? 99 : cons[k].hi * d;
    }
}

static void sample(int a, float t, float gait, Quaternion *rot, Vector3 *hips)
{
    const Clip *c = &clips[a];
    if (is_gait(a)) {
        float dur = (c->frames - 1) / c->fps, ph = gait + gait_off[a];
        pose_sample(c, (ph - floorf(ph)) * dur, rot, hips);
    } else pose_sample(c, t, rot, hips);
}

void anim_to(int i, int a, float t0)
{
    Ped *p = &peds[i];
    if (p->anim == a) return;
    p->prev_anim = p->anim; p->prev_t = p->anim_t; pv[i].prev_rate = pv[i].rate;
    p->anim = a; p->anim_t = t0; p->blend = 0;
}

void locomote(int i, float speed, bool crouch, bool back)
{
    Ped *p = &peds[i];
    int cur = p->anim, a;
    float rate = 1;
    if (crouch) {
        a = speed > 0.25f ? A_CROUCH_WALK : A_COWER;
        if (a == A_CROUCH_WALK) rate = speed / CLIP_CROUCH_MS;
    } else if (speed < (cur == A_IDLE ? 0.3f : 0.15f)) a = A_IDLE;
    else if (speed < (cur == A_JOG || cur == A_SPRINT ? 2.2f : 2.6f)) { a = A_WALK; rate = speed / CLIP_WALK_MS; }
    else if (speed < (cur == A_SPRINT ? 4.3f : 4.8f)) { a = A_JOG; rate = speed / CLIP_JOG_MS; }
    else { a = A_SPRINT; rate = speed / CLIP_SPRINT_MS; }
    if (is_gait(a)) rate = Clamp(rate, 0.5f, 1.7f) * (back ? -1 : 1);
    anim_to(i, a, 0);
    pv[i].rate = rate;
}

void anim_step(int i, float dt)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    anim_init();
    p->anim_t += dt * v->rate;
    p->prev_t += dt * v->prev_rate;
    p->blend = fminf(1, p->blend + dt / FADE_S);
    hu_w[i] = Clamp(hu_w[i] + (p->state == PS_HANDS_UP ? 4 : -4) * dt, 0, 1);
    if (is_gait(p->anim) && clips[p->anim].frames > 1) {
        const Clip *c = &clips[p->anim];
        v->gait += dt * v->rate / ((c->frames - 1) / c->fps);
        v->gait -= floorf(v->gait);
    }
    bool afoot = !v->rag_on && p->state != PS_IN_CAR && p->state != PS_COWER;
    bool hold = p->weapon != W_FIST && afoot;
    int want = p->aiming ? A_PISTOL_AIM : A_PISTOL_IDLE;
    if (hold && v->up_anim != want) { v->up_anim = want; v->up_t = 0; v->up_w *= 0.5f; }
    v->up_t += dt;
    v->up_w = Clamp(v->up_w + (hold ? 1 : -1) * 6 * dt, 0, 1);
    float pt = p->aiming && hold ? Clamp(p->aim_pitch, -0.9f, 0.9f) : 0;
    v->pitch += (pt - v->pitch) * fminf(1, 8 * dt);
    if (v->shot >= 0) {
        v->shot_t += dt;
        if (v->shot_t >= v->shot_len) v->shot = -1;
    }
}

void ped_play(int i, int anim)
{
    if (i < 0 || i >= MAX_PEDS || !peds[i].used || anim < 0 || anim >= A_COUNT) return;
    Priv *v = &pv[i];
    if (v->rag_on) return;
    const Clip *c = &clips[anim];
    v->shot = anim;
    v->shot_t = 0;
    v->shot_len = (c->frames - 1) / c->fps;
    if (anim == A_PISTOL_SHOOT) v->shot_len = 0.18f;
    v->shot_full = anim == A_ROLL || (v->speed < 0.6f && anim != A_PISTOL_SHOOT && anim != A_PISTOL_RELOAD);
}

static float jitter(int i, float t, int k)
{
    uint32_t a = (uint32_t)(t * 9) + i * 7919u + k * 104729u, b = a + 1;
    a = (a ^ 61u) * 2654435761u; a ^= a >> 15; b = (b ^ 61u) * 2654435761u; b ^= b >> 15;
    return Lerp((a & 255) / 127.5f - 1, (b & 255) / 127.5f - 1, smooth(t * 9 - floorf(t * 9)));
}

static void hands_up(int i, Quaternion *rot)
{
    float w = smooth(hu_w[i]);
    if (w < 0.001f) return;
    int hand[2] = { skel.hand_r, skel.hand_l };
    for (int s = 0; s < 2; s++) {
        int fa = skel.parent[hand[s]], ua = fa < 0 ? -1 : skel.parent[fa];
        if (ua < 0) continue;
        float side = s ? -1 : 1, n = jitter(i, game_s, s);
        Quaternion up = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, side * (2.3f + 0.08f * n) * w);
        Quaternion fb = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, side * (0.25f + 0.12f * jitter(i, game_s, s + 2)) * w);
        rot[ua] = QuaternionMultiply(up, rot[ua]);
        rot[fa] = QuaternionMultiply(fb, rot[fa]);
    }
}

static void stand_pose(int i, Quaternion *rot, Vector3 *hips)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    Quaternion b[MAX_BONES];
    Vector3 hb;
    sample(p->anim, p->anim_t, v->gait, rot, hips);
    if (p->blend < 1) {
        sample(p->prev_anim, p->prev_t, v->gait, b, &hb);
        float k = smooth(p->blend);
        for (int j = 0; j < skel.count; j++) rot[j] = nlerp(b[j], rot[j], k);
        *hips = Vector3Lerp(hb, *hips, k);
    }
    if (v->up_w > 0.001f) {
        sample(v->up_anim, v->up_t, 0, b, &hb);
        for (int j = 0; j < skel.count; j++)
            if (upper_mask[j] > 0) rot[j] = nlerp(rot[j], b[j], upper_mask[j] * v->up_w);
    }
    if (fabsf(v->pitch) > 0.002f) {
        static const float share[3] = { 0.3f, 0.35f, 0.35f };
        for (int j = 0; j < 3; j++) {
            Quaternion q = QuaternionFromAxisAngle((Vector3){ 1, 0, 0 }, -v->pitch * share[j]);
            rot[1 + j] = QuaternionMultiply(q, rot[1 + j]);
        }
    }
    if (i == PLAYER && weapon_reach > 0) {
        float w = smooth(weapon_reach);
        for (int j = 1; j <= skel.hand_r; j++) rot[j] = nlerp(rot[j], clips[A_IDLE].rot[j], w);
        int elbow = skel.parent[skel.hand_r];
        rot[elbow] = QuaternionMultiply(rot[elbow], QuaternionFromAxisAngle((Vector3){ 1, 0, 0 }, 0.65f * w));
    }
    hands_up(i, rot);
    if (v->shot >= 0) {
        float w = Clamp(fminf(v->shot_t / 0.07f, (v->shot_len - v->shot_t) / 0.18f), 0, 1);
        sample(v->shot, v->shot_t, 0, b, &hb);
        for (int j = 0; j < skel.count; j++) {
            float m = (v->shot_full ? 1 : upper_mask[j]) * w;
            if (m > 0) rot[j] = nlerp(rot[j], b[j], m);
        }
        if (v->shot_full) hips->y = Lerp(hips->y, hb.y, w);
    }
}

// ---- ragdoll

static Vector3 sub(Vector3 a, Vector3 b) { return Vector3Subtract(a, b); }

Vector3 rag_centre(int i)
{
    Ped *p = &peds[i];
    return Vector3Lerp(p->rag[0], p->rag[1], 0.5f);
}

static Vector3 torso_up(const Ped *p) { return Vector3Normalize(sub(p->rag[1], p->rag[0])); }

float rag_yaw(int i)
{
    Ped *p = &peds[i];
    Vector3 up = torso_up(p), f = Vector3CrossProduct(pv[i].side, up);
    if (up.x * up.x + up.z * up.z > 0.25f) return atan2f(up.x, up.z);
    return atan2f(f.x, f.z);
}

void rag_begin(int i, Vector3 impulse, Vector3 shift)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    anim_init();
    if (!v->ready) ped_build(i);
    Vector3 vel = { p->vel.x, p->vel.y, p->vel.z };
    for (int k = 0; k < 11; k++) {
        Vector3 w = Vector3Transform(bind_p[rb[k]], MatrixMultiply(v->bones[rb[k]], v->xf));
        w = Vector3Add(w, shift);
        Vector3 d = Vector3Add(vel, Vector3Scale(impulse, rag_w[k]));
        d.x += frand(-0.4f, 0.4f); d.z += frand(-0.4f, 0.4f);
        p->rag[k] = w;
        p->rag_prev[k] = Vector3Subtract(w, Vector3Scale(d, DT));
    }
    v->side = (Vector3){ cosf(p->yaw), 0, -sinf(p->yaw) };
    v->rag_on = true; v->rag_sleep = false; v->rag_still = 0; v->rag_s = 0; v->getup = -1;
    v->shot = -1;
    v->flinch_s = 0;
}

void rag_tick(int i)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    Vector3 *x = p->rag, *o = p->rag_prev;
    if (!v->rag_sleep) {
        float moved = 0;
        for (int k = 0; k < 11; k++) {
            Vector3 vel = Vector3Scale(sub(x[k], o[k]), 0.996f);
            o[k] = x[k];
            x[k] = Vector3Add(x[k], vel);
            x[k].y -= GRAVITY * DT * DT;
        }
        for (int it = 0; it < 7; it++) {
            for (int k = 0; k < NCON; k++) {
                int a = cons[k].a, b = cons[k].b;
                Vector3 d = sub(x[b], x[a]);
                float len = Vector3Length(d);
                if (len < 1e-5f) continue;
                float t = Clamp(len, con_lo[k], con_hi[k]);
                if (t == len) continue;
                Vector3 corr = Vector3Scale(d, (len - t) / len * 0.5f);
                x[a] = Vector3Add(x[a], corr);
                x[b] = sub(x[b], corr);
            }
            for (int k = 0; k < 11; k++) {
                float gy = ground_y(x[k].x, x[k].z) + rr[k];
                if (x[k].y < gy) {
                    float fall = o[k].y - x[k].y;
                    x[k].y = gy;
                    o[k].x += (x[k].x - o[k].x) * 0.35f;
                    o[k].z += (x[k].z - o[k].z) * 0.35f;
                    if (k < 2 && fall > 0.12f && v->hit_cd <= 0) {
                        sfx(S_BODYFALL, x[k], Clamp(fall * 4, 0.3f, 1), frand(0.9f, 1.1f));
                        v->hit_cd = 0.5f;
                    }
                }
            }
        }
        for (int k = 0; k < 11; k++) {
            Vector3 before = x[k];
            world_push(&x[k], rr[k] + 0.04f, 0.2f);
            if (k < 3 && v->rag_s > 0.3f) cars_push(&x[k], 0.2f);
            if (x[k].x != before.x || x[k].z != before.z) {
                o[k].x = x[k].x; o[k].z = x[k].z;
            }
            moved = fmaxf(moved, Vector3Distance(x[k], o[k]));
        }
        v->rag_still = moved < 0.0012f ? v->rag_still + 1 : 0;
        v->rag_s += DT;
        if (v->rag_still > 40 || v->rag_s > 9) v->rag_sleep = true;
        // keep the torso frame from spinning about its own axis
        Vector3 up = torso_up(p);
        Vector3 hint = Vector3Add(sub(x[3], x[4]), sub(x[7], x[8]));
        hint = sub(hint, Vector3Scale(up, Vector3DotProduct(hint, up)));
        if (Vector3Length(hint) > 0.05f) v->side = Vector3Add(Vector3Scale(v->side, 0.95f), Vector3Scale(Vector3Normalize(hint), 0.05f));
        v->side = sub(v->side, Vector3Scale(up, Vector3DotProduct(v->side, up)));
        if (Vector3Length(v->side) < 0.1f) v->side = Vector3Perpendicular(up);
        v->side = Vector3Normalize(v->side);
    }
    p->pos = (Vector3){ x[0].x, ground_y(x[0].x, x[0].z), x[0].z };
    p->vel = Vector3Scale(sub(x[0], o[0]), TICK_HZ);
}

static void rag_pose(int i, Quaternion *rot, Vector3 *hips)
{
    Ped *p = &peds[i];
    Vector3 *x = p->rag;
    Vector3 up = torso_up(p), side = pv[i].side;
    Vector3 u0 = Vector3Normalize(sub(bind_p[3], bind_p[0])), s0 = { 1, 0, 0 };
    Matrix bm = basis(s0, u0, Vector3CrossProduct(s0, u0));
    Matrix cm = basis(side, up, Vector3CrossProduct(side, up));
    Quaternion dt = QuaternionFromMatrix(MatrixMultiply(MatrixTranspose(bm), cm));

    Quaternion d[MAX_BONES];
    for (int j = 0; j < MAX_BONES; j++) d[j] = dt;
    #define LIMB(base, from, to, tgt) QuaternionMultiply(QuaternionFromVector3ToVector3( \
        Vector3RotateByQuaternion(Vector3Normalize(sub(bind_p[to], bind_p[from])), base), Vector3Normalize(tgt)), base)
    Vector3 chest_dir = sub(x[2], x[1]);
    d[5] = LIMB(dt, 3, 5, chest_dir);
    d[4] = d[5];
    static const int arm[2][4] = { { 7, 8, 9, 3 }, { 11, 12, 13, 4 } };   // upperarm, lowerarm, hand, elbow point
    for (int s = 0; s < 2; s++) {
        int ua = arm[s][0], la = arm[s][1], hd = arm[s][2], el = arm[s][3], hand = el + 2;
        Vector3 sh = Vector3Add(x[1], Vector3RotateByQuaternion(sub(bind_p[ua], bind_p[3]), dt));
        d[ua] = LIMB(dt, ua, la, sub(x[el], sh));
        d[la] = LIMB(d[ua], la, hd, sub(x[hand], x[el]));
        d[hd] = d[la];
    }
    static const int leg[2][4] = { { 14, 15, 16, 7 }, { 18, 19, 20, 8 } };  // thigh, calf, foot, knee point
    for (int s = 0; s < 2; s++) {
        int th = leg[s][0], ca = leg[s][1], ft = leg[s][2], kn = leg[s][3], foot = kn + 2;
        Vector3 hp = Vector3Add(x[0], Vector3RotateByQuaternion(sub(bind_p[th], bind_p[0]), dt));
        d[th] = LIMB(dt, th, ca, sub(x[kn], hp));
        d[ca] = LIMB(d[th], ca, ft, sub(x[foot], x[kn]));
        d[ft] = d[ca];
        d[ft + 1] = d[ca];
    }
    #undef LIMB
    Quaternion wq[MAX_BONES];
    for (int j = 0; j < skel.count; j++) {
        wq[j] = QuaternionMultiply(d[j], bind_q[j]);
        int par = skel.parent[j];
        rot[j] = par < 0 ? wq[j] : QuaternionMultiply(QuaternionInvert(wq[par]), wq[j]);
    }
    *hips = x[0];
}

void getup_begin(int i)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    rag_pose(i, v->gu_rot, &v->gu_hips);
    p->yaw = rag_yaw(i);
    p->pos = (Vector3){ p->rag[0].x, ground_y(p->rag[0].x, p->rag[0].z), p->rag[0].z };
    p->vel = (Vector3){0};
    v->getup = 0;
    p->anim = A_IDLE; p->prev_anim = A_IDLE; p->blend = 1; v->rate = 1;
}

static void getup_pose(int i, Quaternion *rot, Vector3 *hips)
{
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    Quaternion idle[MAX_BONES];
    Vector3 ih;
    pose_sample(&clips[A_IDLE], p->anim_t, idle, &ih);
    Quaternion ry = QuaternionFromAxisAngle((Vector3){ 0, 1, 0 }, p->yaw);
    idle[0] = QuaternionMultiply(ry, idle[0]);
    ih = Vector3Add(p->pos, Vector3RotateByQuaternion(ih, ry));
    float k = smooth(v->getup);
    for (int j = 0; j < skel.count; j++) rot[j] = nlerp(v->gu_rot[j], idle[j], k);
    *hips = Vector3Lerp(v->gu_hips, ih, k);
}

// seated people ride the car as it is drawn, not as it was at the end of our tick
static Matrix seat_xf(const Ped *p, const Priv *v)
{
    const Car *c = &cars[p->car];
    const CarModel *m = &car_models[c->model];
    Matrix xf = car_matrix(c);
    Vector3 seat = Vector3Transform((Vector3){ m->seat.x, m->seat.y - SEAT_DROP, m->seat.z }, xf);
    Vector3 at = Vector3Lerp(v->sit_from, seat, smooth(v->sit));
    xf.m12 = at.x; xf.m13 = at.y; xf.m14 = at.z;
    return xf;
}

// ---- building the pose every tick

void ped_build(int i)
{
    anim_init();
    Ped *p = &peds[i];
    Priv *v = &pv[i];
    if (!ready) return;
    Quaternion rot[MAX_BONES];
    Vector3 hips;
    Matrix xf;
    if (v->rag_on) {
        if (v->getup >= 0) getup_pose(i, rot, &hips); else rag_pose(i, rot, &hips);
        xf = MatrixIdentity();
    } else {
        stand_pose(i, rot, &hips);
        if (p->state == PS_IN_CAR && p->car >= 0 && cars[p->car].used) {
            xf = seat_xf(p, v);
        } else {
            float s = v->scale;
            xf = MatrixMultiply(MatrixMultiply(MatrixScale(s, s, s), MatrixRotateY(p->yaw)),
                                MatrixTranslate(p->pos.x, p->pos.y, p->pos.z));
        }
    }
    pose_matrices(rot, hips, v->bones);
    v->xf = xf;
    v->ready = true;
}

void ped_pose(const Ped *p, Matrix *out)
{
    int i = (int)(p - peds);
    if (i >= 0 && i < MAX_PEDS && pv[i].ready) memcpy(out, pv[i].bones, sizeof(Matrix) * MAX_BONES);
    else for (int j = 0; j < MAX_BONES; j++) out[j] = MatrixIdentity();
}

Vector3 ped_hand(const Ped *p)
{
    int i = (int)(p - peds);
    if (i < 0 || i >= MAX_PEDS || !pv[i].ready) return Vector3Add(p->pos, (Vector3){ 0, 1.2f, 0 });
    return Vector3Transform(bind_p[skel.hand_r], MatrixMultiply(pv[i].bones[skel.hand_r], pv[i].xf));
}

void peds_draw(int pass)
{
    static const Matrix grip = { -1, 0, 0, 0.0f, 0, 0, 1, 0.07f, 0, 1, 0, 0, 0, 0, 0, 1 };
    float maxd = pass == PASS_SHADOW ? 55 : 150;
    for (int i = 0; i < MAX_PEDS; i++) {
        Ped *p = &peds[i];
        Priv *v = &pv[i];
        if (!p->used || !v->ready) continue;
        Vector3 d = Vector3Subtract(p->pos, cam.position);
        float dist2 = d.x * d.x + d.z * d.z;
        if (dist2 > maxd * maxd) continue;
        if (pass == PASS_MAIN && dist2 > 144) {
            Vector3 f = Vector3Subtract(cam.target, cam.position);
            if (d.x * f.x + d.z * f.z < 0) continue;
        }
        if (p->state == PS_IN_CAR && p->car >= 0 && cars[p->car].used) v->xf = seat_xf(p, v);
        draw_char(body_mesh[p->body], v->xf, v->bones, p->pal);
        if (p->hair >= 0) draw_char(hair_mesh[p->hair], v->xf, v->bones, p->pal);
        if (p->weapon != W_FIST && p->state != PS_IN_CAR && gun_mesh[p->weapon].vaoId) {
            Matrix hand = MatrixMultiply(MatrixInvert(skel.inv_bind[skel.hand_r]), v->bones[skel.hand_r]);
            draw_mesh(gun_mesh[p->weapon], MatrixMultiply(MatrixMultiply(grip, hand), v->xf), WHITE, MAT_PROP);
        }
    }
}
