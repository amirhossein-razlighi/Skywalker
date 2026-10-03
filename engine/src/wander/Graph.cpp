// Wander code <-> Blueprint-style node graphs (see Graph.h for the JSON format).

#include "skywalker/wander/Graph.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "skywalker/core/Strings.h"
#include "skywalker/wander/Parser.h"

namespace sky::wander {

namespace {

constexpr double kCol = 300;   // horizontal step between statements
constexpr double kIndent = 40; // nested bodies start this far right of their parent
constexpr double kGap = 18;

// ---------------------------------------------------------------------------
// Pin / node helpers
// ---------------------------------------------------------------------------

Json pin(const std::string& name, const char* kind) { return Json::object({{"name", name}, {"kind", kind}}); }

double nodeHeight(const Json& n) {
    size_t rows = std::max(n.get("inputs").size(), n.get("outputs").size());
    return 34 + 22 * static_cast<double>(std::max<size_t>(rows, 1));
}

// Literals, names and short property paths stay inline on pins (`value`).
bool inlineable(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Number:
        case Expr::Kind::String:
        case Expr::Kind::Bool:
        case Expr::Kind::None:
        case Expr::Kind::Color:
        case Expr::Kind::Ident: return true;
        case Expr::Kind::Vector:
            return std::all_of(e.args.begin(), e.args.end(), [](const ExprPtr& a) { return a && a->kind == Expr::Kind::Number; });
        case Expr::Kind::Member: {
            const Expr* cur = e.lhs.get();
            int depth = 1;
            while (cur && cur->kind == Expr::Kind::Member) {
                cur = cur->lhs.get();
                ++depth;
            }
            return cur && cur->kind == Expr::Kind::Ident && depth <= 2;
        }
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// AST -> graph
// ---------------------------------------------------------------------------

class GraphWriter {
public:
    explicit GraphWriter(const Json& layout) : layout_(layout) {}

    Json handlerBody(const Handler& h, const std::string& id) {
        Json body = begin(id, "handler");
        body["trigger"] = h.custom ? h.argument : toString(h.trigger);
        if (!h.custom && !h.argument.empty()) body["argument"] = h.argument;
        if (!h.binding.empty()) body["binding"] = h.binding;
        if (h.custom) body["custom"] = true;
        std::string title = "on " + (h.custom ? h.argument : std::string(toString(h.trigger)));
        if (!h.custom && !h.argument.empty()) title += " \"" + h.argument + "\"";
        body["title"] = title;
        return finish(body, h.body, title, h.comments);
    }

    Json fnBody(const FnDecl& f, const std::string& id) {
        Json body = begin(id, "function");
        body["name"] = f.name;
        Json params = Json::array();
        std::string sig;
        for (const auto& p : f.params) {
            Json pj = Json::object({{"name", p.name}});
            if (!p.type.empty()) pj["type"] = p.type.text;
            params.push(pj);
            sig += (sig.empty() ? "" : ", ") + p.name;
        }
        body["params"] = params;
        if (!f.returns.empty()) body["returns"] = f.returns.text;
        body["title"] = "fn " + f.name + "(" + sig + ")";
        return finish(body, f.body, body["title"].asString(), f.comments);
    }

    Json testBody(const TestDecl& t, const std::string& id) {
        Json body = begin(id, "test");
        body["name"] = t.name;
        body["title"] = "test \"" + t.name + "\"";
        return finish(body, t.body, body["title"].asString(), t.comments);
    }

private:
    Json begin(const std::string& id, const char* kind) {
        nodes_ = Json::array();
        links_ = Json::array();
        bodyId_ = id;
        return Json::object({{"id", id}, {"kind", kind}});
    }

    Json finish(Json body, const Block& block, const std::string& title, const std::vector<std::string>& comments) {
        std::string entry = bodyId_ + "/entry";
        Json n = Json::object({{"id", entry}, {"type", "entry"}, {"title", title}});
        n["inputs"] = Json::array();
        n["outputs"] = Json::array({pin("then", "exec")});
        if (!comments.empty()) n["comment"] = commentsJson(comments);
        place(n, 0, 0);
        double h = nodeHeight(n);
        nodes_.push(n);
        layoutChain(block, bodyId_, entry, "then", kCol, 0);
        (void)h;
        body["entry"] = entry;
        body["nodes"] = nodes_;
        body["links"] = links_;
        return body;
    }

    static Json commentsJson(const std::vector<std::string>& cs) {
        Json a = Json::array();
        for (const auto& c : cs) a.push(c);
        return a;
    }

    void place(Json& n, double x, double y) {
        const std::string& id = n.get("id").asString();
        if (const Json* p = layout_.find(id); p && p->isArray() && p->size() == 2) {
            n["x"] = (*p)[0].asNumber();
            n["y"] = (*p)[1].asNumber();
        } else {
            n["x"] = x;
            n["y"] = y;
        }
    }

    void link(const std::string& from, const std::string& out, const std::string& to, const std::string& in) {
        links_.push(Json::object({{"from", from}, {"out", out}, {"to", to}, {"in", in}}));
    }

    // Lays out a statement chain starting at (x, y); returns {width, height}.
    std::pair<double, double> layoutChain(const Block& block, const std::string& path, std::string prev, std::string prevPin,
                                          double x, double y) {
        double cx = x;
        double bottom = y;
        for (size_t i = 0; i < block.size(); ++i) {
            const Stmt& s = *block[i];
            std::string id = path + "/s" + std::to_string(i);
            Json n = Json::object({{"id", id}});
            std::vector<std::pair<std::string, const Block*>> nested;
            Json props = Json::object();
            Json inputs = Json::array({pin("in", "exec")});
            Json outputs = Json::array();
            std::vector<std::pair<std::string, const Expr*>> dataIns;
            statementShape(s, n, props, dataIns, nested);
            for (const auto& [name, e] : dataIns) inputs.push(pin(name, "data"));
            for (const auto& [name, b] : nested) outputs.push(pin(name, "exec"));
            outputs.push(pin("next", "exec"));
            n["props"] = props;
            if (!s.comments.empty()) n["comment"] = commentsJson(s.comments);
            // Data inputs: inline values or expression nodes stacked below the statement.
            double ey = y;
            n["inputs"] = inputs;
            n["outputs"] = outputs;
            double h = nodeHeight(n);
            ey += h + kGap;
            Json& ins = n["inputs"];
            for (const auto& [name, e] : dataIns) {
                Json* p = nullptr;
                for (auto& el : ins.elements()) {
                    if (el.get("name").asString() == name) p = &el;
                }
                if (!e || !p) continue;
                if (inlineable(*e)) {
                    (*p)["value"] = formatExpr(*e);
                } else {
                    std::string eid = exprNode(*e, id + ":" + name, cx, ey, 0);
                    link(eid, "value", id, name);
                }
            }
            place(n, cx, y);
            nodes_.push(n);
            link(prev, prevPin, id, "in");
            double childY = ey;
            double childW = 0;
            for (const auto& [name, b] : nested) {
                auto [w, hh] = layoutChain(*b, id + "." + slug(name), id, name, cx + kIndent, childY);
                childY += std::max(hh, 0.0) + kGap * 2;
                childW = std::max(childW, w + kIndent);
            }
            bottom = std::max(bottom, std::max(childY, ey));
            cx += std::max(kCol, childW + kGap);
            prev = id;
            prevPin = "next";
        }
        return {cx - x, bottom - y};
    }

    static std::string slug(const std::string& s) {
        std::string out;
        for (char c : s) out += c == ' ' ? '_' : c;
        return out;
    }

    // Places an expression node (and its children); `ey` advances below it.
    std::string exprNode(const Expr& e, const std::string& id, double x, double& ey, int depth) {
        Json n = Json::object({{"id", id}});
        Json props = Json::object();
        std::vector<std::pair<std::string, const Expr*>> ins;
        switch (e.kind) {
            case Expr::Kind::Binary:
                n["type"] = "op";
                props["op"] = e.text;
                ins = {{"a", e.lhs.get()}, {"b", e.rhs.get()}};
                break;
            case Expr::Kind::Unary:
                n["type"] = e.text == "not" ? "not" : "neg";
                ins = {{"value", e.lhs.get()}};
                break;
            case Expr::Kind::Member:
                n["type"] = "member";
                props["name"] = e.text;
                ins = {{"object", e.lhs.get()}};
                break;
            case Expr::Kind::Index:
                n["type"] = "index";
                ins = {{"object", e.lhs.get()}, {"index", e.rhs.get()}};
                break;
            case Expr::Kind::Call:
                n["type"] = "call";
                props["function"] = e.text;
                for (size_t i = 0; i < e.args.size(); ++i) ins.emplace_back("arg" + std::to_string(i + 1), e.args[i].get());
                break;
            case Expr::Kind::MethodCall:
                n["type"] = "method";
                props["method"] = e.text;
                ins.emplace_back("object", e.lhs.get());
                for (size_t i = 0; i < e.args.size(); ++i) ins.emplace_back("arg" + std::to_string(i + 1), e.args[i].get());
                break;
            case Expr::Kind::List:
                n["type"] = "list";
                for (size_t i = 0; i < e.args.size(); ++i) ins.emplace_back("item" + std::to_string(i + 1), e.args[i].get());
                break;
            case Expr::Kind::Map: {
                n["type"] = "map";
                Json keys = Json::array();
                for (size_t i = 0; i < e.parts.size() && i < e.args.size(); ++i) {
                    keys.push(e.parts[i]);
                    ins.emplace_back("key:" + e.parts[i], e.args[i].get());
                }
                props["keys"] = keys;
                break;
            }
            case Expr::Kind::Vector: {
                n["type"] = "vector";
                static const char* names[] = {"x", "y", "z", "w"};
                for (size_t i = 0; i < e.args.size() && i < 4; ++i) ins.emplace_back(names[i], e.args[i].get());
                break;
            }
            case Expr::Kind::Interp: {
                n["type"] = "text";
                Json parts = Json::array();
                for (const auto& p : e.parts) parts.push(p);
                props["parts"] = parts;
                for (size_t i = 0; i < e.args.size(); ++i) ins.emplace_back("part" + std::to_string(i + 1), e.args[i].get());
                break;
            }
            default:
                n["type"] = "value";
                props["code"] = formatExpr(e);
                break;
        }
        if (e.parenthesized) props["grouped"] = true;
        n["props"] = props;
        Json inputs = Json::array();
        for (const auto& [name, child] : ins) inputs.push(pin(name, "data"));
        n["inputs"] = inputs;
        n["outputs"] = Json::array({pin("value", "data")});
        double myY = ey;
        ey += nodeHeight(n) + kGap;
        Json& inArr = n["inputs"];
        for (size_t i = 0; i < ins.size(); ++i) {
            const Expr* child = ins[i].second;
            if (!child) continue;
            if (inlineable(*child)) {
                inArr.elements()[i]["value"] = formatExpr(*child);
            } else {
                std::string cid = exprNode(*child, id + "." + ins[i].first, x - 30, ey, depth + 1);
                link(cid, "value", id, ins[i].first);
            }
        }
        place(n, x - 30.0 * depth - 230, myY);
        nodes_.push(n);
        return id;
    }

    void statementShape(const Stmt& s, Json& n, Json& props, std::vector<std::pair<std::string, const Expr*>>& ins,
                        std::vector<std::pair<std::string, const Block*>>& nested) {
        auto type = [&](const char* t) { n["type"] = t; };
        switch (s.kind) {
            case Stmt::Kind::Let:
            case Stmt::Kind::Const:
                type("let");
                props["name"] = s.name;
                if (!s.type.empty()) props["type"] = s.type.text;
                if (s.kind == Stmt::Kind::Const) props["const"] = true;
                ins = {{"value", s.value.get()}};
                return;
            case Stmt::Kind::Assign:
                type("set");
                props["target"] = s.target ? formatExpr(*s.target) : "";
                ins = {{"value", s.value.get()}};
                return;
            case Stmt::Kind::OpAssign:
                type("set");
                props["target"] = s.target ? formatExpr(*s.target) : "";
                props["op"] = s.name;
                ins = {{"value", s.value.get()}};
                return;
            case Stmt::Kind::If: {
                type("if");
                size_t conds = 0;
                bool hasElse = false;
                for (size_t i = 0; i < s.branches.size(); ++i) {
                    const auto& [cond, body] = s.branches[i];
                    if (!cond) {
                        hasElse = true;
                        nested.emplace_back("else", &body);
                        continue;
                    }
                    std::string inName = conds == 0 ? "if" : "elif " + std::to_string(conds);
                    std::string outName = conds == 0 ? "then" : "elif " + std::to_string(conds);
                    ins.emplace_back(inName, cond.get());
                    nested.emplace_back(outName, &body);
                    ++conds;
                }
                // Keep "else" last among the nested outputs.
                std::stable_partition(nested.begin(), nested.end(), [](const auto& p) { return p.first != "else"; });
                props["branches"] = static_cast<double>(conds);
                props["else"] = hasElse;
                return;
            }
            case Stmt::Kind::While:
                type("while");
                ins = {{"condition", s.value.get()}};
                nested.emplace_back("body", &s.body);
                return;
            case Stmt::Kind::For:
                type("for");
                props["var"] = s.name;
                if (!s.name2.empty()) props["var2"] = s.name2;
                props["range"] = s.isRange;
                if (s.isRange) {
                    if (s.inclusive) props["inclusive"] = true;
                    ins = {{"from", s.value.get()}, {"to", s.extra.get()}};
                    if (s.extra2) ins.emplace_back("step", s.extra2.get());
                } else {
                    ins = {{"items", s.value.get()}};
                }
                nested.emplace_back("body", &s.body);
                return;
            case Stmt::Kind::Repeat:
                type("repeat");
                ins = {{"times", s.value.get()}};
                nested.emplace_back("body", &s.body);
                return;
            case Stmt::Kind::Every:
            case Stmt::Kind::After:
                type(s.kind == Stmt::Kind::Every ? "every" : "after");
                ins = {{"seconds", s.value.get()}};
                nested.emplace_back("body", &s.body);
                return;
            case Stmt::Kind::Wait:
                type("wait");
                props["mode"] = s.waitKind == WaitKind::Until ? "until" : s.waitKind == WaitKind::Frames ? "frames" : "seconds";
                ins = {{s.waitKind == WaitKind::Until ? "condition" : "amount", s.value.get()}};
                return;
            case Stmt::Kind::Break: type("break"); return;
            case Stmt::Kind::Continue: type("continue"); return;
            case Stmt::Kind::Stop: type("stop"); return;
            case Stmt::Kind::Return:
                type("return");
                if (s.value) ins = {{"value", s.value.get()}};
                return;
            case Stmt::Kind::GoTo:
                type("go_to");
                props["state"] = s.name;
                return;
            case Stmt::Kind::Move:
                type("move");
                ins = {{"entity", s.target.get()}, {"offset", s.value.get()}};
                return;
            case Stmt::Kind::MoveToward:
                type("move_toward");
                ins = {{"entity", s.target.get()}, {"destination", s.value.get()}, {"speed", s.extra.get()}};
                return;
            case Stmt::Kind::Rotate:
                type("rotate");
                ins = {{"entity", s.target.get()}, {"degrees", s.value.get()}};
                return;
            case Stmt::Kind::Look:
                type("look");
                ins = {{"entity", s.target.get()}, {"point", s.value.get()}};
                return;
            case Stmt::Kind::Emit:
                type("emit");
                props["event"] = s.name;
                if (s.value) ins.emplace_back("payload", s.value.get());
                if (s.extra) ins.emplace_back("receiver", s.extra.get());
                return;
            case Stmt::Kind::Destroy:
                type("destroy");
                ins = {{"entity", s.target.get()}};
                return;
            case Stmt::Kind::Log:
                type("log");
                ins = {{"value", s.value.get()}};
                return;
            case Stmt::Kind::Call:
                type("call");
                ins = {{"call", s.value.get()}};
                return;
            case Stmt::Kind::Expect:
                type("expect");
                ins = {{"condition", s.value.get()}};
                if (s.extra) ins.emplace_back("message", s.extra.get());
                return;
            case Stmt::Kind::Press: type("press"); ins = {{"key", s.value.get()}}; return;
            case Stmt::Kind::Hold: type("hold"); ins = {{"key", s.value.get()}}; return;
            case Stmt::Kind::Release: type("release"); ins = {{"key", s.value.get()}}; return;
            case Stmt::Kind::Click: type("click"); ins = {{"entity", s.value.get()}}; return;
        }
    }

    const Json& layout_;
    Json nodes_;
    Json links_;
    std::string bodyId_;
};

Json varJson(const VarDecl& v) {
    Json j = Json::object({{"name", v.name}, {"value", v.initial ? formatExpr(*v.initial) : "none"}});
    if (!v.type.empty()) j["type"] = v.type.text;
    if (v.isParam) {
        j["param"] = true;
        if (v.minValue) j["min"] = formatExpr(*v.minValue);
        if (v.maxValue) j["max"] = formatExpr(*v.maxValue);
        if (!v.doc.empty()) j["doc"] = v.doc;
    }
    if (!v.comments.empty()) {
        Json c = Json::array();
        for (const auto& s : v.comments) c.push(s);
        j["comment"] = c;
    }
    return j;
}

Json constJson(const ConstDecl& c) {
    Json j = Json::object({{"name", c.name}, {"value", c.value ? formatExpr(*c.value) : "none"}});
    if (!c.type.empty()) j["type"] = c.type.text;
    if (!c.comments.empty()) {
        Json a = Json::array();
        for (const auto& s : c.comments) a.push(s);
        j["comment"] = a;
    }
    return j;
}

// ---------------------------------------------------------------------------
// Graph -> AST
// ---------------------------------------------------------------------------

class GraphReader {
public:
    Result<Block> body(const Json& b) {
        nodes_.clear();
        links_.clear();
        execOut_.clear();
        dataIn_.clear();
        steps_ = 0;
        bodyId_ = b.get("id").asString();
        for (const auto& n : b.get("nodes").elements()) {
            std::string id = n.get("id").asString();
            if (id.empty()) return fail("a node in " + where() + " has no id");
            if (nodes_.count(id)) return fail("two nodes in " + where() + " share the id \"" + id + "\"");
            nodes_[id] = &n;
        }
        for (const auto& l : b.get("links").elements()) {
            std::string from = l.get("from").asString(), out = l.get("out").asString();
            std::string to = l.get("to").asString(), in = l.get("in").asString();
            if (!nodes_.count(from) || !nodes_.count(to)) {
                return fail("a wire in " + where() + " connects a missing node (" + from + " -> " + to + ")");
            }
            if (isExecPin(*nodes_[from], out, true)) {
                if (execOut_.count({from, out})) {
                    return fail("exec output \"" + out + "\" of node " + from + " has two wires (exec flow must be a single path)");
                }
                execOut_[{from, out}] = to;
            } else {
                if (dataIn_.count({to, in})) return fail("input \"" + in + "\" of node " + to + " has two wires");
                dataIn_[{to, in}] = from;
            }
            links_.push_back(&l);
        }
        std::string entry = b.get("entry").asString();
        if (entry.empty() || !nodes_.count(entry)) {
            for (const auto& [id, n] : nodes_) {
                if (n->get("type").asString() == "entry") entry = id;
            }
        }
        if (entry.empty()) return fail(where() + " has no entry node");
        error_.clear();
        Block out = chain(entry, "then");
        if (!error_.empty()) return Error::make("invalid_graph", error_);
        return out;
    }

    std::string error_;

private:
    Result<Block> fail(std::string msg) { return Error::make("invalid_graph", std::move(msg)); }
    std::string where() const { return "body " + bodyId_; }

    static bool isExecPin(const Json& node, const std::string& name, bool output) {
        for (const auto& p : node.get(output ? "outputs" : "inputs").elements()) {
            if (p.get("name").asString() == name) return p.get("kind").asString() == "exec";
        }
        // Unknown pins: statement outputs are exec, expression outputs are data.
        return output && name != "value";
    }

    void err(const std::string& msg) {
        if (error_.empty()) error_ = msg;
    }

    Block chain(const std::string& from, const std::string& pinName) {
        Block out;
        std::set<std::string> visited;
        auto it = execOut_.find({from, pinName});
        std::string cur = it == execOut_.end() ? "" : it->second;
        while (!cur.empty() && error_.empty()) {
            if (!visited.insert(cur).second || ++steps_ > 100000) {
                err("exec wires form a loop at node " + cur + " (use a while/for node for loops)");
                break;
            }
            const Json& n = *nodes_.at(cur);
            StmtPtr s = statement(n, cur);
            if (!s) break;
            out.push_back(std::move(s));
            auto next = execOut_.find({cur, "next"});
            cur = next == execOut_.end() ? "" : next->second;
        }
        return out;
    }

    const Json* inputPin(const Json& n, const std::string& name) {
        for (const auto& p : n.get("inputs").elements()) {
            if (p.get("name").asString() == name) return &p;
        }
        return nullptr;
    }

    ExprPtr parseCode(const std::string& code, const std::string& what) {
        std::vector<Diagnostic> diags;
        ExprPtr e = parseExpression(code, &diags);
        if (!e) {
            std::string msg = diags.empty() ? "invalid expression" : diags.front().message;
            err(what + ": cannot read \"" + code + "\": " + msg);
        }
        return e;
    }

    // Value of a data input: the wired expression node, or the inline value.
    ExprPtr input(const Json& n, const std::string& id, const std::string& name, bool optional = false) {
        if (auto it = dataIn_.find({id, name}); it != dataIn_.end()) return expression(it->second, 0);
        const Json* p = inputPin(n, name);
        std::string code = p ? p->get("value").asString() : "";
        if (str::trim(code).empty()) {
            if (!optional) err("input \"" + name + "\" of node " + id + " (" + n.get("type").asString() + ") is empty");
            return nullptr;
        }
        return parseCode(code, "input \"" + name + "\" of node " + id);
    }

    ExprPtr expression(const std::string& id, int depth) {
        if (depth > 200) {
            err("data wires nest too deeply (a cycle?) at node " + id);
            return nullptr;
        }
        const Json& n = *nodes_.at(id);
        const std::string type = n.get("type").asString();
        const Json& props = n.get("props");
        SourceLoc loc;
        auto in = [&](const std::string& name) { return inputOf(n, id, name, depth); };
        ExprPtr e;
        if (type == "op") {
            e = std::make_unique<Expr>(Expr::Kind::Binary, loc);
            e->text = props.get("op").asString();
            static const std::set<std::string> ops{"+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!=", "and", "or", "in"};
            if (!ops.count(e->text)) err("node " + id + " has an unknown operator \"" + e->text + "\"");
            e->lhs = in("a");
            e->rhs = in("b");
        } else if (type == "not" || type == "neg") {
            e = std::make_unique<Expr>(Expr::Kind::Unary, loc);
            e->text = type == "not" ? "not" : "-";
            e->lhs = in("value");
        } else if (type == "member") {
            e = std::make_unique<Expr>(Expr::Kind::Member, loc);
            e->text = props.get("name").asString();
            e->lhs = in("object");
        } else if (type == "index") {
            e = std::make_unique<Expr>(Expr::Kind::Index, loc);
            e->lhs = in("object");
            e->rhs = in("index");
        } else if (type == "call" || type == "method") {
            e = std::make_unique<Expr>(type == "call" ? Expr::Kind::Call : Expr::Kind::MethodCall, loc);
            e->text = props.get(type == "call" ? "function" : "method").asString();
            if (e->text.empty()) err("node " + id + " has no " + std::string(type == "call" ? "function" : "method") + " name");
            if (type == "method") e->lhs = in("object");
            for (const auto& name : argPins(n, "arg")) e->args.push_back(in(name));
        } else if (type == "list") {
            e = std::make_unique<Expr>(Expr::Kind::List, loc);
            for (const auto& name : argPins(n, "item")) e->args.push_back(in(name));
        } else if (type == "map") {
            e = std::make_unique<Expr>(Expr::Kind::Map, loc);
            for (const auto& k : props.get("keys").elements()) {
                e->parts.push_back(k.asString());
                e->args.push_back(in("key:" + k.asString()));
            }
        } else if (type == "vector") {
            e = std::make_unique<Expr>(Expr::Kind::Vector, loc);
            for (const char* c : {"x", "y", "z", "w"}) {
                if (inputPin(n, c) || dataIn_.count({id, c})) e->args.push_back(in(c));
            }
        } else if (type == "text") {
            e = std::make_unique<Expr>(Expr::Kind::Interp, loc);
            for (const auto& p : props.get("parts").elements()) e->parts.push_back(p.asString());
            for (const auto& name : argPins(n, "part")) e->args.push_back(in(name));
            if (e->parts.size() != e->args.size() + 1) err("text node " + id + " needs one more text part than inputs");
        } else if (type == "value") {
            e = parseCode(props.get("code").asString(), "node " + id);
        } else {
            err("node " + id + " has unknown expression type \"" + type + "\"");
            return nullptr;
        }
        if (e && props.get("grouped").asBool()) e->parenthesized = true;
        return e;
    }

    ExprPtr inputOf(const Json& n, const std::string& id, const std::string& name, int depth) {
        if (auto it = dataIn_.find({id, name}); it != dataIn_.end()) return expression(it->second, depth + 1);
        return input(n, id, name);
    }

    // Pins named prefix1, prefix2, ... in order (declared pins and wired inputs).
    std::vector<std::string> argPins(const Json& n, const std::string& prefix) {
        std::vector<std::pair<int, std::string>> found;
        auto consider = [&](const std::string& name) {
            if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
                int k = std::atoi(name.c_str() + prefix.size());
                if (k > 0 && std::none_of(found.begin(), found.end(), [&](const auto& f) { return f.second == name; })) {
                    found.emplace_back(k, name);
                }
            }
        };
        for (const auto& p : n.get("inputs").elements()) consider(p.get("name").asString());
        std::sort(found.begin(), found.end());
        std::vector<std::string> out;
        for (const auto& f : found) out.push_back(f.second);
        return out;
    }

    StmtPtr statement(const Json& n, const std::string& id) {
        const std::string type = n.get("type").asString();
        const Json& props = n.get("props");
        SourceLoc loc;
        auto make = [&](Stmt::Kind k) {
            auto s = std::make_unique<Stmt>(k, loc);
            for (const auto& c : n.get("comment").elements()) s->comments.push_back(c.asString());
            return s;
        };
        auto in = [&](const std::string& name) { return input(n, id, name); };
        auto nestedChain = [&](const std::string& pinName) { return chain(id, pinName); };
        if (type == "let") {
            auto s = make(props.get("const").asBool() ? Stmt::Kind::Const : Stmt::Kind::Let);
            s->name = props.get("name").asString();
            if (s->name.empty()) err("let node " + id + " needs a name");
            s->type.text = props.get("type").asString();
            s->value = in("value");
            return s;
        }
        if (type == "set") {
            std::string op = props.get("op").asString();
            auto s = make(op.empty() ? Stmt::Kind::Assign : Stmt::Kind::OpAssign);
            s->name = op;
            if (!op.empty() && op != "+" && op != "-" && op != "*" && op != "/") err("set node " + id + " has an unknown op \"" + op + "\"");
            s->target = parseCode(props.get("target").asString(), "the target of set node " + id);
            s->value = in("value");
            return s;
        }
        if (type == "if") {
            auto s = make(Stmt::Kind::If);
            int branches = static_cast<int>(props.get("branches").asNumber(1));
            for (int i = 0; i < branches; ++i) {
                std::string inName = i == 0 ? "if" : "elif " + std::to_string(i);
                std::string outName = i == 0 ? "then" : "elif " + std::to_string(i);
                ExprPtr cond = in(inName);
                Block body = nestedChain(outName);
                s->branches.emplace_back(std::move(cond), std::move(body));
            }
            if (props.get("else").asBool()) s->branches.emplace_back(nullptr, nestedChain("else"));
            return s;
        }
        if (type == "while" || type == "repeat" || type == "every" || type == "after") {
            auto s = make(type == "while"    ? Stmt::Kind::While
                          : type == "repeat" ? Stmt::Kind::Repeat
                          : type == "every"  ? Stmt::Kind::Every
                                             : Stmt::Kind::After);
            s->value = in(type == "while" ? "condition" : type == "repeat" ? "times" : "seconds");
            s->body = nestedChain("body");
            return s;
        }
        if (type == "for") {
            auto s = make(Stmt::Kind::For);
            s->name = props.get("var").asString();
            s->name2 = props.get("var2").asString();
            if (s->name.empty()) err("for node " + id + " needs a loop variable name");
            s->isRange = props.get("range").asBool();
            if (s->isRange) {
                s->inclusive = props.get("inclusive").asBool();
                s->value = in("from");
                s->extra = in("to");
                s->extra2 = input(n, id, "step", true);
            } else {
                s->value = in("items");
            }
            s->body = nestedChain("body");
            return s;
        }
        if (type == "wait") {
            auto s = make(Stmt::Kind::Wait);
            std::string mode = props.get("mode").asString("seconds");
            s->waitKind = mode == "until" ? WaitKind::Until : mode == "frames" ? WaitKind::Frames : WaitKind::Seconds;
            s->value = in(mode == "until" ? "condition" : "amount");
            return s;
        }
        if (type == "break") return make(Stmt::Kind::Break);
        if (type == "continue") return make(Stmt::Kind::Continue);
        if (type == "stop") return make(Stmt::Kind::Stop);
        if (type == "return") {
            auto s = make(Stmt::Kind::Return);
            s->value = input(n, id, "value", true);
            return s;
        }
        if (type == "go_to") {
            auto s = make(Stmt::Kind::GoTo);
            s->name = props.get("state").asString();
            if (s->name.empty()) err("go_to node " + id + " needs a state name");
            return s;
        }
        if (type == "move" || type == "rotate" || type == "look") {
            auto s = make(type == "move" ? Stmt::Kind::Move : type == "rotate" ? Stmt::Kind::Rotate : Stmt::Kind::Look);
            s->target = in("entity");
            s->value = in(type == "move" ? "offset" : type == "rotate" ? "degrees" : "point");
            return s;
        }
        if (type == "move_toward") {
            auto s = make(Stmt::Kind::MoveToward);
            s->target = in("entity");
            s->value = in("destination");
            s->extra = in("speed");
            return s;
        }
        if (type == "emit") {
            auto s = make(Stmt::Kind::Emit);
            s->name = props.get("event").asString();
            if (s->name.empty()) err("emit node " + id + " needs an event name");
            s->value = input(n, id, "payload", true);
            s->extra = input(n, id, "receiver", true);
            return s;
        }
        if (type == "destroy") {
            auto s = make(Stmt::Kind::Destroy);
            s->target = in("entity");
            return s;
        }
        if (type == "log") {
            auto s = make(Stmt::Kind::Log);
            s->value = in("value");
            return s;
        }
        if (type == "call") {
            auto s = make(Stmt::Kind::Call);
            s->value = in("call");
            if (s->value && s->value->kind != Expr::Kind::Call && s->value->kind != Expr::Kind::MethodCall) {
                err("call node " + id + " must be wired to a function or method call");
            }
            return s;
        }
        if (type == "expect") {
            auto s = make(Stmt::Kind::Expect);
            s->value = in("condition");
            s->extra = input(n, id, "message", true);
            return s;
        }
        if (type == "press" || type == "hold" || type == "release") {
            auto s = make(type == "press" ? Stmt::Kind::Press : type == "hold" ? Stmt::Kind::Hold : Stmt::Kind::Release);
            s->value = in("key");
            return s;
        }
        if (type == "click") {
            auto s = make(Stmt::Kind::Click);
            s->value = in("entity");
            return s;
        }
        err("node " + id + " has unknown statement type \"" + type + "\"");
        return nullptr;
    }

    std::map<std::string, const Json*> nodes_;
    std::vector<const Json*> links_;
    std::map<std::pair<std::string, std::string>, std::string> execOut_;
    std::map<std::pair<std::string, std::string>, std::string> dataIn_;
    std::string bodyId_;
    int steps_ = 0;
};

std::vector<std::string> commentsOf(const Json& j) {
    std::vector<std::string> out;
    for (const auto& c : j.get("comment").elements()) out.push_back(c.asString());
    return out;
}

// Comments on a body's entry node belong to the handler/fn/test declaration.
std::vector<std::string> commentsOfEntry(const Json& body) {
    std::string entry = body.get("entry").asString();
    for (const auto& n : body.get("nodes").elements()) {
        if (n.get("id").asString() == entry) return commentsOf(n);
    }
    return {};
}

ExprPtr codeExpr(const Json& j, const std::string& what, std::string& error) {
    std::string code = j.asString();
    if (str::trim(code).empty()) code = "none";
    std::vector<Diagnostic> diags;
    ExprPtr e = parseExpression(code, &diags);
    if (!e && error.empty()) error = what + ": cannot read \"" + code + "\"" + (diags.empty() ? "" : ": " + diags.front().message);
    return e;
}

}  // namespace

Json toGraph(const Module& m, const Json& layout) {
    GraphWriter w(layout);
    Json g = Json::object({{"format", "wander-graph"}, {"version", 1}});
    Json uses = Json::array();
    for (const auto& u : m.uses) uses.push(Json::object({{"path", u.path}, {"alias", u.alias}}));
    g["uses"] = uses;
    Json consts = Json::array();
    for (const auto& c : m.consts) consts.push(constJson(c));
    g["consts"] = consts;
    Json fns = Json::array();
    for (size_t i = 0; i < m.fns.size(); ++i) fns.push(w.fnBody(m.fns[i], "f" + std::to_string(i)));
    g["functions"] = fns;
    Json behaviors = Json::array();
    for (size_t bi = 0; bi < m.behaviors.size(); ++bi) {
        const BehaviorDef& b = m.behaviors[bi];
        std::string bid = "b" + std::to_string(bi);
        Json bj = Json::object({{"name", b.name}, {"intent", b.intent}, {"implicit", b.implicit}});
        Json vars = Json::array();
        for (const auto& v : b.vars) vars.push(varJson(v));
        bj["vars"] = vars;
        Json bconsts = Json::array();
        for (const auto& c : b.consts) bconsts.push(constJson(c));
        bj["consts"] = bconsts;
        Json bfns = Json::array();
        for (size_t i = 0; i < b.fns.size(); ++i) bfns.push(w.fnBody(b.fns[i], bid + ".f" + std::to_string(i)));
        bj["functions"] = bfns;
        Json handlers = Json::array();
        for (size_t i = 0; i < b.handlers.size(); ++i) handlers.push(w.handlerBody(b.handlers[i], bid + ".h" + std::to_string(i)));
        bj["handlers"] = handlers;
        Json states = Json::array();
        for (size_t si = 0; si < b.states.size(); ++si) {
            const StateDecl& st = b.states[si];
            Json sh = Json::array();
            for (size_t i = 0; i < st.handlers.size(); ++i) {
                sh.push(w.handlerBody(st.handlers[i], bid + ".s" + std::to_string(si) + ".h" + std::to_string(i)));
            }
            Json sj = Json::object({{"name", st.name}, {"handlers", sh}});
            if (!st.comments.empty()) {
                Json c = Json::array();
                for (const auto& s : st.comments) c.push(s);
                sj["comment"] = c;
            }
            states.push(sj);
        }
        bj["states"] = states;
        Json tests = Json::array();
        for (size_t i = 0; i < b.tests.size(); ++i) tests.push(w.testBody(b.tests[i], bid + ".t" + std::to_string(i)));
        bj["tests"] = tests;
        if (!b.comments.empty()) {
            Json c = Json::array();
            for (const auto& s : b.comments) c.push(s);
            bj["comment"] = c;
        }
        behaviors.push(bj);
    }
    g["behaviors"] = behaviors;
    Json tests = Json::array();
    for (size_t i = 0; i < m.tests.size(); ++i) tests.push(w.testBody(m.tests[i], "t" + std::to_string(i)));
    g["tests"] = tests;
    if (!m.trailingComments.empty()) {
        Json c = Json::array();
        for (const auto& s : m.trailingComments) c.push(s);
        g["comment"] = c;
    }
    return g;
}

Result<std::string> fromGraph(const Json& g) {
    if (!g.isObject()) return Error::make("invalid_graph", "a graph must be a JSON object (see behavior_graph)");
    if (g.contains("format") && g.get("format").asString() != "wander-graph") {
        return Error::make("invalid_graph", "unknown graph format \"" + g.get("format").asString() + "\"");
    }
    Module m;
    std::string error;
    GraphReader reader;

    auto fnFrom = [&](const Json& j) -> Result<FnDecl> {
        FnDecl f;
        f.name = j.get("name").asString();
        if (f.name.empty()) return Error::make("invalid_graph", "function body " + j.get("id").asString() + " has no name");
        for (const auto& p : j.get("params").elements()) {
            Param pa;
            pa.name = p.isString() ? p.asString() : p.get("name").asString();
            pa.type.text = p.get("type").asString();
            f.params.push_back(std::move(pa));
        }
        f.returns.text = j.get("returns").asString();
        auto body = reader.body(j);
        if (!body) return body.error();
        f.body = std::move(body.value());
        f.comments = commentsOfEntry(j);
        return f;
    };
    auto handlerFrom = [&](const Json& j) -> Result<Handler> {
        Handler h;
        std::string t = j.get("trigger").asString("tick");
        h.custom = j.get("custom").asBool();
        if (h.custom) {
            h.trigger = Trigger::Event;
            h.argument = t;
        } else if (t == "start") h.trigger = Trigger::Start;
        else if (t == "tick") h.trigger = Trigger::Tick;
        else if (t == "event") h.trigger = Trigger::Event;
        else if (t == "key") h.trigger = Trigger::Key;
        else if (t == "click") h.trigger = Trigger::Click;
        else if (t == "enter") h.trigger = Trigger::Enter;
        else if (t == "exit") h.trigger = Trigger::Exit;
        else {
            h.trigger = Trigger::Event;
            h.custom = true;
            h.argument = t;
        }
        if (!h.custom && (h.trigger == Trigger::Event || h.trigger == Trigger::Key)) {
            h.argument = j.get("argument").asString();
            if (h.argument.empty()) {
                return Error::make("invalid_graph", "handler " + j.get("id").asString() + " (on " + t + ") needs an \"argument\"");
            }
        }
        h.binding = j.get("binding").asString();
        auto body = reader.body(j);
        if (!body) return body.error();
        h.body = std::move(body.value());
        h.comments = commentsOfEntry(j);
        return h;
    };
    auto testFrom = [&](const Json& j) -> Result<TestDecl> {
        TestDecl t;
        t.name = j.get("name").asString();
        auto body = reader.body(j);
        if (!body) return body.error();
        t.body = std::move(body.value());
        t.comments = commentsOfEntry(j);
        return t;
    };
    auto constFrom = [&](const Json& j) {
        ConstDecl c;
        c.name = j.get("name").asString();
        c.type.text = j.get("type").asString();
        c.value = codeExpr(j.get("value"), "const " + c.name, error);
        c.comments = commentsOf(j);
        return c;
    };

    for (const auto& u : g.get("uses").elements()) {
        UseDecl d;
        d.path = u.get("path").asString();
        d.alias = u.get("alias").asString();
        m.uses.push_back(std::move(d));
    }
    for (const auto& c : g.get("consts").elements()) m.consts.push_back(constFrom(c));
    for (const auto& f : g.get("functions").elements()) {
        auto fn = fnFrom(f);
        if (!fn) return fn.error();
        m.fns.push_back(std::move(fn.value()));
    }
    for (const auto& bj : g.get("behaviors").elements()) {
        BehaviorDef b;
        b.name = bj.get("name").asString("Main");
        b.intent = bj.get("intent").asString();
        b.implicit = bj.get("implicit").asBool();
        b.comments = commentsOf(bj);
        for (const auto& v : bj.get("vars").elements()) {
            VarDecl d;
            d.name = v.get("name").asString();
            d.type.text = v.get("type").asString();
            d.isParam = v.get("param").asBool();
            d.initial = codeExpr(v.get("value"), "var " + d.name, error);
            if (d.isParam && v.contains("min") && v.contains("max")) {
                d.minValue = codeExpr(v.get("min"), "param " + d.name + " min", error);
                d.maxValue = codeExpr(v.get("max"), "param " + d.name + " max", error);
            }
            d.doc = v.get("doc").asString();
            d.comments = commentsOf(v);
            b.vars.push_back(std::move(d));
        }
        for (const auto& c : bj.get("consts").elements()) b.consts.push_back(constFrom(c));
        for (const auto& f : bj.get("functions").elements()) {
            auto fn = fnFrom(f);
            if (!fn) return fn.error();
            b.fns.push_back(std::move(fn.value()));
        }
        for (const auto& h : bj.get("handlers").elements()) {
            auto hd = handlerFrom(h);
            if (!hd) return hd.error();
            b.handlers.push_back(std::move(hd.value()));
        }
        for (const auto& sj : bj.get("states").elements()) {
            StateDecl st;
            st.name = sj.get("name").asString();
            st.comments = commentsOf(sj);
            for (const auto& h : sj.get("handlers").elements()) {
                auto hd = handlerFrom(h);
                if (!hd) return hd.error();
                st.handlers.push_back(std::move(hd.value()));
            }
            b.states.push_back(std::move(st));
        }
        for (const auto& t : bj.get("tests").elements()) {
            auto td = testFrom(t);
            if (!td) return td.error();
            b.tests.push_back(std::move(td.value()));
        }
        m.behaviors.push_back(std::move(b));
    }
    for (const auto& t : g.get("tests").elements()) {
        auto td = testFrom(t);
        if (!td) return td.error();
        m.tests.push_back(std::move(td.value()));
    }
    m.trailingComments = commentsOf(g);
    if (!error.empty()) return Error::make("invalid_graph", error);
    return format(m);
}

Json graphLayout(const Json& graph) {
    Json out = Json::object();
    std::function<void(const Json&)> visit = [&](const Json& j) {
        if (j.isObject()) {
            if (j.contains("nodes") && j.get("nodes").isArray()) {
                for (const auto& n : j.get("nodes").elements()) {
                    if (n.contains("x") && n.contains("y")) {
                        out[n.get("id").asString()] = Json::array({std::round(n.get("x").asNumber()), std::round(n.get("y").asNumber())});
                    }
                }
            }
            for (const auto& [k, v] : j.members()) {
                if (k != "nodes") visit(v);
            }
        } else if (j.isArray()) {
            for (const auto& v : j.elements()) visit(v);
        }
    };
    visit(graph);
    return out;
}

Json graphPalette() {
    auto node = [](const char* type, const char* group, const char* title, std::initializer_list<const char*> inputs,
                   std::initializer_list<const char*> outputs, const char* doc, Json props = Json::object()) {
        Json ins = Json::array(), outs = Json::array();
        bool statement = std::string(group) != "expression";
        if (statement) ins.push(pin("in", "exec"));
        for (const char* i : inputs) ins.push(pin(i, "data"));
        for (const char* o : outputs) outs.push(pin(o, statement ? "exec" : "data"));
        if (statement) outs.push(pin("next", "exec"));
        return Json::object({{"type", type}, {"group", group}, {"title", title}, {"inputs", ins}, {"outputs", outs},
                             {"doc", doc}, {"props", props}});
    };
    Json p = Json::array();
    p.push(node("let", "variables", "Let", {"value"}, {}, "Declare a local: let name = value", Json::object({{"name", "x"}})));
    p.push(node("set", "variables", "Set", {"value"}, {}, "Assign a variable or property (props.target, optional props.op)",
                Json::object({{"target", "self.position"}})));
    p.push(node("if", "flow", "Branch", {"if"}, {"then", "else"}, "if / elif / else", Json::object({{"branches", 1}, {"else", true}})));
    p.push(node("while", "flow", "While", {"condition"}, {"body"}, "Loop while true (bounded by the step budget)"));
    p.push(node("for", "flow", "For Each", {"items"}, {"body"}, "Loop over a list, map or string",
                Json::object({{"var", "item"}, {"range", false}})));
    p.push(node("for", "flow", "For Range", {"from", "to"}, {"body"}, "Loop over numbers: from..to (end excluded)",
                Json::object({{"var", "i"}, {"range", true}})));
    p.push(node("repeat", "flow", "Repeat", {"times"}, {"body"}, "Run the body n times"));
    p.push(node("every", "flow", "Every", {"seconds"}, {"body"}, "Run the body every n seconds"));
    p.push(node("after", "flow", "After", {"seconds"}, {"body"}, "Run the body once, n seconds after start"));
    p.push(node("wait", "flow", "Wait", {"amount"}, {}, "Pause this handler (seconds | frames | until)",
                Json::object({{"mode", "seconds"}})));
    p.push(node("break", "flow", "Break", {}, {}, "Leave the loop"));
    p.push(node("continue", "flow", "Continue", {}, {}, "Next loop iteration"));
    p.push(node("return", "flow", "Return", {"value"}, {}, "Return from a fn (or leave the handler)"));
    p.push(node("stop", "flow", "Stop", {}, {}, "Leave the handler"));
    p.push(node("go_to", "flow", "Go To State", {}, {}, "Switch the state machine", Json::object({{"state", "Idle"}})));
    p.push(node("move", "actions", "Move By", {"entity", "offset"}, {}, "move entity by offset"));
    p.push(node("move_toward", "actions", "Move Toward", {"entity", "destination", "speed"}, {}, "move entity toward a point at a speed"));
    p.push(node("rotate", "actions", "Rotate", {"entity", "degrees"}, {}, "rotate entity by Euler degrees"));
    p.push(node("look", "actions", "Look At", {"entity", "point"}, {}, "turn entity to face a point"));
    p.push(node("emit", "actions", "Emit Event", {"payload", "receiver"}, {}, "send an event (next tick)",
                Json::object({{"event", "my_event"}})));
    p.push(node("destroy", "actions", "Destroy", {"entity"}, {}, "destroy an entity (end of tick)"));
    p.push(node("log", "actions", "Log", {"value"}, {}, "print to the log"));
    p.push(node("call", "actions", "Call", {"call"}, {}, "run a function/method call for its effect"));
    p.push(node("expect", "tests", "Expect", {"condition"}, {}, "test assertion"));
    p.push(node("op", "expression", "Operator", {"a", "b"}, {"value"}, "+ - * / % < <= > >= == != and or in",
                Json::object({{"op", "+"}})));
    p.push(node("not", "expression", "Not", {"value"}, {"value"}, "logical not"));
    p.push(node("neg", "expression", "Negate", {"value"}, {"value"}, "-x"));
    p.push(node("member", "expression", "Get Property", {"object"}, {"value"}, "object.name (position, hp, light.intensity...)",
                Json::object({{"name", "position"}})));
    p.push(node("index", "expression", "Get Item", {"object", "index"}, {"value"}, "list[i] / map[key]"));
    p.push(node("call", "expression", "Function", {"arg1"}, {"value"}, "call a builtin or fn (props.function)",
                Json::object({{"function", "distance"}})));
    p.push(node("method", "expression", "Method", {"object", "arg1"}, {"value"}, "object.method(args)",
                Json::object({{"method", "push"}})));
    p.push(node("vector", "expression", "Vector", {"x", "y", "z"}, {"value"}, "(x, y, z)"));
    p.push(node("list", "expression", "List", {"item1"}, {"value"}, "[items]"));
    p.push(node("map", "expression", "Map", {}, {"value"}, "{key: value} (props.keys, inputs key:<name>)",
                Json::object({{"keys", Json::array()}})));
    p.push(node("text", "expression", "Text", {"part1"}, {"value"}, "string interpolation (props.parts)",
                Json::object({{"parts", Json::array({"", ""})}})));
    p.push(node("value", "expression", "Expression", {}, {"value"}, "any Wander expression as code (props.code)",
                Json::object({{"code", "0"}})));
    return p;
}

}  // namespace sky::wander
