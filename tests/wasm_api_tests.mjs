// Exercises the WebAssembly frontend bridge the way a browser does: load the
// module, call every operation through quidra_wasm_invoke, and assert on the
// structured envelope. Building the module is not evidence that it works, so
// nothing here is satisfied by a successful link.
//
// Usage: node tests/wasm_api_tests.mjs <path-to-build-dir>

import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

const buildDir = process.argv[2];
if (!buildDir) {
    console.error("usage: node tests/wasm_api_tests.mjs <build-dir>");
    process.exit(2);
}

const modulePath = resolve(buildDir, "quidra-core.js");
const { default: createQuidraCore } = await import(pathToFileURL(modulePath).href);
const core = await createQuidraCore();

let failures = 0;
let checks = 0;

function check(condition, description, detail) {
    checks += 1;
    if (condition) return true;
    failures += 1;
    console.error(`FAIL: ${description}`);
    if (detail !== undefined) {
        console.error(`      ${typeof detail === "string" ? detail : JSON.stringify(detail)}`);
    }
    return false;
}

// The ownership contract: invoke hands back an owned buffer, the caller frees
// it. Leaking here would show up as growing memory across the suite.
function invoke(request) {
    const requestPointer = core.stringToNewUTF8(JSON.stringify(request));
    let resultPointer = 0;
    try {
        resultPointer = core._quidra_wasm_invoke(requestPointer);
        if (resultPointer === 0) throw new Error("quidra_wasm_invoke returned null");
        const text = core.UTF8ToString(resultPointer);
        return JSON.parse(text);
    } finally {
        if (resultPointer !== 0) core._quidra_wasm_free(resultPointer);
        core._free(requestPointer);
    }
}

const VALID = 'int add(int a, int b)\n    return a + b\n\nprint(add(2, 3))\n';
const INVALID = 'int x = zzz\n';
const UNFORMATTED = 'int    x   =   1\nprint(x)\n';

// --- metadata -------------------------------------------------------------
{
    const response = invoke({ schema_version: 1, operation: "metadata" });
    check(response.ok === true, "metadata succeeds", response);
    check(response.schema_version === 1, "metadata reports the envelope version", response);
    const metadata = response.metadata ?? {};
    for (const field of [
        "product_version",
        "language_version",
        "ir_version",
        "core_commit",
        "wasm_schema_version",
        "default_filename",
    ]) {
        check(metadata[field] !== undefined && metadata[field] !== "",
              `metadata carries ${field}`, metadata);
    }
    check(/^\d+\.\d+\.\d+$/.test(metadata.product_version ?? ""),
          "product_version looks like a release version", metadata.product_version);
    check(metadata.core_commit !== "unknown",
          "core_commit was resolved at configure time", metadata.core_commit);

    // project.toml is the only place the version is authored. If the bridge
    // ever drifts from it, this is where it shows.
    try {
        const projectToml = readFileSync(resolve(buildDir, "..", "project.toml"), "utf8");
        const declared = /^version = "([^"]+)"$/m.exec(projectToml);
        if (declared) {
            check(metadata.product_version === declared[1],
                  "product_version matches project.toml", `${metadata.product_version} vs ${declared[1]}`);
        }
    } catch {
        // Running against an unpacked build tree without the source beside it.
    }
}

// --- check ----------------------------------------------------------------
{
    const response = invoke({ schema_version: 1, operation: "check", source: VALID });
    check(response.ok === true && response.valid === true, "valid source checks clean", response);
    check(Array.isArray(response.diagnostics) && response.diagnostics.length === 0,
          "valid source reports no diagnostics", response);
}
{
    const response = invoke({ schema_version: 1, operation: "check", source: INVALID });
    check(response.ok === true, "checking invalid source is still a completed call", response);
    check(response.valid === false, "invalid source is reported invalid", response);
    const [first] = response.diagnostics ?? [];
    check(first !== undefined, "invalid source produces a diagnostic", response);
    if (first) {
        check(typeof first.code === "string" && first.code.length > 0,
              "diagnostic carries a stable code", first);
        check(typeof first.message === "string" && first.message.length > 0,
              "diagnostic carries a message", first);
        check(typeof first.span?.start?.offset === "number",
              "diagnostic carries a start offset", first);
        check(typeof first.span?.start?.line === "number" &&
              typeof first.span?.start?.column === "number",
              "diagnostic carries line and column", first);
        check(first.span.end.offset >= first.span.start.offset,
              "diagnostic range is well ordered", first);
    }
}

