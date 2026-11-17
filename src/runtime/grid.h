// grid_program / field_program. The cell math is a small expression language
// compiled once, then run over the whole grid. Arrays the script owns are
// read at the start of a step and written back at the end.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "emit/draw_batch.h"
#include "runtime/eval.h"

namespace pixil {

struct GridState;

std::unique_ptr<GridState> make_grid_state();

// numpy.random.seed, used only by random_field. Independent of Pixil random().
// Also remembers the script seed and resets the unseeded Generator counter.
void seed_numpy_random(int64_t seed);

// One fresh numpy Generator, the way an unseeded default_rng() is captured:
// the nth call is seeded with [script_seed, n], counting from 1.
void numpy_reseed();
// numpy Generator.integers(lo, hi): half-open, 32-bit Lemire, int64 result.
int64_t numpy_integers(int64_t lo, int64_t hi);

void define_grid_program(GridState& state, const std::string& name, const std::vector<std::string>& lines);
void define_field_program(GridState& state, const std::string& name, const std::vector<std::string>& lines);

void grid_fill_array(PixArray& array, double value);
void grid_reset(GridState& state, const std::string& name);
void grid_step(GridState& state, Env& env, const std::string& name, DrawBatch& batch);
void field_render(GridState& state, Env& env, const std::string& name, DrawBatch& batch);

}  // namespace pixil
