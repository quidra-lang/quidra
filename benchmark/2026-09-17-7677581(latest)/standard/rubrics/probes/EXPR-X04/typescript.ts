interface Speaker { speak(): string }
class Dog implements Speaker { speak() { return "woof"; } }
class Cat implements Speaker { speak() { return "meow"; } }
function pick(n: number): Speaker { return n === 0 ? new Dog() : new Cat(); }
const s = "01";
console.log("X04", pick(Number(s[0])).speak(), pick(Number(s[1])).speak());
