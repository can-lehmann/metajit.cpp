# Rewrite rule DSL

A standalone Python 3 tokenizer, recursive-descent parser, type checker,
Z3 proof backend, matcher constructor, imperative code IR, and C++ emitter for
JITIR rewrite rules. Header generation integrates the rules into metajit's
builder and a shared local rewriter for Simplify and SimplifyTrace, while
retaining handwritten folding fallbacks.
Proofs require `z3-solver`; the other Python components use the standard library.

From the metajit repository root, run all tests with a Python environment that
has `z3-solver` installed:

```sh
python3 -m unittest discover -s ruledsl -t .
```

The parser, type-checker, matcher, and imperative IR tests also run without Z3:

```sh
python3 -m unittest ruledsl.test_parser ruledsl.test_typecheck ruledsl.test_matcher ruledsl.test_ir ruledsl.test_lower
```

Native emission tests use an available `clang++` or `g++`, targeting C++17 with
warnings as errors and undefined-behavior sanitization. They run generated code
in a small standalone model of the builder API, with statistics enabled and
disabled. They do not regenerate metajit headers or change the production build.

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

Each rule starts at column one. Its body uses consistent space indentation,
with one statement per line and a final replacement introduced by `=>`.
Blank lines and `#` comments are ignored. Expressions cannot span lines.

Patterns contain names, integer literals, operation calls, and optional `: T`
annotations on values or operation results. Repeated names refer to the same
matched value. Names starting with `C` match constants. The parser preserves
names and annotations; the separate type checker resolves them against
instruction signatures.

Calculations use `name = expression`. Conditions use `where expression` for
type conditions or `check expression` for value conditions. Expressions support
function calls, parentheses, unary `+`, `-`, `~`, and `not`, arithmetic `+`, `-`,
`*`, `//`, `%`, shifts `<<`, `>>`, bitwise `&`, `^`, `|`, comparisons, and boolean
`and` and `or`, with Python precedence. Chained comparisons have a distinct AST
node. Constant calculations have an inferred fixed-width bitvector type.
The parser does not evaluate expressions.

Integer literals support decimal, hexadecimal, and binary notation. A minus
sign is a separate token, parsed as part of a signed literal in patterns or as
a unary operator in expressions.

`ruledsl.parse(source, filename)` returns a tuple of rules with source spans.
`ruledsl.ParseError` includes the filename, line, column, and source excerpt.

## Type checking

`ruledsl.check(source, filename)` parses and checks all rules, returning a tuple
of `CheckedRule` objects. Type errors raise `ruledsl.TypeCheckError` with source
locations. The original AST is preserved alongside expression types, bindings,
named type variables, and legal concrete instantiations. `type_of(expression,
instantiation)` returns an expression's concrete type for one instantiation.
Symbolic type representatives and their domains are also retained for matching
and eventual C++ emission, independently of the enumerated proof assignments.

Supported concrete types are `Bool`, `Int8`, `Int16`, `Int32`, and `Int64`.
Unannotated values get fresh type variables. Annotations name type variables or
restrict a value to a concrete type. Instruction signatures unify operands and
results and restrict type domains. For example, `Add(x, 0) => x` has four integer
instantiations, while `And(x, x) => x` also permits `Bool`.

`signatures.py` contains the current integer/Boolean subset of JITIR signatures,
including arithmetic, bitwise operations, shifts, comparisons, selects, resizes,
popcount, and freeze. Pointer, floating-point, memory, and tracing operations are
not supported yet. Signatures also record commutativity for future matching.

Type variables must be introduced by pattern annotations. Replacement types
must be inferable from the match, its root result, or concrete annotations.
An intermediate conversion with an independent, unconstrained replacement type
is rejected rather than inventing an arbitrary type.

Calculations can use matched constants and earlier calculation bindings, but
cannot inspect runtime IR values. Constant operands and results share a type
unless explicitly converted with `bits(C, T)`, which zero-extends or truncates.
`width(T)` supplies a compile-time width constant, and `min(a, b)` and `max(a, b)`
operate on constants of the same type. Numeric calculations without an inferred
type from the match or replacement default to `Int64`.

`ctz(C)` counts trailing zero bits of an unsigned constant, returning the input
bit width for zero. Its result has an independently inferred numeric type.
Generated C++ uses a ternary expression to handle zero before evaluating
`__builtin_ctzll`.

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
These conditions filter the enumerated concrete type assignments. Type equality
and inequality are allowed, as are width comparisons. A rule with no legal
assignments is rejected. Type-condition arithmetic wraps at its inferred width;
comparisons, division, and right shifts use unsigned values. Invalid evaluated
division and shift operations produce diagnostics. Boolean operators and
chained comparisons short-circuit.

