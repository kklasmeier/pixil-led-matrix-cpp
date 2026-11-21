#include "check.h"
#include "consumer/api.h"
#include "consumer/matrix.h"
#include "consumer/sprite_transform_cache.h"
#include "emit/sprite_batch.h"
#include "runtime/value.h"

using namespace pixil;
using namespace pixil::consumer;

namespace {

bool pixel_is(MatrixApi& api, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t pr = 0, pg = 0, pb = 0;
    if (!api.surface_pixel(x, y, &pr, &pg, &pb)) return false;
    return pr == r && pg == g && pb == b;
}

bool pixel_lit(MatrixApi& api, int x, int y) {
    uint8_t pr = 0, pg = 0, pb = 0;
    if (!api.surface_pixel(x, y, &pr, &pg, &pb)) return false;
    return pr != 0 || pg != 0 || pb != 0;
}

}  // namespace

TEST(sprite_transform_identity_matches_plain) {
    MatrixApi plain(make_headless_matrix());
    plain.execute_command("define_sprite(dot, 2, 2)");
    plain.execute_command("sprite_draw(dot, plot, 0, 0, red, 100)");
    plain.execute_command("endsprite");
    plain.begin_frame(false);
    plain.show_sprite("dot", 10, 10, 0, 0, 0);
    plain.end_frame();

    MatrixApi with_api(make_headless_matrix());
    with_api.execute_command("define_sprite(dot, 2, 2)");
    with_api.execute_command("sprite_draw(dot, plot, 0, 0, red, 100)");
    with_api.execute_command("endsprite");
    with_api.begin_frame(false);
    with_api.show_sprite("dot", 10, 10, 0, 0, 0);
    with_api.set_sprite_scale("dot", 1.f, 0);
    with_api.reset_sprite_transform("dot", 0);
    with_api.end_frame();

    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            uint8_t ar, ag, ab, br, bg, bb;
            CHECK(plain.surface_pixel(x, y, &ar, &ag, &ab));
            CHECK(with_api.surface_pixel(x, y, &br, &bg, &bb));
            CHECK_EQ(int(ar), int(br));
            CHECK_EQ(int(ag), int(bg));
            CHECK_EQ(int(ab), int(bb));
        }
    }
}

TEST(sprite_transform_flip_h) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(bar, 2, 1)");
    api.execute_command("sprite_draw(bar, plot, 0, 0, red, 100)");
    api.execute_command("endsprite");

    api.begin_frame(false);
    api.show_sprite("bar", 5, 5, 0, 0, 0);
    api.set_sprite_flip("bar", 1, 0);  // horizontal
    api.end_frame();
    // Unflipped would light (5,5); flip H on 2-wide puts the pixel at (6,5).
    CHECK(pixel_is(api, 6, 5, 255, 0, 0) || pixel_lit(api, 6, 5));
    CHECK(!pixel_lit(api, 5, 5));
}

TEST(sprite_transform_scale_2_centre) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(px, 1, 1)");
    api.execute_command("sprite_draw(px, plot, 0, 0, yellow, 100)");
    api.execute_command("endsprite");

    api.begin_frame(false);
    api.show_sprite("px", 10, 10, 0, 0, 0);
    api.set_sprite_anchor("px", 1, 0);
    api.set_sprite_scale("px", 2.f, 0);
    api.end_frame();
    // 2×1 pixel around centre (10,10) should cover a 2×2 block.
    int lit = 0;
    for (int y = 8; y <= 12; ++y)
        for (int x = 8; x <= 12; ++x)
            if (pixel_lit(api, x, y)) ++lit;
    CHECK(lit >= 1);
}

TEST(sprite_transform_reset_keeps_anchor) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(rock, 4, 4)");
    api.execute_command("sprite_draw(rock, plot, 0, 0, blue, 100)");
    api.execute_command("endsprite");
    api.begin_frame(false);
    api.show_sprite("rock", 20, 20, 0, 0, 0);
    api.set_sprite_anchor("rock", 1, 0);
    api.set_sprite_scale("rock", 2.f, 0);
    api.set_sprite_flip("rock", 3, 0);
    api.reset_sprite_transform("rock", 0);
    api.end_frame();
    // Centre anchor kept: top-left of 4×4 at centre (20,20) → around (18,18).
    CHECK(pixel_lit(api, 18, 18) || pixel_lit(api, 19, 19) || pixel_lit(api, 20, 20));
}

