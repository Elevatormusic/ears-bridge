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

echo "==> Staging disk image contents"
STAGE="$(mktemp -d)"
ditto "$APP" "$STAGE/$APP_NAME.app"        # ditto preserves the bundle correctly
ln -s /Applications "$STAGE/Applications"  # the drag-to-install target

mkdir -p "$DIST"
DMG="$DIST/EARS-Bridge-$VERSION-macOS.dmg"
rm -f "$DMG"

echo "==> Creating $DMG"
hdiutil create -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
rm -rf "$STAGE"

echo ""
echo "============================================================"
echo " Disk image written to: $DMG"
echo "============================================================"
