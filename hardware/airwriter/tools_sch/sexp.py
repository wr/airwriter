import re

class Sym(str):
    """Bare symbol/atom, unquoted on output."""
    pass

def tokenize(s):
    tokens = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c in ' \t\r\n':
            i += 1
            continue
        if c == '(' or c == ')':
            tokens.append(c)
            i += 1
            continue
        if c == '"':
            j = i + 1
            buf = []
            while j < n:
                if s[j] == '\\' and j + 1 < n:
                    buf.append(s[j])
                    buf.append(s[j+1])
                    j += 2
                    continue
                if s[j] == '"':
                    break
                buf.append(s[j])
                j += 1
            tokens.append(('str', s[i:j+1]))  # keep raw including quotes
            i = j + 1
            continue
        j = i
        while j < n and s[j] not in ' \t\r\n()':
            j += 1
        tokens.append(('atom', s[i:j]))
        i = j
    return tokens

def parse(s):
    tokens = tokenize(s)
    pos = [0]
    def parse_expr():
        tok = tokens[pos[0]]
        if tok == '(':
            pos[0] += 1
            lst = []
            while tokens[pos[0]] != ')':
                lst.append(parse_expr())
            pos[0] += 1
            return lst
        elif isinstance(tok, tuple):
            pos[0] += 1
            if tok[0] == 'str':
                return tok[1]  # raw string w/ quotes, e.g. '"D1"'
            else:
                return Sym(tok[1])
        else:
            raise ValueError(f"unexpected token {tok}")
    exprs = []
    while pos[0] < len(tokens):
        exprs.append(parse_expr())
    return exprs

def serialize(node, indent=0):
    if isinstance(node, list):
        parts = []
        for item in node:
            parts.append(serialize(item))
        return '(' + ' '.join(parts) + ')'
    else:
        return str(node)

def unquote(s):
    # s is like '"D1"' -> D1 ; handles escaped quotes minimally
    assert s.startswith('"') and s.endswith('"')
    return s[1:-1].replace('\\"', '"')

def quote(s):
    return '"' + s.replace('"', '\\"') + '"'
