#pragma once
// Character tech hooks the engine installs into its subsystems (CharacterHooks.cpp): the foot IK
// ground probe of the AnimationSystem and the body colliders / skinned roots of the groom system.

namespace sky {

class Engine;

/// Called once from the Engine constructor, after the animation system and groom cache exist.
void installCharacterHooks(Engine& engine);

}  // namespace sky
