import sys

def fix_file(path, debug_expr=False):
    with open(path, 'r') as f:
        content = f.read()
    
    # Fix known issues
    if 'bunker.bnk' in path:
        # any known dangling end ifs? 
        # Actually let's just add the debug prints correctly
        if 'lit <- LiteralExpr: new: "LiteralExpr", numTok, "int";' in content:
            content = content.replace('lit <- LiteralExpr: new: "LiteralExpr", numTok, "int";', 'lit <- LiteralExpr: new: "LiteralExpr", numTok, "int"; print: "DEBUG: created LiteralExpr NUM val=" + lit.tok.value;')
        if 'lit <- LiteralExpr: new: "LiteralExpr", strTok, "string";' in content:
            content = content.replace('lit <- LiteralExpr: new: "LiteralExpr", strTok, "string";', 'lit <- LiteralExpr: new: "LiteralExpr", strTok, "string"; print: "DEBUG: created LiteralExpr STR val=" + lit.tok.value;')

    if 'transpiler.bnk' in path:
        # Identify emitExpression and emitLiteral
        # Add prints that use printf directly if possible, or just print
        if 'head: Header <- expr;' in content and debug_expr:
            content = content.replace('type: string <- head.type;', 'type: string <- head.type; print: "DEBUG: emitExpression type=" + type;')
        if 'lit: LiteralExpr <- expr;' in content and debug_expr:
            content = content.replace('lit: LiteralExpr <- expr;', 'lit: LiteralExpr <- expr; print: "DEBUG: emitLiteral val=\'" + lit.tok.value + "\' type=" + lit.literalType;')

    with open(path, 'w') as f:
        f.write(content)

fix_file('src/selfhost/bunker.bnk')
fix_file('src/selfhost/transpiler.bnk', debug_expr=True)
print("Files patched.")
