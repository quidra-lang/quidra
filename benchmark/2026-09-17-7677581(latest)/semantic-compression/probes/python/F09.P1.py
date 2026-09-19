s1 = "".join(("ab", "cd"))
s2 = "".join(("ab", "cd"))
# BEGIN PROBE F09.P1
eq = s1 == s2
# END PROBE F09.P1
print(eq, s1 is s2)
