def main() -> int:
    big = 2**40
# BEGIN PROBE F10.P1
    small = big if -2**31 <= big < 2**31 else 0
    return small
# END PROBE F10.P1


print(main())
