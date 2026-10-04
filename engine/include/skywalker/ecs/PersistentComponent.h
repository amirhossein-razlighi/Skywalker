#pragma once
// `persistent`: the entity (and its children) survives runtime scene changes (change_scene): a game
// manager, the player, the music (docs/SCENE_FLOW.md). Included at the end of Components.h.
//
// When the next scene contains an entity with the same `persistent` key (the same game manager placed
// in every level so each level also runs on its own), the incoming copy is dropped and the running
// one is kept.

#include <string>

#include "skywalker/ecs/Reflection.h"

namespace sky {

struct Persistent {
    std::string id;      // key that identifies the same entity in other scenes ("" = its name)
    bool spawn = false;  // moves to the spawn_at point of a scene change (default: entities tagged "player")

    static const TypeInfo& type();
};

}  // namespace sky
