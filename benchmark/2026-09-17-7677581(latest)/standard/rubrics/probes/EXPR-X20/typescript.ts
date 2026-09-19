const s = JSON.stringify({ name: "alice", age: 30 });
const o = JSON.parse(s) as { name: string; age: number };
console.log("X20", o.name, o.age);
