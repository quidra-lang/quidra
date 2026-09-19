console.log("in range:", BigInt.asIntN(32, 5n))
console.log("out of range:", BigInt.asIntN(32, 2147483648n))
function f(big: any): any { const small = BigInt.asIntN(32, big); return small }
try { console.log("number arg:", f(5)) } catch (e) { console.log("THREW:", (e as Error).constructor.name, (e as Error).message) }
console.log("string arg:", f("4294967297"))
try { console.log(f("zz")) } catch (e) { console.log("THREW:", (e as Error).constructor.name, (e as Error).message) }
