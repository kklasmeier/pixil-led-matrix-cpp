#include "runtime/grid.h"

#include "runtime/machine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

#include "runtime/python_ops.h"

namespace pixil {

namespace {

constexpr int kMtN = 624;
constexpr int kMtM = 397;

struct NumpyRng {
    uint32_t mt[kMtN]{};
    int mti = kMtN;

    void seed(uint32_t s) {
        for (int pos = 0; pos < kMtN; ++pos) {
            mt[pos] = s;
            s = 1812433253U * (s ^ (s >> 30)) + static_cast<uint32_t>(pos + 1);
        }
        mti = kMtN;
    }

    uint32_t next() {
        static constexpr uint32_t mag01[2] = {0U, 0x9908b0dfU};
        if (mti >= kMtN) {
            int kk = 0;
            uint32_t y;
            for (; kk < kMtN - kMtM; ++kk) {
                y = (mt[kk] & 0x80000000U) | (mt[kk + 1] & 0x7fffffffU);
                mt[kk] = mt[kk + kMtM] ^ (y >> 1) ^ mag01[y & 1U];
            }
            for (; kk < kMtN - 1; ++kk) {
                y = (mt[kk] & 0x80000000U) | (mt[kk + 1] & 0x7fffffffU);
                mt[kk] = mt[kk + (kMtM - kMtN)] ^ (y >> 1) ^ mag01[y & 1U];
            }
            y = (mt[kMtN - 1] & 0x80000000U) | (mt[0] & 0x7fffffffU);
            mt[kMtN - 1] = mt[kMtM - 1] ^ (y >> 1) ^ mag01[y & 1U];
            mti = 0;
        }
        uint32_t y = mt[mti++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680U;
        y ^= (y << 15) & 0xefc60000U;
        y ^= (y >> 18);
        return y;
    }

    double sample() {
        uint32_t a = next() >> 5;
        uint32_t b = next() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }

    double uniform(double lo, double hi) { return lo + (hi - lo) * sample(); }
};

NumpyRng g_numpy;

struct Node {
    enum Kind { Num, Ident, Neg, Not, And, Or, Cmp, Bin, Call } kind = Num;
    std::string text;
    double number = 0;
    std::vector<Node> kids;
};

struct Num {
    bool array = false;
    double scalar = 0;
    std::vector<double> data;

    static Num of(double s) { return Num{false, s, {}}; }
    static Num of_array(std::vector<double> d) { return Num{true, 0, std::move(d)}; }
};

double at(const Num& n, size_t i) { return n.array ? n.data[i] : n.scalar; }

size_t width_of(const Num& a, const Num& b) {
    if (a.array && b.array && a.data.size() != b.data.size()) {
        throw PixilError(ErrorKind::Value, "grid arrays have different lengths");
    }
    if (a.array) return a.data.size();
    if (b.array) return b.data.size();
    return 0;
}

template <typename Op>
Num apply2(const Num& a, const Num& b, Op op) {
    size_t n = width_of(a, b);
    if (n == 0) return Num::of(op(a.scalar, b.scalar));
    std::vector<double> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = op(at(a, i), at(b, i));
    return Num::of_array(std::move(out));
}

double np_power(double a, double b) {
    if (std::isfinite(b) && b == std::trunc(b) && std::fabs(b) <= 32.0) {
        int e = static_cast<int>(std::trunc(b));
        if (e == 0) return 1.0;
        double r = 1.0;
        double base = a;
        int n = std::abs(e);
        while (n != 0) {
            if ((n & 1) != 0) r *= base;
            base *= base;
            n >>= 1;
        }
        if (e < 0) return r == 0.0 ? 0.0 : 1.0 / r;
        return r;
    }
    if (a < 0.0) return std::numeric_limits<double>::quiet_NaN();
    return std::pow(a, b);
}

int whole(double v) {
    Value r = py_round(Value::real(v));
    if (r.is_int()) return static_cast<int>(r.as_int());
    return static_cast<int>(r.as_float());
}

int truncated(double v) { return static_cast<int>(std::trunc(v)); }

struct Token {
    enum Kind { End, Num, Ident, Op, LParen, RParen, Comma } kind = End;
    std::string text;
    double number = 0;
};

std::vector<Token> tokenize(const std::string& expr) {
    std::vector<Token> out;
    size_t i = 0;
    auto push = [&](Token t) { out.push_back(std::move(t)); };
    while (i < expr.size()) {
        char c = expr[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '(') {
            push({Token::LParen, "(", 0});
            ++i;
            continue;
        }
        if (c == ')') {
            push({Token::RParen, ")", 0});
            ++i;
            continue;
        }
        if (c == ',') {
            push({Token::Comma, ",", 0});
            ++i;
            continue;
        }
        if (i + 1 < expr.size()) {
            std::string two = expr.substr(i, 2);
            if (two == "==" || two == "!=" || two == "<=" || two == ">=") {
                push({Token::Op, two, 0});
                i += 2;
                continue;
            }
        }
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '<' || c == '>' || c == '!') {
            push({Token::Op, std::string(1, c), 0});
            ++i;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < expr.size() && std::isdigit(static_cast<unsigned char>(expr[i + 1])))) {
            size_t j = i;
            while (j < expr.size() && (std::isdigit(static_cast<unsigned char>(expr[j])) || expr[j] == '.')) ++j;
            Token t;
            t.kind = Token::Num;
            t.text = expr.substr(i, j - i);
            t.number = std::stod(t.text);
            push(std::move(t));
            i = j;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < expr.size() && (std::isalnum(static_cast<unsigned char>(expr[j])) || expr[j] == '_')) ++j;
            std::string word = expr.substr(i, j - i);
            if (word == "and" || word == "or" || word == "not") push({Token::Op, word, 0});
            else push({Token::Ident, word, 0});
            i = j;
            continue;
        }
        throw PixilError(ErrorKind::Syntax, "bad grid expression near '" + expr.substr(i) + "'");
    }
    out.push_back(Token{});
    return out;
}

struct Parser {
    std::vector<Token> toks;
    size_t i = 0;
    const Token& peek() const { return toks[i]; }
    const Token& take() { return toks[i++]; }

    Node parse() {
        Node n = parse_or();
        if (peek().kind != Token::End) throw PixilError(ErrorKind::Syntax, "trailing token in grid expression");
        return n;
    }

    Node parse_or() {
        Node left = parse_and();
        while (peek().kind == Token::Op && peek().text == "or") {
            take();
            Node n;
            n.kind = Node::Or;
            n.kids.push_back(std::move(left));
            n.kids.push_back(parse_and());
            left = std::move(n);
        }
        return left;
    }

    Node parse_and() {
        Node left = parse_not();
        while (peek().kind == Token::Op && peek().text == "and") {
            take();
            Node n;
            n.kind = Node::And;
            n.kids.push_back(std::move(left));
            n.kids.push_back(parse_not());
            left = std::move(n);
        }
        return left;
    }

    Node parse_not() {
        if (peek().kind == Token::Op && peek().text == "not") {
            take();
            Node n;
            n.kind = Node::Not;
            n.kids.push_back(parse_not());
            return n;
        }
        return parse_cmp();
    }

