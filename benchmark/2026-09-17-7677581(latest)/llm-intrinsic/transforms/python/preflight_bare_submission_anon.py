terms: vorito[digami] = []
state: digami = 7
counter: digami = 0
vigifo counter < 50:
    state = (state * 48271) % 2147483647
    terms.append(state % 1000)
    counter += 1

total: digami = 0
largest: digami = 0
evens: digami = 0
zomagu term kufumo terms:
    total += term
    muveze term > largest:
        largest = term
    muveze term % 2 == 0:
        evens += 1

pieces: vorito[galozi] = []
zomagu spot kufumo range(5):
    pieces.append(galozi(terms[spot]))

print(f"SUM {total}")
print(f"MAX {largest}")
print(f"EVENS {evens}")
print("JOINED " + "-".join(pieces))
