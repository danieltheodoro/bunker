import os

def fix_file(path, search, replace):
    with open(path, 'r') as f:
        content = f.read()
    if search in content:
        new_content = content.replace(search, replace)
        with open(path, 'w') as f:
            f.write(new_content)
        print(f"Fixed {path}")
    else:
        print(f"Could not find search string in {path}")

# Fix bunker.bnk with more debug
bunker_path = "src/selfhost/bunker.bnk"
bunker_search = """                        args: ASTList <- nil;
                        methodName: string <- "";

                        if nextTk.type == "IDENT":"""
bunker_replace = """                        args: ASTList <- nil;
                        methodName: string <- "";
                        print: "DEBUG: parseCall nextTk type=" + nextTk.type + " val=" + nextTk.value;

                        if nextTk.type == "IDENT":"""

fix_file(bunker_path, bunker_search, bunker_replace)