    Node parse_cmp() {
        Node left = parse_add();
        while (peek().kind == Token::Op &&
               (peek().text == "==" || peek().text == "!=" || peek().text == "<" || peek().text == ">" ||
                peek().text == "<=" || peek().text == ">=")) {
            std::string op = take().text;
            Node n;
            n.kind = Node::Cmp;
            n.text = op;
            n.kids.push_back(std::move(left));
            n.kids.push_back(parse_add());
            left = std::move(n);
        }
        return left;
    }

    Node parse_add() {
        Node left = parse_mul();
        while (peek().kind == Token::Op && (peek().text == "+" || peek().text == "-")) {
            std::string op = take().text;
            Node n;
            n.kind = Node::Bin;
            n.text = op;
            n.kids.push_back(std::move(left));
            n.kids.push_back(parse_mul());
            left = std::move(n);
        }
        return left;
    }

    Node parse_mul() {
        Node left = parse_unary();
        while (peek().kind == Token::Op && (peek().text == "*" || peek().text == "/")) {
            std::string op = take().text;
            Node n;
            n.kind = Node::Bin;
            n.text = op;
            n.kids.push_back(std::move(left));
            n.kids.push_back(parse_unary());
            left = std::move(n);
        }
        return left;
    }

    Node parse_unary() {
        if (peek().kind == Token::Op && peek().text == "-") {
            take();
            Node n;
            n.kind = Node::Neg;
            n.kids.push_back(parse_unary());
            return n;
        }
        return parse_primary();
    }

    Node parse_primary() {
        const Token& t = peek();
        if (t.kind == Token::Num) {
            Node n;
            n.kind = Node::Num;
            n.number = take().number;
            return n;
        }
        if (t.kind == Token::Ident) {
            Node n;
            n.text = take().text;
            if (peek().kind == Token::LParen) {
                take();
                n.kind = Node::Call;
                if (peek().kind != Token::RParen) {
                    n.kids.push_back(parse_or());
                    while (peek().kind == Token::Comma) {
                        take();
                        n.kids.push_back(parse_or());
                    }
                }
                if (peek().kind != Token::RParen) throw PixilError(ErrorKind::Syntax, "unclosed call");
                take();
                return n;
            }
            n.kind = Node::Ident;
            return n;
        }
        if (t.kind == Token::LParen) {
            take();
            Node n = parse_or();
            if (peek().kind != Token::RParen) throw PixilError(ErrorKind::Syntax, "unclosed parenthesis");
            take();
            return n;
        }
        throw PixilError(ErrorKind::Syntax, "bad grid expression");
    }
};

Node parse_expr(const std::string& text) { return Parser{tokenize(text), 0}.parse(); }

struct DrawSpec {
    std::string mode = "palette";
    std::string palette_field;
    std::string color_base = "1";
    std::string color_scale = "65";
    std::string intensity_base = "28";
    std::string intensity_scale = "72";
    std::string fill_field;
    std::string fill_color = "cyan";
    std::string fill_intensity = "90";
    std::string fire_field;
    Node plot_if;
    bool has_plot = false;
    Node color;
    bool has_color = false;
    Node opacity;
    bool has_opacity = false;
};

struct StepBlock {
    std::string field;
    std::vector<std::pair<std::string, Node>> assigns;
};

struct GridProgram {
    std::string name;
    std::string size = "32";
    std::string cell = "1";
    std::vector<std::string> fields;
    std::string steps = "1";
    std::string boundary = "clamp";
    std::vector<StepBlock> blocks;
    DrawSpec draw;
};

struct FieldProgram {
    std::string name;
    std::string size = "64";
    std::string sites_x;
    std::string sites_y;
    std::string weights;
    std::string phases;
    std::string active;
    std::string mode = "formula";
    std::vector<std::pair<std::string, Node>> value;
    std::string result;
    Node plot_if;
    bool has_plot = false;
    Node color;
    bool has_color = false;
    Node opacity;
    bool has_opacity = false;
    bool signed_field = false;
    std::string peak_color = "cyan";
    std::string trough_color = "blue";
    std::string intensity_scale = "35";
    bool edges = false;
    std::string edge_ratio = "1.05";
    std::string site_colors;
    std::string edge_color = "white";
    std::string edge_opacity = "90";
    std::string fill_opacity = "60";
};

struct Buf {
    std::vector<double> half[2];
    int active = 0;
};

struct Runtime {
    int side = 0;
    std::string boundary;
    std::unordered_map<std::string, Buf> fields;
};

std::string first_word(const std::string& line) {
    size_t sp = line.find_first_of(" \t");
    return sp == std::string::npos ? line : line.substr(0, sp);
}

std::string rest_of(const std::string& line) {
    size_t sp = line.find_first_of(" \t");
    if (sp == std::string::npos) return "";
    size_t b = line.find_first_not_of(" \t", sp);
    return b == std::string::npos ? "" : line.substr(b);
}

size_t assign_at(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '=') continue;
        if (i > 0 && (s[i - 1] == '!' || s[i - 1] == '<' || s[i - 1] == '>' || s[i - 1] == '=')) continue;
        if (i + 1 < s.size() && s[i + 1] == '=') continue;
        return i;
    }
    return std::string::npos;
}

std::vector<std::pair<std::string, Node>> parse_assigns(const std::vector<std::string>& body) {
    std::vector<std::pair<std::string, Node>> out;
    for (const auto& line : body) {
        size_t eq = assign_at(line);
        if (eq == std::string::npos) throw PixilError(ErrorKind::Syntax, "expected assignment, got: " + line);
        std::string name = line.substr(0, eq);
        size_t e = name.find_last_not_of(" \t");
        name = name.substr(0, e + 1);
        std::string expr = line.substr(eq + 1);
        size_t b = expr.find_first_not_of(" \t");
        expr = b == std::string::npos ? "" : expr.substr(b);
        out.emplace_back(name, parse_expr(expr));
    }
    return out;
}

std::vector<std::string> block_body(const std::vector<std::string>& lines, size_t& i) {
    std::vector<std::string> body;
    ++i;
    while (i < lines.size()) {
        if (lines[i] == "}") {
            ++i;
            return body;
        }
        body.push_back(lines[i]);
        ++i;
    }
    throw PixilError(ErrorKind::Syntax, "unclosed grid block");
}

DrawSpec parse_draw(const std::vector<std::string>& body) {
    DrawSpec spec;
    for (const auto& line : body) {
        std::string key = first_word(line);
        std::string rest = rest_of(line);
        if (key == "palette_field") {
            spec.mode = "palette";
            spec.palette_field = rest;
        } else if (key == "color_base") spec.color_base = rest;
        else if (key == "color_scale") spec.color_scale = rest;
        else if (key == "intensity_base") spec.intensity_base = rest;
        else if (key == "intensity_scale") spec.intensity_scale = rest;
        else if (key == "fill_field") {
            spec.mode = "fill";
            spec.fill_field = rest;
        } else if (key == "fill_color") spec.fill_color = rest;
        else if (key == "fill_intensity") spec.fill_intensity = rest;
        else if (key == "fire_field") {
            spec.mode = "fire";
            spec.fire_field = rest;
        } else if (key == "plot_if") {
            spec.mode = "expr";
            spec.plot_if = parse_expr(rest);
            spec.has_plot = true;
        } else if (key == "color") {
            spec.color = parse_expr(rest);
            spec.has_color = true;
        } else if (key == "opacity") {
            spec.opacity = parse_expr(rest);
            spec.has_opacity = true;
        } else {
            throw PixilError(ErrorKind::Syntax, "unknown draw directive: " + key);
        }
    }
    return spec;
}

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

