def main() -> bool:
    s1 = "".join(["a", "b", "c"])
    s2 = "".join(["ab", "c"])
# BEGIN PROBE F09.P1
    eq = s1 == s2
# END PROBE F09.P1
    return eq


print(main())
