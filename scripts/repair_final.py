import sys
import re

def repair(content):
    # 0. Strip ALL existing end else. to start clean
    # We use a regex that handles indentation
    content = re.sub(r'^\s*end else\.\n', '', content, flags=re.MULTILINE)
    
    # 1. Fix unclosed quotes and split patterns
    content = re.sub(r'==\s*";', '== ":";', content)
    
    # Fix split Strings: startsWith
    pattern_strings = re.compile(r'condition:\s+return Strings;\s+end condition\.\s+then:\s+startsWith: (.*?), ";\s+self: advance;', re.MULTILINE)
    content = pattern_strings.sub(r'condition:\n            return Strings: startsWith: \1, ":";\n        end condition.\n        then:\n            self: advance;', content)

    # Fix split (self; ... isAtEnd):
    content = re.sub(r'return \(self;\s+end condition\.\s+then:\s+isAtEnd\):\s+', 
                     r'return self: isAtEnd;\n        end condition.\n        then:\n            ', content, flags=re.MULTILINE)

    # 2. Fix "ielse:", doubled colons, etc.
    content = content.replace('ielse:', 'else:')
    content = content.replace('then::', 'then:')
    content = content.replace('condition::', 'condition:')
    content = content.replace('":";"', '":";')
    content = content.replace('":";;', '":";')

    # 3. Canonicalize other terminators (remove duplicates)
    for term in ['end condition.', 'end then.', 'end if.', 'end method.', 'end Entity.', 'end loop.', 'end while.']:
        safe_term = term.replace('.', r'\.')
        pattern = re.compile(f'({safe_term})\\n\\s*{safe_term}', re.MULTILINE)
        content = pattern.sub(r'\1', content)
        content = pattern.sub(r'\1', content)

    # 4. Add back end else. correctly using a stack
    lines = content.split('\n')
    new_lines = []
    stack = [] # stack of (indent, has_else)
    
    for line in lines:
        stripped = line.strip()
        indent_size = len(line) - len(line.lstrip())
        
        if stripped == 'if:':
            stack.append({'indent': indent_size, 'has_else': False})
        elif stripped == 'else:':
            if stack:
                stack[-1]['has_else'] = True
        
        if stripped == 'end if.':
            if stack:
                info = stack.pop()
                if info['has_else']:
                    new_lines.append(' ' * info['indent'] + 'end else.')
        
        new_lines.append(line)
        
    return '\n'.join(new_lines)

if __name__ == "__main__":
    filepath = sys.argv[1]
    with open(filepath, 'r') as f:
        data = f.read()
    with open(filepath, 'w') as f:
        f.write(repair(data))
    print(f"Repaired {filepath}")
