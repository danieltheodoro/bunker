import os

# Full re-implementation of parseCall for clarity, fixed end if.
parse_call_reimpl = """    has method parseCall -> void*:
        expr <- self: parsePrimary;
        
        loop:
            tk: Token <- self: peek;
            
            # Method Call: ':'
            if tk.type == "SYMBOL" and tk.value == ":":
                nt: Token <- self: peekNext;
                if nt.type == "IDENT" or nt.type == "STRING" or nt.type == "NUMBER" or nt.value == "(":
                    self: advance; # Consume ':'
                    
                    args: ASTList <- nil;
                    methodName: string <- "";
                    
                    # Lookahead to see if it's receiver: method: args
                    n2: Token <- self: peekNext;
                    if nt.type == "IDENT" and n2.value == ":":
                         # receiver: method: args
                         methodName <- nt.value;
                         self: advance; # Consume methodName (nt)
                         self: advance; # Consume second ':'
                         args <- self: parseArgumentList;
                    else:
                         # method: args or receiver: method (no args)
                         # We treat it as methodName: args if expr is a VariableExpr and it's a simple call
                         head: Header <- expr;
                         isVar <- false;
                         if head != nil:
                              if head.type == "VariableExpr": 
                                   isVar <- true;
                              end if.
                         end if.
                         
                         if isVar:
                              vRec: VariableExpr <- Casts: toVariableExpr: expr;
                              methodName <- vRec.name.value;
                              expr <- nil;
                              args <- self: parseArgumentList;
                         else:
                              # It's an instance method call on expr, nt is the methodName
                              if nt.type == "IDENT":
                                   methodName <- nt.value;
                                   self: advance; # Consume methodName
                                   # Optional args if another ':'
                                   tkNext: Token <- self: peek;
                                   if tkNext.type == "SYMBOL" and tkNext.value == ":":
                                        self: advance;
                                        args <- self: parseArgumentList;
                                   end if.
                              else:
                                   print: "Error: Expected method name after ':', got " + nt.value;
                              end if.
                         end if.
                    end if.
                    
                    callRec: MethodCall <- MethodCall: new: "MethodCall", expr, methodName, args;
                    expr <- callRec;
                    continue;
                end if.
            end if.
            
            # Property Access: '.'
            if tk.type == "SYMBOL" and tk.value == ".":
                propTk: Token <- self: peekNext;
                if propTk.type == "IDENT":
                    self: advance; # Consume '.'
                    pTok: Token <- self: consume: "IDENT", "Expected property name.";
                    getRec: GetExpr <- GetExpr: new: "GetExpr", expr, pTok.value;
                    expr <- getRec;
                    continue;
                end if.
            end if.
            
            break;
        end loop.
        
        return expr;
    end method.
"""

# Find the current parseCall and replace it
bunker_path = "src/selfhost/bunker.bnk"
with open(bunker_path, 'r') as f:
    lines = f.readlines()

start_line = -1
end_line = -1
for i, line in enumerate(lines):
    if "has method parseCall -> void*:" in line:
        start_line = i
    if start_line != -1 and "return expr;" in line and i + 1 < len(lines) and "    end method." in lines[i+1]:
        end_line = i + 1
        break

if start_line != -1 and end_line != -1:
    new_lines = lines[:start_line] + [parse_call_reimpl] + lines[end_line+1:]
    with open(bunker_path, 'w') as f:
        f.writelines(new_lines)
    print(f"Successfully re-implemented parseCall in {bunker_path}")
else:
    print(f"Could not find parseCall in {bunker_path} (start={start_line}, end={end_line})")
