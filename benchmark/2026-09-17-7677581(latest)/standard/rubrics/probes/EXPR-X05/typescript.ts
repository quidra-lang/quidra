function makeAdder(n: number): (x: number) => number { return (x) => x + n; }
console.log("X05", makeAdder(10)(5));
