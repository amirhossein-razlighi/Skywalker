#include "skywalker/ecs/PersistentComponent.h"

namespace sky {

const TypeInfo& Persistent::type() {
    static const TypeInfo info{
        "persistent",
        "Keeps this entity and its children alive across runtime scene changes (change_scene in Wander, scene_change): "
        "a game manager, the player, background music. Its behaviors keep running with their state. If the next scene "
        "has an entity with the same persistent key, that copy is dropped, so a manager can sit in every level.",
        {
            SKY_FIELD(Persistent, id, String,
                      "Key that identifies the same entity in other scenes (\"game_manager\"). Empty: the entity's name"),
            SKY_FIELD(Persistent, spawn, Bool,
                      "Move to the spawn_at entity of a scene change (the player). Without any, entities tagged \"player\" move"),
        }};
    return info;
}

}  // namespace sky
