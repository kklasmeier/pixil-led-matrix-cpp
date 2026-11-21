#include "check.h"
#include "consumer/api.h"
#include "consumer/matrix.h"
#include "emit/draw_batch.h"
#include "emit/sprite_batch.h"
#include "runtime/value.h"

using namespace pixil;
using namespace pixil::consumer;

TEST(layers_set_clear_fingerprint) {
    MatrixApi api(make_headless_matrix());
    api.begin_frame(false);
    api.set_layer(10);
    api.plot(5, 5, ColorArg{true, "red", 0}, 100);
    api.set_layer(20);
    api.plot(6, 6, ColorArg{true, "blue", 0}, 100);
    api.end_frame();
    std::string both = api.fingerprint();
    CHECK(both != "empty");

    api.begin_frame(true);
    api.clear_layer(10);
    api.end_frame();
    std::string after = api.fingerprint();
    CHECK(after != "empty");
    CHECK(after != both);
}

TEST(layers_sprite_between_bands) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(dot, 1, 1)");
    api.execute_command("sprite_draw(dot, plot, 0, 0, yellow, 100)");
    api.execute_command("endsprite");

    api.begin_frame(false);
    api.set_layer(10);
    api.plot(10, 10, ColorArg{true, "red", 0}, 100);
    api.show_sprite("dot", 10, 10, 0, 0, 0);  // layer 10
    api.set_layer(20);
    api.plot(10, 10, ColorArg{true, "blue", 0}, 100);  // covers sprite where opaque
    api.end_frame();
    CHECK(api.fingerprint() != "empty");
}

TEST(layers_draw_batch_ops) {
    DrawBatch batch;
    CHECK(batch.append("set_layer", {Value::integer(30)}));
    CHECK(batch.append("clear_layer", {Value::integer(30)}));
    CHECK_EQ(batch.record_count(), 2u);
    CHECK_EQ(batch.bytes().size(), 6u);
    CHECK_EQ(int(batch.bytes()[0]), int(OP_SET_LAYER));
    CHECK_EQ(int(batch.bytes()[3]), int(OP_CLEAR_LAYER));
}

TEST(layers_show_sprite_packs_z) {
    SpriteBatch sprites;
    sprites.append("show_sprite", {Value::str("ball"), Value::integer(1), Value::integer(2),
                                   Value::integer(0), Value::integer(7), Value::integer(0)});
    // op + namelen + "ball" + x + y + instance + cel + z = 1+1+4+2+2+1+1+2 = 14
    CHECK_EQ(sprites.bytes().size(), 14u);
    CHECK_EQ(int(sprites.bytes()[0]), 1);  // OP_SHOW
    // z at end: little-endian 7
    CHECK_EQ(int(sprites.bytes()[12]), 7);
    CHECK_EQ(int(sprites.bytes()[13]), 0);
}

TEST(layers_burnout_scoped) {
    MatrixApi api(make_headless_matrix());
    api.set_layer(10);
    api.execute_command("plot(4, 4, red, 100, 80, instant)");
    api.set_layer(20);
    api.execute_command("plot(4, 4, blue, 100, 80, instant)");
    CHECK(api.fingerprint() != "empty");
    api.clear_layer(10);
    // Layer 20 burnout still alive until rest expires it.
    CHECK(api.fingerprint() != "empty");
    api.rest(0.15);
    CHECK_EQ(api.fingerprint(), std::string("empty"));
}

TEST(layers_hide_show) {
    MatrixApi api(make_headless_matrix());
    api.begin_frame(false);
    api.set_layer(10);
    api.plot(8, 8, ColorArg{true, "green", 0}, 100);
    api.end_frame();
    std::string visible = api.fingerprint();
    CHECK(visible != "empty");

    api.hide_layer(10);
    api.begin_frame(true);
    api.end_frame();
    // Hidden layer pixels are not in the composite fingerprint.
    CHECK_EQ(api.fingerprint(), std::string("empty"));

    api.show_layer(10);
    api.begin_frame(true);
    api.end_frame();
    CHECK_EQ(api.fingerprint(), visible);
}

TEST(layers_delete_hides_sprites) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(dot, 1, 1)");
    api.execute_command("sprite_draw(dot, plot, 0, 0, yellow, 100)");
    api.execute_command("endsprite");
    api.set_layer(15);
    api.show_sprite("dot", 4, 4, 0, 0, 0);
    api.delete_layer(15);
    CHECK_EQ(api.current_layer(), 0u);  // deleting current layer → back to 0
    // Recreating the layer does not unhide the sprite.
    api.set_layer(15);
    CHECK_EQ(api.current_layer(), 15u);
    api.begin_frame(true);
    CHECK_EQ(api.current_layer(), 0u);  // begin_frame resets current layer
    api.end_frame();
}

TEST(layers_reorder_overwrite) {
    MatrixApi api(make_headless_matrix());
    api.begin_frame(false);
    api.set_layer(1);
    api.plot(1, 1, ColorArg{true, "red", 0}, 100);
    api.set_layer(10);
    api.plot(2, 2, ColorArg{true, "blue", 0}, 100);
    api.end_frame();
    std::string both = api.fingerprint();

    api.reorder_layer(1, 10);  // overwrites old 10; forest(1) becomes 10
    api.begin_frame(true);
    api.end_frame();
    std::string after = api.fingerprint();
    CHECK(after != "empty");
    CHECK(after != both);

    api.reorder_layer(99, 10);  // missing from → no-op
    api.begin_frame(true);
    api.end_frame();
    CHECK_EQ(api.fingerprint(), after);
}

TEST(layers_reorder_moves_burnout) {
    MatrixApi api(make_headless_matrix());
    api.set_layer(5);
    api.execute_command("plot(9, 9, red, 100, 200, instant)");
    api.reorder_layer(5, 50);
    api.clear_layer(5);  // old slot gone / empty — burnout should have moved
    CHECK(api.fingerprint() != "empty");
    api.clear_layer(50);
    CHECK_EQ(api.fingerprint(), std::string("empty"));
}

TEST(layers_set_sprite_layer_and_z) {
    MatrixApi api(make_headless_matrix());
    api.execute_command("define_sprite(dot, 1, 1)");
    api.execute_command("sprite_draw(dot, plot, 0, 0, cyan, 100)");
    api.execute_command("endsprite");
    api.set_layer(0);
    api.show_sprite("dot", 3, 3, 0, 0, 0);
    api.set_sprite_layer("dot", 40, 0);  // auto-creates layer 40
    api.set_sprite_z("dot", 9, 0);
    api.begin_frame(true);
    api.set_layer(40);
    api.plot(3, 3, ColorArg{true, "magenta", 0}, 100);
    api.end_frame();
    CHECK(api.fingerprint() != "empty");
}
