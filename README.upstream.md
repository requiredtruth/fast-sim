# Out on Parole

Small open-world crime game in C and raylib. 

## build

Needs clang, python3 + numpy, raylib source in `raw/raylib` and the model/sound packs below in `raw/`.

    make         # dev build (mac)
    make dist    # release builds -> dist/

## controls

WASD move / drive, mouse aim, LMB shoot, F get in/out, shift sprint, space jump / handbrake,
R reload, Q/E weapons, C crouch, H horn, M map, V camera, tab radio, esc pause. Gamepad works too.

## credits

- Characters, animations, guns: Quaternius (CC0)
- Cars: [Generic passenger car pack](https://sketchfab.com/3d-models/20f9af9b8a404d5cb022ac6fe87f21f5) by comrade1280, CC BY 4.0, simplified and repainted
- Police helicopter: kazuma, Poly Pizza (CC0)
- Sounds: Kenney and OpenGameArt (Tabasco / Vincent Sevedge, kurt, rubberduck, StarNinjas, Joth, TinyWorlds, Till Behrend, Nocturnal_Vanguard, WuxiaScrub), CC0. Engines, sirens and ambience are synthesized in `src/audio.c`.
