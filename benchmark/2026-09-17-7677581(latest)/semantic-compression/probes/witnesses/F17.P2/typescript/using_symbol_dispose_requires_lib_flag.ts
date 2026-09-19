let released = 0;

// BEGIN PROBE F17.P2
class Handle {
  constructor(readonly id: number) {}
  [Symbol.dispose]() { released++; }
}
{
  using h = new Handle(1);
}
const result = released;
// END PROBE F17.P2

console.log(result);
