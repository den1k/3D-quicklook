#!/bin/bash
# Copies a built QuickLook3D.app to ~/Applications and registers its Quick Look extension.
#   scripts/install.sh [path/to/QuickLook3D.app]     (default: build/QuickLook3D.app)
set -euo pipefail
APP=${1:-"$(dirname "$0")/../build/QuickLook3D.app"}
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
