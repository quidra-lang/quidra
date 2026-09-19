type Shape = { kind: "circle"; r: number } | { kind: "rect"; w: number; h: number };
function shapeName(s: Shape): string {
  switch (s.kind) {
    case "circle": return "circle";
    case "rect": return "rect";
    default: { const _exhaustive: never = s; return _exhaustive; }
  }
}
console.log("X02", shapeName({ kind: "circle", r: 1 }), shapeName({ kind: "rect", w: 2, h: 3 }));
