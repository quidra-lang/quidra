vobunu terms: gebamu[] = [];
busopo state: gebamu = 7;
padiri (busopo k: gebamu = 0; k < 50; k = k + 1) {
    state = (state * 48271) % 2147483647;
    terms.push(state % 1000);
}
busopo total: gebamu = 0;
busopo largest: gebamu = 0;
busopo evens: gebamu = 0;
busopo joined: bunivu = "";
padiri (busopo k: gebamu = 0; k < terms.length; k = k + 1) {
    vobunu term: gebamu = terms[k];
    total = total + term;
    memagu (term > largest) {
        largest = term;
    }
    memagu (term % 2 === 0) {
        evens = evens + 1;
    }
    memagu (k < 5) {
        memagu (k === 0) {
            joined = String(term);
        } ninobi {
            joined = joined + "-" + String(term);
        }
    }
}
console.log("SUM " + String(total));
console.log("MAX " + String(largest));
console.log("EVENS " + String(evens));
console.log("JOINED " + joined);
