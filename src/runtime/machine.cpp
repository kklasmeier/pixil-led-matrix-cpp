#include "runtime/machine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "runtime/python_ops.h"
#include "syntax/parser.h"

namespace pixil {

namespace {

struct BreakLoop {};

const std::unordered_set<std::string> kDrawCommands = {
    "plot", "mplot", "draw_line", "draw_rectangle", "draw_circle", "draw_polygon", "draw_ellipse", "draw_arc",
};

// Drawing commands accepted inside define_sprite...endsprite (regex_patterns.py).
const std::unordered_set<std::string> kSpriteDraws = {
    "plot", "mplot", "draw_line", "draw_rectangle", "draw_circle", "draw_polygon", "draw_ellipse", "draw_arc", "clear",
};

const std::unordered_set<std::string> kSpriteOps = {"show_sprite", "move_sprite", "hide_sprite"};

int64_t rounded_int(const Value& v) {
    Value r = py_round(v);
    if (r.is_int()) return r.as_int();
    if (r.is_float()) return static_cast<int64_t>(r.as_float());
    throw PixilError(ErrorKind::Type, std::string("cannot round ") + v.type_name());
}

// Argument types from legacy parameter_types + param_bounds. Commands not in the
// table pass each value through py_str.
enum class Arg { Int, Float, Color, Intensity, Burnout, Str, Text, Font, Bool };

const std::unordered_map<std::string, std::vector<Arg>> kArgTypes = {
    {"plot", {Arg::Int, Arg::Int, Arg::Color, Arg::Intensity, Arg::Burnout, Arg::Str}},
    {"mplot", {Arg::Int, Arg::Int, Arg::Color, Arg::Intensity, Arg::Burnout, Arg::Str}},
    {"draw_line", {Arg::Int, Arg::Int, Arg::Int, Arg::Int, Arg::Color, Arg::Intensity, Arg::Burnout, Arg::Str}},
    {"draw_rectangle",
     {Arg::Int, Arg::Int, Arg::Int, Arg::Int, Arg::Color, Arg::Intensity, Arg::Bool, Arg::Float, Arg::Str}},
    {"draw_circle", {Arg::Int, Arg::Int, Arg::Int, Arg::Color, Arg::Intensity, Arg::Bool, Arg::Float, Arg::Str}},
    {"draw_polygon",
     {Arg::Int, Arg::Int, Arg::Int, Arg::Int, Arg::Color, Arg::Intensity, Arg::Float, Arg::Bool, Arg::Float, Arg::Str}},
    {"draw_ellipse",
     {Arg::Int, Arg::Int, Arg::Int, Arg::Int, Arg::Color, Arg::Intensity, Arg::Bool, Arg::Float, Arg::Float, Arg::Str}},
    {"draw_arc",
     {Arg::Int, Arg::Int, Arg::Int, Arg::Int, Arg::Float, Arg::Color, Arg::Intensity, Arg::Bool, Arg::Float, Arg::Str}},
    {"draw_text",
     {Arg::Int, Arg::Int, Arg::Text, Arg::Font, Arg::Int, Arg::Color, Arg::Intensity, Arg::Str, Arg::Str}},
    {"clear_text", {Arg::Int, Arg::Int}},
    {"define_sprite", {Arg::Str, Arg::Int, Arg::Int}},
    {"show_sprite", {Arg::Str, Arg::Int, Arg::Int, Arg::Int, Arg::Int, Arg::Int}},
    {"move_sprite", {Arg::Str, Arg::Int, Arg::Int, Arg::Int, Arg::Int, Arg::Int}},
    {"hide_sprite", {Arg::Str, Arg::Int}},
    {"dispose_sprite", {Arg::Str, Arg::Int}},
};

// format_numeric_for_display: a whole-number float shows as an int.
std::string display_text(const Value& v) {
    if (v.is_float() && std::isfinite(v.as_float()) && v.as_float() == std::trunc(v.as_float())) {
        return py_str(py_int(v));
    }
    return py_str(v);
}

std::string quoted(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s;
    return "\"" + s + "\"";
}

// convert_to_type('bool'): a string is true only when it is "true".
bool param_bool(const Value& v) {
    if (v.is_bool()) return v.as_bool();
    if (v.is_str()) {
        std::string s = v.as_str();
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s == "true";
    }
    if (v.is_none()) return false;
    return truthy(v);
}

std::string format_arg(Arg type, const Value& v) {
    switch (type) {
        case Arg::Int: return std::to_string(rounded_int(v));
        case Arg::Float:
            // A non-negative integer literal stays "0". A minus sign misses that
            // fast path, so -30 is formatted as a float: "-30.0".
            if (v.is_int() && v.as_int() >= 0) return std::to_string(v.as_int());
            return py_float_repr(v.to_double());
        case Arg::Color:
            if (v.is_str()) return v.as_str();
            return std::to_string(std::clamp<int64_t>(rounded_int(v), 0, 99));
        case Arg::Intensity: return std::to_string(std::clamp<int64_t>(rounded_int(v), 0, 100));
        case Arg::Bool: return param_bool(v) ? "true" : "false";
        case Arg::Burnout: {
            double d = v.to_double();
            if (d == -1.0) return "-1";
            if (d < 0.0) d = 0.0;
            if (std::isfinite(d) && d == std::trunc(d)) return std::to_string(static_cast<long long>(d));
            return py_float_repr(d);
        }
        case Arg::Str: return py_str(v);
        case Arg::Text: return quoted(display_text(v));
        case Arg::Font: return quoted(py_str(v));
    }
    return py_str(v);
}

// _format_sprite_draw_arg: quote empty strings, identifiers, and anything with a letter.
// "true" and "white" become "\"true\"" and "\"white\""; "11" stays 11.
bool sprite_arg_needs_quotes(const std::string& s) {
    if (s.empty()) return true;
    for (unsigned char c : s) {
        if (std::isalpha(c)) return true;
    }
    unsigned char first = static_cast<unsigned char>(s[0]);
    if (!(std::isalpha(first) || first == '_')) return false;
    for (unsigned char c : s) {
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return true;
}

std::string quote_sprite_arg(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string format_sprite_draw(const std::string& sprite, const std::string& cmd, const std::vector<Value>& args) {
    const std::string emit_name = cmd == "mplot" ? "plot" : cmd;
    auto types = kArgTypes.find(emit_name);
    std::string out = "sprite_draw(" + sprite + ", " + emit_name;
    for (size_t i = 0; i < args.size(); ++i) {
        out += ", ";
        std::string text = (types != kArgTypes.end() && i < types->second.size())
                               ? format_arg(types->second[i], args[i])
                               : py_str(args[i]);
        if (sprite_arg_needs_quotes(text)) text = quote_sprite_arg(text);
        out += text;
    }
    out += ")";
    return out;
}

std::string trim_ws(std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool print_expr_evaluates(const std::string& expr) {
    return expr.find_first_of("[]+-*/()") != std::string::npos;
}

// Legacy print f-strings: evaluate brace expressions, and leave {name} when that fails.
std::string interpolate_print(const std::string& body, Env& env) {
    std::string out;
    size_t i = 0;
    while (i < body.size()) {
        size_t lb = body.find('{', i);
        if (lb == std::string::npos) {
            out += body.substr(i);
            break;
        }
        out += body.substr(i, lb - i);
        size_t rb = body.find('}', lb);
        if (rb == std::string::npos) {
            out += body.substr(lb);
            break;
        }
        std::string expr = trim_ws(body.substr(lb + 1, rb - lb - 1));
        bool wrote = false;
        if (print_expr_evaluates(expr) || expr.rfind("v_", 0) == 0) {
            try {
                ExprPtr tree = parse_expression(expr, 0);
                resolve(*tree, env.symbols);
                env.ensure_slots();
                out += py_str(eval(*tree, env));
                wrote = true;
            } catch (const PixilError&) {
                out += "{" + expr + "}";
                wrote = true;
            }
        }
        if (!wrote) out += expr;
        i = rb + 1;
    }
    return out;
}

void render_print(const std::string& content, Env& env) {
    if (content.size() >= 3 && content[0] == 'f' && (content[1] == '"' || content[1] == '\'')) {
        char q = content[1];
        if (content.back() == q) {
            std::cout << interpolate_print(content.substr(2, content.size() - 3), env) << '\n';
            return;
        }
    }
    std::string text = content;
    while (!text.empty() && (text.front() == '"' || text.front() == '\'')) text.erase(text.begin());
    while (!text.empty() && (text.back() == '"' || text.back() == '\'')) text.pop_back();
    std::cout << text << '\n';
}

std::string format_command(const std::string& name, const std::vector<Value>& args);

const PixArray& numeric_array(const Value& v, const std::string& what) {
    if (!v.is_array() || v.as_array()->type != ArrayType::Numeric) {
        throw PixilError(ErrorKind::Value, what + " must be a numeric array");
    }
    return *v.as_array();
}

int64_t count_value(const Value& v) {
    Value n = py_int(v);
    if (!n.is_int()) throw PixilError(ErrorKind::Value, "count is out of range");
    return n.as_int();
}

// draw_lines packs one draw_line per segment. Inside a frame the endpoints stay
// raw so the batch rounds them, which is what pack_line_record does.
void expand_draw_lines(const std::vector<Value>& args, bool in_frame, DrawBatch& batch,
                       const std::function<void(std::string)>& emit_text) {
    if (args.size() < 5) throw PixilError(ErrorKind::Value, "draw_lines needs endpoints and a color");
    const PixArray& x0 = numeric_array(args[0], "draw_lines x0");
    const PixArray& y0 = numeric_array(args[1], "draw_lines y0");
    const PixArray& x1 = numeric_array(args[2], "draw_lines x1");
    const PixArray& y1 = numeric_array(args[3], "draw_lines y1");
    int64_t n = static_cast<int64_t>(x0.data.size());
    n = std::min(n, static_cast<int64_t>(y0.data.size()));
    n = std::min(n, static_cast<int64_t>(x1.data.size()));
    n = std::min(n, static_cast<int64_t>(y1.data.size()));
    const PixArray* inten = nullptr;
    Value scalar = Value::integer(100);
    if (args.size() >= 6) {
        if (args[5].is_array()) inten = &numeric_array(args[5], "draw_lines intensity");
        else scalar = args[5];
    }
    if (args.size() >= 7) n = std::min(n, count_value(args[6]));
    if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        auto at = [&](const PixArray& a) { return a.data[static_cast<size_t>(i)]; };
        Value bright = inten != nullptr ? at(*inten) : scalar;
        if (in_frame) {
            batch.append("draw_line", {at(x0), at(y0), at(x1), at(y1), args[4], bright});
        } else {
            emit_text(format_command(
                "draw_line", {py_int(at(x0)), py_int(at(y0)), py_int(at(x1)), py_int(at(y1)), args[4], py_int(bright)}));
        }
    }
}

// move_sprites packs instance ids 0..n-1. A following array is cel, then z.
// A number in either slot is the count. Coords are rounded by the sprite batch.
void expand_move_sprites(const std::vector<Value>& args, bool in_frame, SpriteBatch& sprites,
                         const std::function<void(std::string)>& emit_text) {
    if (args.size() < 3) throw PixilError(ErrorKind::Value, "move_sprites needs a name, x, and y");
    const PixArray& x = numeric_array(args[1], "move_sprites x");
    const PixArray& y = numeric_array(args[2], "move_sprites y");
    const PixArray* cel = nullptr;
    const PixArray* z = nullptr;
    bool have_count = false;
    int64_t count = 0;
    size_t slot = 3;
    auto take_count = [&](const Value& v) {
        have_count = true;
        count = count_value(v);
    };
    if (slot < args.size()) {
        if (args[slot].is_array()) {
            cel = &numeric_array(args[slot], "move_sprites cel");
            ++slot;
            if (slot < args.size()) {
                if (args[slot].is_array()) {
                    z = &numeric_array(args[slot], "move_sprites z");
                    ++slot;
                    if (slot < args.size()) take_count(args[slot]);
                } else {
                    take_count(args[slot]);
                }
            }
        } else {
            take_count(args[slot]);
        }
    }
    int64_t n = std::min(static_cast<int64_t>(x.data.size()), static_cast<int64_t>(y.data.size()));
    if (have_count) n = std::min(n, count);
    if (n < 0) n = 0;
    for (int64_t i = 0; i < n; ++i) {
        std::vector<Value> one;
        one.push_back(args[0]);
        Value px = x.data[static_cast<size_t>(i)];
        Value py = y.data[static_cast<size_t>(i)];
        if (!in_frame) {
            px = py_int(px);
            py = py_int(py);
        }
        one.push_back(px);
        one.push_back(py);
        one.push_back(Value::integer(i));
        if (cel != nullptr || z != nullptr) {
            one.push_back(cel != nullptr ? py_int(cel->data[static_cast<size_t>(i)]) : Value::integer(255));
        }
        if (z != nullptr) one.push_back(py_int(z->data[static_cast<size_t>(i)]));
        if (in_frame) sprites.append("move_sprite", one);
        else emit_text(format_command("move_sprite", one));
    }
}

std::string format_command(const std::string& name, const std::vector<Value>& args) {
    // Legacy emits no-arg commands as bare words: clear() becomes "clear".
    if (args.empty()) return name;
    auto types = kArgTypes.find(name);
    std::string out = name + "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out += ", ";
        if (types != kArgTypes.end() && i < types->second.size()) out += format_arg(types->second[i], args[i]);
        else out += py_str(args[i]);
    }
    out += ")";
    return out;
}

}  // namespace

bool Machine::should_stop() const {
    if (stopped) return true;
    return opt_.stop_requested && opt_.stop_requested();
}

void Machine::emit(std::string cmd) {
    if (should_stop()) {
        stopped = true;
        return;
    }
    if (opt_.max_commands >= 0 && commands >= opt_.max_commands) {
        stopped = true;
        return;
    }
    if (opt_.emit) opt_.emit(cmd);
    ++commands;
    if (opt_.max_commands >= 0 && commands >= opt_.max_commands) stopped = true;
}

Value Machine::eval_arg(const Expr& expr) { return eval(expr, env); }

void Machine::pace_after_frame() {
    if (!opt_.pace || target_fps_ <= 0) return;
    using clock = std::chrono::steady_clock;
    const auto interval = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / target_fps_));
    const auto now = clock::now();
    if (next_frame_at_.time_since_epoch().count() != 0 && now < next_frame_at_) {
        while (!should_stop()) {
            const auto left = next_frame_at_ - clock::now();
            if (left <= clock::duration::zero()) break;
            auto slice = left;
            if (slice > std::chrono::milliseconds(5)) slice = std::chrono::milliseconds(5);
            std::this_thread::sleep_for(slice);
        }
        if (should_stop()) {
            stopped = true;
            return;
        }
    }
    next_frame_at_ = clock::now() + interval;
}

