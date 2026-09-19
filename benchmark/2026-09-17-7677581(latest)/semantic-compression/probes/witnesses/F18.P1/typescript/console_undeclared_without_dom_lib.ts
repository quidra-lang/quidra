class R { [Symbol.dispose](): void { console.log("disposed"); } }
function f(): void { using r = new R(); console.log("in " + String(r)); }
f();
