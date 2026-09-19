// MB-01 -- Fibonacci: naive double recursion, n = 30..37 inclusive.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-01.
// No memoisation, no iterative rewrite, no closed form, no caching between the
// eight top-level calls.
//
// The `process` and `require` declarations are local because the frozen build
// recipe compiles a single .ts file with no @types/node package available.

declare const process: {
  argv: string[];
  hrtime: { bigint(): bigint };
};
declare function require(id: string): any;

function fib(n: number): number {
  if (n < 2) {
    return n;
  }
  return fib(n - 1) + fib(n - 2);
}

function workload(): number {
  let total = 0;
  for (let n = 30; n <= 37; n++) {
    total = total + fib(n);
  }
  return total;
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let total = 0;
  if (mode === "steady") {
    const raw = process.argv.length > 3 ? Number(process.argv[3]) : 7;
    const iterations = raw >= 1 ? Math.floor(raw) : 7;
    const fs = require("node:fs");
    for (let k = 0; k < iterations; k++) {
      const t0 = process.hrtime.bigint();
      total = workload();
      const elapsed = process.hrtime.bigint() - t0;
      // fs.writeSync(1, ..) is Node's synchronous stdout write: section 5.2
      // requires each ITER line to be flushed immediately.
      fs.writeSync(1, `ITER ${k} ${elapsed}\n`);
    }
  } else {
    total = workload();
  }
  console.log(`MB01 ${total}`);
}

main();
