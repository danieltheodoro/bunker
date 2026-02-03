import subprocess
import os

report = []

def log(msg):
    print(msg)
    report.append(msg)

def run(cmd):
    log(f"Running: {cmd}")
    try:
        res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        log(f"Exit: {res.returncode}")
        log(f"Stdout:\n{res.stdout}")
        log(f"Stderr:\n{res.stderr}")
        return res
    except Exception as e:
        log(f"Error: {e}")

log("--- Testing C compiler ---")
# Ensure test_c.c exists
with open("test_c.c", "w") as f:
    f.write('#include <stdio.h>\nint main() { printf("Hello from C\\n"); return 0; }\n')

run("gcc -o test_c test_c.c && ./test_c")

log("--- Testing Bunker v2 with test_import.bnk ---")
if os.path.exists("./bnk_selfhost_vNext2"):
    run("./bnk_selfhost_vNext2 test_import.bnk")
else:
    log("Error: bnk_selfhost_vNext2 not found")

log("--- Checking for output files ---")
if os.path.exists("test_import.bnk.c"):
    log("SUCCESS: test_import.bnk.c generated")
    try:
        with open("test_import.bnk.c") as f:
            log("Content head:\n" + f.read()[:500])
    except Exception as e:
        log(f"Error reading c file: {e}")
else:
    log("FAILURE: test_import.bnk.c NOT generated")

with open("verify_report.txt", "w") as f:
    f.write("\n".join(report))
