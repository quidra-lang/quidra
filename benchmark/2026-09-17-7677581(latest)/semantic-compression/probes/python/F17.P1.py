# BEGIN PROBE F17.P1
def read_all():
    with open("data.txt") as f:
        text = f.read()
    return len(text.encode())
# END PROBE F17.P1


print(read_all())
