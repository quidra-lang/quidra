// Witness for F18.P1 determinacy reading 2: `console` is a writable host global.
// $ node replaced_console_log.js   ->  prints only "after: seen=true"; the "x" never reaches stdout.
console.log = () => { globalThis.seen = true; };
console.log("x");
process.stdout.write("after: seen=" + globalThis.seen + "\n");
