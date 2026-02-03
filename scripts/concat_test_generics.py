
files = [
    "src/std/bunker/std/strings.bnk",
    "src/std/bunker/std/collections.bnk",
    "test_generics.bnk"
]

output = ["module TestGenerics;\n\n"]

for fpath in files:
    with open(fpath, 'r') as f:
        for line in f:
            l = line.strip()
            if l.startswith("module ") or l.startswith("include ") or l.startswith("import "):
                continue
            output.append(line)
    output.append("\n")

with open("test_generics_full.bnk", "w") as f:
    f.writelines(output)
