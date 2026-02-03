#!/usr/bin/env python3
"""
Fix all unbalanced methods in bunker.bnk by adding missing terminators
"""

with open("src/selfhost/bunker.bnk") as f:
    lines = f.readlines()

# Find all unbalanced methods
current_method = None
depth = 0
method_start = 0
fixes_needed = {}

for i, line in enumerate(lines):
    line_num = i + 1
    stripped = line.strip()
    
    if 'has method ' in line or 'has static method ' in line:
        if current_method and depth != 0:
            fixes_needed[method_start] = (current_method, depth)
        
        method_name = line.split('has method ')[-1].split('has static method ')[-1].split(':')[0].strip()
        current_method = method_name
        method_start = line_num
        depth = 0
    
    if current_method:
        if 'break; end if.' in stripped:
            continue
            
        if stripped.startswith('if ') and stripped.endswith(':'):
            depth += 1
        elif stripped == 'else:':
            pass
        elif stripped == 'end if.':
            depth -= 1
        elif stripped == 'loop:':
            depth += 1
        elif stripped == 'end loop.':
            depth -= 1
        
        if stripped == 'end method.':
            if depth != 0:
                fixes_needed[line_num] = (current_method, depth)
            current_method = None
            depth = 0

# Apply fixes - insert terminators before 'end method.' lines
new_lines = []
for i, line in enumerate(lines):
    line_num = i + 1
    
    # Check if this is an 'end method.' that needs fixing
    if line_num in fixes_needed:
        method_name, missing_count = fixes_needed[line_num]
        print(f"Fixing {method_name} at line {line_num}: adding {missing_count} terminator(s)")
        
        # Add missing terminators before 'end method.'
        indent = "    "
        for _ in range(missing_count):
            new_lines.append(indent + "end if.\n")
    
    new_lines.append(line)

# Write back
with open("src/selfhost/bunker.bnk", "w") as f:
    f.writelines(new_lines)

print(f"\nFixed {len(fixes_needed)} method(s) in bunker.bnk")
