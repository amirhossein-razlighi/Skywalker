#pragma once
// Blueprint-style node graph view of Wander code.
//
// toGraph() turns a parsed script into a JSON node graph: every handler, fn and test is a
// *body* with an entry node; statements are exec-flow nodes chained by exec wires
// (`if` has then/elif/else outputs, loops a `body` output); expressions are data nodes
// (operators, calls, member access, ...) wired into statement inputs; literals and
// plain names sit inline on input pins (`value`), like Unreal's pin defaults. States are
// sub-graphs holding their handlers. Nodes get an automatic layout; saved positions
// (Script::graph) override it.
//
// fromGraph() turns a (possibly edited) graph back into canonical Wander source. The
// mapping is lossless: code -> graph -> code yields the same program (tested on every
// example script). Unknown or malformed nodes produce precise errors naming the node.
//
// Graph JSON (version 1):
// {
//   "format": "wander-graph", "version": 1,
//   "uses": [{"path", "alias"}], "consts": [{"name", "type", "value"}],
//   "functions": [Body], "tests": [Body],
//   "behaviors": [{"name", "intent", "implicit", "vars": [{"name", "type", "value", "param", "min", "max", "doc"}],
//                  "consts": [...], "functions": [Body], "handlers": [Body], "states": [{"name", "handlers": [Body]}],
//                  "tests": [Body]}]
// }
// Body: {"id", "kind": "handler"|"function"|"test", "title", "entry": nodeId,
//        handler: "trigger", "argument", "binding", "custom"; function: "name", "params", "returns"; test: "name",
//        "nodes": [{"id", "type", "x", "y", "props": {...}, "comment": [...],
//                   "inputs": [{"name", "kind": "exec"|"data", "value"?}], "outputs": [{"name", "kind"}]}],
//        "links": [{"from", "out", "to", "in"}]}
// Statement node types: let set if while for repeat every after wait break continue return stop go_to move
//   move_toward rotate look emit destroy log call expect press hold release click
// Expression node types: op not neg member index call method list map vector text value

#include <string>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/wander/Ast.h"

namespace sky::wander {

/// Graph for a parsed module. `layout` ({"nodeId": [x, y]}) overrides automatic positions.
Json toGraph(const Module& module, const Json& layout = Json());

/// Wander source for a graph (canonical formatting).
Result<std::string> fromGraph(const Json& graph);

/// The node positions of a graph ({"nodeId": [x, y]}), to persist editor layouts.
Json graphLayout(const Json& graph);

/// Node palette for editors: every node type with its pins and a description.
Json graphPalette();

}  // namespace sky::wander
