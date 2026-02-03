#!/usr/bin/env python3
import sys
import re
import os

def migrate_syntax(content):
    lines = content.split('\n')
    output = []
    
    i = 0
    stack = [] # Track context if needed, mostly for indentation
    
    # Simple recursive descent or stack based approach is best
    # But since we are token matching line-by-lineish, let's try a custom iterator
    
    stmt_stack = [] # (type, indentation)
    
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        indent = line[:len(line) - len(stripped)]
        
        # Check for old if
        # Pattern: 'if <expr>:'
        # We need to distinguish it from new syntax 'if:' just in case (though we assume old files)
        # And ensure we don't match strings etc. (basic heuristic)
        
        if stripped.startswith('if ') and stripped.endswith('end if.'):
            # Handle one-line if: if <cond>: <stmt>; end if.
            # Extract content between if and end if.
            content_part = stripped[3:-7].strip() # remove 'if ' and 'end if.'
            
            if ':' in content_part:
                cond_part, body_part = content_part.split(':', 1)
                cond = cond_part.strip()
                body = body_part.strip()
                
                output.append(f"{indent}if:")
                output.append(f"{indent}condition:")
                output.append(f"{indent}    return {cond};")
                output.append(f"{indent}end condition.")
                output.append(f"{indent}then:")
                output.append(f"{indent}    {body}")
                output.append(f"{indent}end then.")
                output.append(f"{indent}end if.")
                i += 1
                continue
        
        elif stripped.startswith('if ') and stripped.endswith(':'):
            # Extract condition
            # if condition:
            condition = stripped[3:-1].strip()
            
            # Start new syntax
            output.append(f"{indent}if:")
            output.append(f"{indent}condition:")
            output.append(f"{indent}    return {condition};")
            output.append(f"{indent}end condition.")
            output.append(f"{indent}then:")
            
            # We are now in THEN block
            # We need to know when this block ends (else or end if)
            # We push to stack so we can insert 'end then.'
            stmt_stack.append({'type': 'if', 'indent': indent, 'stage': 'then'})
            i += 1
            continue
            
        elif stripped.startswith('else:') and stmt_stack and stmt_stack[-1]['type'] == 'if':
            # Found else for current if
            # Close previous block (then)
            context = stmt_stack[-1]
            prev_indent = context['indent']
            
            output.append(f"{prev_indent}end then.")
            output.append(f"{prev_indent}else:")
            
            context['stage'] = 'else'
            i += 1
            continue

        elif stripped == 'end else.':
            # Ignore existing end else, we will generate it
            i += 1
            continue
            
        elif stripped == 'end if.' and stmt_stack and stmt_stack[-1]['type'] == 'if':
            # Found end of if
            context = stmt_stack.pop()
            prev_indent = context['indent']
            stage = context['stage']
            
            output.append(f"{prev_indent}end {stage}.")
            output.append(f"{prev_indent}end if.")
            i += 1
            continue
            
        elif stripped == '.' and stmt_stack:
            # Handle dot termination for if
            # Check if top of stack is if
            if stmt_stack[-1]['type'] == 'if':
                context = stmt_stack.pop()
                prev_indent = context['indent']
                stage = context['stage']
                
                output.append(f"{prev_indent}end {stage}.")
                output.append(f"{prev_indent}end if.")
                i += 1
                continue
            
        # Standard line preservation
        output.append(line)
        i += 1
        
    return '\n'.join(output)

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 migrate.py <file>")
        return
        
    filepath = sys.argv[1]
    with open(filepath, 'r') as f:
        content = f.read()
        
    new_content = migrate_syntax(content)
    
    # Determine output
    if '-i' in sys.argv:
        with open(filepath, 'w') as f:
            f.write(new_content)
        print(f"Migrated {filepath}")
    else:
        print(new_content)

if __name__ == '__main__':
    main()
