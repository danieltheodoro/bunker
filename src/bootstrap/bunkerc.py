#!/usr/bin/env python3
import sys
import os
import re
import subprocess
from typing import List, Optional, Any, Tuple
from dataclasses import dataclass
import copy

# ==========================================
# 1. AST NODES
# ==========================================

class Node:
    pass

@dataclass
class Program(Node):
    module: str
    stmts: List[Node]

@dataclass
class EntityDecl(Node):
    name: str
    members: List[Node]
    type_params: List[str] = None # e.g. ['T', 'U']
    implements: List[str] = None # List of trait names

@dataclass
class TraitDecl(Node):
    name: str
    methods: List['MethodDecl'] # Only headers (body should be empty or default)
    type_params: List[str] = None

@dataclass
class MethodDecl(Node):
    name: str
    is_public: bool
    is_static: bool
    params: List[Tuple[str, str]] # List of (name, type)
    return_type: Optional[str]
    body: List[Node]

@dataclass
class FieldDecl(Node):
    name: str
    type: str
    is_public: bool
    is_static: bool

@dataclass
class UnabstractedBlock(Node):
    stmts: List['AsmStmt']

@dataclass
class AsmStmt(Node):
    arch: str # 'x86_64', 'c', etc
    code: str

@dataclass
class ForeignDecl(Node):
    abi: str # 'c23' etc
    funcs: List['ForeignFuncDecl']

@dataclass
class ForeignFuncDecl(Node):
    name: str # Bunker name (usually same as C)
    real_name: str # C name
    params: List[Tuple[str, str]]
    return_type: str

@dataclass
class StructDecl(Node):
    name: str
    fields: List['FieldDecl']
    type_params: List[str] = None

@dataclass
class MethodCall(Node):
    target: Optional['Expr']  # None for implicit 'this' or global
    method: str
    args: List['Expr']

@dataclass
class StringLiteral(Node):
    value: str

@dataclass
class IntegerLiteral(Node):
    value: int

@dataclass
class FloatLiteral(Node):
    value: float

@dataclass
class BooleanLiteral(Node):
    value: bool

@dataclass
class NilLiteral(Node):
    pass

@dataclass
class Block(Node):
    stmts: List[Node]

@dataclass
class IfStmt(Node):
    condition: Node
    then_branch: Node
    else_branch: Optional[Node] = None

@dataclass
class WhileStmt(Node):
    condition: Node
    body: Node

@dataclass
class GroupingExpr(Node):
    expression: Node

@dataclass
class LoopStmt(Node):
    body: Node

@dataclass
class DeferStmt(Node):
    stmt: Node

@dataclass
class BreakStmt(Node):
    label: Optional[str] = None

@dataclass
class ScopedStmt(Node):
    body: Node

@dataclass
class ContinueStmt(Node):
    label: Optional[str] = None

@dataclass
class ReturnStmt(Node):
    expr: Node

@dataclass
class PanicStmt(Node):
    message: Node
@dataclass
class TryStmt(Node):
    body: Node
    catch_var: Optional[str]
    catch_body: Optional[Node]
    finally_body: Optional[Node]

@dataclass
class ThrowStmt(Node):
    expr: Node

@dataclass
class LambdaExpr(Node):
    params: list  # List of (name, optional_type) tuples
    body: Node    # Expression or Block

@dataclass
class ArrayLiteral(Node):
    elements: list[Node]
    element_type: Optional[str] = None

@dataclass
class ArrayAccess(Node):
    target: Node
    index: Node

@dataclass
class UnaryExpr(Node):
    op: str
    expr: Node


@dataclass
class BinaryExpr(Node):
    left: Node
    op: str
    right: Node

@dataclass
class FieldAccess(Node):
    target: Node
    field: str

@dataclass
class Variable(Node):
    name: str

@dataclass
class AssignStmt(Node):
    target: Node
    expr: Node
    type_hint: Optional[str] = None

# ==========================================
# 2. LEXER
# ==========================================

class TokenType:
    KEYWORD = 'KEYWORD'
    IDENT = 'IDENT'
    STRING = 'STRING'
    INTEGER = 'INTEGER'
    FLOAT = 'FLOAT'
    SYMBOL = 'SYMBOL'
    OPERATOR = 'OPERATOR'
    ASSIGN = 'ASSIGN'
    EOF = 'EOF'

@dataclass
class Token:
    type: str
    value: str
    line: int

class Lexer:
    def __init__(self, source: str):
        self.source = source
        self.pos = 0
        self.line = 1
        self.tokens = []
        self._tokenize()

    def _tokenize(self):
        # Regex patterns
        patterns = [
            ('COMMENT', r'#.*'),
            (TokenType.STRING, r'"(?:[^"\\]|\\.)*"'),
            (TokenType.KEYWORD, r'\b(let|module|Entity|Trait|Struct|struct|has|public|private|method|return|if|else|while|nil|import|include|panic|static|unabstracted|foreign|asm|func|loop|break|continue|defer|try|catch|finally|throw|true|false|end|scoped|implements|condition|then)\b'),
            (TokenType.ASSIGN, r'<-'),
            (TokenType.OPERATOR, r'==|!=|>=|<=|->|[\+\-\*\/%<>=!&]'), # Split operators to support generic nested brackets
            (TokenType.FLOAT, r'\d+\.\d+'),
            (TokenType.INTEGER, r'\d+'),
            (TokenType.IDENT, r'[a-zA-Z_][a-zA-Z0-9_]*'),
            (TokenType.SYMBOL, r'[:\.\(\),\[\];\\{}]'),
            ('WHITESPACE', r'\s+'),
        ]
        
        full_regex = '|'.join(f'(?P<{name}>{pattern})' for name, pattern in patterns)
        
        for match in re.finditer(full_regex, self.source):
            kind = match.lastgroup
            value = match.group()
            
            if kind == 'WHITESPACE' or kind == 'COMMENT':
                self.line += value.count('\n')
                continue
            
            if kind == TokenType.STRING:
                self.line += value.count('\n')
                value = value[1:-1] # Strip quotes
                
            self.tokens.append(Token(kind, value, self.line))
        
        self.tokens.append(Token(TokenType.EOF, '', self.line))

# ==========================================
# 3. PARSER (Recursive Descent)
# ==========================================

