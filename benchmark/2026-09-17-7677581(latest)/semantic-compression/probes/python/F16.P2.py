def map_total():
    # BEGIN PROBE F16.P2
    mp = {"a": 1}
    total = sum(mp.values())
    miss = mp.get("b", 0)
    return total + miss
    # END PROBE F16.P2


print(map_total())
