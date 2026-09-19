function probe(): boolean {
// BEGIN PROBE F04.P3
const first = { id: 5 }
const second = [first][0]
const same = first === second
return same
// END PROBE F04.P3
}

console.log(probe())