GridProgram compile_grid(const std::string& name, const std::vector<std::string>& lines) {
    GridProgram program;
    program.name = name;
    size_t i = 0;
    while (i < lines.size()) {
        const std::string& line = lines[i];
        if (starts(line, "size ")) program.size = rest_of(line);
        else if (starts(line, "cell ")) program.cell = rest_of(line);
        else if (starts(line, "fields ")) {
            std::string raw = rest_of(line);
            std::string cur;
            for (char c : raw) {
                if (c == ',') {
                    if (!cur.empty()) program.fields.push_back(cur);
                    cur.clear();
                } else if (c != ' ' && c != '\t') cur.push_back(c);
            }
            if (!cur.empty()) program.fields.push_back(cur);
        } else if (starts(line, "steps ")) program.steps = rest_of(line);
        else if (starts(line, "boundary ")) program.boundary = rest_of(line);
        else if (starts(line, "step ")) {
            std::string header = rest_of(line);
            size_t sp = header.find(' ');
            std::string field = sp == std::string::npos ? header : header.substr(0, sp);
            if (!field.empty() && field.back() == '{') field.pop_back();
            while (!field.empty() && (field.back() == ' ' || field.back() == '\t')) field.pop_back();
            auto body = block_body(lines, i);
            program.blocks.push_back(StepBlock{field, parse_assigns(body)});
            continue;
        } else if (line == "draw {" || starts(line, "draw ")) {
            auto body = block_body(lines, i);
            program.draw = parse_draw(body);
            continue;
        } else {
            throw PixilError(ErrorKind::Syntax, "unknown grid_program directive: " + line);
        }
        ++i;
    }
    if (program.fields.empty()) throw PixilError(ErrorKind::Value, "grid_program " + name + ": fields required");
    if (program.blocks.empty()) throw PixilError(ErrorKind::Value, "grid_program " + name + ": step required");
    return program;
}

FieldProgram compile_field(const std::string& name, const std::vector<std::string>& lines) {
    FieldProgram program;
    program.name = name;
    size_t i = 0;
    while (i < lines.size()) {
        const std::string& line = lines[i];
        if (starts(line, "size ")) program.size = rest_of(line);
        else if (starts(line, "sites ")) {
            std::string raw = rest_of(line);
            std::vector<std::string> parts;
            std::string cur;
            for (char c : raw) {
                if (c == ',') {
                    if (!cur.empty()) parts.push_back(cur);
                    cur.clear();
                } else if (c != ' ' && c != '\t') cur.push_back(c);
            }
            if (!cur.empty()) parts.push_back(cur);
            if (parts.size() != 2) throw PixilError(ErrorKind::Value, "sites requires x and y arrays");
            program.sites_x = parts[0];
            program.sites_y = parts[1];
        } else if (starts(line, "weights ")) program.weights = rest_of(line);
        else if (starts(line, "phases ")) program.phases = rest_of(line);
        else if (starts(line, "active ")) program.active = rest_of(line);
        else if (starts(line, "signed ")) program.signed_field = rest_of(line) == "true";
        else if (starts(line, "peak_color ")) program.peak_color = rest_of(line);
        else if (starts(line, "trough_color ")) program.trough_color = rest_of(line);
        else if (starts(line, "intensity_scale ")) program.intensity_scale = rest_of(line);
        else if (starts(line, "mode ")) program.mode = rest_of(line);
        else if (starts(line, "plot_if ")) {
            program.plot_if = parse_expr(rest_of(line));
            program.has_plot = true;
        } else if (starts(line, "color ")) {
            program.color = parse_expr(rest_of(line));
            program.has_color = true;
        } else if (starts(line, "opacity ")) {
            program.opacity = parse_expr(rest_of(line));
            program.has_opacity = true;
        } else if (starts(line, "edges ")) program.edges = rest_of(line) == "true";
        else if (starts(line, "edge_ratio ")) program.edge_ratio = rest_of(line);
        else if (starts(line, "site_colors ")) program.site_colors = rest_of(line);
        else if (starts(line, "edge_color ")) program.edge_color = rest_of(line);
        else if (starts(line, "edge_opacity ")) program.edge_opacity = rest_of(line);
        else if (starts(line, "fill_opacity ")) program.fill_opacity = rest_of(line);
        else if (line == "value {" || starts(line, "value ")) {
            auto body = block_body(lines, i);
            program.value = parse_assigns(body);
            if (!program.value.empty()) program.result = program.value.back().first;
            continue;
        } else {
            throw PixilError(ErrorKind::Syntax, "unknown field_program directive: " + line);
        }
        ++i;
    }
    if (program.sites_x.empty() || program.sites_y.empty()) {
        throw PixilError(ErrorKind::Value, "field_program " + name + ": sites required");
    }
    return program;
}

Value& slot_value(Env& env, const std::string& name) {
    int slot = env.symbols.find(name);
    if (slot < 0 || static_cast<size_t>(slot) >= env.slots.size() || env.slots[static_cast<size_t>(slot)].is_none()) {
        throw PixilError(ErrorKind::Name, "Variable '" + name + "' not found");
    }
    return env.slots[static_cast<size_t>(slot)];
}

PixArray& need_array(Env& env, const std::string& name) {
    Value& v = slot_value(env, name);
    if (!v.is_array() || v.as_array()->type != ArrayType::Numeric) {
        throw PixilError(ErrorKind::Value, name + " is not a numeric array");
    }
    return *v.as_array();
}

std::vector<double> read_array(const PixArray& arr) {
    std::vector<double> out(arr.data.size());
    for (size_t i = 0; i < arr.data.size(); ++i) out[i] = arr.data[i].to_double();
    return out;
}

void write_array(PixArray& arr, const std::vector<double>& data) {
    if (arr.data.size() != data.size()) throw PixilError(ErrorKind::Value, "field length changed");
    for (size_t i = 0; i < data.size(); ++i) arr.data[i] = Value::real(data[i]);
}

std::unordered_map<std::string, double> scalars_of(Env& env) {
    std::unordered_map<std::string, double> out;
    for (size_t i = 0; i < env.slots.size() && i < env.symbols.size(); ++i) {
        const Value& v = env.slots[i];
        if (v.is_number()) out.emplace(env.symbols.name_of(static_cast<int>(i)), v.to_double());
    }
    return out;
}

double resolve_scalar(const std::string& text, const std::unordered_map<std::string, double>& scalars) {
    if (!text.empty() && (std::isdigit(static_cast<unsigned char>(text[0])) || text[0] == '-' || text[0] == '.')) {
        try {
            size_t used = 0;
            double v = std::stod(text, &used);
            if (used == text.size()) return v;
        } catch (const std::exception&) {
        }
    }
    auto found = scalars.find(text);
    if (found == scalars.end()) throw PixilError(ErrorKind::Name, "cannot resolve " + text);
    return found->second;
}

std::string resolve_color(const std::string& text, Env& env) {
    if (text.rfind("v_", 0) == 0) {
        const Value& v = slot_value(env, text);
        if (v.is_str()) {
            std::string s = v.as_str();
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
            return s;
        }
    }
    return text;
}

