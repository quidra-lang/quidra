def caller():
    # BEGIN PROBE F06.P2
    def divmod2(a, b):
        return divmod(a, b)

    q, r = divmod2(7, 3)
    return q + r
    # END PROBE F06.P2


print(caller())
