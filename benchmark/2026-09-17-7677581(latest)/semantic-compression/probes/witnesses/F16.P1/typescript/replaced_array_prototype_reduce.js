Array.prototype.reduce = function () { globalThis.seen = true; return { hijacked: true }; };
function sequenceTotal() {
  const xs = [1, 2, 3];
  const total = xs.reduce((a, b) => a + b);
  return total;
}
console.log(JSON.stringify(sequenceTotal()), "globalThis.seen =", globalThis.seen);
