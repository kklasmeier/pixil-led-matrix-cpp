#include "syntax/script.h"

#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>

#include "syntax/parser.h"

namespace pixil {

namespace {

[[noreturn]] void syntax(int line, const std::string& msg) {
    throw PixilError(ErrorKind::Syntax, "line " + std::to_string(line) + ": " + msg);
}

std::string trim(std::string_view in) {
    size_t b = 0, e = in.size();
    while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
    return std::string(in.substr(b, e - b));
}

// Drop a '#' comment that is not inside a quoted string. Legacy preprocess_lines.
std::string strip_comment(const std::string& line) {
    bool in_single = false, in_double = false, escape = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\' && (in_single || in_double)) {
            escape = true;
            continue;
        }
        if (!in_single && !in_double && c == 'f' && i + 1 < line.size() &&
            (line[i + 1] == '"' || line[i + 1] == '\'')) {
            in_double = line[i + 1] == '"';
            in_single = !in_double;
            ++i;
            continue;
        }
        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) in_double = !in_double;
        else if (c == '#' && !in_single && !in_double) return trim(line.substr(0, i));
    }
    return trim(line);
}

bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool starts_with(const std::string& s, std::string_view p) { return s.rfind(p, 0) == 0; }

// `elseif` is the documented spelling. `elsif` is the same clause; two scripts use it.
bool is_elseif(const std::string& t) {
    if (t.size() < 5 || t.compare(t.size() - 5, 5, " then") != 0) return false;
    return starts_with(t, "elseif ") || starts_with(t, "elsif ");
}

std::string_view elseif_keyword(const std::string& t) {
    return starts_with(t, "elsif ") ? std::string_view("elsif ") : std::string_view("elseif ");
}

// `}` closes a procedure. `}1111` on Space_Invaders.pix line 673 is the same closer with junk after it.
bool closes_procedure(const std::string& t) {
    return t == "}" || (t.size() > 1 && t[0] == '}' && !std::isalpha(static_cast<unsigned char>(t[1])));
}

// First '=' that is an assignment, not '==', '!=', '<=', '>='.
size_t find_assign(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '=') continue;
        if (i > 0 && (s[i - 1] == '!' || s[i - 1] == '<' || s[i - 1] == '>' || s[i - 1] == '=')) continue;
        if (i + 1 < s.size() && s[i + 1] == '=') continue;
        return i;
    }
    return std::string::npos;
}

std::vector<std::string> split_args(const std::string& s, int line) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    bool in_single = false, in_double = false, escape = false;
    for (char c : s) {
        if (escape) {
            cur.push_back(c);
            escape = false;
            continue;
        }
        if (c == '\\' && (in_single || in_double)) {
            cur.push_back(c);
            escape = true;
            continue;
        }
        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) in_double = !in_double;
        if (!in_single && !in_double) {
            if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') --depth;
            else if (c == ',' && depth == 0) {
                out.push_back(trim(cur));
                cur.clear();
                continue;
            }
        }
        cur.push_back(c);
    }
    if (in_single || in_double || depth != 0) syntax(line, "unbalanced arguments");
    if (!cur.empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}

ExprPtr parse_arg(const std::string& arg, int line) {
    if (arg.empty()) syntax(line, "empty argument");
    try {
        return parse_expression(arg, line);
    } catch (const PixilError&) {
        bool bare = is_ident_char(arg[0]);
        for (char c : arg)
            if (!is_ident_char(c)) bare = false;
        if (!bare) throw;
        // Bare word the expression parser rejected (a hyphenated name, later).
        ExprPtr e = std::make_unique<Expr>();
        e->kind = ExprKind::Name;
        e->name = arg;
        e->line = line;
        return e;
    }
}

struct Parser {
    struct Line {
        int number;
        std::string text;
    };
    std::vector<Line> lines;
    size_t i = 0;

    explicit Parser(std::string_view source) {
        std::string all(source);
        std::istringstream in(all);
        std::string raw;
        int n = 0;
        while (std::getline(in, raw)) {
            ++n;
            std::string text = strip_comment(raw);
            if (!text.empty()) lines.push_back({n, std::move(text)});
        }
    }

    bool more() const { return i < lines.size(); }
    const Line& cur() const { return lines[i]; }

    bool at(std::string_view exact) const { return more() && cur().text == exact; }

