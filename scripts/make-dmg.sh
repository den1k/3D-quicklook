#!/bin/bash
# Builds the app and packs it into build/QuickLook3D-<version>.dmg for a GitHub release.
# Needs the static OpenCASCADE from scripts/build-occt.sh so the app runs without Homebrew.
set -euo pipefail
cd "$(dirname "$0")/.."

[ -d third_party/occt/lib ] || { echo "error: run scripts/build-occt.sh first (the release must include STEP support)" >&2; exit 1; }
env -u QL3D_OCCT ./build.sh

VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" App/Info.plist)
DMG="build/QuickLook3D-$VERSION.dmg"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -R build/QuickLook3D.app "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "3D Quick Look" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
echo "built $DMG"
