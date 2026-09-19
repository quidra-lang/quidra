import sys


def probe() -> int:
    # BEGIN PROBE F05.P2
    def put(name):
        sys._getframe(1).f_locals[name] = 12

    cell = 3
    put("cell")
    return cell
    # END PROBE F05.P2


print(probe())