struct EvalCtx {
    const std::unordered_map<std::string, double>* scalars = nullptr;
    const std::unordered_map<std::string, std::vector<double>>* fields = nullptr;
    std::unordered_map<std::string, std::vector<double>>* temps = nullptr;
    const std::unordered_map<std::string, std::vector<double>>* grid_vars = nullptr;
    const std::unordered_map<std::string, Num>* site_vars = nullptr;
    int side = 0;
    std::string boundary;
    bool in_field = false;
    const std::vector<double>* px = nullptr;
    const std::vector<double>* py = nullptr;
    const std::vector<double>* weights = nullptr;
    const std::vector<double>* phases = nullptr;
    const std::vector<double>* active = nullptr;
};

const std::vector<double>* find_vec(const std::unordered_map<std::string, std::vector<double>>* map, const std::string& name) {
    if (map == nullptr) return nullptr;
    auto it = map->find(name);
    return it == map->end() ? nullptr : &it->second;
}

Num lookup(const std::string& name, const EvalCtx& ctx) {
    if (ctx.temps != nullptr) {
        auto it = ctx.temps->find(name);
        if (it != ctx.temps->end()) return Num::of_array(it->second);
    }
    if (ctx.scalars != nullptr) {
        auto it = ctx.scalars->find(name);
        if (it != ctx.scalars->end()) return Num::of(it->second);
    }
    if (const auto* g = find_vec(ctx.grid_vars, name)) return Num::of_array(*g);
    if (ctx.site_vars != nullptr) {
        auto it = ctx.site_vars->find(name);
        if (it != ctx.site_vars->end()) return it->second;
    }
    if (const auto* f = find_vec(ctx.fields, name)) return Num::of_array(*f);
    throw PixilError(ErrorKind::Name, "unknown identifier in grid expression: " + name);
}

std::vector<double> lap4(const std::vector<double>& field, int side, const std::string& boundary) {
    auto atg = [&](int y, int x) { return field[static_cast<size_t>(y * side + x)]; };
    std::vector<double> out(static_cast<size_t>(side * side));
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            int yu, yd, xl, xr;
            if (boundary == "wrap") {
                yu = (y + side - 1) % side;
                yd = (y + 1) % side;
                xl = (x + side - 1) % side;
                xr = (x + 1) % side;
            } else {
                yu = y == 0 ? 0 : y - 1;
                yd = y == side - 1 ? side - 1 : y + 1;
                xl = x == 0 ? 0 : x - 1;
                xr = x == side - 1 ? side - 1 : x + 1;
            }
            double c = atg(y, x);
            out[static_cast<size_t>(y * side + x)] = atg(y, xl) + atg(y, xr) + atg(yu, x) + atg(yd, x) - 4.0 * c;
        }
    }
    return out;
}

std::vector<double> neighbor_count(const std::vector<double>& field, int side, const std::string& boundary, bool orthogonal) {
    std::vector<double> alive(static_cast<size_t>(side * side));
    for (size_t i = 0; i < alive.size(); ++i) alive[i] = field[i] > 0.5 ? 1.0 : 0.0;
    auto sample = [&](int y, int x) {
        if (boundary == "wrap") {
            y = (y + side) % side;
            x = (x + side) % side;
        } else {
            y = std::clamp(y, 0, side - 1);
            x = std::clamp(x, 0, side - 1);
        }
        return alive[static_cast<size_t>(y * side + x)];
    };
    const int shifts[8][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    int nshift = orthogonal ? 4 : 8;
    std::vector<double> out(alive.size(), 0.0);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            double total = 0;
            for (int s = 0; s < nshift; ++s) total += sample(y + shifts[s][0], x + shifts[s][1]);
            out[static_cast<size_t>(y * side + x)] = total;
        }
    }
    return out;
}

std::vector<double> below_avg(const std::vector<double>& field, int side) {
    std::vector<double> out = field;
    for (int y = 0; y < side - 1; ++y) {
        int by = y + 1;
        for (int x = 0; x < side; ++x) {
            double total = field[static_cast<size_t>(by * side + x)];
            int count = 1;
            if (x > 0) {
                total += field[static_cast<size_t>(by * side + (x - 1))];
                ++count;
            }
            if (x < side - 1) {
                total += field[static_cast<size_t>(by * side + (x + 1))];
                ++count;
            }
            out[static_cast<size_t>(y * side + x)] = total / static_cast<double>(count);
        }
    }
    return out;
}

std::vector<double> grid_axis(int side, bool horizontal) {
    std::vector<double> out(static_cast<size_t>(side * side));
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) out[static_cast<size_t>(y * side + x)] = horizontal ? x : y;
    }
    return out;
}

Num eval_node(const Node& node, EvalCtx& ctx);

std::vector<double> sum_sites(const Node& expr, EvalCtx& ctx) {
    size_t n = static_cast<size_t>(ctx.side * ctx.side);
    size_t sites = ctx.px == nullptr ? 0 : ctx.px->size();
    std::vector<double> acc(n, 0.0);
    std::vector<double> gx = grid_axis(ctx.side, true);
    std::vector<double> gy = grid_axis(ctx.side, false);
    for (size_t s = 0; s < sites; ++s) {
        if (ctx.active != nullptr && (*ctx.active)[s] <= 0.0) continue;
        std::vector<double> dx(n), dy(n), dist2(n);
        for (size_t i = 0; i < n; ++i) {
            dx[i] = gx[i] - (*ctx.px)[s];
            dy[i] = gy[i] - (*ctx.py)[s];
            dist2[i] = dx[i] * dx[i] + dy[i] * dy[i];
        }
        double w = ctx.weights != nullptr ? (*ctx.weights)[s] : 1.0;
        double ph = ctx.phases != nullptr ? (*ctx.phases)[s] : 0.0;
        double act = ctx.active != nullptr ? (*ctx.active)[s] : 1.0;
        std::unordered_map<std::string, Num> site_vars{
            {"dx", Num::of_array(std::move(dx))},
            {"dy", Num::of_array(std::move(dy))},
            {"dist2", Num::of_array(std::move(dist2))},
            {"weight", Num::of(w)},
            {"phase", Num::of(ph)},
            {"active", Num::of(act)},
        };
        EvalCtx site = ctx;
        site.site_vars = &site_vars;
        Num val = eval_node(expr, site);
        for (size_t i = 0; i < n; ++i) acc[i] += at(val, i);
    }
    return acc;
}

