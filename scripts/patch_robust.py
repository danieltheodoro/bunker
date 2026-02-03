
path = 'src/selfhost/bunker.bnk'
with open(path, 'r') as f:
    lines = f.readlines()

found = False
for i in range(len(lines)):
    # Look for the import check condition
    if 'return tkImp.value == "import";' in lines[i]:
        # Search forward for 'continue;'
        for j in range(i, i+100):
             if 'continue;' in lines[j]:
                  # Check if it looks like the end of the import block
                  if 'end then.' in lines[j+1] and 'end if.' in lines[j+2]:
                       print(f"Found block end at {j}")
                       
                       # Insert match 1
                       lines.insert(j+2, "                   else: break; end else.\n")
                       
                       # Outer end then/if should be at j+4, j+5 (original indices j+3, j+4)
                       # Because we inserted 1 line, they are at j+4, j+5.
                       if 'end then.' in lines[j+4] and 'end if.' in lines[j+5]:
                            lines.insert(j+5, "              else: break; end else.\n")
                            found = True
                            break
        if found: break

if found:
    with open(path, 'w') as f:
        f.writelines(lines)
    with open("patched.txt", "w") as f:
        f.write("Success")
    print("Patched bunker.bnk")
else:
    with open("patched.txt", "w") as f:
        f.write("Failed")
    print("Could not find patterns")
