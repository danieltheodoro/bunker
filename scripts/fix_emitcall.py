#!/usr/bin/env python3
"""
Precisely remove the 9 extra 'end if.' terminators from emitCall method
"""

with open("src/selfhost/transpiler.bnk", "r") as f:
    lines = f.readlines()

# Remove lines 342-350 (0-indexed: 341-349) which are the extra 'end if.' terminators
# Keep line 351 (0-indexed: 350) which is 'end method.'
new_lines = lines[:341] + lines[350:]

with open("src/selfhost/transpiler.bnk", "w") as f:
    f.writelines(new_lines)

print("Removed 9 extra 'end if.' terminators from emitCall method")
