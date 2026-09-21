with open('i3_t2_raw.txt') as f:
    s = f.read()
s = s.replace('<<', '{').replace('>>', '}').replace(';;', ',').replace('!!', ';')
with open('Shapes.java', 'w') as f:
    f.write(s)
print(s)
