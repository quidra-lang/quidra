console.log("ADV-START");

const xs: number[] = [1, 2, 3, 4, 5];
let iters: number = 0;
for (const x of xs) {
    iters += 1;
    if (x === 2) {
        xs.push(99);
    }
}

console.log("OBS=ITERS:" + String(iters) + "|LEN:" + String(xs.length));
console.log("ADV-END");