void Machine::pace_rest(double seconds) {
    if (!opt_.pace || seconds <= 0) return;
    using clock = std::chrono::steady_clock;
    const auto deadline =
        clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(seconds));
    while (!should_stop() && clock::now() < deadline) {
        const auto left = deadline - clock::now();
        if (left <= clock::duration::zero()) break;
        auto slice = left;
        if (slice > std::chrono::milliseconds(10)) slice = std::chrono::milliseconds(10);
        std::this_thread::sleep_for(slice);
    }
    if (should_stop()) stopped = true;
    // A rest breaks the fps cadence — next end_frame starts a fresh interval.
    next_frame_at_ = {};
}

void Machine::finish_frame() {
    if (in_frame_) {
        emit(preserve_ ? "begin_frame(true)" : "begin_frame(false)");
        if (!batch_.empty()) emit(batch_.command());
        if (!sprites_.empty()) emit(sprites_.command());
        for (const auto& extra : frame_extra_) emit(extra);
    }
    emit("end_frame");
    in_frame_ = false;
    preserve_ = false;
    batch_.clear();
    sprites_.clear();
    frame_extra_.clear();
    ++frames;
    if (opt_.max_frames >= 0 && frames >= opt_.max_frames) stopped = true;
    pace_after_frame();
}

