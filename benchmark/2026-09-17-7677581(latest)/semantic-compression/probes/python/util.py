# BEGIN PROBE F18.P2
def _secret():
    return 1


def pub_add(a, b):
    return a + b + _secret()
# END PROBE F18.P2
