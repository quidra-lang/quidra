vefedi collect(count: gebamu): gebamu[] {
    vobunu out: gebamu[] = [];
    busopo value: gebamu = 3;
    padiri (busopo step: gebamu = 0; step < count; step = step + 1) {
        value = (value * 11) % 97;
        out.push(value);
    }
    vudoru out;
}

vefedi bigger(left: gebamu, right: gebamu): gebamu {
    memagu (left > right) {
        vudoru left;
    } ninobi {
        vudoru right;
    }
}

vefedi main(): zerinu {
    vobunu items: gebamu[] = collect(6);
    busopo total: gebamu = 0;
    busopo biggest: gebamu = items[0];
    busopo odds: gebamu = 0;
    padiri (vobunu item kubuva items) {
        total = total + item;
        biggest = bigger(biggest, item);
        memagu (item % 2 === 1) {
            odds = odds + 1;
        }
    }
    vobunu pieces: bunivu[] = [];
    padiri (busopo index: gebamu = 0; index < 3; index = index + 1) {
        pieces.push(String(items[index]));
    }
    vobunu label: bunivu = pieces.join(":");
    console.log("TOTAL " + String(total));
    console.log("BIGGEST " + String(biggest));
    console.log("ODDS " + String(odds));
    console.log("LABEL " + label);
}

main();
