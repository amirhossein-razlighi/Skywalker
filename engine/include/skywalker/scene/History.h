#pragma once
// Transactional, actor-attributed undo/redo.
//
// Every edit runs inside a transaction:  begin(actor, label) -> scene edits -> commit().
// The History observes the Scene and lazily snapshots each entity the *first* time it
// is touched ("before") and again at commit ("after"). Undo/redo simply restores those
// snapshots. This is generic: any edit expressible through the Scene API is undoable,
// batches of agent tool calls become one atomic step, and a failed batch is rolled
// back with `rollback()` leaving the scene exactly as it was.
//
// Each entry records *who* made the change (a human, or a named agent), which powers
// the editor's activity feed ("Nimbus the level designer moved 3 entities").

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/scene/Scene.h"

namespace sky {

struct HistoryEntry {
    uint64_t serial = 0;
    std::string actor;  // "user", "agent:Nimbus", "mcp:claude-code", ...
    std::string label;  // "Move Crate", "Batch (5 ops)"
    std::vector<EntityId> order;              // touch order
    std::vector<std::pair<EntityId, Json>> before;
    std::vector<std::pair<EntityId, Json>> after;
    bool environmentChanged = false;
    Json environmentBefore;
    Json environmentAfter;

    Json summary() const;  // compact description for agents and the activity feed
};

class History final : public ChangeObserver {
public:
    explicit History(Scene& scene, size_t capacity = 256);
    ~History() override;

    void begin(std::string actor, std::string label);
    bool inTransaction() const { return active_; }
    /// Commits the open transaction. Returns false if nothing changed (no entry added).
    bool commit();
    /// Restores all touched state to how it was at begin().
    void rollback();

    bool canUndo() const { return cursor_ > 0; }
    bool canRedo() const { return cursor_ < entries_.size(); }
    const HistoryEntry* undo();
    const HistoryEntry* redo();
    void clear();

    const std::deque<HistoryEntry>& entries() const { return entries_; }
    size_t cursor() const { return cursor_; }
    const HistoryEntry* lastCommitted() const { return cursor_ ? &entries_[cursor_ - 1] : nullptr; }

    // ChangeObserver
    void beforeEntityChange(EntityId id) override;
    void beforeEnvironmentChange() override;

private:
    void restore(const std::vector<EntityId>& order, const std::vector<std::pair<EntityId, Json>>& states,
                 bool environment, const Json& env, bool reverse);

    Scene& scene_;
    size_t capacity_;
    std::deque<HistoryEntry> entries_;
    size_t cursor_ = 0;
    uint64_t serial_ = 0;
    bool active_ = false;
    HistoryEntry pending_;
};

}  // namespace sky
