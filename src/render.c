#include "city.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLSL(...) #__VA_ARGS__

float day_hours;
int quality = 2;
Vector3 sun_dir;
Matrix r_view, r_proj, r_vp;
Vector3 r_cam_right, r_cam_up;
Vector3 r_sun_col, r_amb_col, r_fog_col;
float r_night;

static int SH_SIZE = 4096;
#define SH_HALF 80.0f
static int MSAA = 4;
static float render_scale = 1;
static GLenum hdr_fmt = GL_RGBA16F, bloom_fmt = GL_R11F_G11F_B10F;
#define NB 6

// ---------------------------------------------------------------- shader plumbing

enum { U_VP, U_MODEL, U_TINT, U_BONES, U_PAL, U_LVP, U_LDIR, U_LCOL, U_SUNDIR, U_MOONDIR, U_SUNGLOW, U_SKY,
       U_GND, U_TOP, U_HOR, U_FOG, U_FOGDEN, U_CAM, U_NIGHT, U_TIME, U_SHINFO, U_LIGHTS, U_LIGHTCOL,
       U_NLIGHTS, U_LAMPS, U_NLAMPS, U_CELLS, U_INVVP, U_SRC, U_SRC2, U_TEXEL, U_EXPO, U_BLOOM, U_GRADE, U_OFF,
       U_COUNT };

static const char *uname[U_COUNT] = {
    "uVP", "uModel", "uTint", "uBones", "uPal", "uLVP", "uLDir", "uLCol", "uSunDir", "uMoonDir", "uSunGlow",
    "uSky", "uGnd", "uTop", "uHor", "uFog", "uFogDen", "uCam", "uNight", "uTime", "uShInfo", "uLights",
    "uLightCol", "uNLights", "uLamps", "uNLamps", "uCells", "uInvVP", "uSrc", "uSrc2", "uTexel", "uExpo",
    "uBloom", "uGrade", "uOff",
};

typedef struct { GLuint id; GLint u[U_COUNT]; unsigned stamp; } Prog;

static char prelude[1024];

static void pretty(char *dst, const char *src)
{
    for (; *src; src++) {
        *dst++ = *src;
        if (*src == ';' || *src == '{' || *src == '}') *dst++ = '\n';
    }
    *dst = 0;
}

static GLuint compile(GLenum type, const char *body)
{
    char *src = malloc(strlen(prelude) + strlen(body) * 2 + 16);
    strcpy(src, prelude);
    pretty(src + strlen(src), body);
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, (const char **)&src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader error (%s):\n%s\n", type == GL_VERTEX_SHADER ? "vs" : "fs", log);
        int line = 1;
        for (char *c = src; *c; c++) {
            if (c == src || c[-1] == '\n') fprintf(stderr, "%3d ", line++);
            fputc(*c, stderr);
        }
        exit(1);
    }
    free(src);
    return s;
}

GLuint r_program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    GLuint a = compile(GL_VERTEX_SHADER, vs), b = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, a);
    glAttachShader(p, b);
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "link error:\n%s\n", log);
        exit(1);
    }
    glDeleteShader(a);
    glDeleteShader(b);
    return p;
}

static Prog prog_make(const char *vs, const char *fs)
{
    Prog p = { r_program(vs, fs) };
    glUseProgram(p.id);
    for (int i = 0; i < U_COUNT; i++) p.u[i] = glGetUniformLocation(p.id, uname[i]);
    glUniform1i(glGetUniformLocation(p.id, "uTex"), 0);
    glUniform1i(glGetUniformLocation(p.id, "uShadow"), 1);
    glUniform1i(glGetUniformLocation(p.id, "uSig"), 2);
    return p;
}

// ---------------------------------------------------------------- shader source

static const char *VS_FULL = GLSL(
    out vec2 vUV;
    void main() {
        vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
        vUV = p;
        gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    });

static const char *VS_STATIC = GLSL(
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec2 aUV;
    layout(location = 2) in vec3 aN;
    layout(location = 3) in vec4 aCol;
    layout(location = 5) in vec2 aLS;
    uniform mat4 uVP;
    uniform vec3 uOff;
    out vec3 vPos; out vec2 vUV; out vec3 vN; out vec4 vCol;
    flat out float vLayer; out float vSeed; flat out int vFlag;
    void main() {
        vPos = aPos + uOff; vUV = aUV; vN = aN; vCol = aCol; vLayer = aLS.x; vSeed = aLS.y;
        vFlag = int(aCol.a * 255.0 + 0.5);
        gl_Position = uVP * vec4(vPos, 1.0);
        if ((vFlag & 32) != 0) gl_Position.z -= gl_Position.w * 3e-6;
    });

static const char *VS_SHADOW_STATIC = GLSL(
    layout(location = 0) in vec3 aPos;
    layout(location = 1) in vec2 aUV;
    layout(location = 3) in vec4 aCol;
    layout(location = 5) in vec2 aLS;
    uniform mat4 uVP;
    uniform vec3 uOff;
    out vec2 vUV; flat out float vLayer; flat out int vFlag;
    void main() {
        vUV = aUV; vLayer = aLS.x; vFlag = int(aCol.a * 255.0 + 0.5);
        gl_Position = uVP * vec4(aPos + uOff, 1.0);
    });

static const char *FS_SHADOW_STATIC = GLSL(
    in vec2 vUV; flat in float vLayer; flat in int vFlag;
    uniform sampler2DArray uTex;
    void main() {
        if ((vFlag & 2) != 0 && texture(uTex, vec3(vUV, vLayer)).a < 0.5) discard;
    });

static const char *VS_SHADOW_MESH = GLSL(
    layout(location = 0) in vec3 aPos;
    uniform mat4 uVP, uModel;
    void main() { gl_Position = uVP * (uModel * vec4(aPos, 1.0)); });

static const char *VS_SHADOW_SKIN = GLSL(
    layout(location = 0) in vec3 aPos;
    layout(location = 7) in vec4 aIds;
    layout(location = 8) in vec4 aW;
    uniform mat4 uVP, uModel;
    uniform mat4 uBones[32];
    void main() {
        mat4 s = aW.x * uBones[int(aIds.x + 0.5)] + aW.y * uBones[int(aIds.y + 0.5)]
               + aW.z * uBones[int(aIds.z + 0.5)] + aW.w * uBones[int(aIds.w + 0.5)];
        gl_Position = uVP * (uModel * (s * vec4(aPos, 1.0)));
    });

static const char *FS_EMPTY = GLSL(void main() {});

