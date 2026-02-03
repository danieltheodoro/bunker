import sys

with open('src/selfhost/bunker.bnk', 'r') as f:
    lines = f.readlines()

new_lines = []
for line in lines:
    new_lines.append(line)
    if 'stmt <- self: parseStatement;' in line:
        new_lines.append('            if stmt != nil: h: Header <- stmt; print: "DEBUG: parseBlock ADDED stmt type=" + h.type; end if.\n')

with open('src/selfhost/bunker.bnk', 'w') as f:
    f.writelines(new_lines)

# Also fix the transpiler debug to be unique
with open('src/selfhost/transpiler.bnk', 'r') as f:
    t_content = f.read()
t_content = t_content.replace('print: "DEBUG: emitExpression type=" + type;', 'print: "DEBUG: emitStatement type=" + type;')
# but restore it for emitExpression
t_content = t_content.replace('has method emitExpression: expr: void* -> void:\n        head: Header <- expr;\n        type: string <- head.type; print: "DEBUG: emitStatement type=" + type;', 'has method emitExpression: expr: void* -> void:\n        head: Header <- expr;\n        type: string <- head.type; print: "DEBUG: emitExpression type=" + type;')

with open('src/selfhost/transpiler.bnk', 'w') as f:
    f.write(t_content)
