// MB-08 -- Strings: text construction plus five character-level passes.
// NW = 200000 words, R = 20.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-08.
// The text is ASCII, so the byte view and the string view agree; passes 1-4
// mutate the text in place, so the working representation is a Uint8Array --
// the mutable working buffer pinned for the `typescript` configuration by the
// binding table in section 4.12(b).
// Pass 5 runs on the language's own `string` type, constructed fresh from the
// working buffer inside each timed round, and accessed with `charCodeAt(i)`,
// which is what section 4.12(b) pins for this configuration.
// Every per-round pass is the explicit character loop: no toUpperCase, no
// reverse(), no indexOf/regex for cnt_ab, no split() for cnt_w, no reuse of the
// U and V buffers between rounds, no caching of the pass-5 string.
//
// The `process` and `require` declarations are local because the frozen build
// recipe compiles a single .ts file with no @types/node package available.

declare const process: {
  argv: string[];
  hrtime: { bigint(): bigint };
};
declare function require(id: string): any;

const NW = 200000;
const R = 20;
const Q = 1000000007;
const SEED = 20268917;

/** Park-Miller / MINSTD generator: state = (48271 * state) mod 2147483647. */
class Lehmer {
  private state: number;

  constructor(seed: number) {
    this.state = seed;
  }

  nextInt(): number {
    this.state = (48271 * this.state) % 2147483647;
    return this.state;
  }
}

/** Order-sensitive rolling hash over `len` character codes. */
function rhash(seq: Uint8Array, len: number): number {
  let h = 0;
  for (let i = 0; i < len; i++) {
    h = (h * 131 + seq[i]) % 1000000007;
  }
  return h;
}

interface Mb08Result {
  acc: number;
  len: number;
  cntAb: number;
  cntW: number;
}

function workload(): Mb08Result {
  const gen = new Lehmer(SEED);

  const words: string[] = [];
  for (let w = 0; w < NW; w++) {
    const L = 4 + (gen.nextInt() % 13); // length 4..16
    const codes: number[] = new Array(L);
    for (let c = 0; c < L; c++) {
      codes[c] = 97 + (gen.nextInt() % 26); // 'a' + ...
    }
    words.push(String.fromCharCode(...codes));
  }
  const joined = words.join(" ");
  const len = joined.length;

  // Mutable ASCII view of the joined text.
  const text = new Uint8Array(len);
  for (let i = 0; i < len; i++) {
    text[i] = joined.charCodeAt(i);
  }
  const decoder = new TextDecoder();

  let acc = 0;
  let cntAb = 0;
  let cntW = 0;
  for (let r = 0; r < R; r++) {
    const p = 7 * r + 11; // anti-elimination (section 2.6)
    if (text[p] === 32) {
      text[p] = 120; // 'x'
    } else {
      text[p] = 97 + ((text[p] - 97 + 1) % 26);
    }

    const h1 = rhash(text, len); // pass 1

    const u = new Uint8Array(len); // pass 2: upper-case
    for (let i = 0; i < len; i++) {
      const c = text[i];
      u[i] = c >= 97 && c <= 122 ? c - 32 : c;
    }
    const h2 = rhash(u, len);

    const v = new Uint8Array(len); // pass 3: reverse
    for (let i = 0; i < len; i++) {
      v[i] = text[len - 1 - i];
    }
    const h3 = rhash(v, len);

    cntAb = 0; // pass 4: naive search for "ab"
    for (let i = 0; i < len - 1; i++) {
      if (text[i] === 97 && text[i + 1] === 98) {
        cntAb = cntAb + 1;
      }
    }

    // Pass 5: word count on the language's own string type. `S` is built fresh
    // from the working buffer inside the timed round and is read with the
    // pinned `charCodeAt(i)` accessor, never as a byte view (section 4.12(b)).
    const s = decoder.decode(text);
    cntW = 1;
    for (let i = 0; i < len; i++) {
      if (s.charCodeAt(i) === 32) {
        cntW = cntW + 1;
      }
    }

    for (const x of [h1, h2, h3, cntAb, cntW]) {
      acc = (acc * 31 + x) % Q;
    }
  }

  return { acc, len, cntAb, cntW };
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  // Each steady iteration re-seeds the generator to SEED and rebuilds the words,
  // the join and the working buffer inside the timed region, so it performs
  // exactly the work `once` performs.
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let res: Mb08Result;
  if (mode === "steady") {
    const raw = process.argv.length > 3 ? Number(process.argv[3]) : 7;
    const iterations = raw >= 1 ? Math.floor(raw) : 7;
    const fs = require("node:fs");
    res = { acc: 0, len: 0, cntAb: 0, cntW: 0 };
    for (let k = 0; k < iterations; k++) {
      const t0 = process.hrtime.bigint();
      res = workload();
      const elapsed = process.hrtime.bigint() - t0;
      // fs.writeSync(1, ..) is Node's synchronous stdout write: section 5.2
      // requires each ITER line to be flushed immediately.
      fs.writeSync(1, `ITER ${k} ${elapsed}\n`);
    }
  } else {
    res = workload();
  }

  console.log(`MB08 ${res.acc} ${res.len} ${res.cntAb} ${res.cntW}`);
}

main();