// --- format ---------------------------------------------------------------
{
    const response = invoke({ schema_version: 1, operation: "format", source: UNFORMATTED });
    check(response.ok === true, "format succeeds", response);
    check(typeof response.source === "string" && response.source.length > 0,
          "format returns source", response);
    check(response.changed === true, "format reports that it changed the source", response);

    const again = invoke({ schema_version: 1, operation: "format", source: response.source });
    check(again.ok === true && again.changed === false,
          "formatting formatted source is a no-op", again);
}

// --- typed IR -------------------------------------------------------------
{
    const response = invoke({ schema_version: 1, operation: "ir", source: VALID });
    check(response.ok === true, "IR lowering succeeds for valid source", response);
    check(typeof response.text === "string" && response.text.length > 0,
          "IR returns text", response);
    check(typeof response.ir_version === "string", "IR reports its format version", response);
}
{
    const response = invoke({ schema_version: 1, operation: "ir", source: INVALID });
    check(response.ok === false, "IR refuses source that does not check", response);
    check(response.error?.kind === "compile_failed", "IR failure is structured", response);
    check(Array.isArray(response.error?.diagnostics) && response.error.diagnostics.length > 0,
          "IR failure carries the diagnostics", response);
}

// --- inspect --------------------------------------------------------------
let inspection;
{
    const response = invoke({ schema_version: 1, operation: "inspect", source: VALID });
    check(response.ok === true, "inspect succeeds", response);
    inspection = response.inspection;
    check(inspection !== undefined && typeof inspection === "object",
          "inspect returns the compiler's own inspection document", response);
    check(typeof inspection?.revision === "string" && inspection.revision.length === 64,
          "inspection carries a revision hash", inspection?.revision);
    check(Array.isArray(inspection?.nodes) && inspection.nodes.length > 0,
          "inspection carries nodes", inspection?.nodes?.length);
}

// --- patch ----------------------------------------------------------------
// A real round trip: inspect supplies the revision, node id, hash and kind that
// the patch protocol keys on, so this proves the two operations agree.
{
    const target = (inspection?.nodes ?? []).find(
        (node) => node.source === "2" || node.source === "3");
    if (!check(target !== undefined, "found an integer literal node to patch",
               (inspection?.nodes ?? []).slice(0, 5))) {
        // fall through; the remaining patch assertions are skipped
    } else {
        const patch = JSON.stringify({
            schema_version: 2,
            base_revision: inspection.revision,
            operations: [{
                op: "replace_node",
                node_id: target.node_id,
                expected_hash: target.source_hash,
                expected_kind: target.kind,
                replacement: "7",
            }],
        });
        const response = invoke({ schema_version: 1, operation: "patch", source: VALID, patch });
        check(response.ok === true, "patch applies", response);
        check(response.base_revision === inspection.revision,
              "patch reports the base revision it was built against", response);
        check(typeof response.revision === "string" && response.revision !== response.base_revision,
              "patch reports a new revision", response);
        check(typeof response.source === "string" && response.source.includes("7"),
              "patched source contains the replacement", response.source);

        // Replaying the same patch must be refused: the source has moved on.
        const stale = invoke({
            schema_version: 1, operation: "patch", source: response.source, patch,
        });
        check(stale.ok === false, "a stale patch is refused", stale);
        check(typeof stale.error?.kind === "string", "stale patch failure is structured", stale);
    }
}
{
    // A patch whose result would not compile must be refused, with the caller's
    // source left untouched.
    const target = (inspection?.nodes ?? []).find((node) => node.source === "2");
    if (target) {
        const patch = JSON.stringify({
            schema_version: 2,
            base_revision: inspection.revision,
            operations: [{
                op: "replace_node",
                node_id: target.node_id,
                expected_hash: target.source_hash,
                expected_kind: target.kind,
                replacement: "undefined_symbol",
            }],
        });
        const response = invoke({ schema_version: 1, operation: "patch", source: VALID, patch });
        check(response.ok === false, "a patch that breaks the program is refused", response);
        check(response.source === undefined, "a refused patch returns no source", response);
    }
}
{
    const response = invoke({
        schema_version: 1, operation: "patch", source: VALID, patch: "{not json",
    });
    check(response.ok === false, "malformed patch JSON is refused", response);
    check(response.error?.kind === "patch_failed", "malformed patch failure is structured",
          response);
}

