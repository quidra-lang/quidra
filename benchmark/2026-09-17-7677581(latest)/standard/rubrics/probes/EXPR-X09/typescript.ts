let log = "";
function f(): void { try { throw new Error("boom"); } finally { log += "X09 cleanup "; } }
try { f(); } catch (e) { log += "caught"; }
console.log(log);
