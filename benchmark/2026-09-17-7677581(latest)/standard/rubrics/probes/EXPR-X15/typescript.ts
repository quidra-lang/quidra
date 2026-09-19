const t = "h\u00e9llo";
let n = 0;
for (const cp of t) { void cp; n++; }
console.log("X15", n);
