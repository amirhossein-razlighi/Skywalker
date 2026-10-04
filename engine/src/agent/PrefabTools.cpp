// Entity links and linked prefabs (docs/PREFABS.md): how agents see who references what, find broken links,
// copy/paste entities with their links intact, and inspect, revert, apply, unpack and relink prefab instances.

#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_set>

#include "ToolHelpers.h"
#include "skywalker/assets/Prefab.h"
#include "skywalker/core/Strings.h"
#include "skywalker/scene/PrefabLink.h"

namespace sky::tools {

using namespace schema;
namespace fs = std::filesystem;

namespace {

std::string nameOf(const Scene& s, EntityId e) {
    const EntityRecord* r = s.record(e);
    return r ? r->name : std::string();
}

std::string refText(const Scene& s, EntityId e) { return formatEntityRef(e) + " " + nameOf(s, e); }

std::string linkLabel(const LinkInfo& l) {
    std::string where = l.component + "." + l.field;
    if (l.index >= 0) where += "[" + std::to_string(l.index) + "]";
    return where;
}

std::string linkTargetText(const LinkInfo& l) {
    if (!l.link.name.empty()) return "\"" + l.link.name + "\"" + (l.link.id ? " (" + formatEntityRef(l.link.id) + ")" : "");
    return formatEntityRef(l.link.id);
}

/// "ok" (bound to an existing id), "by_name" (resolved through its name only: breaks on rename until bound),
/// "dangling" (points at nothing).
const char* linkStatus(const LinkInfo& l) {
    if (!l.target) return "dangling";
    return l.byName ? "by_name" : "ok";
}

Json linkJson(const Scene& s, const LinkInfo& l) {
    Json j = Json::object({{"from", l.from},
                           {"from_name", nameOf(s, l.from)},
                           {"component", l.component},
                           {"field", l.field},
                           {"stored", reflect::entityLinkToJson(l.link)},
                           {"target", l.target},
                           {"target_name", l.target ? nameOf(s, l.target) : ""},
                           {"status", linkStatus(l)}});
    if (l.index >= 0) j["index"] = l.index;
    return j;
}

Json prefabInfo(const Scene& s, EntityId e) {
    const EntityRecord* r = s.record(e);
    if (!r || !r->prefab.linked()) return {};
    EntityId root = r->prefab.instance;
    const EntityRecord* rr = s.record(root);
    Json j = Json::object({{"instance", root}, {"pid", r->prefab.pid}, {"root", root == e}});
    if (rr) j["source"] = rr->prefab.source;
    if (auto t = s.prefabTemplate(rr ? rr->prefab.source : ""); t && t->path.count(r->prefab.pid)) {
        j["node"] = t->path.at(r->prefab.pid);
    }
    return j;
}

Result<std::shared_ptr<const PrefabTemplate>> latestTemplate(Engine& engine, EntityId root) {
    const EntityRecord* r = engine.scene().record(root);
    if (!r) return Error::make("not_found", "no entity " + formatEntityRef(root));
    if (!r->prefab.guid.empty()) {
        if (auto t = engine.prefabTemplateAsset("guid:" + r->prefab.guid)) return t;
    }
    return engine.prefabTemplateAsset(r->prefab.source);
}

std::string overrideLine(const prefab::Override& o) {
    std::string v = o.value.dump();
    std::string pv = o.prefabValue.dump();
    if (v.size() > 80) v = v.substr(0, 77) + "...";
    if (pv.size() > 60) pv = pv.substr(0, 57) + "...";
    return o.node + " (" + formatEntityRef(o.entity) + ") " + o.property + " = " + v + "  (prefab: " + pv + ")";
}

std::string diffText(const Scene& s, const prefab::InstanceDiff& d) {
    std::ostringstream os;
    os << refText(s, d.root) << " <- " << (d.tmpl ? d.tmpl->source : "?") << ": " << d.count(false) << " override(s)";
    os << ", " << d.members.size() << " entities\n";
    for (const auto& o : d.overrides) {
        if (!o.placement) os << "  " << overrideLine(o) << "\n";
    }
    for (uint32_t pid : d.removed) {
        os << "  removed: " << (d.tmpl && d.tmpl->path.count(pid) ? d.tmpl->path.at(pid) : "") << " (pid " << pid << ")\n";
    }
    for (EntityId a : d.added) os << "  added: " << refText(s, a) << " under " << refText(s, s.record(a)->parent) << "\n";
    for (const auto& w : d.warnings) os << "  warning: " << w << "\n";
    return os.str();
}

/// Clipboard document for entity_copy / entity_paste: complete entity records (ids, parents, links, prefab
/// membership), subtrees included.
Json clipboardFor(const Scene& s, const std::vector<EntityId>& roots) {
    Json ents = Json::array();
    std::unordered_set<EntityId> seen;
    Json rootIds = Json::array();
    for (EntityId r : roots) {
        std::vector<EntityId> sub;
        collectSubtree(s, r, sub);
        if (!seen.count(r)) rootIds.push(r);
        for (EntityId e : sub) {
            if (!seen.insert(e).second) continue;
            Json j = s.entityToJson(e);
            if (e == r) j["parent"] = 0;  // roots are re-parented on paste
            ents.push(std::move(j));
        }
    }
    return Json::object({{"format", "skywalker.clipboard"}, {"roots", rootIds}, {"entities", ents}});
}

}  // namespace

void addPrefabTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"entity_refs", "Entity references",
             "Who references an entity and what it references (joint targets, camera follow, look-at, bone "
             "attachments, IK, groom/particle colliders...). Links store the target's id plus its name, so they survive "
             "renames. Without `entity`: a scene-wide health check listing dangling links (pointing at nothing), links "
             "that only resolve by name (fragile), duplicate unique names and broken prefab instances, each with a fix. "
             "Example: {\"entity\": \"Door\"} before renaming or deleting it.",
             "entity",
             object({{"entity", entity("Entity to inspect; omit for the scene-wide check")},
                     {"limit", integer("Max problems listed (default 50)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     Json out = Json::array(), in = Json::array();
                     std::ostringstream os;
                     os << refText(s, *id);
                     const EntityRecord* r = s.record(*id);
                     if (r->unique) os << " [unique: find(\"%" << r->name << "\")]";
                     os << "\n";
                     auto outgoing = s.linksFrom(*id);
                     auto incoming = s.linksTo(*id);
                     os << "outgoing (" << outgoing.size() << "):\n";
                     for (const auto& l : outgoing) {
                         out.push(linkJson(s, l));
                         os << "  " << linkLabel(l) << " -> " << (l.target ? refText(s, l.target) : linkTargetText(l)) << " ["
                            << linkStatus(l) << "]\n";
                     }
                     os << "incoming (" << incoming.size() << "):\n";
                     for (const auto& l : incoming) {
                         in.push(linkJson(s, l));
                         os << "  " << refText(s, l.from) << " " << linkLabel(l) << " [" << linkStatus(l) << "]\n";
                     }
                     Json j = Json::object({{"entity", *id}, {"name", r->name}, {"unique", r->unique}, {"outgoing", out},
                                            {"incoming", in}});
                     if (Json p = prefabInfo(s, *id); !p.isNull()) {
                         j["prefab"] = p;
                         os << "prefab: " << (p.get("root").asBool() ? "instance root" : "member") << " of "
                            << refText(s, static_cast<EntityId>(p.get("instance").asInt())) << " ("
                            << p.get("source").asString() << ")\n";
                     }
                     ToolResult res = ToolResult::text(os.str());
                     res.structured = j;
                     return res;
                 }
                 size_t limit = static_cast<size_t>(a.get("limit").asInt(50));
                 size_t total = 0;
                 Json dangling = Json::array(), byName = Json::array(), dupes = Json::array(), broken = Json::array();
                 std::ostringstream os;
                 for (EntityId e : s.entities()) {
                     for (const auto& l : s.linksFrom(e)) {
                         ++total;
                         if (!l.target && dangling.size() < limit) {
                             Json j = linkJson(s, l);
                             j["fix"] = "entity_update {\"entity\": " + std::to_string(e) + ", \"components\": {\"" + l.component +
                                        "\": {\"" + l.field + "\": \"<entity name or #id>\"}}} (or null to clear it)";
                             dangling.push(j);
                             os << "dangling: " << refText(s, e) << " " << linkLabel(l) << " -> " << linkTargetText(l) << "\n";
                         } else if (l.target && l.byName && byName.size() < limit) {
                             byName.push(linkJson(s, l));
                             os << "by name only: " << refText(s, e) << " " << linkLabel(l) << " -> \"" << l.link.name
                                << "\" (ambiguous or created before its target; set it again to bind it)\n";
                         }
                     }
                 }
                 // Unique names must be unique within their owner (a prefab instance, or the scene).
                 std::map<std::pair<EntityId, std::string>, std::vector<EntityId>> uniques;
                 for (EntityId e : s.entities()) {
                     const EntityRecord* r = s.record(e);
                     if (r->unique) uniques[{s.ownerOf(e), r->name}].push_back(e);
                 }
                 for (const auto& [key, ids] : uniques) {
                     if (ids.size() < 2) continue;
                     Json list = Json::array();
                     for (EntityId e : ids) list.push(e);
                     dupes.push(Json::object({{"owner", key.first}, {"name", key.second}, {"entities", list}}));
                     os << "duplicate unique name \"" << key.second << "\" in " << (key.first ? refText(s, key.first) : "the scene")
                        << ": " << ids.size() << " entities (find(\"%" << key.second << "\") picks the first)\n";
                 }
                 for (EntityId root : prefab::instances(s)) {
                     const EntityRecord* r = s.record(root);
                     if (!r->prefab.pending.isNull()) {
                         broken.push(Json::object({{"instance", root}, {"name", r->name}, {"source", r->prefab.source},
                                                   {"problem", "prefab file missing; placeholder keeps its overrides"}}));
                         os << "broken prefab: " << refText(s, root) << " (" << r->prefab.source << " missing)\n";
                     }
                 }
                 for (const auto& w : s.loadWarnings()) os << "load: " << w << "\n";
                 bool clean = dangling.size() == 0 && byName.size() == 0 && dupes.size() == 0 && broken.size() == 0;
                 std::string head = std::to_string(total) + " links in the scene; " +
                                    (clean ? "no problems" : std::to_string(dangling.size()) + " dangling, " +
                                                                 std::to_string(byName.size()) + " by name only, " +
                                                                 std::to_string(dupes.size()) + " duplicate unique names, " +
                                                                 std::to_string(broken.size()) + " broken prefab instances");
                 ToolResult res = ToolResult::text(head + "\n" + os.str());
                 Json warnings = Json::array();
                 for (const auto& w : s.loadWarnings()) warnings.push(w);
                 res.structured = Json::object({{"links", total},
                                                {"dangling", dangling},
                                                {"by_name", byName},
                                                {"duplicate_unique", dupes},
                                                {"broken_prefabs", broken},
                                                {"load_warnings", warnings}});
                 return res;
             }});

