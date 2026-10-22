from dataclasses import dataclass

import z3

from .types import ConcreteType


@dataclass(frozen=True)
class Value:
    bits: z3.BitVecRef
    poison: z3.BoolRef
    type: ConcreteType

    @classmethod
    def constant(cls, value, type_):
        return cls(z3.BitVecVal(value, type_.width), z3.BoolVal(False), type_)

    @classmethod
    def fresh(cls, name, type_, may_be_poison=True):
        poison = z3.Bool(f"{name}_poison") if may_be_poison else z3.BoolVal(False)
        return cls(z3.BitVec(name, type_.width), poison, type_)


def resize_bits(bits, width, signed=False):
    if width < bits.size():
        return z3.Extract(width - 1, 0, bits)
    if width == bits.size():
        return bits
    if signed:
        return z3.SignExt(width - bits.size(), bits)
    return z3.ZeroExt(width - bits.size(), bits)


def boolean_bits(condition):
    return z3.If(condition, z3.BitVecVal(1, 1), z3.BitVecVal(0, 1))


def bit_reverse(bits):
    if bits.size() == 1:
        return bits
    return z3.Concat(*(z3.Extract(index, index, bits) for index in range(bits.size())))


def mul_high_u(left, right):
    width = left.size()
    product = z3.ZeroExt(width, left) * z3.ZeroExt(width, right)
    return z3.Extract(2 * width - 1, width, product)


def emit(kind, args, type_):
    poison = z3.Or(*(arg.poison for arg in args))
    a = args[0].bits
    if kind == "Freeze":
        arbitrary = z3.FreshConst(z3.BitVecSort(type_.width), "freeze")
        bits = z3.If(args[0].poison, arbitrary, a)
        poison = z3.BoolVal(False)
    elif kind == "Select":
        bits = z3.If(a == 1, args[1].bits, args[2].bits)
        poison = z3.Or(args[0].poison,
                       z3.If(a == 1, args[1].poison, args[2].poison))
    elif kind in {"ResizeU", "ResizeS", "ResizeX"}:
        if kind == "ResizeX" and type_.width > a.size():
            upper = z3.FreshConst(z3.BitVecSort(type_.width - a.size()), "resize")
            bits = z3.Concat(upper, a)
        else:
            bits = resize_bits(a, type_.width, kind == "ResizeS")
    elif kind == "Popcount":
        bits = z3.BitVecVal(0, type_.width)
        for index in range(a.size()):
            bits += z3.ZeroExt(type_.width - 1, z3.Extract(index, index, a))
    elif kind == "BitReverse":
        bits = bit_reverse(a)
    else:
        b = args[1].bits
        if kind == "Add":
            bits = a + b
        elif kind == "Sub":
            bits = a - b
        elif kind == "Mul":
            bits = a * b
        elif kind == "MulHighU":
            bits = mul_high_u(a, b)
        elif kind == "And":
            bits = a & b
        elif kind == "Or":
            bits = a | b
        elif kind == "Xor":
            bits = a ^ b
        elif kind == "Eq":
            bits = boolean_bits(a == b)
        elif kind == "LtS":
            bits = boolean_bits(a < b)
        elif kind == "LtU":
            bits = boolean_bits(z3.ULT(a, b))
        elif kind == "Shl":
            bits = a << b
        elif kind == "ShrU":
            bits = z3.LShR(a, b)
        elif kind == "ShrS":
            bits = a >> b
        elif kind == "DivU":
            bits = z3.UDiv(a, b)
        elif kind == "DivS":
            bits = a / b
        elif kind == "ModU":
            bits = z3.URem(a, b)
        elif kind == "ModS":
            bits = z3.SRem(a, b)
        else:
            raise ValueError(f"unsupported instruction {kind!r}")
        if kind in {"Shl", "ShrU", "ShrS"}:
            poison = z3.Or(poison, z3.UGE(b, a.size()))
        if kind in {"DivU", "DivS", "ModU", "ModS"}:
            poison = z3.Or(poison, b == 0)
            if kind in {"DivS", "ModS"}:
                poison = z3.Or(poison, z3.And(a == 1 << (a.size() - 1), b == -1))
    return Value(z3.simplify(bits), z3.simplify(poison), type_)
