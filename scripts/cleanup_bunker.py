import sys
import re

def fix_bunker_bnk(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Aggressive cleanup of redundant semicolons and quotes
    content = content.replace(';;', ';')
    content = content.replace('";;', '";')
    content = content.replace('":";;', '":";')
    content = content.replace('":";"', '":";')
    
    # Fix the common patterns again just in case
    content = re.sub(r'return (.*) == ":";;', r'return \1 == ":";', content)
    content = re.sub(r'return (.*) == ":";;', r'return \1 == ":";', content) # repeat for triple
    
    with open(filepath, 'w') as f:
        f.write(content)

if __name__ == '__main__':
    fix_bunker_bnk(sys.argv[1])