void Machine::run(const Script& script, const RunOptions& options) {
    opt_ = options;
    env.ensure_slots();
    run_list(script.body);
}

void Machine::run_list(const std::vector<StmtPtr>& body) {
    for (const auto& stmt : body) {
        if (should_stop()) {
            stopped = true;
            return;
        }
        run_stmt(*stmt);
    }
}

void Machine::run_stmt(const Stmt& stmt) {
    switch (stmt.kind) {
        case StmtKind::Assign:
            env.ensure_slots();
            env.slots[static_cast<size_t>(stmt.slot)] = eval(*stmt.expr, env);
            break;
        case StmtKind::CreateArray: {
            env.ensure_slots();
            Value n = py_int(eval(*stmt.expr, env));
            if (!n.is_int() || n.as_int() < 0) {
                throw PixilError(ErrorKind::Value, "line " + std::to_string(stmt.line) + ": array size must be a non-negative integer");
            }
            auto arr = std::make_shared<PixArray>(static_cast<size_t>(n.as_int()),
                                                   stmt.string_array ? ArrayType::String : ArrayType::Numeric);
            env.slots[static_cast<size_t>(stmt.slot)] = Value::array(std::move(arr));
            break;
        }
        case StmtKind::ArrayAssign: {
            env.ensure_slots();
            Value& cell = env.slots[static_cast<size_t>(stmt.slot)];
            if (!cell.is_array()) {
                throw PixilError(ErrorKind::Type, "line " + std::to_string(stmt.line) + ": '" + stmt.name + "' is not an array");
            }
            array_store(*cell.as_array(), eval(*stmt.args[0], env), eval(*stmt.expr, env));
            break;
        }
        case StmtKind::Proc:
            procs_[stmt.name] = &stmt;
            break;
        case StmtKind::GridProgram:
            define_grid_program(*grids_, stmt.name, stmt.text);
            break;
        case StmtKind::FieldProgram:
            define_field_program(*grids_, stmt.name, stmt.text);
            break;
        case StmtKind::Import:
            import_module(stmt);
            break;
        case StmtKind::Call: {
            auto found = procs_.find(stmt.name);
            if (found == procs_.end()) {
                throw PixilError(ErrorKind::Name, "line " + std::to_string(stmt.line) + ": procedure '" + stmt.name + "' is not defined");
            }
            call_proc(stmt, *found->second);
            break;
        }
        case StmtKind::If:
            for (const auto& clause : stmt.clauses) {
                if (!clause.cond || truthy(eval(*clause.cond, env))) {
                    run_list(clause.body);
                    break;
                }
            }
            break;
        case StmtKind::For: {
            double start = eval(*stmt.args[0], env).to_double();
            double end = eval(*stmt.args[1], env).to_double();
            double step = eval(*stmt.args[2], env).to_double();
            if (step == 0.0) throw PixilError(ErrorKind::Value, "line " + std::to_string(stmt.line) + ": for step is 0");
            double current = start;
            const double epsilon = 1e-10;
            int iterations = 0;
            env.ensure_slots();
            while ((step > 0 && current <= end + epsilon) || (step < 0 && current >= end - epsilon)) {
                if (should_stop()) {
                    stopped = true;
                    return;
                }
                if (++iterations > 1000000) break;
                env.slots[static_cast<size_t>(stmt.slot)] = Value::real(current);
                try {
                    run_list(stmt.body);
                } catch (const BreakLoop&) {
                    break;
                }
                current += step;
            }
            break;
        }
        case StmtKind::While: {
            int iterations = 0;
            while (truthy(eval(*stmt.expr, env))) {
                if (should_stop()) {
                    stopped = true;
                    return;
                }
                if (++iterations > 1000000) break;
                try {
                    run_list(stmt.body);
                } catch (const BreakLoop&) {
                    break;
                }
            }
            break;
        }
        case StmtKind::Break:
            throw BreakLoop();
        case StmtKind::BeginFrame:
            batch_.clear();
            sprites_.clear();
            frame_extra_.clear();
            in_frame_ = true;
            preserve_ = stmt.preserve;
            break;
        case StmtKind::EndFrame:
            finish_frame();
            break;
        case StmtKind::Command:
            run_command(stmt);
            break;
    }
}