// shared by every lit fragment shader
static const char *FS_COMMON = GLSL(
    uniform vec3 uLDir, uLCol, uSunDir, uMoonDir, uSunGlow, uSky, uGnd, uTop, uHor, uFog, uCam;
    uniform float uFogDen, uNight, uTime;
    uniform mat4 uLVP;
    uniform sampler2DShadow uShadow;
    uniform vec4 uShInfo;
    uniform vec4 uLights[24]; uniform vec3 uLightCol[24]; uniform int uNLights;
    uniform vec4 uLamps[32]; uniform int uNLamps;

    float hash12(vec2 p) {
        vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        p3 += dot(p3, p3.yzx + 33.33);
        return fract((p3.x + p3.y) * p3.z);
    }
    float hash13(vec3 p3) {
        p3 = fract(p3 * 0.1031);
        p3 += dot(p3, p3.zyx + 31.32);
        return fract((p3.x + p3.y) * p3.z);
    }
    float vnoise(vec2 p) {
        vec2 i = floor(p), f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        return mix(mix(hash12(i), hash12(i + vec2(1, 0)), f.x), mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), f.x), f.y);
    }
    float fbm(vec2 p) {
        float a = 0.5, s = 0.0;
        for (int i = 0; i < 5; i++) { s += a * vnoise(p); p = p * 2.03 + 17.1; a *= 0.5; }
        return s;
    }
    vec3 sky_grad(vec3 d) {
        float h = clamp(d.y, 0.0, 1.0);
        vec3 c = mix(uHor, uTop, pow(max(h, 0.0), 0.5));
        if (d.y < 0.0) c = mix(uHor, uHor * 0.5, clamp(-d.y * 3.0, 0.0, 1.0));
        float s = max(dot(d, uSunDir), 0.0);
        c += uSunGlow * (pow(max(s, 0.0), 6.0) * 0.12 + pow(max(s, 0.0), 40.0) * 0.6);
        return c;
    }
    float shadow_at(vec3 p, vec3 n, float ndl) {
        float k = uShInfo.x;
        vec3 q = p + n * k * 1.8 + uLDir * k * 0.6;
        vec3 c = (uLVP * vec4(q, 1.0)).xyz * 0.5 + 0.5;
        if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0 || c.z > 1.0) return 1.0;
        float bias = (0.05 + 0.12 * (1.0 - ndl)) / uShInfo.y;
        float s = 0.0;
        for (int i = -1; i <= 1; i++)
            for (int j = -1; j <= 1; j++)
                s += texture(uShadow, vec3(c.xy + vec2(i, j) * uShInfo.z, c.z - bias));
        s /= 9.0;
        float e = max(abs(c.x - 0.5), abs(c.y - 0.5)) * 2.0;
        return mix(s, 1.0, smoothstep(0.75, 1.0, e));
    }
    vec3 dyn_lights(vec3 p, vec3 n) {
        vec3 c = vec3(0.0);
        for (int i = 0; i < uNLights; i++) {
            vec3 d = uLights[i].xyz - p;
            float r = uLights[i].w, q = dot(d, d) / (r * r);
            if (q >= 1.0) continue;
            float a = (1.0 - q) * (1.0 - q);
            c += uLightCol[i] * a * (max(dot(n, normalize(d)), 0.0) * 0.85 + 0.15);
        }
        if (uNight > 0.01) {
            for (int i = 0; i < uNLamps; i++) {
                vec3 d = uLamps[i].xyz - p;
                float r = uLamps[i].w, q = dot(d, d) / (r * r);
                if (q >= 1.0) continue;
                float a = (1.0 - q) * (1.0 - q) / (1.0 + q * 3.0);
                c += vec3(1.0, 0.65, 0.32) * 3.0 * uNight * a * (max(dot(n, normalize(d)), 0.0) * 0.8 + 0.2);
            }
        }
        // Compress overlapping light pools before material shading, preserving their hue.
        return c / (1.0 + 0.28 * uNight * max(c.r, max(c.g, c.b)));
    }
    vec3 apply_fog(vec3 c, vec3 p) {
        vec3 v = p - uCam;
        float d = length(v);
        float f = 1.0 - exp(-d * uFogDen * exp(-max(p.y, 0.0) * 0.0045));
        vec3 fc = uFog + uSunGlow * 0.5 * pow(max(dot(v / d, uSunDir), 0.0), 8.0);
        return mix(c, fc, f);
    });

static const char *FS_WORLD = GLSL(
    in vec3 vPos; in vec2 vUV; in vec3 vN; in vec4 vCol;
    flat in float vLayer; in float vSeed; flat in int vFlag;
    out vec4 oCol;
    uniform sampler2DArray uTex;
    uniform sampler2D uSig;
    uniform vec2 uCells[64];
    void main() {
        int fl = vFlag, ly = int(vLayer + 0.5);
        vec4 t = texture(uTex, vec3(vUV, vLayer));
        bool cut = (fl & 2) != 0;
        float ca = 1.0;
        if (cut) {
            ca = clamp((t.a - 0.5) / max(fwidth(t.a), 0.0001) + 0.5, 0.0, 1.0);
            if (ca <= 0.0) discard;
        }
        vec3 V = uCam - vPos;
        float dist = length(V);
        V /= dist;
        vec3 N = vN / max(length(vN), 0.0001);
        if (dot(N, V) < 0.0) N = -N;
        vec3 alb = t.rgb * pow(max(vCol.rgb, vec3(1e-4)), vec3(2.2));
        if ((fl & 16) != 0) {
            vec3 g = texture(uTex, vec3(vUV, float(L_GRASS))).rgb * pow(max(vCol.rgb, vec3(1e-4)), vec3(2.2));
            float dn = texture(uTex, vec3(vPos.xz * 0.045, float(L_DETAIL))).r;
            alb = mix(alb, g, smoothstep(0.42, 0.58, vSeed + (dn - 0.5) * 0.7));
        }
        vec2 duv = abs(N.y) > 0.6 ? vPos.xz : (abs(N.x) > abs(N.z) ? vPos.zy : vPos.xy);
        float dt = texture(uTex, vec3(duv * 0.6, float(L_DETAIL))).r;
        alb *= mix(1.0, dt * 2.0, 0.3 * (1.0 - smoothstep(25.0, 110.0, dist)));

        if (ly == L_GRASS || ly == L_SAND || ly == L_ASPHALT || ly == L_SIDEWALK || ly == L_QUAY || ly == L_SNOW) {
            float mv = vnoise(vPos.xz * 0.045) * 0.6 + vnoise(vPos.xz * 0.17 + 7.0) * 0.4;
            alb *= mix(0.8, 1.18, mv);
            if (ly == L_GRASS) alb = mix(alb, alb * vec3(1.3, 1.0, 0.55), smoothstep(0.55, 0.8, vnoise(vPos.xz * 0.02 + 3.0)));
        }
        bool leaf = ly == L_PALM || ly == L_LEAF;
        float ndl = leaf ? pow(dot(N, uLDir) * 0.5 + 0.5, 2.0) : max(dot(N, uLDir), 0.0);
        float sh = ndl > 0.0 ? shadow_at(vPos, N, ndl) : 1.0;
        vec3 amb = mix(uGnd, uSky, N.y * 0.5 + 0.5);
        float ao = N.y > 0.7 ? 1.0 : 0.86;
        if (leaf) ao = mix(0.5, 1.0, clamp(vSeed, 0.0, 1.0));
        vec3 emis = vec3(0.0);
        vec3 direct = uLCol * ndl * sh;
        if (leaf) { direct += uLCol * sh * 0.35 * max(dot(-N, uLDir), 0.0); amb *= 1.35; }

        if ((fl & 1) != 0) {
            vec2 cell = floor(vUV * uCells[ly]);
            float h = hash12(cell + vec2(vSeed * 1.37, vSeed * 0.71));
            float glass = t.a;
            float fr = pow(1.0 - max(dot(N, V), 0.0), 4.0);
            vec3 r = sky_grad(reflect(-V, N));
            alb *= 1.0 + glass * (h - 0.5) * 0.5;
            emis += r * glass * (0.06 + 0.4 * fr) * (1.0 - uNight * 0.8);
            float on = step(h, 0.4 * uNight);
            vec3 wc = h2c(fract(h * 9.7));
            float lvl = 0.12 + 0.38 * fract(h * 13.1);
            emis += on * glass * wc * lvl;
            alb *= 1.0 - on * glass * 0.7;
        }
        if ((fl & 8) != 0) {
            float fr = 0.12 + 0.8 * pow(1.0 - max(dot(N, V), 0.0), 4.0);
            emis += sky_grad(reflect(-V, N)) * fr * 0.8;
        }
        vec3 col;
        if ((fl & 64) != 0) {
            col = alb * mix(vec3(0.5, 0.48, 0.46), vec3(0.34, 0.24, 0.14), uNight) * 0.9;
            col += dyn_lights(vPos, N) * alb * 0.3;
        } else {
            col = alb * (direct + amb * ao + dyn_lights(vPos, N)) + emis;
        }
        if ((fl & 4) != 0) col += alb * uNight * vSeed * 0.3;
        if ((fl & 128) != 0) {
            int g = int(vSeed) / 4, lamp = int(vSeed) % 4;
            int st = int(texelFetch(uSig, ivec2(g, 0), 0).r * 255.0 + 0.5);
            bool on = (lamp == 0 && st == 2) || (lamp == 1 && st == 1) || (lamp == 2 && st == 0);
            vec3 lc = lamp == 0 ? vec3(5.0, 0.25, 0.15) : (lamp == 1 ? vec3(5.0, 3.0, 0.2) : vec3(0.3, 5.0, 1.8));
            col = on ? lc : vec3(0.015);
        }
        oCol = vec4(apply_fog(col, vPos), ca);
    });

