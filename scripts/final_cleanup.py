import sys
import re

def final_cleanup(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    
    # 1. Fix corrupted SYMBOL check: return ... == ";
    # This was likely return ... == ":";
    content = content.replace('== ";', '== ":";')
    
    # 2. Cleanup tripled/doubled semicolons/quotes
    content = content.replace('":";"', '":";')
    content = content.replace('":";;', '":";')
    content = content.replace('":";', '":";')
    content = content.replace(';;', ';')
    
    # 3. Fix line 3277 specifically (was handled by fix_bunker_corruption but just in case)
    content = content.replace('":"; or', '":" or')
    
    with open(filepath, 'w') as f:
        f.write(content)

if __name__ == '__main__':
    final_cleanup(sys.argv[1])
