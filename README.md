# Long Road

A road survival game. You drive one endless highway through desert, plains, forest, badlands and snowy mountains. Find fuel, food and parts at the stops along the way, keep your car running, and see how far you get.

Long Road is a fork of [Out on Parole](https://github.com/shaydex-codes/outonparole), a small C and raylib open-world game. It keeps that game's renderer, car physics, characters, ragdolls, weapons, effects and sound mixer. The city, the police and the missions are replaced by the highway and the survival game. Every car, person, building, plant, gun and animation is built from code at startup, so the game has no model or texture files.

[Download the latest release](https://github.com/requiredtruth/fast-sim/releases/latest)

| Edition | File | How to run |
| --- | --- | --- |
| Android 8+ | `LongRoad.apk` | Install it. Allow installs from your browser or file manager if Android asks. |
| Browser | `LongRoad.html` | Open it in Chrome, Edge, Firefox or Safari. It's one file and works offline. |
| Windows x64 | `LongRoad-windows-x64.zip` | Unzip, run `LongRoad.exe`. |
| Linux x64 | `LongRoad-linux-x64.tar.gz` | Extract, run `./LongRoad/longroad`. Needs X11 and OpenGL 3.3. |


## Choose a world

Open **Choose World** on the title screen. Each world keeps its own trip. Existing Long Road saves appear under **Long Road**.

| World | Terrain and stops |
| --- | --- |
| Long Road | The original endless highway through mixed biomes |
| Metro Grid | The original fork’s coastal city with its downtown, waterfront, shop interiors and connected street grid |
| Pine Country | Dense woodland, ranger stations, campsites and fishing ponds |
| Frostline | Snowy mountain roads with frequent garages and motels |
| Red Mesa | Red-rock badlands with wrecks and trading posts |

Dropping an item places its 3D model at your feet. Ground caches stay in that world's save and can be picked up again through **Use**.

## Playing

- Drive as far as you can. Stops appear every kilometre or two: gas stations, diners, motels, farms, garages, ranger stations, trading posts, campgrounds, small towns and pileups.
- Keep **food, water, rest and warmth** up. Empty bars cost health. Nights get cold, deserts make you thirsty, rain soaks you.
- Your car burns **fuel** and wears out its **engine, tyres, battery and coolant**. Hand-pump station tanks, siphon fuel from wrecks, carry jerry cans in the trunk, patch tyres, swap batteries and rebuild engines. You can strip abandoned cars for parts or take one over.
- **Search** shelves, fridges, lockers, toolboxes, desks, trunks, bags and bodies. **Trade** with traders using dollars. **Craft** bandages, armour plating, tyre patches and engine parts, and cook and boil water at a campfire.
- Light a **campfire** with two pieces of firewood and a lighter or matches, pitch a **tent**, sleep in motel beds or in the car. Sleeping in the open is a risk.
- **Hunt** deer with the rifle for meat. **Fish** in ponds. Fill bottles at wells (clean) and ponds (dirty, so boil it).
- **Raiders** hold some stops and chase you down the highway in armoured buggies. Wolves come out at night in the woods. The road gets more dangerous the further you go.
- Death ends the trip. The game saves on its own every minute, when you sleep and when you quit.

### Controls

| | Phone | Keyboard and mouse | Gamepad |
| --- | --- | --- | --- |
| Steer / walk | Tilt the phone like a wheel. Tilt forward to go, back to brake. Or use the on-screen stick and pedals | WASD or arrows | Left stick |
| Look | Drag on the right side | Mouse | Right stick |
| Gas / brake | GAS and BRAKE pedals, or tilt | W / S | Triggers |
| Use, search, trade | USE | E | B |
| Get in or out of a car | CAR / EXIT | F | Y |
| Pack | Bag button | Tab | Back |
| Fire, aim, reload | FIRE, AIM, RELOAD | Mouse buttons, R | Triggers, X |
| Switch weapon | SWAP | Q or wheel | Bumpers |
| Lights, horn, handbrake | LIGHTS, HORN, HAND | L, H, Space | |
| Trip map, camera, pause | Pause menu, CAM | M, V, Esc | Start |

Settings has tilt steering, tilt to go, tilt sensitivity, invert and **Calibrate**: hold the phone the way you like and tap it. Graphics start on High. Both tilt controls start off. Difficulty starts on Peaceful, which keeps survival mechanics active and suppresses raiders and wolves. Normal and Hard add hostile encounters.

Use **Place One in World** from the Pack to position an item on the ground, a counter or another flat surface. Aim at the surface, rotate with **Z** or the touch button, then press **Use** to place it.

**Autoplay Demo** lets the game drive, scavenge, craft, refuel and rest. Pick any world. Use **End Demo** from its pause menu to return to the title. Demo runs are separate from saved trips.

On Android, **Settings → Download APK Update** opens the latest GitHub APK. Install it from Android’s installer.

## Mods

Mods are plain text files. Each line describes one thing, so a mod can be one line long. The game's own items and recipes use exactly this format; they live in `src/data.c`, and a mod line that uses an existing id changes that item.

Where mods go:

- **Android:** `Android/data/dev.fastsim.longroad/files/mods/` on the phone's storage (reachable over USB or with a file manager)
- **Windows:** `%APPDATA%\LongRoad\mods\`, or a `mods` folder next to `LongRoad.exe`
- **Linux:** `~/.local/share/LongRoad/mods/`, or a `mods` folder next to the binary
- **Browser:** Mods → Import a mod file

Switch mods on and off in **Mods**. They load when you start or continue a trip. `example_mod.txt` is created for you as a template.

```
# lines starting with # are comments
mod name="Desert Larder" author="you"
item id=prickly_pear name="Prickly pear" kind=food eat=8 water=14 weight=0.2 stack=10 price=2 icon=leaf color=c04080 loot=bag:3,crate:2 desc="Spines removed. Mostly."
item id=canteen name="Canteen" kind=drink drink=45 weight=0.9 stack=2 price=15 empty=bottle icon=bottle color=6a7a4a loot=locker:2,trunk:1
recipe id=pear_juice out=water:1 in=prickly_pear:2
tune hunger=0.9 fuel=1.2
start item=prickly_pear:3
```

**item** fields:

| Field | Meaning |
| --- | --- |
| `id` | Unique name, letters, digits and `_`. Reusing an existing id changes that item. |
| `name`, `desc` | Shown in the pack. Quote values that contain spaces. |
| `kind` | `food`, `drink`, `med`, `fuel`, `part`, `tool`, `ammo`, `weapon`, `gear`, `mat`, `trade` |
| `weight`, `stack`, `price` | Kilograms, how many fit in one slot, trader price in dollars |
| `eat`, `drink`, `water` | Food and water restored. `water` is the water a food also gives. |
| `heal`, `bandage`, `rest`, `warm`, `cure` | Health, stop bleeding plus health, rest, warmth, less sickness |
| `sick` | Chance from 0 to 1 that it makes you ill |
| `fuel`, `engine`, `tire`, `battery`, `radiator`, `armor` | Litres or points restored on the car you stand next to |
| `weapon` | `pistol`, `rifle`, `shotgun`, `bat`: owning the item lets you draw that weapon |
| `ammo` | Which weapon this ammunition feeds |
| `light`, `tent` | Flashlight (`light=1`) or its batteries (`light=` seconds), a tent |
| `empty` | Item left over after use, for example `empty=bottle` |
| `loot` | Where it turns up and how often: `shelf`, `fridge`, `crate`, `locker`, `trunk`, `desk`, `toolbox`, `bag`, `bed`, `corpse`, each `place:weight` |
| `icon`, `color` | `can bottle box cross jerry gear wrench bullet gun bag plank coin tire battery pill flame meat leaf`, and a hex colour |

**recipe** `id`, `out=item:count`, `in=item:count,item:count`, optional `tool=item` (kept, not used up), and `fire=1` to require a campfire.

**tune** multipliers: `hunger`, `thirst`, `fuel`, `raiders`, `loot`, `cold`.

**start** adds `item=id:count` to the starting pack.

Problems in a mod show at the bottom of the Mods screen with the file and line number.

## Building

Everything builds from Linux with `build.sh`:

```sh
git clone --depth 1 -b 5.5 https://github.com/raysan5/raylib deps/raylib
bash build.sh linux                   # gcc, X11 dev headers
bash build.sh windows                 # mingw-w64
source emsdk/emsdk_env.sh && bash build.sh web          # Emscripten 3.1.x
ANDROID_SDK_ROOT=... ANDROID_NDK_HOME=... bash build.sh android
```

The sound bank is packed from `assets/sfx` with `tools/pack_sfx.py`, which needs Python 3 and ffmpeg. Outputs go to `dist/`. The GitHub workflow builds all four on every push and publishes a release from `main` when `RELEASE` changes. For APKs that update in place, set the `LONGROAD_SIGNING_KEY` (base64 PKCS#12 keystore, alias `longroad`) and `LONGROAD_KEYSTORE_PASSWORD` secrets.

`longroad --seed N --shot out.png --at SECONDS [--play drive] [--km K] [--hours H] [--weather rain|snow|fog|dust] [--ui pack|craft|car|map] [--touch]` renders a frame without playing, for tests.

### Source map

| File | What it does |
| --- | --- |
| `road.c` | The highway, terrain, biomes, stops, wrecks, collision, streaming, weather |
| `paint.c` | Every surface texture, painted from code at startup |
| `procgen.c` | Cars, people, hair, guns, props, animals and all 22 animation clips |
| `survive.c` | Stats, pack and trunk, looting, crafting, trading, sleeping, saving |
| `data.c` | Built-in items and recipes, and the mod loader |
| `raiders.c` | People at stops, raider gangs and cars, wildlife |
| `hud.c` | HUD, menus and touch controls |
| `platform.c` | Tilt, storage and screen settings per platform |
| `car.c`, `ped.c`, `ped_anim.c`, `combat.c`, `render.c`, `fx.c`, `audio.c`, `out.c`, `camera.c` | From Out on Parole, adapted |

## Credits

- Built on [Out on Parole](https://github.com/shaydex-codes/outonparole) and its renderer, physics, animation, effects and mixer code.
- Sounds from Out on Parole: Kenney and OpenGameArt (Tabasco / Vincent Sevedge, kurt, rubberduck, StarNinjas, Joth, TinyWorlds, Till Behrend, Nocturnal_Vanguard, WuxiaScrub), CC0. Engines, wind and rain are synthesized.
- [raylib](https://www.raylib.com) by Ramon Santamaria and contributors, zlib licence.


## Android release signing

The official build uses the `LONGROAD_SIGNING_KEY` and `LONGROAD_KEYSTORE_PASSWORD` repository Actions secrets. The key secret is base64-encoded PKCS#12 with alias `longroad`. Keep the same pair across releases for Android to install updates over the existing app.

To provision the v2.2.0 key from the private backup, extract it into a private directory and run:

```bash
base64 -w0 LongRoad.p12 | gh secret set LONGROAD_SIGNING_KEY --repo requiredtruth/fast-sim
gh secret set LONGROAD_KEYSTORE_PASSWORD --repo requiredtruth/fast-sim < password.txt
```

The public `.cms` asset contains only the encrypted bootstrap backup. Store the extracted key and password privately.
