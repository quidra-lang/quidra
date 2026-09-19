function generate(count: number): number[] {
    const terms: number[] = [];
    let state: number = 7;
    for (let step: number = 0; step < count; step = step + 1) {
        state = (state * 48271) % 2147483647;
        terms.push(state % 1000);
    }
    return terms;
}

function main(): void {
    const terms: number[] = generate(50);
    let total: number = 0;
    let largest: number = terms[0];
    let evens: number = 0;
    for (const term of terms) {
        total = total + term;
        if (term > largest) {
            largest = term;
        }
        if (term % 2 === 0) {
            evens = evens + 1;
        }
    }
    const pieces: string[] = [];
    for (let index: number = 0; index < 5; index = index + 1) {
        pieces.push(String(terms[index]));
    }
    const joined: string = pieces.join("-");
    console.log("SUM " + String(total));
    console.log("MAX " + String(largest));
    console.log("EVENS " + String(evens));
    console.log("JOINED " + joined);
}

main();
