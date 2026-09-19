let released = 0;

class Handle {
  constructor(readonly id: number) {}
  close() { released++; }
}
{
  const h = new Handle(1);
}
const result = released;

console.log(result);