    std::vector<StmtPtr> parse_until(const std::function<bool(const std::string&)>& stop) {
        std::vector<StmtPtr> body;
        while (more() && !stop(cur().text)) body.push_back(parse_one());
        return body;
    }

    StmtPtr parse_one() {
        const Line& ln = cur();
        const std::string& t = ln.text;
        if (starts_with(t, "if ") && t.size() > 8 && t.rfind(" then") == t.size() - 5) return parse_if();
        if (starts_with(t, "for ")) return parse_for();
        if (starts_with(t, "while ") && t.size() > 10 && t.rfind(" then") == t.size() - 5) return parse_while();
        if (t == "break") {
            ++i;
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::Break;
            s->line = ln.number;
            return s;
        }
        if (t == "begin_frame" || starts_with(t, "begin_frame(")) return parse_begin_frame();
        if (t == "end_frame" || t == "end_frame()") {
            ++i;
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::EndFrame;
            s->line = ln.number;
            return s;
        }
        if (starts_with(t, "v_")) {
            size_t eq = find_assign(t);
            if (eq != std::string::npos && t.find('[') != std::string::npos && t.find('[') < eq) return parse_array_assign();
            if (eq != std::string::npos) return parse_assign();
        }
        if (starts_with(t, "def ")) return parse_def();
        if (starts_with(t, "grid_program ") || starts_with(t, "field_program ")) return parse_program();
        if (starts_with(t, "call ")) return parse_call();
        if (starts_with(t, "import ")) return parse_import();
        if (starts_with(t, "create_array(")) return parse_create_array();
        if (starts_with(t, "print(") && t.back() == ')') return parse_print();
        if (t == "else" || t == "endif" || t == "endwhile" || closes_procedure(t) || is_elseif(t) ||
            starts_with(t, "endfor ")) {
            syntax(ln.number, "unexpected '" + t + "'");
        }
        // Legacy accepts a bare word (`clear`) as a zero-argument command.
        if (!t.empty() && (std::isalpha(static_cast<unsigned char>(t[0])) || t[0] == '_')) {
            bool bare = true;
            for (char c : t) {
                if (!is_ident_char(c)) {
                    bare = false;
                    break;
                }
            }
            if (bare) {
                ++i;
                auto s = std::make_unique<Stmt>();
                s->kind = StmtKind::Command;
                s->line = ln.number;
                s->name = t;
                s->bare = true;
                return s;
            }
        }
        return parse_command();
    }

    StmtPtr parse_array_assign() {
        const Line ln = cur();
        ++i;
        size_t eq = find_assign(ln.text);
        std::string lhs = trim(ln.text.substr(0, eq));
        std::string rhs = trim(ln.text.substr(eq + 1));
        size_t lb = lhs.find('[');
        size_t rb = lhs.rfind(']');
        if (lb == std::string::npos || rb == std::string::npos || rb + 1 != lhs.size() || rhs.empty()) {
            syntax(ln.number, "expected v_name[index] = value");
        }
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::ArrayAssign;
        s->line = ln.number;
        s->name = trim(lhs.substr(0, lb));
        s->args.push_back(parse_arg(lhs.substr(lb + 1, rb - lb - 1), ln.number));
        s->expr = parse_expression(rhs, ln.number);
        return s;
    }

    StmtPtr parse_create_array() {
        const Line ln = cur();
        ++i;
        auto lp = ln.text.find('(');
        std::string inside = ln.text.substr(lp + 1, ln.text.size() - lp - 2);
        auto parts = split_args(inside, ln.number);
        if (parts.size() < 2 || parts.size() > 3) syntax(ln.number, "create_array(name, size) or create_array(name, size, type)");
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::CreateArray;
        s->line = ln.number;
        s->name = parts[0];
        s->expr = parse_arg(parts[1], ln.number);
        if (parts.size() == 3) {
            if (parts[2] == "string") s->string_array = true;
            else if (parts[2] != "numeric") syntax(ln.number, "array type must be numeric or string");
        }
        return s;
    }

    static bool is_path_segment(const std::string& name) {
        if (name.empty()) return false;
        const unsigned char first = static_cast<unsigned char>(name[0]);
        if (!std::isalpha(first) && name[0] != '_') return false;
        for (char c : name) {
            if (!is_ident_char(c)) return false;
        }
        return true;
    }

