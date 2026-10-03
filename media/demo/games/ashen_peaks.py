"""Ashen Peaks — mythic mountain valley (original IP; benchmark: modern AAA open worlds).

An eroded alpine valley under drifting volumetric clouds: photoscanned pines and firs, ferns,
grass and mossy boulders streamed as GPU-instanced foliage over a 1.2 km terrain painted with
Poly Haven ground, rock and snow scans.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import polyhaven as ph  # noqa: E402
from kit import Studio  # noqa: E402

PROJECT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))),
                       "examples", "ashen_peaks")


def build(studio):
    pixel = studio.agent("Pixel")
    cirro = studio.agent("Cirro")
    aurora = studio.agent("Aurora")
    cirro.call("scene_new", name="Ashen Peaks", empty=True)

    # --- Materials: Poly Haven photoscans (CC0) -----------------------------------------
    ground = ph.texture(pixel, "forest_leaves_04", tiling=1.0, triplanar=False)
    grass = ph.texture(pixel, "sparse_grass", tiling=1.0, triplanar=False)
    scree = ph.texture(pixel, "rock_ground_02", tiling=1.0, triplanar=False)
    cliff = ph.texture(pixel, "rock_face_03", tiling=1.0, triplanar=True)
    snow = ph.texture(pixel, "snow_02", tiling=1.0, triplanar=False)

    # --- Terrain ------------------------------------------------------------------------------
    layers = [
        {"name": "forest floor", "material": ground, "tiling": 4},
        {"name": "meadow", "material": grass, "tiling": 3.5, "heightMin": 8, "heightMax": 150, "slopeMax": 22, "noise": 0.8, "sharpness": 0.3},
        {"name": "scree", "material": scree, "tiling": 5, "slopeMin": 26, "slopeMax": 40, "noise": 0.6, "sharpness": 0.35},
        {"name": "cliff", "material": cliff, "tiling": 9, "slopeMin": 36, "noise": 0.5, "sharpness": 0.55, "triplanar": True},
        {"name": "snow", "material": snow, "tiling": 6, "heightMin": 205, "slopeMax": 44, "noise": 0.8, "sharpness": 0.6},
    ]
    cirro.call("terrain_create", name="Valley", preset="mountain_valley", size=2400, resolution=2049, seed=11, layers=layers,
               generator={"maxHeight": 420, "featureSize": 900})

    # --- Foliage: photoscanned forest ---------------------------------------------------------
    fir = ph.model(pixel, "fir_sapling_medium", res="1k")
    fir_s = ph.model(pixel, "fir_sapling", res="1k")
    blossom = ph.model(pixel, "jacaranda_tree", res="1k")
    shrubs = [ph.model(pixel, f"shrub_0{i}", res="1k") for i in (1, 2, 4)]
    fern = ph.model(pixel, "fern_02")
    grass_m = ph.model(pixel, "grass_medium_01")
    grass_m2 = ph.model(pixel, "grass_medium_02")
    rocks = ph.model(pixel, "rock_moss_set_01")
    boulder = ph.model(pixel, "boulder_01", res="1k")

    def asset_layer(res, **fields):
        layer = {"prefab": res["prefab"]} if res.get("prefab") else {"mesh": res["mesh"], "material": res.get("material", "")}
        layer.update(fields)
        return layer

    cirro.call("foliage_add", entity="Valley", name="Forest", seed=4, layers=[
        asset_layer(fir, density=0.006, scaleMin=1.6, scaleMax=2.8, slopeMax=33, heightMax=195, terrainLayer=0,
                    alignToNormal=0.05, clumping=0.8, cullDistance=1500, wind=0.3, randomTilt=2, subsurface=0.4),
        asset_layer(fir_s, density=0.01, scaleMin=1.0, scaleMax=2.2, slopeMax=33, heightMax=200, terrainLayer=0,
                    clumping=0.75, cullDistance=500, wind=0.45, seed=3, subsurface=0.4),
        asset_layer(blossom, density=0.0006, scaleMin=0.8, scaleMax=1.2, slopeMax=18, heightMin=10, heightMax=120,
                    terrainLayer=1, clumping=0.6, cullDistance=1500, wind=0.3, subsurface=0.5),
        *[asset_layer(sh, density=0.01, scaleMin=0.7, scaleMax=1.4, slopeMax=32, heightMax=180, clumping=0.8,
                      cullDistance=180, wind=0.6, seed=10 + i, subsurface=0.5) for i, sh in enumerate(shrubs)],
        asset_layer(fern, density=0.15, scaleMin=0.6, scaleMax=1.3, slopeMax=35, heightMax=170, terrainLayer=0,
                    clumping=0.8, cullDistance=70, wind=0.7, castShadows=False, subsurface=0.5),
        asset_layer(grass_m, density=1.4, scaleMin=0.7, scaleMax=1.3, slopeMax=24, terrainLayer=1, clumping=0.5,
                    cullDistance=60, wind=1.0, castShadows=False, subsurface=0.6),
        asset_layer(grass_m2, density=1.0, scaleMin=0.7, scaleMax=1.3, slopeMax=24, terrainLayer=1, clumping=0.6,
                    cullDistance=55, wind=1.0, castShadows=False, seed=21, subsurface=0.6),
        asset_layer(rocks, density=0.002, scaleMin=0.8, scaleMax=2.2, slopeMax=45, clumping=0.6, cullDistance=400,
                    alignToNormal=0.8, sink=0.3, wind=0),
        asset_layer(boulder, density=0.0004, scaleMin=1.5, scaleMax=4.0, slopeMax=50, clumping=0.5, cullDistance=900,
                    alignToNormal=0.6, sink=0.6, wind=0, seed=33),
    ])

    # --- Sky, light, look ------------------------------------------------------------------------
    aurora.call("environment_update", skyMode="atmosphere", sunElevation=13, sunAzimuth=245, sunIntensity=3.4,
                sunColor="#ffe2bc", clouds=0.45, cloudHeight=1800, cloudThickness=1600, cloudScale=1.3,
                fogDensity=0.0009, fogHeight=0.01, fogColor="#b9c6d6", haze=0.0015, godRays=0.5,
                ambient=0.35, autoExposure=True, exposureCompensation=0.2, look="golden_hour", lookStrength=0.6,
                bloomIntensity=0.35, vignette=0.25, grain=0.08, showGrid=False, windSpeed=4, gi=1, ssr=1)
    cirro.call("scene_save", path="scenes/main.sky.json")


if __name__ == "__main__":
    os.makedirs(PROJECT, exist_ok=True)
    st = Studio(PROJECT)
    try:
        build(st)
    finally:
        st.close()
