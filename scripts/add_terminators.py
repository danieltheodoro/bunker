#!/usr/bin/env python3
"""
Add the 9 missing 'end if.' terminators to emitCall method in transpiler.bnk
"""

with open("src/selfhost/transpiler.bnk", "r") as f:
    lines = f.readlines()

# Insert 9 'end if.' terminators before line 342 (0-indexed: 341)
# Line 342 is 'end method.' for emitCall
insert_pos = 341  # Before 'end method.'

# The terminators need proper indentation
terminators = [
    "         end if.\n",  # Close if call.metTok.value == "free"
    "    end if.\n",       # Close if entityName == "Memory"
    "         end if.\n",  # Close if isStatic
    "    end if.\n",       # Close if hRec.type == "VariableExpr"
    "         end if.\n",  # Close if call.receiver != nil
    "    end if.\n",       # Close if self.symbols != nil
    "         end if.\n",  # Close if l.head != nil
    "    end if.\n",       # Close if call.args != nil
    "         end if.\n",  # Close if call.metTok.value == "print"
]

# Insert in reverse order so line numbers don't shift
for terminator in terminators:
    lines.insert(insert_pos, terminator)

with open("src/selfhost/transpiler.bnk", "w") as f:
    f.writelines(lines)

print(f"Added 9 'end if.' terminators before line {insert_pos + 1}")
print("Verifying...")

# Verify
with open("src/selfhost/transpiler.bnk", "r") as f:
    verify_lines = f.readlines()
    
for i in range(insert_pos, insert_pos + 12):
    print(f"Line {i+1}: {verify_lines[i].rstrip()}")
