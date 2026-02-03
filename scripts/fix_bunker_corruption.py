import sys
import re

def fix_bunker_bnk(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    
    # 1. Fix (self; ... isAtEnd): pattern
    pattern_self = re.compile(r'condition:\s+return \(self;\s+end condition\.\s+then:\s+isAtEnd\):\s*', re.MULTILINE)
    content = pattern_self.sub('condition:\n            return self: isAtEnd;\n        end condition.\n        then:\n            ', content)

    # 2. Fix other string-split patterns
    pattern_strings = re.compile(r'return (.*) == ";\s+end condition\.\s+then:\s+":\s*', re.MULTILINE)
    content = pattern_strings.sub(r'return \1 == ":";\n        end condition.\n        then:\n            ', content)

    # 3. Fix line 3277 specifically (has multiple or)
    pattern_3277 = re.compile(r'return (.*) == ";\s+end condition\.\s+then:\s+" or (.*): break;', re.MULTILINE)
    content = pattern_3277.sub(r'return \1 == ":" or \2;\n        end condition.\n        then:\n            break;', content)

    # 4. Cleanup doubled quotes if any remain
    content = content.replace('":";"', '":";')
    
    # 5. Fix "ielse:" to "else:"
    content = content.replace('ielse:', 'else:')

    # 6. Fix missing 'end else.'
    # We look for 'else:' followed by anything that doesn't contain 'end else.' but ends with 'end if.'
    # But wait, nested if's make this hard. 
    # Let's do a simple line-by-lineish correction for 'else:' blocks.
    lines = content.split('\n')
    new_lines = []
    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        indent = line[:line.find(stripped)] if stripped else ""
        
        if stripped == 'else:':
            # Collect body until 'end if.' or another 'end'
            body_lines = []
            j = i + 1
            depth = 1
            found_end_if = False
            while j < len(lines):
                s = lines[j].strip()
                if s == 'if:': depth += 1
                if s == 'end if.':
                    depth -= 1
                    if depth == 0:
                        found_end_if = True
                        break
                if s == 'end else.' and depth == 1:
                    # Already has end else.
                    break
                body_lines.append(lines[j])
                j += 1
            
            if found_end_if:
                # Missing end else.
                new_lines.append(line)
                new_lines.extend(body_lines)
                new_lines.append(f"{indent}end else.")
                i = j
                continue

        new_lines.append(line)
        i += 1
    
    content = '\n'.join(new_lines)

    with open(filepath, 'w') as f:
        f.write(content)

if __name__ == '__main__':
    fix_bunker_bnk(sys.argv[1])