Num escape_iter(Num cr, Num ci, int max_iter, const Num* z0r, const Num* z0i) {
    size_t n = 1;
    if (cr.array) n = cr.data.size();
    if (ci.array) n = std::max(n, ci.data.size());
    if (z0r != nullptr && z0r->array) n = std::max(n, z0r->data.size());
    if (z0i != nullptr && z0i->array) n = std::max(n, z0i->data.size());
    if (n == 1 && !cr.array && z0r == nullptr) {
        // A scalar mandelbrot point is still a length-1 field.
    }
    std::vector<double> zr(n, 0.0), zi(n, 0.0), cre(n), cim(n);
    for (size_t i = 0; i < n; ++i) {
        cre[i] = at(cr, cr.array ? i : 0);
        cim[i] = at(ci, ci.array ? i : 0);
        if (z0r != nullptr) zr[i] = at(*z0r, z0r->array ? i : 0);
        if (z0i != nullptr) zi[i] = at(*z0i, z0i->array ? i : 0);
    }
    std::vector<double> esc(n, 0.0);
    std::vector<char> mask(n, 1);
    for (int iter = 0; iter < max_iter; ++iter) {
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            if (mask[i] == 0) continue;
            any = true;
            double nr = zr[i] * zr[i] - zi[i] * zi[i] + cre[i];
            double ni = 2.0 * zr[i] * zi[i] + cim[i];
            zr[i] = nr;
            zi[i] = ni;
            double mag2 = nr * nr + ni * ni;
            if (mag2 > 4.0 || !std::isfinite(mag2)) {
                esc[i] = iter + 1;
                mask[i] = 0;
            }
        }
        if (!any) break;
    }
    for (size_t i = 0; i < n; ++i)
        if (mask[i] != 0) esc[i] = max_iter;
    return Num::of_array(std::move(esc));
}

Num burning_ship(Num cr, Num ci, int max_iter) {
    size_t n = cr.array ? cr.data.size() : (ci.array ? ci.data.size() : 1);
    std::vector<double> zr(n, 0.0), zi(n, 0.0), esc(n, 0.0);
    std::vector<char> mask(n, 1);
    for (int iter = 0; iter < max_iter; ++iter) {
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            if (mask[i] == 0) continue;
            any = true;
            double ax = std::fabs(zr[i]);
            double ay = std::fabs(zi[i]);
            double nr = ax * ax - ay * ay + at(cr, cr.array ? i : 0);
            double ni = 2.0 * ax * ay + at(ci, ci.array ? i : 0);
            zr[i] = nr;
            zi[i] = ni;
            double mag2 = nr * nr + ni * ni;
            if (mag2 > 4.0 || !std::isfinite(mag2)) {
                esc[i] = iter + 1;
                mask[i] = 0;
            }
        }
        if (!any) break;
    }
    for (size_t i = 0; i < n; ++i)
        if (mask[i] != 0) esc[i] = max_iter;
    return Num::of_array(std::move(esc));
}

Num escape_perturb(double ar, double ai, const Num& dr, const Num& di, int max_iter) {
    size_t n = dr.array ? dr.data.size() : di.data.size();
    std::vector<double> dzr(n, 0.0), dzi(n, 0.0), esc(n, 0.0), dre(n), dim(n);
    for (size_t i = 0; i < n; ++i) {
        dre[i] = at(dr, i);
        dim[i] = at(di, i);
    }
    std::vector<char> mask(n, 1);
    double zr = 0, zi = 0;
    bool ref_ok = true;
    for (int iter = 0; iter < max_iter && ref_ok; ++iter) {
        double zr2 = zr * zr - zi * zi + ar;
        double zi2 = 2.0 * zr * zi + ai;
        double zmag2 = zr2 * zr2 + zi2 * zi2;
        if (!std::isfinite(zmag2) || zmag2 > 4.0 || std::fabs(zr2) > 1e6 || std::fabs(zi2) > 1e6 ||
            std::fabs(zr2) > 4.0 || std::fabs(zi2) > 4.0) {
            ref_ok = false;
            break;
        }
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            double drr = mask[i] != 0 ? dzr[i] : 0.0;
            double dii = mask[i] != 0 ? dzi[i] : 0.0;
            double t1r = 2.0 * (zr * drr - zi * dii);
            double t1i = 2.0 * (zr * dii + zi * drr);
            double t2r = drr * drr - dii * dii;
            double t2i = 2.0 * drr * dii;
            double nr = t1r + t2r + dre[i];
            double ni = t1i + t2i + dim[i];
            if (mask[i] != 0) {
                any = true;
                double rr = zr2 + nr;
                double ii = zi2 + ni;
                double mag2 = rr * rr + ii * ii;
                if (mag2 > 4.0 || !std::isfinite(mag2)) {
                    esc[i] = iter + 1;
                    mask[i] = 0;
                    dzr[i] = 0;
                    dzi[i] = 0;
                } else {
                    dzr[i] = nr;
                    dzi[i] = ni;
                }
            }
        }
        zr = zr2;
        zi = zi2;
        if (!any) break;
    }
    bool leftover = false;
    for (char m : mask)
        if (m != 0) leftover = true;
    if (leftover) {
        std::vector<double> cre(n), cim(n);
        for (size_t i = 0; i < n; ++i) {
            cre[i] = ar + dre[i];
            cim[i] = ai + dim[i];
        }
        Num full = escape_iter(Num::of_array(std::move(cre)), Num::of_array(std::move(cim)), max_iter, nullptr, nullptr);
        for (size_t i = 0; i < n; ++i)
            if (mask[i] != 0) esc[i] = full.data[i];
    }
    return Num::of_array(std::move(esc));
}

