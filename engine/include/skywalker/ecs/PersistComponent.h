#pragma once
// `persist`: marks an entity whose state goes into save games (docs/SAVE_GAMES.md). Included at
// the end of Components.h.
//
// A save stores, for every entity with this component, either its whole state (`mode: all`: name,
// tags, enabled, parent, every component) or only the listed `fields` ("transform",
// "light.intensity"), plus its Wander vars. Loading matches entities by `id` (or by entity id
// when `id` is empty), recreates runtime-spawned ones (`spawned`), and destroys scene entities
// that were destroyed before the save.

#include <string>

#include "skywalker/core/Json.h"
#include "skywalker/ecs/Reflection.h"

namespace sky {

struct Persist {
    std::string id;                // stable save key; "" = the entity id ("#12"), stable for entities placed in a scene
    std::string mode = "all";      // all (whole entity) | fields (only `fields`) | vars (Wander vars only)
    Json fields = Json::array();   // mode fields: ["transform", "light.intensity", "health"] (component or component.field)
    bool vars = true;              // save the entity's Wander vars too
    bool spawned = false;          // created at run time (spawn): recreated with its children when a save is loaded
    std::string prefab;            // prefab it was spawned from (filled in from the prefab link when empty)

    static const TypeInfo& type();
};

}  // namespace sky
