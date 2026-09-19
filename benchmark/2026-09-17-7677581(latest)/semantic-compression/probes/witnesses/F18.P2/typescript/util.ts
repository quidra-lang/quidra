// BEGIN PROBE F18.P2
export function pubAdd(a: number, b: number) {
  return a + b + secret();
}

function secret() {
  return 1;
}
// END PROBE F18.P2
