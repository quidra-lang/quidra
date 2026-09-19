interface Circle { kind: "Circle"; r: number }
interface Rect { kind: "Rect"; w: number; h: number }
type Shape = Circle | Rect

function areaOf(s: Shape): number {
// BEGIN PROBE F14.P2
let area: number
switch (s.kind) {
case "Circle":
area = 3.141592653589793 * s.r * s.r
break
case "Rect":
area = s.w * s.h
break
}
return area
// END PROBE F14.P2
}

console.log(areaOf({ kind: "Circle", r: 2.0 }))