    reg.add({"entity_copy", "Copy entities",
             "Copy entities (with children) to a clipboard document for entity_paste — in this scene or after "
             "scene_load in another. Links between copied entities and prefab instances inside the copy are kept.",
             "entity",
             object({{"entities", array(entity(), "Entities to copy (their children come along)")}}, {"entities"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 std::vector<EntityId> roots;
                 for (const auto& e : a.get("entities").elements()) {
                     auto id = resolve(engine, e);
                     if (!id) return ToolResult::error(id.error());
                     roots.push_back(*id);
                 }
                 Json clip = clipboardFor(engine.scene(), roots);
                 return ToolResult::json(clip, "copied " + std::to_string(clip.get("entities").size()) + " entities");
             }});

    reg.add({"entity_paste", "Paste entities",
             "Create copies of a clipboard document from entity_copy. Links among the pasted entities point at the new "
             "copies (a pasted rig stays wired to itself); links to other entities keep their target; prefab instances "
             "stay linked. Optional parent and position offset.",
             "entity",
             object({{"data", Json::object({{"type", "object"}, {"description", "The clipboard document from entity_copy"}})},
                     {"parent", entity("Parent of the pasted roots (default: none)")},
                     {"offset", vec3("Added to each pasted root's position")}},
                    {"data"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 const Json& data = a.get("data");
                 if (data.get("format").asString() != "skywalker.clipboard") {
                     return ToolResult::error(Error::make("invalid_arguments", "data must be the result of entity_copy",
                                                          "call entity_copy {\"entities\": [...]} first"));
                 }
                 Scene clip;
                 Json doc = Json::object({{"format", "skywalker.scene"}, {"entities", data.get("entities")}});
                 if (Status s = clip.loadJson(doc); !s) return fail(s);
                 std::vector<EntityId> roots;
                 for (const auto& r : data.get("roots").elements()) {
                     auto id = static_cast<EntityId>(r.asInt());
                     if (clip.exists(id)) roots.push_back(id);
                 }
                 EntityId parent = kNoEntity;
                 if (a.contains("parent")) {
                     auto p = resolve(engine, a.get("parent"));
                     if (!p) return ToolResult::error(p.error());
                     parent = *p;
                 }
                 Vec3 offset{0, 0, 0};
                 reflect::jsonToVec3(a.get("offset"), offset);
                 std::vector<EntityId> created;
                 Status st = engine.edit(ctx.actor, "Paste " + std::to_string(roots.size()) + " entities", [&]() -> Status {
                     created = engine.scene().cloneTrees(clip, roots, parent);
                     for (EntityId r : created) {
                         if (auto* t = engine.scene().get<Transform>(r)) t->position += offset;
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 std::string out;
                 Json ids = Json::array();
                 for (EntityId r : created) {
                     out += describe(engine.scene(), r) + "\n";
                     ids.push(r);
                 }
                 ToolResult r = ToolResult::text("pasted:\n" + out);
                 r.structured = Json::object({{"created", ids}});
                 return r;
             }});

    reg.add({"prefab_overrides", "Prefab overrides",
             "How a linked prefab instance differs from its prefab: per-field overrides (with the prefab's value), "
             "removed nodes, added entities and its placement (root name/transform, never applied). `entity` may be "
             "the instance root or any entity inside it. Without `entity`: every instance in the scene with its override "
             "count. Then use prefab_revert to undo overrides or prefab_apply to push them into the prefab file.",
             "asset",
             object({{"entity", entity("An instance root or member; omit to list all instances")},
                     {"prefab", string("Only instances of this prefab (path) when listing")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     EntityId root = prefab::instanceOf(s, *id);
                     if (!root) {
                         return ToolResult::error(Error::make("not_linked", refText(s, *id) + " is not part of a prefab instance",
                                                              "prefab_overrides without arguments lists the instances; "
                                                              "prefab_relink links copies made before prefabs were linked"));
                     }
                     auto d = prefab::diff(s, root);
                     if (!d) return ToolResult::error(d.error());
                     Json j = d->toJson(s);
                     if (auto latest = latestTemplate(engine, root); latest && d->tmpl && (*latest)->version != d->tmpl->version) {
                         j["stale"] = true;  // the file changed; the next asset scan re-expands it
                     }
                     ToolResult res = ToolResult::text(diffText(s, *d));
                     res.structured = j;
                     return res;
                 }
                 std::string only = a.get("prefab").asString();
                 Json list = Json::array();
                 std::ostringstream os;
                 for (EntityId root : prefab::instances(s, only)) {
                     const EntityRecord* r = s.record(root);
                     Json j = Json::object({{"instance", root}, {"name", r->name}, {"source", r->prefab.source}});
                     if (!r->prefab.pending.isNull()) {
                         j["broken"] = true;
                         os << refText(s, root) << " <- " << r->prefab.source << " (MISSING: placeholder)\n";
                     } else if (auto d = prefab::diff(s, root)) {
                         j["override_count"] = d->count(false);
                         j["removed"] = d->removed.size();
                         j["added"] = d->added.size();
                         os << refText(s, root) << " <- " << r->prefab.source << ": " << d->count(false) << " override(s)\n";
                     } else {
                         j["error"] = d.error().message;
                         os << refText(s, root) << " <- " << r->prefab.source << ": " << d.error().message << "\n";
                     }
                     list.push(j);
                 }
                 ToolResult res = ToolResult::text(list.size() ? os.str() : "no linked prefab instances in this scene");
                 res.structured = Json::object({{"instances", list}});
                 return res;
             }});

    reg.add({"prefab_revert", "Revert prefab overrides",
             "Undo overrides so an instance matches its prefab again. `property` reverts one override of `entity` "
             "(\"light.intensity\", \"name\", \"vars.hp\", \"collider\" for an added/removed component); without it, all "
             "of that entity's overrides (placement kept). `all`: true reverts the whole instance and restores removed "
             "nodes (added entities stay). Undoable.",
             "asset",
             object({{"entity", entity("Instance root or member")},
                     {"property", string("One override to revert, as listed by prefab_overrides")},
                     {"all", boolean("Revert the whole instance (default false)")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 size_t n = 0;
                 Status st = engine.edit(ctx.actor, "Revert prefab overrides", [&]() -> Status {
                     auto r = prefab::revert(engine.scene(), *id, a.get("property").asString(), a.get("all").asBool(false));
                     if (!r) return r.error();
                     n = *r;
                     return {};
                 });
                 if (!st) return fail(st);
                 EntityId root = prefab::instanceOf(engine.scene(), *id);
                 size_t left = 0;
                 if (auto d = prefab::diff(engine.scene(), root)) left = d->count(false);
                 return ToolResult::json(Json::object({{"reverted", n}, {"remaining", left}, {"instance", root}}),
                                         "reverted " + std::to_string(n) + "; " + std::to_string(left) + " override(s) remain");
             }});

    reg.add({"prefab_apply", "Apply overrides to prefab",
             "Push an instance's overrides into its prefab file so every instance (in all scenes) gets them. Without "
             "`property`: everything — overrides, removed nodes and added entities (they become prefab nodes and stay "
             "linked). With `property`: only that override of `entity`. Placement (root name/transform) is never "
             "applied. Other instances in this scene update immediately and keep their own overrides. Undo restores the "
             "scene; the file keeps the new version (re-apply or edit to change it back).",
             "asset",
             object({{"entity", entity("Instance root or member")},
                     {"property", string("Apply only this override of `entity` (e.g. \"mesh.color\")")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 if (engine.playState() != PlayState::Editing) {
                     return ToolResult::error(Error::make("invalid_state", "stop the simulation before applying to a prefab",
                                                          "sim_control {\"action\": \"stop\"}"));
                 }
                 prefab::ApplyResult result;
                 std::string source;
                 Status st = engine.edit(ctx.actor, "Apply to prefab", [&]() -> Status {
                     auto writer = [&](const Json& doc) -> Result<std::shared_ptr<const PrefabTemplate>> {
                         EntityId root = prefab::instanceOf(engine.scene(), *id);
                         const EntityRecord* r = engine.scene().record(root);
                         source = r->prefab.source;
                         if (Status s = savePrefab(engine.resolvePath(source), doc); !s) return s.error();
                         return buildPrefabTemplate(doc, source, r->prefab.guid);
                     };
                     auto r = prefab::apply(engine.scene(), *id, a.get("property").asString(), writer);
                     if (!r) return r.error();
                     result = std::move(*r);
                     return {};
                 });
                 if (!st) return fail(st);
                 engine.refreshAssets();  // the asset database and caches see the new file (no re-expansion needed)
                 Json warnings = Json::array();
                 for (const auto& w : result.warnings) warnings.push(w);
                 return ToolResult::json(Json::object({{"prefab", source},
                                                       {"applied", result.applied},
                                                       {"instances_updated", result.instancesUpdated},
                                                       {"warnings", warnings}}),
                                         "applied " + std::to_string(result.applied) + " change(s) to " + source + "; " +
                                             std::to_string(result.instancesUpdated) +
                                             " other instance(s) in this scene updated (other scenes update when loaded)");
             }});

    reg.add({"prefab_unpack", "Unpack prefab instance",
             "Make an instance local: its entities stay exactly as they are but are no longer linked to the prefab (edits "
             "to the prefab stop reaching it, and the scene saves it in full); also known as \"make local\". Undoable.",
             "asset", object({{"entity", entity("Instance root or member")}}, {"entity"}), true, false,
             [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 EntityId root = prefab::instanceOf(engine.scene(), *id);
                 if (!root) {
                     return ToolResult::error(Error::make("not_linked", refText(engine.scene(), *id) + " is not part of a prefab instance"));
                 }
                 size_t n = 0;
                 Status st = engine.edit(ctx.actor, "Unpack prefab", [&]() -> Status {
                     n = prefab::unpack(engine.scene(), root);
                     return {};
                 });
                 if (!st) return fail(st);
                 return ToolResult::json(Json::object({{"instance", root}, {"unlinked", n}}),
                                         "unpacked " + refText(engine.scene(), root) + " (" + std::to_string(n) + " entities)");
             }});

    reg.add({"prefab_relink", "Relink prefab copies",
             "Link existing copies of a prefab (made before prefabs were linked, or by copying entities) back to it. A "
             "candidate links when its hierarchy matches the prefab node for node (same child names in order) and it has "
             "at most `max_overrides` differences besides placement; values are never changed (differences become "
             "overrides). Without `entities` the whole scene is searched. dry_run reports without linking.",
             "asset",
             object({{"prefab", string("Prefab path, e.g. prefabs/tree.prefab.json")},
                     {"entities", array(entity(), "Candidates (default: every unlinked entity)")},
                     {"max_overrides", integer("Most differences allowed (default 0: exact copies only)")},
                     {"dry_run", boolean("Only report what would be linked")}},
                    {"prefab"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto tmpl = engine.prefabTemplateAsset(a.get("prefab").asString());
                 if (!tmpl) return ToolResult::error(tmpl.error());
                 Scene& s = engine.scene();
                 std::vector<EntityId> candidates;
                 if (a.contains("entities")) {
                     for (const auto& e : a.get("entities").elements()) {
                         auto id = resolve(engine, e);
                         if (!id) return ToolResult::error(id.error());
                         candidates.push_back(*id);
                     }
                 } else {
                     candidates = s.entities();
                 }
                 size_t maxOverrides = static_cast<size_t>(std::max<int64_t>(0, a.get("max_overrides").asInt(0)));
                 bool dryRun = a.get("dry_run").asBool(false);
                 Json linked = Json::array(), skipped = Json::array();
                 std::ostringstream os;
                 Status st = engine.edit(ctx.actor, "Relink prefab copies", [&]() -> Status {
                     for (EntityId c : candidates) {
                         if (!s.exists(c) || s.record(c)->prefab.linked()) continue;
                         std::unordered_map<uint32_t, EntityId> members;
                         if (!prefab::match(s, c, **tmpl, members)) continue;
                         if (Status ls = prefab::link(s, c, *tmpl, members); !ls) return ls;
                         auto d = prefab::diff(s, c);
                         size_t n = d ? d->count(false) : SIZE_MAX;
                         Json j = Json::object({{"entity", c}, {"name", s.record(c)->name}, {"overrides", d ? Json(n) : Json()}});
                         if (n <= maxOverrides) {
                             linked.push(j);
                         } else {
                             skipped.push(j);
                             prefab::unpack(s, c);
                         }
                     }
                     if (dryRun) return Error::make("dry_run", "dry run");  // roll everything back
                     return {};
                 });
                 if (!st && !dryRun) return fail(st);
                 os << (dryRun ? "would link " : "linked ") << linked.size() << " cop" << (linked.size() == 1 ? "y" : "ies")
                    << " of " << (*tmpl)->source;
                 if (skipped.size()) {
                     os << "; " << skipped.size() << " matched the hierarchy but differ in more than " << maxOverrides
                        << " field(s) (raise max_overrides to link them; differences become overrides)";
                 }
                 return ToolResult::json(Json::object({{"prefab", (*tmpl)->source}, {"linked", linked}, {"skipped", skipped},
                                                       {"dry_run", dryRun}}),
                                         os.str());
             }});
}

}  // namespace sky::tools
