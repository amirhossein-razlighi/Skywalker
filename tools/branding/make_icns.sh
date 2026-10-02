#!/usr/bin/env bash
# Regenerates editor/Resources/AppIcon.icns from the SwiftUI icon definition.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

swift "$root/tools/branding/make_icon.swift" "$work/icon_1024.png"
set_dir="$work/AppIcon.iconset"
mkdir -p "$set_dir"
for size in 16 32 128 256 512; do
  sips -z $size $size "$work/icon_1024.png" --out "$set_dir/icon_${size}x${size}.png" >/dev/null
  double=$((size * 2))
  sips -z $double $double "$work/icon_1024.png" --out "$set_dir/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$set_dir" -o "$root/editor/Resources/AppIcon.icns"
cp "$work/icon_1024.png" "$root/assets/brand/app-icon.png"
echo "wrote editor/Resources/AppIcon.icns and assets/brand/app-icon.png"
