vefedi main(): zerinu {
    vobunu terms: gebamu[] = [];
    busopo state: gebamu = 7;
    busopo made: gebamu = 0;
    biremo (made < 50) {
        state = (state * 48271) % 2147483647;
        terms.push(state % 1000);
        made = made + 1;
    }
    busopo total: gebamu = 0;
    busopo largest: gebamu = terms[0];
    busopo evens: gebamu = 0;
    padiri (vobunu term kubuva terms) {
        total = total + term;
        memagu (term > largest) {
            largest = term;
        }
        memagu (term % 2 === 0) {
            evens = evens + 1;
        }
    }
    vobunu head: bunivu[] = [];
    padiri (busopo i: gebamu = 0; i < 5; i = i + 1) {
        head.push(String(terms[i]));
    }
    console.log("SUM " + String(total));
    console.log("MAX " + String(largest));
    console.log("EVENS " + String(evens));
    console.log(`JOINED ${head.join("-")}`);
}

main();
