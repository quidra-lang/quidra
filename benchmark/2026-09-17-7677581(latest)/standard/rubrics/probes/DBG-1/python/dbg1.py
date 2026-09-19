# DBG-1: aggregate with named int + named string field, dynamic array of 3,
# a function that loops over the array, prints a checksum, runs >= 2s CPU.
ITERATIONS = 8000000
MODULUS = 1000000007


class Item:
    def __init__(self, count, name):
        self.count = count
        self.name = name


def accumulate(items, iterations):
    total = 0
    for _ in range(iterations):
        for item in items:
            total = (total * 31 + item.count + len(item.name)) % MODULUS
    return total


def main():
    items = []
    items.append(Item(7, "alpha"))
    items.append(Item(11, "bravo"))
    items.append(Item(13, "charlie"))
    checksum = accumulate(items, ITERATIONS)
    print("checksum={}".format(checksum))


main()
