#!/usr/bin/env python3
"""Run the "Your first game in 15 minutes" tutorial end to end and capture its screenshots.

    python3 website/scripts/tutorial_coin_run.py --cli build/release/bin/skywalker [--keep DIR]

Every tool call below is the exact call shown on website/docs/getting-started/first-game.md, in the same order, so the
tutorial is executable documentation: if a step breaks, this script fails. It builds the project in a temporary folder
(or DIR with --keep), plays it with simulated input, checks that coins are collected, and writes the screenshots to
website/docs/assets/images/tutorial/.
"""
from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_media as mm  # noqa: E402

OUT = mm.ASSETS / "images" / "tutorial"

PLAYER = """behavior PlayerMove
  intent "Walk with WASD or the left stick, jump with space or the south button."
  on tick
    let m = axis("move")
    walk(self, (m.x, 0, -m.y))
  end
  on action "jump"
    jump(self)
  end
end
"""

COIN = """behavior Coin
  intent "Spins in place. When the player touches it, it reports a collected coin and disappears."
  param spin = 120 in 0..360 "degrees per second"
  on tick
    rotate self by (0, spin * dt, 0)
  end
  on trigger_enter "player"
    emit "coin_collected"
    destroy self
  end

  test "spins"
    let before = self.rotation.y
    wait 0.5
    expect self.rotation.y != before
  end
end
"""

SCORE = """behavior Score
  intent "Counts collected coins and shows the count on the HUD."
  var coins = 0
  on event "coin_collected"
    coins += 1
    find("Score").ui.text = "Coins: {coins} / 5"
    if coins >= 5 then
      find("Score").ui.text = "All coins collected!"
    end
  end
end
"""

FOLLOW = """behavior FollowCam
  intent "Follow the player from behind and above, smoothly."
  on tick
    let p = find("Player")
    if p then
      self.position = lerp(self.position, p.position + (0, 5, 9), 1 - exp(-4 * dt))
      look self at p
    end
  end
end
"""

# (tool, args) in the order of the tutorial page.
STEPS: list[tuple[str, dict]] = [
    ("scene_new", {"name": "Coin Run", "empty": True}),
    ("environment_update", {"preset": "sunset", "skyMode": "atmosphere", "clouds": 0.4, "fogDensity": 0.002, "haze": 0.004,
                            "showGrid": False}),
    ("batch", {"operations": [
        {"tool": "entity_create", "args": {"name": "Island", "mesh": "cylinder", "position": [0, -0.5, 0],
                                           "scale": [24, 1, 24], "color": "#7fb069"}},
        {"tool": "entity_create", "args": {"name": "Step 1", "mesh": "cube", "position": [4, 0.4, -4],
                                           "scale": [3, 0.8, 3], "color": "#d9c7a7"}},
        {"tool": "entity_create", "args": {"name": "Step 2", "mesh": "cube", "position": [7, 1.2, -8],
                                           "scale": [3, 0.8, 3], "color": "#d9c7a7"}},
        {"tool": "entity_create", "args": {"name": "Player", "mesh": "capsule", "position": [0, 1, 4],
                                           "color": "#3cc9b0", "tags": ["player"]}},
        {"tool": "entity_create", "args": {"name": "Camera", "position": [0, 5, 13], "rotation": [-18, 0, 0],
                                           "components": {"camera": {"primary": True, "fov": 55}}}}]}),
    ("physics_add", {"entities": ["Island", "Step 1", "Step 2"], "preset": "static_level"}),
    ("physics_add", {"entity": "Player", "preset": "player_character"}),
    ("behavior_set", {"entity": "Player", "name": "PlayerMove", "source": PLAYER}),
    ("behavior_set", {"entity": "Camera", "name": "FollowCam", "source": FOLLOW}),
    ("entity_create", {"name": "Coin", "mesh": "torus", "position": [0, 1, 0], "rotation": [90, 0, 0],
                       "scale": [0.6, 0.6, 0.6], "color": "#f5c542",
                       "components": {"mesh": {"metallic": 1, "roughness": 0.25, "emissive": "#f5c54240"}}}),
    ("physics_add", {"entity": "Coin", "preset": "trigger_zone"}),
    ("behavior_set", {"entity": "Coin", "name": "Coin", "source": COIN}),
    ("wander_test", {"entity": "Coin", "name": "Coin"}),
    ("entity_duplicate", {"entity": "Coin", "offset": [-3, 0, -2], "count": 2}),
    ("entity_duplicate", {"entity": "Coin", "name": "High Coin", "offset": [7, 1.6, -8]}),
    ("entity_duplicate", {"entity": "Coin", "name": "Far Coin", "offset": [-6, 0, 3]}),
    ("ui_create", {"canvas": {"name": "HUD", "theme": "dark"}, "elements": [
        {"type": "text", "name": "Score", "anchor": "top_left", "position": [32, 28], "text": "Coins: 0 / 5",
         "style": "large"}]}),
    ("behavior_set", {"entity": "HUD", "name": "Score", "source": SCORE}),
    ("scene_save", {"path": "scenes/main.sky.json"}),
    ("game_settings", {"operation": "set", "settings": {"title": "Coin Run", "startScene": "scenes/main.sky.json",
                                                         "version": "1.0.0"}}),
]


def run(cli: str, project: Path) -> None:
    (project / "scenes").mkdir(parents=True, exist_ok=True)
    mcp = mm.Mcp(cli, project)
    try:
        for tool, args in STEPS:
            result = mcp.call(tool, args)
            if tool == "wander_test" and (result.get("failed") or not result.get("compiled")):
                raise SystemExit(f"wander_test failed: {result}")
            if tool == "batch":
                mm.capture(mcp, OUT / "step-layout.webp", view="scene")
        mm.capture(mcp, OUT / "step-coins.webp", view="scene")
        mm.capture(mcp, OUT / "step-annotated.webp", view="scene", annotate=True)
        # Play: walk toward the first coin, then check the score.
        mcp.call("sim_control", {"action": "play"})
        mcp.call("sim_input", {"axes": [{"name": "move", "x": 0, "y": 1, "ticks": 70}]})
        mcp.call("sim_control", {"action": "step", "ticks": 70})
        hud = mcp.call("entity_get", {"entity": "HUD"})
        coins = hud.get("vars", {}).get("coins")
        if not coins:
            raise SystemExit(f"walking forward did not collect a coin (HUD vars: {hud.get('vars')})")
        mm.capture(mcp, OUT / "step-play.webp", view="scene")
        mcp.call("sim_control", {"action": "stop"})
        print(f"tutorial OK: {coins} coin(s) collected after walking forward")
    finally:
        mcp.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--keep", help="build the project in this folder and keep it")
    opts = ap.parse_args()
    cli = str(Path(opts.cli).resolve())
    if opts.keep:
        project = Path(opts.keep)
        if project.exists():
            shutil.rmtree(project)
        project.mkdir(parents=True)
        run(cli, project)
    else:
        with tempfile.TemporaryDirectory() as tmp:
            run(cli, Path(tmp) / "coin_run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
