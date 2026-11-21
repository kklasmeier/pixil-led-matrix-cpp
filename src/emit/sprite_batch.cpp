#include "emit/sprite_batch.h"

#include <cmath>

#include "emit/draw_batch.h"
#include "runtime/python_ops.h"

namespace pixil {

namespace {

constexpr uint8_t kOpShow = 1;
constexpr uint8_t kOpMove = 2;
constexpr uint8_t kOpHide = 3;
constexpr uint8_t kOpMoveZ = 4;
constexpr uint8_t kOpTransform = 5;
constexpr uint8_t kOpMoveSub = 6;
constexpr uint8_t kOpShowSub = 7;
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

void append_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
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

// 8.8 fixed from a positive scale (0.0–255.99).
uint16_t scale_to_u8_8(double s) {
    if (s < 0.0) s = 0.0;
    if (s > 255.99) s = 255.99;
    return static_cast<uint16_t>(std::lround(s * 256.0));
}

// Degrees → hundredths in 0..35999.
uint16_t angle_to_centi(double deg) {
    double wrapped = std::fmod(deg, 360.0);
    if (wrapped < 0.0) wrapped += 360.0;
    return static_cast<uint16_t>(std::lround(wrapped * 100.0) % 36000);
}

int64_t to_fixed_1_16(const Value& v) {
    return static_cast<int64_t>(std::lround(v.to_double() * 16.0));
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
    if (cmd == "sprite_transform") {
        // name, instance, scale_x, scale_y, angle, tilt_x, tilt_y, flags
        if (args.size() < 8) arg_error("sprite_transform requires name, inst, sx, sy, angle, tx, ty, flags");
        bytes_.push_back(kOpTransform);
        append_name(bytes_, name);
        bytes_.push_back(byte_of(rounded_int(args[1])));
        append_u16(bytes_, scale_to_u8_8(args[2].to_double()));
        append_u16(bytes_, scale_to_u8_8(args[3].to_double()));
        append_u16(bytes_, angle_to_centi(args[4].to_double()));
        append_u16(bytes_, angle_to_centi(args[5].to_double()));
        append_u16(bytes_, angle_to_centi(args[6].to_double()));
        bytes_.push_back(byte_of(rounded_int(args[7])));
        bytes_.push_back(0);  // reserved
        return;
    }

    const bool show = cmd == "show_sprite" || cmd == "show_sprite_sub";
    const bool move = cmd == "move_sprite" || cmd == "move_sprite_sub";
    const bool sub = cmd == "show_sprite_sub" || cmd == "move_sprite_sub";
    if (!show && !move) {
        arg_error("sprite_batch does not support command: " + std::string(cmd));
    }
    if (args.size() < 3) arg_error(std::string(cmd) + " requires a name, x, and y");

    const int64_t x = sub ? to_fixed_1_16(args[1]) : rounded_int(args[1]);
    const int64_t y = sub ? to_fixed_1_16(args[2]) : rounded_int(args[2]);
    const uint8_t instance = args.size() > 3 ? byte_of(rounded_int(args[3])) : 0;
    uint8_t cel = kCelNone;
    bool with_z = false;
    int64_t z = 0;
    uint8_t op = 0;
    if (show) {
        if (args.size() > 4) z = rounded_int(args[4]);
        if (args.size() > 5) cel = byte_of(rounded_int(args[5]));
        with_z = true;
        op = sub ? kOpShowSub : kOpShow;
    } else {
        if (args.size() > 4) cel = byte_of(rounded_int(args[4]));
        if (args.size() > 5) {
            with_z = true;
            z = rounded_int(args[5]);
            op = sub ? kOpMoveSub : kOpMoveZ;
        } else if (sub) {
            // OP_MOVE_SUB always carries a z trailer (0 if omitted) so the size is fixed.
            with_z = true;
            z = 0;
            op = kOpMoveSub;
        } else {
            op = kOpMove;
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