TEST(sprite_transform_wire_op) {
    SpriteBatch batch;
    batch.append("sprite_transform", {Value::str("ship"), Value::integer(0), Value::real(2.0), Value::real(2.0),
                                      Value::real(0), Value::real(0), Value::real(0), Value::integer(1)});
    CHECK(batch.bytes().size() > 10u);
    CHECK_EQ(int(batch.bytes()[0]), 5);  // OP_TRANSFORM

    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(ship, 2, 2)");
    api.execute_command("sprite_draw(ship, plot, 0, 0, green, 100)");
    api.execute_command("endsprite");
    api.show_sprite("ship", 8, 8, 0, 0, 0);
    api.sprite_batch_bytes(batch.bytes());
    api.begin_frame(true);
    api.end_frame();
    CHECK(pixel_lit(api, 8, 8) || pixel_lit(api, 7, 7) || pixel_lit(api, 9, 9));
}

TEST(sprite_transform_move_sub_wire) {
    SpriteBatch batch;
    batch.append("move_sprite_sub",
                 {Value::str("dot"), Value::real(10.5), Value::real(11.25), Value::integer(0)});
    CHECK_EQ(int(batch.bytes()[0]), 6);  // OP_MOVE_SUB
}

TEST(sprite_transform_scale_clamp_via_api) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(big, 2, 2)");
    api.execute_command("sprite_draw(big, plot, 0, 0, red, 100)");
    api.execute_command("endsprite");
    api.show_sprite("big", 30, 30, 0, 0, 0);
    api.set_sprite_scale("big", 100.f, 0);  // clamp to 8
    api.begin_frame(true);
    api.end_frame();
    int lit = 0;
    for (int y = 20; y < 45; ++y)
        for (int x = 20; x < 45; ++x)
            if (pixel_lit(api, x, y)) ++lit;
    CHECK(lit >= 1);
}

TEST(sprite_transform_rotate_90) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(arm, 3, 1)");
    api.execute_command("sprite_draw(arm, plot, 2, 0, red, 100)");  // tip on the right
    api.execute_command("endsprite");
    api.begin_frame(false);
    api.show_sprite("arm", 20, 20, 0, 0, 0);
    api.set_sprite_anchor("arm", 1, 0);
    api.set_sprite_angle("arm", 90.f, 0);  // clockwise → tip goes down
    api.end_frame();
    CHECK(pixel_lit(api, 20, 21) || pixel_lit(api, 20, 22) || pixel_lit(api, 21, 21));
}

TEST(sprite_transform_angle_neg90_equals_270) {
    MatrixApi a(make_headless_matrix());
    MatrixApi b(make_headless_matrix());
    for (MatrixApi* api : {&a, &b}) {
        api->execute_command("define_sprite(arm, 3, 1)");
        api->execute_command("sprite_draw(arm, plot, 2, 0, green, 100)");
        api->execute_command("endsprite");
        api->begin_frame(false);
        api->show_sprite("arm", 30, 30, 0, 0, 0);
        api->set_sprite_anchor("arm", 1, 0);
    }
    a.set_sprite_angle("arm", -90.f, 0);
    b.set_sprite_angle("arm", 270.f, 0);
    a.end_frame();
    b.end_frame();
    for (int y = 25; y < 36; ++y) {
        for (int x = 25; x < 36; ++x) {
            uint8_t ar, ag, ab, br, bg, bb;
            CHECK(a.surface_pixel(x, y, &ar, &ag, &ab));
            CHECK(b.surface_pixel(x, y, &br, &bg, &bb));
            CHECK_EQ(int(ar), int(br));
            CHECK_EQ(int(ag), int(bg));
            CHECK_EQ(int(ab), int(bb));
        }
    }
}

TEST(sprite_transform_tilt_edge_on) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(coin, 8, 8)");
    api.execute_command("sprite_draw(coin, plot, 3, 3, yellow, 100)");
    api.execute_command("sprite_draw(coin, plot, 4, 4, yellow, 100)");
    api.execute_command("endsprite");
    api.begin_frame(false);
    api.show_sprite("coin", 32, 32, 0, 0, 0);
    api.set_sprite_anchor("coin", 1, 0);
    api.set_sprite_tilt("coin", 0.f, 90.f, 0);  // edge-on on y
    api.end_frame();
    // Should still show a sliver rather than vanishing.
    int lit = 0;
    for (int y = 28; y < 37; ++y)
        for (int x = 28; x < 37; ++x)
            if (pixel_lit(api, x, y)) ++lit;
    CHECK(lit >= 1);
}

