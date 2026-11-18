#include "consumer/parse.h"

#include <cmath>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "consumer/batch_decode.h"
#include "consumer/color_rgb.h"

namespace pixil::consumer {
namespace {

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool parse_bool(std::string_view s) {
    std::string t = trim(s);
    for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t == "true" || t == "1" || t == "yes";
}

double parse_number(std::string_view s) {
    std::string t = trim(s);
    size_t idx = 0;
    double v = std::stod(t, &idx);
    if (idx != t.size()) throw std::runtime_error("bad number: " + t);
    return v;
}

int parse_int(std::string_view s) { return static_cast<int>(std::trunc(parse_number(s))); }

std::optional<int> parse_optional_int(const std::vector<std::string>& args, size_t i) {
    if (i >= args.size() || args[i].empty()) return std::nullopt;
    return parse_int(args[i]);
}

BurnoutMode parse_burnout_mode(const std::vector<std::string>& args, size_t i) {
    if (i >= args.size() || args[i].empty()) return BurnoutMode::Instant;
    std::string t = args[i];
    for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t == "fade" ? BurnoutMode::Fade : BurnoutMode::Instant;
}

std::vector<std::string> split_args(std::string_view inside) {
    std::vector<std::string> out;
    std::string cur;
    bool in_str = false;
    for (size_t i = 0; i < inside.size(); ++i) {
        char c = inside[i];
        if (in_str) {
            if (c == '\\' && i + 1 < inside.size()) {
                cur.push_back(inside[++i]);
                continue;
            }
            if (c == '"') {
                in_str = false;
                continue;
            }
            cur.push_back(c);
            continue;
        }
        if (c == '"') {
            in_str = true;
            continue;
        }
        if (c == ',') {
            out.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (in_str) throw std::runtime_error("unterminated string in command");
    if (!trim(cur).empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}

ColorArg require_color(std::string_view tok) {
    if (auto c = parse_color_token(tok)) return *c;
    throw std::runtime_error("bad color: " + std::string(tok));
}

}  // namespace

void parse_and_execute(MatrixApi& api, const std::string& command) {
    std::string cmd = trim(command);
    if (cmd.empty()) return;

    auto strip_batch_payload = [](std::string_view text, size_t prefix_len) {
        std::string inside = trim(text.substr(prefix_len, text.size() - prefix_len - 1));
        if (inside.size() >= 2 && inside.front() == '"' && inside.back() == '"') {
            inside = inside.substr(1, inside.size() - 2);
        }
        return inside;
    };

    if (cmd.rfind("draw_batch(", 0) == 0 && cmd.back() == ')') {
        api.draw_batch_bytes(base64_decode(strip_batch_payload(cmd, 11)));
        return;
    }
    if (cmd.rfind("plot_batch(", 0) == 0 && cmd.back() == ')') {
        api.plot_batch_bytes(base64_decode(strip_batch_payload(cmd, 11)));
        return;
    }
    if (cmd.rfind("sprite_batch(", 0) == 0 && cmd.back() == ')') {
        api.sprite_batch_bytes(base64_decode(strip_batch_payload(cmd, 13)));
        return;
    }

    if (cmd == "clear") {
        api.clear();
        return;
    }
    if (cmd == "end_frame") {
        api.end_frame();
        return;
    }
    if (cmd == "sync_queue") return;
    if (cmd == "dispose_all_sprites") {
        api.dispose_all_sprites();
        return;
    }
    if (cmd == "endsprite") {
        api.endsprite();
        return;
    }
    if (cmd == "hide_background") {
        api.hide_background(std::nullopt);
        return;
    }

    auto lparen = cmd.find('(');
    if (lparen == std::string::npos || cmd.back() != ')') {
        throw std::runtime_error("unrecognized command: " + cmd);
    }
    std::string name = trim(cmd.substr(0, lparen));
    auto args = split_args(std::string_view(cmd).substr(lparen + 1, cmd.size() - lparen - 2));

    if (name == "fps") {
        api.set_fps(args.empty() ? 0 : parse_number(args[0]));
        return;
    }
    if (name == "rest") {
        api.rest(args.empty() ? 0 : parse_number(args[0]));
        return;
    }
    if (name == "begin_frame") {
        api.begin_frame(!args.empty() && parse_bool(args[0]));
        return;
    }
    if (name == "plot") {
        if (args.size() < 3) throw std::runtime_error("plot needs x,y,color");
        int inten = args.size() > 3 ? parse_int(args[3]) : 100;
        api.plot(parse_int(args[0]), parse_int(args[1]), require_color(args[2]), inten,
                 parse_optional_int(args, 4), parse_burnout_mode(args, 5));
        return;
    }
    if (name == "draw_line") {
        if (args.size() < 5) throw std::runtime_error("draw_line needs 5+ args");
        int inten = args.size() > 5 ? parse_int(args[5]) : 100;
        api.draw_line(parse_int(args[0]), parse_int(args[1]), parse_int(args[2]), parse_int(args[3]),
                      require_color(args[4]), inten, parse_optional_int(args, 6), parse_burnout_mode(args, 7));
        return;
    }
    if (name == "draw_rectangle") {
        if (args.size() < 5) throw std::runtime_error("draw_rectangle needs 5+ args");
        int inten = args.size() > 5 ? parse_int(args[5]) : 100;
        bool fill = args.size() > 6 && parse_bool(args[6]);
        api.draw_rectangle(parse_int(args[0]), parse_int(args[1]), parse_int(args[2]), parse_int(args[3]),
                           require_color(args[4]), inten, fill, parse_optional_int(args, 7),
                           parse_burnout_mode(args, 8));
        return;
    }
    if (name == "draw_circle") {
        if (args.size() < 4) throw std::runtime_error("draw_circle needs 4+ args");
        int inten = args.size() > 4 ? parse_int(args[4]) : 100;
        bool fill = args.size() > 5 && parse_bool(args[5]);
        api.draw_circle(parse_int(args[0]), parse_int(args[1]), parse_int(args[2]), require_color(args[3]), inten,
                        fill, parse_optional_int(args, 6), parse_burnout_mode(args, 7));
        return;
    }
    if (name == "draw_polygon") {
        if (args.size() < 5) throw std::runtime_error("draw_polygon needs 5+ args");
        int inten = args.size() > 5 ? parse_int(args[5]) : 100;
        float rot = args.size() > 6 ? float(parse_number(args[6])) : 0.f;
        bool fill = args.size() > 7 && parse_bool(args[7]);
        api.draw_polygon(parse_int(args[0]), parse_int(args[1]), parse_int(args[2]), parse_int(args[3]),
                         require_color(args[4]), inten, rot, fill, parse_optional_int(args, 8),
                         parse_burnout_mode(args, 9));
        return;
    }
    if (name == "draw_ellipse") {
        if (args.size() < 5) throw std::runtime_error("draw_ellipse needs 5+ args");
        int inten = args.size() > 5 ? parse_int(args[5]) : 100;
        bool fill = args.size() > 6 && parse_bool(args[6]);
        float rot = args.size() > 7 ? float(parse_number(args[7])) : 0.f;
        api.draw_ellipse(parse_int(args[0]), parse_int(args[1]), parse_int(args[2]), parse_int(args[3]),
                         require_color(args[4]), inten, fill, rot, parse_optional_int(args, 8),
                         parse_burnout_mode(args, 9));
        return;
    }
    if (name == "draw_arc") {
        if (args.size() < 6) throw std::runtime_error("draw_arc needs 6+ args");
        int inten = args.size() > 6 ? parse_int(args[6]) : 100;
        bool fill = args.size() > 7 && parse_bool(args[7]);
        api.draw_arc(parse_int(args[0]), parse_int(args[1]), parse_int(args[2]), parse_int(args[3]),
                     float(parse_number(args[4])), require_color(args[5]), inten, fill,
                     parse_optional_int(args, 8), parse_burnout_mode(args, 9));
        return;
    }
    if (name == "throttle") return;

    if (name == "define_sprite") {
        if (args.size() < 3) throw std::runtime_error("define_sprite needs name,width,height");
        api.define_sprite(args[0], parse_int(args[1]), parse_int(args[2]));
        return;
    }
    if (name == "sprite_cel") {
        api.sprite_cel(parse_optional_int(args, 0));
        return;
    }
    if (name == "sprite_draw") {
        if (args.size() < 2) throw std::runtime_error("sprite_draw needs name,cmd");
        std::vector<std::string> rest(args.begin() + 2, args.end());
        api.sprite_draw(args[0], args[1], rest);
        return;
    }
    if (name == "show_sprite") {
        if (args.size() < 3) throw std::runtime_error("show_sprite needs name,x,y");
        int instance = args.size() > 3 ? parse_int(args[3]) : 0;
        int z = args.size() > 4 ? parse_int(args[4]) : 0;
        auto cel = parse_optional_int(args, 5);
        api.show_sprite(args[0], parse_int(args[1]), parse_int(args[2]), instance, z, cel);
        return;
    }
    if (name == "hide_sprite") {
        if (args.empty()) throw std::runtime_error("hide_sprite needs name");
        int instance = args.size() > 1 ? parse_int(args[1]) : 0;
        api.hide_sprite(args[0], instance);
        return;
    }
    if (name == "move_sprite") {
        if (args.size() < 3) throw std::runtime_error("move_sprite needs name,x,y");
        int instance = args.size() > 3 ? parse_int(args[3]) : 0;
        auto cel = parse_optional_int(args, 4);
        auto z = parse_optional_int(args, 5);
        api.move_sprite(args[0], parse_int(args[1]), parse_int(args[2]), instance, cel, z);
        return;
    }
    if (name == "dispose_sprite") {
        if (args.empty()) throw std::runtime_error("dispose_sprite needs name");
        int instance = args.size() > 1 ? parse_int(args[1]) : 0;
        api.dispose_sprite(args[0], instance);
        return;
    }
    if (name == "set_background") {
        if (args.empty()) throw std::runtime_error("set_background needs sprite_name");
        int layer = args.size() > 1 ? parse_int(args[1]) : 0;
        int cel = args.size() > 2 ? parse_int(args[2]) : 0;
        api.set_background(args[0], layer, cel);
        return;
    }
    if (name == "hide_background") {
        api.hide_background(parse_optional_int(args, 0));
        return;
    }
    if (name == "nudge_background") {
        if (args.size() < 2) throw std::runtime_error("nudge_background needs dx,dy");
        int layer = args.size() > 2 ? parse_int(args[2]) : 0;
        api.nudge_background(parse_int(args[0]), parse_int(args[1]), layer, parse_optional_int(args, 3));
        return;
    }
    if (name == "set_background_offset") {
        if (args.size() < 2) throw std::runtime_error("set_background_offset needs x,y");
        int layer = args.size() > 2 ? parse_int(args[2]) : 0;
        api.set_background_offset(parse_int(args[0]), parse_int(args[1]), layer, parse_optional_int(args, 3));
        return;
    }
    if (name == "draw_text") {
        if (args.size() < 6) throw std::runtime_error("draw_text needs x,y,text,font,size,color");
        int inten = args.size() > 6 ? parse_int(args[6]) : 100;
        std::string effect = args.size() > 7 ? args[7] : "NORMAL";
        std::string modifier = args.size() > 8 ? args[8] : "";
        api.draw_text(parse_int(args[0]), parse_int(args[1]), args[2], args[3], parse_int(args[4]),
                      require_color(args[5]), inten, effect, modifier);
        return;
    }
    if (name == "clear_text") {
        if (args.size() < 2) throw std::runtime_error("clear_text needs x,y");
        api.clear_text(parse_int(args[0]), parse_int(args[1]));
        return;
    }

    throw std::runtime_error("unsupported consumer command: " + name);
}

}  // namespace pixil::consumer
