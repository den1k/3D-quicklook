#!/bin/bash
# Builds QuickLook3D.app (host app + Quick Look preview extension) into build/.
#   ./build.sh            build
#   ./build.sh install    build, copy to ~/Applications and register with macOS
set -euo pipefail
cd "$(dirname "$0")"

TARGET=arm64-apple-macos13.0
OCCT=third_party/occt          # optional: OpenCASCADE install prefix, enables STEP
B=build
APP="$B/QuickLook3D.app"
APPEX="$APP/Contents/PlugIns/QL3DPreview.appex"

mkdir -p "$B/obj"

# --- C++ loading core -------------------------------------------------------
CXXFLAGS=(-std=c++17 -O2 -Wall -Wextra -target "$TARGET" -fapplication-extension)
SOURCES=(Core/loader.cpp Core/stl.cpp Core/obj.cpp Core/threemf.cpp)
LIBS=(-lc++ -lcompression)
if [ -d "$OCCT/lib" ]; then
    CXXFLAGS+=(-DQL3D_WITH_STEP "-I$(echo "$OCCT"/include/opencascade)")
    SOURCES+=(Core/step.cpp)
    LIBS+=("$OCCT"/lib/libTK*.a)
else
    echo "note: $OCCT not found, building without STEP support"
fi
OBJECTS=()
PIDS=()
for src in "${SOURCES[@]}"; do
    obj="$B/obj/$(basename "$src" .cpp).o"
    clang++ "${CXXFLAGS[@]}" -c "$src" -o "$obj" &
    PIDS+=($!)
    OBJECTS+=("$obj")
done
for pid in "${PIDS[@]}"; do wait "$pid"; done  # stop on the first failed compile
rm -f "$B/libql3d.a"
ar -rcs "$B/libql3d.a" "${OBJECTS[@]}"

SWIFTFLAGS=(-O -target "$TARGET" -import-objc-header Core/ql3d.h)

# --- test tools -------------------------------------------------------------
clang++ -std=c++17 -O2 -target "$TARGET" Tools/bench.cpp "$B/libql3d.a" "${LIBS[@]}" -Wl,-dead_strip -o "$B/ql3d-bench"
swiftc "${SWIFTFLAGS[@]}" Shared/SceneBuilder.swift Tools/render/main.swift "$B/libql3d.a" "${LIBS[@]}" \
    -Xlinker -dead_strip -o "$B/ql3d-render"

# --- app bundle -------------------------------------------------------------
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APPEX/Contents/MacOS"
cp App/Info.plist "$APP/Contents/Info.plist"
cp Extension/Info.plist "$APPEX/Contents/Info.plist"

swiftc -O -target "$TARGET" App/main.swift -o "$APP/Contents/MacOS/QuickLook3D"
swiftc "${SWIFTFLAGS[@]}" -module-name QL3DPreview -parse-as-library -application-extension \
    Shared/SceneBuilder.swift Extension/PreviewViewController.swift "$B/libql3d.a" "${LIBS[@]}" \
    -Xlinker -dead_strip -Xlinker -e -Xlinker _NSExtensionMain \
    -o "$APPEX/Contents/MacOS/QL3DPreview"

# Ad-hoc signatures: fine for this Mac; distributing to others needs a Developer ID.
codesign --force --sign - --entitlements Extension/QL3DPreview.entitlements "$APPEX"
codesign --force --sign - "$APP"
echo "built $APP"

if [ "${1:-}" = "install" ]; then
    DEST="$HOME/Applications/QuickLook3D.app"
    mkdir -p "$HOME/Applications"
    rm -rf "$DEST"
    cp -R "$APP" "$DEST"
    LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
    # Replacing the bundle makes macOS drop the old registration asynchronously, which can
    # land after ours. Register, then confirm it stuck, retrying if it did not.
    registered=""
    for _ in 1 2 3 4 5; do
        "$LSREGISTER" -f -R "$DEST"
        pluginkit -a "$DEST/Contents/PlugIns/QL3DPreview.appex"
        sleep 2
        if pluginkit -m -A -i com.local.quicklook3d.preview | grep -q quicklook3d; then registered=1; break; fi
    done
    [ -n "$registered" ] || { echo "error: macOS did not register the extension" >&2; exit 1; }
    pluginkit -e use -i com.local.quicklook3d.preview
    qlmanage -r >/dev/null 2>&1 || true
    echo "installed $DEST"
fi
