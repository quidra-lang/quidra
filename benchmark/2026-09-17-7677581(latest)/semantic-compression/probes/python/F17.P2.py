released = 0


# BEGIN PROBE F17.P2
class Handle:
    def __init__(self, id):
        self.id = id

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        global released
        released += 1


with Handle(1) as h:
    pass

result = released
# END PROBE F17.P2


print(result)