class Parser:
    def __init__(self, tokens: List[Token]):
        self.tokens = tokens
        self.pos = 0

    def peek(self) -> Token:
        return self.tokens[self.pos]

    def peek_next(self) -> Token:
        if self.pos + 1 < len(self.tokens):
            return self.tokens[self.pos + 1]
        return Token(TokenType.EOF, '', self.line)

    def consume(self, expected_type: str = None, expected_value: str = None) -> Token:
        token = self.peek()
        if expected_type and token.type != expected_type:
            raise SyntaxError(f"Expected {expected_type}, got {token.type} '{token.value}' at line {token.line}")
        if expected_value and token.value != expected_value:
            raise SyntaxError(f"Expected '{expected_value}', got '{token.value}' at line {token.line}")
        self.pos += 1
        return token

    def _consume_field_name(self) -> str:
        if self.peek().type == TokenType.KEYWORD:
            return self.consume(TokenType.KEYWORD).value
        return self.consume(TokenType.IDENT).value

    def parse_type_ref(self) -> str:
        token = self.peek()
        if token.type == TokenType.IDENT or token.type == TokenType.KEYWORD:
             name = self.consume().value
        else:
             raise SyntaxError(f"Expected type name, got {token.value} at line {token.line}")
        # Check for generics <T, U>
        if self.peek().type == TokenType.OPERATOR and self.peek().value == '<':
            self.consume(TokenType.OPERATOR, '<')
            args = []
            while True:
                args.append(self.parse_type_ref())
                if self.peek().type == TokenType.SYMBOL and self.peek().value == ',':
                    self.consume(TokenType.SYMBOL, ',')
                    continue
                break
            
            self.consume(TokenType.OPERATOR, '>')
            name = f"{name}<{', '.join(args)}>"
        
        # Check for pointers
        while self.peek().type == TokenType.OPERATOR and self.peek().value == '*':
            self.consume(TokenType.OPERATOR, '*')
            name += "*"
        
        return name

    def parse_program(self) -> Program:
        # module name
        self.consume(TokenType.KEYWORD, 'module')
        module_name = self.consume(TokenType.IDENT).value
        # Handle multipart: .sub.name
        while self.peek().value == '.':
             self.consume(TokenType.SYMBOL, '.')
             if self.peek().type == TokenType.IDENT:
                 module_name += "." + self.consume(TokenType.IDENT).value
             else:
                 # Trailing dot terminator
                 break
        self.consume(TokenType.SYMBOL, ';')
        
        stmts = []
        
        # Parse Imports
        while self.peek().type == TokenType.KEYWORD:
             if self.peek().value == 'import' or self.peek().value == 'include':
                 self.consume() # keyword
                 # Parse module path a.b.c
                 path_str = self.consume(TokenType.IDENT).value
                 while self.peek().value == '.':
                     # Check if separator
                     if self.peek_next().type == TokenType.IDENT:
                         self.consume(TokenType.SYMBOL, '.')
                         path_str += "." + self.consume(TokenType.IDENT).value
                     else:
                         break
                 
                 # Consume trailing delimiter (statement end)
                 self.consume(TokenType.SYMBOL, ';')
                 
                 # Optional : Alias
                 if self.peek().value == ':':
                     self.consume(TokenType.SYMBOL, ':')
                     self.consume(TokenType.IDENT) # alias
                 
                 # Recursive Load
                 # Check local or std
                 candidates = [
                     path_str.replace('.', '/') + ".bnk",
                     os.path.join("src/std", path_str.replace('.', '/') + ".bnk")
                 ]
                 
                 loaded = False
                 for filepath in candidates:
                     print(f"DEBUG: Checking candidate {filepath}")
                     if os.path.exists(filepath):
                          print(f"DEBUG: Found {filepath}")
                          # Prevent circular? Naive check
                          with open(filepath, 'r') as f:
                              src = f.read()
                          l = Lexer(src)
                          p = Parser(l.tokens)
                          print(f"DEBUG: Parsing imported tokens: {len(l.tokens)} tokens")
                          imported_prog = p.parse_program()
                          # Append imported entities to current scope
                          print(f"DEBUG: Note: Imported entities: {[s.name for s in imported_prog.stmts if hasattr(s, 'name')]}")
                          stmts.extend(imported_prog.stmts)
                          # print(f"DEBUG: Imported {len(imported_prog.stmts)} stmts from {filepath}")
                          loaded = True
                          break
                 
                 if not loaded:
                     print(f"Warning: Could not resolve import '{path_str}'")

             else:
                 break

        while self.peek().type != TokenType.EOF:
            if self.peek().type == TokenType.KEYWORD:
                if self.peek().value == 'Entity':
                    stmts.append(self.parse_entity())
                elif self.peek().value == 'Trait':
                    stmts.append(self.parse_trait())
                elif self.peek().value in ['Struct', 'struct']:
                    stmts.append(self.parse_struct())
                elif self.peek().value == 'foreign':
                    stmts.append(self.parse_foreign())
                else:
                    self.consume()
            else:
                self.consume()
        
        return Program(module_name, stmts)

    def parse_entity(self) -> EntityDecl:
        self.consume(TokenType.KEYWORD, 'Entity')
        name = self.consume(TokenType.IDENT).value
        
        print(f"DEBUG: Entering parse_entity {name}", flush=True)
        type_params = []
        if self.peek().type == TokenType.OPERATOR and self.peek().value == '<':
            self.consume(TokenType.OPERATOR, '<')
            while True:
                type_params.append(self.consume(TokenType.IDENT).value)
                if self.peek().value == ',': 
                    self.consume(TokenType.SYMBOL, ',')
                    continue
                break
            self.consume(TokenType.OPERATOR, '>')
        
        implements = []
        if self.peek().type == TokenType.KEYWORD and self.peek().value == 'implements':
            self.consume(TokenType.KEYWORD, 'implements')
            while True:
                implements.append(self.parse_type_ref())
                if self.peek().value == ',':
                    self.consume(TokenType.SYMBOL, ',')
                    continue
                break
        
        self.consume(TokenType.SYMBOL, ':')
        
        members = []
        # Support 'has a method...'
        while self.peek().value == 'has':
            # print(f"DEBUG: parse_entity loop peek: {self.peek()}")
            self.consume(TokenType.KEYWORD, 'has')
            is_public = False
            is_static = False
            while self.peek().value in ['a', 'an', 'public', 'private', 'static']:
                if self.peek().value == 'public':
                    is_public = True
                if self.peek().value == 'static':
                    is_static = True
                self.consume()
            
            if self.peek().value == 'method':
                method_decl = self.parse_method(is_public, is_static)
                members.append(method_decl)
                print(f"DEBUG: Parsed method. Next token: {self.peek()}")
            elif self.peek().value == 'state' or self.peek().type == TokenType.IDENT:
                # Handle state or field
                if self.peek().value == 'state':
                    self.consume()
                
                field_name = self._consume_field_name()
                self.consume(TokenType.SYMBOL, ':')
                # Use generic type ref
                field_type = self.parse_type_ref()
                members.append(FieldDecl(field_name, field_type, is_public, is_static))
                
                # Semicolon separator for fields
                if self.peek().value == ';':
                    self.consume(TokenType.SYMBOL, ';')
            else:
                raise SyntaxError(f"Unexpected token in entity: {self.peek()}")
        
        # Closing dot for the EntityDecl construct itself
        if self.peek().value == '.':
             self.consume(TokenType.SYMBOL, '.')
        else:
             self.consume(TokenType.KEYWORD, 'end')
             if self.peek().value in ['Entity', 'entity']:
                 self.consume()
             self.consume(TokenType.SYMBOL, '.')
        return EntityDecl(name, members, type_params, implements)

    def parse_trait(self) -> TraitDecl:
        print("DEBUG: Entering parse_trait", flush=True)
        self.consume(TokenType.KEYWORD, 'Trait')
        name = self.consume(TokenType.IDENT).value
        
        type_params = []
        if self.peek().type == TokenType.OPERATOR and self.peek().value == '<':
            self.consume(TokenType.OPERATOR, '<')
            while True:
                type_params.append(self.consume(TokenType.IDENT).value)
                if self.peek().value == ',': 
                    self.consume(TokenType.SYMBOL, ',')
                    continue
                break
            self.consume(TokenType.OPERATOR, '>')
        
        self.consume(TokenType.SYMBOL, ':')
        
        methods = []
        while self.peek().value == 'has':
            self.consume(TokenType.KEYWORD, 'has')
            # Traits only have public instance methods usually, but we support modifiers
            while self.peek().value in ['a', 'an', 'public', 'private', 'static']:
                self.consume()
            
            if self.peek().value == 'method':
                method_decl = self.parse_method(True, False) # Trait methods are public by default
                methods.append(method_decl)
            else:
                raise SyntaxError(f"Unexpected token in trait: {self.peek()}")
        
        self.consume(TokenType.KEYWORD, 'end')
        if self.peek().value in ['Trait', 'trait']:
            self.consume()
        self.consume(TokenType.SYMBOL, '.')
        return TraitDecl(name, methods, type_params)

    def parse_struct(self) -> StructDecl:
        struct_keyword = self.consume(TokenType.KEYWORD).value  # Accept 'Struct' or 'struct'
        name = self.consume(TokenType.IDENT).value
        
        type_params = []
        if self.peek().type == TokenType.OPERATOR and self.peek().value == '<':
            self.consume(TokenType.OPERATOR, '<')
            while True:
                type_params.append(self.consume(TokenType.IDENT).value)
                if self.peek().value == ',': 
                    self.consume(TokenType.SYMBOL, ',')
                    continue
                break
            self.consume(TokenType.OPERATOR, '>')
        
        # Helper to determine block style
        is_brace = False
        if self.peek().value == '{':
             self.consume(TokenType.SYMBOL, '{')
             is_brace = True
        else:
             self.consume(TokenType.SYMBOL, ':')
        
        fields = []
        while True:
             # Check for end of block
             if is_brace:
                 if self.peek().value == '}': break
             else:
                 if self.peek().value == 'end': break # New syntax
                 if self.peek().value == '.': break # Legacy fallback
 # Structs also end with 'end Struct.'
                 # Fallback/Safety check for '.'
                 if self.peek().value == '.': break # Legacy or error safety
             
             field_name = self._consume_field_name()
             self.consume(TokenType.SYMBOL, ':')
             # Use generic type ref
             field_type = self.parse_type_ref()
             # Optional semicolon
             if self.peek().value == ';':
                 self.consume(TokenType.SYMBOL, ';')
             elif self.peek().value == ',': # Optional comma
                 self.consume(TokenType.SYMBOL, ',')
            
             fields.append(FieldDecl(field_name, field_type, True, False)) # All struct fields public/instance
             
        if is_brace:
            self.consume(TokenType.SYMBOL, '}')
        elif self.peek().value == '.':
            self.consume(TokenType.SYMBOL, '.')
        else:
            self.consume(TokenType.KEYWORD, 'end')
            if self.peek().value in ['Struct', 'struct']:
                self.consume()
            self.consume(TokenType.SYMBOL, '.')
            
        return StructDecl(name, fields, type_params)

    def parse_foreign(self) -> ForeignDecl:
        self.consume(TokenType.KEYWORD, 'foreign')
        abi = self.consume(TokenType.IDENT).value # e.g. c23
        self.consume(TokenType.SYMBOL, ':')
        
        funcs = []
        while self.peek().value != 'end':
            # func "name" (args) -> ret
            self.consume(TokenType.KEYWORD, 'func')
            real_name_token = self.consume(TokenType.STRING)
            real_name = real_name_token.value.strip('"') 
            
            # Param list (arg: type, arg: type)
            self.consume(TokenType.SYMBOL, '(')
            params = []
            if self.peek().value != ')':
                while True:
                    p_name = self.consume(TokenType.IDENT).value
                    self.consume(TokenType.SYMBOL, ':')
                    p_type = self.consume(TokenType.IDENT).value
                    params.append((p_name, p_type))
                    if self.peek().value == ',':
                         self.consume(TokenType.SYMBOL, ',')
                    else:
                        break
            self.consume(TokenType.SYMBOL, ')')
            
            # Return type
            return_type = "void"
            if self.peek().value == '->':
                self.consume(TokenType.OPERATOR, '->')
                return_type = self.consume(TokenType.IDENT).value
                
            funcs.append(ForeignFuncDecl(real_name, real_name, params, return_type))
            
            # Allow optional semicolon
            if self.peek().value == ';':
                self.consume(TokenType.SYMBOL, ';')
            
            # Optional terminator for function decl? For now no, just next line
            # But wait, grammar usually requires separators. Let's assume newline/start of next func implies it, or create strict rule later.
            # Spec example: func "sys" (...). NO DOT on func line, only at end of foreign block.
            
        self.consume(TokenType.KEYWORD, 'end')
        self.consume(TokenType.KEYWORD, 'foreign')
        self.consume(TokenType.SYMBOL, '.')
        return ForeignDecl(abi, funcs)

    def parse_method(self, is_public: bool, is_static: bool) -> MethodDecl:
        print("DEBUG: Entering parse_method", flush=True)
        self.consume(TokenType.KEYWORD, 'method')
        name = self.consume(TokenType.IDENT).value
        print(f"DEBUG: Parsing method {name}", flush=True)
        
        params = []
        return_type = "void"
        
        # Robust Parameter Parsing
        if self.peek().value == ':' or self.peek().value == '->' or self.peek().value == '-':
            if self.peek().value == ':':
                self.consume(TokenType.SYMBOL, ':')
            
            should_parse_params = False
            token = self.peek()
            if token.type == TokenType.IDENT:
                 if self.pos+1 < len(self.tokens) and self.tokens[self.pos+1].value == ':':
                      should_parse_params = True
            
            if token.type == TokenType.OPERATOR and (token.value == '->' or token.value == '-'):
                 should_parse_params = True

            if should_parse_params:
                # Parameters shouldn't be terminated by '.' anymore but probably just end when ':' (body start) is seen? 
                # Or wait, method declaration in Bunker: `method name: p1: t1, p2: t2 :`
                # The loop condition `while self.peek().value != '.'` was checking for the END of the params/signature line?
                # Actually, the signature ends with a COLON ':' before the body block starts.
                # Or sometimes body starts immediately. 
                # Let's adjust loop to stop at ':'
                while self.peek().value != ':' and self.peek().value != '.': # Keep dot check for safety/legacy
                     token = self.peek()
                     if self.peek().value == ";": self.consume() # Fix syntax and error msg                   
                     if token.type == TokenType.OPERATOR and (token.value == '->' or token.value == '-'):
                          if token.value == '-':
                               self.consume()
                               if self.peek().value == '>': self.consume()
                          else: self.consume()
                          return_type = self.parse_type_ref()
                          break
                     
                     if token.type == TokenType.IDENT:
                          if self.pos+1 < len(self.tokens) and self.tokens[self.pos+1].value == ':':
                               param_start = self.pos
                               try:
                                   p_name = self.consume(TokenType.IDENT).value
                                   self.consume(TokenType.SYMBOL, ':')
                                   if self.peek().type != TokenType.IDENT:
                                        raise Exception("Not a type")
                                   # Check if this is actually a variable declaration (has <- after type)
                                   # by looking ahead after parsing the type
                                   type_start = self.pos
                                   p_type = self.parse_type_ref()
                                   if self.peek().type == TokenType.ASSIGN:
                                       # This is a variable declaration, not a parameter
                                       # Backtrack and stop parsing parameters
                                       self.pos = param_start
                                       break
                                   params.append((p_name, p_type))
                                   if self.peek().value == ',': self.consume()
                                   continue
                               except:
                                   self.pos = param_start
                                   break
                          else:
                               break
                     
                     # Not a parameter? Stop parsing.
                     break
                
                if self.peek().value == ':': 
                    self.consume(TokenType.SYMBOL, ':')
        
        body, dot = self.parse_block_body("method")
        
        if self.peek().value == 'end' and self.peek_next().value == 'method':
             self.consume(TokenType.KEYWORD, 'end')
             self.consume(TokenType.KEYWORD, 'method')
             self.consume(TokenType.SYMBOL, '.')
        elif self.peek().value == '.':
             self.consume(TokenType.SYMBOL, '.')
        elif not dot:
             # Fallback: if not dot-terminated, we MANDATE end method.
             self.consume(TokenType.KEYWORD, 'end')
             self.consume(TokenType.KEYWORD, 'method')
             self.consume(TokenType.SYMBOL, '.')
        
        return MethodDecl(name, is_public, is_static, params, return_type, body)

    def parse_block_body(self, context='') -> Tuple[List[Node], bool]:
        # Returns (stmts, terminated_by_dot)
        stmts = []
        terminated_by_dot = False
        
        while True:
            # STOP tokens
            token = self.peek()
            if token.type == TokenType.EOF: break
            if token.value == 'end': 
                if os.getenv("DEBUG_BLOCKS"): print(f"DEBUG: parse_block_body {context} breaking on 'end' at line {token.line}", flush=True)
                break # Generic terminator start
            if token.value == 'else': break # Else terminates 'then' block
            if token.value == 'catch': break # catch terminates try block
            if token.value == 'finally': break # finally terminates catch block
            
            # Check for '.' terminator: ONLY allowed if last stmt was return
            if token.type == TokenType.SYMBOL and token.value == '.':
                 if stmts and isinstance(stmts[-1], ReturnStmt):
                     self.consume(TokenType.SYMBOL, '.')
                     terminated_by_dot = True
                     break
                 else:
                     # Dot terminates block (e.g. shorthand if/while).
                     # We don't consume it here, parent rule will.
                     break
            
            stmts.append(self.parse_stmt())
            
            # Terminator check for statements
            if self.peek().value == ';':
                self.consume(TokenType.SYMBOL, ';')
                
        return stmts, terminated_by_dot

    def parse_stmt(self) -> Node:
        # Check for IF / WHILE / RETURN / PANIC / ASM / LOOP / BREAK / CONTINUE / DEFER
        if self.peek().type == TokenType.KEYWORD:
            kw = self.peek().value
            if kw == 'if': return self.parse_if()
            if kw == 'while': return self.parse_while()
            if kw == 'loop': return self.parse_loop()
            if kw == 'unabstracted': return self.parse_unabstracted()
            if kw == 'return':
                self.consume(TokenType.KEYWORD, 'return')
                if self.peek().value == ':': self.consume(TokenType.SYMBOL, ':')
                # Optional expr
                if self.peek().value not in [';', '.', 'else', 'has']:
                     expr = self.parse_expr()
                     return ReturnStmt(expr) 
                return ReturnStmt(NilLiteral())
            if kw == 'panic':
                self.consume(TokenType.KEYWORD, 'panic')
                if self.peek().value == ':': self.consume(TokenType.SYMBOL, ':')
                message = self.parse_expr()
                return PanicStmt(message)
            if kw == 'break':
                self.consume(TokenType.KEYWORD, 'break')
                label = None
                if self.peek().type == TokenType.STRING: # Bunker uses identifiers starting with ' for labels, lexer might see strings or we need to handle specially
                     # For now, simplistic break.
                     # v2.6.0 Spec says: break 'label.
                     # We'll support simple break for now.
                     pass
                if self.peek().value == '.': self.consume()
                elif self.peek().value == ';': self.consume()
                return BreakStmt(label)
            if kw == 'continue':
                self.consume(TokenType.KEYWORD, 'continue')
                if self.peek().value == '.': self.consume()
                elif self.peek().value == ';': self.consume()
                return ContinueStmt()
            if kw == 'try':
                return self.parse_try()
            if kw == 'throw':
                return self.parse_throw()
            if kw == 'scoped':
                self.consume(TokenType.KEYWORD, 'scoped')
                self.consume(TokenType.SYMBOL, ':')
                body_stmts, _ = self.parse_block_body('generic')
                body = Block(body_stmts)
                self.consume(TokenType.KEYWORD, 'end')
                self.consume(TokenType.KEYWORD, 'scoped')
                if self.peek().value == '.': self.consume()
                return ScopedStmt(body)

            if kw == 'defer':
                return self.parse_defer()
            if kw == 'nil':
                 self.consume(TokenType.KEYWORD, 'nil')
                 return NilLiteral()
            if kw == 'asm':
                self.consume(TokenType.KEYWORD, 'asm')
                arch = self.consume(TokenType.IDENT).value
                self.consume(TokenType.SYMBOL, ':')
                code_token = self.consume(TokenType.STRING)
                code = code_token.value.strip('"').replace('\\"', '"')
                return AsmStmt(arch, code)
            
            if kw == 'let':
                return self.parse_let()
                
                
        if self.peek().type == TokenType.IDENT or (self.peek().type == TokenType.KEYWORD and self.peek().value == 'foreign'):
             token = self.peek()
             
             # Check for `c* : "..."` (Raw C)
             if token.type == TokenType.IDENT and token.value.startswith('c') and \
                self.pos + 2 < len(self.tokens) and \
                self.tokens[self.pos+1].value == ':' and \
                self.tokens[self.pos+2].type == TokenType.STRING:
                    arch = self.consume(TokenType.IDENT).value
                    self.consume(TokenType.SYMBOL, ':')
                    code_token = self.consume(TokenType.STRING)
                    code = code_token.value.strip('"').replace('\\"', '"')
                    return AsmStmt(arch, code)

             # Check for `system <os> : "..."`
             if token.value == 'system' and \
                self.pos + 2 < len(self.tokens) and \
                self.tokens[self.pos+1].type == TokenType.IDENT and \
                self.tokens[self.pos+2].value == ':':
                    self.consume() # system
                    os_name = self.consume(TokenType.IDENT).value
                    self.consume(TokenType.SYMBOL, ':')
                    code_token = self.consume(TokenType.STRING)
                    code = code_token.value.strip('"').replace('\\"', '"')
                    return AsmStmt(f"system_{os_name}", code)

             return self.parse_assign_or_expr_stmt()
             
        raise SyntaxError(f"Unexpected token {self.peek()} in stmt")

    def parse_let(self) -> Node:
        self.consume(TokenType.KEYWORD, 'let')
        name = self.consume(TokenType.IDENT).value
        
        type_hint = None
        if self.peek().value == ':':
             self.consume(TokenType.SYMBOL, ':')
             type_hint = self.parse_type_ref()
             
        self.consume(TokenType.ASSIGN)
        expr = self.parse_expr()
        
        if self.peek().value == ';':
             self.consume(TokenType.SYMBOL, ';')
             
        return AssignStmt(Variable(name), expr, type_hint)

    def parse_unabstracted(self) -> UnabstractedBlock:
        self.consume(TokenType.KEYWORD, 'unabstracted')
        self.consume(TokenType.SYMBOL, ':')
        
        print(f"DEBUG PARSER: parse_unabstracted started")
        stmts = []
        while self.peek().value != '.':
            # Use general parse_stmt to support if/while/asm/system etc.
            stmts.append(self.parse_stmt())
            
            # Optional terminator
            if self.peek().value == ';':
                self.consume()
                
        self.consume(TokenType.SYMBOL, '.')
        print(f"DEBUG PARSER: parse_unabstracted finished with {len(stmts)} stmts")
        return UnabstractedBlock(stmts)

    def parse_assign_or_expr_stmt(self) -> Node:
        start_pos = self.pos
        
        # Helper lookahead
        def check(idx, type, value=None):
             if self.pos + idx >= len(self.tokens): return False
             t = self.tokens[self.pos + idx]
             return t.type == type and (value is None or t.value == value)

        # 1. Variable Declaration: name : type <- ...
        if check(0, TokenType.IDENT) and check(1, TokenType.SYMBOL, ':') and check(2, TokenType.IDENT):
             # Need to check if there's an ASSIGN after the type
             # Save position to potentially backtrack
             saved_pos = self.pos
             try:
                 name = self.consume(TokenType.IDENT).value
                 self.consume(TokenType.SYMBOL, ':')
                 type_hint = self.parse_type_ref()
                 if self.peek().type == TokenType.ASSIGN:
                     self.consume(TokenType.ASSIGN)
                     expr = self.parse_expr()
                     return AssignStmt(Variable(name), expr, type_hint)
                 else:
                     # Not a variable declaration, backtrack
                     self.pos = saved_pos
             except:
                 # Parsing failed, backtrack
                 self.pos = saved_pos
        
        # 2. Simple Assignment: name <- ...
        if check(0, TokenType.IDENT) and check(1, TokenType.ASSIGN):
             name = self.consume(TokenType.IDENT).value
             self.consume(TokenType.ASSIGN)
             expr = self.parse_expr()
             return AssignStmt(Variable(name), expr, None)

        # 3. Fallback: Parse Expression first (handles field/array access, calls)
        expr = self.parse_expr()
        
        # 4. Complex Assignment: expr <- ...
        if self.peek().value == '<-':
             self.consume(TokenType.ASSIGN)
             rhs = self.parse_expr()
             return AssignStmt(expr, rhs)
             
        return expr



    def parse_if(self) -> IfStmt:
        print(f"DEBUG: parse_if started (NEW SYNTAX) at line {self.peek().line}", flush=True)
        self.consume(TokenType.KEYWORD, 'if')
        self.consume(TokenType.SYMBOL, ':')
        
        # Parse condition block
        print("DEBUG: parse_if parsing CONDITION block", flush=True)
        self.consume(TokenType.KEYWORD, 'condition')
        self.consume(TokenType.SYMBOL, ':')
        cond_stmts, _ = self.parse_block_body('if-condition')
        condition = Block(cond_stmts)
        
        # Expect 'end condition.'
        self.consume(TokenType.KEYWORD, 'end')
        self.consume(TokenType.KEYWORD, 'condition')
        self.consume(TokenType.SYMBOL, '.')
        print("DEBUG: parse_if CONDITION block done", flush=True)
        
        # Parse then block
        print("DEBUG: parse_if parsing THEN block", flush=True)
        self.consume(TokenType.KEYWORD, 'then')
        self.consume(TokenType.SYMBOL, ':')
        then_stmts, _ = self.parse_block_body('if-then')
        then_branch = Block(then_stmts)
        
        # Expect 'end then.'
        self.consume(TokenType.KEYWORD, 'end')
        self.consume(TokenType.KEYWORD, 'then')
        self.consume(TokenType.SYMBOL, '.')
        print("DEBUG: parse_if THEN block done", flush=True)
        
        # Optional else block
        else_branch = None
        if self.peek().value == 'else':
            print("DEBUG: parse_if parsing ELSE block", flush=True)
            self.consume(TokenType.KEYWORD, 'else')
            self.consume(TokenType.SYMBOL, ':')
            else_stmts, _ = self.parse_block_body('if-else')
            else_branch = Block(else_stmts)
            
            # Expect 'end else.'
            self.consume(TokenType.KEYWORD, 'end')
            self.consume(TokenType.KEYWORD, 'else')
            self.consume(TokenType.SYMBOL, '.')
            print("DEBUG: parse_if ELSE block done", flush=True)
        
        # Final 'end if.'
        self.consume(TokenType.KEYWORD, 'end')
        self.consume(TokenType.KEYWORD, 'if')
        self.consume(TokenType.SYMBOL, '.')
        
        print("DEBUG: parse_if FINISHED", flush=True)
        return IfStmt(condition, then_branch, else_branch)

    def parse_while(self) -> WhileStmt:
        self.consume(TokenType.KEYWORD, 'while')
        condition = self.parse_expr()
        self.consume(TokenType.SYMBOL, ':')
        
        stmts, dot = self.parse_block_body('generic')
        
        # New syntax: end while.
        if self.peek().value == 'end' and self.peek_next().value == 'while':
            self.consume(TokenType.KEYWORD, 'end')
            self.consume(TokenType.KEYWORD, 'while')
            self.consume(TokenType.SYMBOL, '.')
        elif not dot:
            self.consume(TokenType.SYMBOL, '.')
        
        return WhileStmt(condition, Block(stmts))

    def parse_loop(self) -> LoopStmt:
        self.consume(TokenType.KEYWORD, 'loop')
        self.consume(TokenType.SYMBOL, ':')
        stmts, dot = self.parse_block_body('generic')
        block = Block(stmts)
        print(f"DEBUG: parse_loop dot={dot}. Peek: {self.peek()}", flush=True)
        
        # New syntax: end loop.
        if self.peek().value == 'end' and self.peek_next().value == 'loop':
            self.consume(TokenType.KEYWORD, 'end')
            self.consume(TokenType.KEYWORD, 'loop')
            self.consume(TokenType.SYMBOL, '.')
        elif not dot:
            self.consume(TokenType.SYMBOL, '.')
        
        return LoopStmt(block)
        


    def parse_defer(self) -> DeferStmt:
        self.consume(TokenType.KEYWORD, 'defer')
        self.consume(TokenType.SYMBOL, ':')
        stmt = self.parse_stmt()
        # Defer does not consume a dot itself, the contained stmt might or it might end with semicolon
        return DeferStmt(stmt)

    def parse_throw(self) -> ThrowStmt:
        self.consume(TokenType.KEYWORD, 'throw')
        if self.peek().value == ':': self.consume(TokenType.SYMBOL, ':')
        expr = self.parse_expr()
        # Do not consume terminator here.
        return ThrowStmt(expr)

    def parse_try(self) -> TryStmt:
        self.consume(TokenType.KEYWORD, 'try')
        self.consume(TokenType.SYMBOL, ':')
        body_stmts, _ = self.parse_block_body('generic')
        body = Block(body_stmts)
        
        catch_var = None
        catch_body = None
        finally_body = None
        
        # Check for catch
        # print(f"DEBUG: parse_try check catch, peek={self.peek()}")
        if self.peek().value == 'catch':
            self.consume(TokenType.KEYWORD, 'catch')
            
            # Optional variable: catch error:
            if self.peek().type == TokenType.IDENT:
                 catch_var = self.consume(TokenType.IDENT).value
                 
            self.consume(TokenType.SYMBOL, ':')
            catch_stmts, _ = self.parse_block_body('generic')
            catch_body = Block(catch_stmts)

        # Check for finally
        if self.peek().value == 'finally':
            self.consume(TokenType.KEYWORD, 'finally')
            self.consume(TokenType.SYMBOL, ':')
            finally_stmts, _ = self.parse_block_body('generic')
            finally_body = Block(finally_stmts)

        # Try stmt itself ends with a dot or 'end try.'
        if self.peek().value == 'end':
             self.consume(TokenType.KEYWORD, 'end')
             if self.peek().value == 'try':
                  self.consume(TokenType.KEYWORD, 'try')
             self.consume(TokenType.SYMBOL, '.')
        else:
             self.consume(TokenType.SYMBOL, '.')
        return TryStmt(body, catch_var, catch_body, finally_body)

    def parse_lambda(self) -> LambdaExpr:
        r"""Parse lambda: \param. body or \x, y. expression or \x.\y. x + y."""
        self.consume(TokenType.SYMBOL, '\\')
        
        params = []
        
        # Parse parameters
        # Can be: /. body (no params), /x. body (one param), or /x, y. body (multiple params)
        if self.peek().value != '.':
            # Parse first parameter
            param_name = self.consume(TokenType.IDENT).value
            param_type = None
            
            # Optional type annotation: /x: int. body
            if self.peek().value == ':' and self.pos + 1 < len(self.tokens) and \
               self.tokens[self.pos+1].type == TokenType.IDENT and \
               self.tokens[self.pos+1].value in ['int', 'float', 'str', 'string', 'bool']:
                self.consume(TokenType.SYMBOL, ':')
                param_type = self.consume(TokenType.IDENT).value
            
            params.append((param_name, param_type))
            
            # Parse additional parameters
            while self.peek().value == ',':
                self.consume(TokenType.SYMBOL, ',')
                param_name = self.consume(TokenType.IDENT).value
                param_type = None
                
                if self.peek().value == ':' and self.pos + 1 < len(self.tokens) and \
                   self.tokens[self.pos+1].type == TokenType.IDENT and \
                   self.tokens[self.pos+1].value in ['int', 'float', 'str', 'string', 'bool']:
                    self.consume(TokenType.SYMBOL, ':')
                    param_type = self.consume(TokenType.IDENT).value
                
                params.append((param_name, param_type))
        
        # Expect dot separator
        self.consume(TokenType.SYMBOL, '.')
        
        # Parse body (single expression for now)
        body = self.parse_expr()
        
        # Optional trailing dot (allows \x.\y. x + y.)
        if self.peek().value == '.':
            self.consume(TokenType.SYMBOL, '.')
        
        return LambdaExpr(params, body)


    def parse_method_call(self) -> MethodCall:
        if self.peek().type == TokenType.KEYWORD and self.peek().value == 'foreign':
            name_token = self.consume(TokenType.KEYWORD, 'foreign')
        else:
            name_token = self.consume(TokenType.IDENT)
        name = name_token.value
        
        target = None
        method_name = name
        args = []
        
        if self.peek().value == ':':
            self.consume(TokenType.SYMBOL, ':')
            # Lookahead: is this 'target : method' or 'method : arg'?
            if self.peek().type == TokenType.IDENT and name != 'print':
                # Case: target: method: args
                if self.pos+1 < len(self.tokens) and self.tokens[self.pos+1].value == ':':
                    target = Variable(name)
                    method_name = self.consume(TokenType.IDENT).value
                    self.consume(TokenType.SYMBOL, ':')
                elif name[0].isupper(): # Static call Entity: method
                    target = Variable(name)
                    method_name = self.consume(TokenType.IDENT).value
                    if self.peek().value == ':': self.consume(TokenType.SYMBOL, ':')
                else:
                    # Global or implicit this: print: arg (where arg is IDENT)
                    pass
            
            # Parse arguments
            if self.peek().value not in ['.', ';', 'else', 'has']:
                args.append(self.parse_expr())
                while self.peek().value == ',':
                    self.consume(TokenType.SYMBOL, ',')
                    if self.peek().value not in ['.', ';', 'else', 'has']:
                         args.append(self.parse_expr())
                    
        return MethodCall(target, method_name, args)

    def parse_expr(self) -> Node:
        return self.parse_logical_or()

    def parse_logical_or(self) -> Node:
        left = self.parse_logical_and()
        while self.peek().value == 'or':
            op = self.consume(TokenType.IDENT).value # 'or' is IDENT
            right = self.parse_logical_and()
            left = BinaryExpr(left, op, right)
        return left

    def parse_logical_and(self) -> Node:
        left = self.parse_additive()
        while self.peek().value == 'and':
            op = self.consume(TokenType.IDENT).value
            right = self.parse_additive()
            left = BinaryExpr(left, op, right)
        return left

    def parse_additive(self) -> Node:
        left = self.parse_multiplicative()
        while self.peek().type == TokenType.OPERATOR and self.peek().value in ['+', '-', '>', '<', '==', '!=', '>=', '<=']:
            op = self.consume(TokenType.OPERATOR).value
            right = self.parse_multiplicative()
            left = BinaryExpr(left, op, right)
        return left

    def parse_multiplicative(self) -> Node:
        left = self.parse_unary()
        while self.peek().type == TokenType.OPERATOR and self.peek().value in ['*', '/', '%']:
            op = self.consume(TokenType.OPERATOR).value
            right = self.parse_unary()
            left = BinaryExpr(left, op, right)
        return left

    def parse_unary(self) -> Node:
        if self.peek().type == TokenType.OPERATOR and self.peek().value in ['-', '+', '!', '*', '&']:
            op = self.consume(TokenType.OPERATOR).value
            expr = self.parse_unary()
            return UnaryExpr(op, expr)
        return self.parse_primary()

    def parse_primary(self) -> Node:
        token = self.peek()
        if token.type == TokenType.INTEGER:
            self.consume()
            return IntegerLiteral(int(token.value))
        if token.type == TokenType.FLOAT:
            self.consume()
            return FloatLiteral(float(token.value))
        if token.type == TokenType.STRING:
            self.consume()
            return StringLiteral(token.value)
        if token.type == TokenType.KEYWORD and token.value == 'nil':
            self.consume()
            return NilLiteral()
        if token.type == TokenType.KEYWORD and token.value == 'true':
            self.consume()
            return BooleanLiteral(True)
        if token.type == TokenType.KEYWORD and token.value == 'false':
            self.consume()
            return BooleanLiteral(False)
        if token.type == TokenType.IDENT:
            name = token.value
            self.consume()
            
            # Check for generics <T, U> (Only for type-like identifiers starting with uppercase)
            if name[0].isupper() and self.peek().type == TokenType.OPERATOR and self.peek().value == '<':
                # Disambiguation: Is it Type<...>: (method call or static field) or Type<...><- (let assignment) ?
                # Or is it a comparison 'a < b'?
                found_type_marker = False
                offset = 1 # We already consumed 'name', so self.pos is at '<'
                depth = 1
                while self.pos + offset < len(self.tokens):
                    t = self.tokens[self.pos + offset]
                    if t.type == TokenType.EOF or t.value in [';', '.', 'then']:
                        break
                    
                    if t.value == '<':
                        depth += 1
                    elif t.value == '>':
                        depth -= 1
                        if depth == 0:
                            # Look one more for ':' or '<-'
                            if self.pos + offset + 1 < len(self.tokens):
                                t_next = self.tokens[self.pos + offset + 1]
                                if t_next.value in [':', '<-', ',', ')', ']', ';', '.', 'then', 'else', 'do', 'has']:
                                    found_type_marker = True
                            break
                    offset += 1
                
                if found_type_marker:
                    self.consume(TokenType.OPERATOR, '<')
                    args = []
                    while True:
                        args.append(self.parse_type_ref())
                        if self.peek().type == TokenType.SYMBOL and self.peek().value == ',':
                            self.consume(TokenType.SYMBOL, ',')
                            continue
                        break
                    self.consume(TokenType.OPERATOR, '>')
                    name = f"{name}<{', '.join(args)}>"
            
            node = Variable(name)
            
            # Loop for postfix chains: .field, [index]
            while True:
                 # Check for Field Access: .field
                 if self.peek().value == '.' and self.pos + 1 < len(self.tokens) and \
                    (self.tokens[self.pos+1].type == TokenType.IDENT or self.tokens[self.pos+1].type == TokenType.KEYWORD) and \
                    self.peek().line == self.tokens[self.pos+1].line:
                      self.consume(TokenType.SYMBOL, '.')
                      field_name = self._consume_field_name()
                      node = FieldAccess(node, field_name)
                      continue
                 
                 # Check for Array Access: [index]
                 if self.peek().value == '[' and \
                    self.peek().line == self.tokens[self.pos-1].line:
                      self.consume(TokenType.SYMBOL, '[')
                      index = self.parse_expr()
                      self.consume(TokenType.SYMBOL, ']')
                      node = ArrayAccess(node, index)
                      continue
                 
                 break

            # Check for special method calls like print: arg
            # Check BEFORE generic method call to avoid 'print: var' being parsed as 'print.var()'
            if self.peek().value == ':' and isinstance(node, Variable) and node.name == 'print':
                self.consume(TokenType.SYMBOL, ':')
                args = []
                args.append(self.parse_expr())
                while self.peek().value == ',':
                    self.consume(TokenType.SYMBOL, ',')
                    if self.peek().value not in [';', '.']:
                        args.append(self.parse_expr())
                return MethodCall(None, 'print', args)

            # Check for Method Call 'Target : Method : args'
            if self.peek().value == ':' and self.pos + 1 < len(self.tokens) and \
               self.tokens[self.pos+1].type == TokenType.IDENT and \
               self.peek().line == self.tokens[self.pos-1].line and \
               self.tokens[self.pos+1].line == self.peek().line:
                  self.consume(TokenType.SYMBOL, ':')
                  method_name = self.consume(TokenType.IDENT).value
                  args = []
                  if self.peek().value == ':': # Args?
                       if self.pos+1 < len(self.tokens) and self.tokens[self.pos+1].line == self.peek().line:
                            self.consume(TokenType.SYMBOL, ':')
                            args.append(self.parse_expr())
                            while self.peek().value == ',':
                                 print(f"DEBUG: Found comma in args. Next: {self.tokens[self.pos+1]}", flush=True)
                                 self.consume(TokenType.SYMBOL, ',')
                                 if not (self.peek().type == TokenType.SYMBOL and self.peek().value in [';', '.']):
                                      args.append(self.parse_expr())
                  return MethodCall(node, method_name, args)
            
            # Check for function call (lambda call) fn: args
            if self.peek().value == ':' and self.pos + 1 < len(self.tokens) and \
               self.tokens[self.pos+1].type != TokenType.IDENT and \
               (self.tokens[self.pos+1].type != TokenType.KEYWORD or self.tokens[self.pos+1].value in ['nil', 'true', 'false', 'try']) and \
               self.peek().line == self.tokens[self.pos-1].line:
                  self.consume(TokenType.SYMBOL, ':')
                  args = []
                  args.append(self.parse_expr())
                  while self.peek().value == ',':
                       self.consume(TokenType.SYMBOL, ',')
                       if self.peek().value not in [';', '.']:
                            args.append(self.parse_expr())
                  return MethodCall(node, '', args)

            return node
        if token.type == TokenType.KEYWORD and token.value == 'foreign':
             self.consume()
             # Check for foreign call in expr
             if self.peek().value == ':' and self.pos + 1 < len(self.tokens):
                 self.consume(TokenType.SYMBOL, ':')
                 method_name = self.consume(TokenType.IDENT).value
                 args = []
                 if self.peek().value == ':':
                      if self.pos+1 < len(self.tokens) and self.tokens[self.pos+1].line == token.line:
                           self.consume(TokenType.SYMBOL, ':')
                           args.append(self.parse_expr())
                           while self.peek().value == ',':
                                self.consume(TokenType.SYMBOL, ',')
                                if self.peek().value not in [';', '.']:
                                     args.append(self.parse_expr())
                 return MethodCall(Variable("foreign"), method_name, args)
             return Variable("foreign")
        # Lambda: \param. body or \x, y. expression
        if token.value == '\\':
            return self.parse_lambda()
        if token.value == '(':
            self.consume(TokenType.SYMBOL, '(')
            expr = self.parse_expr()
            self.consume(TokenType.SYMBOL, ')')
            return GroupingExpr(expr)
        # Array Literal: [1, 2, 3]
        if token.value == '[':
            return self.parse_array_literal()
        raise SyntaxError(f"Unexpected token in expr: {token}")

    def parse_array_literal(self) -> ArrayLiteral:
        self.consume(TokenType.SYMBOL, '[')
        elements = []
        
        # Empty array
        if self.peek().value == ']':
             self.consume(TokenType.SYMBOL, ']')
             return ArrayLiteral(elements)
        
        # Parse elements
        elements.append(self.parse_expr())
        while self.peek().value == ',':
             self.consume(TokenType.SYMBOL, ',')
             if self.peek().value != ']': # Allow trailing comma
                 elements.append(self.parse_expr())
                 
        self.consume(TokenType.SYMBOL, ']')
        return ArrayLiteral(elements)

