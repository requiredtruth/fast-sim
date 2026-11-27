#!/usr/bin/env bash
# Builds Long Road for one or more targets: ./build.sh linux windows web android
# Needs: raylib 5.5 source at $RAYLIB (default deps/raylib), ffmpeg + python3 for the sound bank,
# mingw-w64 for windows, emsdk (emcc on PATH) for web, Android SDK + NDK for android.
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$PWD"
RAYLIB="${RAYLIB:-$ROOT/deps/raylib}"
VERSION="$(sed -n '1s/^v//p' RELEASE 2>/dev/null || echo 0.0.0)"
OUT="$ROOT/dist"
mkdir -p build "$OUT"

[[ -d "$RAYLIB/src" ]] || { echo "raylib 5.5 source not found at $RAYLIB (git clone --depth 1 -b 5.5 https://github.com/raysan5/raylib deps/raylib)"; exit 1; }

# generated sources: the sound bank
if [[ ! -f build/sfx.c || tools/pack_sfx.py -nt build/sfx.c || src/game.h -nt build/sfx.c ]]; then
    python3 tools/pack_sfx.py > /dev/null
fi

SRC=(src/*.c build/sfx.c)
FEATURES="-DSUPPORT_FILEFORMAT_OBJ=0 -DSUPPORT_FILEFORMAT_MTL=0 -DSUPPORT_FILEFORMAT_IQM=0 -DSUPPORT_FILEFORMAT_GLTF=0
 -DSUPPORT_FILEFORMAT_VOX=0 -DSUPPORT_FILEFORMAT_M3D=0 -DSUPPORT_TRACELOG=1 -DSUPPORT_CAMERA_SYSTEM=0
 -DSUPPORT_GESTURES_SYSTEM=1 -DSUPPORT_SCREEN_CAPTURE=0 -DSUPPORT_AUTOMATION_EVENTS=0"
GAMEFLAGS=(-std=gnu11 -O2 -DNDEBUG -w -Isrc -I"$RAYLIB/src" -DGAME_VERSION="\"$VERSION\"")

# raylib, one static library per target
raylib_lib() { # target cc ar platform graphics extra...
    local t=$1 cc=$2 ar=$3 plat=$4 gfx=$5; shift 5
    local dir="build/rl-$t"
    [[ -f "$dir/libraylib.a" ]] && return
    mkdir -p "$dir"
    local mods=(rcore rshapes rtextures rtext rmodels utils)
    [[ "$plat" == PLATFORM_DESKTOP_GLFW ]] && mods+=(rglfw)
    for m in "${mods[@]}"; do
        $cc -std=gnu11 -O2 -w -DNDEBUG -D$plat -D$gfx $FEATURES "$@" -I"$RAYLIB/src" -I"$RAYLIB/src/external/glfw/include" \
            -c "$RAYLIB/src/$m.c" -o "$dir/$m.o" &
    done
    wait
    $ar rcs "$dir/libraylib.a" "$dir"/*.o
}

build_linux() {
    raylib_lib linux gcc ar PLATFORM_DESKTOP_GLFW GRAPHICS_API_OPENGL_33 -D_GLFW_X11
    gcc "${GAMEFLAGS[@]}" -DPLATFORM_DESKTOP "${SRC[@]}" build/rl-linux/libraylib.a -o build/longroad \
        -lm -lpthread -ldl -lrt -lX11 -s
    mkdir -p build/pkg-linux/LongRoad && cp build/longroad build/pkg-linux/LongRoad/longroad
    cp -r mods build/pkg-linux/LongRoad/ 2>/dev/null || true
    tar -C build/pkg-linux -czf "$OUT/LongRoad-linux-x64.tar.gz" LongRoad
    echo "linux: $(stat -c %s build/longroad) bytes"
}

build_windows() {
    raylib_lib win x86_64-w64-mingw32-gcc x86_64-w64-mingw32-ar PLATFORM_DESKTOP_GLFW GRAPHICS_API_OPENGL_33
    x86_64-w64-mingw32-windres android/icon.rc -O coff -o build/icon.o 2>/dev/null || : > build/icon.o
    x86_64-w64-mingw32-gcc "${GAMEFLAGS[@]}" -DPLATFORM_DESKTOP "${SRC[@]}" $( [[ -s build/icon.o ]] && echo build/icon.o ) \
        build/rl-win/libraylib.a -o build/LongRoad.exe -static -mwindows -s -lopengl32 -lgdi32 -lwinmm -lshell32
    rm -rf build/pkg-win && mkdir -p build/pkg-win/LongRoad && cp build/LongRoad.exe build/pkg-win/LongRoad/
    cp -r mods build/pkg-win/LongRoad/ 2>/dev/null || true
    (cd build/pkg-win && rm -f "$OUT/LongRoad-windows-x64.zip" && zip -qr9 "$OUT/LongRoad-windows-x64.zip" LongRoad)
    echo "windows: $(stat -c %s build/LongRoad.exe) bytes"
}

build_web() {
    command -v emcc >/dev/null || { echo "emcc not on PATH (source emsdk_env.sh)"; exit 1; }
    raylib_lib web emcc emar PLATFORM_WEB GRAPHICS_API_OPENGL_ES3
    mkdir -p build/web
    emcc "${GAMEFLAGS[@]}" -Os -DPLATFORM_WEB -DGRAPHICS_API_OPENGL_ES3 "${SRC[@]}" build/rl-web/libraylib.a -o build/web/index.html \
        -sUSE_GLFW=3 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1 -sALLOW_MEMORY_GROWTH=1 \
        -sINITIAL_MEMORY=128MB -sSTACK_SIZE=1MB -sEXPORTED_FUNCTIONS=_main,_web_audio_pull,_web_tilt,_web_import_mod,_malloc,_free \
        -sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAPU8,ccall -sSINGLE_FILE=1 -sENVIRONMENT=web --shell-file web/shell.html
    cp build/web/index.html "$OUT/LongRoad.html"
    echo "web: $(stat -c %s "$OUT/LongRoad.html") bytes"
}

build_android() {
    local sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
    local ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-}}"
    [[ -d "$sdk" && -d "$ndk" ]] || { echo "set ANDROID_SDK_ROOT and ANDROID_NDK_HOME"; exit 1; }
    local api=26 bt
    bt="$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)"
    local plat
    plat="$(ls -d "$sdk"/platforms/android-* | sort -V | tail -1)/android.jar"
    local tc="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
    local work=build/apk
    rm -rf "$work" && mkdir -p "$work/lib"
    for abi in arm64-v8a armeabi-v7a; do
        local triple cflags=""
        case $abi in
            arm64-v8a) triple=aarch64-linux-android$api ;;
            armeabi-v7a) triple=armv7a-linux-androideabi$api; cflags="-mfpu=neon -mfloat-abi=softfp" ;;
        esac
        local cc="$tc/$triple-clang $cflags"
        local glue="$ndk/sources/android/native_app_glue"
        raylib_lib "android-$abi" "$cc" "$tc/llvm-ar" PLATFORM_ANDROID GRAPHICS_API_OPENGL_ES3 -I"$glue" -fPIC
        mkdir -p "$work/lib/$abi" "build/glue-$abi"
        $cc -O2 -w -fPIC -c "$glue/android_native_app_glue.c" -o "build/glue-$abi/glue.o"
        $cc "${GAMEFLAGS[@]}" -fPIC -DPLATFORM_ANDROID -DGRAPHICS_API_OPENGL_ES3 -I"$glue" "${SRC[@]}" "build/glue-$abi/glue.o" \
            build/rl-android-$abi/libraylib.a -shared -o "$work/lib/$abi/libmain.so" \
            -u ANativeActivity_onCreate -llog -landroid -lEGL -lGLESv3 -lOpenSLES -laaudio -lm -s
    done
    local code
    IFS=. read -r a b c <<< "$VERSION"
    code=$((10#$a * 1000000 + 10#$b * 1000 + 10#$c))
    sed -e "s/@CODE@/$code/" -e "s/@NAME@/$VERSION/" android/AndroidManifest.xml > "$work/AndroidManifest.xml"
    "$bt/aapt2" compile --dir android/res -o "$work/res.zip"
    mkdir -p "$work/assets" && cp -r mods "$work/assets/" 2>/dev/null || true
    "$bt/aapt2" link -I "$plat" --manifest "$work/AndroidManifest.xml" -A "$work/assets" -o "$work/unsigned.apk" "$work/res.zip"
    (cd "$work" && zip -qr0 unsigned.apk lib)
    "$bt/zipalign" -f -p 4 "$work/unsigned.apk" "$work/aligned.apk"
    local ks="${LONGROAD_KEYSTORE:-build/debug.p12}" pass="${LONGROAD_KEYSTORE_PASSWORD:-android}"
    [[ -f "$ks" ]] || keytool -genkeypair -keystore "$ks" -storetype pkcs12 -storepass "$pass" -keypass "$pass" -alias longroad \
        -dname 'CN=Long Road' -keyalg RSA -keysize 2048 -validity 10000 >/dev/null
    LR_PASS="$pass" "$bt/apksigner" sign --ks "$ks" --ks-key-alias longroad --ks-pass env:LR_PASS --out "$OUT/LongRoad.apk" "$work/aligned.apk"
    "$bt/apksigner" verify "$OUT/LongRoad.apk"
    rm -f "$OUT"/*.idsig
    echo "android: $(stat -c %s "$OUT/LongRoad.apk") bytes"
}

targets=("$@")
[[ ${#targets[@]} -eq 0 ]] && targets=(linux)
for t in "${targets[@]}"; do "build_$t"; done