// --- patch schema ---------------------------------------------------------
{
    const response = invoke({ schema_version: 1, operation: "patch_schema" });
    check(response.ok === true, "patch_schema succeeds", response);
    check(response.schema !== undefined, "patch_schema returns a schema document", response);
}

// --- error handling: nothing may escape as an exception -------------------
{
    const response = invoke({ schema_version: 1, operation: "run", source: VALID });
    check(response.ok === false, "an unimplemented operation fails cleanly", response);
    check(response.error?.kind === "unknown_operation",
          "execution is reported as unimplemented, not attempted", response);
}
{
    const response = invoke({ schema_version: 99, operation: "check", source: VALID });
    check(response.ok === false, "an unknown schema version is refused", response);
    check(response.error?.kind === "unsupported_schema_version",
          "schema mismatch is named", response);
}
{
    const response = invoke({ schema_version: 1, operation: "check" });
    check(response.ok === false, "a missing source is refused", response);
    check(response.error?.kind === "invalid_request", "missing source is named", response);
}
{
    // Raw malformed input, bypassing JSON.stringify.
    const pointer = core.stringToNewUTF8("{ this is not json");
    const resultPointer = core._quidra_wasm_invoke(pointer);
    const parsed = JSON.parse(core.UTF8ToString(resultPointer));
    core._quidra_wasm_free(resultPointer);
    core._free(pointer);
    check(parsed.ok === false, "malformed request JSON fails cleanly", parsed);
    check(parsed.error?.kind === "invalid_request", "malformed request is named", parsed);
}
{
    // Deeply nested input used to exhaust the stack. The compiler's nesting
    // budgets must turn that into a diagnostic, inside the Wasm sandbox too.
    const deep = `int x = 1${" + 1".repeat(2000)}\n`;
    const response = invoke({ schema_version: 1, operation: "check", source: deep });
    check(response.ok === true, "a pathologically deep expression does not trap", response.ok);
    check(response.valid === false, "a pathologically deep expression is rejected", response.valid);
    check((response.diagnostics ?? []).some((d) => d.code === "NESTING_DEPTH"),
          "depth rejection uses the NESTING_DEPTH code",
          (response.diagnostics ?? []).map((d) => d.code));

    // The module must still be usable afterwards.
    const after = invoke({ schema_version: 1, operation: "check", source: VALID });
    check(after.ok === true && after.valid === true,
          "the module still works after a rejected deep input", after);
}
{
    // Unicode must survive the boundary in both directions.
    const source = 'string s = "こんにちは 🌸"\nprint(s)\n';
    const response = invoke({ schema_version: 1, operation: "format", source });
    check(response.ok === true, "unicode source formats", response);
    check(response.source.includes("こんにちは 🌸"),
          "unicode survives the Wasm boundary", response.source);
}

console.log(`wasm api tests: ${checks - failures}/${checks} assertions passed`);
if (failures > 0) {
    console.error(`wasm api tests: ${failures} failed`);
    process.exit(1);
}
