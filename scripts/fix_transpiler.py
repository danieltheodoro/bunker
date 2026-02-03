#!/usr/bin/env python3
"""
Fix the emitCall method in transpiler.bnk by adding missing 'end if.' terminators.
"""

with open("src/selfhost/transpiler.bnk", "r") as f:
    lines = f.readlines()

# Find the emitCall method (starts around line 221)
in_emit_call = False
fixed_lines = []
depth = 0
line_num = 0

for i, line in enumerate(lines):
    line_num = i + 1
    stripped = line.strip()
    
    # Track when we enter emitCall
    if "has method emitCall:" in line:
        in_emit_call = True
        depth = 0
    
    # Track depth if we're in emitCall
    if in_emit_call:
        # Count if statements
        if stripped.startswith("if ") and stripped.endswith(":"):
            if "break; end if." not in stripped:
                depth += 1
        elif stripped == "else:":
            pass  # else doesn't change depth
        elif stripped == "end if.":
            depth -= 1
        
        # End of method
        if stripped == "end method." and in_emit_call:
            # Before ending, ensure depth is 0
            while depth > 0:
                # Add missing end if before the end method
                indent = "         " + ("    " * depth)
                fixed_lines.append(indent + "end if.\n")
                depth -= 1
            fixed_lines.append(line)
            in_emit_call = False
            continue
    
    fixed_lines.append(line)

# Write back
with open("src/selfhost/transpiler.bnk", "w") as f:
    f.writelines(fixed_lines)

print("Fixed transpiler.bnk")