    static bool is_import_path(const std::string& name) {
        if (name.empty() || name.front() == '/' || name.back() == '/') return false;
        size_t start = 0;
        for (;;) {
            const auto slash = name.find('/', start);
            const std::string seg = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (!is_path_segment(seg)) return false;
            if (slash == std::string::npos) return true;
            start = slash + 1;
        }
    }

    static bool is_proc_name(const std::string& name) {
        if (name.empty() || name.front() == '.' || name.back() == '.') return false;
        bool start = true;
        for (char c : name) {
            if (c == '.') {
                if (start) return false;
                start = true;
                continue;
            }
            if (!is_ident_char(c)) return false;
            if (start && !std::isalpha(static_cast<unsigned char>(c)) && c != '_') return false;
            start = false;
        }
        return !start;
    }

    StmtPtr parse_program() {
        const Line ln = cur();
        ++i;
        const bool grid = starts_with(ln.text, "grid_program ");
        const std::string keyword = grid ? "grid_program " : "field_program ";
        if (ln.text.back() != '{') syntax(ln.number, "expected '" + keyword + "name {'");
        std::string name = trim(ln.text.substr(keyword.size(), ln.text.size() - keyword.size() - 1));
        if (name.empty()) syntax(ln.number, "expected a program name");
        auto s = std::make_unique<Stmt>();
        s->kind = grid ? StmtKind::GridProgram : StmtKind::FieldProgram;
        s->line = ln.number;
        s->name = name;
        int depth = 1;
        while (more()) {
            const std::string& text = cur().text;
            int opens = 0, closes = 0;
            for (char c : text) {
                if (c == '{') ++opens;
                else if (c == '}') ++closes;
            }
            depth += opens - closes;
            ++i;
            if (depth <= 0) break;
            s->text.push_back(text);
        }
        if (depth > 0) syntax(ln.number, "unclosed program");
        return s;
    }

    StmtPtr parse_def() {
        const Line ln = cur();
        ++i;
        const std::string& t = ln.text;
        if (t.back() != '{') syntax(ln.number, "expected 'def name {'");
        std::string header = trim(t.substr(4, t.size() - 5));
        std::string name = header;
        std::vector<std::string> params;
        auto lp = header.find('(');
        if (lp != std::string::npos) {
            if (header.back() != ')') syntax(ln.number, "expected ')' after procedure parameters");
            name = trim(header.substr(0, lp));
            std::string inside = header.substr(lp + 1, header.size() - lp - 2);
            if (!trim(inside).empty()) params = split_args(inside, ln.number);
        }
        if (!is_proc_name(name) || name.find('.') != std::string::npos) {
            syntax(ln.number, "invalid procedure name '" + name + "'");
        }
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Proc;
        s->line = ln.number;
        s->name = name;
        for (const auto& param : params) {
            if (param.rfind("v_", 0) != 0) syntax(ln.number, "parameter '" + param + "' must start with v_");
            s->args.push_back(parse_expression(param, ln.number));
        }
        s->body = parse_until([](const std::string& text) { return closes_procedure(text); });
        if (!more() || !closes_procedure(cur().text)) syntax(ln.number, "procedure '" + name + "' is missing '}'");
        ++i;
        return s;
    }

    StmtPtr parse_call() {
        const Line ln = cur();
        ++i;
        std::string rest = trim(ln.text.substr(5));
        std::string name = rest;
        std::vector<std::string> args;
        auto lp = rest.find('(');
        if (lp != std::string::npos) {
            if (rest.back() != ')') syntax(ln.number, "expected ')' after call arguments");
            name = trim(rest.substr(0, lp));
            std::string inside = rest.substr(lp + 1, rest.size() - lp - 2);
            if (!trim(inside).empty()) args = split_args(inside, ln.number);
        }
        if (!is_proc_name(name)) syntax(ln.number, "invalid procedure name '" + name + "'");
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Call;
        s->line = ln.number;
        s->name = name;
        for (const auto& arg : args) s->args.push_back(parse_arg(arg, ln.number));
        return s;
    }

    StmtPtr parse_import() {
        const Line ln = cur();
        ++i;
        std::string name = trim(ln.text.substr(7));
        if (!is_import_path(name)) syntax(ln.number, "import expects a library path");
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Import;
        s->line = ln.number;
        s->name = name;
        return s;
    }

