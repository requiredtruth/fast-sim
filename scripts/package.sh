#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sdk="${ANDROID_SDK_ROOT:?Set ANDROID_SDK_ROOT}"
tools="$sdk/build-tools/35.0.0"
platform="$sdk/platforms/android-35/android.jar"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p dist "$work/classes" "$work/dex"
cp index.html dist/fast-sim.html

for platform_name in linux windows; do
  ext=
  if [[ "$platform_name" == windows ]]; then ext=.exe; fi
  CGO_ENABLED=0 GOOS="$platform_name" GOARCH=amd64 go build -trimpath -ldflags='-s -w' -o "$work/fast-sim$ext" launcher.go
  if [[ "$platform_name" == linux ]]; then
    tar -C "$work" -czf dist/fast-sim-linux-x64.tar.gz fast-sim
  else
    (cd "$work" && zip -9 "$OLDPWD/dist/fast-sim-windows-x64.zip" fast-sim.exe)
  fi
done

if [[ -n "${ECJ_JAR:-}" ]]; then
  java -jar "$ECJ_JAR" -nowarn -source 8 -target 8 -bootclasspath "$platform" -d "$work/classes" android/MainActivity.java
else
  javac -source 8 -target 8 -bootclasspath "$platform" -d "$work/classes" android/MainActivity.java
fi
"$tools/d8" --min-api 23 --lib "$platform" --output "$work/dex" "$work/classes"/dev/fastsim/dustbound/*.class
"$tools/aapt2" compile --dir android/res -o "$work/resources.zip"
mkdir "$work/assets"
cp index.html "$work/assets/"
version="$(sed -n '1s/^v//p' RELEASE)"
if [[ ! "$version" =~ ^1\.0\.([0-9]+)$ ]]; then
  echo "Expected a v1.0.x release version." >&2
  exit 1
fi
version_code="$((10#${BASH_REMATCH[1]} + 1))"
sed -e "s/android:versionCode=\"1\"/android:versionCode=\"$version_code\"/" \
    -e "s/android:versionName=\"1.0.0\"/android:versionName=\"$version\"/" android/AndroidManifest.xml > "$work/AndroidManifest.xml"
"$tools/aapt2" link -I "$platform" --manifest "$work/AndroidManifest.xml" -A "$work/assets" -o "$work/unsigned.apk" "$work/resources.zip"
(cd "$work/dex" && zip -9 "$work/unsigned.apk" classes.dex)
"$tools/zipalign" -f 4 "$work/unsigned.apk" "$work/aligned.apk"
keystore="${FAST_SIM_KEYSTORE:-$work/fast-sim.p12}"
if [[ -z "${FAST_SIM_KEYSTORE:-}" ]]; then
  keytool -genkeypair -keystore "$keystore" -storepass android -keypass android -alias fast-sim -dname 'CN=Fast Sim distribution' -keyalg RSA -keysize 2048 -validity 10000
fi
export FAST_SIM_KEYSTORE_PASSWORD="${FAST_SIM_KEYSTORE_PASSWORD:-android}"
"$tools/apksigner" sign --ks "$keystore" --ks-key-alias fast-sim --ks-pass env:FAST_SIM_KEYSTORE_PASSWORD --out dist/fast-sim.apk "$work/aligned.apk"
"$tools/apksigner" verify dist/fast-sim.apk
(cd dist && rm -f ./*.idsig && sha256sum fast-sim* > SHA256SUMS.txt)
total="$(find dist -type f -printf '%s\n' | awk '{total+=$1} END {print total+0}')"
if (( total > 100000000 )); then
  echo "Release exceeds the 100 MB limit ($total bytes)." >&2
  exit 1
fi
