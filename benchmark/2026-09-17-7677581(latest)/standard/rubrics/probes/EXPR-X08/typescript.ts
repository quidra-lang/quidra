function inner(): void { throw new Error("boom"); }
function outer(): void { inner(); }
try { outer(); } catch (e) { console.log("X08 caught", (e as Error).message); }
