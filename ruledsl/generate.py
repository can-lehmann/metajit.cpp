from .cpp import emit
from . import ast
from .lower import expression_nodes, lower
from .matcher import MatcherProgram, build_matchers
from .optimize import optimize


def generate(source, filename="<string>", handwritten=(), timeout_ms=120000, include_simplify=False,
             simplify_source=None, simplify_filename="<simplify>"):
    from .proof import prove

    rules = prove(source, filename, timeout_ms)
    if simplify_source is not None:
        if not include_simplify:
            raise ValueError("simplify_source requires include_simplify")
        rules += prove(simplify_source, simplify_filename, timeout_ms)
    names = [checked.rule.name for checked in rules]
    if len(set(names)) != len(names):
        raise ValueError("duplicate rule names across rule sources")
    operations = dict.fromkeys(node.name for checked in rules
                               for node in expression_nodes(checked.rule.replacement)
                               if isinstance(node, ast.Call))
    matcher = build_matchers(rules, include_facts=False)
    code = emit(optimize(lower(matcher, handwritten, operations)))
    if include_simplify:
        from .lower_simplify import lower as lower_simplify
        code += emit(optimize(lower_simplify(build_matchers(rules))), include_statistics=False)
    fallback = emit(optimize(lower(MatcherProgram((), {}), handwritten,
                                   dict.fromkeys((*matcher.matchers, *operations)))))
    if include_simplify:
        fallback += emit(optimize(lower_simplify(MatcherProgram((), {}))), include_statistics=False)
    return "#ifndef METAJIT_DISABLE_RULEDSL\n" + code + "#else\n" + fallback + "#endif\n"
