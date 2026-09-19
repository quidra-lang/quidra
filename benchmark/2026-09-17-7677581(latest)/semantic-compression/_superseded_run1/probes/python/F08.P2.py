def main() -> int:
    a = 7
    b = 0
# BEGIN PROBE F08.P2
    q = 0 if b == 0 else a // b
    return q
# END PROBE F08.P2


print(main())