    StmtPtr parse_assign() {
        const Line ln = cur();
        ++i;
        size_t eq = find_assign(ln.text);
        std::string name = trim(ln.text.substr(0, eq));
        std::string rhs = trim(ln.text.substr(eq + 1));
        if (name.empty() || rhs.empty()) syntax(ln.number, "incomplete assignment");
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Assign;
        s->line = ln.number;
        s->name = name;
        s->expr = parse_expression(rhs, ln.number);
        return s;
    }

    StmtPtr parse_if() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::If;
        s->line = cur().number;
        auto take_cond = [&](std::string_view keyword) {
            const std::string& t = cur().text;
            std::string cond = trim(t.substr(keyword.size(), t.size() - keyword.size() - 5));
            if (cond.empty()) syntax(cur().number, "missing condition");
            ++i;
            return parse_expression(cond, s->line);
        };
        IfClause first;
        first.cond = take_cond("if ");
        first.body = parse_until([](const std::string& t) { return t == "else" || t == "endif" || is_elseif(t); });
        s->clauses.push_back(std::move(first));
        while (more() && is_elseif(cur().text)) {
            IfClause c;
            c.cond = take_cond(elseif_keyword(cur().text));
            c.body = parse_until([](const std::string& t) { return t == "else" || t == "endif" || is_elseif(t); });
            s->clauses.push_back(std::move(c));
        }
        if (at("else")) {
            ++i;
            IfClause c;
            c.body = parse_until([](const std::string& t) { return t == "endif"; });
            s->clauses.push_back(std::move(c));
        }
        if (!at("endif")) syntax(s->line, "if block is missing endif");
        ++i;
        return s;
    }

    StmtPtr parse_for() {
        const Line ln = cur();
        ++i;
        // for v_t in (start, end, step)   — a trailing " then" is accepted and ignored
        std::string t = ln.text;
        if (t.size() >= 5 && t.compare(t.size() - 5, 5, " then") == 0) t = trim(t.substr(0, t.size() - 5));
        auto sp = t.find(' ');
        auto in_pos = t.find(" in ");
        auto lp = t.find('(');
        if (sp == std::string::npos || in_pos == std::string::npos || lp == std::string::npos || t.back() != ')') {
            syntax(ln.number, "expected 'for v_name in (start, end, step)'");
        }
        std::string name = trim(t.substr(sp, in_pos - sp));
        std::string inside = t.substr(lp + 1, t.size() - lp - 2);
        auto parts = split_args(inside, ln.number);
        if (parts.size() != 3) syntax(ln.number, "for loop needs start, end, and step");
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::For;
        s->line = ln.number;
        s->name = name;
        for (const auto& p : parts) s->args.push_back(parse_arg(p, ln.number));
        const std::string end_line = "endfor " + name;
        s->body = parse_until([&](const std::string& text) { return text == end_line; });
        if (!at(end_line)) syntax(ln.number, "for block is missing '" + end_line + "'");
        ++i;
        return s;
    }

    StmtPtr parse_while() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::While;
        s->line = cur().number;
        const std::string& t = cur().text;
        std::string cond = trim(t.substr(6, t.size() - 6 - 5));
        if (cond.empty()) syntax(s->line, "missing condition");
        ++i;
        s->expr = parse_expression(cond, s->line);
        s->body = parse_until([](const std::string& text) { return text == "endwhile"; });
        if (!at("endwhile")) syntax(s->line, "while block is missing endwhile");
        ++i;
        return s;
    }

    StmtPtr parse_begin_frame() {
        const Line ln = cur();
        ++i;
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::BeginFrame;
        s->line = ln.number;
        if (ln.text == "begin_frame" || ln.text == "begin_frame()") return s;
        auto lp = ln.text.find('(');
        std::string arg = trim(ln.text.substr(lp + 1, ln.text.size() - lp - 2));
        std::string lower = arg;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "true") s->preserve = true;
        else if (lower != "false") syntax(ln.number, "begin_frame argument must be true or false");
        return s;
    }

    // print is producer-side text. Keep the raw argument, including f-strings,
    // instead of parsing it as a consumer command.
    StmtPtr parse_print() {
        const Line ln = cur();
        ++i;
        auto lp = ln.text.find('(');
        std::string inner = trim(ln.text.substr(lp + 1, ln.text.size() - lp - 2));
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Command;
        s->line = ln.number;
        s->name = "print";
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Literal;
        e->literal = Value::str(inner);
        e->line = ln.number;
        s->args.push_back(std::move(e));
        return s;
    }

    StmtPtr parse_command() {
        const Line ln = cur();
        ++i;
        const std::string& t = ln.text;
        auto lp = t.find('(');
        if (lp == std::string::npos || t.back() != ')') syntax(ln.number, "expected a command or assignment");
        std::string name = trim(t.substr(0, lp));
        if (name.empty()) syntax(ln.number, "missing command name");
        for (char c : name)
            if (!is_ident_char(c)) syntax(ln.number, "invalid command name '" + name + "'");
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Command;
        s->line = ln.number;
        s->name = name;
        std::string inside = t.substr(lp + 1, t.size() - lp - 2);
        if (!trim(inside).empty()) {
            for (const auto& arg : split_args(inside, ln.number)) {
                if (is_hyphen_word(arg)) {
                    auto e = std::make_unique<Expr>();
                    e->kind = ExprKind::Literal;
                    e->literal = Value::str(arg);
                    e->line = ln.number;
                    s->args.push_back(std::move(e));
                } else {
                    s->args.push_back(parse_arg(arg, ln.number));
                }
            }
        }
        return s;
    }

    // `piboto-regular`: hyphen-joined words where every part starts with a
    // letter. In a command argument that is a name (font, color), not subtraction.
    static bool is_hyphen_word(const std::string& a) {
        if (a.find('-') == std::string::npos || starts_with(a, "v_")) return false;
        bool part_start = true;
        for (char c : a) {
            if (c == '-') {
                if (part_start) return false;
                part_start = true;
                continue;
            }
            if (part_start && !std::isalpha(static_cast<unsigned char>(c))) return false;
            if (!is_ident_char(c)) return false;
            part_start = false;
        }
        return !part_start;
    }
};

