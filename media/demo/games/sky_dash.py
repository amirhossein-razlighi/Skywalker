"""Sky Dash — the 2D side-scroller sample, rebuilt through the showcase pipeline.

Reuses build_sky_dash.py (engine tools + Wander only); here it runs as the crew's level
designer so the edits are attributed and recordable like every other showcase game.
"""
import os

META = dict(
    id="sky_dash", title="Sky Dash", genre="2D Platformer", mood="bright",
    pitch="Run, hop, grab every coin. Same engine, two dimensions.",
    view=dict(eye=[14, 4, 30], target=[14, 3, 0]),
)

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build(studio):
    cir = studio.agent("Cirro")
    cir.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])

    class _Shared:
        """Hands the level designer's connection to the original build script."""
        def __new__(cls, *args, **kwargs):
            return cir.sky

    source = open(os.path.join(HERE, "build_sky_dash.py")).read()
    source = source.replace("from sky import Sky", "").replace("import sys\n", "").replace("sky.close()", "")
    exec(compile(source, "build_sky_dash.py", "exec"),
         {"Sky": _Shared, "sys": type("argv", (), {"argv": ["build_sky_dash.py", studio.project]})(), "__name__": "sky_dash"})
    studio.agent("Aurora").call("environment_update", clouds=0.25, tonemap="neutral", ao=0.6, saturation=1.12)
    studio.agent("Nimbus").call("scene_save", path="scenes/main.sky.json")


def shots():
    return [dict(name="play", frames=270, view="scene", warmup=0)]
