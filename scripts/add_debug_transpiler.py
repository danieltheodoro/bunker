import sys

with open('src/selfhost/transpiler.bnk', 'r') as f:
    lines = f.readlines()

for i, line in enumerate(lines):
    if 'has method emitExpression:' in line:
        # Add debug print after local vars (if any)
        lines.insert(i+4, '        # print: "DEBUG: emitExpression type=" + type;\n')
    if 'has method emitLiteral:' in line:
        lines.insert(i+4, '        # print: "DEBUG: emitLiteral val=\'" + lit.tok.value + "\' type=" + lit.literalType;\n')

# Actually, let's enable them
for i, line in enumerate(lines):
    if 'DEBUG: emitExpression' in line or 'DEBUG: emitLiteral' in line:
        lines[i] = line.replace('# ', '')

with open('src/selfhost/transpiler.bnk', 'w') as f:
    f.writelines(lines)