bool is_bare_word(const Expr& e) { return e.kind == ExprKind::Unknown && !e.name.empty(); }

void soften_command_words(Stmt& stmt) {
    if (stmt.kind == StmtKind::Command) {
        for (auto& arg : stmt.args) {
            if (is_bare_word(*arg)) {
                arg->kind = ExprKind::Literal;
                arg->literal = Value::str(arg->name);
            }
        }
    }
    for (auto& clause : stmt.clauses)
        for (auto& child : clause.body) soften_command_words(*child);
    for (auto& child : stmt.body) soften_command_words(*child);
}

void resolve_stmt(Stmt& stmt, SymbolTable& symbols) {
    if (stmt.expr) resolve(*stmt.expr, symbols);
    for (auto& arg : stmt.args) resolve(*arg, symbols);
    if (stmt.kind == StmtKind::Assign || stmt.kind == StmtKind::For || stmt.kind == StmtKind::ArrayAssign ||
        stmt.kind == StmtKind::CreateArray) {
        if (stmt.name.rfind("v_", 0) != 0) syntax(stmt.line, "variable must start with v_");
        stmt.slot = symbols.intern(stmt.name);
    }
    if (stmt.kind == StmtKind::Proc) {
        for (const auto& arg : stmt.args) {
            if (arg->kind != ExprKind::Var) syntax(stmt.line, "parameter must be a v_ variable");
            stmt.params.push_back(arg->slot);
        }
    }
    for (auto& clause : stmt.clauses) {
        if (clause.cond) resolve(*clause.cond, symbols);
        for (auto& child : clause.body) resolve_stmt(*child, symbols);
    }
    for (auto& child : stmt.body) resolve_stmt(*child, symbols);
    soften_command_words(stmt);
}

}  // namespace

Script parse_script(std::string_view source) {
    Parser parser(source);
    Script script;
    script.body = parser.parse_until([](const std::string&) { return false; });
    if (parser.more()) syntax(parser.cur().number, "unexpected '" + parser.cur().text + "'");
    return script;
}

Script parse_script_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw PixilError(ErrorKind::Value, "cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_script(ss.str());
}

void resolve_script(Script& script, SymbolTable& symbols) {
    for (auto& stmt : script.body) resolve_stmt(*stmt, symbols);
}

}  // namespace pixil
