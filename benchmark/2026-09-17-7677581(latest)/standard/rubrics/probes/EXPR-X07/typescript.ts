class Upto { constructor(private n: number) {}
  [Symbol.iterator]() { let i = 0; const n = this.n;
    return { next(): IteratorResult<number> { return i < n ? { value: i++, done: false } : { value: undefined as any, done: true }; } }; } }
const out: number[] = [];
for (const v of new Upto(3)) out.push(v);
console.log("X07", out.join(" "));
