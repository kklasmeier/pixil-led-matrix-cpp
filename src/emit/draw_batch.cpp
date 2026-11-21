#include "emit/draw_batch.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "emit/colors.h"
#include "runtime/python_ops.h"

namespace pixil {

namespace {

constexpr uint8_t kIntensityDefault = 255;
constexpr uint32_t kBurnoutNone = 0xFFFFFFFFu;

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : out_(out) {}
    void u8(uint32_t v) { out_.push_back(static_cast<uint8_t>(v)); }
    void u16(uint32_t v) {
        u8(v & 0xFF);
        u8((v >> 8) & 0xFF);
    }
    void i16(int32_t v) { u16(static_cast<uint32_t>(v) & 0xFFFF); }
    void u32(uint32_t v) {
        u16(v & 0xFFFF);
        u16(v >> 16);
    }
    void f32(double v) {
        float f = static_cast<float>(v);
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        u32(bits);
    }
    void pad(int n) {
        for (int i = 0; i < n; ++i) u8(0);
    }

private:
    std::vector<uint8_t>& out_;
};

[[noreturn]] void arg_error(const std::string& msg) { throw PixilError(ErrorKind::Value, msg); }

// Python float(value) for parsed arguments (numbers or numeric strings).
double py_float(const Value& v) {
    if (v.is_number()) return v.to_double();
    if (v.is_str()) {
        const char* s = v.as_str().c_str();
        char* end = nullptr;
        double d = std::strtod(s, &end);
        while (end && *end == ' ') ++end;
        if (end != s && end && *end == '\0') return d;
        arg_error("could not convert string to float: " + py_repr(v));
    }
    throw PixilError(ErrorKind::Type, std::string("float() argument must be a string or a real number, not '") +
                                          v.type_name() + "'");
}

// int(float(value))
int64_t trunc_int(const Value& v) {
    double d = py_float(v);
    if (!std::isfinite(d)) arg_error("cannot convert non-finite float to integer");
    return static_cast<int64_t>(std::trunc(d));
}

uint32_t coord_uint16(const Value& v) {
    int64_t n = trunc_int(v);
    return static_cast<uint32_t>(std::clamp<int64_t>(n, 0, 65535));
}

uint32_t coord_uint16(int64_t n) { return static_cast<uint32_t>(std::clamp<int64_t>(n, 0, 65535)); }

// int(value) as Python would apply it (floats truncate, decimal strings parse).
int64_t py_int_arg(const Value& v) {
    Value r = py_int(v);
    if (!r.is_int()) arg_error("integer argument out of range");
    return r.as_int();
}

uint8_t intensity_byte(const Value* v) {
    if (v == nullptr || v->is_none()) return kIntensityDefault;
    int64_t n = py_int_arg(*v);
    if (n < 0 || n > 255) arg_error("ubyte format requires 0 <= number <= 255");
    return static_cast<uint8_t>(n);
}

uint32_t burnout_uint(const Value* v) {
    if (v == nullptr || v->is_none()) return kBurnoutNone;
    int64_t n = py_int_arg(*v);
    if (n < 0 || n > 0xFFFFFFFFLL) arg_error("argument out of range");
    return static_cast<uint32_t>(n);
}

std::string strip_quotes(std::string s) {
    auto strip = [&](char q) {
        size_t b = 0, e = s.size();
        while (b < e && s[b] == q) ++b;
        while (e > b && s[e - 1] == q) --e;
        s = s.substr(b, e - b);
    };
    strip('"');
    strip('\'');
    return s;
}

uint8_t burnout_mode_byte(const Value* v, bool strip) {
    if (v == nullptr || v->is_none()) return 0;
    if (!v->is_str()) {
        throw PixilError(ErrorKind::Type, std::string("burnout mode must be a string, got ") + v->type_name());
    }
    std::string m = strip ? strip_quotes(v->as_str()) : v->as_str();
    for (auto& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (m == "instant") return 0;
    if (m == "fade") return 1;
    arg_error("Unknown burnout mode: " + v->as_str() + ". Must be 'instant' or 'fade'");
}

uint8_t fill_byte(const Value& v) {
    if (v.is_bool()) return v.as_bool() ? 1 : 0;
    if (v.is_str()) {
        std::string s = v.as_str();
        size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
        s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return (s == "true" || s == "1" || s == "yes") ? 1 : 0;
    }
    return truthy(v) ? 1 : 0;
}

bool center_radius_visible(const Value& xv, const Value& yv, int64_t r) {
    int64_t xc = trunc_int(xv), yc = trunc_int(yv);
    if (r <= 0) return false;
    if (xc < 0 || yc < 0) return false;
    if (xc + r <= 0 || yc + r <= 0) return false;
    if (xc - r >= kPanelWidth || yc - r >= kPanelHeight) return false;
    return true;
}

struct Segment {
    int64_t x0, y0, x1, y1;
};

int outcode(double x, double y, double xmax, double ymax) {
    int code = 0;
    if (x < 0) code |= 1;
    else if (x > xmax) code |= 2;
    if (y < 0) code |= 4;
    else if (y > ymax) code |= 8;
    return code;
}

int64_t py_round_int(double d) { return static_cast<int64_t>(std::nearbyint(d)); }

// Cohen–Sutherland clip to [0, w-1] x [0, h-1] (clip_line_segment).
std::optional<Segment> clip_line(double x0, double y0, double x1, double y1) {
    const double xmax = kPanelWidth - 1, ymax = kPanelHeight - 1;
    int o0 = outcode(x0, y0, xmax, ymax), o1 = outcode(x1, y1, xmax, ymax);
    for (;;) {
        if (!(o0 | o1)) return Segment{py_round_int(x0), py_round_int(y0), py_round_int(x1), py_round_int(y1)};
        if (o0 & o1) return std::nullopt;
        int out = o0 ? o0 : o1;
        double x, y;
        if (out & 8) {
            x = x0 + (x1 - x0) * (ymax - y0) / (y1 - y0);
            y = ymax;
        } else if (out & 4) {
            x = x0 + (x1 - x0) * (0.0 - y0) / (y1 - y0);
            y = 0.0;
        } else if (out & 2) {
            y = y0 + (y1 - y0) * (xmax - x0) / (x1 - x0);
            x = xmax;
        } else {
            y = y0 + (y1 - y0) * (0.0 - x0) / (x1 - x0);
            x = 0.0;
        }
        if (out == o0) {
            x0 = x;
            y0 = y;
            o0 = outcode(x0, y0, xmax, ymax);
        } else {
            x1 = x;
            y1 = y;
            o1 = outcode(x1, y1, xmax, ymax);
        }
    }
}

void need(const std::vector<Value>& args, size_t n, std::string_view cmd) {
    if (args.size() < n) {
        throw PixilError(ErrorKind::Type, std::string(cmd) + " requires at least " + std::to_string(n) + " arguments");
    }
}

const Value* opt(const std::vector<Value>& args, size_t i) { return i < args.size() ? &args[i] : nullptr; }

const Value kHundred = Value::integer(100);
const Value kFalse = Value::boolean(false);

void tail(Writer& w, int16_t color, const Value* intensity, const Value* burnout, const Value* mode, bool strip_mode) {
    w.i16(color);
    w.u8(intensity_byte(intensity));
    w.u32(burnout_uint(burnout));
    w.u8(burnout_mode_byte(mode, strip_mode));
}

}  // namespace

bool DrawBatch::append(std::string_view cmd, const std::vector<Value>& a) {
    const size_t before = bytes_.size();
    try {
        if (!append_record(cmd, a)) return false;
    } catch (...) {
        bytes_.resize(before);
        throw;
    }
    ++records_;
    return true;
}

bool DrawBatch::append_record(std::string_view cmd, const std::vector<Value>& a) {
    Writer w(bytes_);
    if (cmd == "plot" || cmd == "mplot") {
        need(a, 3, cmd);
        uint32_t x = coord_uint16(a[0]), y = coord_uint16(a[1]);
        int16_t color = color_id_from_value(a[2]);
        std::optional<int64_t> intensity;
        if (a.size() > 3 && !a[3].is_none() && !(a[3].is_str() && a[3].as_str().empty())) {
            intensity = trunc_int(a[3]);
            if (*intensity < 0 || *intensity > 100) {
                arg_error("Intensity must be 0-100, got " + std::to_string(*intensity));
            }
        }
        std::optional<int64_t> burnout;
        if (a.size() > 4 && !a[4].is_none() && !(a[4].is_str() && a[4].as_str().empty())) {
            burnout = trunc_int(a[4]);
            if (*burnout < 0 || *burnout > 4294967294LL) arg_error("Burnout must be 0-4294967294");
        }
        w.u8(OP_PLOT);
        w.u16(x);
        w.u16(y);
        w.i16(color);
        w.u8(intensity ? static_cast<uint32_t>(*intensity) : kIntensityDefault);
        w.u32(burnout ? static_cast<uint32_t>(*burnout) : kBurnoutNone);
        w.u8(burnout_mode_byte(opt(a, 5), true));
        w.pad(8);
    } else if (cmd == "draw_line") {
        need(a, 5, cmd);
        auto seg = clip_line(py_float(a[0]), py_float(a[1]), py_float(a[2]), py_float(a[3]));
        if (!seg) return false;
        int16_t color = color_id_from_value(a[4]);
        w.u8(OP_LINE);
        w.u16(coord_uint16(seg->x0));
        w.u16(coord_uint16(seg->y0));
        w.u16(coord_uint16(seg->x1));
        w.u16(coord_uint16(seg->y1));
        tail(w, color, a.size() > 5 ? &a[5] : &kHundred, opt(a, 6), opt(a, 7), true);
        w.pad(3);
    } else if (cmd == "draw_rectangle") {
        need(a, 5, cmd);
        int64_t left = trunc_int(a[0]), top = trunc_int(a[1]);
        int64_t rw = trunc_int(a[2]), rh = trunc_int(a[3]);
        if (rw <= 0 || rh <= 0) return false;
        int64_t right = std::min<int64_t>(left + rw, kPanelWidth);
        int64_t bottom = std::min<int64_t>(top + rh, kPanelHeight);
        int64_t cl = std::max<int64_t>(left, 0), ct = std::max<int64_t>(top, 0);
        if (right <= cl || bottom <= ct) return false;
        int16_t color = color_id_from_value(a[4]);
        w.u8(OP_RECT);
        w.u16(coord_uint16(cl));
        w.u16(coord_uint16(ct));
        w.u16(coord_uint16(right - cl));
        w.u16(coord_uint16(bottom - ct));
        tail(w, color, a.size() > 5 ? &a[5] : &kHundred, opt(a, 7), opt(a, 8), false);
        w.u8(fill_byte(a.size() > 6 ? a[6] : kFalse));
        w.pad(2);
    } else if (cmd == "draw_circle") {
        need(a, 4, cmd);
        if (!center_radius_visible(a[0], a[1], trunc_int(a[2]))) return false;
        int16_t color = color_id_from_value(a[3]);
        w.u8(OP_CIRCLE);
        w.u16(coord_uint16(a[0]));
        w.u16(coord_uint16(a[1]));
        w.u16(coord_uint16(a[2]));
        tail(w, color, a.size() > 4 ? &a[4] : &kHundred, opt(a, 6), opt(a, 7), false);
        w.u8(fill_byte(a.size() > 5 ? a[5] : kFalse));
        w.pad(3);
    } else if (cmd == "draw_polygon") {
        need(a, 5, cmd);
        double rotation = a.size() > 6 ? py_float(a[6]) : 0.0;
        if (!center_radius_visible(a[0], a[1], trunc_int(a[2]))) return false;
        int16_t color = color_id_from_value(a[4]);
        w.u8(OP_POLYGON);
        w.u16(coord_uint16(a[0]));
        w.u16(coord_uint16(a[1]));
        w.u16(coord_uint16(a[2]));
        w.u16(coord_uint16(a[3]));
        w.f32(rotation);
        tail(w, color, a.size() > 5 ? &a[5] : &kHundred, opt(a, 8), opt(a, 9), false);
        w.u8(fill_byte(a.size() > 7 ? a[7] : kFalse));
        w.pad(3);
    } else if (cmd == "draw_ellipse") {
        need(a, 5, cmd);
        double rotation = a.size() > 7 ? py_float(a[7]) : 0.0;
        int64_t xr = trunc_int(a[2]), yr = trunc_int(a[3]);
        if (xr <= 0 && yr <= 0) return false;
        if (!center_radius_visible(a[0], a[1], std::max(xr, yr))) return false;
        int16_t color = color_id_from_value(a[4]);
        w.u8(OP_ELLIPSE);
        w.u16(coord_uint16(a[0]));
        w.u16(coord_uint16(a[1]));
        w.u16(coord_uint16(a[2]));
        w.u16(coord_uint16(a[3]));
        w.f32(rotation);
        tail(w, color, a.size() > 5 ? &a[5] : &kHundred, opt(a, 8), opt(a, 9), false);
        w.u8(fill_byte(a.size() > 6 ? a[6] : kFalse));
        w.pad(3);
    } else if (cmd == "draw_arc") {
        need(a, 6, cmd);
        auto seg = clip_line(py_float(a[0]), py_float(a[1]), py_float(a[2]), py_float(a[3]));
        if (!seg) return false;
        double bulge = py_float(a[4]);
        int16_t color = color_id_from_value(a[5]);
        w.u8(OP_ARC);
        w.u16(coord_uint16(seg->x0));
        w.u16(coord_uint16(seg->y0));
        w.u16(coord_uint16(seg->x1));
        w.u16(coord_uint16(seg->y1));
        w.f32(bulge);
        tail(w, color, a.size() > 6 ? &a[6] : &kHundred, opt(a, 8), opt(a, 9), false);
        w.u8(fill_byte(a.size() > 7 ? a[7] : kFalse));
        w.pad(3);
    } else if (cmd == "set_layer" || cmd == "clear_layer") {
        need(a, 1, cmd);
        int64_t layer = trunc_int(a[0]);
        if (layer < 0 || layer > 65535) arg_error(std::string(cmd) + " layer must be 0..65535");
        w.u8(cmd == "set_layer" ? OP_SET_LAYER : OP_CLEAR_LAYER);
        w.u16(static_cast<uint32_t>(layer));
    } else {
        throw PixilError(ErrorKind::Value, "draw_batch does not support command: " + std::string(cmd));
    }
    return true;
}

std::string DrawBatch::command() const {
    return "draw_batch(\"" + base64_encode(bytes_.data(), bytes_.size()) + "\")";
}

std::string base64_encode(const uint8_t* data, size_t len) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t n = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8) | data[i + 2];
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(kAlphabet[(n >> 6) & 63]);
        out.push_back(kAlphabet[n & 63]);
    }
    if (i < len) {
        uint32_t n = uint32_t{data[i]} << 16;
        if (i + 1 < len) n |= uint32_t{data[i + 1]} << 8;
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? kAlphabet[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

}  // namespace pixil
