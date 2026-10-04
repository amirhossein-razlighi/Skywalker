#pragma once
// Character tech hooks the engine installs into its subsystems (CharacterHooks.cpp): the foot IK
// ground probe of the AnimationSystem and the body colliders / skinned roots of the groom system.

namespace sky {

class Engine;

/// Called once from the Engine constructor, after the animation system and groom cache exist.
void installCharacterHooks(Engine& engine);

/// Debug views skeleton / ik_targets / groom_roots: X-ray overlays (thin boxes and markers sized to
/// ~2 px) appended to the frame for the view in frame.debugView.
void addCharacterDebugOverlays(Engine& engine, struct FrameData& frame);

}  // namespace sky