`check` accepts conditions depending on matched constants as well. It is
type-checked but not evaluated by the type checker; the prover interprets it
symbolically. Dependencies propagate through calculation bindings, so a
matched constant cannot be hidden in a `where` condition through an alias.
Conditions and type references are distinct from JITIR's `Bool` values and
cannot be used as replacement IR values. Unbounded integer calculations and
known-bits facts are not part of this implementation.

## Proofs

```python
from ruledsl.proof import prove

rules = prove("""add_reassoc: Add(Add(x, C1), C2)
    C = C1 + C2
    => Add(x, C)
""", "example.rules")
```

`prove` returns the checked rules after proving every legal concrete type
instantiation, subject to explicit proof annotations. `ProofError` contains source
locations and the failing type assignment. A refinement failure also includes a
concrete counterexample with input values, poison flags, and source and replacement results. Each Z3 query
has a two-minute timeout by default, configurable with `timeout_ms`. Unknown
results and timeouts fail the proof.

Place `SORRY_Z3_SMALL_WIDTHS` first in a rule body to prove refinement only for
instantiations where every IR type in the input and replacement patterns is at
most 16 bits. This includes operands and intermediate operations, even when the
result is `Bool`. Wider instantiations remain enabled in generated code and are
reported with `UnprovedWidthsWarning`. At least one small-width instantiation
must exist. Type checking, calculation-definedness checks, guard satisfiability,
and the requirement for a defined source execution still apply at every width.
Unannotated rules continue to require full proofs.

```text
mod_u_power_of_two: ModU(x, C)
    SORRY_Z3_SMALL_WIDTHS
    check C != 0 and C & (C - 1) == 0
    Cmask = C - 1
    => And(x, Cmask)
```

The instruction semantics are adapted from RtlTrace's `tools/find_redundant.py`,
with additional support for `Popcount`. Each value has fixed-width bits and a
poison flag. Matched constants cannot be poison; other matched values can.
Repeated names denote the same value. Arithmetic wraps. Oversized shifts,
division by zero, and signed division or remainder overflow produce poison.
`Select` propagates poison from its condition and its selected arm only.
Consequently, `Select(c, c, x) => Or(c, x)` and its And counterpart are invalid
for potentially poisoned `x`. Their active DSL versions require a matched
constant as the other arm.

Under the rule's checks, a defined source must produce an equal, non-poison
replacement. Removing poison is permitted. For every concrete type assignment,
the checks must be satisfiable and there must be a defined source execution.
An unreachable assignment produces an error suggesting an explicit `where`
restriction rather than silently omitting that assignment.

For patterns containing `Select`, refinement is checked separately with the
first encountered select condition set to zero and one. This exhaustive split
keeps multiplication of constant-select arms tractable without enumerating all
combinations of independent conditions. Counterexamples retain the selected
condition value. A timeout or any other unknown solver result rejects the proof.

DSL constant calculations use unsigned comparisons, division, remainder, and
right shifts. Division by zero and shifts outside the operand width are invalid
calculations. The prover checks that each calculation is valid under preceding
checks, independently of source poison. A later check cannot protect an earlier
calculation. Boolean operators and chained comparisons short-circuit, including
their calculation-validity requirements.

`Freeze` uses arbitrary fixed bits for poison inputs, and widening `ResizeX`
uses arbitrary fixed upper bits. Distinct operation occurrences have independent
choices. As in the miner, the pointwise equality criterion is conservative: it
can reject valid refinements that require relating source and replacement
nondeterministic choices. It does not treat unspecified bits as zero or reuse
choices merely because operation patterns look identical.

## Matcher construction

```python
from ruledsl.matcher import build_matchers
from ruledsl.proof import prove

rules = prove("""add_zero: Add(x, 0)
    => x
""")
program = build_matchers(rules)
```

`build_matchers` takes checked rules; it does not itself invoke Z3. For the
production pipeline, pass the successfully proven rules returned by `prove`.
The result is a backend-independent `MatcherProgram` with a matcher per root
opcode and one global rule-name table. Every commutative variant retains the
source rule's ID so firing counters aggregate variants of the same rule.

Patterns are normalized separately from the source AST. All combinations of
commutative argument orderings are generated recursively, with identical
variants deduplicated. Noncommutative operations retain argument order.
No associativity rewrites are inferred implicitly.

Each opcode has up to three matching stages: rules returning constants, then
rules returning existing values or matched operation subtrees, then rules
building new operations. Within a
stage, structurally specific candidates are considered before wildcard
candidates; stable sorting prefers more structural and repeated-value tests,
with file order breaking ties at leaves. Constant tests and nested opcode tests
are shared through a pattern-matrix decision tree adapted from PyPy ruleopt.