static const char *FS_WORLD_FIX =
    "vec3 h2c(float h) { return h < 0.5 ? vec3(1.0, 0.55, 0.22) : (h < 0.8 ? vec3(1.0, 0.72, 0.4) : (h < 0.93 ? vec3(0.75, 0.85, 0.8) : vec3(0.5, 0.6, 0.95))); }\n";

static const char *FS_GLASS = GLSL(
    in vec3 vPos; in vec2 vUV; in vec3 vN; in vec4 vCol;
    flat in float vLayer; in float vSeed; flat in int vFlag;
    out vec4 oCol;
    void main() {
        vec3 V = normalize(uCam - vPos);
        vec3 N = normalize(vN);
        if (dot(N, V) < 0.0) N = -N;
        float f = pow(1.0 - max(dot(N, V), 0.0), 4.0);
        vec3 r = sky_grad(reflect(-V, N));
        float a = 0.06 + 0.7 * f;
        vec3 spec = uLCol * pow(max(dot(reflect(-V, N), uLDir), 0.0), 200.0) * 0.4;
        vec3 col = (r * (0.25 + f) + spec) * a + pow(max(vCol.rgb, vec3(1e-4)), vec3(2.2)) * 0.02;
        oCol = vec4(apply_fog(col, vPos), a);
    });

static const char *VS_WATER = GLSL(
    layout(location = 0) in vec3 aPos;
    layout(location = 5) in vec2 aLS;
    uniform mat4 uVP;
    uniform vec3 uOff;
    out vec3 vPos; out float vDepth;
    void main() { vPos = aPos + uOff; vDepth = aLS.y; gl_Position = uVP * vec4(vPos, 1.0); });

static const char *FS_WATER = GLSL(
    in vec3 vPos; in float vDepth;
    out vec4 oCol;
    vec2 wave(vec2 p, vec2 d, float k, float a, float s) {
        float ph = k * dot(p, d) + s * uTime;
        return d * a * k * cos(ph);
    }
    void main() {
        vec3 V = uCam - vPos;
        float dist = length(V);
        V /= dist;
        vec2 p = vPos.xz;
        vec2 g = wave(p, normalize(vec2(1.0, 0.3)), 0.11, 0.45, 0.9)
               + wave(p, normalize(vec2(0.4, 1.0)), 0.17, 0.3, 1.2)
               + wave(p, normalize(vec2(-0.7, 0.6)), 0.29, 0.18, 1.5)
               + wave(p, normalize(vec2(-0.5, -0.9)), 0.43, 0.1, 1.8);
        float ripple = 1.0 - smoothstep(40.0, 200.0, dist);
        vec2 q = p * 0.35 + vec2(uTime * 0.25, -uTime * 0.18);
        vec2 gn = vec2(vnoise(q) - vnoise(q + vec2(5.2, 1.3)), vnoise(q * 1.7 + 3.1) - vnoise(q * 1.7 + 9.4)) * 0.5;
        g += ripple * gn * 1.4;
        vec3 N = normalize(vec3(-g.x, 1.0, -g.y));
        N = normalize(mix(N, vec3(0.0, 1.0, 0.0), smoothstep(150.0, 700.0, dist) * 0.9));
        float nv = max(dot(N, V), 0.0);
        float fr = 0.02 + 0.98 * pow(clamp(1.0 - nv, 0.0, 1.0), 5.0);
        vec3 R = reflect(-V, N);
        R.y = abs(R.y);
        vec3 refl = sky_grad(R);
        float dd = 1.0 - exp(-vDepth * 0.35);
        vec3 lightc = uSky * 0.55 + uLCol * 0.1;
        vec3 body = mix(vec3(0.06, 0.34, 0.36), vec3(0.01, 0.07, 0.17), dd) * lightc;
        vec3 col = mix(body, refl, clamp(fr * 1.1, 0.0, 1.0));
        col += uLCol * pow(max(dot(R, uLDir), 0.0), 400.0) * 2.0;
        float n = vnoise(p * 0.7 + uTime * 0.3);
        float foam = smoothstep(0.0, 0.9, 0.9 - vDepth - (n - 0.5) * 1.1) * 0.85;
        float wash = smoothstep(0.82, 1.0, sin(vDepth * 1.3 - uTime * 0.9 + n * 2.5)) * smoothstep(1.0, 7.0, 8.0 - vDepth) * 0.4;
        foam = clamp(foam + wash, 0.0, 1.0);
        col = mix(col, (uSky * 0.6 + uLCol * 0.25 + 0.04) * vec3(0.95, 0.98, 1.0), foam);
        float a = clamp(smoothstep(0.0, 1.4, vDepth) * 0.9 + 0.1, 0.0, 1.0);
        a = max(a, max(fr, foam));
        oCol = vec4(apply_fog(col, vPos) * a, a);
    });

static const char *VS_MESH = GLSL(
    layout(location = 0) in vec3 aPos;
    layout(location = 2) in vec3 aN;
    layout(location = 3) in vec4 aCol;
    uniform mat4 uVP, uModel;
    out vec3 vPos; out vec3 vN; out vec4 vCol;
    void main() {
        vec4 w = uModel * vec4(aPos, 1.0);
        vPos = w.xyz; vN = mat3(uModel) * aN; vCol = aCol;
        gl_Position = uVP * w;
    });

static const char *VS_SKIN = GLSL(
    layout(location = 0) in vec3 aPos;
    layout(location = 2) in vec3 aN;
    layout(location = 3) in vec4 aCol;
    layout(location = 7) in vec4 aIds;
    layout(location = 8) in vec4 aW;
    uniform mat4 uVP, uModel;
    uniform mat4 uBones[32];
    out vec3 vPos; out vec3 vN; out vec4 vCol;
    void main() {
        mat4 s = aW.x * uBones[int(aIds.x + 0.5)] + aW.y * uBones[int(aIds.y + 0.5)]
               + aW.z * uBones[int(aIds.z + 0.5)] + aW.w * uBones[int(aIds.w + 0.5)];
        vec4 w = uModel * (s * vec4(aPos, 1.0));
        vPos = w.xyz; vN = mat3(uModel) * (mat3(s) * aN); vCol = aCol;
        gl_Position = uVP * w;
    });

static const char *FS_CAR = GLSL(
    in vec3 vPos; in vec3 vN; in vec4 vCol;
    out vec4 oCol;
    uniform vec4 uTint;
    void main() {
        int m = int(vCol.a * 255.0 + 0.5), flags = int(uTint.a * 255.0 + 0.5);
        vec3 V = normalize(uCam - vPos);
        vec3 N = normalize(vN);
        if (dot(N, V) < 0.0) N = -N;
        vec3 base = pow(max(vCol.rgb, vec3(1e-4)), vec3(2.2));
        float lum = dot(base, vec3(0.333));
        vec3 alb = base, emis = vec3(0.0);
        float refl = 0.02, spec = 0.0, rough = 40.0;
        float nv = max(dot(N, V), 0.0);
        float fr = 0.04 + 0.96 * pow(clamp(1.0 - nv, 0.0, 1.0), 5.0);
        if (m >= 224) { alb = pow(max(uTint.rgb, vec3(1e-4)), vec3(2.2)) * pow(max(lum, 0.0), 0.4); refl = fr * 0.85 + 0.03; spec = 1.0; rough = 140.0; }
        else if (m >= 96 && m < 160) { alb = vec3(0.012, 0.016, 0.02); refl = 0.18 + 0.8 * fr; spec = 1.0; rough = 220.0; }
        else if (m >= 160) {
            bool red = base.r > base.b;
            float ph = fract(uTime * 2.6 + (red ? 0.0 : 0.5));
            bool on = (flags & 4) != 0 && (ph < 0.12 || (ph > 0.2 && ph < 0.32));
            emis = (red ? vec3(1.0, 0.05, 0.03) : vec3(0.1, 0.25, 1.0)) * (on ? mix(14.0, 3.5, uNight) : 0.15);
            alb *= 0.2;
        }
        else if (m >= 48) {
            float lit = max(uNight, (flags & 1) != 0 ? 0.35 : 0.0);
            emis = mix(vec3(1.0, 0.93, 0.78) * 0.35, vec3(0.9, 0.55, 0.26), lit);
            alb = base * 0.5;
        }
        else if (m >= 16) {
            float k = (flags & 2) != 0 ? 5.0 : 0.25 + 1.6 * uNight;
            emis = vec3(1.0, 0.04, 0.02) * k * (0.5 + base.r);
            alb = base * 0.5;
        }
        float ndl = max(dot(N, uLDir), 0.0);
        float sh = ndl > 0.0 ? shadow_at(vPos, N, ndl) : 1.0;
        vec3 amb = mix(uGnd, uSky, N.y * 0.5 + 0.5);
        vec3 col = alb * (uLCol * ndl * sh + amb * 0.92 + dyn_lights(vPos, N)) + emis;
        vec3 R = reflect(-V, N);
        col += sky_grad(R) * refl * (R.y < 0.0 ? 0.5 : 1.0);
        vec3 H = normalize(uLDir + V);
        col += uLCol * sh * spec * pow(max(dot(N, H), 0.0), rough) * (rough * 0.02 + 0.5) * fr * 2.0 * step(0.0, ndl);
        oCol = vec4(apply_fog(col, vPos), 1.0);
    });

