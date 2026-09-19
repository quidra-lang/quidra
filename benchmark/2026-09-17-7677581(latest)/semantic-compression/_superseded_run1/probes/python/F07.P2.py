def main() -> str:
    a = -7
    b = 3
# BEGIN PROBE F07.P2
    q = a // b
    m = a % b
    d = a / b
# END PROBE F07.P2
    return f"{q} {m} {d}"


print(main())
