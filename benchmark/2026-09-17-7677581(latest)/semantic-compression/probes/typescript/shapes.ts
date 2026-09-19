// BEGIN PROBE F14.P3
export interface Shape {
area(): number
}

export class Circle implements Shape {
constructor(private r: number) {}
area(): number {
return 3.141592653589793 * this.r * this.r
}
}

export class Rect implements Shape {
constructor(private w: number, private h: number) {}
area(): number {
return this.w * this.h
}
}
// END PROBE F14.P3
