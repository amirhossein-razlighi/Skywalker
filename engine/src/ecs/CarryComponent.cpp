#include "skywalker/ecs/CarryComponent.h"

namespace sky {

const TypeInfo& Carry::type() {
    static const TypeInfo info{
        "carry",
        "Carries this entity and its children across runtime scene changes (change_scene in Wander, scene_change): "
        "a game manager, the player, background music. Its behaviors keep running with their state. If the next scene "
        "has an entity with the same carry key, that copy is dropped, so a manager can sit in every level. Not the same "
        "as `persist`, which marks what a save game restores (a player usually has both).",
        {
            SKY_FIELD(Carry, id, String,
                      "Key that identifies the same entity in other scenes (\"game_manager\"). Empty: the entity's name"),
            SKY_FIELD(Carry, spawn, Bool,
                      "Move to the spawn_at entity of a scene change (the player). Without any, entities tagged \"player\" move"),
        }};
    return info;
}

}  // namespace sky
