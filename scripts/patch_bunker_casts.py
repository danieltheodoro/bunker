import re

with open('src/selfhost/bunker.bnk', 'r') as f:
    content = f.read()

# Helper to avoid the cast issue
content = content.replace('if body.terminatedByDot == false:', 'if (Casts: isBlockTerminatedByDot: body) == false:')
content = content.replace('if thenBlock.terminatedByDot == false:', 'if (Casts: isBlockTerminatedByDot: thenBlock) == false:')

# Update parseMethod check too
content = content.replace('print: "DEBUG: parseMethod end check. tkEnd=" + tkEnd.value + " tkNext=" + tkNext.value + " termByDot=" + body.terminatedByDot;',
                          'print: "DEBUG: parseMethod end check. tkEnd=" + tkEnd.value + " tkNext=" + tkNext.value + " termByDot=" + (Casts: isBlockTerminatedByDot: body);')

# Change Block* back to void* in signatures where needed if bootstrap fails, 
# but let's try keeping body: Block if it works with Casts.

with open('src/selfhost/bunker.bnk', 'w') as f:
    f.write(content)
