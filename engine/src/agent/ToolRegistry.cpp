#include "skywalker/agent/ToolRegistry.h"

#include <algorithm>
#include <exception>

#include "skywalker/core/Strings.h"

namespace sky {

ToolResult ToolResult::text(std::string t) {
    ToolResult r;
    r.content.push_back({ContentBlock::Type::Text, std::move(t), {}, {}});
    return r;
}

ToolResult ToolResult::json(Json payload, std::string summary) {
    ToolResult r;
    std::string body = payload.dump();
    r.content.push_back({ContentBlock::Type::Text, summary.empty() ? body : summary + "\n" + body, {}, {}});
    r.structured = std::move(payload);
    return r;
}

ToolResult ToolResult::error(const Error& e) {
    ToolResult r;
    std::string msg = "error [" + e.code + "]: " + e.message;
    if (!e.hint.empty()) msg += "\nhint: " + e.hint;
    r.content.push_back({ContentBlock::Type::Text, msg, {}, {}});
    r.structured = Json::object({{"error", e.code}, {"message", e.message}});
    if (!e.hint.empty()) r.structured["hint"] = e.hint;
    r.isError = true;
    return r;
}

ToolResult& ToolResult::image(std::string base64Png) {
    content.push_back({ContentBlock::Type::Image, {}, std::move(base64Png), "image/png"});
    return *this;
}

Json ToolResult::toMcp() const {
    Json blocks = Json::array();
    for (const auto& b : content) {
        if (b.type == ContentBlock::Type::Text) {
            blocks.push(Json::object({{"type", "text"}, {"text", b.text}}));
        } else {
            blocks.push(Json::object({{"type", "image"}, {"data", b.data}, {"mimeType", b.mimeType}}));
        }
    }
    Json out = Json::object({{"content", blocks}, {"isError", isError}});
    if (structured.isObject()) out["structuredContent"] = structured;
    return out;
}

void ToolRegistry::add(ToolDef def) { tools_.push_back(std::move(def)); }

const ToolDef* ToolRegistry::find(std::string_view name) const {
    for (const auto& t : tools_) {
        if (t.name == name) return &t;
    }
    return findDynamic(name);
}

const ToolDef* ToolRegistry::findDynamic(std::string_view name) const {
    std::lock_guard lock(dynamicMutex_);
    for (const auto& t : dynamic_) {
        if (t->name == name) return t.get();
    }
    return nullptr;
}

Status ToolRegistry::addDynamic(ToolDef def) {
    for (const auto& t : tools_) {
        if (t.name == def.name) return Error::make("name_taken", "'" + def.name + "' is a built-in tool", "pick another name");
    }
    std::lock_guard lock(dynamicMutex_);
    if (dynamic_.size() + retired_.size() >= kMaxDynamicDefinitions) {
        return Error::make("limit_reached", "too many dynamic tool definitions in this engine session",
                           "restart the engine, or re-register tools less often");
    }
    auto entry = std::make_shared<const ToolDef>(std::move(def));
    for (auto& t : dynamic_) {
        if (t->name == entry->name) {
            retired_.push_back(std::move(t));
            t = std::move(entry);
            return {};
        }
    }
    dynamic_.push_back(std::move(entry));
    return {};
}

bool ToolRegistry::removeDynamic(std::string_view name) {
    std::lock_guard lock(dynamicMutex_);
    for (auto it = dynamic_.begin(); it != dynamic_.end(); ++it) {
        if ((*it)->name == name) {
            retired_.push_back(std::move(*it));
            dynamic_.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<ToolDef> ToolRegistry::dynamicTools() const {
    std::lock_guard lock(dynamicMutex_);
    std::vector<ToolDef> out;
    out.reserve(dynamic_.size());
    for (const auto& t : dynamic_) out.push_back(*t);
    return out;
}

ToolResult ToolResult::defer(std::function<void()> work, std::function<ToolResult()> finish, std::function<void()> cancel) {
    ToolResult r;
    r.deferred = std::make_shared<DeferredWork>(DeferredWork{std::move(work), std::move(finish), std::move(cancel)});
    return r;
}

ToolResult ToolResult::complete() {
    if (!deferred) return std::move(*this);
    std::shared_ptr<DeferredWork> d = std::move(deferred);
    try {
        if (d->work) d->work();
        return d->finish ? d->finish() : ToolResult::text("");
    } catch (const std::exception& e) {
        return ToolResult::error(Error::make("internal_error", std::string("tool crashed: ") + e.what()));
    } catch (...) {
        return ToolResult::error(Error::make("internal_error", "tool crashed"));
    }
}

ToolResult ToolRegistry::call(std::string_view name, const Json& args, ToolContext& ctx) const {
    return invoke(name, args, ctx).complete();
}

ToolResult ToolRegistry::invoke(std::string_view name, const Json& args, ToolContext& ctx) const {
    const ToolDef* tool = find(name);
    if (!tool) {
        std::vector<std::string> names;
        for (const auto& t : tools_) names.push_back(t.name);
        for (const auto& t : dynamicTools()) names.push_back(t.name);
        std::string guess = str::closest(name, names, 4);
        return ToolResult::error(Error::make("unknown_tool", "no tool named '" + std::string(name) + "'",
                                             guess.empty() ? "call tools/list to see available tools"
                                                           : "did you mean '" + guess + "'?"));
    }
    Json normalized = args.isNull() ? Json::object() : args;
    if (Status s = validateSchema(tool->inputSchema, normalized); !s) return ToolResult::error(s.error());
    try {
        return tool->handler(normalized, ctx);
    } catch (const std::exception& e) {
        return ToolResult::error(Error::make("internal_error", std::string("tool crashed: ") + e.what()));
    } catch (...) {
        return ToolResult::error(Error::make("internal_error", "tool crashed"));
    }
}

Json ToolRegistry::listJson() const {
    Json list = Json::array();
    const std::vector<ToolDef> dynamic = dynamicTools();
    auto describe = [&list](const ToolDef& t) {
        list.push(Json::object({{"name", t.name},
                                {"title", t.title},
                                {"description", t.description},
                                {"inputSchema", t.inputSchema},
                                {"annotations", Json::object({{"title", t.title},
                                                              {"readOnlyHint", !t.mutates},
                                                              {"destructiveHint", t.destructive},
                                                              {"openWorldHint", t.openWorld}})},
                                // Lets clients group tools / grant permissions per category.
                                {"_meta", Json::object({{"skywalker/category", t.category}})}}));
    };
    for (const auto& t : tools_) describe(t);
    for (const auto& t : dynamic) describe(t);  // tools hosted by external processes (py_*)
    return Json::object({{"tools", list}});
}

std::string ToolRegistry::catalogueMarkdown() const {
    std::vector<std::string> categories;
    for (const auto& t : tools_) {
        if (std::find(categories.begin(), categories.end(), t.category) == categories.end()) categories.push_back(t.category);
    }
    std::string out = std::string("# Tool reference\n\nGenerated by `skywalker tools --markdown` (") + SKY_VERSION_STRING +
                      "). ✎ = modifies the scene (undoable, attributed to the caller).\n";
    for (const auto& category : categories) {
        out += "\n## " + category + "\n\n| Tool | Description |\n|---|---|\n";
        for (const auto& t : tools_) {
            if (t.category == category) out += "| `" + t.name + "`" + (t.mutates ? " ✎" : "") + " | " + t.description + " |\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Schema validation
// ---------------------------------------------------------------------------

namespace {

bool typeMatches(const std::string& type, const Json& v) {
    if (type == "object") return v.isObject();
    if (type == "array") return v.isArray();
    if (type == "string") return v.isString();
    if (type == "number") return v.isNumber();
    if (type == "integer") return v.isNumber() && static_cast<double>(v.asInt()) == v.asNumber();
    if (type == "boolean") return v.isBool();
    if (type == "null") return v.isNull();
    return true;
}

}  // namespace

Status validateSchema(const Json& schema, const Json& value, const std::string& path) {
    if (!schema.isObject()) return {};
    if (const Json* t = schema.find("type")) {
        bool ok = false;
        std::string expected;
        if (t->isString()) {
            ok = typeMatches(t->asString(), value);
            expected = t->asString();
        } else {
            for (const auto& alt : t->elements()) {
                ok = ok || typeMatches(alt.asString(), value);
                expected += (expected.empty() ? "" : " or ") + alt.asString();
            }
        }
        if (!ok) {
            return Error::make("invalid_arguments", path + " must be " + expected + ", got " + Json::typeName(value.type()));
        }
    }
    if (const Json* e = schema.find("enum")) {
        bool found = false;
        std::vector<std::string> options;
        for (const auto& opt : e->elements()) {
            found = found || opt == value;
            options.push_back(opt.asString());
        }
        if (!found) {
            std::string guess = value.isString() ? str::closest(value.asString(), options, 3) : "";
            std::string all;
            for (const auto& o : options) all += (all.empty() ? "" : ", ") + o;
            return Error::make("invalid_arguments", path + " must be one of: " + all,
                               guess.empty() ? "" : "did you mean \"" + guess + "\"?");
        }
    }
    if (value.isObject()) {
        const Json& props = schema.get("properties");
        for (const auto& req : schema.get("required").elements()) {
            if (!value.contains(req.asString())) {
                return Error::make("invalid_arguments", path + "." + req.asString() + " is required");
            }
        }
        bool closed = schema.get("additionalProperties").isBool() && !schema.get("additionalProperties").asBool();
        for (const auto& [key, v] : value.members()) {
            const Json* p = props.find(key);
            if (!p) {
                if (closed) {
                    std::vector<std::string> names;
                    for (const auto& [k, unused] : props.members()) names.push_back(k);
                    std::string guess = str::closest(key, names, 3);
                    return Error::make("invalid_arguments", path + " has no property \"" + key + "\"",
                                       guess.empty() ? "" : "did you mean \"" + guess + "\"?");
                }
                continue;
            }
            if (Status s = validateSchema(*p, v, path + "." + key); !s) return s;
        }
    }
    if (value.isArray()) {
        if (const Json* items = schema.find("items")) {
            for (size_t i = 0; i < value.size(); ++i) {
                if (Status s = validateSchema(*items, value[i], path + "[" + std::to_string(i) + "]"); !s) return s;
            }
        }
        if (const Json* mn = schema.find("minItems"); mn && value.size() < static_cast<size_t>(mn->asInt())) {
            return Error::make("invalid_arguments", path + " needs at least " + std::to_string(mn->asInt()) + " items");
        }
        if (const Json* mx = schema.find("maxItems"); mx && value.size() > static_cast<size_t>(mx->asInt())) {
            return Error::make("invalid_arguments", path + " allows at most " + std::to_string(mx->asInt()) + " items");
        }
    }
    return {};
}

namespace schema {

Json object(std::initializer_list<Json::Member> properties, std::initializer_list<const char*> required) {
    Json req = Json::array();
    for (const char* r : required) req.push(r);
    Json o = Json::object({{"type", "object"}, {"properties", Json(Json::Object(properties))}, {"additionalProperties", false}});
    if (req.size()) o["required"] = req;
    return o;
}
Json string(std::string d) { return Json::object({{"type", "string"}, {"description", std::move(d)}}); }
Json number(std::string d) { return Json::object({{"type", "number"}, {"description", std::move(d)}}); }
Json integer(std::string d) { return Json::object({{"type", "integer"}, {"description", std::move(d)}}); }
Json boolean(std::string d) { return Json::object({{"type", "boolean"}, {"description", std::move(d)}}); }
Json vec3(std::string d) {
    return Json::object({{"type", "array"},
                         {"items", Json::object({{"type", "number"}})},
                         {"minItems", 3},
                         {"maxItems", 3},
                         {"description", std::move(d)}});
}
Json enumeration(std::initializer_list<const char*> values, std::string d) {
    Json v = Json::array();
    for (const char* s : values) v.push(s);
    return Json::object({{"type", "string"}, {"enum", v}, {"description", std::move(d)}});
}
Json enumeration(const std::vector<std::string>& values, std::string d) {
    Json v = Json::array();
    for (const auto& s : values) v.push(s);
    return Json::object({{"type", "string"}, {"enum", v}, {"description", std::move(d)}});
}
Json any(std::string d) { return Json::object({{"description", std::move(d)}}); }
Json array(Json items, std::string d) {
    return Json::object({{"type", "array"}, {"items", std::move(items)}, {"description", std::move(d)}});
}
Json entity(std::string d) {
    return Json::object({{"type", Json::array({"integer", "string"})}, {"description", std::move(d)}});
}

}  // namespace schema
}  // namespace sky
