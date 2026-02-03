import sys

with open('src/selfhost/transpiler.bnk', 'r') as f:
    lines = f.readlines()

start_idx = -1
end_idx = -1

for i, line in enumerate(lines):
    if 'has method emitCall:' in line:
        start_idx = i
    if 'has method emitIfStmt:' in line:
        end_idx = i
        break

if start_idx == -1 or end_idx == -1:
    print(f"Could not find emitCall block. Start: {start_idx}, End: {end_idx}")
    sys.exit(1)

new_method = """    has method emitCall: expr: void* -> void:
        let call: MethodCall <- expr;
        let st: SymbolTable <- nil;
        let lc: ASTList <- nil;
        let nc: ASTNode <- nil;
        let first: bool <- true;
        
        if call.metTok.value == "print":
            let argType: string <- "string";
            if call.args != nil:
                let l: ASTList <- call.args;
                if l.head != nil:
                    let n: ASTNode <- l.head;
                    
                    if self.symbols != nil:
                        let st_print: SymbolTable <- self.symbols;
                        let h: Header <- n.value;
                        if h.type == "VariableExpr":
                            let v: VariableExpr <- n.value;
                            argType <- st_print: resolve: v.name.value;
                        else:
                            if h.type == "LiteralExpr":
                                let lit: LiteralExpr <- n.value;
                                argType <- lit.literalType;
                            else:
                                if h.type == "BinaryExpr":
                                    argType <- "int";
                                end if.
                            end if.
                        end if.
                    end if.
                    
                    if argType == "int":
                        self: emit: "printf(\\"%lld\\\\n\\", (long long)";
                    else:
                        if argType == "bool":
                            self: emit: "printf(\\"%s\\\\n\\", (";
                            self: emitExpression: n.value;
                            self: emit: " ? \\"true\\" : \\"false\\")";
                            self: emit: ")";
                            return.
                        else:
                            self: emit: "printf(\\"%s\\\\n\\", (char*)";
                        end if.
                    end if.
                    self: emitExpression: n.value;
                end if.
            end if.
            self: emit: ")";
            return.
        end if.

        # Generic Call or Static Call
        let isStatic: bool <- false;
        let entityName: string <- "";
        
        if call.receiver != nil:
            let hRec: Header <- call.receiver;
            if hRec.type == "VariableExpr":
                let vRec: VariableExpr <- call.receiver;
                if vRec.name.value == "Memory":
                    isStatic <- true;
                    entityName <- "Memory";
                else:
                    if self.symbols != nil:
                        let st2: SymbolTable <- self.symbols;
                        let resType: string <- st2: resolve: vRec.name.value;
                        if resType == "entity":
                            isStatic <- true;
                            entityName <- vRec.name.value;
                        end if.
                    end if.
                end if.
            end if.
        end if.

        if isStatic:
            if entityName == "Memory":
                if call.metTok.value == "free":
                    self: emit: "gc_free(";
                    if call.args != nil:
                        let lc2: ASTList <- call.args;
                        let nc2: ASTNode <- lc2.head;
                        if nc2 != nil: self: emitExpression: nc2.value; end if.
                    end if.
                    self: emit: ")";
                    return.
                end if.
            end if.
            
            self: emit: entityName + "_" + call.metTok.value + "(NULL";
            if call.args != nil:
                let lc3: ASTList <- call.args;
                let nc3: ASTNode <- lc3.head;
                loop:
                    if nc3 == nil: break; end if.
                    self: emit: ", ";
                    self: emitExpression: nc3.value;
                    nc3 <- nc3.next;
                end loop.
            end if.
            self: emit: ")";
        else:
            if call.receiver != nil:
                self: emit: "MethodCall_Generic(";
                self: emitExpression: call.receiver;
                self: emit: ", \\"" + call.metTok.value + "\\")";
            else:
                self: emit: call.metTok.value + "(";
                if call.args != nil:
                    let lc4: ASTList <- call.args;
                    let nc4: ASTNode <- lc4.head;
                    let firstCall: bool <- true;
                    loop:
                        if nc4 == nil: break; end if.
                        if (firstCall == false): self: emit: ", "; end if.
                        self: emitExpression: nc4.value;
                        firstCall <- false;
                        nc4 <- nc4.next;
                    end loop.
                end if.
                self: emit: ")";
            end if.
        end if.
    end method.

"""

output_lines = lines[:start_idx] + [new_method] + lines[end_idx:]
with open('src/selfhost/transpiler.bnk', 'w') as f:
    f.writelines(output_lines)

print(f"Successfully replaced emitCall from line {start_idx+1} to {end_idx}")
