function collect(count: number): number[] {
    const out: number[] = [];
    let state: number = 7;
    for (let step: number = 0; step < count; step = step + 1) {
        state = (state * 48271) % 2147483647;
        out.push(state % 1000);
    }
    return out;
}

function bigger(left: number, right: number): number {
    if (left > right) {
        return left;
    } else {
        return right;
    }
}

function main(): void {
    const items: number[] = collect(50);
    let total: number = 0;
    let biggest: number = items[0];
    let evens: number = 0;
    for (const item of items) {
        total = total + item;
        biggest = bigger(biggest, item);
        if (item % 2 === 0) {
            evens = evens + 1;
        }
    }
    const pieces: string[] = [];
    for (let index: number = 0; index < 5; index = index + 1) {
        pieces.push(String(items[index]));
    }
    const joined: string = pieces.join("-");
    console.log("SUM " + String(total));
    console.log("MAX " + String(biggest));
    console.log("EVENS " + String(evens));
    console.log("JOINED " + joined);
}

main();
