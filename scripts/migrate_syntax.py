import re
import sys

# Token types
KEYWORD = 'KEYWORD'
IDENT = 'IDENT'
SYMBOL = 'SYMBOL'
OTHER = 'OTHER'

keywords = {
    'if', 'else', 'while', 'loop', 'method', 'Entity', 'Struct', 'foreign', 'defer', 'func'
}

def tokenize(text):
    tokens = []
    # Simplified regex for migration purposes
    # Strings, comments, keywords, idents, symbols
    token_spec = [
        ('STRING',   r'"(?:[^"\\]|\\.)*"'), # String literal
        ('COMMENT',  r'#.*'),               # Comment
        ('KEYWORD',  r'\b(?:' + '|'.join(keywords) + r')\b'),
        ('IDENT',    r'[a-zA-Z_][a-zA-Z0-9_]*'),
        ('SYMBOL',   r'[:\.\(\),\[\];]'),
        ('WS',       r'\s+'),
        ('OTHER',    r'.'),
    ]
    tok_regex = '|'.join('(?P<%s>%s)' % pair for pair in token_spec)
    
    for mo in re.finditer(tok_regex, text):
        kind = mo.lastgroup
        value = mo.group()
        if kind == 'WS':
            tokens.append(('WS', value, mo.start()))
        elif kind == 'COMMENT':
            tokens.append(('COMMENT', value, mo.start()))
        else:
            tokens.append((kind, value, mo.start()))
    return tokens

def migrate(source):
    tokens = tokenize(source)
    output = []
    
    # Block stack: list of 'block_type'
    stack = []
    
    idx = 0
    last_end = 0
    
    # Helper to peek next non-ws/comment token
    def peek_next(i):
        j = i + 1
        while j < len(tokens):
            if tokens[j][0] not in ('WS', 'COMMENT'):
                return tokens[j]
            j += 1
        return None
    
    # Helper to check if dot is property access
    def is_prop_access(i):
        # dot at i
        # prev: tokens[i-1]
        # next: tokens[i+1] (skip WS)
        
        # Check prev (ignoring WS)
        k = i - 1
        while k >= 0 and tokens[k][0] in ('WS', 'COMMENT'):
            k -= 1
        
        prev_tok = tokens[k] if k >= 0 else None
        
        # Check next (ignoring WS)
        next_tok = peek_next(i)
        
        # Determine if property access
        # If followed by IDENT
        if next_tok and next_tok[0] == 'IDENT':
             # And prev is IDENT or ) or ]
             if prev_tok and (prev_tok[0] == 'IDENT' or prev_tok[1] in (')', ']','"',"'")):
                 return True
        return False
        
    while idx < len(tokens):
        kind, value, start = tokens[idx]
        
        # Handle Keywords pushing to stack
        if kind == 'KEYWORD':
            if value in keywords:
                if value == 'else':
                    # else is special.
                    # It implies we are in an 'if' block.
                    # Push 'else' to stack.
                    stack.append('else')
                elif value == 'func':
                     # func in foreign block. Does NOT start a block that ends with dot (usually).
                     # Foreign block ends with dot.
                     # But 'unabstracted' uses parse_stmt loop?
                     # Let's assume func doesn't push.
                     pass
                else:
                    stack.append(value)
        
        # Handle Dot
        if kind == 'SYMBOL' and value == '.':
            if not is_prop_access(idx):
                # Block terminator!
                if stack:
                    blk = stack.pop()
                    
                    term = f"end {blk}."
                    
                    # Special nesting logic
                    # If we popped 'else', check if we need to close 'if' too?
                    # The source has ONE dot 'if ... else ... .'
                    # So popping 'else' handles the dot. 
                    # But we must ALSO emit 'end if.' immediately after?
                    # Yes.
                    
                    if blk == 'else':
                        # Check if parent is 'if'
                        if stack and stack[-1] == 'if':
                             parent = stack.pop()
                             term += f" end {parent}."
                    
                    # Replace '.' with term
                    # We need to construct output.
                    # Append everything from last_end to start
                    output.append(source[last_end:start])
                    output.append(term)
                    last_end = start + 1
                    idx += 1
                    continue
                else:
                    # Dot with no stack? Module definition or Import?
                    # 'module name.' -> 'module name;' in our grammar?
                    # Old grammar used ';' for imports/modules.
                    # But wait, `bunker.bnk` uses `.` for Main entity end?
                    # `Main` is Entity.
                    pass

        idx += 1
        
    output.append(source[last_end:])
    return "".join(output)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 migrate_syntax.py <file>")
        sys.exit(1)
        
    filepath = sys.argv[1]
    with open(filepath, 'r') as f:
        content = f.read()
        
    new_content = migrate(content)
    
    with open(filepath, 'w') as f:
        f.write(new_content)
    print(f"Migrated {filepath}")
