import os

def fix_file(path, search, replace):
    with open(path, 'r') as f:
        content = f.read()
    if search in content:
        new_content = content.replace(search, replace)
        with open(path, 'w') as f:
            f.write(new_content)
        print(f"Fixed {path}")
    else:
        print(f"Could not find search string in {path}")

# Fix bunker.bnk
bunker_path = "src/selfhost/bunker.bnk"
bunker_search = """        return expr;
    end method.

    has method parsePrimary -> void*:"""
bunker_replace = """        return expr;
    end method.

    has method parseArgumentList -> ASTList:
        list: ASTList <- ASTList: new: nil, nil;
        tail: ASTNode <- nil;
        
        loop:
             arg: void* <- self: parseExpression;
             
             node: ASTNode <- ASTNode: new: arg, nil;
             
             if list.head == nil:
                 list.head <- node;
                 tail <- node;
             else:
                 tailNode: ASTNode <- Casts: toASTNode: tail;
                 tailNode.next <- node;
                 tail <- node;
             end if.
             
             tk3: Token <- self: peek;
             if tk3.type == "SYMBOL" or tk3.type == "OPERATOR":
                 if tk3.value == ",":
                      self: advance;
                      continue;
                 end if.
             end if.
             
             break;
        end loop.
        return list;
    end method.

    has method parsePrimary -> void*:"""

fix_file(bunker_path, bunker_search, bunker_replace)

# Fix transpiler.bnk
transpiler_path = "src/selfhost/transpiler.bnk"
transpiler_search = """        if type == "AssignExpr": self: emitAssign: expr; return; end if.
        
        self: emit: "// Unknown expr: " + type;"""
transpiler_replace = """        if type == "AssignExpr": self: emitAssign: expr; return; end if.
        if type == "BinaryExpr": self: emitBinary: expr; return; end if.
        if type == "UnaryExpr": self: emitUnary: expr; return; end if.
        
        self: emit: "// Unknown expr: " + type;"""

fix_file(transpiler_path, transpiler_search, transpiler_replace)
