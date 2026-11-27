# builds assets/sfx/v_*.ogg from the recorded lines in raw/voice (16 kHz mono).
# every line: high-pass 100 Hz, light afftdn, trimmed, same rms, peak under -1 dBFS.
# dispatch lines get a radio filter and squelch. the lines strangers say get a woman copy (+4 st)
# through rubberband (all but three lines, see NO_WOMAN); pack_sfx.py packs them as variants. the other-man voice (-2 st) is a runtime
# pitch in audio.c: a second copy of everything would not fit the size budget.
import os, subprocess
import numpy as np

SR = 16000
HERE = os.path.dirname(os.path.abspath(__file__))
RAW = HERE + "/../raw/voice/"
OUT = HERE + "/../assets/sfx/"
for f in os.listdir(OUT):
    if f.startswith("v_"): os.remove(OUT + f)
RMS_DB, PEAK_DB = -20, -1
rng = np.random.default_rng(11)

# name -> recordings; (file, from, to) cuts a piece out of the long take
PEOPLE = "people.wav"
LINES = {
    "freeze": ["policefreeze"], "ground": ["getontheground"], "drop": ["droptheweapon"],
    "pullover": ["pullovernow"], "hands": ["handswhereicanseeem"], "shots": ["shotsfired"],
    "onfoot": ["onfoot"], "arrest": ["underarrest"],
    "north": ["headingnorth"], "pursuit": ["inpursuit"], "lost": ["lostvisual"],
    "takeit": ["takeit"], "dontshoot": ["dontshoot"], "outta": ["outtamystore"],
    "phone": ["intehcorner"], "notoday": ["ooyeanottoday"],
}
RADIO = {"north", "pursuit", "lost"}
STRANGERS = {  # recordings, each gets a woman copy (not the grunts)
    "scream": ["scream1", "scream2", "scream3"], "grunt": ["grunt1", "grunt2long", "grunt3"],
    "mycar": [(PEOPLE, 0, 1.68)], "help": [(PEOPLE, 1.68, 3.37)], "cops": [(PEOPLE, 3.37, 5.4)],
    "what": [(PEOPLE, 5.4, 7.36)], "watch": [(PEOPLE, 7.36, 9.09)], "gun": [(PEOPLE, 9.09, 12)],
}

NO_WOMAN = {"grunt", "cops", "gun"}   # these play +4 st at runtime for women, to stay in the size budget


def ff(src, af, extra=()):
    cmd = ["ffmpeg", "-v", "error", *extra, "-i", src, "-af", af, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"]
    return np.frombuffer(subprocess.run(cmd, capture_output=True, check=True).stdout, dtype=np.float32).copy()


def load(rec, radio):
    af = "highpass=f=100,afftdn=nr=8:nf=-45"
    if radio: af = "highpass=f=300,lowpass=f=3400," + af
    if isinstance(rec, tuple):
        return ff(RAW + rec[0], af, ["-ss", str(rec[1]), "-t", str(rec[2] - rec[1])])
    return ff(RAW + rec + ".mp3", af)


def trim(x):
    hop = SR // 100
    env = np.sqrt(np.convolve(x * x, np.ones(hop) / hop, "same"))
    loud = np.nonzero(env > env.max() * 0.012)[0]   # about -38 dB under the peak
    a, b = max(0, loud[0] - SR // 25), min(len(x), loud[-1] + SR // 10)
    x = x[a:b].copy()
    fade = SR // 50
    x[:fade] *= np.linspace(0, 1, fade)
    x[-fade:] *= np.linspace(1, 0, fade)
    return x


def pitched(x, st):
    pcm = np.concatenate([x, np.zeros(SR // 10, np.float32)]).tobytes()   # tail room for the stretcher
    cmd = ["ffmpeg", "-v", "error", "-f", "f32le", "-ar", str(SR), "-ac", "1", "-i", "-",
           "-af", f"rubberband=pitch={2 ** (st / 12):.5f}:formant=shifted:pitchq=quality", "-f", "f32le", "-"]
    y = np.frombuffer(subprocess.run(cmd, input=pcm, capture_output=True, check=True).stdout, dtype=np.float32)
    return trim(y.copy())


def level(x):
    live = x[np.abs(x) > np.abs(x).max() * 0.05]
    g = 10 ** (RMS_DB / 20) / np.sqrt(np.mean(live ** 2))
    g = min(g, 10 ** (PEAK_DB / 20) / np.abs(x).max())
    return x * g


def hiss(n, decay):
    t = np.arange(n) / SR
    noise = np.convolve(rng.standard_normal(n), np.ones(3) / 3, "same")   # roughly band-limited
    return (noise * np.exp(-t * decay) * 0.3).astype(np.float32)


def radio(x):
    x = np.tanh(2.2 * x) / np.tanh(2.2)
    x = level(x)
    y = np.concatenate([hiss(SR // 14, 40)[::-1], x, hiss(SR // 10, 28)])   # squelch opens, then closes
    crack = (rng.random(len(y)) > 0.9985) * rng.standard_normal(len(y)) * 0.12
    return y + crack.astype(np.float32)


def write(name, x):
    x = x * min(1, 10 ** (PEAK_DB / 20) / np.abs(x).max())
    pcm = (np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes()
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", str(SR), "-ac", "1", "-i", "-",
                    "-c:a", "libvorbis", "-q:a", "4", OUT + name + ".ogg"], input=pcm, check=True)
    print(f"{name:14s} {len(x) / SR:5.2f} s")


def put(name, clips):   # name.ogg, name_2.ogg ...
    for i, c in enumerate(clips):
        write(name if i == 0 else f"{name}_{i + 1}", c)


for k, recs in LINES.items():
    put("v_" + k, [radio(trim(load(r, True))) if k in RADIO else level(trim(load(r, False))) for r in recs])
for k, recs in STRANGERS.items():
    base = [trim(load(r, False)) for r in recs]
    put("v_" + k, [level(x) for x in base + ([] if k in NO_WOMAN else [pitched(x, 4) for x in base])])
