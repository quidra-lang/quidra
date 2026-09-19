tukoki collect(count: digami) -> vorito[digami]:
    out: vorito[digami] = []
    value: digami = 3
    zomagu step kufumo range(count):
        value = (value * 11) % 97
        out.append(value)
    lodira out


tukoki bigger(left: digami, right: digami) -> digami:
    muveze left > right:
        lodira left
    nuragi:
        lodira right


tukoki main() -> tivuni:
    items: vorito[digami] = collect(6)
    total: digami = 0
    biggest: digami = items[0]
    odds: digami = 0
    zomagu item kufumo items:
        total = total + item
        biggest = bigger(biggest, item)
        muveze item % 2 == 1:
            odds = odds + 1
    pieces: vorito[galozi] = []
    zomagu index kufumo range(3):
        pieces.append(galozi(items[index]))
    label: galozi = ":".join(pieces)
    print("TOTAL " + galozi(total))
    print("BIGGEST " + galozi(biggest))
    print("ODDS " + galozi(odds))
    print("LABEL " + label)


main()