# ==========================================
# 4. CODE GEN (C11)
# ==========================================

class CTranspiler:
    def __init__(self):
        self.code = []
        self.entities = {}
        self.traits = {} # Map trait_name -> TraitDecl
        self.headers = [
            "#include <stdio.h>", "#include <stdlib.h>", "#include <stdbool.h>", "#include <pthread.h>",
            "#include <unistd.h>", "#include <string.h>", "#include <math.h>", "#include <ctype.h>",
            "#include <setjmp.h>", "#include <stdint.h>", "#include <time.h>",
            "typedef struct { void* obj; void* vtable; } BunkerTraitObject;",
            "typedef FILE __BunkerFile;",
            "typedef struct { long long value; char* errorMessage; bool isError; } BunkerResult;",
            "// Exception Handling",
            "typedef struct { char* message; } BunkerException;",
            "static jmp_buf* current_env = NULL;",
            "static BunkerException global_exception = {0};",
            "// Function pointer type for lambdas",
            "typedef void* (*BunkerFunction)(void*, ...);",
            "// Closure wrapper: holds function + environment",
            "typedef struct { BunkerFunction fn; void* env; } BunkerClosure;",
            "// Array struct",
            "typedef struct { void** data; size_t length; size_t capacity; } BunkerArray;",
            "// GC Runtime",
            "typedef struct GC_Header {",
            "    uint32_t magic;",
            "    uint32_t marked;",
            "    size_t size;",
            "    void* forwarding;",
            "    struct GC_Header* next;",
            "} GC_Header;",
            "",
            "#define GC_MAGIC 0x88BDB88B",
            "",
            "static GC_Header* gc_allocations = NULL;",
            "static size_t gc_old_gen_count = 0;",
            "static size_t gc_old_gen_threshold = 500000; // Lowered threshold",
            "void* gc_stack_bottom = NULL;",
            "",
            "static void** gc_mark_stack = NULL;",
            "static size_t gc_mark_stack_top = 0;",
            "static size_t gc_mark_stack_cap = 0;",
            "",
            "// Generational GC: Nursery (Semi-space)",
            "static char* nursery_start = NULL;",
            "static char* nursery_top = NULL;",
            "static char* nursery_end = NULL;",
            "#define NURSERY_SIZE (128 * 1024 * 1024) // 128MB",
            "",
            "#define PAGE_SHIFT 12",
            "#define PAGE_MAP_SIZE 131072 // 128K entries",
            "static uintptr_t gc_page_map[PAGE_MAP_SIZE];",
            "",
            "static void gc_register_pages(void* ptr, size_t size) {",
            "    if (!ptr) return;",
            "    uintptr_t start = (uintptr_t)ptr >> PAGE_SHIFT;",
            "    uintptr_t end = ((uintptr_t)ptr + size - 1) >> PAGE_SHIFT;",
            "    for (uintptr_t p = start; p <= end; p++) {",
            "        uintptr_t h = (p * 11400714819323198485ULL) & (PAGE_MAP_SIZE - 1);",
            "        while (gc_page_map[h] != 0 && gc_page_map[h] != p) {",
            "            h = (h + 1) & (PAGE_MAP_SIZE - 1);",
            "        }",
            "        gc_page_map[h] = p;",
            "    }",
            "}",
            "",
            "static int gc_is_bunker_page(void* ptr) {",
            "    uintptr_t p = (uintptr_t)ptr >> PAGE_SHIFT;",
            "    if (p == 0) return 0;",
            "    uintptr_t h = (p * 11400714819323198485ULL) & (PAGE_MAP_SIZE - 1);",
            "    while (gc_page_map[h] != 0) {",
            "        if (gc_page_map[h] == p) return 1;",
            "        h = (h + 1) & (PAGE_MAP_SIZE - 1);",
            "    }",
            "    return 0;",
            "}",
            "",
            "// Prototypes",
            "void gc_scan_region(void* start, void* end);",
            "void gc_mark(void* ptr);",
            "void gc_collect();",
            "void* gc_evacuate(void* ptr);",
            "void gc_process_marks();",
            "void gc_push_mark(void* ptr);",
            "int gc_is_bunker_object(void* ptr);",
            "",
            "int gc_is_bunker_object(void* ptr) {",
            "    if (!ptr || ((uintptr_t)ptr & (sizeof(void*)-1))) return 0;",
            "    if ((char*)ptr >= nursery_start && (char*)ptr < nursery_end) return 1;",
            "    ",
            "    if (!gc_is_bunker_page(ptr)) return 0;",
            "    ",
            "    GC_Header* h = (GC_Header*)ptr - 1;",
            "    if (h->magic != GC_MAGIC) return 0;",
            "    return 1;",
            "}",
            "",
            "void gc_push_mark(void* ptr) {",
            "    if (gc_mark_stack_top >= gc_mark_stack_cap) {",
            "        gc_mark_stack_cap = gc_mark_stack_cap ? gc_mark_stack_cap * 2 : 1024;",
            "        gc_mark_stack = (void**)realloc(gc_mark_stack, gc_mark_stack_cap * sizeof(void*));",
            "    }",
            "    gc_mark_stack[gc_mark_stack_top++] = ptr;",
            "}",
            "",
            "void gc_process_marks() {",
            "    while (gc_mark_stack_top > 0) {",
            "        void* ptr = gc_mark_stack[--gc_mark_stack_top];",
            "        GC_Header* h = (GC_Header*)ptr - 1;",
            "        gc_scan_region(ptr, (void*)((char*)ptr + h->size));",
            "    }",
            "}",
            "",
            "void* gc_evacuate(void* ptr) {",
            "    if (!ptr || ((uintptr_t)ptr & (sizeof(void*)-1)) || (char*)ptr < nursery_start || (char*)ptr >= nursery_end) return ptr;",
            "    ",
            "    GC_Header* h = (GC_Header*)ptr - 1;",
            "    if (!gc_is_bunker_object(ptr)) return ptr;",
            "    if (h->forwarding) return h->forwarding;",
            "",
            "    // Move to Old Generation",
            "    size_t total_size = sizeof(GC_Header) + h->size;",
            "    GC_Header* new_h = (GC_Header*)calloc(1, total_size);",
            "    if (!new_h) return ptr;",
            "    new_h->magic = GC_MAGIC;",
            "    new_h->marked = 0;",
            "    new_h->size = h->size;",
            "    new_h->forwarding = NULL;",
            "    new_h->next = gc_allocations;",
            "    gc_allocations = new_h;",
            "    gc_old_gen_count++;",
            "    ",
            "    void* new_ptr = (void*)(new_h + 1);",
            "    gc_register_pages(new_h, total_size);",
            "    ",
            "    h->forwarding = new_ptr;",
            "    memcpy(new_ptr, ptr, h->size);",
            "    ",
            "    if (getenv(\"DEBUG_GC\")) printf(\"DEBUG GC: Promoted %p to %p (size %zu)\\n\", ptr, new_ptr, h->size);",
            "    ",
            "    return new_ptr;",
            "}",
            "",
            "void gc_scan_region(void* start, void* end) {",
            "    if (start > end) { void* tmp = start; start = end; end = tmp; }",
            "    char* p = (char*)((uintptr_t)start & ~(uintptr_t)(sizeof(void*)-1));",
            "    char* p_end = (char*)end;",
            "    while (p + sizeof(void*) <= p_end) {",
            "        void** slot = (void**)p;",
            "        *slot = gc_evacuate(*slot);",
            "        gc_mark(*slot);",
            "        p += sizeof(void*);",
            "    }",
            "}",
            "",
            "void gc_mark(void* ptr) {",
            "    if (!gc_is_bunker_object(ptr)) return;",
            "    if ((char*)ptr >= nursery_start && (char*)ptr < nursery_end) return;",
            "    ",
            "    GC_Header* h = (GC_Header*)ptr - 1;",
            "    if (!h->marked) {",
            "        h->marked = 1;",
            "        gc_push_mark(ptr);",
            "    }",
            "}",
            "",
            "void gc_minor_collect() {",
            "    if (getenv(\"DEBUG_GC\")) printf(\"DEBUG GC: Starting Minor Collection (Nursery -> Old)\\n\");",
            "    ",
            "    jmp_buf env;",
            "    setjmp(env);",
            "    __builtin_unwind_init();",
            "    volatile void* stack_top = &stack_top;",
            "    ",
            "    // We don't mark old generation objects yet, just evacuate nursery ones",
            "    // But since we are moving things, we MUST scan the stack and update pointers.",
            "    gc_scan_region((void*)stack_top, gc_stack_bottom);",
            "    gc_scan_region((void*)env, (void*)((char*)env + sizeof(jmp_buf)));",
            "    ",
            "    // Also scan all Old Generation objects because they might point into the nursery",
            "    GC_Header* h = gc_allocations;",
            "    while (h) {",
            "        void* obj_ptr = (void*)(h + 1);",
            "        gc_scan_region(obj_ptr, (void*)((char*)obj_ptr + h->size));",
            "        h = h->next;",
            "    }",
            "    gc_process_marks();",
            "    ",
            "    // Reset nursery",
            "    nursery_top = nursery_start;",
            "    if (getenv(\"DEBUG_GC\")) printf(\"DEBUG GC: Minor Collection Complete\\n\");",
            "}",
            "",
            "void gc_free(void* ptr) {",
            "    if (!ptr) return;",
            "    GC_Header* h = (GC_Header*)ptr - 1;",
            "    GC_Header** prev = &gc_allocations;",
            "    GC_Header* curr = gc_allocations;",
            "    while (curr) {",
            "        if (curr == h) {",
            "            *prev = curr->next;",
            "            if (getenv(\"DEBUG_GC\")) printf(\"DEBUG GC: Explicit Free %p\\n\", ptr);",
            "            free(curr);",
            "            return;",
            "        }",
            "        prev = &curr->next;",
            "        curr = curr->next;",
            "    }",
            "}",
            "",
            "void gc_collect() {",
            "    jmp_buf env;",
            "    setjmp(env);",
            "    __builtin_unwind_init();",
            "    volatile void* stack_top = &stack_top;",
            "    ",
            "    // Mark",
            "    gc_scan_region((void*)stack_top, gc_stack_bottom);",
            "    gc_scan_region((void*)env, (void*)((char*)env + sizeof(jmp_buf)));",
            "    gc_process_marks();",
            "    ",
            "    // Sweep",
            "    int collected = 0;",
            "    int kept = 0;",
            "    memset(gc_page_map, 0, sizeof(gc_page_map)); /* Rebuild Page Map */",
            "    GC_Header** node = &gc_allocations;",
            "    while (*node) {",
            "        GC_Header* curr = *node;",
            "        if (!curr->marked) {",
            "            *node = curr->next;",
            "            // if (getenv(\"DEBUG_GC\")) printf(\"DEBUG GC: Freeing %p (size %zu)\\n\", (void*)(curr+1), curr->size);",
            "            free(curr);",
            "            collected++;",
            "        } else {",
            "            curr->marked = 0;",
            "            gc_register_pages(curr, sizeof(GC_Header) + curr->size);",
            "            node = &(*node)->next;",
            "            kept++;",
            "        }",
            "    }",
            "    gc_old_gen_count = kept;",
            "    if (getenv(\"DEBUG_GC\")) printf(\"DEBUG GC: Collection done. Collected: %d, Kept: %d\\n\", collected, kept);",
            "}",
            "",
             "void* gc_alloc(size_t size) {",
            "    if (size < sizeof(void*)) size = sizeof(void*);",
            "    size = (size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);",
            "    size_t total_size = sizeof(GC_Header) + size;",
            "",
            "    if (nursery_top + total_size <= nursery_end) {",
            "        GC_Header* h = (GC_Header*)nursery_top;",
            "        h->magic = GC_MAGIC;",
            "        h->marked = 0;",
            "        h->size = size;",
            "        h->forwarding = NULL;",
            "        h->next = (GC_Header*)0x1;",
            "        nursery_top += total_size;",
            "        return (void*)(h + 1);",
            "    }",
            "",
            "    if (gc_old_gen_count > gc_old_gen_threshold) {",
            "        gc_collect();",
            "        gc_old_gen_threshold = gc_old_gen_count + (gc_old_gen_count / 2) + 10000;",
            "    }",
            "",
            "    gc_minor_collect();",
            "",
            "    if (total_size > NURSERY_SIZE) {",
            "        GC_Header* h = (GC_Header*)calloc(1, total_size);",
            "        if (!h) return NULL;",
            "        h->magic = GC_MAGIC;",
            "        h->marked = 0;",
            "        h->size = size;",
            "        h->forwarding = NULL;",
            "        h->next = gc_allocations;",
            "        gc_allocations = h;",
            "        gc_old_gen_count++;",
            "        gc_register_pages(h, total_size);",
            "        return (void*)(h + 1);",
            "    }",
            "",
            "    GC_Header* h = (GC_Header*)nursery_top;",
            "    h->magic = GC_MAGIC;",
            "    h->marked = 0;",
            "    h->size = size;",
            "    h->forwarding = NULL;",
            "    h->next = (GC_Header*)0x1;",
            "    nursery_top += total_size;",
            "    return (void*)(h + 1);",
            "}",
            "",
            "void* gc_get_stack_base() {",
            "    return pthread_get_stackaddr_np(pthread_self());",
            "}",
            "",
            "#define GC_INIT() { \\",
            "    gc_stack_bottom = gc_get_stack_base(); \\",
            "    nursery_start = (char*)malloc(NURSERY_SIZE); \\",
            "    nursery_top = nursery_start; \\",
            "    nursery_end = nursery_start + NURSERY_SIZE; \\",
            "    (void)current_env; (void)global_exception; \\",
            "}",
            "",
            "// Helper: Create Array",
            "BunkerArray* Bunker_make_array(size_t count) {",
            "    BunkerArray* arr = gc_alloc(sizeof(BunkerArray));",
            "    arr->length = count;",
            "    arr->capacity = count > 0 ? count : 4;",
            "    arr->data = gc_alloc(sizeof(void*) * arr->capacity);",
            "    return arr;",
            "}",
            "",
            "// Helper: Array Map",
            "BunkerArray* Bunker_Array_map(BunkerArray* self, BunkerClosure* fn) {",
            "    BunkerArray* result = Bunker_make_array(self->length);",
            "    for (size_t i = 0; i < self->length; i++) {",
            "        // fn signature: void* (*)(void* env, void* arg)",
            "        result->data[i] = fn->fn(fn->env, self->data[i]);",
            "    }",
            "    return result;",
            "}",
            "",
            "// Helper: Array Filter",
            "BunkerArray* Bunker_Array_filter(BunkerArray* self, BunkerClosure* predicate) {",
            "    BunkerArray* result = Bunker_make_array(self->length); // Worst case size",
            "    size_t count = 0;",
            "    for (size_t i = 0; i < self->length; i++) {",
            "        // predicate returns bool (which is int/long long in C usually, or strictly bool)",
            "        // If we cast to intptr_t we can check truthiness",
            "        void* res = predicate->fn(predicate->env, self->data[i]);",
            "        if ((long long)(intptr_t)res != 0) {",
            "            result->data[count++] = self->data[i];",
            "        }",
            "    }",
            "    result->length = count; // Resize?",
            "    return result;",
            "}",
            "",
            "// Helper: Array Reduce",
            "void* Bunker_Array_reduce(BunkerArray* self, void* initial, BunkerClosure* fn) {",
            "    void* acc = initial;",
            "    for (size_t i = 0; i < self->length; i++) {",
            "        acc = fn->fn(fn->env, acc, self->data[i]);",
            "    }",
            "    return acc;",
            "}",
            "// Helper: String + Int",
            "char* bunker_str_add_int(char* s, long long i) {",
            "    int len = strlen(s);",
            "    int needed = len + 22; // max int64 digits + sign + null",
            "    char* buffer = gc_alloc(needed);",
            "    sprintf(buffer, \"%s%lld\", s, i);",
            "    return buffer;",
            "}",
            "",
            "// Helper: String + Double",
            "char* bunker_str_add_double(char* s, double d) {",
            "    int len = strlen(s);",
            "    int needed = len + 50; ",
            "    char* buffer = gc_alloc(needed);",
            "    sprintf(buffer, \"%s%g\", s, d);",
            "    return buffer;",
            "}",
            "",
            "// Helper: String Concatenation",
            "char* BNK_RT_Strings_concat(void* _self, char* s1, char* s2) {",
            "    if (!s1) s1 = \"\";",
            "    if (!s2) s2 = \"\";",
            "    size_t len1 = strlen(s1);",
            "    size_t len2 = strlen(s2);",
            "    char* result = gc_alloc(len1 + len2 + 1);",
            "    strcpy(result, s1);",
            "    strcat(result, s2);",
            "    return result;",
            "}",
            "",
            "// Helper: Strings.split",
            "BunkerArray* BNK_RT_Strings_split(void* _self, char* str, char* delimiter) {",
            "    if (!str || !delimiter) return Bunker_make_array(0);",
            "    ",
            "    // Copy string because strtok modifies it",
            "    char* s = gc_alloc(strlen(str) + 1);",
            "    strcpy(s, str);",
            "    ",
            "    // First pass: count tokens",
            "    int count = 0;",
            "    char* tmp = gc_alloc(strlen(str) + 1);",
            "    strcpy(tmp, str);",
            "    char* token = strtok(tmp, delimiter);",
            "    while (token) {",
            "        count++;",
            "        token = strtok(NULL, delimiter);",
            "    }",
            "    ",
            "    BunkerArray* arr = Bunker_make_array(count);",
            "    ",
            "    // Second pass: fill array",
            "    token = strtok(s, delimiter);",
            "    int i = 0;",
            "    while (token) {",
            "        char* item = gc_alloc(strlen(token) + 1);",
            "        strcpy(item, token);",
            "        arr->data[i++] = item;",
            "        token = strtok(NULL, delimiter);",
            "    }",
            "    return arr;",
            "}",
            "",
            "// Helper: Strings.toInt",
            "long long BNK_RT_Strings_toInt(void* _self, char* s) {",
            "    return atoll(s);",
            "}",
            "",
            "// Helper: Strings.toFloat",
            "double BNK_RT_Strings_toFloat(void* _self, char* s) {",
            "    return atof(s);",
            "}",
            "",
            "// Helper: Strings.length",
            "long long BNK_RT_Strings_length(void* _self, char* s) {",
            "    return s ? strlen(s) : 0;",
            "}",
            "",
            "// Helper: Strings.substring",
            "char* BNK_RT_Strings_substring(void* _self, char* s, long long start, long long end) {",
            "    if (!s) return \"\";",
            "    long long len = strlen(s);",
            "    if (start < 0) start = 0;",
            "    if (end > len) end = len;",
            "    if (start >= end) return \"\";",
            "    long long substr_len = end - start;",
            "    char* result = gc_alloc(substr_len + 1);",
            "    strncpy(result, s + start, substr_len);",
            "    result[substr_len] = '\\0';",
            "    return result;",
            "}",
            "",
            "// Helper: Strings.startsWith",
            "bool BNK_RT_Strings_startsWith(void* _self, char* s, char* prefix) {",
            "    if (!s || !prefix) return false;",
            "    return strncmp(s, prefix, strlen(prefix)) == 0;",
            "}",
            "",
            "// Helper: IO.readFileLines",
            "BunkerArray* IO_readFileLines(void* _self, char* path) {",
            "    FILE* f = fopen(path, \"r\");",
            "    if (!f) return Bunker_make_array(0);",
            "    ",
            "    // Pre-allocate list logic (dynamic resize would be better but simple array for now)",
            "    // Count lines first",
            "    int lines = 0;",
            "    char ch;",
            "    while(!feof(f)) {",
            "        ch = fgetc(f);",
            "        if(ch == '\\n') lines++;",
            "    }",
            "    lines++; // Last line might not end in newline",
            "    rewind(f);",
            "    ",
            "    BunkerArray* arr = Bunker_make_array(lines);",
            "    int i = 0;",
            "    char buffer[4096];",
            "    while (fgets(buffer, sizeof(buffer), f)) {",
            "        // Remove newline",
            "        buffer[strcspn(buffer, \"\\n\")] = 0;",
            "        char* line = gc_alloc(strlen(buffer) + 1);",
            "        strcpy(line, buffer);",
            "        if (i < lines) arr->data[i++] = line;",
            "    }",
            "    fclose(f);",
            "    arr->length = i; // Adjust actual count",
            "    return arr;",
            "}",
            "",
            "// Helper: IO.input",
            "char* IO_input(void* _self, char* prompt) {",
            "    printf(\"%s\", prompt);",
            "    char buffer[4096];",
            "    if (fgets(buffer, sizeof(buffer), stdin)) {",
            "        buffer[strcspn(buffer, \"\\n\")] = 0;",
            "        char* res = gc_alloc(strlen(buffer) + 1);",
            "        strcpy(res, buffer);",
            "        return res;",
            "    }",
            "    return \"\";",
            "}",
            "",
            "// Helper: Math.random",
            "double Math_random(void* _self) {",
            "    return (double)rand() / (double)RAND_MAX;",
            "}",
            "",
            "// Helper: Math.min / max",
            "double Math_min(void* _self, double a, double b) { return a < b ? a : b; }",
            "double Math_max(void* _self, double a, double b) { return a > b ? a : b; }",
            "",
            "// End of helpers"
        ]
        self.indent_level = 0
        self.vars = {} # name -> c_type
        self.foreign_func_types = {}
        self.defer_stack = [] # Stack of deferred statements for current scope

    def emit(self, line: str):
        indent = "    " * self.indent_level
        self.code.append(f"{indent}{line}")

    def mangle_type_name(self, t: str) -> str:
        if '<' in t:
             base = t.split('<')[0]
             args = t[len(base)+1:-1]
             flat = args.replace('<','_').replace('>','_').replace(',','_').replace(' ','')
             res = f"{base}_{flat}"
             # print(f"DEBUG MANGLE: {t} -> {res}")
             return res
        return t

    def map_type(self, t: str) -> str:
        if t == 'int': return "long long"
        if t == 'float': return "double"
        if t in ['string', 'str']: return "char*"
        if t == 'bool': return "bool"
        if t in ['void', 'nil']: return "void" 
        if t == 'void*': return "void*"
        if t == 'function' or t.startswith('function<'): return "BunkerClosure*"  # Lambda/closure pointer
        if t.startswith('Array<'): return "BunkerArray*"
        if t.startswith('*'): 
             # Pointer type: *int -> long long*
             sub = t[1:]
             return f"{self.map_type(sub)}*"
        if t == 'unknown': return "void*" # Fallback
        if t in self.traits: return "BunkerTraitObject"
        
        mangled = self.mangle_type_name(t)
        return f"{mangled}*"


    def transpile_struct_decl(self, node: StructDecl):
        # 1. Emit struct definition (typedef already emitted in forward decls)
        self.emit(f"struct {node.name} {{")
        self.indent_level += 1
        for field in node.fields:
             c_type = self.map_type(field.type)
             self.emit(f"{c_type} {field.name};")
        self.indent_level -= 1
        self.emit(f"}};")
        
        # 2. Emit Constructor using static method convention: Name_new
        args_str_list = []
        for field in node.fields:
             c_type = self.map_type(field.type)
             args_str_list.append(f"{c_type} {field.name}")
        
        # Dummy "self" arg for static dispatch consistency
        params = ["void* _dummy"] + args_str_list
        params_str = ", ".join(params)
        
        self.emit(f"void* {node.name}_new({params_str}) {{")
        self.indent_level += 1
        self.emit(f"{node.name}* _inst = gc_alloc(sizeof({node.name}));")
        if node.name == 'Token':
             self.emit('if (getenv("DEBUG_TOKEN")) printf("DEBUG: Token_new: ptr=%p type=%s val_ptr=%p val=%s\\n", (void*)_inst, type, (void*)value, value);')
        for field in node.fields:
             self.emit(f"_inst->{field.name} = {field.name};")
        self.emit("return _inst;")
        self.indent_level -= 1
        self.emit("}")

    def _find_captured_vars(self, node: Node, param_names: set) -> set:
        """Find all variables used in node that aren't in param_names or self.vars at lambda creation time"""
        captured = set()
        
        if isinstance(node, Variable):
            # If it's not a parameter and it exists in current scope, it's captured
            if node.name not in param_names and node.name in self.vars:
                captured.add(node.name)
        elif isinstance(node, BinaryExpr):
            captured.update(self._find_captured_vars(node.left, param_names))
            captured.update(self._find_captured_vars(node.right, param_names))
        elif isinstance(node, UnaryExpr):
            captured.update(self._find_captured_vars(node.expr, param_names))
        elif isinstance(node, MethodCall):
            if node.target:
                captured.update(self._find_captured_vars(node.target, param_names))
            for arg in node.args:
                captured.update(self._find_captured_vars(arg, param_names))
        elif isinstance(node, FieldAccess):
            captured.update(self._find_captured_vars(node.target, param_names))
        # Add more node types as needed
        
        return captured


    def transpile_foreign_decl(self, node: ForeignDecl):
        for func in node.funcs:
            # Record type for expression resolution
            self.foreign_func_types[func.name] = func.return_type
            
            c_ret = self.map_type(func.return_type)
            
            c_args = []
            for p_name, p_type in func.params:
                c_p_type = self.map_type(p_type)
                c_args.append(f"{c_p_type} {p_name}")
            
            args_str = ", ".join(c_args)
            args_str = ", ".join(c_args)
            sig = f"extern {c_ret} {func.real_name}({args_str});"
            if func.real_name in ['system', 'printf', 'malloc', 'free', 'exit']:
                pass # Don't redeclare standard functions
            elif sig not in self.headers:
                self.headers.append(sig)

    def emit_method_prototype(self, node: MethodDecl, entity_name: str):
        if node.name == 'main': return
        if entity_name in ['File', 'FileSystem', 'Strings', 'Math', 'Result', 'List', 'Map']: return

        ret_c_type = self.map_type(node.return_type) if node.return_type else "void"
        
        c_params = ["void* _self"]
        for p_name, p_type in node.params:
             p_c_type = self.map_type(p_type)
             c_params.append(f"{p_c_type} {p_name}")
        
        c_name = f"{entity_name}_{node.name}"
        self.emit(f"{ret_c_type} {c_name}({', '.join(c_params)});")

    def transpile(self, node: Node):
        if isinstance(node, Program):
            # Pass 0: Register all declared functions/structs?
            for stmt in node.stmts:
                if isinstance(stmt, EntityDecl):
                     self.entities[stmt.name] = stmt
                if isinstance(stmt, StructDecl):
                     self.entities[stmt.name] = stmt
            
            emitted_typedefs = set()
            emitted_structs = set()
            
            # Pass 0.2: Record Traits
            for stmt in node.stmts:
                if isinstance(stmt, TraitDecl):
                     self.traits[stmt.name] = stmt
            
            # Pass 0.3: Emit Trait VTable Typedefs & Structs
            for stmt in node.stmts:
                if isinstance(stmt, TraitDecl):
                     self.emit(f"typedef struct VT_{stmt.name} VT_{stmt.name};")
                     self.emit(f"struct VT_{stmt.name} {{")
                     self.indent_level += 1
                     for m in stmt.methods:
                          ret_type = self.map_type(m.return_type)
                          # Function pointer for method. First arg is always 'this' (void*)
                          param_types = ["void*"]
                          for _, p_type in m.params:
                               param_types.append(self.map_type(p_type))
                          self.emit(f"{ret_type} (*{m.name})({', '.join(param_types)});")
                     self.indent_level -= 1
                     self.emit("};")

            # Pass 0.5: Emit Entity Forward Typedefs
            for stmt in node.stmts:
                name = stmt.name
                if name in emitted_typedefs: continue
                
                if isinstance(stmt, EntityDecl):
                     self.emit(f"typedef struct {stmt.name} {stmt.name};")
                     emitted_typedefs.add(name)
                if isinstance(stmt, StructDecl):
                     self.emit(f"typedef struct {stmt.name} {stmt.name};")
                     emitted_typedefs.add(name)

            # Pass 1: Emit Struct Definitions and Foreign Decls
            for stmt in node.stmts:
                if isinstance(stmt, StructDecl):
                     if stmt.name in emitted_structs: continue
                     self.transpile_struct_decl(stmt)
                     emitted_structs.add(stmt.name)
                elif isinstance(stmt, ForeignDecl):
                     self.transpile_foreign_decl(stmt)
            
            # Pass 1.2: Emit Entity Struct Definitions
            for stmt in node.stmts:
                if isinstance(stmt, EntityDecl):
                     if stmt.name in emitted_structs: continue
                     
                     # Emit the actual struct definition
                     self.emit(f"struct {stmt.name} {{")
                     self.indent_level += 1
                     for member in stmt.members:
                          if isinstance(member, FieldDecl):
                               c_type = self.map_type(member.type)
                               self.emit(f"{c_type} {member.name};")
                     self.indent_level -= 1
                     self.emit(f"}};")
                     emitted_structs.add(stmt.name)
            
            emitted_prototypes = set()
            # Pass 1.5: Emit Method Prototypes for Entities
            for stmt in node.stmts:
                if isinstance(stmt, EntityDecl):
                     for member in stmt.members:
                          if isinstance(member, MethodDecl):
                               # Dedup prototypes? Methods are usually unique per Entity.
                               # But if Entity is duplicated?
                               # We already deduped the Entity Struct.
                               # But we iterate stmts again.
                               proto_key = f"{stmt.name}_{member.name}"
                               if proto_key in emitted_prototypes: continue
                               
                               self.emit_method_prototype(member, stmt.name)
                               emitted_prototypes.add(proto_key)

            # Pass 1.6: Emit VTable instances for Entities
            for stmt in node.stmts:
                if isinstance(stmt, EntityDecl) and hasattr(stmt, 'implements') and stmt.implements:
                     for t_name in stmt.implements:
                          if t_name in self.traits:
                               trait = self.traits[t_name]
                               vt_name = f"VT_{stmt.name}_{t_name}"
                               self.emit(f"static VT_{t_name} {vt_name} = {{")
                               self.indent_level += 1
                               for tm in trait.methods:
                                    self.emit(f".{tm.name} = (void*){stmt.name}_{tm.name},")
                               self.indent_level -= 1
                               self.emit("};")

            # Pass 2: Emit Implementations
            emitted_impls = set()
            idx = 0
            while idx < len(node.stmts):
                stmt = node.stmts[idx]
                idx += 1
                if not isinstance(stmt, StructDecl) and not isinstance(stmt, ForeignDecl):
                    if isinstance(stmt, EntityDecl):
                         if stmt.name in emitted_impls: continue
                         emitted_impls.add(stmt.name)
                    
                    if hasattr(stmt, "name"): print(f"DEBUG: Loop transpiling {stmt.name} ({type(stmt).__name__})")
                    self.transpile(stmt)
            print("DEBUG: Program transpilation FINISHED", flush=True)
                
        elif isinstance(node, EntityDecl):
            if node.type_params:
                 return # Skip templates, they are handled via instantiate_template
            # Flatten methods
            for member in node.members:
                if isinstance(member, MethodDecl):
                    self.transpile_method(member, node.name)
        
        elif isinstance(node, MethodDecl):
            pass # Handled above

    def transpile_method(self, node: MethodDecl, entity_name: str):
        # Native handling for StdLib
        if False and entity_name == 'File':
            c_name = f"File_{node.name}"
            if node.name == 'open':
                 # File open: path -> result
                 self.emit("void* File_open(char* path) {")
                 self.emit("    FILE* f = fopen(path, \"r\");")
                 self.emit("    return f;") 
                 self.emit("}")
                 return
            elif node.name == 'create':
                 # File create -> opened for write
                 self.emit("void* File_create(char* path) {")
                 self.emit("    FILE* f = fopen(path, \"w\");")
                 self.emit("    return f;") 
                 self.emit("}")
                 return
            elif node.name == 'writeString':
                 # File writeString(f, str)
                 self.emit("void File_writeString(void* f, char* s) {")
                 self.emit("    if (f && s) {")
                 self.emit("        fputs(s, (FILE*)f);")
                 self.emit("        fflush((FILE*)f);")  # Flush immediately since close might not be called
                 self.emit("    }")
                 self.emit("}")
                 return
            elif node.name == 'readToString':
                 # File readToString -> str (malloc'd)
                 self.emit("char* File_readToString(void* f) {")
                 self.emit("    FILE* file = (FILE*)f;")
                 self.emit("    if (!file) return \"\";")
                 self.emit("    fseek(file, 0, SEEK_END);")
                 self.emit("    long length = ftell(file);")
                 self.emit("    fseek(file, 0, SEEK_SET);")
                 self.emit("    char* buffer = gc_alloc(length + 1);")
                 self.emit("    if (buffer) {")
                 self.emit("        fread(buffer, 1, length, file);")
                 self.emit("        buffer[length] = '\\0';")
                 self.emit("    } else { return \"\"; }")
                 self.emit("    return buffer;")
                 self.emit("}")
                 return
            elif node.name == 'close':
                 self.emit("void File_close(void* f) {")
                 self.emit("    if (f) fclose((FILE*)f);")
                 self.emit("}")
                 return

        elif entity_name == 'FileSystem':
            c_name = f"FileSystem_{node.name}"
            if node.name == 'exists':
                 self.emit("bool FileSystem_exists(void* _self, char* path) {")
                 self.emit("    return access(path, F_OK) != -1;")
                 self.emit("}")
                 return
            elif node.name == 'delete':
                 self.emit("bool FileSystem_delete(void* _self, char* path) {")
                 self.emit("    return remove(path) == 0;")
                 self.emit("}")
                 return

        elif False and entity_name == 'Strings':
            c_name = f"Strings_{node.name}"
            if node.name == 'concat':
                 self.emit("char* Strings_concat(void* _self, char* s1, char* s2) {")
                 self.emit("    size_t len1 = strlen(s1);")
                 self.emit("    size_t len2 = strlen(s2);")
                 self.emit("    char* result = gc_alloc(len1 + len2 + 1);")
                 self.emit("    strcpy(result, s1);")
                 self.emit("    strcat(result, s2);")
                 self.emit("    return result;")
                 self.emit("}")
                 return
            elif node.name == 'length':
                 self.emit("long long Strings_length(void* _self, char* s) {")
                 self.emit("    return (long long)strlen(s);")
                 self.emit("}")
                 return
            elif node.name == 'substring':
                 self.emit("char* Strings_substring(void* _self, char* s, long long start, long long end) {")
                 self.emit("    size_t len = strlen(s);")
                 self.emit("    if (start < 0) start = 0;")
                 self.emit("    if (end > len) end = len;")
                 self.emit("    if (start >= end) return strdup(\"\");")
                 self.emit("    size_t sub_len = end - start;")
                 self.emit("    char* result = gc_alloc(sub_len + 1);")
                 self.emit("    strncpy(result, s + start, sub_len);")
                 self.emit("    result[sub_len] = '\\0';")
                 self.emit("    return result;")
                 self.emit("}")
                 return
            elif node.name == 'startsWith':
                 self.emit("bool Strings_startsWith(void* _self, char* s, char* prefix) {")
                 self.emit("    return strncmp(s, prefix, strlen(prefix)) == 0;")
                 self.emit("}")
                 return
            elif node.name == 'endsWith':
                 self.emit("bool Strings_endsWith(void* _self, char* s, char* suffix) {")
                 self.emit("    size_t s_len = strlen(s);")
                 self.emit("    size_t suffix_len = strlen(suffix);")
                 self.emit("    if (suffix_len > s_len) return false;")
                 self.emit("    return strcmp(s + (s_len - suffix_len), suffix) == 0;")
                 self.emit("}")
                 return

        elif entity_name == 'List':
            if node.name == 'create':
                 self.emit("void* List_create(void* _self) {")
                 self.emit("    typedef struct { long long* data; size_t size; size_t capacity; } BunkerList;")
                 self.emit("    BunkerList* list = malloc(sizeof(BunkerList));")
                 self.emit("    list->data = malloc(sizeof(long long) * 4);")
                 self.emit("    list->size = 0;")
                 self.emit("    list->capacity = 4;")
                 self.emit("    return list;")
                 self.emit("}")
                 return
            elif node.name == 'append':
                 self.emit("void List_append(void* _list, long long value) {")
                 self.emit("    typedef struct { long long* data; size_t size; size_t capacity; } BunkerList;")
                 self.emit("    BunkerList* list = (BunkerList*)_list;")
                 self.emit("    if (list->size >= list->capacity) {")
                 self.emit("        list->capacity *= 2;")
                 self.emit("        list->data = realloc(list->data, sizeof(long long) * list->capacity);")
                 self.emit("    }")
                 self.emit("    list->data[list->size++] = value;")
                 self.emit("}")
                 return
            elif node.name == 'get':
                 self.emit("long long List_get(void* _list, long long index) {")
                 self.emit("    typedef struct { long long* data; size_t size; size_t capacity; } BunkerList;")
                 self.emit("    BunkerList* list = (BunkerList*)_list;")
                 self.emit("    if (index < 0 || index >= list->size) return -1;")
                 self.emit("    return list->data[index];")
                 self.emit("}")
                 return
            elif node.name == 'size':
                 self.emit("long long List_size(void* _list) {")
                 self.emit("    typedef struct { long long* data; size_t size; size_t capacity; } BunkerList;")
                 self.emit("    BunkerList* list = (BunkerList*)_list;")
                 self.emit("    return (long long)list->size;")
                 self.emit("}")
                 return
            elif node.name == 'remove':
                 self.emit("void List_remove(void* _list, long long index) {")
                 self.emit("    typedef struct { long long* data; size_t size; size_t capacity; } BunkerList;")
                 self.emit("    BunkerList* list = (BunkerList*)_list;")
                 self.emit("    if (index < 0 || index >= list->size) return;")
                 self.emit("    for (size_t i = index; i < list->size - 1; i++) {")
                 self.emit("        list->data[i] = list->data[i + 1];")
                 self.emit("    }")
                 self.emit("    list->size--;")
                 self.emit("}")
                 return

        elif entity_name == 'Map':
            if node.name == 'create':
                 self.emit("void* Map_create(void* _self) {")
                 self.emit("    typedef struct { char** keys; long long* values; size_t size; size_t capacity; } BunkerMap;")
                 self.emit("    BunkerMap* map = malloc(sizeof(BunkerMap));")
                 self.emit("    map->keys = malloc(sizeof(char*) * 4);")
                 self.emit("    map->values = malloc(sizeof(long long) * 4);")
                 self.emit("    map->size = 0;")
                 self.emit("    map->capacity = 4;")
                 self.emit("    return map;")
                 self.emit("}")
                 return
            elif node.name == 'put':
                 self.emit("void Map_put(void* _map, char* key, long long value) {")
                 self.emit("    typedef struct { char** keys; long long* values; size_t size; size_t capacity; } BunkerMap;")
                 self.emit("    BunkerMap* map = (BunkerMap*)_map;")
                 self.emit("    for (size_t i = 0; i < map->size; i++) {")
                 self.emit("        if (strcmp(map->keys[i], key) == 0) {")
                 self.emit("            map->values[i] = value;")
                 self.emit("            return;")
                 self.emit("        }")
                 self.emit("    }")
                 self.emit("    if (map->size >= map->capacity) {")
                 self.emit("        map->capacity *= 2;")
                 self.emit("        map->keys = realloc(map->keys, sizeof(char*) * map->capacity);")
                 self.emit("        map->values = realloc(map->values, sizeof(long long) * map->capacity);")
                 self.emit("    }")
                 self.emit("    map->keys[map->size] = strdup(key);")
                 self.emit("    map->values[map->size] = value;")
                 self.emit("    map->size++;")
                 self.emit("}")
                 return
            elif node.name == 'get':
                 self.emit("long long Map_get(void* _map, char* key) {")
                 self.emit("    typedef struct { char** keys; long long* values; size_t size; size_t capacity; } BunkerMap;")
                 self.emit("    BunkerMap* map = (BunkerMap*)_map;")
                 self.emit("    for (size_t i = 0; i < map->size; i++) {")
                 self.emit("        if (strcmp(map->keys[i], key) == 0) {")
                 self.emit("            return map->values[i];")
                 self.emit("        }")
                 self.emit("    }")
                 self.emit("    return -1;")
                 self.emit("}")
                 return
            elif node.name == 'contains':
                 self.emit("bool Map_contains(void* _map, char* key) {")
                 self.emit("    typedef struct { char** keys; long long* values; size_t size; size_t capacity; } BunkerMap;")
                 self.emit("    BunkerMap* map = (BunkerMap*)_map;")
                 self.emit("    for (size_t i = 0; i < map->size; i++) {")
                 self.emit("        if (strcmp(map->keys[i], key) == 0) {")
                 self.emit("            return true;")
                 self.emit("        }")
                 self.emit("    }")
                 self.emit("    return false;")
                 self.emit("}")
                 return
            elif node.name == 'remove':
                 self.emit("void Map_remove(void* _map, char* key) {")
                 self.emit("    typedef struct { char** keys; long long* values; size_t size; size_t capacity; } BunkerMap;")
                 self.emit("    BunkerMap* map = (BunkerMap*)_map;")
                 self.emit("    for (size_t i = 0; i < map->size; i++) {")
                 self.emit("        if (strcmp(map->keys[i], key) == 0) {")
                 self.emit("            free(map->keys[i]);")
                 self.emit("            for (size_t j = i; j < map->size - 1; j++) {")
                 self.emit("                map->keys[j] = map->keys[j + 1];")
                 self.emit("                map->values[j] = map->values[j + 1];")
                 self.emit("            }")
                 self.emit("            map->size--;")
                 self.emit("            return;")
                 self.emit("        }")
                 self.emit("    }")
                 self.emit("}")
                 return

        elif entity_name == 'Math':
            if node.name == 'abs':
                 self.emit("double Math_abs(void* _self, double x) {")
                 self.emit("    return fabs(x);")
                 self.emit("}")
                 return
            elif node.name == 'sqrt':
                 self.emit("double Math_sqrt(void* _self, double x) {")
                 self.emit("    return sqrt(x);")
                 self.emit("}")
                 return
            elif node.name == 'pow':
                 self.emit("double Math_pow(void* _self, double base, double exp) {")
                 self.emit("    return pow(base, exp);")
                 self.emit("}")
                 return
            elif node.name == 'sin':
                 self.emit("double Math_sin(void* _self, double x) {")
                 self.emit("    return sin(x);")
                 self.emit("}")
                 return
            elif node.name == 'cos':
                 self.emit("double Math_cos(void* _self, double x) {")
                 self.emit("    return cos(x);")
                 self.emit("}")
                 return

        elif entity_name == 'Result':
            if node.name == 'ok':
                 self.emit("void* Result_ok(void* _self, long long val) {")
                 self.emit("    BunkerResult* res = malloc(sizeof(BunkerResult));")
                 self.emit("    res->value = val;")
                 self.emit("    res->errorMessage = NULL;")
                 self.emit("    res->isError = false;")
                 self.emit("    return res;")
                 self.emit("}")
                 return
            elif node.name == 'fail':
                 self.emit("void* Result_fail(void* _self, char* msg) {")
                 self.emit("    BunkerResult* res = malloc(sizeof(BunkerResult));")
                 self.emit("    res->value = 0;")
                 self.emit("    res->errorMessage = strdup(msg);")
                 self.emit("    res->isError = true;")
                 self.emit("    return res;")
                 self.emit("}")
                 return

        c_name = f"{entity_name}_{node.name}"
        if node.name == 'main':
             c_name = 'main'
             self.emit("int main(int argc, char** argv) {")
             self.emit("    int _ret = 0;")
             self.emit("    volatile void* dummy; gc_stack_bottom = (void*)&dummy;")
             self.emit("    GC_INIT();")
        else:
             print(f"DEBUG: Transpiling {c_name}, body len: {len(node.body)}")
             if len(node.body) > 0:
                 print(f"DEBUG: First stmt type: {type(node.body[0])}")
                 pass
             
             # Map Bunker types to C types
             ret_c_type = self.map_type(node.return_type) if node.return_type else "void"
            
             # Build C parameter list (always include _self for Stage 0)
             c_params = ["void* _self"]
             for p_name, p_type in node.params:
                 p_c_type = self.map_type(p_type)
                 c_params.append(f"{p_c_type} {p_name}")
             
             self.emit(f"{ret_c_type} {c_name}({', '.join(c_params)}) {{")
        
        self.indent_level += 1
        
        # Reset vars for local scope and inject parameters
        self.vars = {}
        self.current_class = entity_name
        self.vars['self'] = entity_name
        self.current_return_type = node.return_type
        for p_name, p_type in node.params:
            self.vars[p_name] = p_type
            
        # Declare _ret if method has a return type (except main)
        if node.name != 'main' and node.return_type and node.return_type != 'void':
            ret_c_type = self.map_type(node.return_type)
            self.emit(f"{ret_c_type} _ret = 0;")
        
        if node.name != 'main' and entity_name:
             self.emit(f"struct {entity_name}* self = (struct {entity_name}*)_self;")
             self.emit("(void)self;")
        
        for stmt in node.body:
            self.transpile_stmt(stmt)
            
        # Emit defers for the method scope (LIFO)
        # Note: If we had returns inside, they should have handled their own defers.
        # This handles the fall-through case.
        while len(self.defer_stack) > 0:
            stmt = self.defer_stack.pop()
            self.emit("/* Deferred (Method Exit) */")
            self.transpile_stmt(stmt)
            
        self.emit("end:")
        self.emit("(void)&&end;")
        if node.name == 'main':
            self.emit("return _ret;")
        elif node.return_type and node.return_type != 'void':
            self.emit("return _ret;")
        elif node.return_type == 'void':
            self.emit("return;")
        else:
            self.emit("return 0;")
        
        self.indent_level -= 1
        self.emit("}")

    def transpile_stmt(self, node: Node):
        if isinstance(node, MethodCall):

            if node.method == 'print':
                arg = node.args[0]
                val_code, val_type = self.transpile_expr(arg)
                if val_type == 'int':
                    self.emit(f'printf("%lld\\n", (long long){val_code});')
                elif val_type in ['string', 'str']:
                    self.emit(f'printf("%s\\n", {val_code});')
                elif val_type == 'bool':
                    self.emit(f'printf("%s\\n", {val_code} ? "true" : "false");')
                elif val_type in ['float', 'double']:
                    self.emit(f'printf("%g\\n", {val_code});')
                elif val_type == 'nil':
                    self.emit('printf("nil\\n");')
                else:
                    self.emit(f'printf("Unknown type: %s\\n", "{val_type}");')
            
            # General Method Call: target_method(args)
            elif node.target:
                 # target : method : args
                 # Map to TargetType_method(target, args)
                 # Since we don't have full type info here, we rely on Entity naming convention
                 # If target is a variable 'f' of type 'File', we call File_method(f, ...)
                 
                 t_code, t_type = self.transpile_expr(node.target)
                  
                 # Foreign Call
                 if t_code == 'foreign':
                     arg_codes = [self.transpile_expr(arg)[0] for arg in node.args]
                     args_str = ", ".join(arg_codes)
                     self.emit(f"{node.method}({args_str});")
                     return

                 if t_type in self.traits:
                      # Trait dispatch
                      trait = self.traits[t_type]
                      tm = next((m for m in trait.methods if m.name == node.method), None)
                      if not tm: raise TypeError(f"Trait {t_type} has no method {node.method}")
                      ret_t = self.map_type(tm.return_type)
                      
                      arg_codes = []
                      for arg in node.args:
                           a_c, _ = self.transpile_expr(arg)
                           arg_codes.append(a_c)
                      args_str = ", ".join(arg_codes)
                      if args_str: args_str = ", " + args_str
                      
                      self.emit(f"(({ret_t} (*)(void*, ...))((VT_{t_type}*){t_code}.vtable)->{node.method})({t_code}.obj{args_str});")
                      return

                 # Resolve C function name
                 c_func = f"{self.mangle_type_name(t_type)}_{node.method}"
                 
                 # Args
                 arg_codes = []
                 # Check if this is a static call (target is type name) or instance call (target is variable)
                 # Static call: t_code == t_type and starts with uppercase (e.g., "App" == "App")
                 # Instance call: t_code is a variable name
                 if t_type[0].isupper() and t_code == t_type:
                     # Static call: use NULL as 'this'
                     arg_codes.append("NULL")
                     
                     # Memory (Manual alloc/free)
                     if t_type == 'Memory':
                         if node.method == 'free':
                             ptr_code, _ = self.transpile_expr(node.args[0])
                             self.emit(f"gc_free({ptr_code});")
                             return
                         if node.method == 'copy':
                             dest_code, _ = self.transpile_expr(node.args[0])
                             src_code, _ = self.transpile_expr(node.args[1])
                             size_code, _ = self.transpile_expr(node.args[2])
                             self.emit(f"memcpy({dest_code}, {src_code}, {size_code});")
                             return
                             
                 else:
                     # Instance call: use actual target as 'this'
                     arg_codes.append(t_code)
                 
                 for arg in node.args:
                     a_c, _ = self.transpile_expr(arg)
                     arg_codes.append(a_c)
                 args_str = ", ".join(arg_codes)
                 self.emit(f"{c_func}({args_str});")

            else:
                 # Self call or Global?
                 # Assume global or implicit this, skip for now unless it's a known global
                 pass
        
        elif isinstance(node, UnabstractedBlock):
            # Recursively transpile statements inside, which may include AsmStmt or standard control flow
            for stmt in node.stmts:
                self.transpile_stmt(stmt)

        elif isinstance(node, AsmStmt):
             arch = node.arch.lower()
             
             if arch.startswith('c'):
                 # Raw C code insertion - emit line by line
                 code_lines = node.code.strip().split('\n')
                 for line in code_lines:
                     if line.strip():  # Skip empty lines
                         self.emit(line)
                         
             elif arch.startswith('system_'):
                 # system <os>: "cmd"
                 os_name = arch.replace('system_', '')
                 cmd_escaped = node.code.replace('"', '\\"')
                 sys_call = f'system("{cmd_escaped}");'
                 
                 if os_name == 'darwin':
                      self.emit("#if defined(__APPLE__)")
                      self.emit(sys_call)
                      self.emit("#endif")
                 elif os_name == 'windows':
                      self.emit("#if defined(_WIN32)")
                      self.emit(sys_call)
                      self.emit("#endif")
                 elif os_name == 'linux':
                      self.emit("#if defined(__linux__)")
                      self.emit(sys_call)
                      self.emit("#endif")
                 else:
                      self.emit(f"// Unknown OS for system call: {os_name}")
                      self.emit(sys_call) # Emit anyway?
             
             else:
                 # Assembly code: wrap in __asm__
                 code_escaped = node.code.replace('"', '\\"')
                 asm_line = f'__asm__("{code_escaped}");'
                 
                 if arch in ['x86_64', 'amd64']:
                     self.emit("#if defined(__x86_64__) || defined(_M_X64)")
                     self.emit(asm_line)
                     self.emit("#endif")
                 elif arch in ['arm64', 'aarch64']:
                     self.emit("#if defined(__aarch64__) || defined(_M_ARM64)")
                     self.emit(asm_line)
                     self.emit("#endif")
                 else:
                     # Fallback or unknown arch
                     self.emit(asm_line)

        elif isinstance(node, PanicStmt):
            msg_code, _ = self.transpile_expr(node.message)
            self.emit(f'printf("PANIC: %s\\n", {msg_code});')
            self.emit('exit(1);')

        elif isinstance(node, AssignStmt):
            expr_code, expr_type = self.transpile_expr(node.expr)
            if isinstance(node.target, ArrayAccess):
                t_code, t_type = self.transpile_expr(node.target.target)
                i_code, _ = self.transpile_expr(node.target.index)
                self.emit(f"{t_code}->data[(int){i_code}] = (void*)(intptr_t){expr_code};")
                return
            
            # Handle Variable Assignment / Declaration
            if isinstance(node.target, Variable):
                name = node.target.name
                final_type = node.type_hint if node.type_hint else expr_type
                
                if final_type in self.traits and expr_type not in self.traits:
                    # Entity to Trait conversion (Trait Object)
                    if expr_type == 'nil':
                        expr_code = "(BunkerTraitObject){ .obj = NULL, .vtable = NULL }"
                    else:
                        vt_name = f"VT_{expr_type}_{final_type}"
                        expr_code = f"(BunkerTraitObject){{ .obj = {expr_code}, .vtable = &{vt_name} }}"

                c_type = self.map_type(final_type)
                
                if name not in self.vars:
                    # Declare
                    self.emit(f"{c_type} {name} = {expr_code};")
                    self.vars[name] = final_type
                else:
                    # Assign
                    self.emit(f"{name} = {expr_code};")
                    if node.type_hint and self.vars[name] != node.type_hint:
                         self.vars[name] = node.type_hint
            
            # Handle Field/Array Assignment
            else:
                 target_code, target_type = self.transpile_expr(node.target)
                 self.emit(f"{target_code} = {expr_code};")
        
        elif isinstance(node, ScopedStmt):
            self.emit("{ /* scoped */")
            self.indent_level += 1
            self.transpile_block(node.body)
            self.indent_level -= 1
            self.emit("}")

        elif isinstance(node, IfStmt):
            # Condition is now a Block - use statement expression to evaluate it
            # ({ statements; result_value; })
            self.emit("if (({ ")
            self.indent_level += 1
            # Save original state
            saved_code_len = len(self.code)
            # Transpile the condition block
            for stmt in node.condition.stmts:
                if isinstance(stmt, ReturnStmt):
                    # Return in condition block means "use this as the condition value"
                    expr_code, _ = self.transpile_expr(stmt.expr)
                    self.emit(expr_code + ";")
                    break
                else:
                    self.transpile_stmt(stmt)
            # Check if there was no explicit return - use last statement as value
            # For now, we require explicit return in condition block
            self.indent_level -= 1
            self.emit("})) {")
            self.indent_level += 1
            self.transpile_block(node.then_branch)
            self.indent_level -= 1
            
            if node.else_branch:
                self.emit("} else {")
                self.indent_level += 1
                self.transpile_block(node.else_branch)
                self.indent_level -= 1
            self.emit("}")

        elif isinstance(node, WhileStmt):
            cond_code, _ = self.transpile_expr(node.condition)
            self.emit(f"while ({cond_code}) {{")
            self.indent_level += 1
            self.transpile_block(node.body)
            self.indent_level -= 1
            self.emit("}")
            
        if isinstance(node, ReturnStmt):
            # Emit all active defers before returning
            # We must iterate in reverse order of the entire defer stack (or current function scope?)
            # Simplified: emit all defers in current function scope.
            # Ideally we track scope depth.
            
            # For now, just dumping all known defers (assuming flat function body for prototype).
            # A real implementation requires scope tracking.
            for stmt in reversed(self.defer_stack):
                self.emit("/* Deferred */")
                self.transpile_stmt(stmt)

            expr_code, _ = self.transpile_expr(node.expr)
            if getattr(self, 'current_return_type', None) and self.current_return_type != 'void':
                self.emit(f"_ret = {expr_code};")
            self.emit("goto end;") 
            
        elif isinstance(node, LoopStmt):
            self.emit("while (1) {")
            self.indent_level += 1
            self.transpile_block(node.body)
            self.indent_level -= 1
            self.emit("}")

        elif isinstance(node, BreakStmt):
            self.emit("break;") 

        elif isinstance(node, ContinueStmt):
            self.emit("continue;") 
            
        elif isinstance(node, DeferStmt):
            self.defer_stack.append(node.stmt)
            self.emit("// Registered defer statement")

        elif isinstance(node, ThrowStmt):
             msg_code, _ = self.transpile_expr(node.expr)
             self.emit(f"global_exception.message = {msg_code};")
             self.emit("if (current_env) longjmp(*current_env, 1);")
             self.emit("else { printf(\"Uncaught exception: %s\\n\", global_exception.message); exit(1); }")

        elif isinstance(node, TryStmt):
             self.emit("{") # Open scope for try-catch vars
             self.indent_level += 1
             
             self.emit("jmp_buf *prev_env = current_env;")
             self.emit("jmp_buf try_env;")
             self.emit("current_env = &try_env;")
             
             self.emit("if (setjmp(try_env) == 0) {")
             self.indent_level += 1
             self.transpile_block(node.body)
             self.indent_level -= 1
             self.emit("} else {")
             self.indent_level += 1
             # Catch block
             self.emit("current_env = prev_env;") # Restore env immediately in catch
             if node.catch_var:
                  # Declare catch variable with the exception message
                  self.emit(f"char* {node.catch_var} = global_exception.message;")
                  self.vars[node.catch_var] = 'string'
             
             if node.catch_body:
                 self.transpile_block(node.catch_body)
             
             self.indent_level -= 1
             self.emit("}")
             
             # Finally block (always executes after try/catch finishes)
             # Note: setjmp logic means we fall through here from both try (normal) and catch (error)
             self.emit("current_env = prev_env;") # Restore env if normal execution allowed it
             
             if node.finally_body:
                 self.transpile_block(node.finally_body)
                 
             self.indent_level -= 1
             self.emit("}")

    def transpile_block(self, node: Block):
        # We need scope-based defer tracking here.
        # Save current stack length
        stack_len = len(self.defer_stack)
        
        # SCOPING FIX: Save current vars to handle block scoping
        vars_snapshot = set(self.vars.keys())
        
        for stmt in node.stmts:
            self.transpile_stmt(stmt)
            
        # Emit defers for this block (LIFO)
        # Only emit defers added IN THIS BLOCK
        while len(self.defer_stack) > stack_len:
            stmt = self.defer_stack.pop()
            self.emit("/* Deferred */")
            self.transpile_stmt(stmt)
            
        # SCOPING FIX: Remove variables declared in this block
        current_vars = list(self.vars.keys())
        for v in current_vars:
             if v not in vars_snapshot:
                 del self.vars[v]


    def transpile_expr(self, node: Node) -> Tuple[str, str]:
        # Returns (c_code, bnk_type)
        if isinstance(node, IntegerLiteral):
            return str(node.value), 'int'
        
        if isinstance(node, FloatLiteral):
            return str(node.value), "float"

        if isinstance(node, BooleanLiteral):
            return "true" if node.value else "false", "bool"
            
        if isinstance(node, StringLiteral):
            return f'"{node.value}"', 'string'
            
        if isinstance(node, NilLiteral):
            return "0", 'nil'

        if isinstance(node, Variable):
            if node.name == 'self':
                 return '_self', self.vars.get('self', 'unknown')
            type_name = self.vars.get(node.name, 'unknown')
            # Check if it is a known Entity Name (Static Access) or User Entity (UpperCamelCase)
            if type_name == 'unknown' and (node.name in ['File', 'FileSystem', 'Strings', 'List', 'Map', 'Math', 'Result', 'System'] or node.name[0].isupper()):
                 return node.name, node.name # Type, Type
            
            return node.name, type_name

        if isinstance(node, UnaryExpr):
            code, t = self.transpile_expr(node.expr)
            op = node.op
            
            # Address-of: &var
            if op == '&':
                return f"(&{code})", f"*{t}"
            
            # Dereference: *ptr
            if op == '*':
                # Remove one level of pointer type
                deref_t = "unknown"
                if t.startswith('*'):
                     deref_t = t[1:]
                return f"(*{code})", deref_t

            return f"{node.op}{code}", t

        if isinstance(node, BinaryExpr):
            left_code, left_type = self.transpile_expr(node.left)
            right_code, right_type = self.transpile_expr(node.right)
            
            res_type = ""
            if left_type != right_type:
                # Implicit coercion in C: int -> double
                if (left_type == 'int' and right_type == 'float') or (left_type == 'float' and right_type == 'int'):
                     res_type = "float"
                else:
                     res_type = left_type 
            else:
                res_type = left_type
            
            # Logic for comparison ops returning bool
            if node.op in ['>', '<', '==', '!=', '>=', '<=']:
                res_type = 'bool'
                # Check for string comparison
                if left_type in ['string', 'str'] and right_type in ['string', 'str']:
                     if node.op == '==':
                          return f"strcmp({left_code}, {right_code}) == 0", 'bool'
                     if node.op == '!=':
                          return f"strcmp({left_code}, {right_code}) != 0", 'bool'
                return f"{left_code} {node.op} {right_code}", 'bool'

            if node.op == '+':
                # String concatenation
                if left_type in ['string', 'str'] or right_type in ['string', 'str']:
                     if left_type in ['string', 'str'] and right_type in ['string', 'str']:
                          return f"BNK_RT_Strings_concat(NULL, {left_code}, {right_code})", "string"
                     if left_type in ['string', 'str'] and right_type in ['int', 'long long']:
                          return f"bunker_str_add_int({left_code}, {right_code})", "string"
                     if left_type in ['string', 'str'] and right_type in ['float', 'double']:
                          return f"bunker_str_add_double({left_code}, {right_code})", "string"

            if node.op == 'or':
                 return f"{left_code} || {right_code}", "bool"
            if node.op == 'and':
                 return f"{left_code} && {right_code}", "bool"

            return f"{left_code} {node.op} {right_code}", res_type
                
        if isinstance(node, ArrayAccess):
            target_code, target_type = self.transpile_expr(node.target)
            index_code, _ = self.transpile_expr(node.index)
            
            if target_type.startswith("Array<") or target_type == "BunkerArray*":
                 # Extract element type
                 elem_type = "void*"
                 if target_type.startswith("Array<"):
                      elem_type = target_type[6:-1]
                 
                 # Emit access with cast
                 c_type = self.map_type(elem_type)
                 if elem_type in ['int', 'bool', 'char', 'long long']: 
                      return f"(({c_type})(intptr_t){target_code}->data[(int){index_code}])", elem_type
                 else:
                      return f"(({c_type}){target_code}->data[(int){index_code}])", elem_type
            
            return f"{target_code}[(int){index_code}]", "unknown"
        
        if isinstance(node, GroupingExpr):
            code, t = self.transpile_expr(node.expression)
            return f"({code})", t

        if isinstance(node, FieldAccess):
            target_code, target_type = self.transpile_expr(node.target)
            
            # Array Length
            if target_type.startswith("Array<") or target_type == "BunkerArray*":
                if node.field == "length":
                     return f"{target_code}->length", "int"
                # Fallback
                return f"{target_code}->{node.field}", "unknown"

            # Emit: ((StructName*)ptr)->field
            # Try to resolve field type from Entity definition
            field_type = "unknown_field_type"
            mangled_t = self.mangle_type_name(target_type)
            if mangled_t in self.entities:
                 entity_decl = self.entities[mangled_t]
                 members = entity_decl.members if hasattr(entity_decl, 'members') else entity_decl.fields
                 for member in members:
                      if isinstance(member, FieldDecl) and member.name == node.field:
                           if hasattr(member.type, 'name'):
                                field_type = member.type.name
                           else:
                                field_type = str(member.type)
                           break
            else:
                 # Debug why not found
                 pass # print(f"DEBUG: FieldAccess target_type={target_type} not in entities (keys={list(self.entities.keys())})")
            
            mangled_target = self.mangle_type_name(target_type)
            return f"(({mangled_target}*){target_code})->{node.field}", field_type

        if isinstance(node, MethodCall):
            if node.method == 'print' and not node.target:
                 arg_code, arg_type = self.transpile_expr(node.args[0])
                 # Check if string or int
                 if arg_type in ['string', 'str', 'char*']:
                     return f'printf("%s\\n", {arg_code})', "void"
                 elif arg_type in ['int', 'long long']:
                     return f'printf("%lld\\n", {arg_code})', "void"
                 elif arg_type == 'float' or arg_type == 'double':
                     return f'printf("%f\\n", {arg_code})', "void"
                 else:
                     return f'printf("%s\\n", (char*){arg_code})', "void"

            # Check for function call (lambda call): fn: args
            if node.method == '':
                fn_code, fn_type = self.transpile_expr(node.target)
                args_code = [self.transpile_expr(arg)[0] for arg in node.args]
                temp_var = f"_closure_tmp_{id(node)}"
                all_args = [f"{temp_var}->env"] + args_code
                args_str = ", ".join(all_args)
                call_expr = f"({{BunkerClosure* {temp_var} = {fn_code}; {temp_var}->fn({args_str});}})"
                return call_expr, "void*"
            
            # Target : Method
            method_code = ""
            ret_type = "void*"
            
            if node.target:
                t_code, t_type = self.transpile_expr(node.target)
                
                # Foreign Call check (Priority)
                if isinstance(node.target, Variable) and (node.target.name == 'foreign' or t_code == 'foreign'):
                      # Foreign call: direct C function
                      args_code = [self.transpile_expr(arg)[0] for arg in node.args]
                      args_str = ", ".join(args_code)
                      ret_type = self.foreign_func_types.get(node.method, 'void*')
                      return f"{node.method}({args_str})", ret_type
                
                # Array Methods
                if t_type.startswith("Array<") or t_type == "BunkerArray*" or t_code.startswith("Bunker_make_array"):
                     if node.method == 'map':
                         fn_code, _ = self.transpile_expr(node.args[0])
                         return f"Bunker_Array_map({t_code}, {fn_code})", "Array<unknown>"
                     if node.method == 'filter':
                         fn_code, _ = self.transpile_expr(node.args[0])
                         return f"Bunker_Array_filter({t_code}, {fn_code})", t_type
                     if node.method == 'reduce':
                         init_code, _ = self.transpile_expr(node.args[0])
                         fn_code, _ = self.transpile_expr(node.args[1])
                         # Cast initial value to void*
                         return f"Bunker_Array_reduce({t_code}, (void*)(intptr_t){init_code}, {fn_code})", "unknown"

                # Determine return type based on built-in entities/methods
                # (Shared by static and instance calls)
                special_ret = None
                if t_type == 'File':
                    if node.method == 'readToString': special_ret = "string"
                    elif node.method in ['close', 'writeString']: special_ret = "void"
                elif t_type == 'FileSystem':
                    if node.method in ['exists', 'delete']: special_ret = "bool"
                elif t_type == 'List':
                    if node.method in ['append', 'remove']: special_ret = "void"
                    elif node.method in ['get', 'size']: special_ret = "int"
                elif t_type == 'Map':
                    if node.method in ['put', 'remove', 'set']: special_ret = "void"
                    elif node.method == 'get': special_ret = "int" 
                    elif node.method == 'contains': special_ret = "bool"
                elif t_type == 'Strings':
                    if node.method in ['concat', 'substring', 'trim', 'toUpper', 'toLower', 'replace']: special_ret = "string"
                    elif node.method in ['startsWith', 'endsWith', 'contains']: special_ret = "bool"
                    elif node.method in ['length', 'indexOf', 'toInt']: special_ret = "int"
                    elif node.method == 'toFloat': special_ret = "float"
                    elif node.method == 'split': special_ret = "List<string>"
                elif t_type == 'System':
                    if node.method in ['os', 'arch']: special_ret = "string"
                elif t_type == 'Math':
                    special_ret = "float"

                # Check if this is a static call (target is an Entity name, not a variable instance)
                # We assume Static Call if code matches type name (e.g. "File" : open)
                if t_type[0].isupper() and t_code == t_type:
                    # Generic Struct Allocation
                    if t_type in self.structs:
                         if node.method == 'alloc':
                              return f"gc_alloc(sizeof({t_type}))", t_type
                         if node.method == 'new':
                              # Call StructName_new(NULL, args...)
                              args_code = [self.transpile_expr(a)[0] for a in node.args]
                              args_str = "NULL"
                              if args_code: args_str += ", " + ", ".join(args_code)
                              return f"{t_type}_new({args_str})", t_type

                    # Foreign check removed (handled above)
                    
                    if t_type.startswith('Array<'):
                        if node.method == 'create':
                             args_code = [self.transpile_expr(a)[0] for a in node.args]
                             return f"Bunker_make_array({args_code[0]})", "BunkerArray*"
                    
                    if node.method == 'alloc':
                         mangled = self.mangle_type_name(t_type)
                         return f"({mangled}*)gc_alloc(sizeof(struct {mangled}))", t_type

                    if t_type == 'Memory':
                        if node.method == 'alloc':
                             size_code, _ = self.transpile_expr(node.args[0])
                             return f"malloc({size_code})", "void*" # Manual alloc uses system malloc for now
                        if node.method == 'free':
                             ptr_code, _ = self.transpile_expr(node.args[0])
                             return f"free({ptr_code})", "void"
                        if node.method == 'copy':
                             dest_code, _ = self.transpile_expr(node.args[0])
                             src_code, _ = self.transpile_expr(node.args[1])
                             size_code, _ = self.transpile_expr(node.args[2])
                             return f"memcpy({dest_code}, {src_code}, {size_code})", "void"

                    elif t_type == 'Result':
                        c_func = f"Result_{node.method}"
                        args_code = [self.transpile_expr(a)[0] for a in node.args]
                        args_str = "NULL, " + ", ".join(args_code)
                        return f"{c_func}({args_str})", "Result"
                    else:
                        # Struct Instantiation: Struct : new
                        if node.method == 'new':
                            c_func = f"{self.mangle_type_name(t_type)}_{node.method}"
                            args_code = [self.transpile_expr(a)[0] for a in node.args]
                            args_str = "NULL"
                            if args_code: args_str += ", " + ", ".join(args_code)
                            return f"{c_func}({args_str})", t_type
                        
                        # General static call for user-defined entities
                        c_func = f"{self.mangle_type_name(t_type)}_{node.method}"
                        args_code = [self.transpile_expr(a)[0] for a in node.args]
                        args_str = "NULL"
                        if args_code: args_str += ", " + ", ".join(args_code)
                        
                        ret_type = "void*"
                        mangled_t = self.mangle_type_name(t_type)
                        if mangled_t in self.entities:
                             entity_decl = self.entities[mangled_t]
                             entity_members = entity_decl.members if hasattr(entity_decl, 'members') else entity_decl.fields
                             for member in entity_members:
                                  if isinstance(member, MethodDecl) and member.name == node.method:
                                       if member.return_type: ret_type = member.return_type
                                       else: ret_type = "void"
                                       break

                        if special_ret: ret_type = special_ret
                        return f"{c_func}({args_str})", ret_type
                
                else:
                    # Instance method call: variable : method : args
                    # Map to Type_method(variable, args)
                    if t_type in self.traits:
                         # Trait dispatch
                         trait = self.traits[t_type]
                         tm = next((m for m in trait.methods if m.name == node.method), None)
                         ret_t = "void*"
                         if tm: ret_t = self.map_type(tm.return_type)
                         
                         args = [self.transpile_expr(a)[0] for a in node.args]
                         args_str = ", ".join(args)
                         if args_str: args_str = ", " + args_str
                         
                         return f"(({ret_t} (*)(void*, ...))((VT_{t_type}*){t_code}.vtable)->{node.method})({t_code}.obj{args_str})", ret_t

                    c_func = f"{self.mangle_type_name(t_type)}_{node.method}"
                    args = [t_code] + [self.transpile_expr(a)[0] for a in node.args]
                    args_str = ", ".join(args)
                    
                    # Determine return type based on method
                    ret_type = "void*"  # default
                    mangled_t = self.mangle_type_name(t_type)
                    if mangled_t in self.entities:
                         entity_decl = self.entities[mangled_t]
                         for member in entity_decl.members:
                              if isinstance(member, MethodDecl) and member.name == node.method:
                                   if member.return_type: ret_type = member.return_type
                                   else: ret_type = "void"
                                   break
                    if special_ret: ret_type = special_ret
                    
                    return f"{c_func}({args_str})", ret_type
                
                # Result type property access (not a static call, not a regular instance method)
                if t_type == 'Result':
                    # Member access pattern result : value
                    # In Bunker v0.1.0 we map properties to fields
                    # typedef struct { long long value; char* errorMessage; bool isError; } BunkerResult;
                    if node.method == 'value': return f"((BunkerResult*){t_code})->value", "int"
                    if node.method == 'errorMessage': return f"((BunkerResult*){t_code})->errorMessage", "string"
                    if node.method == 'isError': return f"((BunkerResult*){t_code})->isError", "bool"
                
                return f"{c_func}({args_str})", ret_type
            
            return "0", "int" # Fallback for void call in expr?

        if isinstance(node, ArrayLiteral):
            length = len(node.elements)
            # Infer element type from first element if present
            elem_type = "unknown"
            if length > 0:
                 _, elem_type = self.transpile_expr(node.elements[0])
            
            # Use GCC statement expression
            arr_var = f"_arr_{id(node)}"
            
            init_code = []
            init_code.append(f"BunkerArray* {arr_var} = Bunker_make_array({length});")
            
            for i, elem in enumerate(node.elements):
                 val_code, val_type = self.transpile_expr(elem)
                 # Cast to void* (assuming int/long fits in pointer)
                 if val_type in ['int', 'long long']:
                     init_code.append(f"{arr_var}->data[{i}] = (void*)(intptr_t){val_code};")
                 else:
                     init_code.append(f"{arr_var}->data[{i}] = (void*){val_code};")
            
            init_code.append(f"{arr_var};")
            
            block_content = " ".join(init_code)
            return f"({{ {block_content} }})", f"Array<{elem_type}>"
        
        if isinstance(node, LambdaExpr):
            # Generate a unique lambda function
            if not hasattr(self, 'lambda_counter'):
                self.lambda_counter = 0
            self.lambda_counter += 1
            lambda_name = f"lambda_{self.lambda_counter}"
            closure_struct_name = f"Closure_{self.lambda_counter}"
            
            # Find captured variables (variables used in body but not in parameters)
            param_names_set = {name for name, _ in node.params}
            captured_vars = self._find_captured_vars(node.body, param_names_set)
            
            # Generate C function for lambda
            # Determine parameter types
            param_decls = []
            param_names = []
            
            # If there are captured variables, add closure struct as first parameter
            if captured_vars:
                param_decls.append(f"{closure_struct_name}* _env")
            else:
                param_decls.append("void* _env")
            
            for param_name, param_type in node.params:
                if param_type == 'int':
                    c_type = 'long long'
                elif param_type == 'float':
                    c_type = 'double'
                elif param_type in ['str', 'string']:
                    c_type = 'char*'
                elif param_type == 'bool':
                    c_type = 'bool'
                else:
                    c_type = 'void*'
                param_decls.append(f"{c_type} {param_name}")
                param_names.append(param_name)
            
            params_str = ", ".join(param_decls) if param_decls else "void"
            
            # Transpile lambda body
            old_vars = self.vars.copy()
            for param_name, param_type in node.params:
                self.vars[param_name] = param_type if param_type else 'unknown'
            
            # If closure, replace captured var references with _env->var
            if captured_vars:
                # Temporarily modify vars to indicate they come from closure
                for var_name in captured_vars:
                    self.vars[var_name] = self.vars.get(var_name, 'unknown')
            
            body_code, body_type = self.transpile_expr(node.body)
            
            # Replace captured variable references with _env->var
            if captured_vars:
                for var_name in captured_vars:
                    # Simple replacement (this is a bit hacky but works for now)
                    body_code = body_code.replace(f" {var_name} ", f" _env->{var_name} ")
                    body_code = body_code.replace(f"({var_name} ", f"(_env->{var_name} ")
                    body_code = body_code.replace(f" {var_name})", f" _env->{var_name})")
                    body_code = body_code.replace(f"({var_name})", f"(_env->{var_name})")
            
            self.vars = old_vars
            
            # Determine return type
            if body_type == 'int':
                ret_type = 'long long'
            elif body_type == 'float':
                ret_type = 'double'
            elif body_type in ['str', 'string']:
                ret_type = 'char*'
            elif body_type == 'bool':
                ret_type = 'bool'
            else:
                ret_type = 'void*'
            
            # Generate closure struct if needed
            if captured_vars:
                closure_struct = f"typedef struct {{\n"
                for var_name in sorted(captured_vars):
                    var_type = self.vars.get(var_name, 'unknown')
                    c_type = self.map_type(var_type)
                    closure_struct += f"    {c_type} {var_name};\n"
                closure_struct += f"}} {closure_struct_name};"
                
                if not hasattr(self, 'closure_structs'):
                    self.closure_structs = []
                self.closure_structs.append(closure_struct)
            
            # Generate the lambda function definition
            lambda_def = f"{ret_type} {lambda_name}({params_str}) {{\n    return {body_code};\n}}"
            
            if not hasattr(self, 'lambda_functions'):
                self.lambda_functions = []
            self.lambda_functions.append(lambda_def)
            
            # Return a BunkerClosure wrapper
            # We need to create the closure struct and return a pointer to BunkerClosure
            # Since this is an expression, we'll use a helper approach
            
            if captured_vars:
                # For closures, we need to:
                # 1. Allocate closure struct
                # 2. Populate it with captured variables
                # 3. Create BunkerClosure wrapper
                # 4. Return pointer to wrapper
                
                # We'll generate a helper function that does this
                helper_name = f"make_closure_{self.lambda_counter}"
                helper_code = f"BunkerClosure* {helper_name}("
                helper_params = []
                for var_name in sorted(captured_vars):
                    var_type = self.vars.get(var_name, 'unknown')
                    c_type = self.map_type(var_type)
                    helper_params.append(f"{c_type} {var_name}")
                helper_code += ", ".join(helper_params) if helper_params else "void"
                helper_code += ") {\n"
                helper_code += f"    {closure_struct_name}* env = gc_alloc(sizeof({closure_struct_name}));\n"
                for var_name in sorted(captured_vars):
                    helper_code += f"    env->{var_name} = {var_name};\n"
                helper_code += f"    BunkerClosure* closure = gc_alloc(sizeof(BunkerClosure));\n"
                helper_code += f"    closure->fn = (BunkerFunction){lambda_name};\n"
                helper_code += f"    closure->env = env;\n"
                helper_code += f"    return closure;\n"
                helper_code += "}"
                
                if not hasattr(self, 'closure_helpers'):
                    self.closure_helpers = []
                self.closure_helpers.append(helper_code)
                
                # Return call to helper function
                helper_args = [var_name for var_name in sorted(captured_vars)]
                return f"{helper_name}({', '.join(helper_args)})", f"function<{ret_type}({params_str})>"
            else:
                # No closure - create simple wrapper with NULL env
                # Use compound literal
                wrapper = f"(&(BunkerClosure){{(BunkerFunction){lambda_name}, NULL}})"
                return wrapper, f"function<{ret_type}({params_str})>"
                    
        return "", "unknown"
 

    def get_source(self) -> str:
        # Add System implementation
        sys_impl = """
char* System_os(void* _self) {
    #if defined(_WIN32)
    return "windows";
    #elif defined(__APPLE__)
    return "macos";
    #elif defined(__linux__)
    return "linux";
    #else
    return "unknown";
    #endif
}

char* System_arch(void* _self) {
    #if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
    #elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
    #else
    return "unknown";
    #endif
}
"""
        # Add closure structs if any
        closure_code = []
        if hasattr(self, 'closure_structs') and self.closure_structs:
            closure_code = ['// Closure structs'] + self.closure_structs + ['']
        
        # Add forward declarations for lambda functions (needed by closure helpers)
        lambda_forward_decls = []
        if hasattr(self, 'lambda_functions') and self.lambda_functions:
            lambda_forward_decls = ['// Lambda forward declarations']
            for lambda_def in self.lambda_functions:
                # Extract function signature (everything before the opening brace)
                sig = lambda_def.split('{')[0].strip()
                lambda_forward_decls.append(f"{sig};")
            lambda_forward_decls.append('')
        
        # Add closure helper functions if any
        closure_helper_code = []
        if hasattr(self, 'closure_helpers') and self.closure_helpers:
            closure_helper_code = ['// Closure helpers'] + self.closure_helpers + ['']
        
        # Add lambda functions if any
        lambda_code = []
        if hasattr(self, 'lambda_functions') and self.lambda_functions:
            lambda_code = ['// Lambda functions'] + self.lambda_functions + ['']
        
        return '\n'.join(self.headers + [sys_impl] + closure_code + lambda_forward_decls + closure_helper_code + lambda_code + self.code)

