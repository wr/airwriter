"""Minimal, string-aware s-expression block scanner for .kicad_pcb text edits.
Parenthesis counting must skip quoted strings -- several footprints in this project
carry parentheses inside their descr text."""

def block_end(s, i):
    """Given index i of an opening '(', return index just past its matching ')'."""
    assert s[i] == '('
    d = 0; j = i; n = len(s)
    while j < n:
        c = s[j]
        if c == '"':
            j += 1
            while j < n:
                if s[j] == '\\': j += 2; continue
                if s[j] == '"': break
                j += 1
        elif c == '(': d += 1
        elif c == ')':
            d -= 1
            if d == 0: return j + 1
        j += 1
    raise ValueError("unbalanced")

def top_blocks(s, tag):
    """Yield (start, end, text) for every top-level '(<tag> ' block."""
    needle = "\n\t(" + tag + " "
    i = 0
    while True:
        k = s.find(needle, i)
        if k < 0: return
        st = k + needle.index("(")
        en = block_end(s, st)
        yield st, en, s[st:en]
        i = en
