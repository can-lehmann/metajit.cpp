from .matcher import Operation, pattern_positions
from .signatures import SIGNATURES


class TypeFacts:
    def __init__(self, pattern):
        self.parents = {}
        self.domains = {}
        positions = tuple(pattern_positions(pattern))
        for path, node in positions:
            self.parents[path] = path
            self.domains[path] = None
        for path, node in positions:
            if not isinstance(node, Operation):
                continue
            signature = SIGNATURES[node.name]
            variables = {}
            for position, variable in ((path, signature.result), *(
                    (path + (index,), variable)
                    for index, variable in enumerate(signature.parameters))):
                if variable in variables:
                    self.unify(variables[variable], position)
                else:
                    variables[variable] = position
            for variable, domain in signature.domains:
                if variable not in signature.unrestricted_types:
                    self.restrict(variables[variable], domain)

    def resolve(self, path):
        parent = self.parents[path]
        if parent != path:
            parent = self.resolve(parent)
            self.parents[path] = parent
        return parent

    def domain(self, path):
        return self.domains[self.resolve(path)]

    def restrict(self, path, domain):
        root = self.resolve(path)
        previous = self.domains[root]
        self.domains[root] = set(domain) if previous is None else previous.intersection(domain)

    def unify(self, left, right):
        left = self.resolve(left)
        right = self.resolve(right)
        if left == right:
            return
        domain = self.domains[right]
        if domain is not None:
            self.restrict(left, domain)
        self.parents[right] = left