`IsConstant` and `IsOperation` nodes have `yes`, `no`, and `next` continuations.
The selected branch is tried first; `next` holds wildcard candidates that may
match regardless of the test result. If a candidate's later guards fail,
matching continues. Operand paths below an operation are only visited after
that operation's opcode check succeeds. Paths are tuples of operand indices;
the empty path denotes the root operation, which can be virtual for `fold_*`.

Terminal candidates carry variable bindings, repeated-value equality tests,
literal tests, and symbolic type bindings. A repeated value must be the same
SSA value or an equal constant of the same type, matching the builder's
`is_always_equal` behavior. Literal values are masked to the matched type width.
Type bindings record equal-type paths and permitted domains, allowing predicates
such as `is_int` rather than enumerating concrete type combinations. The rule
body retains symbolic `where`, calculation, and `check` expressions for later
lowering. Matcher construction never reads the proof-instantiation list.

This layer builds the decision trees and candidate plans. It does not execute
rule bodies, construct replacement instructions, or emit C++. Tests interpret
the plans with a small value model and compare matches against an independent
recursive matcher.

## Imperative code IR

`ruledsl.ir` represents the code to be emitted separately from matcher decisions
and the DSL AST. Its statements are `Block`, `Bind`, `If`, `ExprStmt`, and
`Return`. Expressions are `Ref`, `Literal`, `Symbol`, `Call`, `Member`, `Unary`, and
`Binary`. Constructing these nodes does not execute calls or build JITIR.

`Value` objects identify local bindings independently of their eventual printed
names. Two values can have the same name hint without aliasing each other.
`Symbol` identifies an external name, while `Ref` refers to a local identity.
Calls can appear as standalone statements for effects such as firing counters.
`Return(Literal(None))` can represent a failed fold, while `Return()` represents
a return without a value. Literal payloads can include concrete JITIR type
values, without enumerating type instantiations in the emitted code.

`Node.walk()` traverses syntax in source order, including both branches and
repeated occurrences of shared expression nodes. Bound value identities are
not syntax children. `validate(block, parameters)` checks single definitions,
use-before-definition, and lexical scope. Branch-local bindings cannot escape
to their sibling branch or enclosing block. Validation does not infer types,
check instruction semantics, or rewrite the tree. There are no optimization
passes. `Member` represents pointer-member access in the C++ backend.

## Standalone generation

From the metajit repository root:

```sh
python3 -m ruledsl ruledsl/example.rules --handwritten Add --handwritten Sub -o /tmp/folds.inc.hpp
```

The equivalent Python API is:

```python
from ruledsl.generate import generate

cpp = generate(source, "folds.rules", handwritten={"Add", "Sub"})
```

`--include-simplify` or `include_simplify=True` also emits the existing-instruction
rewriter alongside the builder folds, using one proof and matcher construction.
The simplification lowering lives in `lower_simplify.py`; it shares matching,
guards, replacement lowering, and the imperative IR/C++ emitter with folding.

Generation always type-checks and proves every rule before lowering or emitting.
`--timeout-ms` sets the per-query proof timeout. Output goes to stdout unless
`--output` is specified. A proof or generation failure does not overwrite an
existing output file. The package imports Z3 only when proofs are requested.

The output is a fragment of public builder-class members, without includes,
namespace declarations, or a surrounding class. It requires the existing
`Value`, `Const`, instruction classes, `Type`, `is_int`, `is_int_or_bool`,
`type_width`, `type_mask`, and builder APIs. The surrounding translation unit
must include the standard definitions for `uint64_t` and `std::min`/`std::max`.

Generated public `fold_*` methods first try DSL rules. On a miss, they call a
registered handwritten `_fold_*` method with the same signature, or raw
`build_*` when no handwritten method is registered. Registration is explicit;
the generator does not inspect C++ to discover methods. Unknown operation names
are rejected. A registered operation gets a generated wrapper even without DSL
rules, and operations referenced in replacements get wrappers as well. Existing
handwritten folds for any of these methods must be registered to retain their
behavior when integration eventually replaces the public methods.

The matcher root is virtual: use the function's operands and result type rather
than allocating an instruction to inspect. Binary arithmetic obtains its
result type from an operand; comparisons have `Bool` result type; resizes receive
an explicit result type. Type variables become local C++ `Type` values with
equality and width checks. Constant calculations use unsigned fixed-width
arithmetic and explicit masks, matching the proof semantics.

