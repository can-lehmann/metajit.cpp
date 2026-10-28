# Copyright 2026 Can Joshua Lehmann
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from .cpp import emit
from . import ast
from .lower import expression_nodes, lower
from .matcher import MatcherProgram, build_matchers
from .optimize import optimize


def generate(source, filename="<string>", handwritten=(), timeout_ms=120000, include_simplify=False,
             simplify_source=None, simplify_filename="<simplify>", simplify_folds=False):
    from .proof import prove

    if simplify_folds and not include_simplify:
        raise ValueError("simplify_folds requires include_simplify")
    fold_rules = prove(source, filename, timeout_ms)
    simplify_rules = ()
    if simplify_source is not None:
        if not include_simplify:
            raise ValueError("simplify_source requires include_simplify")
        simplify_rules = prove(simplify_source, simplify_filename, timeout_ms)
    rules = fold_rules + simplify_rules
    operations = dict.fromkeys(node.name for checked in rules
                               for node in expression_nodes(checked.rule.replacement)
                               if isinstance(node, ast.Call))
    matcher = build_matchers(fold_rules, include_facts=False)
    matcher = MatcherProgram(tuple(checked.rule.name for checked in rules), matcher.matchers)
    code = emit(optimize(lower(matcher, handwritten, operations)))
    if include_simplify:
        from .lower_simplify import lower as lower_simplify
        simplify_matcher = (build_matchers(rules) if simplify_folds else
                            build_matchers(simplify_rules, rule_id_offset=len(fold_rules)))
        code += emit(optimize(lower_simplify(simplify_matcher)), include_statistics=False)
    fallback = emit(optimize(lower(MatcherProgram((), {}), handwritten,
                                   dict.fromkeys((*matcher.matchers, *operations)))))
    if include_simplify:
        fallback += emit(optimize(lower_simplify(MatcherProgram((), {}))), include_statistics=False)
    return "#ifndef METAJIT_DISABLE_RULEDSL\n" + code + "#else\n" + fallback + "#endif\n"