# ==========================================
# 5. TYPE CHECKER (Static Analysis)
# ==========================================

class TypeError(Exception):
    pass

class TypeChecker:
    def __init__(self):
        self.stmts = []
        # Map variable name -> type (str)
        # We start with global scope
        self.env = {} 
        self.env = {} 
        self.structs = {} # Map struct_name -> list of fields  
        self.templates = {} # name -> Node (Entity/Struct with type_params)
        self.instantiated_names = set()
        self.program_node = None # Reference to append generic instantiations
        self.foreign_funcs = {} # Map foreign_func_name -> return_type
        self.traits = {} # Map trait_name -> TraitDecl
        self.entities = {} # Map entity_name -> EntityDecl

    def ensure_type_instantiated(self, type_name: str):
        if '<' not in type_name: return
        
        base, args = self.parse_generic_type_str(type_name)
        if base in self.templates:
             mangled = self.mangle_name(base, args)
             if mangled not in self.instantiated_names:
                  self.instantiate_template(base, args, mangled)

    def parse_generic_type_str(self, t: str):
        if '<' not in t: return t, []
        base = t.split('<')[0]
        args_str = t[len(base)+1:-1]
        args = []
        depth = 0
        current = ""
        for char in args_str:
            if char == '<': depth += 1
            elif char == '>': depth -= 1
            
            if char == ',' and depth == 0:
                args.append(current.strip())
                current = ""
            else:
                current += char
        if current: args.append(current.strip())
        return base, args

    def mangle_name(self, base: str, args: List[str]) -> str:
        # Simple mangling: List<int> -> List_int
        # Map<str, int> -> Map_str_int
        flat_args = [a.replace('<','_').replace('>','_').replace(',','_').replace(' ','') for a in args]
        return f"{base}_{'_'.join(flat_args)}"

    def instantiate_template(self, base: str, args: List[str], mangled_name: str):
        if mangled_name in self.entities: return
        template = self.templates[base]
        print(f"DEBUG: Instantiating {base} as {mangled_name} with args {args}")
        members = template.members if isinstance(template, EntityDecl) else template.fields
        print(f"DEBUG: Template has {len(members)} members/fields")
        if len(members) > 0:
            first_member = members[0]
            print(f"DEBUG: First member type: {type(first_member)}")
            if hasattr(first_member, 'body'):
                print(f"DEBUG: First member body len: {len(first_member.body)}")
                if len(first_member.body) > 0:
                    print(f"DEBUG: First body stmt type: {type(first_member.body[0])}")
                    if hasattr(first_member.body[0], 'stmts'):
                        print(f"DEBUG: UnabstractedBlock has {len(first_member.body[0].stmts)} stmts BEFORE deepcopy")
        
        inst = copy.deepcopy(template)
        
        inst_members = inst.members if isinstance(inst, EntityDecl) else inst.fields
        if len(inst_members) > 0 and hasattr(inst_members[0], 'body') and len(inst_members[0].body) > 0:
            if hasattr(inst_members[0].body[0], 'stmts'):
                print(f"DEBUG: UnabstractedBlock has {len(inst_members[0].body[0].stmts)} stmts AFTER deepcopy")
        
        inst.name = mangled_name
        inst.type_params = [] # Concrete now
        
        param_map = dict(zip(template.type_params, args))
        
        # Recursive replace
        # print(f"DEBUG: Before replace_types, members[0] target name: {inst_members[0].body[0].target.name if hasattr(inst_members[0].body[0], 'target') else 'N/A'}")
        self.replace_types(inst, param_map)
        # print(f"DEBUG: After replace_types, members[0] target name: {inst_members[0].body[0].target.name if hasattr(inst_members[0].body[0], 'target') else 'N/A'}")
        
        self.instantiated_names.add(mangled_name)
        
        if self.program_node:
             self.program_node.stmts.append(inst)
             if isinstance(inst, EntityDecl):
                  self.entities[inst.name] = inst
             elif isinstance(inst, StructDecl):
                  self.structs[inst.name] = inst.fields
             # Note: Transpiler iterates stmts, so it will pick this up.
             # But TypeChecker loop also needs to check this new instance?
             # My check logic uses index loop, so yes, it will pick it up.

    def replace_types(self, node: Any, mapping: dict):
        # Helper to replace type strings in AST
        if isinstance(node, list):
            for x in node: self.replace_types(x, mapping)
        elif isinstance(node, FieldDecl):
            node.type = self.substitute_type(node.type, mapping)
        elif isinstance(node, MethodDecl):
            node.return_type = self.substitute_type(node.return_type, mapping)
            new_params = []
            for name, t in node.params:
                 new_params.append((name, self.substitute_type(t, mapping)))
            node.params = new_params
            self.replace_types(node.body, mapping)
        elif isinstance(node, AssignStmt):
            if node.type_hint:
                 node.type_hint = self.substitute_type(node.type_hint, mapping)
            self.replace_types(node.target, mapping)
            self.replace_types(node.expr, mapping)
        elif isinstance(node, (EntityDecl, StructDecl)):
             if hasattr(node, 'members'): self.replace_types(node.members, mapping)
             if hasattr(node, 'fields'): self.replace_types(node.fields, mapping)
        elif isinstance(node, Block):
             self.replace_types(node.stmts, mapping)
        elif isinstance(node, IfStmt):
            self.replace_types(node.condition, mapping)
            self.replace_types(node.then_branch, mapping)
            if node.else_branch: self.replace_types(node.else_branch, mapping)
        elif isinstance(node, WhileStmt):
            self.replace_types(node.condition, mapping)
            self.replace_types(node.body, mapping)
        elif isinstance(node, PanicStmt):
            self.replace_types(node.message, mapping)
        # Other nodes might contain types? MethodCall args are expressions.
        # But maybe type casts or 'new T' calls?
        # Check MethodCall target if it is variable reference to a type? (Entity : method)
        elif isinstance(node, MethodCall):
            if node.target: self.replace_types(node.target, mapping)
            self.replace_types(node.args, mapping)
        elif isinstance(node, Variable):
            node.name = self.substitute_type(node.name, mapping)
        elif isinstance(node, ArrayAccess):
            self.replace_types(node.target, mapping)
            self.replace_types(node.index, mapping)
        elif isinstance(node, BinaryExpr):
            self.replace_types(node.left, mapping)
            self.replace_types(node.right, mapping)
        elif isinstance(node, UnaryExpr):
            self.replace_types(node.expr, mapping)
        elif isinstance(node, ReturnStmt):
            if node.expr: self.replace_types(node.expr, mapping)
        elif isinstance(node, FieldAccess):
            if node.target: self.replace_types(node.target, mapping)
        elif isinstance(node, UnabstractedBlock):
             self.replace_types(node.stmts, mapping)
        elif isinstance(node, AsmStmt):
             for t_param, t_arg in mapping.items():
                  c_type = "long long" if t_arg == 'int' else "double" if t_arg == 'float' else "char*" if t_arg in ['string', 'str'] else "bool" if t_arg == 'bool' else "void*"
                  if t_arg == 'void*': c_type = 'void*'
                  if t_arg not in ['int', 'float', 'string', 'str', 'bool', 'void*', 'void']:
                       if '<' in t_arg:
                             base = t_arg.split('<')[0]
                             args = t_arg[len(base)+1:-1]
                             flat = args.replace('<','_').replace('>','_').replace(',','_').replace(' ','')
                             c_type = f"{base}_{flat}*"
                       else:
                             c_type = f"{t_arg}*"
                  
                  import re
                  node.code = re.sub(f"\\b{t_param}\\b", c_type, node.code) 

    def substitute_type(self, t: str, mapping: dict) -> str:
        # Replace T with int
        # Replace List<T> with List<int>
        # Check full match first
        if t in mapping: return mapping[t]
        
        # Check partial match for generics
        if '<' in t:
             base, args = self.parse_generic_type_str(t)
             new_args = [self.substitute_type(a, mapping) for a in args]
             return f"{base}<{', '.join(new_args)}>"
             
        return t

    def check(self, node: Node):
        if isinstance(node, Program):
            self.program_node = node
            # Loop with index to handle appended stmts during instantiation
            i = 0
            while i < len(node.stmts):
                self.check(node.stmts[i])
                i += 1
                
        elif isinstance(node, Block):
            for stmt in node.stmts:
                try:
                    self.check(stmt)
                except Exception as e:
                    print(f"DEBUG: Error checking stmt type {type(stmt)}: {e}")
                    if isinstance(stmt, AssignStmt) and isinstance(stmt.target, Variable):
                        print(f"DEBUG: AssignStmt target: {stmt.target.name}")
                    
                    # Try to get line info
                    if hasattr(stmt, 'target') and hasattr(stmt.target, 'name') and hasattr(stmt.target.name, 'line'):
                         print(f"DEBUG: At line {stmt.target.name.line}")
                    elif hasattr(stmt, 'condition') and hasattr(stmt.condition, 'line'): 
                         pass
                    
                    if hasattr(stmt, 'name') and hasattr(stmt.name, 'line'):
                         print(f"DEBUG: At line {stmt.name.line}")
                    elif hasattr(stmt, 'line'):
                         print(f"DEBUG: At line {stmt.line}")
                    raise e

        elif isinstance(node, StructDecl):
            if node.type_params:
                self.templates[node.name] = node
                return
            # print(f"DEBUG: Registering struct {node.name}")
            self.structs[node.name] = node.fields

        elif isinstance(node, TraitDecl):
            if node.type_params:
                self.templates[node.name] = node
                return
            self.traits[node.name] = node

        elif isinstance(node, ForeignDecl):
            for func in node.funcs:
                # Register foreign function: global or namespaced?
                # For now, let's register as "foreign.funcName" or check "foreign" target in method call
                self.foreign_funcs[func.name] = func.return_type

        elif isinstance(node, EntityDecl):
            if node.type_params:
                self.templates[node.name] = node
                return
            # Register Entity type
            self.env[node.name] = node.name 
            self.entities[node.name] = node
            self.current_entity_name = node.name # Set context
            
            # Register Entity fields as struct fields for resolution
            fields = []
            entity_members = node.members if hasattr(node, 'members') else node.fields
            for m in entity_members:
                 if isinstance(m, FieldDecl):
                      fields.append(m)
            self.structs[node.name] = fields 
            
            # Verify implemented traits
            if node.implements:
                for t_name in node.implements:
                    if t_name not in self.traits:
                        raise TypeError(f"Entity {node.name} implements unknown Trait {t_name}")
                    trait = self.traits[t_name]
                    # Verify each method in trait is implemented by entity
                    for tm in trait.methods:
                        found = False
                        for em in entity_members:
                            if isinstance(em, MethodDecl) and em.name == tm.name:
                                # Check signatures (Basic check for now: name and params count)
                                if len(em.params) != len(tm.params):
                                     raise TypeError(f"Method {em.name} signature mismatch for trait {t_name} (params count)")
                                found = True
                                break
                        if not found:
                            raise TypeError(f"Entity {node.name} fails to implement method {tm.name} from trait {t_name}")

            # Check members
            entity_members = node.members if hasattr(node, 'members') else node.fields
            for member in entity_members:
                self.check(member)

        elif isinstance(node, MethodDecl):
            # Register parameters
            for p_name, p_type in node.params:
                self.env[p_name] = p_type
            
            # Register 'self' if not static
            # We don't have is_static info easily here unless we pass it or check decl?
            # Ideally TypeChecker should track current entity.
            # But roughly, we can add self = EntityName if we knew it.
            # For now, just rely on explicit self usage being valid if we whitelist 'self'?
            # Actually, we need to add 'self' to env so Variable('self') resolves.
            if self.current_entity_name:
                 self.env['self'] = self.current_entity_name
            else:
                 self.env['self'] = 'unknown' 
            
            # Check body
            self.check(Block(node.body)) # Wrap in block to reuse loop

        elif isinstance(node, IfStmt):
            self.check(node.condition)
            self.check(node.then_branch)
            if node.else_branch:
                self.check(node.else_branch)
        
        elif isinstance(node, WhileStmt):
            self.check(node.condition)
            self.check(node.body)

        elif isinstance(node, AssignStmt):
            # 1. Check expr type
            expr_type = self.resolve_type(node.expr)
            
            # 2. Check type hint compatibility
            if node.type_hint:
                # INSTANTIATION TRIGGER
                self.ensure_type_instantiated(node.type_hint)
                
                # Loose check for now
                if node.type_hint != expr_type and expr_type != 'nil':
                    if node.type_hint in self.traits:
                         # Verify implementation
                         if expr_type in self.entities:
                             entity = self.entities[expr_type]
                             if not entity.implements or node.type_hint not in entity.implements:
                                  raise TypeError(f"Type '{expr_type}' does not implement Trait '{node.type_hint}'")
                         else:
                             # Might be a pointer or other type?
                             pass
                    else:
                         pass # Only warn in strict mode. Allow Map <- Map_int etc
            
            # 3. Check existing variable consistency (if exists)
            if isinstance(node.target, Variable):
                name = node.target.name
                if name in self.env:
                    existing_type = self.env[name]
                    if node.type_hint and existing_type != node.type_hint:
                         # Warn or implicit re-assign?
                         pass
                
                # 4. Register
                if node.type_hint:
                    self.env[name] = node.type_hint
                elif name not in self.env:
                    self.env[name] = expr_type
            
        elif isinstance(node, MethodCall):
             for arg in node.args:
                 self.check(arg) # or resolve_type?

    def resolve_type(self, node: Node) -> str:
        if isinstance(node, IntegerLiteral):
            return "int"
        if isinstance(node, FloatLiteral):
            return "float"
        if isinstance(node, StringLiteral):
            return "string"
        if isinstance(node, NilLiteral):
            return "nil"
            
        if isinstance(node, Variable):
            if node.name == 'foreign':
                return 'foreign'
            if node.name not in self.env:
                # Check for capitalized static target
                if node.name[0].isupper() or node.name == 'System':
                    return node.name
                raise TypeError(f"Undefined variable '{node.name}'")
            return self.env[node.name]
            
        if isinstance(node, UnaryExpr):
            return self.resolve_type(node.expr)
            
        if isinstance(node, BinaryExpr):
            left_t = self.resolve_type(node.left)
            right_t = self.resolve_type(node.right)
            
            if left_t != right_t:
                # Implicit coercion: int -> float
                if (left_t == 'int' and right_t == 'float') or (left_t == 'float' and right_t == 'int'):
                    resolved_t = "float"
                elif (left_t == 'string' and right_t == 'int') or (left_t == 'int' and right_t == 'string'):
                     resolved_t = "string"
                # Allow comp with nil
                elif (left_t == 'nil' or right_t == 'nil') and node.op in ['==', '!=']:
                     resolved_t = "bool" # Comparison result
                else:
                    resolved_t = left_t # Fallback
            else:
                resolved_t = left_t
            
            # Check operator validity
            if node.op in ['+', '-', '*', '/']:
                if resolved_t not in ['int', 'float', 'string']: # Added string for concatenation
                    # raise TypeError(f"Operator '{node.op}' requires int or float, got {resolved_t}")
                    pass
                return resolved_t
            
            if node.op in ['>', '<', '==', '!=']:
                # Allow comparison? For now only same types
                return "bool" # We don't have true/false keywords yet but resolving to logical type
                
            return left_t

        if isinstance(node, ArrayAccess):
            target_type = self.resolve_type(node.target)
            if target_type == "void**":
                 return "void*"
            if target_type == "void*":
                 return "void*"
            if not target_type.startswith("Array<"):
                 raise TypeError(f"Type '{target_type}' is not an array, cannot index")
            return target_type[6:-1]

        if isinstance(node, FieldAccess):
            target_type = self.resolve_type(node.target)
            
            if target_type.startswith("Array<") and node.field == "length":
                return "int"
                
            if target_type in self.structs:
                fields = self.structs[target_type]
                for f in fields:
                    if f.name == node.field:
                        return f.type
                raise TypeError(f"Struct '{target_type}' has no field '{node.field}'")
            if target_type not in self.structs:
                 print(f"DEBUG: Known structs: {list(self.structs.keys())}")
                 raise TypeError(f"Type '{target_type}' is not a struct, cannot access field '{node.field}'")

        if isinstance(node, MethodCall):
             # Resolve target type
             try:
                 v_type = self.resolve_type(node.target)
             except TypeError:
                 v_type = "unknown"

             # Array Methods
             if v_type.startswith("Array<"):
                  if node.method == 'map': return "Array<unknown>"
                  if node.method == 'filter': return v_type
                  if node.method == 'reduce': return "unknown"
             
             # Lambda call (fn: args) - indicated by empty method
             if node.method == '':
                 return "unknown" 

             # Known StdLib types (Static calls usually on Variables)
             if isinstance(node.target, Variable):
                  if node.target.name in self.structs and node.method == 'new':
                       return node.target.name
                  if node.target.name == 'File' and node.method == 'open':
                       return "File"
                  if node.target.name == 'Result':
                       if node.method in ['ok', 'fail']: return "Result"
                  if node.target.name == 'System':
                       if node.method in ['os', 'arch']: return "string"

                  if node.target.name == 'Strings':
                       if node.method == 'split': return "List<string>"
                       if node.method in ['toInt', 'indexOf', 'length']: return "int"
                       if node.method == 'toFloat': return "float"
                       if node.method in ['startsWith', 'endsWith', 'contains']: return "bool"
                       if node.method in ['concat', 'substring', 'trim', 'toUpper', 'toLower', 'replace', 'join']: return "string"

                  if node.target.name == 'Math':
                       return "float" # All math returns float for now

                  if node.target.name == 'IO':
                       if node.method == 'readFileLines': return "Array<string>"
                       if node.method == 'input': return "string"
                  
             if v_type in self.traits:
                  trait = self.traits[v_type]
                  for tm in trait.methods:
                       if tm.name == node.method:
                            return tm.return_type
                  raise TypeError(f"Trait '{v_type}' has no method '{node.method}'")

                  # Foreign Call
                  if node.target.name == 'foreign' or v_type == 'foreign':
                      if node.method in self.foreign_funcs:
                          return self.foreign_funcs[node.method]
                      # raise TypeError(f"Undefined foreign function '{node.method}'") # Relax strictness

             if v_type == 'File':
                  if node.method == 'readFile': return "string"
                  if node.method == 'readToString': return "int" # Returns int (status/length) or string? 
                  # Implementation check: readToString usually returns void or modifies buffer?
             if v_type == 'Result':
                  if node.method == 'value': return "int"
                  if node.method == 'errorMessage': return "string"
                  if node.method == 'isError': return "bool"
             
             if node.method == 'consume': return "Token"
             if node.method == 'peek': return "Token"
             if node.method == 'advance': return "void"
             
             # Fallback
             return "void"
        
        if isinstance(node, LambdaExpr):
            # Lambda type is 'function' for now
            # In a more sophisticated system, we'd track parameter and return types
            return "function"

        if isinstance(node, ArrayLiteral):
            if not node.elements:
                return "Array<unknown>"
            elem_type = self.resolve_type(node.elements[0])
            node.element_type = elem_type
            return f"Array<{elem_type}>"
            
        if isinstance(node, BooleanLiteral):
            return "bool"
            
        raise TypeError(f"Cannot resolve type for {node}")

