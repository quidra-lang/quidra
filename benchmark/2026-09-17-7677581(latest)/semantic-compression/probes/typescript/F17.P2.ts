let released = 0;

// BEGIN PROBE F17.P2
class Handle {
  constructor(readonly id: number) {}
  close() { released++; }
}
{
  const h = new Handle(1);
  try { } finally { h.close(); }
}
const result = released;
// END PROBE F17.P2

console.log(result);
