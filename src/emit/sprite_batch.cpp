#include "emit/sprite_batch.h"

#include "emit/draw_batch.h"
#include "runtime/python_ops.h"

namespace pixil {

namespace {

constexpr uint8_t kOpShow = 1;
constexpr uint8_t kOpMove = 2;
constexpr uint8_t kOpHide = 3;
constexpr uint8_t kOpMoveZ = 4;
constexpr uint8_t kCelNone = 255;
constexpr size_t kMaxNameLen = 48;

[[noreturn]] void arg_error(const std::string& msg) { throw PixilError(ErrorKind::Value, msg); }

int64_t rounded_int(const Value& v) {
    Value r = py_round(v);
    if (r.is_int()) return r.as_int();
    if (r.is_float()) return static_cast<int64_t>(r.as_float());
    arg_error(std::string("cannot round ") + v.type_name());
}

uint8_t byte_of(int64_t v) { return static_cast<uint8_t>(v & 0xFF); }

void append_i16(std::vector<uint8_t>& out, int64_t v) {
    auto n = static_cast<uint16_t>(static_cast<int16_t>(v));
    out.push_back(static_cast<uint8_t>(n & 0xFF));
    out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
}

std::string sprite_name(const Value& v) {
    if (!v.is_str()) arg_error(std::string("sprite name must be a string, not ") + v.type_name());
    std::string name = v.as_str();
    if (name.size() >= 2 && ((name.front() == '"' && name.back() == '"') ||
                             (name.front() == '\'' && name.back() == '\''))) {
        name = name.substr(1, name.size() - 2);
    }
    if (name.size() > kMaxNameLen) arg_error("sprite name too long (max 48): " + name);
    return name;
}

void append_name(std::vector<uint8_t>& out, const std::string& name) {
    out.push_back(static_cast<uint8_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
}

}  // namespace

void SpriteBatch::append(std::string_view cmd, const std::vector<Value>& args) {
    if (args.empty()) arg_error(std::string(cmd) + " requires a sprite name");
    const std::string name = sprite_name(args[0]);

    if (cmd == "hide_sprite") {
        bytes_.push_back(kOpHide);
        append_name(bytes_, name);
        bytes_.push_back(args.size() > 1 ? byte_of(rounded_int(args[1])) : 0);
        return;
    }
    if (cmd != "show_sprite" && cmd != "move_sprite") {
        arg_error("sprite_batch does not support command: " + std::string(cmd));
    }
    if (args.size() < 3) arg_error(std::string(cmd) + " requires a name, x, and y");

    const int64_t x = rounded_int(args[1]);
    const int64_t y = rounded_int(args[2]);
    const uint8_t instance = args.size() > 3 ? byte_of(rounded_int(args[3])) : 0;
    uint8_t cel = kCelNone;
    bool with_z = false;
    int64_t z = 0;
    uint8_t op = cmd == "show_sprite" ? kOpShow : kOpMove;
    if (cmd == "show_sprite") {
        // Pack z on OP_SHOW (args[4]); cel remains args[5].
        if (args.size() > 4) z = rounded_int(args[4]);
        if (args.size() > 5) cel = byte_of(rounded_int(args[5]));
        with_z = true;
    } else {
        if (args.size() > 4) cel = byte_of(rounded_int(args[4]));
        if (args.size() > 5) {
            with_z = true;
            z = rounded_int(args[5]);
            op = kOpMoveZ;
        }
    }

    bytes_.push_back(op);
    append_name(bytes_, name);
    append_i16(bytes_, x);
    append_i16(bytes_, y);
    bytes_.push_back(instance);
    bytes_.push_back(cel);
    if (with_z) append_i16(bytes_, z);
}

std::string SpriteBatch::command() const {
    return "sprite_batch(\"" + base64_encode(bytes_.data(), bytes_.size()) + "\")";
}

}  // namespace pixil
