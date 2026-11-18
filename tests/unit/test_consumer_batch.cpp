#include <cstring>
#include <vector>

#include "check.h"
#include "consumer/api.h"
#include "consumer/batch_decode.h"
#include "consumer/matrix.h"

using namespace pixil::consumer;

namespace {

std::vector<uint8_t> pack_mplot(uint16_t x, uint16_t y, int16_t color_id, uint8_t intensity = 100) {
    std::vector<uint8_t> r(20, 0);
    r[0] = uint8_t(x & 0xff);
    r[1] = uint8_t(x >> 8);
    r[2] = uint8_t(y & 0xff);
    r[3] = uint8_t(y >> 8);
    r[4] = uint8_t(uint16_t(color_id) & 0xff);
    r[5] = uint8_t(uint16_t(color_id) >> 8);
    r[6] = intensity;
    // burnout 0xFFFFFFFF = none
    r[7] = r[8] = r[9] = r[10] = 0xff;
    r[11] = 0;  // mode instant
    return r;
}

std::vector<uint8_t> pack_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, int16_t color_id) {
    // op + <4HhBIB3x> = 20 bytes total
    std::vector<uint8_t> r(20, 0);
    r[0] = 2;  // OP_LINE
    auto put = [&](int off, uint16_t v) {
        r[off] = uint8_t(v & 0xff);
        r[off + 1] = uint8_t(v >> 8);
    };
    put(1, x0);
    put(3, y0);
    put(5, x1);
    put(7, y1);
    put(9, uint16_t(color_id));
    r[11] = 100;
    r[12] = r[13] = r[14] = r[15] = 0xff;
    return r;
}

}  // namespace

TEST(consumer_base64_roundtrip) {
    const uint8_t raw[] = {1, 2, 3, 4, 5, 250, 255, 0};
    auto enc = base64_encode_for_test(raw, sizeof raw);
    auto dec = base64_decode(enc);
    CHECK_EQ(dec.size(), sizeof raw);
    CHECK(std::memcmp(dec.data(), raw, sizeof raw) == 0);
}

TEST(consumer_plot_batch_records) {
    auto a = pack_mplot(1, 2, -7);   // red
    auto b = pack_mplot(3, 4, 42);   // spectral
    std::vector<uint8_t> buf = a;
    buf.insert(buf.end(), b.begin(), b.end());

    MatrixApi api(make_headless_matrix());
    api.begin_frame(false);
    api.plot_batch_bytes(buf);
    api.end_frame();
    CHECK(api.fingerprint() != "empty");
}

TEST(consumer_draw_batch_line_size) {
    auto rec = pack_line(0, 0, 10, 10, -2);  // white
    CHECK_EQ(rec.size(), 20u);
    MatrixApi api(make_headless_matrix());
    api.begin_frame(false);
    api.draw_batch_bytes(rec);
    api.end_frame();
    CHECK(api.fingerprint() != "empty");
}

TEST(consumer_draw_arc_text) {
    MatrixApi api(make_headless_matrix());
    api.begin_frame(false);
    api.execute_command("draw_arc(5, 32, 58, 32, 12, gold, 100, false)");
    api.end_frame();
    CHECK(api.fingerprint() != "empty");
}

TEST(consumer_burnout_args_accepted) {
    MatrixApi api(make_headless_matrix());
    // Trailing burnout fields must not throw (C4 will honor them).
    api.execute_command("plot(10, 10, red, 100, 500, instant)");
    api.execute_command("draw_line(0, 0, 8, 8, blue, 80, 1000, fade)");
    api.execute_command("draw_circle(32, 32, 5, green, 100, true, 250, instant)");
}

TEST(consumer_sprite_define_show_frame) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(ball, 8, 8)");
    api.execute_command("sprite_draw(ball, draw_circle, 4, 4, 3, yellow, 100, true)");
    api.execute_command("endsprite");
    api.execute_command("begin_frame(false)");
    api.execute_command("show_sprite(ball, 10, 10, 0, 0, 0)");
    api.execute_command("end_frame");
}

TEST(consumer_sprite_batch_show_hide) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(dot, 2, 2)");
    api.execute_command("sprite_draw(dot, plot, 0, 0, red, 100)");
    api.execute_command("sprite_draw(dot, plot, 1, 1, red, 100)");
    api.execute_command("endsprite");

    // Pack: OP_SHOW + name_len + "dot" + x,y,instance,cel
    std::vector<uint8_t> rec;
    rec.push_back(1);  // OP_SHOW
    rec.push_back(3);
    rec.push_back('d');
    rec.push_back('o');
    rec.push_back('t');
    auto put16 = [&](int16_t v) {
        rec.push_back(uint8_t(uint16_t(v) & 0xff));
        rec.push_back(uint8_t(uint16_t(v) >> 8));
    };
    put16(5);
    put16(6);
    rec.push_back(0);    // instance
    rec.push_back(255);  // cel none
    api.begin_frame(false);
    api.sprite_batch_bytes(rec);
    api.end_frame();

    std::vector<uint8_t> hide;
    hide.push_back(3);  // OP_HIDE
    hide.push_back(3);
    hide.push_back('d');
    hide.push_back('o');
    hide.push_back('t');
    hide.push_back(0);
    api.begin_frame(false);
    api.sprite_batch_bytes(hide);
    api.end_frame();
}

TEST(consumer_background_and_text) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(tile, 4, 4)");
    api.execute_command("sprite_draw(tile, draw_rectangle, 0, 0, 4, 4, blue, 100, true)");
    api.execute_command("endsprite");
    api.execute_command("set_background(tile, 0, 0)");
    api.execute_command("begin_frame(false)");
    api.execute_command("draw_text(2, 2, HI, tiny64_font, 1, white, 100, NORMAL)");
    api.execute_command("end_frame");
    CHECK(api.fingerprint() != "empty");
    api.execute_command("hide_background");
    api.execute_command("clear_text(2, 2)");
}

TEST(consumer_burnout_instant_clears) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("plot(20, 20, red, 100, 60, instant)");
    CHECK(api.fingerprint() != "empty");
    api.rest(0.12);
    CHECK_EQ(api.fingerprint(), std::string("empty"));
}

TEST(consumer_burnout_fade_progresses) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("plot(30, 30, blue, 100, 200, fade)");
    std::string mid;
    api.rest(0.05);
    mid = api.fingerprint();
    CHECK(mid != "empty");
    api.rest(0.25);
    CHECK_EQ(api.fingerprint(), std::string("empty"));
}