Num call_func(const Node& node, EvalCtx& ctx) {
    const std::string& name = node.text;
    if (ctx.in_field && name == "sum_sites" && node.kids.size() == 1) return Num::of_array(sum_sites(node.kids[0], ctx));
    if (!ctx.in_field && name == "lap" && node.kids.size() == 1 && node.kids[0].kind == Node::Ident) {
        const auto* f = find_vec(ctx.fields, node.kids[0].text);
        if (f == nullptr) throw PixilError(ErrorKind::Name, "unknown field " + node.kids[0].text);
        return Num::of_array(lap4(*f, ctx.side, ctx.boundary));
    }
    if (!ctx.in_field && name == "at" && node.kids.size() == 1 && node.kids[0].kind == Node::Ident) {
        const auto* f = find_vec(ctx.fields, node.kids[0].text);
        if (f == nullptr) throw PixilError(ErrorKind::Name, "unknown field " + node.kids[0].text);
        return Num::of_array(*f);
    }
    if (!ctx.in_field && (name == "neighbors" || name == "neighbors4") && node.kids.size() == 1 &&
        node.kids[0].kind == Node::Ident) {
        const auto* f = find_vec(ctx.fields, node.kids[0].text);
        if (f == nullptr) throw PixilError(ErrorKind::Name, "unknown field " + node.kids[0].text);
        return Num::of_array(neighbor_count(*f, ctx.side, ctx.boundary, name == "neighbors4"));
    }
    if (!ctx.in_field && name == "below_avg" && node.kids.size() == 1 && node.kids[0].kind == Node::Ident) {
        const auto* f = find_vec(ctx.fields, node.kids[0].text);
        if (f == nullptr) throw PixilError(ErrorKind::Name, "unknown field " + node.kids[0].text);
        return Num::of_array(below_avg(*f, ctx.side));
    }
    if (!ctx.in_field && name == "random_field" && node.kids.size() == 2) {
        double lo = eval_node(node.kids[0], ctx).scalar;
        double hi = eval_node(node.kids[1], ctx).scalar;
        std::vector<double> out(static_cast<size_t>(ctx.side * ctx.side));
        for (double& v : out) v = g_numpy.uniform(lo, hi);
        return Num::of_array(std::move(out));
    }
    std::vector<Num> args;
    args.reserve(node.kids.size());
    for (const auto& kid : node.kids) args.push_back(eval_node(kid, ctx));
    auto unary = [&](auto op) {
        if (args.size() != 1) throw PixilError(ErrorKind::Value, name + " needs 1 argument");
        if (!args[0].array) return Num::of(op(args[0].scalar));
        std::vector<double> out(args[0].data.size());
        for (size_t i = 0; i < out.size(); ++i) out[i] = op(args[0].data[i]);
        return Num::of_array(std::move(out));
    };
    if (name == "clamp" && args.size() == 3) {
        double lo = args[1].scalar;
        double hi = args[2].scalar;
        if (!args[0].array) return Num::of(std::clamp(args[0].scalar, lo, hi));
        std::vector<double> clipped(args[0].data.size());
        for (size_t i = 0; i < clipped.size(); ++i) clipped[i] = std::clamp(args[0].data[i], lo, hi);
        return Num::of_array(std::move(clipped));
    }
    if (name == "min" && args.size() == 2) return apply2(args[0], args[1], [](double a, double b) { return std::min(a, b); });
    if (name == "max" && args.size() == 2) return apply2(args[0], args[1], [](double a, double b) { return std::max(a, b); });
    if (name == "abs" && args.size() == 1) return unary([](double v) { return std::fabs(v); });
    if (name == "floor" && args.size() == 1) return unary([](double v) { return std::floor(v); });
    if (name == "sin" && args.size() == 1) return unary([](double v) { return std::sin(v); });
    if (name == "cos" && args.size() == 1) return unary([](double v) { return std::cos(v); });
    if (name == "sqrt" && args.size() == 1)
        return unary([](double v) { return std::sqrt(std::max(v, 0.0)); });
    if (name == "atan2" && args.size() == 2)
        return apply2(args[0], args[1], [](double y, double x) { return std::atan2(y, x); });
    if (name == "pow" && args.size() == 2) return apply2(args[0], args[1], [](double a, double b) { return np_power(a, b); });
    if (name == "escape_iter" && (args.size() == 3 || args.size() == 5)) {
        int iters = truncated(args[2].scalar);
        if (args.size() == 3) return escape_iter(args[0], args[1], iters, nullptr, nullptr);
        return escape_iter(args[0], args[1], iters, &args[3], &args[4]);
    }
    if (name == "burning_ship_iter" && args.size() == 3) return burning_ship(args[0], args[1], truncated(args[2].scalar));
    if (name == "escape_zoom" && args.size() == 5) {
        double ar = args[0].scalar, ai = args[1].scalar;
        const Num& dr = args[2];
        const Num& di = args[3];
        int iters = truncated(args[4].scalar);
        double peak = 0;
        size_t n = dr.array ? dr.data.size() : 1;
        for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(at(dr, i)));
        n = di.array ? di.data.size() : 1;
        for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(at(di, i)));
        if (peak > 0.002) {
            size_t len = dr.array ? dr.data.size() : di.data.size();
            std::vector<double> cre(len), cim(len);
            for (size_t i = 0; i < len; ++i) {
                cre[i] = ar + at(dr, i);
                cim[i] = ai + at(di, i);
            }
            return escape_iter(Num::of_array(std::move(cre)), Num::of_array(std::move(cim)), iters, nullptr, nullptr);
        }
        return escape_perturb(ar, ai, dr, di, iters);
    }
    throw PixilError(ErrorKind::Value, "unknown grid call " + name);
}

Num eval_node(const Node& node, EvalCtx& ctx) {
    switch (node.kind) {
        case Node::Num: return Num::of(node.number);
        case Node::Ident: return lookup(node.text, ctx);
        case Node::Neg: {
            Num v = eval_node(node.kids[0], ctx);
            if (!v.array) return Num::of(-v.scalar);
            for (double& x : v.data) x = -x;
            return v;
        }
        case Node::Not: {
            Num v = eval_node(node.kids[0], ctx);
            if (!v.array) return Num::of(v.scalar == 0.0 ? 1.0 : 0.0);
            for (double& x : v.data) x = x == 0.0 ? 1.0 : 0.0;
            return v;
        }
        case Node::And:
        case Node::Or: {
            Num a = eval_node(node.kids[0], ctx);
            Num b = eval_node(node.kids[1], ctx);
            bool is_or = node.kind == Node::Or;
            return apply2(a, b, [is_or](double x, double y) {
                bool r = is_or ? (x != 0.0 || y != 0.0) : (x != 0.0 && y != 0.0);
                return r ? 1.0 : 0.0;
            });
        }
        case Node::Cmp: {
            Num a = eval_node(node.kids[0], ctx);
            Num b = eval_node(node.kids[1], ctx);
            const std::string& op = node.text;
            return apply2(a, b, [&](double x, double y) {
                bool r = false;
                if (op == "==") r = x == y;
                else if (op == "!=") r = x != y;
                else if (op == "<") r = x < y;
                else if (op == ">") r = x > y;
                else if (op == "<=") r = x <= y;
                else r = x >= y;
                return r ? 1.0 : 0.0;
            });
        }
        case Node::Bin: {
            Num a = eval_node(node.kids[0], ctx);
            Num b = eval_node(node.kids[1], ctx);
            if (node.text == "/") {
                return apply2(a, b, [](double x, double y) { return y == 0.0 ? 0.0 : x / y; });
            }
            if (node.text == "+") return apply2(a, b, [](double x, double y) { return x + y; });
            if (node.text == "-") return apply2(a, b, [](double x, double y) { return x - y; });
            return apply2(a, b, [](double x, double y) { return x * y; });
        }
        case Node::Call:
            if (node.text == "where" && node.kids.size() == 3) {
                Num cond = eval_node(node.kids[0], ctx);
                Num a = eval_node(node.kids[1], ctx);
                Num b = eval_node(node.kids[2], ctx);
                size_t n = 0;
                if (cond.array) n = cond.data.size();
                if (a.array) n = std::max(n, a.data.size());
                if (b.array) n = std::max(n, b.data.size());
                if (n == 0) return Num::of(cond.scalar != 0.0 ? a.scalar : b.scalar);
                std::vector<double> out(n);
                for (size_t i = 0; i < n; ++i) out[i] = at(cond, i) != 0.0 ? at(a, i) : at(b, i);
                return Num::of_array(std::move(out));
            }
            return call_func(node, ctx);
    }
    throw PixilError(ErrorKind::Value, "bad grid node");
}

std::vector<double> must_array(const Num& n, int side) {
    size_t len = static_cast<size_t>(side * side);
    if (!n.array) return std::vector<double>(len, n.scalar);
    if (n.data.size() != len) throw PixilError(ErrorKind::Value, "step result has the wrong length");
    return n.data;
}

void plot_cell(DrawBatch& batch, int x, int y, int cell, const Value& color, int intensity) {
    if (cell == 1) {
        batch.append("mplot", {Value::integer(x), Value::integer(y), color, Value::integer(intensity)});
    } else {
        batch.append("draw_rectangle", {Value::integer(x), Value::integer(y), Value::integer(cell), Value::integer(cell),
                                        color, Value::integer(intensity), Value::boolean(true)});
    }
}