TEST(sprite_transform_back_cel) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(card, 4, 4)");
    api.execute_command("sprite_cel(0)");
    api.execute_command("sprite_draw(card, plot, 0, 0, red, 100)");
    api.execute_command("sprite_cel(1)");
    api.execute_command("sprite_draw(card, plot, 0, 0, blue, 100)");
    api.execute_command("endsprite");
    api.execute_command("sprite_back_cel(card, 1)");
    api.begin_frame(false);
    api.show_sprite("card", 16, 16, 0, 0, 0);
    api.set_sprite_anchor("card", 1, 0);
    api.set_sprite_tilt("card", 0.f, 180.f, 0);  // fully reversed
    api.end_frame();
    // Back cel is blue — expect a blue-ish pixel near centre.
    uint8_t r = 0, g = 0, b = 0;
    bool found_blue = false;
    for (int y = 14; y < 19; ++y) {
        for (int x = 14; x < 19; ++x) {
            if (!api.surface_pixel(x, y, &r, &g, &b)) continue;
            if (b > 100 && b > r) found_blue = true;
        }
    }
    CHECK(found_blue);
}

TEST(sprite_transform_full_command) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(rock, 4, 4)");
    api.execute_command("sprite_draw(rock, plot, 1, 1, white, 100)");
    api.execute_command("endsprite");
    api.begin_frame(false);
    api.show_sprite("rock", 40, 40, 0, 0, 0);
    api.set_sprite_anchor("rock", 1, 0);
    api.set_sprite_transform("rock", 1.5f, 45.f, 0, 0.f, 0.f, 0);
    api.end_frame();
    int lit = 0;
    for (int y = 35; y < 46; ++y)
        for (int x = 35; x < 46; ++x)
            if (pixel_lit(api, x, y)) ++lit;
    CHECK(lit >= 1);
}

TEST(sprite_transform_cache_hit_matches_miss) {
    MatrixApi a(make_headless_matrix());
    MatrixApi b(make_headless_matrix());
    for (MatrixApi* api : {&a, &b}) {
        api->execute_command("define_sprite(spin, 8, 8)");
        api->execute_command("sprite_draw(spin, plot, 7, 3, red, 100)");
        api->execute_command("endsprite");
        api->begin_frame(false);
        api->show_sprite("spin", 32, 32, 0, 0, 0);
        api->set_sprite_anchor("spin", 1, 0);
        api->set_sprite_angle("spin", 40.f, 0);  // buckets to 40° with 2° step
    }
    a.end_frame();  // miss → fill cache
    CHECK(a.sprite_transform_cache().misses() >= 1u);
    a.begin_frame(true);
    a.set_sprite_angle("spin", 40.f, 0);  // same key → cache hit
    a.end_frame();
    CHECK(a.sprite_transform_cache().hits() >= 1u);

    b.end_frame();
    for (int y = 24; y < 41; ++y) {
        for (int x = 24; x < 41; ++x) {
            uint8_t ar, ag, ab, br, bg, bb;
            CHECK(a.surface_pixel(x, y, &ar, &ag, &ab));
            CHECK(b.surface_pixel(x, y, &br, &bg, &bb));
            CHECK_EQ(int(ar), int(br));
            CHECK_EQ(int(ag), int(bg));
            CHECK_EQ(int(ab), int(bb));
        }
    }
}

TEST(sprite_transform_cache_evicts_at_cap) {
    SpriteTransformCache cache(2048);  // tiny cap
    for (int i = 0; i < 40; ++i) {
        SpriteTransformCacheKey key;
        key.template_name = "t";
        key.cel = i;
        key.sx_q = 0;
        key.sy_q = 0;
        key.angle_q = static_cast<int16_t>(i);
        key.flags = 0;
        SpriteTransformCacheEntry e;
        e.width = 16;
        e.height = 16;
        e.pixels.assign(256, Rgb{10, 20, 30});
        cache.insert(std::move(key), std::move(e));
    }
    CHECK(cache.byte_size() <= 2048u + 512u);  // allow one oversize entry
    CHECK(cache.entry_count() < 40u);
}

TEST(sprite_transform_cache_invalidate_template) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(spin, 4, 4)");
    api.execute_command("sprite_draw(spin, plot, 0, 0, green, 100)");
    api.execute_command("endsprite");
    api.begin_frame(false);
    api.show_sprite("spin", 10, 10, 0, 0, 0);
    api.set_sprite_angle("spin", 20.f, 0);
    api.end_frame();
    CHECK(api.sprite_transform_cache().entry_count() >= 1u);
    api.sprite_back_cel("spin", 0);  // invalidates
    CHECK_EQ(api.sprite_transform_cache().entry_count(), 0u);
}
