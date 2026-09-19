class V { constructor(public x: number, public y: number) {}
  [Symbol.toPrimitive](hint: string): number { return this.x * 100 + this.y; } }
const sum = (new V(1, 2) as any) + (new V(3, 4) as any);   // user-defined behaviour for '+'
console.log("X10", Math.floor(sum / 100), sum % 100);
