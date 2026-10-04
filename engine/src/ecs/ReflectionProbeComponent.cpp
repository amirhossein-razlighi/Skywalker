#include "skywalker/ecs/ReflectionProbeComponent.h"

#include <cstddef>

namespace sky {

const TypeInfo& ReflectionProbe::type() {
    static const TypeInfo info{
        "reflection_probe",
        "A captured cubemap of the scene around this point. Glossy floors, metal and glass inside its influence volume "
        "reflect the room or street around them (box-projected so reflections line up with walls) instead of the sky, "
        "and take their ambient light from it. Screen-space reflections still win where they find a hit; the probe "
        "fills what is off screen; the sky fills the rest. Put one per room or street section (probe_add sizes it "
        "to the room), set `interior` for closed rooms so no sky light leaks in, and check it with probe_info and "
        "viewport_capture {debug_view: \"reflection_probes\"}.",
        {
            SKY_FIELD_ENUM(ReflectionProbe, shape, "Influence volume: box (rooms, corridors, streets) or sphere", "box", "sphere"),
            SKY_FIELD(ReflectionProbe, size, Vec3, "Box: influence extent in meters, centered on the entity (times its scale). Match the room"),
            SKY_FIELD_RANGE(ReflectionProbe, radius, Float, "Sphere: influence radius in meters (times the entity scale)", 0.1f, 10000.f),
            SKY_FIELD_RANGE(ReflectionProbe, blendDistance, Float,
                            "Meters inside the volume over which the probe fades into neighbors or the sky (0 = hard edge)", 0.f, 1000.f),
            SKY_FIELD(ReflectionProbe, boxProjection, Bool,
                      "Parallax-correct reflections against the volume so they line up with its walls (rooms, corridors); "
                      "off for open areas and distant scenery"),
            SKY_FIELD(ReflectionProbe, projectionSize, Vec3,
                      "Box (m) that reflections are projected onto when it should differ from the influence volume: a street "
                      "split into several probes projects each onto the whole street (its facades and far ends). [0, 0, 0] "
                      "= the volume itself"),
            SKY_FIELD(ReflectionProbe, projectionOffset, Vec3, "Center of projectionSize's box relative to the entity (m, local axes)"),
            SKY_FIELD_RANGE(ReflectionProbe, intensity, Float, "Brightness of the probe's reflections and ambient light", 0.f, 16.f),
            SKY_FIELD(ReflectionProbe, interior, Bool,
                      "Closed room: no sky light inside the volume. Reflections and ambient come only from the probe, and the "
                      "capture lights its walls with ambientColor instead of the sky (the sky still shows through windows)"),
            SKY_FIELD_ENUM(ReflectionProbe, ambient,
                           "Diffuse ambient inside the volume: probe (from the capture, default), sky (the probe only adds "
                           "reflections), color (ambientColor x ambientEnergy)",
                           "probe", "sky", "color"),
            SKY_FIELD(ReflectionProbe, ambientColor, Color,
                      "Interior captures: the ambient light that replaces the sky's; ambient \"color\": the ambient inside"),
            SKY_FIELD_RANGE(ReflectionProbe, ambientEnergy, Float, "Multiplies ambientColor", 0.f, 100.f),
            SKY_FIELD_ENUM(ReflectionProbe, update,
                           "When to capture: once (cached; re-captured when the probe changes or on probe_bake), on_change "
                           "(whenever something in range moves or changes), realtime (every `interval` frames)",
                           "once", "on_change", "realtime"),
            SKY_FIELD_RANGE(ReflectionProbe, interval, Int, "realtime: frames between captures (1 = every frame, within the face budget)", 1.f,
                            600.f),
            SKY_FIELD_RANGE(ReflectionProbe, resolution, Int,
                            "Cubemap face size in px: 64, 128, 256 (default) or 512; rounded to a power of two", 64.f, 512.f),
            SKY_FIELD_RANGE(ReflectionProbe, cullMask, Int,
                            "Render layers the capture draws (bit i = layer i+1; 1048575 = all). Leave the player out of "
                            "realtime probes", 0.f, 1048575.f),
            SKY_FIELD_RANGE(ReflectionProbe, priority, Int, "Where volumes overlap the higher priority wins (equal: the smaller volume)",
                            -1000.f, 1000.f),
            SKY_FIELD(ReflectionProbe, captureOffset, Vec3,
                      "Capture point relative to the entity in meters (local axes); keep it inside the room, away from walls"),
            SKY_FIELD_RANGE(ReflectionProbe, maxDistance, Float, "Capture far plane in meters (0 = automatic from the volume)", 0.f,
                            20000.f),
        }};
    return info;
}

}  // namespace sky
