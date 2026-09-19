import type { Shape } from "./shapes.js"

// BEGIN PROBE F14.P3
class Tri implements Shape {
constructor(private b: number, private h: number) {}
area(): number {
return 0.5 * this.b * this.h
}
}

function triArea(): number {
const s: Shape = new Tri(3.0, 4.0)
return s.area()
}
// END PROBE F14.P3

console.log(triArea())
