class Handle { constructor(readonly id: number) {} }
const h = new Handle(1);
h.id = 2;
console.log(h.id);