static const char *FS_PROP = GLSL(
    in vec3 vPos; in vec3 vN; in vec4 vCol;
    out vec4 oCol;
    uniform vec4 uTint;
    void main() {
        vec3 V = normalize(uCam - vPos);
        vec3 N = normalize(vN);
        if (dot(N, V) < 0.0) N = -N;
        vec3 alb = pow(max(vCol.rgb, vec3(1e-4)), vec3(2.2)) * pow(max(uTint.rgb, vec3(1e-4)), vec3(2.2));
        float ndl = max(dot(N, uLDir), 0.0);
        float sh = ndl > 0.0 ? shadow_at(vPos, N, ndl) : 1.0;
        vec3 amb = mix(uGnd, uSky, N.y * 0.5 + 0.5);
        vec3 col = alb * (uLCol * ndl * sh + amb + dyn_lights(vPos, N));
        oCol = vec4(apply_fog(col, vPos), 1.0);
    });

static const char *FS_CHAR = GLSL(
    in vec3 vPos; in vec3 vN; in vec4 vCol;
    out vec4 oCol;
    uniform vec3 uPal[6];
    void main() {
        vec3 V = normalize(uCam - vPos);
        vec3 N = normalize(vN);
        int reg = clamp(int(vCol.r * 255.0 + 0.5), 0, 5);
        vec3 alb = uPal[reg];
        float ndl = max(dot(N, uLDir), 0.0);
        float sh = ndl > 0.0 ? shadow_at(vPos, N, ndl) : 1.0;
        vec3 amb = mix(uGnd, uSky, N.y * 0.5 + 0.5);
        vec3 col = alb * (uLCol * ndl * sh + amb * 1.05 + dyn_lights(vPos, N));
        float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);
        col += rim * 0.22 * (uSky + uLCol * 0.15) * (0.4 + 0.6 * sh);
        oCol = vec4(apply_fog(col, vPos), 1.0);
    });

static const char *FS_SKY = GLSL(
    in vec2 vUV;
    out vec4 oCol;
    uniform mat4 uInvVP;
    void main() {
        vec4 w = uInvVP * vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
        vec3 d = normalize(w.xyz / w.w - uCam);
        vec3 c = sky_grad(d);
        float sd = dot(d, uSunDir);
        float disc = smoothstep(0.99993, 0.99997, sd);
        c += uSunGlow * disc * 40.0 * step(0.0, d.y + 0.01);
        if (uNight > 0.02 && d.y > -0.05) {
            vec3 q = d * 90.0;
            vec3 cell = floor(q);
            float h = hash13(cell);
            if (h > 0.985) {
                vec3 sp = cell + 0.5 + (vec3(hash13(cell + 1.7), hash13(cell + 5.3), hash13(cell + 9.1)) - 0.5) * 0.7;
                float s = smoothstep(0.2, 0.0, length(q - sp)) * (0.4 + 2.5 * fract(h * 91.0));
                c += vec3(0.8, 0.88, 1.0) * s * uNight * smoothstep(0.0, 0.15, d.y);
            }
        }
        float md = dot(d, uMoonDir);
        if (uMoonDir.y > -0.1) {
            float m = smoothstep(0.99935, 0.99955, md);
            float tex = 0.75 + 0.25 * vnoise(d.xz * 220.0 + d.y * 130.0);
            c += vec3(0.85, 0.9, 1.0) * m * tex * 2.2 * uNight;
            c += vec3(0.35, 0.45, 0.7) * pow(max(md, 0.0), 300.0) * 0.25 * uNight;
        }
        if (d.y > 0.01) {
            vec2 p = d.xz / (d.y + 0.2) * 1.4 + vec2(uTime * 0.01, uTime * 0.004);
            float n = fbm(p);
            float cover = smoothstep(0.5, 0.78, n) * smoothstep(0.01, 0.2, d.y);
            float n2 = fbm(p + uLDir.xz * 0.12);
            float lit = clamp((n - n2) * 3.5 + 0.55, 0.0, 1.0);
            vec3 cl = mix(uTop * 0.8 + uHor * 0.35, uSunGlow * 1.3 + uTop * 0.7 + uHor * 0.3, lit);
            c = mix(c, cl, cover * 0.92);
        }
        oCol = vec4(c, 1.0);
    });

static const char *FS_BRIGHT = GLSL(
    in vec2 vUV; out vec4 oCol;
    uniform sampler2D uSrc; uniform vec2 uTexel; uniform float uExpo;
    vec3 bp(vec3 c) {
        if (any(isnan(c)) || any(isinf(c))) return vec3(0.0);  // one bad pixel would smear across the whole bloom chain
        c = min(c * uExpo, vec3(64.0));
        float b = max(c.r, max(c.g, c.b));
        float k = 0.5;
        float s = clamp(b - 1.0 + k, 0.0, 2.0 * k);
        s = s * s / (4.0 * k + 1e-4);
        return c * (max(s, b - 1.0) / max(b, 1e-4));
    }
    void main() {
        vec3 s = vec3(0.0);
        float w = 0.0;
        for (int i = 0; i < 4; i++) {
            vec2 o = vec2(i & 1, i >> 1) * 2.0 - 1.0;
            vec3 c = bp(texture(uSrc, vUV + o * uTexel).rgb);
            float k = 1.0 / (1.0 + dot(c, vec3(0.3, 0.6, 0.1)));
            s += c * k; w += k;
        }
        oCol = vec4(s / w, 1.0);
    });

static const char *FS_DOWN = GLSL(
    in vec2 vUV; out vec4 oCol;
    uniform sampler2D uSrc; uniform vec2 uTexel;
    void main() {
        vec3 s = texture(uSrc, vUV).rgb * 4.0;
        s += texture(uSrc, vUV + uTexel * vec2(-1, -1)).rgb + texture(uSrc, vUV + uTexel * vec2(1, -1)).rgb;
        s += texture(uSrc, vUV + uTexel * vec2(-1, 1)).rgb + texture(uSrc, vUV + uTexel * vec2(1, 1)).rgb;
        oCol = vec4(s / 8.0, 1.0);
    });

static const char *FS_UP = GLSL(
    in vec2 vUV; out vec4 oCol;
    uniform sampler2D uSrc; uniform vec2 uTexel;
    void main() {
        vec3 s = texture(uSrc, vUV).rgb * 4.0;
        s += (texture(uSrc, vUV + uTexel * vec2(-1, 0)).rgb + texture(uSrc, vUV + uTexel * vec2(1, 0)).rgb
            + texture(uSrc, vUV + uTexel * vec2(0, -1)).rgb + texture(uSrc, vUV + uTexel * vec2(0, 1)).rgb) * 2.0;
        s += texture(uSrc, vUV + uTexel * vec2(-1, -1)).rgb + texture(uSrc, vUV + uTexel * vec2(1, -1)).rgb
           + texture(uSrc, vUV + uTexel * vec2(-1, 1)).rgb + texture(uSrc, vUV + uTexel * vec2(1, 1)).rgb;
        oCol = vec4(s / 16.0, 1.0);
    });

