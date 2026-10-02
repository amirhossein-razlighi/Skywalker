#include "skywalker/scene/History.h"

#include <algorithm>

namespace sky {

Json HistoryEntry::summary() const {
    Json ids = Json::array();
    for (EntityId id : order) ids.push(id);
    return Json::object({{"serial", serial},
                         {"actor", actor},
                         {"label", label},
                         {"entities", ids},
                         {"environment", environmentChanged}});
}

History::History(Scene& scene, size_t capacity) : scene_(scene), capacity_(capacity) { scene_.setObserver(this); }

History::~History() {
    if (scene_.observer() == this) scene_.setObserver(nullptr);
}

void History::begin(std::string actor, std::string label) {
    if (active_) rollback();  // never nest silently: a dangling transaction is a bug
    pending_ = HistoryEntry{};
    pending_.actor = std::move(actor);
    pending_.label = std::move(label);
    active_ = true;
}

void History::beforeEntityChange(EntityId id) {
    if (!active_) return;
    for (EntityId e : pending_.order) {
        if (e == id) return;  // already captured the before-state
    }
    pending_.order.push_back(id);
    pending_.before.emplace_back(id, scene_.snapshotEntity(id));
}

void History::beforeEnvironmentChange() {
    if (!active_ || pending_.environmentChanged) return;
    pending_.environmentChanged = true;
    pending_.environmentBefore = reflect::toJson(&scene_.environment(), Environment::type());
}

bool History::commit() {
    if (!active_) return false;
    active_ = false;
    // Capture after-states and drop entities whose state did not actually change.
    HistoryEntry entry = std::move(pending_);
    std::vector<EntityId> order;
    std::vector<std::pair<EntityId, Json>> before, after;
    for (size_t i = 0; i < entry.order.size(); ++i) {
        EntityId id = entry.order[i];
        Json now = scene_.snapshotEntity(id);
        if (now == entry.before[i].second) continue;
        order.push_back(id);
        before.push_back(entry.before[i]);
        after.emplace_back(id, std::move(now));
    }
    entry.order = std::move(order);
    entry.before = std::move(before);
    entry.after = std::move(after);
    if (entry.environmentChanged) {
        entry.environmentAfter = reflect::toJson(&scene_.environment(), Environment::type());
        entry.environmentChanged = entry.environmentAfter != entry.environmentBefore;
    }
    if (entry.order.empty() && !entry.environmentChanged) return false;

    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(cursor_), entries_.end());  // drop redo tail
    entry.serial = ++serial_;
    entries_.push_back(std::move(entry));
    if (entries_.size() > capacity_) entries_.pop_front();
    cursor_ = entries_.size();
    return true;
}

void History::rollback() {
    if (!active_) return;
    active_ = false;
    restore(pending_.order, pending_.before, pending_.environmentChanged, pending_.environmentBefore, true);
    pending_ = HistoryEntry{};
}

void History::restore(const std::vector<EntityId>& order, const std::vector<std::pair<EntityId, Json>>& states,
                      bool environment, const Json& env, bool reverse) {
    (void)order;
    ChangeObserver* saved = scene_.observer();
    scene_.setObserver(nullptr);
    // Restoring "before" states walks the touch list backwards so that later changes
    // are unwound first; but entities that must be *recreated* need their parents to
    // exist, so recreation happens in forward order in a second pass.
    if (reverse) {
        for (auto it = states.rbegin(); it != states.rend(); ++it) {
            if (it->second.isNull() || scene_.exists(it->first)) scene_.restoreEntity(it->first, it->second);
        }
        for (const auto& [id, snap] : states) {
            if (!snap.isNull() && !scene_.exists(id)) scene_.restoreEntity(id, snap);
        }
        // Second forward pass fixes parent links for entities restored before their parent.
        for (const auto& [id, snap] : states) {
            if (!snap.isNull()) scene_.restoreEntity(id, snap);
        }
    } else {
        for (const auto& [id, snap] : states) scene_.restoreEntity(id, snap);
        for (const auto& [id, snap] : states) {
            if (!snap.isNull()) scene_.restoreEntity(id, snap);
        }
    }
    if (environment) (void)reflect::applyJson(&scene_.environment(), Environment::type(), env);
    scene_.markDirty();
    scene_.setObserver(saved);
}

const HistoryEntry* History::undo() {
    if (active_ || !canUndo()) return nullptr;
    const HistoryEntry& e = entries_[--cursor_];
    restore(e.order, e.before, e.environmentChanged, e.environmentBefore, true);
    return &e;
}

const HistoryEntry* History::redo() {
    if (active_ || !canRedo()) return nullptr;
    const HistoryEntry& e = entries_[cursor_++];
    restore(e.order, e.after, e.environmentChanged, e.environmentAfter, false);
    return &e;
}

void History::clear() {
    entries_.clear();
    cursor_ = 0;
    active_ = false;
}

}  // namespace sky
