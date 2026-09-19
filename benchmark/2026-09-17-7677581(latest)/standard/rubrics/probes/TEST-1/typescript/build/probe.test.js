"use strict";
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
// TEST-1 probe, TypeScript. Exactly 3 tests using node:test, the test runner that
// ships with the Node.js runtime named in TypeScript's frozen run recipe:
// two assert a true condition, one asserts a false condition.
const node_test_1 = __importDefault(require("node:test"));
const node_assert_1 = __importDefault(require("node:assert"));
const probe_js_1 = require("./probe.js");
(0, node_test_1.default)("pass_one", () => {
    node_assert_1.default.ok((0, probe_js_1.add)(1, 1) === 2);
});
(0, node_test_1.default)("pass_two", () => {
    node_assert_1.default.ok((0, probe_js_1.add)(2, 3) === 5);
});
(0, node_test_1.default)("fail_one", () => {
    node_assert_1.default.ok((0, probe_js_1.add)(1, 1) === 3);
});
