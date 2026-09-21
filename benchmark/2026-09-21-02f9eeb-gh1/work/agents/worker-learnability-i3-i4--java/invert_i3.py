import re
with open('i3_t1_raw.txt') as f:
    s = f.read()
s = s.replace('BEGIN', '{').replace('END', '}').replace('~', ';')
with open('Calculator.java', 'w') as f:
    f.write(s)
print(s)
