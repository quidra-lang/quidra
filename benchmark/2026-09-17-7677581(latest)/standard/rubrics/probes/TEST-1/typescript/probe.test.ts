// TEST-1 probe, TypeScript. Exactly 3 tests using node:test, the test runner that
// ships with the Node.js runtime named in TypeScript's frozen run recipe:
// two assert a true condition, one asserts a false condition.
import test from "node:test";
import assert from "node:assert";
import { add } from "./probe.js";

test("pass_one", () => {
  assert.ok(add(1, 1) === 2);
});

test("pass_two", () => {
  assert.ok(add(2, 3) === 5);
});

test("fail_one", () => {
  assert.ok(add(1, 1) === 3);
});
