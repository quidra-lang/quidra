const s1: string = ["ab", "cd"].join("")
const s2: string = "ab".concat("cd")
// BEGIN PROBE F09.P1
const eq = s1 === s2
// END PROBE F09.P1
console.log(eq)
