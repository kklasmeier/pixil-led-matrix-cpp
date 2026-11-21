#include "check.h"

#include <cmath>

#include "runtime/machine.h"
#include "syntax/script.h"

using namespace pixil;

namespace {

struct Ran {
    SymbolTable symbols;
    Script script;
    Machine machine;
    explicit Ran(const char* source) : script(parse_script(source)), machine(symbols) {
        resolve_script(script, symbols);
        machine.run(script, {});
    }
    Value var(const char* name) const {
        int slot = symbols.find(name);
        CHECK(slot >= 0);
        return machine.env.slots[static_cast<size_t>(slot)];
    }
};

}  // namespace

TEST(script_for_is_inclusive) {
    Ran r("v_n = 0\nfor v_i in (0, 2, 1)\n v_n = v_n + 1\nendfor v_i\n");
    CHECK(r.var("v_n").is_int());
    CHECK_EQ(r.var("v_n").as_int(), 3);
}

TEST(script_if_else) {
    Ran r("v_x = 1\nif v_x > 2 then\n v_x = 9\nelse\n v_x = 4\nendif\n");
    CHECK_EQ(r.var("v_x").as_int(), 4);
}

TEST(script_array_and_procedure) {
    Ran r("create_array(v_a, 3)\n"
          "v_a[1] = 4\n"
          "def bump {\n"
          " v_a[1] = v_a[1] + 1\n"
          "}\n"
          "call bump\n"
          "v_x = v_a[1]\n");
    CHECK(r.var("v_x").is_float());
    CHECK_EQ(r.var("v_x").as_float(), 5.0);
}

