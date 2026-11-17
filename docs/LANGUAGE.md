# Pixil language — native reference (Phase 1 draft)

Status: **draft (M1)**. The expression layer is implemented and cross-checked
(`src/syntax`, `src/runtime`, `make crosscheck`). The statement grammar below is
derived from `Pixil_Scripting_Guide.txt`, `pixil_utils/regex_patterns.py` and the
`Pixil.py` dispatch order. It must be proven against the corpus (every script in
`scripts/main` parses) before M3 claims the language is done.

Design rule (charter §6.2): parse each script **once** into an AST, resolve names
to slots and calls to builtin ids, then evaluate the tree. No per-line regex
cascade, and no textual substitution of variables.

## 1. Lexical structure

* Line oriented: one statement per line. `#` starts a comment anywhere outside a
  string literal. Leading indentation is insignificant.
* Variables: `v_` followed by word characters (`v_x`, `v_peg_y`). Any other bare
  word is a keyword, a command or builtin name, a named color/mode (`red`,
  `fade`), or a constant (`pi`, `e`, `tau`, `True`/`true`, `False`/`false`, `None`).
* Numbers: Python literals (`10`, `-3`, `2.5`, `.5`, `1e3`). Integers stay
  integers (see DEVIATIONS D-001).
* Strings: `"…"` or `'…'` with `\n \t \r \\ \' \"` escapes.

## 2. Expressions (implemented)

```ebnf
expr        = or_expr ;
or_expr     = and_expr { "or" and_expr } ;
and_expr    = not_expr { "and" not_expr } ;
not_expr    = "not" not_expr | comparison ;
comparison  = concat { comp_op concat } ;            (* chained: a < b < c *)
comp_op     = "==" | "!=" | "<" | "<=" | ">" | ">=" ;
concat      = additive { "&" additive } ;            (* string concatenation *)
additive    = term { ( "+" | "-" ) term } ;
term        = unary { ( "*" | "/" | "//" | "%" ) unary } ;
unary       = ( "-" | "+" ) unary | power ;
power       = postfix [ "**" unary ] ;               (* right associative *)
postfix     = atom { "[" expr "]" } ;
atom        = number | string | variable | constant
            | builtin "(" [ expr { "," expr } ] ")"
            | "(" expr ")" ;
```

Semantics follow Python exactly (charter §6.6). The cases below are covered by
the Tier 0 oracle:

* `%` and `//` are floor operations: `-1 % 64 == 63`.
* `/` always produces a float.
* `round` uses banker's rounding.
* `and`/`or` short-circuit and return an operand; `not` returns a bool.
* Truthiness follows Python: `0`, `0.0` and `""` are false.
* Floats print in shortest round-trip form (`3.0`, `0.1`).
* `random(min, max, decimals)` reproduces CPython's MT19937 stream for a given seed.

Builtins (`MATH_FUNCTIONS`, resolved at compile time with an arity check):
`abs acos asin atan atan2 ceil copysign cos degrees exp fabs floor fmod
get_datetime get_system int log log10 max min pow radians random remainder round
sin sqrt tan trunc` (`src/syntax/ast.h`). `get_datetime` is stubbed until M3.

## 3. Statements (draft grammar)

```ebnf
script      = { line } ;
line        = [ statement ] [ comment ] newline ;

statement   = assignment | array_assign | create_array
            | if_block | while_block | for_block | "break"
            | proc_def | "call" ident
            | frame_stmt | sprite_def | program_def
            | print_stmt | command ;

assignment  = variable "=" expr ;
array_assign= variable "[" expr "]" "=" expr ;
create_array= "create_array" "(" variable "," expr [ "," ident ] ")" ;  (* ident: numeric | string *)

if_block    = "if" cond "then" newline { line }
              { "elseif" cond "then" newline { line } }
              [ "else" newline { line } ]
              "endif" ;
while_block = "while" cond "then" newline { line } "endwhile" ;
for_block   = "for" variable "in" "(" expr "," expr "," expr ")" newline
              { line } "endfor" variable ;
cond        = expr ;                                  (* superset: D-005 *)

proc_def    = "def" ident "{" newline { line } "}" ;  (* no params; globals only *)

frame_stmt  = "begin_frame" [ "(" [ bool ] ")" ] | "end_frame" [ "()" ] ;
sprite_def  = "define_sprite" "(" ident "," expr "," expr ")" newline
              { line } "endsprite" ;
program_def = ( "grid_program" | "field_program" ) ident "{" newline
              { program_line } "}" ;                  (* keyword lines: size, cell, fields,
                                                         steps, sites, phases, active, … *)

print_stmt  = "print" "(" ( string | fstring | expr ) ")" ;
command     = ident [ "(" [ arg { "," arg } ] ")" ] ; (* verbs in COMMAND_DIALECT.md §5 *)
arg         = expr | bare_word ;                      (* bare_word: color / mode / name *)
```

Open points to settle during the M3 corpus parse:

1. The exact `for` range semantics: inclusive end, float steps (`v_dt = 0.05`),
   negative steps, and re-evaluation of the bounds.
2. Arguments are typed by parameter kind (`PARAMETER_TYPES`): a `color`
   parameter accepts bare named colors, while an `int` parameter evaluates an
   expression and then converts it with `int()`.
3. The program-block sub-language (`grid_program` / `field_program`) should use
   the same expression parser, with a keyword set taken from the legacy engines.
4. Which f-string forms are allowed in `print` (they appear in the corpus only
   inside comments so far).
5. What `break` does inside nested loops and procedures.

## 4. Corpus facts (scripts/main, 2026-11-14)

The most common statements are `if`/`endif` (3917), `call` (2126),
`create_array` (1634), `for`/`endfor` (1425), `plot` (1270), `draw_line` (1018)
and `def` (929). After those come `elseif` (683), `else` (655), `rest` (504),
`begin_frame` (453), `while` (388), `print` (315), `throttle` (134), and
`grid_program`/`field_program` (27). `break` appears 12 times. `continue`,
`return` and `grid_expr` never appear.
