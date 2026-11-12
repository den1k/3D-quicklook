#!/bin/bash
# Builds QuickLook3D.app (host app + Quick Look preview extension) into build/.
#   ./build.sh            build
#   ./build.sh install    build, copy to ~/Applications and register with macOS
set -euo pipefail
cd "$(dirname "$0")"

TARGET=arm64-apple-macos13.0
OCCT=${QL3D_OCCT:-third_party/occt}  # optional: OpenCASCADE install prefix, enables STEP
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
    if ls "$OCCT"/lib/libTK*.a >/dev/null 2>&1; then
        LIBS+=("$OCCT"/lib/libTK*.a)
    else  # shared libraries (e.g. Homebrew's opencascade): link by name, keep only the ones used
        LIBS+=("-L$OCCT/lib" -Xlinker -dead_strip_dylibs)
        for lib in "$OCCT"/lib/libTK*.dylib; do
            name=$(basename "$lib" .dylib)
            case "$name" in *.*) ;; *) LIBS+=("-l${name#lib}") ;; esac
        done
    fi
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
    scripts/install.sh "$APP"
fi