static const char *FS_COMPOSE = GLSL(
    in vec2 vUV; out vec4 oCol;
    uniform sampler2D uSrc, uSrc2;
    uniform float uExpo, uBloom;
    uniform vec4 uGrade;
    vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
    vec3 enc(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
    void main() {
        vec3 c = texture(uSrc, vUV).rgb * uExpo + texture(uSrc2, vUV).rgb * uBloom;
        if (any(isnan(c))) c = vec3(0.0);
        c = aces(c);
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = mix(vec3(l), c, uGrade.w) * uGrade.rgb;
        c = enc(clamp(c, 0.0, 1.0));
        c = mix(c, c * c * (3.0 - 2.0 * c), 0.18);
        vec2 q = vUV - 0.5;
        c *= 1.0 - dot(q, q) * 0.38;
        float n = fract(sin(dot(vUV * 1733.0, vec2(12.9898, 78.233))) * 43758.5453);
        c += (n - 0.5) / 255.0;
        oCol = vec4(c, 1.0);
    });

static const char *FS_FXAA = GLSL(
    in vec2 vUV; out vec4 oCol;
    uniform sampler2D uSrc; uniform vec2 uTexel;
    float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
    void main() {
        vec3 nw = texture(uSrc, vUV + vec2(-1, -1) * uTexel).rgb, ne = texture(uSrc, vUV + vec2(1, -1) * uTexel).rgb;
        vec3 sw = texture(uSrc, vUV + vec2(-1, 1) * uTexel).rgb, se = texture(uSrc, vUV + vec2(1, 1) * uTexel).rgb;
        vec3 m = texture(uSrc, vUV).rgb;
        float lnw = lum(nw), lne = lum(ne), lsw = lum(sw), lse = lum(se), lm = lum(m);
        float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
        vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
        float red = max((lnw + lne + lsw + lse) * 0.03125, 1.0 / 128.0);
        float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + red);
        dir = clamp(dir * rcp, -8.0, 8.0) * uTexel;
        vec3 a = 0.5 * (texture(uSrc, vUV + dir * (1.0 / 3.0 - 0.5)).rgb + texture(uSrc, vUV + dir * (2.0 / 3.0 - 0.5)).rgb);
        vec3 b = a * 0.5 + 0.25 * (texture(uSrc, vUV + dir * -0.5).rgb + texture(uSrc, vUV + dir * 0.5).rgb);
        float lb = lum(b);
        oCol = vec4((lb < lmin || lb > lmax) ? a : b, 1.0);
    });

// ---------------------------------------------------------------- state

static Prog pr_world, pr_glass, pr_water, pr_car, pr_prop, pr_char, pr_sky;
static Prog pr_shw, pr_shmesh, pr_shskin;
static Prog pr_bright, pr_down, pr_up, pr_compose, pr_fxaa;

static GLuint vao_empty;
static GLuint shadow_fbo, shadow_tex;
static struct {
    int w, h;
    GLuint msaa_fbo, msaa_col, msaa_depth;
    GLuint res_fbo, res_tex;
    GLuint ldr_fbo, ldr_tex;
    GLuint b_fbo[NB], b_tex[NB];
    int bw[NB], bh[NB];
} T;

static int cur_pass = PASS_MAIN;
static unsigned stamp = 1;
static GLuint cur_prog;
static Prog *cur_p;

typedef struct { Vector3 p, c; float r; } Lt;
static Lt lt_next[MAX_LIGHTS * 3];
static int n_next;

static struct {
    Matrix vp, lvp, invvp;
    Vector3 cam, ldir, lcol, sundir, moondir, sunglow, sky, gnd, top, hor, fog;
    float fogden, night, time, shinfo[4], expo, bloom, grade[4];
    int nl, nlamps;
    float lights[MAX_LIGHTS * 4], lcols[MAX_LIGHTS * 3], lamps[32 * 4];
} S;

void r_set_offset(Vector3 off)
{
    if (cur_p && cur_p->u[U_OFF] >= 0) glUniform3f(cur_p->u[U_OFF], off.x, off.y, off.z);
}

void light_add(Vector3 p, Vector3 rgb, float radius)
{
    for (int i = 0; i < n_next; i++)
        if (Vector3DistanceSqr(lt_next[i].p, p) < 0.01f) return;
    if (n_next < MAX_LIGHTS * 3) lt_next[n_next++] = (Lt){ p, rgb, radius };
}

static void use(Prog *p)
{
    if (cur_prog != p->id) { glUseProgram(p->id); cur_prog = p->id; }
    cur_p = p;
    if (p->stamp == stamp) return;
    p->stamp = stamp;
#define V3(i, v) if (p->u[i] >= 0) glUniform3f(p->u[i], (v).x, (v).y, (v).z)
#define F1(i, v) if (p->u[i] >= 0) glUniform1f(p->u[i], v)
    if (p->u[U_VP] >= 0) gl_mat(p->u[U_VP], cur_pass == PASS_SHADOW ? S.lvp : S.vp);
    if (p->u[U_LVP] >= 0) gl_mat(p->u[U_LVP], S.lvp);
    if (p->u[U_INVVP] >= 0) gl_mat(p->u[U_INVVP], S.invvp);
    V3(U_LDIR, S.ldir); V3(U_LCOL, S.lcol); V3(U_SUNDIR, S.sundir); V3(U_MOONDIR, S.moondir);
    V3(U_SUNGLOW, S.sunglow); V3(U_SKY, S.sky); V3(U_GND, S.gnd); V3(U_TOP, S.top); V3(U_HOR, S.hor);
    V3(U_FOG, S.fog); V3(U_CAM, S.cam);
    F1(U_FOGDEN, S.fogden); F1(U_NIGHT, S.night); F1(U_TIME, S.time);
    if (p->u[U_SHINFO] >= 0) glUniform4fv(p->u[U_SHINFO], 1, S.shinfo);
    if (p->u[U_LIGHTS] >= 0) glUniform4fv(p->u[U_LIGHTS], MAX_LIGHTS, S.lights);
    if (p->u[U_LIGHTCOL] >= 0) glUniform3fv(p->u[U_LIGHTCOL], MAX_LIGHTS, S.lcols);
    if (p->u[U_NLIGHTS] >= 0) glUniform1i(p->u[U_NLIGHTS], S.nl);
    if (p->u[U_LAMPS] >= 0) glUniform4fv(p->u[U_LAMPS], 32, S.lamps);
    if (p->u[U_NLAMPS] >= 0) glUniform1i(p->u[U_NLAMPS], S.nlamps);
    if (p->u[U_CELLS] >= 0) glUniform2fv(p->u[U_CELLS], L_COUNT, (float *)layer_cells);
}

// ---------------------------------------------------------------- targets

static GLuint make_tex(int w, int h, GLenum ifmt, GLenum fmt, GLenum type, GLenum filter)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static bool fbo_bad;
static void fbo_check(const char *what)
{
    GLenum s = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (s != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "framebuffer %s incomplete: 0x%x\n", what, s); fbo_bad = true; }
}

static void free_targets(void)
{
    if (!T.w) return;
    glDeleteFramebuffers(1, &T.msaa_fbo); glDeleteRenderbuffers(1, &T.msaa_col); glDeleteRenderbuffers(1, &T.msaa_depth);
    glDeleteFramebuffers(1, &T.res_fbo); glDeleteTextures(1, &T.res_tex);
    glDeleteFramebuffers(1, &T.ldr_fbo); glDeleteTextures(1, &T.ldr_tex);
    glDeleteFramebuffers(NB, T.b_fbo); glDeleteTextures(NB, T.b_tex);
    T.w = 0;
}

static void make_targets_once(int w, int h);
static void make_targets(int w, int h)
{
    fbo_bad = false;
    make_targets_once(w, h);
    // some phones list float targets but can't multisample them: drop to 8 bits, then to no MSAA
    if (fbo_bad && (hdr_fmt != GL_RGBA8 || bloom_fmt != GL_RGBA8)) { hdr_fmt = bloom_fmt = GL_RGBA8; fbo_bad = false; make_targets_once(w, h); }
    if (fbo_bad && MSAA > 1) { MSAA = 1; fbo_bad = false; make_targets_once(w, h); }
}

