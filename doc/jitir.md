# JITIR Instruction Reference

## Freeze

Arguments

- **a**: `Value*`

Return Type: `a->type()`

## AssumeConst

Arguments

- **a**: `Value*`

Return Type: `a->type()`

## Select

Arguments

- **cond**: `Value*`
- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `cond->type() == Type::Bool`
- `a->type() == b->type()`
## ResizeU

Arguments

- **a**: `Value*`
- **type**: `Type`

Return Type: `type`

Type Checks:

- `is_int_or_bool(a->type())`
- `is_int_or_bool(type)`
## ResizeS

Arguments

- **a**: `Value*`
- **type**: `Type`

Return Type: `type`

Type Checks:

- `is_int_or_bool(a->type())`
- `is_int_or_bool(type)`
## ResizeX

Arguments

- **a**: `Value*`
- **type**: `Type`

Return Type: `type`

Type Checks:

- `is_int_or_bool(a->type())`
- `is_int_or_bool(type)`
## Load

Arguments

- **ptr**: `Value*`
- **type**: `Type`
- **flags**: `LoadFlags`
- **aliasing**: `AliasingGroup`
- **offset**: `uint64_t`

Return Type: `type`

Type Checks:

- `ptr->type() == Type::Ptr`
## Store

Arguments

- **ptr**: `Value*`
- **value**: `Value*`
- **aliasing**: `AliasingGroup`
- **offset**: `uint64_t`

Return Type: `Type::Void`

Type Checks:

- `ptr->type() == Type::Ptr`
## AddPtr

Arguments

- **ptr**: `Value*`
- **offset**: `Value*`

Return Type: `Type::Ptr`

Type Checks:

- `ptr->type() == Type::Ptr`
- `offset->type() == Type::Int64`
## Add

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## Sub

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## Mul

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## DivS

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## DivU

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## ModS

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## ModU

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## And

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int_or_bool(a->type())`
## Or

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int_or_bool(a->type())`
## Xor

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int_or_bool(a->type())`
## ShrU

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## ShrS

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## Shl

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `a->type()`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## Eq

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `Type::Bool`

Type Checks:

- `a->type() == b->type()`
## LtU

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `Type::Bool`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## LtS

Arguments

- **a**: `Value*`
- **b**: `Value*`

Return Type: `Type::Bool`

Type Checks:

- `a->type() == b->type()`
- `is_int(a->type())`
## Branch

Arguments

- **cond**: `Value*`
- **true_block**: `Block*`
- **false_block**: `Block*`

Return Type: `Type::Void`

Type Checks:

- `cond->type() == Type::Bool`
## Jump

Arguments

- **block**: `Block*`

Return Type: `Type::Void`

## Exit

Arguments


Return Type: `Type::Void`

## Comment

Arguments

- **text**: `const char*`

Return Type: `Type::Void`


