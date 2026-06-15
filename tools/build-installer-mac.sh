#!/usr/bin/env bash
#
# Build the macOS app bundle (universal: arm64 + x86_64) and package it into a
# drag-to-Applications .dmg disk image.
#
# Usage:   tools/build-installer-mac.sh [version]      (version defaults to 0.1.0)
# Output:  dist/EARS-Bridge-<version>-macOS.dmg
#
# Optional code signing: set CODESIGN_IDENTITY to a Developer ID Application identity
# (e.g. "Developer ID Application: Your Name (TEAMID)") to sign the bundle before
# packaging. Without it the app is only ad-hoc signed and users must clear Gatekeeper
# quarantine on first launch (see the README).
set -euo pipefail

VERSION="${1:-0.1.0}"
APP_NAME="EARS Bridge"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# A dedicated build dir, kept separate from the Windows Ninja 'build/' (which can appear
# in this tree via OneDrive sync and would clash with the Xcode generator).
BUILD="$ROOT/build-macos"
DIST="$ROOT/dist"

echo "==> Configuring (Xcode, universal arm64+x86_64)"
cmake -G Xcode -B "$BUILD" \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0

echo "==> Building $APP_NAME (Release)"
cmake --build "$BUILD" --config Release --target EarsBridge

APP="$BUILD/EarsBridge_artefacts/Release/$APP_NAME.app"
[ -d "$APP" ] || { echo "ERROR: app bundle not found at $APP"; exit 1; }

if [ -n "${CODESIGN_IDENTITY:-}" ]; then
  echo "==> Code signing with: $CODESIGN_IDENTITY"
  codesign --force --deep --options runtime --timestamp --sign "$CODESIGN_IDENTITY" "$APP"
  codesign --verify --deep --strict --verbose=2 "$APP"
fi

mkdir -p "$DIST"
DMG="$DIST/EARS-Bridge-$VERSION-macOS.dmg"
rm -f "$DMG"
BG="$ROOT/installer/assets/dmg-background.png"

echo "==> Creating $DMG"
if command -v create-dmg >/dev/null 2>&1; then
  # Styled image: branded background + the app and an Applications drop-link positioned
  # to match the arrow in dmg-background.png (brew install create-dmg).
  create-dmg \
    --volname "$APP_NAME" \
    --background "$BG" \
    --window-pos 200 120 \
    --window-size 660 420 \
    --icon-size 110 \
    --icon "$APP_NAME.app" 180 220 \
    --app-drop-link 480 220 \
    --hide-extension "$APP_NAME.app" \
    --no-internet-enable \
    "$DMG" "$APP"
else
  echo "create-dmg not found (brew install create-dmg) — falling back to a plain hdiutil image"
  STAGE="$(mktemp -d)"
  ditto "$APP" "$STAGE/$APP_NAME.app"        # ditto preserves the bundle correctly
  ln -s /Applications "$STAGE/Applications"  # the drag-to-install target
  hdiutil create -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
  rm -rf "$STAGE"
fi

echo ""
echo "============================================================"
echo " Disk image written to: $DMG"
echo "============================================================"
