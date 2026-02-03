#!/usr/bin/env python3
"""
Comprehensive fix for all missing 'end if.' terminators in transpiler.bnk
"""

with open("src/selfhost/transpiler.bnk", "r") as f:
    lines = f.readlines()

fixed_lines = []
method_stack = []
depth_stack = []

for i, line in enumerate(lines):
    line_num = i + 1
    stripped = line.strip()
    
    # Track method boundaries
    if "has method " in line or "has static method " in line:
        method_name = line.split("has method ")[-1].split("has static method ")[-1].split(":")[0].strip()
        method_stack.append((method_name, line_num))
        depth_stack.append(0)
    
    # Track depth within current method
    if method_stack and depth_stack:
        # Skip shorthand if statements
        if "break; end if." in stripped or (": return; end if." in stripped):
            fixed_lines.append(line)
            continue
            
        # Count if statements
        if (stripped.startswith("if ") or stripped.startswith("let ") and " if " in stripped) and stripped.endswith(":"):
            depth_stack[-1] += 1
        elif stripped == "else:":
            pass  # else doesn't change depth
        elif stripped == "end if.":
            if depth_stack[-1] > 0:
                depth_stack[-1] -= 1
        elif stripped == "loop:":
            depth_stack[-1] += 1
        elif stripped == "end loop.":
            if depth_stack[-1] > 0:
                depth_stack[-1] -= 1
        
        # End of method - add missing terminators
        if stripped == "end method.":
            indent_level = len(line) - len(line.lstrip())
            while depth_stack[-1] > 0:
                # Add missing end if with proper indentation
                fixed_lines.append(" " * (indent_level + 4) + "end if.\n")
                depth_stack[-1] -= 1
                print(f"Added missing 'end if.' before line {line_num} in method {method_stack[-1][0]}")
            
            method_stack.pop()
            depth_stack.pop()
    
    fixed_lines.append(line)

# Write back
with open("src/selfhost/transpiler.bnk", "w") as f:
    f.writelines(fixed_lines)

print(f"Fixed transpiler.bnk - processed {len(lines)} lines")
