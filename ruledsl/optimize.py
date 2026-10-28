from dataclasses import replace

from . import ir


def optimize_block(block):
    statements = []
    for statement in block.statements:
        if isinstance(statement, ir.If):
            statement = replace(statement, then_block=optimize_block(statement.then_block),
                                else_block=optimize_block(statement.else_block))
        statements.append(statement)
        if isinstance(statement, ir.Return):
            break
    return ir.Block(statements)


def optimize(program):
    return replace(program, functions=tuple(replace(function, body=optimize_block(function.body))
                                            for function in program.functions))