void collect_assigned(const Stmt& stmt, std::set<int>& slots) {
    if (stmt.slot >= 0) slots.insert(stmt.slot);
    for (int param : stmt.params) slots.insert(param);
    for (const auto& clause : stmt.clauses) {
        for (const auto& child : clause.body) collect_assigned(*child, slots);
    }
    for (const auto& child : stmt.body) collect_assigned(*child, slots);
}

void Machine::call_proc(const Stmt& call, const Stmt& proc) {
    if (call.args.size() != proc.params.size()) {
        throw PixilError(ErrorKind::Type, "line " + std::to_string(call.line) + ": procedure '" + proc.name +
                                               "' takes " + std::to_string(proc.params.size()) +
                                               " argument(s), got " + std::to_string(call.args.size()));
    }
    // A procedure with no parameters keeps legacy global assignments.
    if (proc.params.empty()) {
        run_list(proc.body);
        return;
    }
    std::vector<Value> argv;
    argv.reserve(call.args.size());
    for (const auto& arg : call.args) argv.push_back(eval_arg(*arg));
    env.ensure_slots();
    std::set<int> locals;
    collect_assigned(proc, locals);
    std::vector<std::pair<int, Value>> saved;
    saved.reserve(locals.size());
    for (int slot : locals) saved.emplace_back(slot, env.slots[static_cast<size_t>(slot)]);
    for (size_t i = 0; i < proc.params.size(); ++i) {
        env.slots[static_cast<size_t>(proc.params[i])] = std::move(argv[i]);
    }
    struct Restore {
        Env& env;
        std::vector<std::pair<int, Value>> saved;
        ~Restore() {
            for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
                env.slots[static_cast<size_t>(it->first)] = std::move(it->second);
            }
        }
    } restore{env, std::move(saved)};
    run_list(proc.body);
}

