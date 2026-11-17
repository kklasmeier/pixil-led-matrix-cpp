// Compile-once script AST. One statement list per script; expressions inside it
// are the same trees the expression evaluator already runs.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/eval.h"
#include "syntax/ast.h"

namespace pixil {

enum class StmtKind {
    Assign, ArrayAssign, CreateArray, If, For, While, Break, Command, BeginFrame, EndFrame, Proc, Call, Import,
    GridProgram, FieldProgram
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct IfClause {
    ExprPtr cond;  // null = else
    std::vector<StmtPtr> body;
};

struct Stmt {
    StmtKind kind = StmtKind::Command;
    int line = 0;
    std::string name;           // variable, command, or loop variable
    int slot = -1;              // Assign / For
    ExprPtr expr;               // Assign rhs, While condition
    std::vector<ExprPtr> args;  // Command args, For start/end/step
    bool preserve = false;      // BeginFrame
    bool string_array = false;  // CreateArray
    bool bare = false;          // Command written without parentheses
    std::vector<IfClause> clauses;
    std::vector<StmtPtr> body;  // For / While / Proc
    std::vector<int> params;    // Proc parameter slots, in order
    std::vector<std::string> text;  // grid_program / field_program body
};

struct Script {
    std::vector<StmtPtr> body;
};

Script parse_script(std::string_view source);
Script parse_script_file(const std::string& path);

// Interns v_* names and turns bare command words (colors, modes) into strings.
void resolve_script(Script& script, SymbolTable& symbols);

}  // namespace pixil
