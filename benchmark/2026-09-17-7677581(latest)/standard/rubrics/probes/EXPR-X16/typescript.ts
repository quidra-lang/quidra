interface HasVal { val(): number }
function get<T extends HasVal>(t: T): number { return t.val(); }
class C implements HasVal { val() { return 7; } }
console.log("X16", get(new C()));
