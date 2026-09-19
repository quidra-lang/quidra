// MB-10 -- File I/O: buffered text write, read back, integer formatting/parsing.
// N = 1000000 lines per file, R = 3 files.
//
// Frozen workload definition: methodology/06_micro_workloads.md section 4, MB-10.
// Writer and reader are pinned for the `typescript` configuration by the binding
// table in section 4.12(c): a manual 65536-byte `Buffer` flushed with
// `fs.writeSync`, and a manual 65536-byte `Buffer` filled with `fs.readSync`
// with carry-over line splitting. Node has no synchronous buffered line writer,
// and `fs.createWriteStream` accumulates unboundedly, which section 2.5 forbids;
// the frozen buffer size is 65536 bytes for every configuration.
// Lines are written one at a time into the buffer, which is flushed whenever the
// next line would not fit; the file content is never assembled in memory.
//
// The "node:fs" types and the `Buffer` / `process` globals are declared locally
// because the frozen build recipe compiles a single .ts file with no @types/node
// package available.

declare const process: {
  argv: string[];
  hrtime: { bigint(): bigint };
};

declare function require(id: string): unknown;

interface NodeBuffer {
  write(text: string, offset: number, encoding: string): number;
  toString(encoding: string, start: number, end: number): string;
}

interface BufferConstructor {
  alloc(size: number): NodeBuffer;
}

declare const Buffer: BufferConstructor;

interface FsModule {
  openSync(path: string, flags: string): number;
  closeSync(fd: number): void;
  writeSync(fd: number, buffer: NodeBuffer, offset: number, length: number): number;
  writeSync(fd: number, text: string): number;
  readSync(
    fd: number,
    buffer: NodeBuffer,
    offset: number,
    length: number,
    position: number | null,
  ): number;
}

const fs = require("node:fs") as FsModule;

const N = 1000000;
const R = 3;
const SEED = 20270917;
const BUFSIZE = 65536; // section 4.12(c): frozen for every configuration

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

interface Mb10Result {
  sumV: number;
  chk: number;
  nbytes: number;
  lines: number;
}

function workload(): Mb10Result {
  const gen = new Lehmer(SEED); // the stream continues across rounds
  let sumV = 0;
  let chk = 0;
  let nbytes = 0;
  let lines = 0;

  const writeBuf = Buffer.alloc(BUFSIZE);
  const readBuf = Buffer.alloc(BUFSIZE);

  for (let r = 0; r < R; r++) {
    const name = `mb10_round_${r}.txt`;

    const wfd = fs.openSync(name, "w");
    let used = 0;
    for (let i = 0; i < N; i++) {
      const v = gen.nextInt();
      const line = `${i} ${v}\n`;
      const len = line.length; // ASCII: one byte per character
      if (used + len > BUFSIZE) {
        fs.writeSync(wfd, writeBuf, 0, used);
        used = 0;
      }
      used = used + writeBuf.write(line, used, "latin1");
      nbytes = nbytes + len;
    }
    if (used > 0) {
      fs.writeSync(wfd, writeBuf, 0, used); // final flush before close
    }
    fs.closeSync(wfd);

    const rfd = fs.openSync(name, "r");
    let carry = "";
    let idx = 0;
    for (;;) {
      const got = fs.readSync(rfd, readBuf, 0, BUFSIZE, null);
      if (got === 0) {
        break;
      }
      const chunk = carry + readBuf.toString("latin1", 0, got);
      let start = 0;
      for (;;) {
        const nl = chunk.indexOf("\n", start);
        if (nl < 0) {
          break;
        }
        const line = chunk.substring(start, nl);
        start = nl + 1;

        const [field0, field1] = line.split(" ");
        const a = Number(field0);
        const v = Number(field1);
        if (a !== idx) {
          throw new Error(`MB10: line index mismatch at ${idx} in ${name}`);
        }
        idx = idx + 1;
        lines = lines + 1;
        sumV = (sumV + v) % 1000000007;
        chk = (chk * 31 + (v % 1000003)) % 1000003;
      }
      carry = chunk.substring(start);
    }
    fs.closeSync(rfd);
  }

  return { sumV, chk, nbytes, lines };
}

function main(): void {
  // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
  // Each steady iteration re-seeds the generator to SEED and rewrites and
  // re-reads all three files inside the timed region, so it performs exactly the
  // work `once` performs.
  const mode = process.argv.length > 2 ? process.argv[2] : "once";
  let res: Mb10Result;
  if (mode === "steady") {
    const raw = process.argv.length > 3 ? Number(process.argv[3]) : 7;
    const iterations = raw >= 1 ? Math.floor(raw) : 7;
    res = { sumV: 0, chk: 0, nbytes: 0, lines: 0 };
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

  console.log(`MB10 ${res.sumV} ${res.chk} ${res.nbytes} ${res.lines}`);
}

main();
