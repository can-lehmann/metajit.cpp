# Rewrite rule DSL for integer peephole rewrites

The DSL defines jitir rewrites that are proven correct with Z3. The rules are
used by builder folding and by Simplify and SimplifyTrace. Rules
are written in Python-like syntax and compiled to C++, but it's still possible
to write handwritten folding rules.

## How to use this

### Setup and workflow

Run the commands below from the metajit repository root. Use Python 3 with
`z3-solver` installed, or the distribution's `python3-z3` package. Other Python
components use only the standard library.

Put builder folding rules in `ruledsl/fold.rules`. Put rules for Simplify and
SimplifyTrace in `ruledsl/simplify.rules`. Each backend uses only its own rule
file. Simplification rules can query KnownBits. Rule names must be unique
within each file.

After editing rules, regenerate their C++ include:

```sh
make PYTHON=/path/to/venv/bin/python3 ruledsl/generated.inc.hpp
```

Generation proves the rules before writing it, and a proof failure leaves the
previous output untouched.

### Writing rules

```text
add_zero: Add(x, 0)
    => x

add_reassoc: Add(Add(x, C1), C2)
    C = C1 + C2
    => Add(x, C)

resize_roundtrip: ResizeU(ResizeU(x: T): U): T
    where width(U) >= width(T)
    => x
```

Each rule starts with the rule name, used in error messages and statistics. Its
body uses consistent space indentation, with one statement per line and a final
replacement introduced by `=>`. Blank lines and `#` comments are ignored.
Expressions cannot span lines.

Patterns contain names, integer literals, operation calls, and optional `: T`
annotations on values or operation results. Repeated names refer to the same
matched value. Names starting with `C` match constants. Types are inferred from
instruction signatures when annotations are omitted.

Calculations use `name = expression`. Conditions use `where expression` for
type conditions or `check expression` for value conditions. Expressions support
function calls, parentheses, unary `+`, `-`, `~`, and `not`, arithmetic `+`, `-`,
`*`, `//`, `%`, shifts `<<`, `>>`, bitwise `&`, `^`, `|`, comparisons, and boolean
`and` and `or`, with Python precedence, including chained comparisons like `a <
b < c`. Constant calculations have an inferred fixed-width bitvector type.

Integer literals support decimal, hexadecimal, and binary notation. Negative
literals are allowed in patterns and calculations.

Commutative patterns match either operand order, but associativity rewrites are
not inferred automatically. Matching prefers rules returning constants, then
existing values or matched subexpressions, then newly constructed operations.
Within each group, more specific patterns take priority, with file order
breaking ties. Replacements can reuse matched subexpressions and are folded
again so further rules can apply.

### Types and calculations

Supported concrete types are `Bool`, `Int8`, `Int16`, `Int32`, and `Int64`.
Unannotated values have inferred types. Annotations name type variables or
restrict a value to a concrete type. Instruction signatures determine which
operands and results share a type and which types are allowed. For example,
`Add(x, 0) => x` has four integer instantiations, while `And(x, x) => x` also
permits `Bool`.

Arithmetic, bitwise operations, shifts, comparisons, selects, resizes,
popcount, bit reversal, unsigned high multiplication, and freeze are supported.
Pointer, floating-point, memory, and tracing operations are not supported yet.

Type variables must be introduced by pattern annotations. Replacement types
must be inferable from the match, its root result, or concrete annotations.
An intermediate conversion with an independent, unconstrained replacement type
is rejected rather than inventing an arbitrary type.

Calculations can use matched constants and earlier calculation bindings, but
cannot inspect runtime IR values. The type of arithmetic and bitwise constant
calculations is the same as their operands. Mixed type calculations are
forbidden, and produce a type error at rule compilation time.
They need explicit conversion with `bits(C, T)`, which zero-extends
or truncates.
`width(T)` supplies a compile-time width constant, and `min(a, b)` and `max(a, b)`
operate on constants of the same type. Numeric calculations without an inferred
type from the match or replacement default to `Int64`.

