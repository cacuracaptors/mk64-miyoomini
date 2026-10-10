#!/usr/bin/env python3
"""Make the game's GNU-as data files (written for the Dreamcast's sh-elf as)
assemble with the ARM GNU as:
  - '#' and '!' (sh-elf as) start a comment anywhere (ARM as only accepts '#' at
    the start of a line) and /* */ comments are dropped,
  - sh-elf prefixes C symbols with '_', ARM does not: every identifier that starts
    with '_' loses ONE leading underscore ('_gKartLuigi000' -> 'gKartLuigi000',
    '__sequencesSegmentStart' -> '_sequencesSegmentStart').
Strings (.incbin paths) are left untouched.
usage: asm_arm.py in.s out.s"""
import re
import sys

MIPS_SET = re.compile(r'^\s*\.set\s+\w+\s*,\s*\$')
IDENT = re.compile(r'(?<![A-Za-z0-9_$.])_([A-Za-z_][A-Za-z0-9_]*)')


def convert(text):
    out = []
    code = []          # pending code text (outside strings/comments)
    i, n = 0, len(text)

    def flush():
        if code:
            out.append(IDENT.sub(r'\1', ''.join(code)))
            code.clear()

    while i < n:
        c = text[i]
        if c == '"':
            flush()
            j = i + 1
            while j < n and text[j] != '"' and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            # keep the line structure
            code.append('\n' * text.count('\n', i, j) or ' ')
            i = j
        elif c == '#' or c == '!':   # '!' is the SH assembler's comment character
            j = text.find('\n', i)
            i = n if j < 0 else j
        else:
            code.append(c)
            i += 1
    flush()
    lines = []
    for l in ''.join(out).split('\n'):
        # MIPS register aliases ('.set C0_SR, $12') would leave undefined '$12' symbols
        if MIPS_SET.match(l):
            l = ''
        lines.append(l.rstrip())
    return '\n'.join(lines)


def main():
    with open(sys.argv[1], encoding='latin-1') as f:
        text = f.read()
    with open(sys.argv[2], 'w', encoding='latin-1') as f:
        f.write(convert(text))
        f.write('\n')


if __name__ == '__main__':
    main()
