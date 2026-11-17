// Executes a resolved script, emitting consumer commands (docs/COMMAND_DIALECT.md).
#pragma once

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
    Value eval_arg(const Expr& expr);
    bool should_stop() const;

    RunOptions opt_;
    DrawBatch batch_;
    SpriteBatch sprites_;
    std::vector<std::string> frame_extra_;
    std::unordered_map<std::string, const Stmt*> procs_;
    std::unordered_set<std::string> loaded_;
    std::unordered_map<std::string, std::string> prefixes_;
    std::vector<std::unique_ptr<Script>> modules_;
    bool in_frame_ = false;
    bool preserve_ = false;
    bool in_sprite_ = false;
    std::string sprite_name_;
    std::unique_ptr<GridState> grids_;
};

}  // namespace pixil
