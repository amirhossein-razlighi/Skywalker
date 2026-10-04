#include "skywalker/ecs/PersistComponent.h"

namespace sky {

const TypeInfo& Persist::type() {
    static const TypeInfo info{
        "persist",
        "Saves this entity in save games (save_game in Wander, the save_game tool): its whole state or chosen fields, "
        "plus its Wander vars. Put it on the player, doors, collectibles, quest givers; leave scenery without it. "
        "Spawned entities (spawn() at run time) need spawned: true to be recreated on load.",
        {
            SKY_FIELD(Persist, id, String,
                      "Stable save key (\"player\", \"door_3\"). Empty: the entity id, stable for entities placed in a scene. "
                      "Set it for entities whose id can change (re-created by a script, moved between scenes)"),
            SKY_FIELD_ENUM(Persist, mode,
                           "all (name, tags, enabled, parent and every component) | fields (only `fields`) | vars (Wander "
                           "vars only)",
                           "all", "fields", "vars"),
            SKY_FIELD_JSON(Persist, fields,
                           "mode fields: what to save, as component names or component.field, e.g. [\"transform\", "
                           "\"light.intensity\", \"body.velocity\"]",
                           R"({"type":"array","items":{"type":"string"}})"),
            SKY_FIELD(Persist, vars, Bool, "Also save the entity's Wander vars (hp, inventory, quest state)"),
            SKY_FIELD(Persist, spawned, Bool,
                      "Created at run time (spawn): a load recreates it, with its children and saved ids, when it is missing; "
                      "copies spawned after the save are removed"),
            SKY_FIELD(Persist, prefab, String,
                      "Prefab it was spawned from (informational, shown in save_inspect; filled in from the prefab link when empty)"),
        }};
    return info;
}

}  // namespace sky