Calculations in rule bodies use fixed-width wrapping arithmetic: addition,
subtraction, multiplication, negation, and bitwise operations are defined for
all operand values. Comparisons, division, remainder, and right shifts use
unsigned semantics. Division (`//`) and remainder (`%`) require a nonzero
divisor; shifts (`<<` and `>>`) require a count below the operand width. The
prover checks these requirements under preceding `check` conditions, respecting
short-circuit evaluation.

`ctz(C)` counts trailing zero bits of an unsigned constant, returning the input
bit width for zero. Its result has an independently inferred numeric type.

`bit_reverse(C)` reverses the bits at the inferred width of `C` and preserves
its type. `mul_high_u(C1, C2)` returns the upper half of the full unsigned
product, with both arguments and the result sharing a type. Unlike ordinary
DSL multiplication, it retains the full product before extracting the high
half. Both helpers also work in type conditions with literal arguments.

Compile-time Boolean results remain conditions in calculations and guards.
When used as IR values, including operands of replacement operations, they
materialize as `0: Bool` or `1: Bool`. They cannot be used as integer constants
or in numeric calculations without an explicit conversion through IR operations.
For example:

```text
eq_constants: Eq(C1, C2)
    C = C1 == C2
    => C
```

`where` accepts conditions depending only on types and literal calculations.
These conditions restrict the types for which the rule applies. Type equality
and inequality are allowed, as are width comparisons. A rule with no legal
assignments is rejected. Type-condition arithmetic wraps at its inferred width;
comparisons, division, and right shifts use unsigned values. Invalid evaluated
division and shift operations produce diagnostics. Boolean operators and
chained comparisons short-circuit.

`check` accepts conditions depending on matched constants (and analysis facts
based on KnownBits for Simplify/SimplifyTrace rules).
The conditions must hold before the replacement can be used. Dependencies
propagate through calculation bindings, so a matched constant cannot be hidden
in a `where` condition through an alias.
Conditions materialize as `Bool` constants when used as replacement IR values;
type references cannot be used as IR values.

### KnownBits conditions in Simplify/SimplifyTrace rules

`known_zeros(x)` and `known_ones(x)` return compile-time masks with the same
type as the matched value `x`. Their argument must be a name bound in the input
pattern, including a matched constant. Masks can be used in calculations and
`check` conditions, but cannot influence `where`, even through aliases.
Fact-dependent rules participate only in simplification matching. The initial
set lives in `simplify.rules`:

```text
add_disjoint: Add(x, y)
    check known_zeros(x) | known_zeros(y) == -1
    => Or(x, y)
```

### Proofs and diagnostics

Every rule is checked automatically during regeneration. It must preserve the
result values whenever the original expression has a defined result. It may
replace poison with a defined result, but must not turn a defined result into
poison (this is refinement). The rule must work at every type allowed by its
pattern and `where` conditions.

When a rule is rejected, the error identifies its file, line, name, and failing
types. A counterexample gives inputs that demonstrate the problem, followed by
the original and replacement results. The examples below show representative
diagnostics; the particular input values chosen can vary.

#### A replacement produces the wrong result

```text
drop_one: Add(x: Int8, 1)
    => x
```

The diagnostic shows that dropping the addition changes the result for `x = 0`:

```console
wrong_result.rules:1:1: rule 'drop_one' (result=Int8): could not prove refinement
counterexample:
  x: Int8 = 0, poison=false
  source: Int8 = 1, poison=false
  replacement: Int8 = 0, poison=false
```

This replacement needs to change: no value of `x` makes dropping the addition
valid. Arithmetic wraps at the matched type's width, so account for that when
interpreting counterexample values.

#### A replacement introduces poison

```text
select_to_or: Select(c, c, x)
    => Or(c, x)
```

This looks like a Boolean identity, but `Select` ignores poison in its
unselected arm. The diagnostic finds a defined original result and a poisoned
replacement:

```console
poison.rules:1:1: rule 'select_to_or' (result=Bool): could not prove refinement
counterexample:
  c: Bool = 1, poison=false
  x: Bool = 0, poison=true
  source: Bool = 1, poison=false
  replacement: Bool = 1, poison=true
```