void render_grid(const GridProgram& program, Runtime& rt, DrawBatch& batch,
                  const std::unordered_map<std::string, double>& scalars) {
    int cell = std::max(1, truncated(resolve_scalar(program.cell, scalars)));
    const DrawSpec& draw = program.draw;
    auto field = [&](const std::string& name) -> const std::vector<double>& {
        return rt.fields.at(name).half[rt.fields.at(name).active];
    };
    auto paint = [&](int y, int x, const Value& color, int intensity) {
        plot_cell(batch, x * cell, y * cell, cell, color, intensity);
    };
    int side = rt.side;
    if (draw.mode == "palette") {
        std::string fname = draw.palette_field.empty() ? program.fields.back() : draw.palette_field;
        const auto& vals = field(fname);
        double base = resolve_scalar(draw.color_base, scalars);
        double scale = resolve_scalar(draw.color_scale, scalars);
        double ib = resolve_scalar(draw.intensity_base, scalars);
        double is = resolve_scalar(draw.intensity_scale, scalars);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                double v = vals[static_cast<size_t>(y * side + x)];
                double intensity = std::clamp(ib + std::floor(v * is), 0.0, 100.0);
                if (truncated(intensity) < 1) continue;
                double color = base + std::floor(v * scale);
                if (color > 99) color -= 98;
                if (color < 1) color = 1;
                paint(y, x, Value::integer(truncated(color)), truncated(intensity));
            }
        }
        return;
    }
    if (draw.mode == "fill") {
        std::string fname = draw.fill_field.empty() ? program.fields.front() : draw.fill_field;
        const auto& vals = field(fname);
        Value color = Value::str(draw.fill_color);
        int intensity = truncated(resolve_scalar(draw.fill_intensity, scalars));
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                if (vals[static_cast<size_t>(y * side + x)] <= 0.5) continue;
                paint(y, x, color, intensity);
            }
        }
        return;
    }
    if (draw.mode == "fire") {
        std::string fname = draw.fire_field.empty() ? program.fields.front() : draw.fire_field;
        const auto& vals = field(fname);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                double h = vals[static_cast<size_t>(y * side + x)];
                if (h <= 0) continue;
                Value color;
                int intensity;
                if (h < 20) {
                    color = Value::integer(0);
                    intensity = truncated(h * 3);
                } else if (h < 40) {
                    color = Value::str("red");
                    intensity = truncated((h - 20) * 3 + 20);
                } else if (h < 70) {
                    color = Value::str("orange");
                    intensity = truncated((h - 40) * 2 + 40);
                } else {
                    color = Value::str("yellow");
                    intensity = truncated(std::min(h, 100.0));
                }
                paint(y, x, color, intensity);
            }
        }
        return;
    }
    std::unordered_map<std::string, std::vector<double>> read;
    for (const auto& name : program.fields) read.emplace(name, field(name));
    std::unordered_map<std::string, std::vector<double>> temps;
    std::unordered_map<std::string, std::vector<double>> grid{{"grid_x", grid_axis(side, true)},
                                                              {"grid_y", grid_axis(side, false)}};
    EvalCtx ctx;
    ctx.scalars = &scalars;
    ctx.fields = &read;
    ctx.temps = &temps;
    ctx.grid_vars = &grid;
    ctx.side = side;
    ctx.boundary = rt.boundary;
    Num mask = draw.has_plot ? eval_node(draw.plot_if, ctx) : Num::of(1);
    Num colors = draw.has_color ? eval_node(draw.color, ctx) : Num::of(1);
    Num opacity = draw.has_opacity ? eval_node(draw.opacity, ctx) : Num::of(100);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            size_t i = static_cast<size_t>(y * side + x);
            if (at(mask, mask.array ? i : 0) <= 0) continue;
            int color = whole(at(colors, colors.array ? i : 0));
            int bright = truncated(std::clamp(at(opacity, opacity.array ? i : 0), 0.0, 100.0));
            paint(y, x, Value::integer(color), bright);
        }
    }
}

std::vector<double> eval_formula(const FieldProgram& program, Env& env, int side,
                                 const std::unordered_map<std::string, double>& scalars) {
    size_t n = static_cast<size_t>(side * side);
    auto xs = read_array(need_array(env, program.sites_x));
    auto ys = read_array(need_array(env, program.sites_y));
    std::vector<double> weights, phases, active;
    if (!program.weights.empty()) weights = read_array(need_array(env, program.weights));
    if (!program.phases.empty()) phases = read_array(need_array(env, program.phases));
    if (!program.active.empty()) active = read_array(need_array(env, program.active));
    if (program.mode == "metaballs") {
        std::vector<double> result(n, 0.0);
        std::vector<double> gx = grid_axis(side, true);
        std::vector<double> gy = grid_axis(side, false);
        for (size_t s = 0; s < xs.size(); ++s) {
            if (!active.empty() && active[s] <= 0) continue;
            double w = weights.empty() ? 1.0 : weights[s];
            for (size_t i = 0; i < n; ++i) {
                double dx = gx[i] - xs[s];
                double dy = gy[i] - ys[s];
                double dist2 = std::max(dx * dx + dy * dy, 1.0);
                result[i] += (w * w) / dist2;
            }
        }
        return result;
    }
    std::unordered_map<std::string, std::vector<double>> temps;
    std::unordered_map<std::string, std::vector<double>> grid{{"grid_x", grid_axis(side, true)},
                                                              {"grid_y", grid_axis(side, false)}};
    EvalCtx ctx;
    ctx.scalars = &scalars;
    ctx.temps = &temps;
    ctx.grid_vars = &grid;
    ctx.side = side;
    ctx.in_field = true;
    ctx.px = &xs;
    ctx.py = &ys;
    ctx.weights = weights.empty() ? nullptr : &weights;
    ctx.phases = phases.empty() ? nullptr : &phases;
    ctx.active = active.empty() ? nullptr : &active;
    for (const auto& [target, expr] : program.value) {
        Num value = eval_node(expr, ctx);
        temps[target] = must_array(value, side);
    }
    if (!program.result.empty() && temps.count(program.result) != 0) return temps[program.result];
    throw PixilError(ErrorKind::Value, "field_program produced no value");
}

void render_voronoi(const FieldProgram& program, Env& env, int side, DrawBatch& batch,
                     const std::unordered_map<std::string, double>& scalars) {
    auto px = read_array(need_array(env, program.sites_x));
    auto py = read_array(need_array(env, program.sites_y));
    if (px.size() < 2) throw PixilError(ErrorKind::Value, "voronoi needs at least two sites");
    std::vector<double> colors;
    if (!program.site_colors.empty()) colors = read_array(need_array(env, program.site_colors));
    double ratio = resolve_scalar(program.edge_ratio, scalars);
    int fill_op = truncated(resolve_scalar(program.fill_opacity, scalars));
    int edge_op = truncated(resolve_scalar(program.edge_opacity, scalars));
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            size_t best = 0;
            double best_d = 1e300;
            double second = 1e300;
            for (size_t s = 0; s < px.size(); ++s) {
                double dx = x - px[s];
                double dy = y - py[s];
                double d = dx * dx + dy * dy;
                if (d < best_d) {
                    second = best_d;
                    best_d = d;
                    best = s;
                } else if (d < second) second = d;
            }
            bool edge = program.edges && second < best_d * ratio;
            if (edge) {
                batch.append("mplot", {Value::integer(x), Value::integer(y), Value::str(program.edge_color),
                                       Value::integer(edge_op)});
            } else {
                int color = colors.empty() ? static_cast<int>((best * 17) % 99 + 1) : truncated(colors[best]);
                batch.append("mplot", {Value::integer(x), Value::integer(y), Value::integer(color), Value::integer(fill_op)});
            }
        }
    }
}

