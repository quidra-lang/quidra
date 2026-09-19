import test from "node:test";
import assert from "node:assert";
import { scale } from "./mod_a.js";
import { scaleAndOffset } from "./mod_b.js";

test("scale", () => {
  assert.strictEqual(scale(2), 6);
});

test("scale_and_offset", () => {
  assert.strictEqual(scaleAndOffset(2), 7);
});