Type guards enforce only constraints not already guaranteed by the matched
JITIR operations. The signature table's `unrestricted_types` identifies type
parameters that JITIR leaves unrestricted even though the DSL currently supports
only integer/Boolean instantiations: the operands of `Eq`, the arms/result of
`Select`, and the input/result of `Freeze`. These retain a domain guard unless
another matched operation establishes the restriction. Type equalities and
domains are propagated through the pattern before emitting additional guards;
explicit rule restrictions such as a resize's input/output type equality remain.

`lower.lower` converts a matcher program into scoped imperative IR, and
`cpp.emit` renders it. Type-only `where` guards execute before body calculations;
type-only aliases referenced by them are expanded in those guards. This ensures
excluded types cannot execute calculations that were proven safe only for
permitted types. Runtime `check` and calculation order is otherwise preserved.
Boolean and chained comparison short-circuiting is retained.

Successful replacement operations use public folding entry points so newly
exposed simplifications can fire. Matching proper subexpressions are reused
when the replacement has the same operation, bindings, and types, accounting
for commutativity. Other replacement instructions are built only after all
guards succeed, with explicit bindings preserving construction order.
Handwritten `_fold_*` methods may call public folds on transformed expressions,
but must eventually use raw `build_*` for an unchanged input. Rule directions
must avoid recursive rewrite cycles; proofs do not establish termination.

The emitter allocates local names by value identity, avoiding C++ keywords,
external symbols, and reserved generated names. It emits unused bindings with
`[[maybe_unused]]` rather than optimizing them away. There is no IR optimizer.

Define `METAJIT_RULE_STATS` to enable the generated public static
`ruledsl_rule_counts` and `ruledsl_rule_names` arrays. Counters are incremented
on successful DSL rewrites, and commutative variants share one source-rule ID.
With statistics disabled, the arrays and increments are omitted by the C++
preprocessor. No runtime hook is required for counting.

## Production integration

`Builder::try_simplify(Inst*)` is shared by Simplify and SimplifyTrace. It returns
`nullptr` without constructing IR when no rule matches. Constants and literals
match only actual `Const` operands, preserving the existing proof contract.
After a successful match, replacement construction calls `fold_*` and reuses
matching proper input subtrees. Rules equivalent to returning the root itself
are ignored. Both entry points increment the same source-rule firing counters.

Each pass retains its traversal, substitution, KnownBits, and guard handling.
Whole-result replacement from KnownBits still runs before the local rewriter.
Missing facts for newly constructed instructions are typed unknown values.
SimplifyTrace grows its fact and substitution tables when rewriting constructs
new instructions, so later guards can establish facts for those instructions.
Those facts are used only as the trace walk proceeds beyond the guard.

`TraceBuilder` already routes its relevant `build_*` methods to
`Builder::fold_*`, so preserving the latter interfaces also preserves trace
recording and the generated builder API.

Generated fold definitions are appended to the allocator builder plugin's
existing output in `jitir.py`, using its existing insertion point in
`jitir.tmpl.hpp`. This avoids another generated include and avoids changing
`lwir.cpp`. Existing handwritten methods are named `_fold_*` and explicitly
registered through `handwritten_folds` in `jitir.py`. Constant-select propagation
for arithmetic, bitwise operations, integer/Boolean equality, unsigned comparison,
unsigned resizing, truncating `ResizeX`, and left/unsigned-right shifts is
expressed in `fold.rules`. Shift rules require every calculated count to be
in range. Pointer/float equality and widening `ResizeX` retain small handwritten
fallbacks; widening `ResizeX` chooses zero upper bits, which the current
pointwise proof model cannot prove against arbitrary upper bits. The shared
constant-propagation helpers and macros have been removed. Convenience overloads
retain their public names and dispatch through generated folds.

Generated folds place a lone constant on the right of commutative operations
before invoking either a handwritten fallback or a raw builder. Matching runs
before this normalization and supports both operand orders, including nested
operations created through raw builders.

Every regeneration must verify the rules under their proof annotations before
writing generated output.
Both generation entry points enforce that requirement. `jitir.py` proves and
generates the folds before invoking any output-writing plugins, so proof errors
leave all existing outputs untouched. Both Makefiles track the rule file,
generator modules, and templates, with grouped targets for the generated headers.

Install Python's `z3-solver` package or the distribution's `python3-z3` package.
Set `PYTHON` to select a Python environment when invoking Make, for example:

```sh
make PYTHON=/path/to/venv/bin/python3 jitir.hpp
make PYTHON=/path/to/venv/bin/python3 test-ruledsl
```

`make test` includes the DSL unittest suite. CI installs the Python Z3 bindings
for header generation. `tests/test_opt.cpp` exercises generated folds through
both `Builder` and `TraceBuilder`, including handwritten fallbacks for pointer
values and constant-select propagation.
