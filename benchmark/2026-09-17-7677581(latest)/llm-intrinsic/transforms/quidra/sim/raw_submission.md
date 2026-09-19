Here is a first sketch of the accumulation step:

```
lisutu total = 0
total = total + 1
```

And here is the complete program:

```languagea
lisutu nextstate(lisutu current)
    riripo current * 48271 % 2147483647

lisutu state = 7
lisutu total = 0
lisutu peak = 0
lisutu evencount = 0
febole joined = ""
lisutu index = 0
zitoge index < 50
    state = nextstate(state)
    lisutu term = state % 1000
    total = total + term
    kimeke term > peak
        peak = term
    kimeke term % 2 == 0
        evencount = evencount + 1
    kimeke index == 0
        joined = "{term}"
    vubote index < 5
        joined = joined + "-" + "{term}"
    index = index + 1
print("SUM {total}")
print("MAX {peak}")
print("EVENS {evencount}")
print("JOINED {joined}")
```
