// Executes a resolved script, emitting consumer commands (docs/COMMAND_DIALECT.md).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "emit/draw_batch.h"
#include "emit/sprite_batch.h"
#include "runtime/eval.h"
#include "runtime/grid.h"
#include "syntax/script.h"

namespace pixil {

struct RunOptions {
    int max_commands = -1;  // stop after this many emitted commands, -1 = no limit
    int max_frames = -1;    // stop after this many end_frame calls, -1 = no limit
    // When true, honor fps()/rest() on the producer so get_datetime and other
    // wall-clock reads advance with script time (legacy in-process semantics).
    // Dry-run and unit tests leave this false so they stay instant.
    bool pace = false;
    std::function<bool()> stop_requested;
    std::function<void(std::string_view)> emit;
    std::string lib_dir;  // directory searched by `import path` (path.pix)
};

class Machine {
public:
    explicit Machine(SymbolTable& symbols);
    ~Machine();

    Env env;
    int commands = 0;
    int frames = 0;
    bool stopped = false;

    void run(const Script& script, const RunOptions& options);

private:
    void run_list(const std::vector<StmtPtr>& body);
    void run_stmt(const Stmt& stmt);
    void run_command(const Stmt& stmt);
    void call_proc(const Stmt& call, const Stmt& proc);
    void import_module(const Stmt& stmt);
    void emit(std::string cmd);
    void finish_frame();
    void flush_frame_batches();
    void pace_after_frame();
    void pace_rest(double seconds);
    Value eval_arg(const Expr& expr);
    bool should_stop() const;

    RunOptions opt_;
    DrawBatch batch_;
    SpriteBatch sprites_;
    // Ordered mid-frame commands (flushed batches + text/extras). Emitted after
    // begin_frame in finish_frame so producer deferral of begin_frame is preserved.
    std::vector<std::string> frame_stream_;
    std::unordered_map<std::string, const Stmt*> procs_;
    std::unordered_set<std::string> loaded_;
    std::unordered_map<std::string, std::string> prefixes_;
    std::vector<std::unique_ptr<Script>> modules_;
    bool in_frame_ = false;
    bool preserve_ = false;
    bool in_sprite_ = false;
    std::string sprite_name_;
    uint16_t current_layer_ = 0;
    std::unique_ptr<GridState> grids_;
    double target_fps_ = 0;
    std::chrono::steady_clock::time_point next_frame_at_{};
};

}  // namespace pixil
