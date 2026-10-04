// Runtime support for scene changes during play (docs/SCENE_FLOW.md): what entities removed between two
// ticks leave behind in the runtime is dropped, so it can never reach another entity. Entity ids are
// never reused, so instances of entities that still exist keep running untouched.

#include "RuntimeInternal.h"

namespace sky::wander {

void Runtime::forgetMissingEntities() {
    Impl& impl = *impl_;
    auto gone = [this](EntityId e) { return e != kNoEntity && !scene_.exists(e); };
    std::erase_if(impl.instances, [&](const auto& kv) { return gone(kv.first.first); });
    std::erase_if(impl.vars, [&](const auto& kv) { return gone(kv.first); });
    std::erase_if(impl.nextPending, [&](const PendingEvent& ev) { return gone(ev.target); });
    std::erase_if(impl.nextContacts, [&](const Contact& c) { return gone(c.self) || gone(c.other); });
}

}  // namespace sky::wander
