function collect(count: number): number[] {
    const out: number[] = [];
    let value: number = 3;
    for (let step: number = 0; step < count; step = step + 1) {
        value = (value * 11) % 97;
        out.push(value);
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
    const items: number[] = collect(6);
    let total: number = 0;
    let biggest: number = items[0];
    let odds: number = 0;
    for (const item of items) {
        total = total + item;
        biggest = bigger(biggest, item);
        if (item % 2 === 1) {
            odds = odds + 1;
        }
    }
    const pieces: string[] = [];
    for (let index: number = 0; index < 3; index = index + 1) {
        pieces.push(String(items[index]));
    }
    const label: string = pieces.join(":");
    console.log("TOTAL " + String(total));
    console.log("BIGGEST " + String(biggest));
    console.log("ODDS " + String(odds));
    console.log("LABEL " + label);
}

main();
