#pragma once
// Dialogue scripts (*.dialogue): a Yarn Spinner-style language that language models write well.
//
//   title: Start
//   tags: intro
//   ---
//   Narrator: The council chamber falls silent. #mood:tense
//   Vale: You're late, Chancellor. #portrait:vale_cold
//   -> I was delayed by the riots.
//       <<set $trust += 1>>
//       Vale: Riots. Of course.
//   -> That is none of your concern. <<if $trust > 2>>
//       <<set $trust -= 2>>
//       <<jump Confrontation>>
//   <<if visited("Archive")>>
//       Vale: I hear you've been reading old letters, {$name}.
//   <<else>>
//       Vale: Sit.
//   <<endif>>
//   <<lights_dim 0.5>>
//   ===
//
// Lines are `Speaker: text` (or narration without a speaker) with trailing #tags (#key:value);
// `{expr}` interpolates. `-> option` lines form a choice group; an option's indented lines are its
// body; a trailing <<if cond>> hides the option (or a line) while cond is false. Commands: set
// (to = += -= *= /=), declare, if/elseif/else/endif, jump, stop, wait <seconds>; any other
// <<command args>> is sent to Wander as the event "dialogue:<command>". Expressions: numbers,
// "strings", true/false, $variables, + - * / %, == != < <= > >= (also eq neq lt gt lte gte is),
// and or not (&& || !), and visited("Node"), visited_count("Node"), random(), random_range(a, b),
// dice(n), round/floor/ceil/abs/min/max.

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Random.h"
#include "skywalker/core/Result.h"

namespace sky::dialogue {

struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;

/// Text with {expression} interpolations.
struct TextTemplate {
    struct Part {
        std::string literal;
        ExprPtr expr;
    };
    std::vector<Part> parts;
    std::string source;  // the text as written (localization looks it up / replaces it)
};

struct Option {
    TextTemplate text;
    ExprPtr condition;  // null = always available
    Json tags = Json::object();
    int target = 0;     // first instruction of the option's body
    int line = 0;
};

struct Instr {
    enum class Op { Line, Choices, Jump, JumpIfFalse, Goto, Set, Declare, Command, Wait, Stop };
    Op op = Op::Stop;
    int line = 0;  // source line
    // Line
    std::string speaker;
    TextTemplate text;
    Json tags = Json::object();
    ExprPtr condition;  // Line (optional), JumpIfFalse
    // Choices
    std::vector<Option> options;
    int target = 0;  // Goto / JumpIfFalse / Choices: after the group
    // Jump
    std::string node;
    // Set / Declare
    std::string var;
    std::string assignOp;  // = += -= *= /=
    ExprPtr value;          // Set / Declare / Wait
    // Command
    std::string command;
    std::vector<TextTemplate> args;
};

struct Diagnostic {
    bool error = true;
    int line = 0;
    std::string code;
    std::string message;
    std::string hint;
    Json toJson() const;
};

struct DialogueNode {
    std::string title;
    Json headers = Json::object();
    std::vector<Instr> code;
    int line = 0;
};

struct Script {
    std::vector<DialogueNode> nodes;
    std::map<std::string, int> index;
    std::vector<Diagnostic> diagnostics;
    std::set<std::string> varsRead, varsWritten, functions, commands;
    std::map<std::string, Json> declared;  // <<declare $x = v>> defaults
    std::map<std::string, std::set<std::string>> jumps;  // node -> nodes it jumps to

    bool ok() const;
    const DialogueNode* find(const std::string& title) const;
    std::vector<std::string> titles() const;
};

/// Parses a script. Syntax errors become diagnostics (the script still contains what parsed).
std::shared_ptr<const Script> parse(std::string_view source);
/// Parses one line of text with {expression} interpolations (a translated line, for example).
TextTemplate parseTemplate(std::string_view text);

/// Lints a parsed script: syntax errors, unknown jump targets (with did-you-mean), missing start
/// node, unreachable nodes, variables read but never set or declared, empty nodes.
Json lint(const Script& script, const std::string& startNode);

/// Reads and writes dialogue variables (names without '$').
struct VarStore {
    std::function<Json(const std::string&)> get;
    std::function<void(const std::string&, const Json&)> set;
};

/// What a runner reports while it executes.
struct RunnerEvents {
    std::function<void(const std::string& command, const std::vector<std::string>& args)> command;
    std::function<void(const std::string& node)> node;
    /// Localization (docs/LOCALIZATION.md): the text to show for a line or choice with the `#line:<id>` tag
    /// (`lineId`, else "") and the script's text `source`; returning `source` keeps it. Unset = no localization.
    std::function<std::string(const std::string& lineId, const std::string& source)> localize;
    /// The display name of a speaker (unset = as written).
    std::function<std::string(const std::string& speaker)> speaker;
};

/// Executes a script one blocking step at a time (a line to show, choices to pick, a wait).
class Runner {
public:
    enum class State { Idle, Line, Choices, Waiting, Ended };
    struct LineView {
        std::string speaker, text;
        Json tags = Json::object();
    };
    struct ChoiceView {
        std::string text;
        Json tags = Json::object();
        int option = 0;  // index into the instruction's options
    };

    explicit Runner(std::shared_ptr<const Script> script, uint64_t seed = 1);

    Status start(const std::string& node, VarStore& vars, const RunnerEvents& events);
    /// Continues after a line (or a finished wait). No-op while choices are pending.
    Status advance(VarStore& vars, const RunnerEvents& events);
    /// Picks one of the available choices (0-based index into choices()).
    Status choose(int index, VarStore& vars, const RunnerEvents& events);
    /// Counts down waits; continues automatically when they finish.
    Status update(float dt, VarStore& vars, const RunnerEvents& events);
    void stop();

    State state() const { return state_; }
    const std::string& node() const { return node_; }
    const LineView& line() const { return line_; }
    const std::vector<ChoiceView>& choices() const { return choices_; }
    int visited(const std::string& node) const;
    const Script& script() const { return *script_; }

private:
    Status run(VarStore& vars, const RunnerEvents& events);
    Status enter(const std::string& node, const RunnerEvents& events);

    std::shared_ptr<const Script> script_;
    Random rng_;
    State state_ = State::Idle;
    std::string node_;
    int nodeIndex_ = -1;
    int pc_ = 0;
    float wait_ = 0;
    LineView line_;
    std::vector<ChoiceView> choices_;
    int choiceInstr_ = -1;
    std::map<std::string, int> visits_;
};

/// Simulates a conversation without UI: picks `choices` in order (indices or text fragments) and
/// returns a transcript, the final variables and where it stopped.
Json preview(std::shared_ptr<const Script> script, const std::string& start, const Json& choices, const Json& vars,
             int maxSteps = 500);

}  // namespace sky::dialogue
