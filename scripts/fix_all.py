import sys
import re

def fix_all_corruption(filepath):
    with open(filepath, 'r') as f:
        content = f.read()

    # 1. Fix Strings: startsWith corruption
    # Pattern:
    # condition:
    #     return Strings;
    # end condition.
    # then:
    #     startsWith: (.*), ";: self: advance;
    pattern_strings_start = re.compile(r'condition:\s+return Strings;\s+end condition\.\s+then:\s+startsWith: (.*?), "; self: advance;', re.MULTILINE)
    content = pattern_strings_start.sub(r'condition:\n            return Strings: startsWith: \1, ":";\n        end condition.\n        then:\n            self: advance;', content)

    # 2. Fix the others again
    content = content.replace('return (self;', 'return self: isAtEnd;')
    content = content.replace('isAtEnd):', '')
    
    # 3. Fix simple == ";
    content = content.replace('== ";', '== ":";')
    content = content.replace('": self: advance;', 'self: advance;')
    
    # 4. Cleanup
    content = content.replace(';;', ';')
    content = content.replace('":";"', '":";')
    content = content.replace('":";;', '":";')
    
    # 5. Fix "ielse:"
    content = content.replace('ielse:', 'else:')

    # 6. Ensure 'end else.'
    # (Simplified version of previous script's logic)
    lines = content.split('\n')
    new_lines = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.strip() == 'else:' and 'end else.' not in lines[i+2:i+10]: # fuzzy check
            # Find matching end if.
            new_lines.append(line)
            i += 1
            while i < len(lines) and lines[i].strip() != 'end if.':
                 new_lines.append(lines[i])
                 i += 1
            if i < len(lines):
                new_lines.append('        end else.')
                new_lines.append(lines[i])
            i += 1
            continue
        new_lines.append(line)
        i += 1
    content = '\n'.join(new_lines)

    with open(filepath, 'w') as f:
        f.write(content)

if __name__ == '__main__':
    fix_all_corruption(sys.argv[1])