static void make_targets_once(int w, int h)
{
    free_targets();
    T.w = w; T.h = h;
    glGenFramebuffers(1, &T.msaa_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, T.msaa_fbo);
    glGenRenderbuffers(1, &T.msaa_col);
    glBindRenderbuffer(GL_RENDERBUFFER, T.msaa_col);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, MSAA, hdr_fmt, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, T.msaa_col);
    glGenRenderbuffers(1, &T.msaa_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, T.msaa_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, MSAA, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, T.msaa_depth);
    fbo_check("msaa");

    T.res_tex = hdr_fmt == GL_RGBA16F ? make_tex(w, h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_LINEAR) : make_tex(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    glGenFramebuffers(1, &T.res_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, T.res_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, T.res_tex, 0);
    fbo_check("resolve");

    T.ldr_tex = make_tex(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    glGenFramebuffers(1, &T.ldr_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, T.ldr_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, T.ldr_tex, 0);
    fbo_check("ldr");

    glGenFramebuffers(NB, T.b_fbo);
    for (int i = 0; i < NB; i++) {
        T.bw[i] = (w >> (i + 1)) > 1 ? w >> (i + 1) : 1;
        T.bh[i] = (h >> (i + 1)) > 1 ? h >> (i + 1) : 1;
        T.b_tex[i] = bloom_fmt == GL_R11F_G11F_B10F ? make_tex(T.bw[i], T.bh[i], GL_R11F_G11F_B10F, GL_RGB, GL_FLOAT, GL_LINEAR)
                                                    : make_tex(T.bw[i], T.bh[i], GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, T.b_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, T.b_tex[i], 0);
        fbo_check("bloom");
    }
}

static void make_shadow_map(void)
{
    glGenTextures(1, &shadow_tex);
    glBindTexture(GL_TEXTURE_2D, shadow_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SH_SIZE, SH_SIZE, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glGenFramebuffers(1, &shadow_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadow_tex, 0);
    GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    fbo_check("shadow");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);   // left bound, the live window drew into the shadow map
}

void render_init(void)
{
#ifdef GLES
    const char *ver = "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2DArray;\nprecision highp sampler2DShadow;\nprecision highp sampler2D;\n";
    // float colour targets need EXT_color_buffer_float; without it the scene renders in 8 bits
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    bool fbf = false;
    if (ext && (strstr(ext, "GL_EXT_color_buffer_float") || strstr(ext, "EXT_color_buffer_float"))) fbf = true;
    if (!fbf) { GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (int i = 0; i < n; i++) { const char *e = (const char *)glGetStringi(GL_EXTENSIONS, i); if (e && strstr(e, "color_buffer_float")) fbf = true; } }
    if (!fbf) { hdr_fmt = GL_RGBA8; bloom_fmt = GL_RGBA8; }
#else
    const char *ver = "#version 330 core\n";
#endif
    if (getenv("LONGROAD_LDR")) { hdr_fmt = GL_RGBA8; bloom_fmt = GL_RGBA8; }
    if (quality == 0) { SH_SIZE = 1536; MSAA = 2; render_scale = 0.72f; }
    else if (quality == 1) { SH_SIZE = 2048; MSAA = 4; render_scale = 0.85f; }
    int n = snprintf(prelude, sizeof prelude,
        "%sconst int L_PALM = %d, L_LEAF = %d, L_GRASS = %d, L_DETAIL = %d, L_SAND = %d, L_ASPHALT = %d, L_SIDEWALK = %d, L_QUAY = %d, L_SNOW = %d;\n", ver,
        L_PALM, L_LEAF, L_GRASS, L_DETAIL, L_SAND, L_ASPHALT, L_SIDEWALK, L_QUAY, L_SNOW);
    (void)n;

    static char fs_world[16384], fs_glass[4096], fs_water[8192];
    static char dbg_body[16384];
    const char *world_body = FS_WORLD;
    if (getenv("DBG_WORLD")) {
        // swap the last line for a debug view: 1 normal, 2 ambient, 3 albedo, 4 col before fog
        snprintf(dbg_body, sizeof dbg_body, "%s", FS_WORLD);
        char *e = strstr(dbg_body, "oCol = vec4(apply_fog(col, vPos), ca);");
        const char *rep[6] = { "oCol = vec4(N * 0.5 + 0.5, 1.0);    ", "oCol = vec4(amb, 1.0);              ", "oCol = vec4(alb, 1.0);              ", "oCol = vec4(col, 1.0);              ",
                               "oCol=vec4(vLayer/64.0,vCol.r,t.r,1);", "oCol=vec4(vCol.g,vCol.b,t.g,1.0);   " };
        int k = atoi(getenv("DBG_WORLD")) - 1;
        if (e && k >= 0 && k < 6) { memcpy(e, rep[k], strlen(rep[k])); memset(e + strlen(rep[k]), ' ', strlen("oCol = vec4(apply_fog(col, vPos), ca);") - strlen(rep[k])); }
        world_body = dbg_body;
    }
    char common[8192];
    snprintf(common, sizeof common, "%s", FS_COMMON);
    snprintf(fs_world, sizeof fs_world, "%s%s%s", common, FS_WORLD_FIX, world_body);
    snprintf(fs_glass, sizeof fs_glass, "%s%s", common, FS_GLASS);
    snprintf(fs_water, sizeof fs_water, "%s%s", common, FS_WATER);
    static char fs_car[12288], fs_prop[8192], fs_char[8192], fs_sky[12288];
    snprintf(fs_car, sizeof fs_car, "%s%s", common, FS_CAR);
    snprintf(fs_prop, sizeof fs_prop, "%s%s", common, FS_PROP);
    snprintf(fs_char, sizeof fs_char, "%s%s", common, FS_CHAR);
    snprintf(fs_sky, sizeof fs_sky, "%s%s", common, FS_SKY);

    pr_world = prog_make(VS_STATIC, fs_world);
    pr_glass = prog_make(VS_STATIC, fs_glass);
    pr_water = prog_make(VS_WATER, fs_water);
    pr_car = prog_make(VS_MESH, fs_car);
    pr_prop = prog_make(VS_MESH, fs_prop);
    pr_char = prog_make(VS_SKIN, fs_char);
    pr_sky = prog_make(VS_FULL, fs_sky);
    pr_shw = prog_make(VS_SHADOW_STATIC, FS_SHADOW_STATIC);
    pr_shmesh = prog_make(VS_SHADOW_MESH, FS_EMPTY);
    pr_shskin = prog_make(VS_SHADOW_SKIN, FS_EMPTY);
    pr_bright = prog_make(VS_FULL, FS_BRIGHT);
    pr_down = prog_make(VS_FULL, FS_DOWN);
    pr_up = prog_make(VS_FULL, FS_UP);
    pr_compose = prog_make(VS_FULL, FS_COMPOSE);
    pr_fxaa = prog_make(VS_FULL, FS_FXAA);

    glGenVertexArrays(1, &vao_empty);
    make_shadow_map();
}

void render_set_quality(int q)
{
    quality = q;
    if (q == 0) render_scale = 0.72f; else if (q == 1) render_scale = 0.85f; else render_scale = 1;
    free_targets();
}

// ---------------------------------------------------------------- per frame scene

static float smooth(float a, float b, float x)
{
    float t = Clamp((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}

static Vector3 mixv(Vector3 a, Vector3 b, float t) { return Vector3Lerp(a, b, t); }

static Vector3 sun_from_hours(float h)
{
    float a;
    if (h >= 5.5f && h <= 19.5f) a = (h - 5.5f) / 14.0f * PI;
    else a = PI + (h < 5.5f ? h + 24 - 19.5f : h - 19.5f) / 10.0f * PI;
    return Vector3Normalize((Vector3){ cosf(a), sinf(a) * 0.94f, 0.28f * sinf(a) });
}

static void scene_colors(float e)
{
    float t1 = smooth(-0.25f, 0.0f, e), t2 = smooth(0.08f, 0.55f, e);
    Vector3 topN = { 0.004f, 0.007f, 0.024f }, topD = { 0.07f, 0.11f, 0.30f }, topY = { 0.09f, 0.26f, 0.68f };
    Vector3 horN = { 0.018f, 0.028f, 0.06f }, horD = { 0.85f, 0.40f, 0.20f }, horY = { 0.56f, 0.70f, 0.90f };
    S.top = mixv(mixv(topN, topD, t1), topY, t2);
    S.hor = mixv(mixv(horN, horD, t1), horY, t2);

    float day = smooth(-0.08f, 0.3f, e);
    float low = 1 - smooth(0.0f, 0.5f, e);
    Vector3 sunHi = { 1.0f * 4.0f, 0.95f * 4.0f, 0.86f * 4.0f }, sunLo = { 1.0f * 3.8f, 0.46f * 3.8f, 0.14f * 3.8f };
    float vis = smooth(-0.07f, 0.08f, e);
    Vector3 sunc = Vector3Scale(mixv(sunHi, sunLo, powf(low, 1.1f)), vis);
    Vector3 moonc = Vector3Scale((Vector3){ 0.32f, 0.42f, 0.7f }, 0.55f * smooth(-0.06f, -0.2f, e));
    bool use_sun = e >= -0.06f;
    S.lcol = use_sun ? sunc : moonc;
    S.sundir = sun_dir;
    S.moondir = Vector3Negate(sun_dir);
    S.ldir = use_sun ? sun_dir : S.moondir;
    S.sunglow = Vector3Scale((Vector3){ 1.0f, 0.62f + 0.3f * (1 - low), 0.35f + 0.5f * (1 - low) }, vis * (0.6f + 0.4f * low));

    Vector3 skyAmb = Vector3Add(Vector3Scale(S.top, 0.55f), Vector3Scale(S.hor, 0.45f));
    {
        float l = skyAmb.x * 0.3f + skyAmb.y * 0.59f + skyAmb.z * 0.11f;
        skyAmb = Vector3Scale(mixv(skyAmb, (Vector3){ l, l, l }, 0.3f), 1.5f);
    }
    Vector3 nightAmb = { 0.022f, 0.036f, 0.075f };
    S.sky = mixv(nightAmb, skyAmb, day);
    S.gnd = mixv((Vector3){ 0.012f, 0.016f, 0.03f }, Vector3Add(Vector3Scale(S.hor, 0.3f), (Vector3){ 0.14f, 0.12f, 0.1f }), day);
    S.fog = mixv(S.hor, Vector3Scale(S.hor, 0.9f), 0.5f);
    S.fogden = Lerp(0.00048f, 0.0009f, 1 - day);
    S.night = 1 - smooth(-0.14f, 0.1f, e);
    S.expo = 0.66f + 1.45f * powf(1 - smooth(-0.1f, 0.35f, e), 1.3f);
    S.bloom = Lerp(0.14f, 0.006f, S.night);
    float g = (1 - smooth(0.05f, 0.4f, e)) * smooth(-0.1f, 0.1f, e);
    Vector3 tint = mixv(mixv((Vector3){ 0.92f, 0.98f, 1.1f }, (Vector3){ 1.0f, 1.0f, 1.0f }, day), (Vector3){ 1.07f, 0.98f, 0.88f }, g);
    S.grade[0] = tint.x; S.grade[1] = tint.y; S.grade[2] = tint.z;
    S.grade[3] = Lerp(0.88f, 1.06f, day) + 0.06f * g;
    // weather: overcast dims the sun, fog and dust close in
    float over = fmaxf(fmaxf(weather_rain, weather_snow), fmaxf(weather_fog * 0.8f, weather_dust * 0.7f));
    S.lcol = Vector3Scale(S.lcol, 1 - 0.62f * over);
    S.sunglow = Vector3Scale(S.sunglow, 1 - 0.7f * over);
    Vector3 grey = Vector3Scale((Vector3){ 0.5f, 0.52f, 0.55f }, 0.25f + 0.75f * day);
    S.top = mixv(S.top, Vector3Scale(grey, 0.8f), over * 0.85f);
    S.hor = mixv(S.hor, grey, over * 0.8f);
    Vector3 dust = Vector3Scale((Vector3){ 0.62f, 0.44f, 0.26f }, 0.2f + 0.8f * day);
    S.fog = mixv(S.fog, grey, fminf(1, (weather_fog + weather_rain * 0.6f + weather_snow) * 0.9f));
    S.fog = mixv(S.fog, dust, weather_dust * 0.9f);
    S.sky = mixv(S.sky, Vector3Scale(S.sky, 0.75f), over);
    S.fogden *= 1.6f + weather_fog * 9 + weather_dust * 11 + weather_rain * 2.5f + weather_snow * 5;
    S.grade[3] *= 1 - 0.25f * over;
    if (hdr_fmt == GL_RGBA8) { S.expo = fmaxf(S.expo, 1.0f); S.bloom *= 0.5f; }   // 8-bit scene: no headroom above 1
    r_sun_col = S.lcol; r_amb_col = S.sky; r_fog_col = S.fog; r_night = S.night;
}

static void shadow_matrix(Vector3 center)
{
    Vector3 up = { 0, 1, 0 };
    Vector3 ld = S.ldir;
    if (ld.y > 0.97f) ld = Vector3Normalize((Vector3){ ld.x, 0.97f, ld.z + 0.1f });
    Matrix lv = MatrixLookAt(Vector3Scale(ld, 300), (Vector3){ 0 }, up);
    float texel = 2 * SH_HALF / SH_SIZE;
    Vector3 c = Vector3Transform(center, lv);
    c.x = floorf(c.x / texel) * texel;
    c.y = floorf(c.y / texel) * texel;
    Matrix o = MatrixOrtho(c.x - SH_HALF, c.x + SH_HALF, c.y - SH_HALF, c.y + SH_HALF, -c.z - 400, -c.z + 400);
    S.lvp = MatrixMultiply(lv, o);
    S.shinfo[0] = texel; S.shinfo[1] = 800; S.shinfo[2] = 1.0f / SH_SIZE; S.shinfo[3] = 1;
}

static void collect_lights(void)
{
    Lt c[MAX_LIGHTS * 4];
    int n = 0;
    Vector3 p[MAX_LIGHTS], col[MAX_LIGHTS];
    float r[MAX_LIGHTS];
    int nf = fx_lights(p, col, r, MAX_LIGHTS);
    for (int i = 0; i < nf; i++) c[n++] = (Lt){ p[i], col[i], r[i] };
    for (int i = 0; i < n_next && n < MAX_LIGHTS * 4; i++) c[n++] = lt_next[i];
    n_next = 0;
    S.nl = 0;
    bool used[MAX_LIGHTS * 4] = { 0 };
    while (S.nl < MAX_LIGHTS && S.nl < n) {
        int best = -1;
        float bd = 1e30f;
        for (int i = 0; i < n; i++) {
            if (used[i]) continue;
            float d = Vector3Distance(c[i].p, S.cam) - c[i].r;
            if (d < bd) { bd = d; best = i; }
        }
        used[best] = true;
        int k = S.nl++;
        S.lights[k * 4 + 0] = c[best].p.x; S.lights[k * 4 + 1] = c[best].p.y;
        S.lights[k * 4 + 2] = c[best].p.z; S.lights[k * 4 + 3] = c[best].r;
        S.lcols[k * 3 + 0] = c[best].c.x; S.lcols[k * 3 + 1] = c[best].c.y; S.lcols[k * 3 + 2] = c[best].c.z;
    }
    S.nlamps = 0;
    if (S.night > 0.02f) {
        Vector4 lp[32];
        int nl = world_lamps(S.cam, lp, 32);
        for (int i = 0; i < nl; i++) memcpy(&S.lamps[i * 4], &lp[i], sizeof(Vector4));
        S.nlamps = nl;
    }
}

static void fullscreen(void)
{
    glBindVertexArray(vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void bind_scene_textures(void)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, world_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, shadow_tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, sig_tex);
    glActiveTexture(GL_TEXTURE0);
}

static void post(GLuint out_fbo, const GLint *vp)
{
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    stamp++;
    // bloom pyramid
    for (int i = 0; i < NB; i++) {
        Prog *p = i == 0 ? &pr_bright : &pr_down;
        use(p);
        glBindFramebuffer(GL_FRAMEBUFFER, T.b_fbo[i]);
        glViewport(0, 0, T.bw[i], T.bh[i]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, i == 0 ? T.res_tex : T.b_tex[i - 1]);
        glUniform1i(p->u[U_SRC], 0);
        glUniform2f(p->u[U_TEXEL], 1.0f / (i == 0 ? T.w : T.bw[i - 1]), 1.0f / (i == 0 ? T.h : T.bh[i - 1]));
        glUniform1f(p->u[U_EXPO], S.expo);
        fullscreen();
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    use(&pr_up);
    for (int i = NB - 2; i >= 0; i--) {
        glBindFramebuffer(GL_FRAMEBUFFER, T.b_fbo[i]);
        glViewport(0, 0, T.bw[i], T.bh[i]);
        glBindTexture(GL_TEXTURE_2D, T.b_tex[i + 1]);
        glUniform1i(pr_up.u[U_SRC], 0);
        glUniform2f(pr_up.u[U_TEXEL], 1.0f / T.bw[i + 1], 1.0f / T.bh[i + 1]);
        fullscreen();
    }
    glDisable(GL_BLEND);

    glBindFramebuffer(GL_FRAMEBUFFER, T.ldr_fbo);
    glViewport(0, 0, T.w, T.h);
    use(&pr_compose);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, T.res_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, T.b_tex[0]);
    glUniform1i(pr_compose.u[U_SRC], 0);
    glUniform1i(pr_compose.u[U_SRC2], 1);
    glUniform1f(pr_compose.u[U_EXPO], S.expo);
    glUniform1f(pr_compose.u[U_BLOOM], S.bloom);
    glUniform4fv(pr_compose.u[U_GRADE], 1, S.grade);
    fullscreen();

    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    use(&pr_fxaa);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, T.ldr_tex);
    glUniform1i(pr_fxaa.u[U_SRC], 0);
    glUniform2f(pr_fxaa.u[U_TEXEL], 1.0f / T.w, 1.0f / T.h);
    fullscreen();
}

void render_frame(int w, int h)
{
    static double bench_t, bench_sum;
    static int bench_n;
    double t_start = GetTime();
    rlDrawRenderBatchActive();
    GLint fbo, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    int ow = w, oh = h;
    w = (int)(ow * render_scale); h = (int)(oh * render_scale);
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    if (w != T.w || h != T.h) make_targets(w, h);

    // camera, with shake
    float sh = fx_shake_amount();
    float tt = (float)game_s;
    Vector3 eye = cam.position, tgt = cam.target;
    if (sh > 0.001f) {
        eye = Vector3Add(eye, (Vector3){ sinf(tt * 71.0f), sinf(tt * 83.0f + 1), sinf(tt * 59.0f + 2) });
        eye = Vector3Add(cam.position, Vector3Scale(Vector3Subtract(eye, cam.position), sh * 0.03f));
        tgt = Vector3Add(tgt, Vector3Scale((Vector3){ sinf(tt * 67.0f + 3), sinf(tt * 77.0f + 4), sinf(tt * 61.0f + 5) }, sh * 0.12f));
    }
    r_view = MatrixLookAt(eye, tgt, cam.up);
    r_proj = MatrixPerspective(cam.fovy * DEG2RAD, (float)ow / oh, 0.25f, 2400.0f);
    r_vp = MatrixMultiply(r_view, r_proj);
    Vector3 fwd = Vector3Normalize(Vector3Subtract(tgt, eye));
    r_cam_right = Vector3Normalize(Vector3CrossProduct(fwd, cam.up));
    r_cam_up = Vector3CrossProduct(r_cam_right, fwd);

    sun_dir = sun_from_hours(day_hours);
    S.vp = r_vp;
    S.invvp = MatrixInvert(r_vp);
    S.cam = eye;
    S.time = tt;
    scene_colors(sun_dir.y);
    Vector3 center = Vector3Add(eye, Vector3Scale(fwd, 55.0f));
    center.y = Clamp(center.y - fwd.y * 0, 0, 40);
    shadow_matrix(center);
    collect_lights();

    stamp++;
    bind_scene_textures();
    world_frame();

    // shadow pass
    bool shadows = S.lcol.x + S.lcol.y + S.lcol.z > 0.02f;
    if (shadows) {
        cur_pass = PASS_SHADOW;
        glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
        glViewport(0, 0, SH_SIZE, SH_SIZE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.0f, 4.0f);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        use(&pr_shw);
        world_cull(S.lvp, center, SH_HALF * 1.6f);
        world_draw_part(GEO_SOLID);
        cars_draw(PASS_SHADOW);
        peds_draw(PASS_SHADOW);
        raiders_draw(PASS_SHADOW);
        survive_draw(PASS_SHADOW);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_POLYGON_OFFSET_FILL);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
        glClear(GL_DEPTH_BUFFER_BIT);
    }

    // main pass
    cur_pass = PASS_MAIN;
    stamp++;
    glBindFramebuffer(GL_FRAMEBUFFER, T.msaa_fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    use(&pr_sky);
    fullscreen();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    world_cull(r_vp, eye, 1e9f);
    glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    use(&pr_world);
    world_draw_part(GEO_GROUND);
    world_draw_part(GEO_SOLID);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    cars_draw(PASS_MAIN);
    peds_draw(PASS_MAIN);
    pickups_draw(PASS_MAIN);
    raiders_draw(PASS_MAIN);
    survive_draw(PASS_MAIN);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    use(&pr_water);
    world_draw_part(GEO_WATER);
    use(&pr_glass);
    world_draw_part(GEO_GLASS);
    fx_draw(PASS_MAIN);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    // resolve and post
    glBindFramebuffer(GL_READ_FRAMEBUFFER, T.msaa_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, T.res_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    post((GLuint)fbo, vp);

    // hand raylib back the state it expects
    glBindVertexArray(0);
    glUseProgram(0);
    cur_prog = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (0) {
        glFinish();
        bench_t = GetTime() - t_start;
        if (bench_n++ > 5) bench_sum += bench_t;
        if (bench_n == 5 + atoi(getenv("OOP_BENCH"))) fprintf(stderr, "frame time %.2f ms over %d frames\n", bench_sum / (bench_n - 6) * 1000, bench_n - 6);
    }
}

// ---------------------------------------------------------------- meshes from other modules

static void draw_vao(Mesh m)
{
    glBindVertexArray(m.vaoId);
    if (m.indices) glDrawElements(GL_TRIANGLES, m.triangleCount * 3, GL_UNSIGNED_SHORT, 0);
    else glDrawArrays(GL_TRIANGLES, 0, m.vertexCount);
    glBindVertexArray(0);
}

static void set_model(Prog *p, Matrix xf)
{
    gl_mat(p->u[U_MODEL], xf);
}

void draw_mesh(Mesh m, Matrix xf, Color tint, int mat)
{
    if (!m.vaoId) return;
    Prog *p = cur_pass == PASS_SHADOW ? &pr_shmesh : mat == MAT_CAR ? &pr_car : &pr_prop;
    use(p);
    set_model(p, xf);
    if (cur_pass == PASS_MAIN) glUniform4f(p->u[U_TINT], tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f, tint.a / 255.0f);
    draw_vao(m);
}

void draw_char(Mesh m, Matrix xf, const Matrix *bones, const Color *pal)
{
    if (!m.vaoId) return;
    Prog *p = cur_pass == PASS_SHADOW ? &pr_shskin : &pr_char;
    use(p);
    set_model(p, xf);
    float b[MAX_BONES * 16];
    int n = skel.count > 0 && skel.count <= MAX_BONES ? skel.count : MAX_BONES;
    for (int i = 0; i < MAX_BONES; i++) memcpy(b + i * 16, MatrixToFloatV(i < n ? bones[i] : MatrixIdentity()).v, 64);
    glUniformMatrix4fv(p->u[U_BONES], MAX_BONES, GL_FALSE, b);
    if (cur_pass == PASS_MAIN) {
        float c[18];
        for (int i = 0; i < 6; i++) {
            c[i * 3 + 0] = powf(pal[i].r / 255.0f, 2.2f);
            c[i * 3 + 1] = powf(pal[i].g / 255.0f, 2.2f);
            c[i * 3 + 2] = powf(pal[i].b / 255.0f, 2.2f);
        }
        glUniform3fv(p->u[U_PAL], 6, c);
    }
    draw_vao(m);
}
