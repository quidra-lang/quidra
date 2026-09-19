const l: number[] = [1, 2, 3];
const m = new Map<string, number>([["a", 1], ["b", 2]]);
const s = new Set<number>([1, 2, 3, 1]);
console.log("X14", l.length, m.get("b"), s.size);