void render_field(const FieldProgram& program, Env& env, DrawBatch& batch) {
    auto scalars = scalars_of(env);
    int side = std::max(1, truncated(resolve_scalar(program.size, scalars)));
    if (program.mode == "voronoi") {
        render_voronoi(program, env, side, batch, scalars);
        return;
    }
    std::vector<double> value = eval_formula(program, env, side, scalars);
    if (program.signed_field) {
        std::string peak = resolve_color(program.peak_color, env);
        std::string trough = resolve_color(program.trough_color, env);
        double scale = resolve_scalar(program.intensity_scale, scalars);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                double h = value[static_cast<size_t>(y * side + x)];
                int intensity = truncated(std::clamp(std::fabs(h) * scale, 0.0, 99.0));
                if (intensity <= 0) continue;
                batch.append("mplot", {Value::integer(x), Value::integer(y), Value::str(h > 0 ? peak : trough),
                                       Value::integer(intensity)});
            }
        }
        return;
    }
    std::unordered_map<std::string, std::vector<double>> temps;
    temps[program.result.empty() ? "v_sum" : program.result] = value;
    temps["v_sum"] = value;
    if (program.result != "v_iter") temps["v_iter"] = value;
    std::unordered_map<std::string, std::vector<double>> grid{{"grid_x", grid_axis(side, true)},
                                                              {"grid_y", grid_axis(side, false)}};
    EvalCtx ctx;
    ctx.scalars = &scalars;
    ctx.temps = &temps;
    ctx.grid_vars = &grid;
    ctx.side = side;
    ctx.in_field = true;
    Num mask = program.has_plot ? eval_node(program.plot_if, ctx) : Num::of(1);
    Num colors = program.has_color ? eval_node(program.color, ctx) : Num::of(50);
    Num opacity = program.has_opacity ? eval_node(program.opacity, ctx) : Num::of(75);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            size_t i = static_cast<size_t>(y * side + x);
            if (at(mask, mask.array ? i : 0) <= 0) continue;
            int color = whole(at(colors, colors.array ? i : 0));
            int bright = truncated(std::clamp(at(opacity, opacity.array ? i : 0), 0.0, 100.0));
            batch.append("mplot", {Value::integer(x), Value::integer(y), Value::integer(color), Value::integer(bright)});
        }
    }
}

void run_step(const GridProgram& program, Runtime& rt, Env& env, DrawBatch& batch) {
    auto scalars = scalars_of(env);
    int side = std::max(1, truncated(resolve_scalar(program.size, scalars)));
    if (rt.fields.empty()) {
        rt.side = side;
        rt.boundary = program.boundary;
        for (const auto& fname : program.fields) {
            auto src = read_array(need_array(env, fname));
            if (src.size() != static_cast<size_t>(side * side)) {
                throw PixilError(ErrorKind::Value, "field " + fname + " has the wrong length");
            }
            Buf buf;
            buf.half[0] = src;
            buf.half[1] = std::move(src);
            buf.half[1] = buf.half[0];
            rt.fields.emplace(fname, std::move(buf));
        }
    }
    rt.side = side;
    rt.boundary = program.boundary;
    for (const auto& fname : program.fields) {
        auto src = read_array(need_array(env, fname));
        Buf& buf = rt.fields.at(fname);
        if (src.size() != buf.half[buf.active].size()) {
            throw PixilError(ErrorKind::Value, "field " + fname + " has the wrong length");
        }
        buf.half[buf.active] = std::move(src);
    }
    int steps = std::max(1, truncated(resolve_scalar(program.steps, scalars)));
    for (int step = 0; step < steps; ++step) {
        for (const auto& block : program.blocks) {
            std::unordered_map<std::string, std::vector<double>> read;
            for (const auto& fname : program.fields) {
                Buf& buf = rt.fields.at(fname);
                read.emplace(fname, buf.half[buf.active]);
            }
            std::unordered_map<std::string, std::vector<double>> temps;
            std::unordered_map<std::string, std::vector<double>> grid{{"grid_x", grid_axis(side, true)},
                                                                      {"grid_y", grid_axis(side, false)}};
            EvalCtx ctx;
            ctx.scalars = &scalars;
            ctx.fields = &read;
            ctx.temps = &temps;
            ctx.grid_vars = &grid;
            ctx.side = side;
            ctx.boundary = rt.boundary;
            const std::vector<double>* result = nullptr;
            std::string out_name = block.field + "_next";
            for (const auto& [target, expr] : block.assigns) {
                Num value = eval_node(expr, ctx);
                temps[target] = must_array(value, side);
                if (target == out_name) result = &temps[target];
            }
            if (result == nullptr) throw PixilError(ErrorKind::Value, "step " + block.field + " did not assign " + out_name);
            Buf& dest = rt.fields.at(block.field);
            dest.half[1 - dest.active] = *result;
        }
        for (const auto& fname : program.fields) {
            Buf& buf = rt.fields.at(fname);
            buf.active = 1 - buf.active;
        }
    }
    for (const auto& fname : program.fields) {
        Buf& buf = rt.fields.at(fname);
        write_array(need_array(env, fname), buf.half[buf.active]);
    }
    render_grid(program, rt, batch, scalars);
}

}  // namespace

struct GridState {
    std::unordered_map<std::string, GridProgram> grids;
    std::unordered_map<std::string, FieldProgram> fields;
    std::unordered_map<std::string, Runtime> rts;
};

std::unique_ptr<GridState> make_grid_state() { return std::make_unique<GridState>(); }

Machine::Machine(SymbolTable& symbols) : env(symbols), grids_(make_grid_state()) {}

Machine::~Machine() = default;

void note_numpy_script_seed(uint32_t seed);

void seed_numpy_random(int64_t seed) {
    if (seed < 0 || seed > 0xffffffffLL) throw PixilError(ErrorKind::Value, "numpy seed is out of range");
    g_numpy.seed(static_cast<uint32_t>(seed));
    note_numpy_script_seed(static_cast<uint32_t>(seed));
}

void define_grid_program(GridState& state, const std::string& name, const std::vector<std::string>& lines) {
    state.grids[name] = compile_grid(name, lines);
}

void define_field_program(GridState& state, const std::string& name, const std::vector<std::string>& lines) {
    state.fields[name] = compile_field(name, lines);
}

void grid_fill_array(PixArray& array, double value) {
    for (auto& cell : array.data) cell = Value::real(value);
}

void grid_reset(GridState& state, const std::string& name) { state.rts.erase(name); }

void grid_step(GridState& state, Env& env, const std::string& name, DrawBatch& batch) {
    auto found = state.grids.find(name);
    if (found == state.grids.end()) throw PixilError(ErrorKind::Name, "unknown grid_program " + name);
    run_step(found->second, state.rts[name], env, batch);
}

void field_render(GridState& state, Env& env, const std::string& name, DrawBatch& batch) {
    auto found = state.fields.find(name);
    if (found == state.fields.end()) throw PixilError(ErrorKind::Name, "unknown field_program " + name);
    render_field(found->second, env, batch);
}

}  // namespace pixil
