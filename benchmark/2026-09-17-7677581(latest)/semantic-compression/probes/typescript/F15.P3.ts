// BEGIN PROBE F15.P3
interface Named {
tag(): string
}

class A implements Named {
tag(): string {
return "a"
}
}

class B implements Named {
tag(): string {
return "b"
}
}

function firstTag(): string {
const items: Named[] = [new A(), new B()]
return items[0].tag()
}
// END PROBE F15.P3

console.log(firstTag())