void Machine::import_module(const Stmt& stmt) {
    const std::string& path = stmt.name;
    if (loaded_.count(path) != 0) return;
    const auto slash = path.rfind('/');
    const std::string prefix = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto existing = prefixes_.find(prefix);
    if (existing != prefixes_.end()) {
        throw PixilError(ErrorKind::Name, "line " + std::to_string(stmt.line) + ": library prefix '" + prefix +
                                               "' is already used by '" + existing->second + "'");
    }
    std::string dir = opt_.lib_dir.empty() ? "scripts/lib" : opt_.lib_dir;
    if (dir.back() != '/') dir.push_back('/');
    auto lib = std::make_unique<Script>(parse_script_file(dir + path + ".pix"));
    for (const auto& item : lib->body) {
        if (item->kind != StmtKind::Proc) {
            throw PixilError(ErrorKind::Value, "line " + std::to_string(item->line) + ": library '" + path +
                                                    "' may only contain procedure definitions");
        }
    }
    resolve_script(*lib, env.symbols);
    env.ensure_slots();
    for (auto& item : lib->body) item->name = prefix + "." + item->name;
    prefixes_.emplace(prefix, path);
    loaded_.insert(path);
    Script* stored = lib.get();
    modules_.push_back(std::move(lib));
    for (const auto& item : stored->body) procs_[item->name] = item.get();
}