TEST(script_frame_batches_draws) {
    std::vector<std::string> cmds;
    SymbolTable symbols;
    Script script = parse_script(
        "fps(60)\nbegin_frame\ndraw_circle(1, 2, 3, cyan, 100, false)\nend_frame\nrest(0.01)\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    RunOptions opt;
    opt.emit = [&](std::string_view c) { cmds.emplace_back(c); };
    machine.run(script, opt);
    CHECK_EQ(cmds.size(), 5u);
    CHECK_EQ(cmds[0], "fps(60.0)");
    CHECK_EQ(cmds[1], "begin_frame(false)");
    CHECK(cmds[2].rfind("draw_batch(\"", 0) == 0);
    CHECK_EQ(cmds[3], "end_frame");
    CHECK_EQ(cmds[4], "rest(0.01)");
}

TEST(script_for_then_and_bare_clear) {
    Ran r("v_n = 0\nfor v_i in (0, 1, 1) then\n v_n = v_n + 1\nendfor v_i\n");
    CHECK(r.var("v_n").is_int());
    CHECK_EQ(r.var("v_n").as_int(), 2);

    std::vector<std::string> cmds;
    SymbolTable symbols;
    Script script = parse_script("clear\nplot(32.4, 31.6, 120, 150.2, 273)\nrest(0)\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    RunOptions opt;
    opt.emit = [&](std::string_view c) { cmds.emplace_back(c); };
    machine.run(script, opt);
    CHECK_EQ(cmds.size(), 3u);
    CHECK_EQ(cmds[0], "clear");
    CHECK_EQ(cmds[1], "plot(32, 32, 99, 100, 273)");
    CHECK_EQ(cmds[2], "rest(0)");

    cmds.clear();
    SymbolTable symbols2;
    Script script2 = parse_script("v_t = 0\nrest(v_t)\n");
    resolve_script(script2, symbols2);
    Machine machine2(symbols2);
    opt.emit = [&](std::string_view c) { cmds.emplace_back(c); };
    machine2.run(script2, opt);
    CHECK_EQ(cmds.size(), 1u);
    CHECK_EQ(cmds[0], "rest(0.0)");
}

TEST(script_draw_text_and_bare_procedure) {
    std::vector<std::string> cmds;
    SymbolTable symbols;
    Script script = parse_script(
        "v_n = 7.0\n"
        "def border {\n"
        " clear_text(1.6, 2)\n"
        "}\n"
        "def show {\n"
        " border\n"
        " draw_text(9.0, -2.6, v_n, piboto-regular, 8, green, 82.5)\n"
        " draw_text(32, 8, \"PONG\", tiny64_font, 14, white, 100, NORMAL, CENTER)\n"
        "}\n"
        "call show\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    RunOptions opt;
    opt.emit = [&](std::string_view c) { cmds.emplace_back(c); };
    machine.run(script, opt);
    CHECK_EQ(cmds.size(), 3u);
    CHECK_EQ(cmds[0], "clear_text(2, 2)");
    CHECK_EQ(cmds[1], "draw_text(9, -3, \"7\", \"piboto-regular\", 8, green, 82)");
    CHECK_EQ(cmds[2], "draw_text(32, 8, \"PONG\", \"tiny64_font\", 14, white, 100, NORMAL, CENTER)");
}

TEST(script_if_condition_evaluated_once) {
    // Seed 1, after 8 draws: random(0, 100, 0) yields 9. One roll, so the
    // else branch runs (D-008: legacy's interpreter path rolled up to 3 times).
    SymbolTable symbols;
    Script script = parse_script(
        "for v_k in (1, 8, 1)\n"
        " v_i = random(0, 100, 0)\n"
        "endfor v_k\n"
        "v_flag = 0\n"
        "if random(0, 100, 0) > 70 then\n"
        " v_flag = 1\n"
        "else\n"
        " v_flag = 2\n"
        "endif\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    machine.env.rng.seed(1);
    machine.run(script, {});
    int slot = symbols.find("v_flag");
    CHECK(slot >= 0);
    CHECK_EQ(machine.env.slots[static_cast<size_t>(slot)].as_int(), 2);
}

TEST(script_sprite_define_show_and_batch) {
    std::vector<std::string> cmds;
    SymbolTable symbols;
    Script script = parse_script(
        "v_ball_size = 2\n"
        "v_paddle_height = 12\n"
        "define_sprite(ball, v_ball_size, v_ball_size)\n"
        " draw_rectangle(0, 0, 1, 1, white, 100, true)\n"
        "endsprite\n"
        "define_sprite(left_paddle, 2, v_paddle_height)\n"
        " draw_rectangle(0, 0, 1, v_paddle_height - 1, white, 100, true)\n"
        "endsprite\n"
        "for v_i in (0, 1, 1)\n"
        " draw_rectangle(31, v_i * 8, 2, 4, white, 100, true)\n"
        "endfor v_i\n"
        "show_sprite(ball, 32.2, 31.6)\n"
        "begin_frame(true)\n"
        " move_sprite(left_paddle, 2, 26)\n"
        " move_sprite(right_paddle, 60, 27)\n"
        " move_sprite(ball, 33, 32)\n"
        "end_frame\n"
        "hide_sprite(ball)\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    RunOptions opt;
    opt.emit = [&](std::string_view c) { cmds.emplace_back(c); };
    machine.run(script, opt);
    CHECK_EQ(cmds.size(), 13u);
    CHECK_EQ(cmds[0], "define_sprite(ball, 2, 2)");
    CHECK_EQ(cmds[1], "sprite_draw(ball, draw_rectangle, 0, 0, 1, 1, \"white\", 100, \"true\")");
    CHECK_EQ(cmds[2], "endsprite");
    CHECK_EQ(cmds[3], "define_sprite(left_paddle, 2, 12)");
    CHECK_EQ(cmds[4], "sprite_draw(left_paddle, draw_rectangle, 0, 0, 1, 11, \"white\", 100, \"true\")");
    CHECK_EQ(cmds[5], "endsprite");
    CHECK_EQ(cmds[6], "draw_rectangle(31, 0, 2, 4, white, 100, true)");
    CHECK_EQ(cmds[7], "draw_rectangle(31, 8, 2, 4, white, 100, true)");
    CHECK_EQ(cmds[8], "show_sprite(ball, 32, 32)");
    CHECK_EQ(cmds[9], "begin_frame(true)");
    CHECK_EQ(cmds[10], "sprite_batch(\"AgtsZWZ0X3BhZGRsZQIAGgAA/wIMcmlnaHRfcGFkZGxlPAAbAAD/AgRiYWxsIQAgAAD/\")");
    CHECK_EQ(cmds[11], "end_frame");
    CHECK_EQ(cmds[12], "hide_sprite(ball)");
}

TEST(script_sprite_cel_and_print_are_not_commands) {
    std::vector<std::string> cmds;
    SymbolTable symbols;
    Script script = parse_script(
        "v_n = 3\n"
        "print(\"Rendering ball cels...\")\n"
        "print(f\"cel {v_n}\")\n"
        "define_sprite(ball, 4, 4)\n"
        " sprite_cel(0)\n"
        " plot(1, 1, red, 80)\n"
        " sprite_cel()\n"
        " plot(2, 2, white, 40)\n"
        "endsprite\n"
        "dispose_sprite(ball, 1)\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    RunOptions opt;
    opt.emit = [&](std::string_view c) { cmds.emplace_back(c); };
    machine.run(script, opt);
    CHECK_EQ(cmds.size(), 7u);
    CHECK_EQ(cmds[0], "define_sprite(ball, 4, 4)");
    CHECK_EQ(cmds[1], "sprite_cel(0)");
    CHECK_EQ(cmds[2], "sprite_draw(ball, plot, 1, 1, \"red\", 80)");
    CHECK_EQ(cmds[3], "sprite_cel()");
    CHECK_EQ(cmds[4], "sprite_draw(ball, plot, 2, 2, \"white\", 40)");
    CHECK_EQ(cmds[5], "endsprite");
    CHECK_EQ(cmds[6], "dispose_sprite(ball, 1)");
}

TEST(script_elsif_and_junk_after_brace) {
    Ran r("v_x = 1\n"
          "if v_x == 0 then\n"
          " v_x = 2\n"
          "elsif v_x == 1 then\n"
          " v_x = 3\n"
          "endif\n");
    CHECK_EQ(r.var("v_x").as_int(), 3);

    std::vector<std::string> cmds;
    SymbolTable symbols;
    Script script = parse_script("def show_over {\n clear\n}1111\ndef show_victory {\n sync_queue\n}\ncall show_victory\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    RunOptions opt;
    opt.emit = [&](std::string_view c) { cmds.emplace_back(c); };
    machine.run(script, opt);
    CHECK_EQ(cmds.size(), 1u);
    CHECK_EQ(cmds[0], "sync_queue");
}

TEST(script_import_particle_verlet_keeps_caller_locals) {
    std::vector<std::string> cmds;
    SymbolTable symbols;
    Script script = parse_script(
        "create_array(v_px, 1, numeric)\n"
        "create_array(v_py, 1, numeric)\n"
        "create_array(v_ox, 1, numeric)\n"
        "create_array(v_oy, 1, numeric)\n"
        "create_array(v_active, 1, numeric)\n"
        "v_px[0] = 2\n"
        "v_py[0] = 3\n"
        "v_ox[0] = 2\n"
        "v_oy[0] = 3\n"
        "v_active[0] = 1\n"
        "v_i = 7\n"
        "import particle\n"
        "call particle.verlet(v_px, v_py, v_ox, v_oy, v_active, 0, 0.4, 1, 1)\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    RunOptions opt;
    opt.lib_dir = "scripts/lib";
    opt.emit = [&](std::string_view c) { cmds.emplace_back(c); };
    machine.run(script, opt);
    CHECK(cmds.empty());
    CHECK_EQ(machine.env.slots[static_cast<size_t>(symbols.find("v_i"))].as_int(), 7);
    const Value& py = machine.env.slots[static_cast<size_t>(symbols.find("v_py"))];
    CHECK(py.is_array());
    double y = py.as_array()->data[0].as_float();
    CHECK(y > 3.39 && y < 3.41);
}

TEST(script_particle_integrate_and_bounds) {
    SymbolTable symbols;
    Script script = parse_script(
        "create_array(v_x, 1, numeric)\n"
        "create_array(v_y, 1, numeric)\n"
        "create_array(v_vx, 1, numeric)\n"
        "create_array(v_vy, 1, numeric)\n"
        "create_array(v_active, 1, numeric)\n"
        "v_x[0] = 0\n"
        "v_y[0] = 0\n"
        "v_vx[0] = 1\n"
        "v_vy[0] = 0\n"
        "v_active[0] = 1\n"
        "import particle\n"
        "call particle.integrate(v_x, v_y, v_vx, v_vy, v_active, 1, 0, 1, 0, 1, \"post_move\", \"none\", 1)\n"
        "call particle.collide_bounds(v_x, v_y, v_vx, v_vy, v_active, 0, 1, 0, 10, 0.5, 0, \"none\", 1)\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    RunOptions opt;
    opt.lib_dir = "scripts/lib";
    opt.emit = [](std::string_view) {};
    machine.run(script, opt);
    auto cell = [&](const char* name) {
        return machine.env.slots[static_cast<size_t>(symbols.find(name))].as_array()->data[0].as_float();
    };
    CHECK_EQ(cell("v_x"), 1.0);
    CHECK_EQ(cell("v_vx"), -1.0);
}

TEST(script_import_path_prefix_and_library_body) {
    CHECK_THROWS(parse_script("import ../ai\n"));
    CHECK_THROWS(parse_script("import arcade/ai.pix\n"));

    std::vector<std::string> cmds;
    SymbolTable symbols;
    Script script = parse_script("import arcade/ai\ncall ai.ping\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    RunOptions opt;
    opt.lib_dir = "tests/fixtures/lib";
    opt.emit = [&](std::string_view c) { cmds.emplace_back(c); };
    machine.run(script, opt);
    CHECK_EQ(cmds.size(), 1u);
    CHECK_EQ(cmds[0], "clear");

    SymbolTable symbols2;
    Script again = parse_script("import arcade/ai\nimport other/ai\n");
    resolve_script(again, symbols2);
    Machine machine2(symbols2);
    opt.emit = [](std::string_view) {};
    CHECK_THROWS(machine2.run(again, opt));

    SymbolTable symbols3;
    Script bad = parse_script("import commands\n");
    resolve_script(bad, symbols3);
    Machine machine3(symbols3);
    CHECK_THROWS(machine3.run(bad, opt));
}

TEST(script_points_lerp_and_project) {
    SymbolTable symbols;
    Script script = parse_script(
        "create_array(v_a, 1, numeric)\n"
        "create_array(v_b, 1, numeric)\n"
        "create_array(v_o, 1, numeric)\n"
        "v_a[0] = 0\n"
        "v_b[0] = 10\n"
        "v_i = 7\n"
        "import points\n"
        "call points.lerp(v_a, v_b, v_o, 0.25, 1)\n"
        "create_array(v_x, 1, numeric)\n"
        "create_array(v_y, 1, numeric)\n"
        "create_array(v_z, 1, numeric)\n"
        "create_array(v_sx, 1, numeric)\n"
        "create_array(v_sy, 1, numeric)\n"
        "create_array(v_depth, 1, numeric)\n"
        "v_x[0] = 1\n"
        "v_y[0] = 0\n"
        "v_z[0] = 0\n"
        "call points.project3d(v_x, v_y, v_z, v_sx, v_sy, 0, pi / 2, 0, 1, 10, 32, 32, v_depth, 1)\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    RunOptions opt;
    opt.lib_dir = "scripts/lib";
    opt.emit = [](std::string_view) {};
    machine.run(script, opt);
    CHECK_EQ(machine.env.slots[static_cast<size_t>(symbols.find("v_i"))].as_int(), 7);
    const Value& out = machine.env.slots[static_cast<size_t>(symbols.find("v_o"))];
    CHECK(out.is_array());
    CHECK(out.as_array()->data[0].as_float() > 2.49 && out.as_array()->data[0].as_float() < 2.51);
    const Value& depth = machine.env.slots[static_cast<size_t>(symbols.find("v_depth"))];
    const Value& sx = machine.env.slots[static_cast<size_t>(symbols.find("v_sx"))];
    CHECK(depth.as_array()->data[0].as_float() < -0.99 && depth.as_array()->data[0].as_float() > -1.01);
    CHECK(sx.as_array()->data[0].as_float() > 31.99 && sx.as_array()->data[0].as_float() < 32.01);
}

TEST(script_grid_neighbors_and_random_field) {
    seed_numpy_random(1);
    SymbolTable symbols;
    Script script = parse_script(
        "create_array(v_g, 4, numeric)\n"
        "v_g[0] = 1\n"
        "grid_program life {\n"
        "    size 2\n"
        "    cell 1\n"
        "    fields v_g\n"
        "    boundary clamp\n"
        "    step v_g {\n"
        "        v_g_next = neighbors4(v_g)\n"
        "    }\n"
        "    draw {\n"
        "        fill_field v_g\n"
        "        fill_color white\n"
        "        fill_intensity 50\n"
        "    }\n"
        "}\n"
        "begin_frame\n"
        "grid_step(life)\n"
        "end_frame\n"
        "create_array(v_h, 4, numeric)\n"
        "grid_program noise {\n"
        "    size 2\n"
        "    fields v_h\n"
        "    step v_h {\n"
        "        v_h_next = random_field(0, 4)\n"
        "    }\n"
        "    draw {\n"
        "        fill_field v_h\n"
        "    }\n"
        "}\n"
        "grid_step(noise)\n");
    resolve_script(script, symbols);
    Machine machine(symbols);
    std::vector<std::string> cmds;
    RunOptions opt;
    opt.emit = [&](std::string_view c) { cmds.emplace_back(c); };
    machine.run(script, opt);
    const auto& g = machine.env.slots[static_cast<size_t>(symbols.find("v_g"))].as_array()->data;
    CHECK(g[0].as_float() > 1.99 && g[0].as_float() < 2.01);
    CHECK(g[1].as_float() > 0.99 && g[1].as_float() < 1.01);
    CHECK(g[2].as_float() > 0.99 && g[2].as_float() < 1.01);
    CHECK(g[3].as_float() == 0.0);
    const auto& h = machine.env.slots[static_cast<size_t>(symbols.find("v_h"))].as_array()->data;
    const double expect[4] = {1.668088018810296, 2.8812979737686324, 0.00045749926937954655, 1.2093302905273591};
    for (int i = 0; i < 4; ++i) CHECK(std::fabs(h[static_cast<size_t>(i)].as_float() - expect[i]) < 1e-12);
    bool batched = false;
    for (const auto& cmd : cmds)
        if (cmd.rfind("draw_batch(", 0) == 0) batched = true;
    CHECK(batched);
}

TEST(script_get_datetime_and_str) {
    Ran r("v_h = get_datetime(\"H\")\n"
          "v_ampm = get_datetime(\"a\")\n"
          "v_clock = get_datetime(\"h:mi:s\")\n"
          "v_weekend = get_datetime(\"isweekend\")\n"
          "v_text = str(15)\n");
    CHECK(r.var("v_h").is_int());
    CHECK(r.var("v_h").as_int() >= 0 && r.var("v_h").as_int() <= 23);
    CHECK(r.var("v_ampm").is_str());
    std::string ampm = r.var("v_ampm").as_str();
    CHECK(ampm == "AM" || ampm == "PM");
    CHECK(r.var("v_clock").is_str());
    CHECK(r.var("v_clock").as_str().size() >= 5);
    CHECK(r.var("v_weekend").is_int());
    int weekend = static_cast<int>(r.var("v_weekend").as_int());
    CHECK(weekend == 0 || weekend == 1);
    CHECK(r.var("v_text").is_str());
    std::string text = r.var("v_text").as_str();
    CHECK_EQ(text, "15");
}

double cell(const Machine& machine, const SymbolTable& symbols, const char* name, size_t i) {
    return machine.env.slots[static_cast<size_t>(symbols.find(name))].as_array()->data[i].as_float();
}

TEST(script_agent_chemotaxis) {
    auto run = [](const char* source, const auto& check) {
        SymbolTable symbols;
        Script script = parse_script(source);
        resolve_script(script, symbols);
        Machine machine(symbols);
        RunOptions opt;
        opt.lib_dir = "scripts/lib";
        opt.emit = [](std::string_view) {};
        machine.run(script, opt);
        check(machine, symbols);
    };
    run(
            "import agent\n"
            "create_array(v_x, 2, numeric)\n"
            "create_array(v_y, 2, numeric)\n"
            "create_array(v_h, 2, numeric)\n"
            "create_array(v_f, 16, numeric)\n"
            "v_x[0] = 0.2\n"
            "v_y[0] = 0.2\n"
            "v_h[0] = 0\n"
            "v_x[1] = 3.8\n"
            "v_y[1] = 1.2\n"
            "v_h[1] = 2\n"
            "v_f[1] = 1\n"
            "v_f[4] = 0.4\n"
            "call agent.chemotaxis(v_x, v_y, v_h, v_f, 4, 1, 1, 0.5, 2, 1)\n",
        [](const Machine& machine, const SymbolTable& symbols) {
        CHECK(std::fabs(cell(machine, symbols, "v_x", 0) - 1.2) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_y", 0) - 0.2) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_h", 0) - 0.0) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_x", 1) - 3.8) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_y", 1) - 2.2) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_h", 1) - 2.0) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_f", 1) - 1.5) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_f", 4) - 0.4) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_f", 11) - 0.5) < 1e-9);
    });
    run(
            "import agent\n"
            "create_array(v_x, 1, numeric)\n"
            "create_array(v_y, 1, numeric)\n"
            "create_array(v_h, 1, numeric)\n"
            "create_array(v_f, 16, numeric)\n"
            "v_x[0] = 0.2\n"
            "v_y[0] = 3.9\n"
            "v_h[0] = 6\n"
            "v_f[0] = 2\n"
            "call agent.chemotaxis(v_x, v_y, v_h, v_f, 4, 1.5, 1, 0.4, 1, 0)\n",
        [](const Machine& machine, const SymbolTable& symbols) {
        CHECK(std::fabs(cell(machine, symbols, "v_x", 0) - 0.2) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_y", 0) - 2.9) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_h", 0) - 6.0) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_f", 0) - 1.5) < 1e-9);
        CHECK(std::fabs(cell(machine, symbols, "v_f", 8) - 0.4) < 1e-9);
    });
}

TEST(script_numpy_integers_match_generator) {
    seed_numpy_random(1);
    Ran r("v_seeded = numpy_reseed()\n"
          "create_array(v_a, 16, numeric)\n"
          "for v_i in (0, 15, 1)\n"
          " v_a[v_i] = numpy_integers(0, 2)\n"
          "endfor v_i\n"
          "v_seeded = numpy_reseed()\n"
          "v_b = numpy_integers(0, 2)\n");
    const int expect[] = {1, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0};
    for (int i = 0; i < 16; ++i) {
        CHECK(r.var("v_a").as_array()->data[static_cast<size_t>(i)].as_float() == expect[i]);
    }
    // The second reseed is default_rng number 2, whose first draw is 1.
    CHECK(r.var("v_b").integral() == 1);
}