# ==========================================
# 6. INTERPRETER (REPL)
# ==========================================

class Interpreter:
    def __init__(self):
        self.env = {}
        # We can integrate TypeChecker here too for REPL safety?
        # Ideally REPL should typecheck BEFORE interpret.
        pass

    def evaluate(self, node: Node):
        if isinstance(node, ScopedStmt):
            self.emit("{ /* scoped */")
            self.indent_level += 1
            
            # Simple implementation for bootstrap: 
            # We don't track nested locals here for complex cleanup yet,
            # but we allow the C compiler to scope variables.
            # To truly 'free' them, the user can call Memory:free inside or we can add magic.
            
            self.transpile_stmt(node.body)
            
            self.indent_level -= 1
            self.emit("}")
            return
            
        if isinstance(node, Block):
            res = None
            for stmt in node.stmts:
                res = self.evaluate(stmt)
            return res 

        elif isinstance(node, IfStmt):
            cond = self.evaluate(node.condition)
            if cond:
                return self.evaluate(node.then_branch)
            elif node.else_branch:
                return self.evaluate(node.else_branch)
        
        elif isinstance(node, WhileStmt):
            last_res = None
            while self.evaluate(node.condition):
                last_res = self.evaluate(node.body)
            return last_res

        elif isinstance(node, AssignStmt):
            val = self.evaluate(node.expr)
            self.env[node.name] = val
            return val
        elif isinstance(node, Variable):
            return self.env.get(node.name, f"Error: '{node.name}' undefined")
        elif isinstance(node, IntegerLiteral):
            return node.value
        elif isinstance(node, FloatLiteral):
            return node.value
        elif isinstance(node, StringLiteral):
            return node.value
        elif isinstance(node, NilLiteral):
            return "nil"
        elif isinstance(node, UnaryExpr):
            val = self.evaluate(node.expr)
            if node.op == '-': return -val
            if node.op == '+': return +val
            if node.op == '!': return not val
            return val
        elif isinstance(node, BinaryExpr):
            l, r = self.evaluate(node.left), self.evaluate(node.right)
            if (isinstance(l, (int, float))) and (isinstance(r, (int, float))):
                if node.op == '+': return l + r
                if node.op == '-': return l - r
                if node.op == '*': return l * r
                if node.op == '/': 
                    if isinstance(l, int) and isinstance(r, int):
                        return l // r
                    return l / r
                if node.op == '>': return l > r
                if node.op == '<': return l < r
                if node.op == '==': return l == r
                if node.op == '!=': return l != r
            return "Type Error"
        elif isinstance(node, MethodCall):
            if node.method == 'print':
                val = self.evaluate(node.args[0])
                print(val)
                return val
        elif isinstance(node, PanicStmt):
            msg = self.evaluate(node.message)
            print(f"PANIC: {msg}")
            sys.exit(1)

    def repl_loop(self):
        print("Bunker REPL v2.6.0")
        print("Type 'exit' or 'quit' to close.")
        print("-" * 20)
        
        buffer = ""
        in_multiline = False
        
        while True:
            try:
                prompt = "... " if in_multiline else ">> "
                line = input(prompt)
                if not line and not in_multiline: continue
                if line in ['exit', 'quit']: break
                
                buffer += line + " "

                stripped = line.strip()
                if stripped.endswith(':'):
                    in_multiline = True
                    continue
                
                if in_multiline:
                    if stripped.endswith('.'):
                        # Simple heuristic: if it ends in dot, it might be closing.
                        # But we keep going until a line is JUST '.' or '..' etc.
                        if stripped.strip('.') == '':
                            in_multiline = False
                        else:
                            continue
                    else:
                        continue
                else:
                    if not buffer.strip().endswith('.'):
                        buffer += '.'
                
                lexer = Lexer(buffer)
                parser = Parser(lexer.tokens)
                try:
                    stmt = parser.parse_stmt()
                    
                    # TYPE CHECK
                    # We need a persistent type checker for the REPL session
                    if not hasattr(self, 'type_checker'):
                        self.type_checker = TypeChecker()
                    
                    try:
                        self.type_checker.check(stmt)
                    except TypeError as te:
                        print(f"Type Error: {te}")
                        buffer = ""
                        in_multiline = False
                        continue
                        
                    res = self.evaluate(stmt)
                    if isinstance(stmt, (BinaryExpr, Variable, IntegerLiteral)):
                        print(res)
                except SyntaxError as e:
                    print(f"Syntax Error: {e}")
                    
                buffer = ""
                in_multiline = False
            except (EOFError, KeyboardInterrupt):
                break

