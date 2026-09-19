import re
m = re.search(r"(\d{4})-(\d{2})", "date 2026-09-17")
print("X21", m.group(1), m.group(2))
