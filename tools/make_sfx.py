# builds assets/sfx/*.ogg from the downloaded CC0 recordings in raw/sfx (see assets/SOUND_CREDITS.md).
# mono, 32 kHz, high-passed, normalised, tiny vorbis. a few layers are synthesized (low thump, tails, ricochet).
import os, subprocess, sys
import numpy as np

SR = 32000
RAW = os.path.join(os.path.dirname(__file__), "..", "raw", "sfx")
OUT = os.path.join(os.path.dirname(__file__), "..", "assets", "sfx")
K = RAW + "/k_impact-sounds/Audio/"
KR = RAW + "/k_rpg-audio/Audio/"
KI = RAW + "/k_interface-sounds/Audio/"
O = RAW + "/oga/"
R = O + "x_100-CC0-SFX_0/"
B = O + "x_bfh/bfh1_"
rng = np.random.default_rng(7)


def load(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).copy()


def lay(path, start=None, dur=None, gain=1.0, pitch=1.0):
    d = load(path)
    d = d / (np.abs(d).max() + 1e-9)
    if start is None:  # 8 ms before the first loud sample
        start = max(0, np.argmax(np.abs(d) > 0.15) / SR - 0.008)
    d = d[int(start * SR):]
    if dur: d = d[:int(dur * SR)]
    if pitch != 1.0:
        idx = np.arange(0, len(d) - 1, pitch)
        d = np.interp(idx, np.arange(len(d)), d)
    return d * gain


def onepole(x, f):
    a = 1 - np.exp(-2 * np.pi * f / SR)
    y = np.empty_like(x); s = 0.0
    for i, v in enumerate(x):
        s += a * (v - s); y[i] = s
    return y


def thump(f0, f1, dur, amp, decay=6.0):
    t = np.arange(int(dur * SR)) / SR
    ph = 2 * np.pi * (f1 * t + (f0 - f1) * (1 - np.exp(-t * 30)) / 30)
    return np.sin(ph) * np.exp(-t * decay / dur) * amp


def rumble(dur, f, amp, decay=5.0):
    t = np.arange(int(dur * SR)) / SR
    return onepole(rng.standard_normal(len(t)).astype(np.float32), f) * np.exp(-t * decay / dur) * amp * 6


def mix(*parts):  # parts: (array, at_seconds)
    n = max(int(at * SR) + len(a) for a, at in parts)
    m = np.zeros(n, np.float32)
    for a, at in parts:
        i = int(at * SR); m[i:i + len(a)] += a
    return m


def save(name, x, peak=0.9, hp=40):
    x = x / (np.abs(x).max() + 1e-9) * peak
    n = min(int(0.02 * SR), len(x))
    x[-n:] *= np.linspace(1, 0, n)
    p = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", str(SR), "-ac", "1", "-i", "-",
                        "-af", f"highpass=f={hp}", "-c:a", "libvorbis", "-q:a", "4",
                        os.path.join(OUT, name + ".ogg")], input=x.astype(np.float32).tobytes())
    if p.returncode: sys.exit("ffmpeg failed on " + name)


def ricochet(seed):
    r = np.random.default_rng(seed)
    t = np.arange(int(0.45 * SR)) / SR
    f = r.uniform(2400, 3600) * np.exp(-t * r.uniform(2.5, 4)) + 700
    whine = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 7)
    zing = np.sin(2 * np.pi * np.cumsum(f * 1.5) / SR) * np.exp(-t * 11) * 0.4
    n = r.standard_normal(len(t)); n = n - onepole(n, 2500)
    return whine + zing + n * np.exp(-t * 60) * 0.5


os.makedirs(OUT, exist_ok=True)

GUN = O + "x_sounds/sounds/"
shot = lambda s: lay(GUN + "cz.wav", s, 0.9)
for tag, s in (("pistol", 2.75), ("pistol_2", 5.45)):
    save(tag, mix((shot(s), 0), (thump(130, 45, 0.3, 0.55), 0), (lay(R + "shot_01.ogg", 0, 0.25, 0.35), 0)))

