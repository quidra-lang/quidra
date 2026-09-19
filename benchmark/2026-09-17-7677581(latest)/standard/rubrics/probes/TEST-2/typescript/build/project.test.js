"use strict";
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const node_assert_1 = __importDefault(require("node:assert"));
const mod_a_js_1 = require("./mod_a.js");
const mod_b_js_1 = require("./mod_b.js");
(0, node_test_1.default)("scale", () => {
    node_assert_1.default.strictEqual((0, mod_a_js_1.scale)(2), 6);
});
(0, node_test_1.default)("scale_and_offset", () => {
    node_assert_1.default.strictEqual((0, mod_b_js_1.scaleAndOffset)(2), 7);
});
