
path = 'src/selfhost/bunker.bnk'
with open(path, 'r') as f:
    lines = f.readlines()

# Line numbers in view_file were 1-indexed.
# 2299: continue; -> index 2298
# 2300: end then. -> index 2299
# 2301: end if.   -> index 2300

idx = 2298
if "continue;" in lines[idx]:
    print("Found 'continue;' at index", idx)
    
    # Check surrounding lines to be sure
    if "end then." in lines[idx+1] and "end if." in lines[idx+2]:
        print("Context confirmed.")
        
        # Insert 'else: break; end else.' between 'end then.' and 'end if.'
        # Indentation should match 'end then.' (which is indented for 'if import')
        indent1 = "                   " # 19 spaces?
        lines.insert(idx+2, f"{indent1}else: break; end else.\n")
        
        # Now 'end if.' is at idx+3.
        # 'end then.' (outer) is at idx+4.
        # 'end if.' (outer) is at idx+5.
        
        if "end then." in lines[idx+4] and "end if." in lines[idx+5]:
             indent2 = "              " # 14 spaces?
             lines.insert(idx+5, f"{indent2}else: break; end else.\n")
             
             with open(path, 'w') as f:
                 f.writelines(lines)
             print("Successfully patched bunker.bnk")
        else:
             print("Outer context mismatch")
             print(lines[idx:idx+6])
    else:
        print("Inner context mismatch")
        print(lines[idx:idx+3])
else:
    print("Could not find 'continue;' at expected index")
    # Search for it?
    for i, line in enumerate(lines):
        if "continue;" in line and "end then." in lines[i+1] and "end if." in lines[i+2] and "end then." in lines[i+3]:
            print(f"Candidate found at {i}")