save("smg", mix((lay(O + "unknown.wav", 0.2, 0.4, 1.0, 1.15), 0), (thump(110, 50, 0.15, 0.4), 0)))
save("shotgun", mix((lay(GUN + "shotty.wav", 0, 0.7), 0), (thump(100, 32, 0.5, 0.9), 0),
                    (rumble(0.45, 1800, 0.5, 6), 0), (lay(O + "blackpowder.wav", 0.05, 0.4, 0.4, 0.7), 0)))
save("empty", lay(KR + "metalClick.ogg", None, 0.2, 1.0, 1.3))
save("reload", lay(O + "x_gun_reload_-_starninjas/gun_reload.1.ogg", None, 0.9))

for i, f in enumerate(("impactPunch_heavy_001", "impactPunch_heavy_003")):
    save("punch" if i == 0 else "punch_2", mix((lay(K + f + ".ogg", None, 0.3), 0),
                                              (lay(K + "impactSoft_medium_00%d.ogg" % i, None, 0.25, 0.6), 0)))
for i, (a, b) in enumerate((("impactPunch_medium_002", "hit_04"), ("impactPunch_medium_000", "hit_02"))):
    save("hit" if i == 0 else "hit_2", mix((lay(K + a + ".ogg", None, 0.25, 1.0, 0.9), 0),
                                           (lay(B + b + ".ogg", None, 0.25, 0.5), 0)))

save("crash_small", mix((lay(B + "metal_hit_02.ogg", None, 0.5), 0), (lay(K + "impactMetal_medium_001.ogg", None, 0.4, 0.7), 0.02),
                        (lay(B + "glass_hit_01.ogg", None, 0.3, 0.3), 0.05), (thump(100, 40, 0.25, 0.5), 0)))
save("crash_small_2", mix((lay(B + "metal_hit_04.ogg", None, 0.5), 0), (lay(K + "impactPlate_medium_002.ogg", None, 0.4, 0.7), 0.01),
                          (thump(90, 38, 0.3, 0.5), 0)))
save("crash_big", mix((lay(K + "impactPlate_heavy_002.ogg", None, 0.8), 0), (lay(K + "impactMetal_heavy_000.ogg", None, 0.8, 0.8, 0.85), 0.04),
                      (lay(B + "metal_falling_02.ogg", None, 0.7, 0.6), 0.12), (lay(O + "glass_breaking.wav", None, 1.0, 0.5), 0.1),
                      (thump(80, 30, 0.7, 0.8), 0), (rumble(0.8, 700, 0.35, 5), 0)))
save("glass", lay(O + "glass_breaking.wav", None, 1.2))

save("explosion", mix((lay(O + "chunky.mp3", None, 2.4), 0), (lay(O + "explosion.wav", None, 1.0, 0.7, 0.8), 0),
                      (lay(R + "explosion.ogg", None, 0.6, 0.5), 0), (thump(70, 24, 1.4, 1.2, 4), 0),
                      (rumble(2.4, 250, 0.9, 5), 0.03)))
save("ricochet", ricochet(1)); save("ricochet_2", ricochet(2))

for i in range(4):
    save("step" if i == 0 else "step_%d" % (i + 1), lay(K + "footstep_concrete_00%d.ogg" % i, None, 0.14), hp=70)
save("door", mix((lay(KR + "doorClose_2.ogg", None, 0.5), 0), (lay(K + "impactMetal_light_001.ogg", None, 0.3, 0.6), 0),
                 (thump(120, 55, 0.12, 0.5), 0)))
save("scream", lay(O + "female_scream_1.ogg", None, 1.2), hp=120)
save("scream_2", lay(O + "screams.ogg", 0.05, 1.3), hp=120)
save("scream_3", lay(O + "screams.ogg", 3.4, 1.2, 1.0, 0.85), hp=100)
save("pickup", lay(KI + "confirmation_002.ogg", None, 0.4, 1.0, 1.1), hp=200)
save("cash", mix((lay(KR + "handleCoins2.ogg", None, 0.4), 0), (lay(R + "bell_01.ogg", None, 0.6, 0.35, 2.0), 0.12)), hp=150)
save("bodyfall", mix((lay(K + "impactSoft_heavy_001.ogg", None, 0.35), 0), (lay(K + "impactSoft_heavy_003.ogg", None, 0.3, 0.6, 0.8), 0.09),
                     (thump(75, 40, 0.2, 0.5), 0)))
save("ui", lay(KI + "click_003.ogg", None, 0.12), hp=300)
print("ok")
