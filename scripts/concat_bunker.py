
import os

files = [
    "src/std/bunker/std/strings.bnk",
    "src/std/bunker/std/io.bnk",
    "src/std/bunker/std/fs.bnk",
    "src/std/bunker/std/collections.bnk", 
    "src/selfhost/ast.bnk",
    "src/selfhost/transpiler.bnk",
    "src/selfhost/typechecker.bnk",
    "src/selfhost/bunker.bnk"
]

output_lines = []

# Add module decl at top
output_lines.append("module bunker;\n\n")

for fname in files:
    print(f"Processing {fname}...")
    if not os.path.exists(fname):
        print(f"Error: {fname} does not exist")
        continue
        
    with open(fname, 'r') as f:
        lines = f.readlines()
        
    for line in lines:
        stripped = line.strip()
        # Skip module decls (we added one at top)
        if stripped.startswith("module "):
            continue
        # Skip includes (we are concatenating)
        if stripped.startswith("include "):
            continue
        # Skip imports (if any, assuming concatenation solves it)
        if stripped.startswith("import "):
             continue
             
        output_lines.append(line)
        
    output_lines.append("\n") # spacing

with open("all_bunker.bnk", "w") as f:
    f.writelines(output_lines)

print("Created all_bunker.bnk")
