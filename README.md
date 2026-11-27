# Fast Sim · Dustbound

An original offline road-survival game. Drive the backroads, stop for supplies, and make a camp before the weather turns. Canvas drawing generates the scenery and vehicles at runtime. The game ships as one self-contained HTML file without external textures, models, or libraries.

[Download the latest release](https://github.com/requiredtruth/fast-sim/releases/latest)

| Edition | Open |
| --- | --- |
| Browser | Open `fast-sim.html` in a modern browser |
| Android | Install `fast-sim.apk` on Android 6 or newer with an up-to-date Android System WebView |
| Linux x64 | Extract the archive and run `./fast-sim` with a default browser and `xdg-open` installed |
| Windows x64 | Extract the ZIP and run `fast-sim.exe` with a default browser configured |

Drive with WASD or arrows, interact with E, enter or exit with F, turn with R, and open the pack with Tab. On a touchscreen use the bottom controls. Use the Trip tab to save manually or start another route.

## Mods

Open **Pack / Mods → Mods** and import JSON. Each mod adds one consumable. Enabled mods share the roadside loot pool, and earlier mods are checked first. A mod stays enabled across new trips.

```json
{
  "version": 1,
  "item": {
    "id": "prickly-pear",
    "name": "Prickly pear",
    "weight": 0.3,
    "effect": "drink",
    "value": 18
  },
  "lootChance": 0.22
}
```

Use a unique lowercase ID with letters, digits, or hyphens. Effects are `eat`, `drink`, and `heal`. Value is the amount restored on the 100-point meter. Chance is a decimal from 0.01 to 1.

## Build

Open `index.html` directly for development. Run `bash scripts/package.sh` on Linux with Go, JDK, Android SDK platform 35 and build-tools 35.0.0, and zip installed. Point `ANDROID_SDK_ROOT` to that SDK. Outputs are written to `dist/` and checked against a combined 100 MB limit.

Set `FAST_SIM_KEYSTORE` and `FAST_SIM_KEYSTORE_PASSWORD` to retain the same Android signing key across builds. For GitHub releases, set the corresponding base64-encoded `FAST_SIM_SIGNING_KEY` and password as Actions secrets.
