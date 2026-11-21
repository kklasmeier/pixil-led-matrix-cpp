#include "consumer/batch_decode.h"

#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

#include "consumer/color_rgb.h"

namespace pixil::consumer {
namespace {

constexpr uint8_t OP_PLOT = 1;
constexpr uint8_t OP_LINE = 2;
constexpr uint8_t OP_RECT = 3;
constexpr uint8_t OP_CIRCLE = 4;
constexpr uint8_t OP_POLYGON = 5;
constexpr uint8_t OP_ELLIPSE = 6;
constexpr uint8_t OP_ARC = 7;
constexpr uint8_t OP_SET_LAYER = 8;
constexpr uint8_t OP_CLEAR_LAYER = 9;

constexpr uint8_t kIntensityDefault = 255;
constexpr uint32_t kBurnoutNone = 0xFFFFFFFFu;

uint16_t ru16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
int16_t ri16(const uint8_t* p) { return static_cast<int16_t>(ru16(p)); }
uint32_t ru32(const uint8_t* p) { return uint32_t(ru16(p)) | (uint32_t(ru16(p + 2)) << 16); }
float rf32(const uint8_t* p) {
    uint32_t bits = ru32(p);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// Protocol: 255 = default(100). Used by plot_batch (mplot) where byte 0 means 0.
int intensity_of(uint8_t b) { return b == kIntensityDefault ? 100 : int(b); }

// draw_batch unpack mirrors Python `intensity or 100` (byte 0 → 100).
int draw_batch_intensity_of(uint8_t b) {
    int v = intensity_of(b);
    return v == 0 ? 100 : v;
}

std::optional<int> burnout_of(uint32_t v) {
    if (v == kBurnoutNone) return std::nullopt;
    return static_cast<int>(v);
}

BurnoutMode mode_of(uint8_t b) { return b == 1 ? BurnoutMode::Fade : BurnoutMode::Instant; }

// Record sizes match shared/draw_batch_protocol.py OP_RECORD_SIZES (incl. op byte).
size_t record_size(uint8_t op) {
    switch (op) {
        case OP_PLOT:
            return 21;
        case OP_LINE:
            return 20;
        case OP_RECT:
            return 20;
        case OP_CIRCLE:
            return 19;
        case OP_POLYGON:
            return 25;
        case OP_ELLIPSE:
            return 25;
        case OP_ARC:
            return 25;
        case OP_SET_LAYER:
        case OP_CLEAR_LAYER:
            return 3;  // op + uint16 layer
        default:
            return 0;
    }
}

}  // namespace

std::vector<uint8_t> base64_decode(std::string_view in) {
    static int8_t kDec[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) kDec[i] = -1;
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; alphabet[i]; ++i) kDec[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
        ready = true;
    }
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int8_t d = kDec[c];
        if (d < 0) throw std::runtime_error(std::string("invalid base64 in draw_batch: char ") + char(c));
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(uint8_t((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

void apply_draw_batch(MatrixApi& api, const std::vector<uint8_t>& bytes) {
    size_t off = 0;
    while (off < bytes.size()) {
        uint8_t op = bytes[off];
        size_t sz = record_size(op);
        if (sz == 0) throw std::runtime_error("unknown draw_batch op " + std::to_string(op));
        if (off + sz > bytes.size()) throw std::runtime_error("truncated draw_batch record");
        const uint8_t* p = bytes.data() + off + 1;
        switch (op) {
            case OP_PLOT: {
                // <HHhBIB8x — mplot semantics: byte 0 stays intensity 0 (no `or 100`).
                int x = ru16(p), y = ru16(p + 2);
                int16_t cid = ri16(p + 4);
                int inten = intensity_of(p[6]);
                auto bo = burnout_of(ru32(p + 7));
                api.plot(x, y, color_from_id(cid), inten, bo, mode_of(p[11]));
                break;
            }
            case OP_LINE: {
                // <4HhBIB3x
                int x0 = ru16(p), y0 = ru16(p + 2), x1 = ru16(p + 4), y1 = ru16(p + 6);
                int16_t cid = ri16(p + 8);
                int inten = draw_batch_intensity_of(p[10]);
                auto bo = burnout_of(ru32(p + 11));
                api.draw_line(x0, y0, x1, y1, color_from_id(cid), inten, bo, mode_of(p[15]));
                break;
            }
            case OP_RECT: {
                // <4HhBIBB2x : x y w h | color intens burnout mode fill | pad2
                int x = ru16(p), y = ru16(p + 2), w = ru16(p + 4), h = ru16(p + 6);
                int16_t cid = ri16(p + 8);
                int inten = draw_batch_intensity_of(p[10]);
                auto bo = burnout_of(ru32(p + 11));
                bool fill = p[16] != 0;
                api.draw_rectangle(x, y, w, h, color_from_id(cid), inten, fill, bo, mode_of(p[15]));
                break;
            }
            case OP_CIRCLE: {
                // <3HhBIBB3x : x y r | color intens burnout mode fill | pad3
                int x = ru16(p), y = ru16(p + 2), r = ru16(p + 4);
                int16_t cid = ri16(p + 6);
                int inten = draw_batch_intensity_of(p[8]);
                auto bo = burnout_of(ru32(p + 9));
                bool fill = p[14] != 0;
                api.draw_circle(x, y, r, color_from_id(cid), inten, fill, bo, mode_of(p[13]));
                break;
            }
            case OP_POLYGON: {
                // <4HfhBIBB3x : x y r sides rot | color intens burnout mode fill | pad3
                int x = ru16(p), y = ru16(p + 2), r = ru16(p + 4), sides = ru16(p + 6);
                float rot = rf32(p + 8);
                int16_t cid = ri16(p + 12);
                int inten = draw_batch_intensity_of(p[14]);
                auto bo = burnout_of(ru32(p + 15));
                bool fill = p[20] != 0;
                api.draw_polygon(x, y, r, sides, color_from_id(cid), inten, rot, fill, bo, mode_of(p[19]));
                break;
            }
            case OP_ELLIPSE: {
                // <4HfhBIBB3x : xc yc xr yr rot | color intens burnout mode fill | pad3
                int xc = ru16(p), yc = ru16(p + 2), xr = ru16(p + 4), yr = ru16(p + 6);
                float rot = rf32(p + 8);
                int16_t cid = ri16(p + 12);
                int inten = draw_batch_intensity_of(p[14]);
                auto bo = burnout_of(ru32(p + 15));
                bool fill = p[20] != 0;
                api.draw_ellipse(xc, yc, xr, yr, color_from_id(cid), inten, fill, rot, bo, mode_of(p[19]));
                break;
            }
            case OP_ARC: {
                // <4HfhBIBB3x : x0 y0 x1 y1 bulge | color intens burnout mode fill | pad3
                int x0 = ru16(p), y0 = ru16(p + 2), x1 = ru16(p + 4), y1 = ru16(p + 6);
                float bulge = rf32(p + 8);
                int16_t cid = ri16(p + 12);
                int inten = draw_batch_intensity_of(p[14]);
                auto bo = burnout_of(ru32(p + 15));
                bool fill = p[20] != 0;
                api.draw_arc(x0, y0, x1, y1, bulge, color_from_id(cid), inten, fill, bo, mode_of(p[19]));
                break;
            }
            case OP_SET_LAYER:
                api.set_layer(static_cast<int>(ru16(p)));
                break;
            case OP_CLEAR_LAYER:
                api.clear_layer(static_cast<int>(ru16(p)));
                break;
            default:
                break;
        }
        off += sz;
    }
}

void apply_plot_batch(MatrixApi& api, const std::vector<uint8_t>& bytes) {
    constexpr size_t kRec = 20;  // <HHhBIB8x
    if (bytes.size() % kRec != 0) {
        throw std::runtime_error("plot_batch size not a multiple of 20");
    }
    for (size_t off = 0; off < bytes.size(); off += kRec) {
        const uint8_t* p = bytes.data() + off;
        int x = ru16(p), y = ru16(p + 2);
        int16_t cid = ri16(p + 4);
        int inten = intensity_of(p[6]);
        auto bo = burnout_of(ru32(p + 7));
        api.stamp_plot(x, y, color_from_id(cid), inten, bo, mode_of(p[11]));
    }
}

void apply_sprite_batch(MatrixApi& api, const std::vector<uint8_t>& bytes) {
    constexpr uint8_t OP_SHOW = 1, OP_MOVE = 2, OP_HIDE = 3, OP_MOVE_Z = 4;
    constexpr uint8_t OP_TRANSFORM = 5, OP_MOVE_SUB = 6, OP_SHOW_SUB = 7;
    size_t off = 0;
    const size_t n = bytes.size();
    auto cel_opt = [](uint8_t cel_b) -> std::optional<int> {
        return cel_b == 255 ? std::optional<int>{} : std::optional<int>{int(cel_b)};
    };
    while (off < n) {
        if (api.abort_requested()) break;
        uint8_t op = bytes[off++];
        if (off >= n) throw std::runtime_error("truncated sprite_batch name length");
        uint8_t name_len = bytes[off++];
        if (off + name_len > n) throw std::runtime_error("truncated sprite_batch name");
        std::string name(reinterpret_cast<const char*>(bytes.data() + off), name_len);
        off += name_len;
        if (op == OP_HIDE) {
            if (off + 1 > n) throw std::runtime_error("truncated hide_sprite record");
            int instance_id = bytes[off++];
            api.hide_sprite(name, instance_id);
            continue;
        }
        if (op == OP_TRANSFORM) {
            // instance, sx, sy, angle, tilt_x, tilt_y, flags, reserved — 13 bytes
            if (off + 13 > n) throw std::runtime_error("truncated sprite_transform record");
            int instance_id = bytes[off];
            float scale_x = static_cast<float>(ru16(bytes.data() + off + 1)) / 256.f;
            float scale_y = static_cast<float>(ru16(bytes.data() + off + 3)) / 256.f;
            float angle = static_cast<float>(ru16(bytes.data() + off + 5)) / 100.f;
            float tilt_x = static_cast<float>(ru16(bytes.data() + off + 7)) / 100.f;
            float tilt_y = static_cast<float>(ru16(bytes.data() + off + 9)) / 100.f;
            uint8_t flags = bytes[off + 11];
            off += 13;
            SpriteTransform t;
            t.scale_x = scale_x;
            t.scale_y = scale_y;
            t.angle_deg = angle;
            t.tilt_x_deg = tilt_x;
            t.tilt_y_deg = tilt_y;
            t.flip_h = (flags & 1) != 0;
            t.flip_v = (flags & 2) != 0;
            t.centre_anchor = (flags & 4) != 0;
            api.set_sprite_transform_state(name, instance_id, t);
            continue;
        }
        if (op == OP_SHOW || op == OP_SHOW_SUB) {
            if (off + 8 > n) throw std::runtime_error("truncated show_sprite record");
            float x = static_cast<float>(ri16(bytes.data() + off));
            float y = static_cast<float>(ri16(bytes.data() + off + 2));
            if (op == OP_SHOW_SUB) {
                x /= 16.f;
                y /= 16.f;
            }
            int instance_id = bytes[off + 4];
            uint8_t cel_b = bytes[off + 5];
            int z = ri16(bytes.data() + off + 6);
            off += 8;
            api.show_sprite(name, x, y, instance_id, z, cel_opt(cel_b));
            continue;
        }
        if (op == OP_MOVE) {
            if (off + 6 > n) throw std::runtime_error("truncated move_sprite record");
            float x = static_cast<float>(ri16(bytes.data() + off));
            float y = static_cast<float>(ri16(bytes.data() + off + 2));
            int instance_id = bytes[off + 4];
            uint8_t cel_b = bytes[off + 5];
            off += 6;
            api.move_sprite(name, x, y, instance_id, cel_opt(cel_b), std::nullopt);
            continue;
        }
        if (op == OP_MOVE_Z || op == OP_MOVE_SUB) {
            if (off + 8 > n) throw std::runtime_error("truncated move_sprite+z record");
            float x = static_cast<float>(ri16(bytes.data() + off));
            float y = static_cast<float>(ri16(bytes.data() + off + 2));
            if (op == OP_MOVE_SUB) {
                x /= 16.f;
                y /= 16.f;
            }
            int instance_id = bytes[off + 4];
            uint8_t cel_b = bytes[off + 5];
            int z = ri16(bytes.data() + off + 6);
            off += 8;
            api.move_sprite(name, x, y, instance_id, cel_opt(cel_b), z);
            continue;
        }
        throw std::runtime_error("unknown sprite_batch op " + std::to_string(op));
    }
}

std::string base64_encode_for_test(const uint8_t* data, size_t len) {
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

}  // namespace pixil::consumer