# ==========================================
# 5. DRIVER
# ==========================================

def main():
    import traceback
    if len(sys.argv) < 2:
        print("Usage: bunkerc <source.bnk> [-o <output>]")
        sys.exit(1)
        
    if sys.argv[1] == '--repl':
        interpreter = Interpreter()
        interpreter.repl_loop()
        sys.exit(0)

    source_path = sys.argv[1]
    with open(source_path, 'r') as f:
        source = f.read()

    # 2. LEXER
    lexer = Lexer(source)
    
    # 3. PARSER
    parser = Parser(lexer.tokens)
    
    try:
        ast = parser.parse_program()
        
        # 4. TYPE CHECKER
        checker = TypeChecker()
        try:
            checker.check(ast)
        except TypeError as te:
            print(f"TYPE ERROR DURING CHECK: {te}")
            traceback.print_exc()
            sys.exit(1)
        
        # 5. TRANSPILER
        transpiler = CTranspiler()
        transpiler.structs = checker.structs  # Pass struct registry
        transpiler.transpile(ast)
        c_source = transpiler.get_source()
        
        # 6. COMPILE C
        output_bin = "output"
        c_file = "output.c"
        
        if "-o" in sys.argv:
            try:
                idx = sys.argv.index("-o")
                user_output = sys.argv[idx+1]
                
                # If user specified .c extension, use it as the C source file
                # and remove the extension for the binary
                if user_output.endswith('.c'):
                    c_file = user_output
                    output_bin = user_output[:-2]  # Remove .c extension
                else:
                    # User specified binary name, append .c for source
                    output_bin = user_output
                    c_file = user_output + ".c"
            except:
                pass
                
        with open(c_file, 'w') as f:
            f.write(c_source)
            
        subprocess.run(["gcc", c_file, "-o", output_bin], check=True)
        # print(f"Compiled successfully to {output_bin}")
        
    except Exception as e:
        print(f"Error: {e}")
        traceback.print_exc()
        sys.exit(1)

if __name__ == '__main__':
    main()
