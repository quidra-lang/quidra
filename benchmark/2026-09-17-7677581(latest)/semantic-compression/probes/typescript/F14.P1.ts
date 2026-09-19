function newShape() {
// BEGIN PROBE F14.P1
interface Circle { kind: "Circle"; r: number }
interface Rect { kind: "Rect"; w: number; h: number }
type Shape = Circle | Rect
const s: Shape = { kind: "Circle", r: 2.0 }
// END PROBE F14.P1
return s
}

console.log(JSON.stringify(newShape()))