Here `Select` chooses `c`, while `Or` uses the poisoned `x`. If the intended
rule only applies when that arm is a literal constant, match it with `C`
instead of `x`. Constant patterns currently match actual, nonpoison constants.

#### A calculation is guarded too late

```text
divide_constants: DivU(C1, C2)
    C = C1 // C2
    check C2 != 0
    => C
```

The relevant part of the error is:

```console
division.rules:2:5: rule 'divide_constants' (result=Int8): calculation may be undefined before its guard
counterexample:
  C1: Int8 = 0, poison=false
  C2: Int8 = 0, poison=false
```

Move `check C2 != 0` before the division. Checks protect only later
calculations. Similarly, guard shift calculations with a count below the
operand width. An invalid calculation is rejected even if the original jitir
expression would produce poison for those inputs.

#### A rule needs a type restriction

```text
byte_low_zero: And(Shl(x: T, 8), 255)
    => 0
```

Shifting left by eight clears the low byte, but an eight-bit value cannot be
shifted by eight without producing poison. Regeneration reports:

```console
width.rules:1:1: rule 'byte_low_zero' (result=Int8, T=Int8): source is always poison under its checks for this type instantiation; restrict the types with where
```

Add `where width(T) > 8` to exclude that type. Types are not silently skipped
when the source is always poison or the `check` conditions can never hold.
Use `where` to describe the types for which the rule is intended.

#### Timeouts and proof limits

A timeout stops generation; it does not mean that a counterexample was found.
The default limit is two minutes per query. Increase it with the CLI's
`--timeout-ms` option or the Python API's `timeout_ms` argument when needed.

For a rule whose larger-width proofs are too expensive, put
`SORRY_Z3_SMALL_WIDTHS` first in its body:

```text
mod_u_power_of_two: ModU(x, C)
    SORRY_Z3_SMALL_WIDTHS
    check C != 0 and C & (C - 1) == 0
    Cmask = C - 1
    => And(x, Cmask)
```

This proves the rewrite only where all matched and replacement IR types are
at most 16 bits, including intermediate types. Larger-width versions remain
active in the generated C++, with an `UnprovedWidthsWarning` identifying the
missing proofs. At least one small-width case must exist. Calculation validity
and checks that the rule can apply are still verified at every width. This
annotation is an explicit acceptance of missing proofs, not a fix for an
incorrect rule.

`Freeze` and widening `ResizeX` have unspecified bits. Do not assume those bits
are zero or that separate occurrences make the same choices. Some valid
rewrites involving these operations cannot currently be proved.

Also check that your rule cannot repeatedly undo another rule. Successful
proofs do not establish that rewriting terminates.

### Testing rules

Run the Python DSL tests with a Python environment that has Z3 installed:

```sh
python3 -m unittest discover -s ruledsl -t .
```

Add C++ regression coverage when a rule needs integration testing. For example,
build the optimizer tests and select the relevant cases with `-k`:

```sh
make PYTHON=/path/to/venv/bin/python3 tests/test_opt
./tests/test_opt -k 'rule DSL'
```

`tests/test_opt.cpp` exercises folding through `Builder`, `TraceBuilder`, and
the simplification passes. Standalone native emission tests require `clang++`
or `g++`. `make test-ruledsl` is another entry point for the Python suite;
`make test` also includes it.

### Statistics and comparison builds

Define `METAJIT_RULE_STATS` to enable the generated public static
`ruledsl_rule_counts` and `ruledsl_rule_names` arrays. Counters are incremented
on successful DSL rewrites, and commutative variants share one source-rule ID.
With statistics disabled, the arrays and increments are omitted by the C++
preprocessor. No runtime hook is required for counting.

Define `METAJIT_DISABLE_RULEDSL` to disable generated rewrites for comparison
tests. Folding entry points retain handwritten `_fold_*` fallbacks where
registered and otherwise use raw `build_*` methods. The generated simplifier
returns `nullptr` without constructing instructions. Rule proofs still run
during rule-include generation.
