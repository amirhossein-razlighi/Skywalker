#!/usr/bin/env bash
# Render every still footage slot listed in src/footage.ts into public/footage/ (PNG, 1920x1080),
# then trace line art for the sketch -> clay -> final reveals.
#
#   media/film/scripts/render_stills.sh                     # all slots
#   media/film/scripts/render_stills.sh --only open_hero    # some slots (comma separated ids)
#   media/film/scripts/render_stills.sh --skip-existing --samples 24
#
# Needs the release CLI: cmake --preset release && cmake --build build/release --target skywalker
# Photoscanned scenes (smugglers_cove, hidden_alley, namaqua_canyon, ashen_peaks) need their Poly Haven
# downloads: build them once with media/demo/showcase.py, or point SKY_ASSETS_FROM at a checkout's examples/
# folder that has them (the script stages a copy that links them; your examples stay untouched).
set -euo pipefail
FILM="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -t film-manifest).json"
node "$FILM/scripts/manifest.mjs" > "$TMP"
python3 "$FILM/scripts/render_stills.py" "$TMP" "$@"
python3 "$FILM/scripts/trace_lines.py" "$FILM/public/footage" "$FILM/public/linework"
rm -f "$TMP"
