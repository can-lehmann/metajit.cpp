from .cpp import emit
from .lower import lower
from .matcher import build_matchers


def generate(source, filename="<string>", handwritten=(), timeout_ms=120000):
    from .proof import prove

    rules = prove(source, filename, timeout_ms)
    return emit(lower(build_matchers(rules), handwritten))
