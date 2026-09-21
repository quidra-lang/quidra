import re
with open('Registry.java') as f:
    lines = f.readlines()

# find method signature lines and preceding marker
sig_re = re.compile(r'^\s*public static (\S+(?:\[\])?) (\w+)\(')
results = []
for i, line in enumerate(lines):
    m = sig_re.match(line)
    if m:
        rtype, name = m.group(1), m.group(2)
        marker_line = lines[i-1].strip()
        if rtype == 'void':
            expected = '// RC-VOID'
        elif rtype.endswith('[]'):
            expected = '// RC-ARRAY'
        elif rtype in ('int','double','boolean','long','char','float','short','byte'):
            expected = '// RC-PRIM'
        else:
            expected = '// RC-OBJ'
        ok = marker_line == expected
        results.append((name, rtype, expected, marker_line, ok))

all_ok = all(r[4] for r in results)
for r in results:
    print(r)
print('ALL_OK' if all_ok else 'FAIL')
