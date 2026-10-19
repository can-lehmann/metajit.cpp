from .cpp import emit
from .lower import lower
from .matcher import build_matchers


def generate(source, filename="<string>", handwritten=(), timeout_ms=120000, include_simplify=False):
    from .proof import prove

    rules = prove(source, filename, timeout_ms)
    matchers = build_matchers(rules)
    code = emit(lower(matchers, handwritten))
    if include_simplify:
        from .lower_simplify import lower as lower_simplify
        code += emit(lower_simplify(matchers), include_statistics=False)
    return code