void Machine::run_command(const Stmt& stmt) {
    // throttle is producer pacing. Phase 1 always drives the consumer as throttle(0) (D-007).
    if (stmt.name == "throttle") return;
    // mplot is buffered, including outside begin_frame. mflush sends that batch.
    // The consumer has no mplot command; a raw mplot() makes it exit.
    if (stmt.name == "mflush") {
        if (!in_frame_ && !batch_.empty()) {
            emit(batch_.command());
            batch_.clear();
        }
        return;
    }
    if (stmt.name == "endsprite") {
        emit("endsprite");
        in_sprite_ = false;
        sprite_name_.clear();
        return;
    }
    // A bare word naming a defined procedure is a call (legacy compiled procedures).
    if (stmt.bare) {
        auto found = procs_.find(stmt.name);
        if (found != procs_.end()) {
            call_proc(stmt, *found->second);
            return;
        }
    }

    std::vector<Value> args;
    args.reserve(stmt.args.size());
    for (const auto& arg : stmt.args) args.push_back(eval_arg(*arg));

    if (stmt.name == "grid_fill" && args.size() >= 2 && args[0].is_array()) {
        grid_fill_array(*args[0].as_array(), args[1].to_double());
        return;
    }
    if (stmt.name == "grid_reset" && !args.empty() && args[0].is_str()) {
        grid_reset(*grids_, args[0].as_str());
        return;
    }
    if (stmt.name == "grid_step" && !args.empty() && args[0].is_str()) {
        grid_step(*grids_, env, args[0].as_str(), batch_);
        return;
    }
    if (stmt.name == "field_render" && !args.empty() && args[0].is_str()) {
        field_render(*grids_, env, args[0].as_str(), batch_);
        return;
    }
    if (stmt.name == "print") {
        if (!args.empty() && args[0].is_str()) render_print(args[0].as_str(), env);
        return;
    }
    if (stmt.name == "sprite_cel") {
        if (!in_sprite_) {
            throw PixilError(ErrorKind::Value, "line " + std::to_string(stmt.line) + ": sprite_cel is only valid inside define_sprite");
        }
        if (args.empty()) emit("sprite_cel()");
        else emit("sprite_cel(" + std::to_string(rounded_int(args[0])) + ")");
        return;
    }
    if (stmt.name == "define_sprite") {
        if (args.size() < 3 || !args[0].is_str()) {
            throw PixilError(ErrorKind::Value, "line " + std::to_string(stmt.line) + ": define_sprite needs a name, width, and height");
        }
        sprite_name_ = args[0].as_str();
        in_sprite_ = true;
        emit(format_command(stmt.name, args));
        return;
    }
    if (in_sprite_) {
        if (!kSpriteDraws.count(stmt.name)) {
            throw PixilError(ErrorKind::Value, "line " + std::to_string(stmt.line) + ": '" + stmt.name +
                                                    "' is not supported during sprite definition");
        }
        emit(format_sprite_draw(sprite_name_, stmt.name, args));
        return;
    }

    if (stmt.name == "fps") {
        double rate = args.empty() ? 0.0 : args[0].to_double();
        target_fps_ = rate > 0 ? rate : 0;
        next_frame_at_ = {};
        emit("fps(" + py_str(Value::real(rate)) + ")");
        return;
    }
    // rest(1) keeps the literal spelling. rest(v) with v holding 0 is rest(0.0).
    if (stmt.name == "rest" && stmt.args.size() == 1 && args.size() == 1) {
        const Expr& src = *stmt.args[0];
        const double seconds = args[0].to_double();
        std::string text = (src.kind == ExprKind::Literal && src.literal.is_int())
                               ? "rest(" + std::to_string(src.literal.as_int()) + ")"
                               : "rest(" + py_float_repr(seconds) + ")";
        if (in_frame_) frame_extra_.push_back(std::move(text));
        else {
            emit(std::move(text));
            // Sleep here too so get_datetime after rest sees wall time advance
            // (consumer rest alone lets the producer race ahead into the inbox).
            pace_rest(seconds);
        }
        return;
    }
    if (stmt.name == "draw_lines" || stmt.name == "move_sprites") {
        auto sink = [&](std::string text) {
            if (in_frame_) frame_extra_.push_back(std::move(text));
            else emit(std::move(text));
        };
        if (stmt.name == "draw_lines") expand_draw_lines(args, in_frame_, batch_, sink);
        else expand_move_sprites(args, in_frame_, sprites_, sink);
        return;
    }
    if (in_frame_ && kDrawCommands.count(stmt.name)) {
        // Display plot/mplot drops a point outside the 64x64 panel. A negative
        // coordinate is not clamped onto the edge.
        if ((stmt.name == "plot" || stmt.name == "mplot") && args.size() >= 2 && args[0].is_number() &&
            args[1].is_number()) {
            int64_t x = static_cast<int64_t>(std::trunc(args[0].to_double()));
            int64_t y = static_cast<int64_t>(std::trunc(args[1].to_double()));
            if (x < 0 || y < 0 || x >= kPanelWidth || y >= kPanelHeight) return;
        }
        // The legacy compiled draw loop truncates coordinates with int(float)
        // before packing. Lines and arcs otherwise round, which walks a value
        // just under an integer onto the next pixel.
        if (stmt.name == "draw_line" || stmt.name == "draw_arc") {
            for (size_t i = 0; i < 4 && i < args.size(); ++i) {
                if (args[i].is_number()) args[i] = py_int(args[i]);
            }
        }
        batch_.append(stmt.name, args);
        return;
    }
    // Outside a frame, mplot still waits for mflush. It is never sent as itself.
    if (stmt.name == "mplot") {
        if (args.size() >= 2 && args[0].is_number() && args[1].is_number()) {
            int64_t x = static_cast<int64_t>(std::trunc(args[0].to_double()));
            int64_t y = static_cast<int64_t>(std::trunc(args[1].to_double()));
            if (x < 0 || y < 0 || x >= kPanelWidth || y >= kPanelHeight) return;
        }
        batch_.append(stmt.name, args);
        return;
    }
    if (kSpriteOps.count(stmt.name)) {
        if (in_frame_) {
            sprites_.append(stmt.name, args);
            return;
        }
    }
    std::string text = format_command(stmt.name, args);
    if (in_frame_) frame_extra_.push_back(std::move(text));
    else emit(std::move(text));
}

}  // namespace pixil
