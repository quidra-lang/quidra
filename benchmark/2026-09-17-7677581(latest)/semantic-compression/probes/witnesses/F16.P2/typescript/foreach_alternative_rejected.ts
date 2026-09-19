function mapTotal(): number {
// BEGIN PROBE F16.P2
const mp = new Map([["a", 1]]);
let total = 0;
mp.forEach(v => total += v);
const miss = mp.get("b") ?? 0;
return total + miss;
// END PROBE F16.P2
}
console.log(mapTotal());
