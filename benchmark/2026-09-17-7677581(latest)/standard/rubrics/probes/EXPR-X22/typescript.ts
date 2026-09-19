declare function require(m: string): any;
const { test } = require("node:test");
const assert = require("node:assert");
test("add", () => { assert.strictEqual(1 + 1, 2); });
