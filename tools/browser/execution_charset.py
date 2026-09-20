"""Match upstream GCC's CP932 execution charset for ordinary C literals.

Clang/Emscripten only accepts UTF-8 as its execution charset. Run this on
preprocessed C (comments removed), before AST lowering, so literal sizes and
initializers have the same bytes as the native build. Octal escapes cannot
consume a following hexadecimal digit, unlike variable-length hex escapes.
"""
import re

_LITERAL = re.compile(r'(?:u8|[LuU])?"(?:\\.|[^"\\])*"|(?:[LuU])?\'(?:\\.|[^\'\\])*\'', re.DOTALL)

def cp932_literals(source):
    def convert(match):
        literal = match.group()
        if literal[0] not in '\"\'':
            return literal  # Explicit Unicode/wide literals retain their encoding.
        return ''.join(c if ord(c) < 128 else ''.join('\\%03o' % b for b in c.encode('cp932')) for c in literal)
    return _LITERAL.sub(convert, source)
