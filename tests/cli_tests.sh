#!/usr/bin/env bash
set -euo pipefail
set -x
QUIDRA="$(realpath "$1")"
ROOT="$(realpath "$2")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

TIME_BIN=""
if [[ -x /usr/bin/time ]] && /usr/bin/time --version 2>&1 | grep -q 'GNU time'; then
    TIME_BIN="/usr/bin/time"
elif command -v gtime >/dev/null 2>&1; then
    TIME_BIN="$(command -v gtime)"
fi

measure_rss() {
    local rss_file="$1"
    local output_file="$2"
    shift 2
    if [[ -n "$TIME_BIN" ]]; then
        "$TIME_BIN" -f '%M' -o "$rss_file" "$@" > "$output_file"
    else
        "$@" > "$output_file"
        printf '0\n' > "$rss_file"
    fi
}

version_output="$($QUIDRA --version)"
[[ "$version_output" == quidra\ * ]]
compiler_version="${version_output#quidra }"
$QUIDRA describe > "$TMP/describe.json"
python3 - "$TMP/describe.json" "$compiler_version" "$ROOT/quidra.manifest.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["language"] == "Quidra"
assert x["compiler_version"] == sys.argv[2]
with open(sys.argv[3]) as f:
    manifest=json.load(f)
assert x == manifest
assert x["modules"] is True
assert x["generics"] is True
assert x["generic_type_inference"] is True
assert x["import_cycles_allowed"] is False
assert x["reference_rebinding"] is True
assert x["substorage_references"] is True
assert x["object_identity_operator"] is False
assert x["class_field_defaults"] is True
assert x["super_method_calls"] is True
assert x["class_value_equality"] is True
assert x["array_value_equality"] is True
assert x["repl"] is True
assert x["formatter"] is True
assert x["lsp"] is True
assert x["package_management"] is True
assert x["c_ffi"] is True
assert x["debug_build"] is True
for name in ["int8","int16","int32","int64 (= int)","uint8","uint16","uint32","uint64","bigint","float32","float64 (= float)","bigreal","bin"]:
    assert name in x["current_types"], name
for name in ["print","write","input","range","array","len","abs","sqrt","min","max","error"]:
    assert name in x["current_builtins"], name
assert x["standard_modules"] == ["math","cli","file","environment","test","time","random","process","map","set","json","http","stats","linear","signal","image","tensor","neural"]
assert x["array_growth_model"].startswith("append(value)")
assert "Unicode code-point" in x["string_operation_model"]
assert "tensor<T><D0, D1, ...>" in x["current_types"]
assert "exact-rank shape pattern" in x["tensor_model"]
assert "runtime expressions are evaluated once" in x["tensor_model"]
assert "captured constraints survive reassignment" in x["tensor_model"]
PY
# GPU discovery is always safe, including on hosts with no supported GPU.
"$QUIDRA" gpu > "$TMP/gpu-info.out"
[[ -s "$TMP/gpu-info.out" ]]

# CPU remains the default placement and explicit CPU copies preserve values.
cat > "$TMP/tensor-device-cpu.qui" <<'QUI'
tensor<float32> source = tensor.ones<float32>([2])
tensor<float32> copied = source.cpu()
print(copied[0].item())
QUI
[[ "$("$QUIDRA" run "$TMP/tensor-device-cpu.qui")" == "1.0" ]]

# An unavailable GPU must fail explicitly. It must never run the allocation on CPU.
cat > "$TMP/tensor-device-unavailable.qui" <<'QUI'
auto value = tensor.zeros<float32>([1], gpu = 2147483647)
print(value[0].item())
QUI
set +e
"$QUIDRA" run "$TMP/tensor-device-unavailable.qui"     > "$TMP/tensor-device-unavailable.out"     2> "$TMP/tensor-device-unavailable.err"
tensor_device_unavailable_rc=$?
set -e
[[ "$tensor_device_unavailable_rc" -eq 101 ]]
grep -q 'gpu(2147483647) is not available' "$TMP/tensor-device-unavailable.err"
[[ ! -s "$TMP/tensor-device-unavailable.out" ]]

cat > "$TMP/tensor-transfer-unavailable.qui" <<'QUI'
auto source = tensor.ones<float32>([1])
auto moved = source.gpu(2147483647)
print(moved[0].item())
QUI
set +e
"$QUIDRA" run "$TMP/tensor-transfer-unavailable.qui"     > "$TMP/tensor-transfer-unavailable.out"     2> "$TMP/tensor-transfer-unavailable.err"
tensor_transfer_unavailable_rc=$?
set -e
[[ "$tensor_transfer_unavailable_rc" -eq 101 ]]
grep -q 'gpu(2147483647) is not available' "$TMP/tensor-transfer-unavailable.err"
[[ ! -s "$TMP/tensor-transfer-unavailable.out" ]]

$QUIDRA check "$ROOT/examples/hello.qui" --json > "$TMP/check-version.json"
python3 - "$TMP/check-version.json" "$ROOT/quidra.manifest.json" <<'PY'
import json,sys
check=json.load(open(sys.argv[1]))
manifest=json.load(open(sys.argv[2]))
assert check["language_version"] == manifest["language_version"], (check, manifest["language_version"])
PY
cat > "$TMP/format.qui" <<'QUI'
int  add (int a ,  int b)
    return a+b
int  result=add (1,2)
string  text = "a  b // c"  // preserve source text and comments
print (result)
QUI
set +e
"$QUIDRA" fmt "$TMP/format.qui" --check
format_check_rc=$?
set -e
[[ "$format_check_rc" -eq 1 ]]
"$QUIDRA" fmt "$TMP/format.qui"
cat > "$TMP/format.expected" <<'QUI'
int add(int a, int b)
    return a + b
int result = add(1, 2)
string text = "a  b // c"  // preserve source text and comments
print(result)
QUI
cmp "$TMP/format.expected" "$TMP/format.qui"
"$QUIDRA" fmt "$TMP/format.qui" --check
"$QUIDRA" check "$TMP/format.qui"

python3 - "$QUIDRA" "$TMP" <<'PY'
import json
import pathlib
import subprocess
import sys

quidra = sys.argv[1]
tmp = pathlib.Path(sys.argv[2])
(tmp / "lsp-lib.qui").write_text("int answer()\n    return 1\n", encoding="utf-8")
uri = (tmp / "lsp-root.qui").as_uri()
messages = [
    {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":tmp.as_uri()}},
    {"jsonrpc":"2.0","method":"initialized","params":{}},
    {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{
        "uri":uri,"languageId":"quidra","version":1,
        "text":'import lib = "./lsp-lib.qui"\nint  value=lib.answer ()\nprint (value)\n'
    }}},
    {"jsonrpc":"2.0","id":2,"method":"textDocument/formatting","params":{
        "textDocument":{"uri":uri},"options":{"tabSize":4,"insertSpaces":True}
    }},
    {"jsonrpc":"2.0","method":"textDocument/didChange","params":{
        "textDocument":{"uri":uri,"version":2},"contentChanges":[{"text":"print(\"😀\" + missing)\n"}]
    }},
    {"jsonrpc":"2.0","id":3,"method":"shutdown","params":None},
    {"jsonrpc":"2.0","method":"exit","params":None},
]
payload = b""
for message in messages:
    body = json.dumps(message,separators=(",",":"),ensure_ascii=False).encode("utf-8")
    payload += f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body
process = subprocess.run([quidra,"lsp"],input=payload,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
assert process.returncode == 0, process.stderr.decode()
data = process.stdout
responses = []
pos = 0
while pos < len(data):
    end = data.find(b"\r\n\r\n",pos)
    assert end >= 0
    headers = data[pos:end].decode("ascii").split("\r\n")
    length = int(next(x.split(":",1)[1].strip() for x in headers if x.lower().startswith("content-length:")))
    start = end + 4
    responses.append(json.loads(data[start:start+length]))
    pos = start + length
init = next(x for x in responses if x.get("id") == 1)
assert init["result"]["capabilities"]["positionEncoding"] == "utf-16"
assert init["result"]["capabilities"]["textDocumentSync"]["change"] == 1
assert init["result"]["capabilities"]["documentFormattingProvider"] is True
published = [x for x in responses if x.get("method") == "textDocument/publishDiagnostics"]
assert published and published[0]["params"]["diagnostics"] == []
fmt = next(x for x in responses if x.get("id") == 2)["result"]
assert len(fmt) == 1
assert fmt[0]["newText"] == (
    'import lib = "./lsp-lib.qui"\n'
    'int value = lib.answer()\n'
    'print(value)\n'
)
changed = next(x for x in published[1:] if x["params"]["diagnostics"])
diagnostic = changed["params"]["diagnostics"][0]
assert diagnostic["code"] == "UNKNOWN_NAME", diagnostic
assert diagnostic["range"]["start"]["line"] == 0, diagnostic
assert diagnostic["range"]["start"]["character"] == 13, diagnostic
assert next(x for x in responses if x.get("id") == 3)["result"] is None
PY

python3 - "$QUIDRA" "$TMP" <<'PY'
import json
import pathlib
import subprocess
import sys

quidra = sys.argv[1]
tmp = pathlib.Path(sys.argv[2])
uri = (tmp / "lsp-semantic.qui").as_uri()
source = (
    "int twice(int value)\n"
    "    return value * 2\n"
    "\n"
    "int input = 4\n"
    "int output = twice(input)\n"
    "print(output)\n"
)
messages = [
    {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":tmp.as_uri()}},
    {"jsonrpc":"2.0","method":"initialized","params":{}},
    {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{
        "uri":uri,"languageId":"quidra","version":1,"text":source
    }}},
    {"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{
        "textDocument":{"uri":uri},"position":{"line":5,"character":8}
    }},
    {"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{
        "textDocument":{"uri":uri},"position":{"line":5,"character":8}
    }},
    {"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{
        "textDocument":{"uri":uri},"position":{"line":5,"character":0}
    }},
    {"jsonrpc":"2.0","id":5,"method":"textDocument/signatureHelp","params":{
        "textDocument":{"uri":uri},"position":{"line":4,"character":20}
    }},
    {"jsonrpc":"2.0","id":6,"method":"textDocument/references","params":{
        "textDocument":{"uri":uri},"position":{"line":5,"character":8},
        "context":{"includeDeclaration":True}
    }},
    {"jsonrpc":"2.0","id":7,"method":"textDocument/rename","params":{
        "textDocument":{"uri":uri},"position":{"line":5,"character":8},"newName":"result"
    }},
    {"jsonrpc":"2.0","id":8,"method":"textDocument/semanticTokens/full","params":{
        "textDocument":{"uri":uri}
    }},
    {"jsonrpc":"2.0","id":9,"method":"shutdown","params":None},
    {"jsonrpc":"2.0","method":"exit","params":None},
]
payload = b""
for message in messages:
    body = json.dumps(message,separators=(",",":")).encode()
    payload += f"Content-Length: {len(body)}\r\n\r\n".encode() + body

process = subprocess.run([quidra,"lsp"],input=payload,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
assert process.returncode == 0, process.stderr.decode()
data = process.stdout
responses = []
pos = 0
while pos < len(data):
    end = data.find(b"\r\n\r\n",pos)
    assert end >= 0
    headers = data[pos:end].decode().split("\r\n")
    length = int(next(x.split(":",1)[1].strip() for x in headers if x.lower().startswith("content-length:")))
    start = end + 4
    responses.append(json.loads(data[start:start+length]))
    pos = start + length

by_id = {x["id"]:x for x in responses if "id" in x}
caps = by_id[1]["result"]["capabilities"]
assert caps["hoverProvider"] is True
assert caps["definitionProvider"] is True
assert caps["completionProvider"]["triggerCharacters"] == ["."]
assert caps["signatureHelpProvider"]["triggerCharacters"] == ["(",","]
assert caps["referencesProvider"] is True
assert caps["renameProvider"] is True
legend = caps["semanticTokensProvider"]["legend"]["tokenTypes"]
for expected in ("function","parameter","variable","keyword","number"):
    assert expected in legend, (expected, legend)
assert caps["semanticTokensProvider"]["full"] is True

hover = by_id[2]["result"]
assert hover["contents"]["value"] == "int", hover
definition = by_id[3]["result"]
assert definition["uri"] == uri
assert definition["range"]["start"] == {"line":4,"character":4}, definition

labels = {item["label"] for item in by_id[4]["result"]}
for expected in ("source_value","output","twice","print"):
    assert expected in labels, (expected, labels)

signature = by_id[5]["result"]
assert signature["signatures"][0]["label"] == "int twice(int value)", signature
assert signature["activeParameter"] == 0

references = by_id[6]["result"]
reference_starts = [item["range"]["start"] for item in references]
assert reference_starts == [
    {"line":4,"character":4},
    {"line":5,"character":6},
], references

rename = by_id[7]["result"]["changes"][uri]
assert [edit["newText"] for edit in rename] == ["result","result"], rename
assert [edit["range"]["start"] for edit in rename] == reference_starts, rename

semantic = by_id[8]["result"]["data"]
assert semantic and len(semantic) % 5 == 0, semantic
observed_types = {legend[semantic[index + 3]] for index in range(0,len(semantic),5)}
for expected in ("function","parameter","variable","keyword","number"):
    assert expected in observed_types, (expected, observed_types)

assert by_id[9]["result"] is None
PY

mkdir -p "$TMP/package-source" "$TMP/package-home"
cat > "$TMP/package-source/main.qui" <<'QUI'
int doubled(int value)
    return value * 2
QUI
HOME="$TMP/package-home" "$QUIDRA" package install "$TMP/package-source" --name local_math
[[ "$(HOME="$TMP/package-home" "$QUIDRA" package list)" == "local_math" ]]
[[ "$(HOME="$TMP/package-home" "$QUIDRA" package path)" == "$TMP/package-home/.quidra/packages" ]]
cat > "$TMP/package-use.qui" <<'QUI'
import math_package = local_math
print(math_package.doubled(21))
QUI
[[ "$(HOME="$TMP/package-home" "$QUIDRA" "$TMP/package-use.qui")" == "42" ]]

mkdir -p "$TMP/package-project"
cat > "$TMP/package-project/main.qui" <<'QUI'
import math_package = local_math
print(math_package.doubled(21))
QUI
(
    cd "$TMP/package-project"
    HOME="$TMP/package-home" "$QUIDRA" package lock main.qui
    grep -Eq '^local_math - [0-9a-f]{64}$' quidra.lock
    HOME="$TMP/package-home" "$QUIDRA" package lock main.qui --check
    HOME="$TMP/package-home" "$QUIDRA" check main.qui
)
printf '\n// package content changed after locking\n' >> "$TMP/package-home/.quidra/packages/local_math/main.qui"
set +e
(
    cd "$TMP/package-project"
    HOME="$TMP/package-home" "$QUIDRA" check main.qui
) >"$TMP/package-lock-check.out" 2>"$TMP/package-lock-check.err"
package_lock_check_rc=$?
(
    cd "$TMP/package-project"
    HOME="$TMP/package-home" "$QUIDRA" package lock main.qui --check
) >"$TMP/package-lock-stale.out" 2>"$TMP/package-lock-stale.err"
package_lock_stale_rc=$?
set -e
[[ "$package_lock_check_rc" -eq 1 ]]
[[ "$package_lock_stale_rc" -eq 1 ]]
grep -q 'PACKAGE_LOCK_MISMATCH' "$TMP/package-lock-check.err"
(
    cd "$TMP/package-project"
    HOME="$TMP/package-home" "$QUIDRA" package lock main.qui >/dev/null
    HOME="$TMP/package-home" "$QUIDRA" package lock main.qui --check
    HOME="$TMP/package-home" "$QUIDRA" check main.qui
    printf 'unused_package - %064d\n' 0 >> quidra.lock
    set +e
    HOME="$TMP/package-home" "$QUIDRA" check main.qui >"$TMP/package-lock-unused.out" 2>"$TMP/package-lock-unused.err"
    package_lock_unused_rc=$?
    set -e
    [[ "$package_lock_unused_rc" -eq 1 ]]
    grep -q 'PACKAGE_LOCK_UNUSED' "$TMP/package-lock-unused.err"
    HOME="$TMP/package-home" "$QUIDRA" package lock main.qui >/dev/null
)

set +e
HOME="$TMP/package-home" "$QUIDRA" package install "$TMP/package-source" --name local_math >"$TMP/package-repeat.out" 2>"$TMP/package-repeat.err"
package_repeat_rc=$?
set -e
[[ "$package_repeat_rc" -eq 1 ]]
grep -q 'already installed' "$TMP/package-repeat.err"
HOME="$TMP/package-home" "$QUIDRA" package install "$TMP/package-source" --name local_math --force >/dev/null
HOME="$TMP/package-home" "$QUIDRA" package remove local_math
[[ -z "$(HOME="$TMP/package-home" "$QUIDRA" package list)" ]]

cat > "$TMP/ffi-scalar.qui" <<'QUI'
extern int c_abs(int value) = "llabs"
print(c_abs(-42))
QUI
[[ "$("$QUIDRA" "$TMP/ffi-scalar.qui")" == "42" ]]
"$QUIDRA" llvm "$TMP/ffi-scalar.qui" > "$TMP/ffi-scalar.ll"
grep -q 'declare i64 @llabs(i64)' "$TMP/ffi-scalar.ll"
grep -q 'call i64 @llabs(i64' "$TMP/ffi-scalar.ll"

cat > "$TMP/ffi-borrowed-inputs.qui" <<'QUI'
extern int32 c_text(const string &text) = "foreign_test_text"
extern int32 c_bin(const bin &data) = "foreign_test_bin"

string text = "ffi-string"
bin payload = bin.fill(24, 1)
int32 text_status = c_text(&text)
int32 bin_status = c_bin(&payload)
QUI
"$QUIDRA" llvm "$TMP/ffi-borrowed-inputs.qui" > "$TMP/ffi-borrowed-inputs.ll"
grep -q 'declare i32 @foreign_test_text(ptr nocapture nonnull readonly, i64)' "$TMP/ffi-borrowed-inputs.ll"
grep -q 'declare i32 @foreign_test_bin(ptr nocapture nonnull readonly, i64)' "$TMP/ffi-borrowed-inputs.ll"
grep -q 'ffi.borrowed.value' "$TMP/ffi-borrowed-inputs.ll"
grep -q 'call i64 @strlen(ptr' "$TMP/ffi-borrowed-inputs.ll"
grep -q 'ffi.bin.length' "$TMP/ffi-borrowed-inputs.ll"
grep -q 'ffi.bin.data' "$TMP/ffi-borrowed-inputs.ll"

"$QUIDRA" build "$ROOT/examples/hello.qui" --debug -o "$TMP/hello-debug"
[[ "$("$TMP/hello-debug")" == "Hello from Quidra" ]]

cat > "$TMP/debug-source.qui" <<'QUI'
int twice(int value)
    return value * 2

print(twice(21))
QUI
"$QUIDRA" build "$TMP/debug-source.qui" --debug --keep-llvm -o "$TMP/debug-source"
[[ "$("$TMP/debug-source")" == "42" ]]
grep -q '^source_filename = "debug-source.qui"
[[ "$($QUIDRA "$ROOT/examples/hello.qui")" == "Hello from Quidra" ]]
[[ "$($QUIDRA run "$ROOT/examples/functions.qui")" == "120" ]]
[[ "$($QUIDRA run "$ROOT/examples/logic.qui")" == "positive even integer" ]]
[[ "$($QUIDRA run "$ROOT/examples/named_arguments.qui")" == "true" ]]
[[ "$($QUIDRA run "$ROOT/examples/arrays.qui")" == $'2\n4\n6' ]]
[[ "$($QUIDRA run "$ROOT/examples/results.qui")" == "42" ]]
[[ "$($QUIDRA run "$ROOT/examples/options.qui")" == "none" ]]
[[ "$($QUIDRA run "$ROOT/examples/classes.qui")" == $'1\n5\n9\n12\n7\n17\n1\n99\n2' ]]
[[ "$($QUIDRA run "$ROOT/examples/value_objects.qui")" == $'true\ntrue\n4' ]]

binary_dependencies() {
    if command -v ldd >/dev/null 2>&1; then
        ldd "$1"
    elif command -v otool >/dev/null 2>&1; then
        otool -L "$1"
    else
        echo "no supported dependency inspection tool" >&2
        return 2
    fi
}

"$QUIDRA" build "$ROOT/examples/hello.qui" -o "$TMP/hello-link"
if binary_dependencies "$TMP/hello-link" | grep -qi 'libcurl'; then
    echo "hello executable unexpectedly links libcurl" >&2
    exit 1
fi

cat > "$TMP/http-link.qui" <<'QUI'
auto result = http.get("https://example.com")
QUI
"$QUIDRA" build "$TMP/http-link.qui" -o "$TMP/http-link"
binary_dependencies "$TMP/http-link" | grep -qi 'libcurl'

mkdir -p "$TMP/file-tree/a/b"
printf 'one' > "$TMP/file-tree/root.txt"
printf 'two' > "$TMP/file-tree/a/child.txt"
printf 'three' > "$TMP/file-tree/a/b/deep.txt"
cat > "$TMP/file-tree.qui" <<QUI
string root = "$TMP/file-tree"
match file.is_directory(root)
    bool value
        print(value)
    error problem
        print(problem)
match file.is_directory(root + "/root.txt")
    bool value
        print(value)
    error problem
        print(problem)
match file.list(root)
    string[] paths
        print(len(paths))
    error problem
        print(problem)
match file.list(root, recursive = true)
    string[] paths
        print(len(paths))
    error problem
        print(problem)
QUI
file_tree_output="$("$QUIDRA" run "$TMP/file-tree.qui")"
file_tree_expected=$(printf 'true\nfalse\n2\n5')
[[ "$file_tree_output" == "$file_tree_expected" ]]

cat > "$TMP/string-controls.qui" <<'QUI'
print("literal:\n\t\r\b\f\v\a\u3042")
print("A{tab}B")
print("A{enter}B")
print("A{home}B")
print("{quote}A{quote}")
print("x{backspace}y")
print("x{page}y")
print("x{vtab}y")
print("x{bell}y")
print("nested {error("ok")}")
QUI
$QUIDRA run "$TMP/string-controls.qui" > "$TMP/string-controls.out"
python3 - "$TMP/string-controls.out" <<'PY'
import sys
actual = open(sys.argv[1], "rb").read()
expected = (
    b"literal:\\n\\t\\r\\b\\f\\v\\a\\u3042\n"
    b"A\tB\n"
    b"A\nB\n"
    b"A\rB\n"
    b"\"A\"\n"
    b"x\x08y\n"
    b"x\x0cy\n"
    b"x\x0by\n"
    b"x\x07y\n"
    b"nested ok\n"
)
assert actual == expected, (actual, expected)
PY

cat > "$TMP/text-and-array.qui" <<'QUI'
string text = "  A日本B  "
string merged = "A" + "B" + "C" + "D"
print(merged)
print(len(text))
print(text.trim())
print(text.contains("日本"))
print(text.starts_with("  A"))
print(text.ends_with("  "))
auto found = text.find("日本")
match found
    int index
        print(index)
    none
        print(int(-1))
print(text.slice(2, 6))
print(text[3])
print("A日本"[2])
string[] parts = "a,b,,c".split(",")
print(len(parts))
print(parts[0])
print(parts[2] == "")
print(parts.join("|"))
bin encoded = "A日本".utf8()
print(len(encoded))
print(encoded[0])
int[] points = "A日本".codepoints()
print(len(points))
print(points[1])
int[] unordered = [3, -1, 2]
int[] ordered = unordered.sorted()
print(ordered[0])
print(ordered[2])
string[] ordered_text = ["b", "あ", "a"].sorted()
print(ordered_text[0])
print(ordered_text[2])
int[] original = [1, 2]
int &first = &original[0]
int[] grown = original.append(3)
first = 9
print(original[0])
print(grown[0])
print(grown[2])
int[] joined = grown.concat([4, 5])
print(len(joined))
print(joined[4])
int choice = 2
if choice == 1
    print("one")
elif choice == 2
    print("two")
else
    print("other")
QUI
text_array_output=$("$QUIDRA" run "$TMP/text-and-array.qui")
text_array_expected=$(printf 'ABCD\n8\nA日本B\ntrue\ntrue\ntrue\n3\nA日本B\n日\n本\n4\na\ntrue\na|b||c\n56\n0\n3\n26085\n-1\n3\na\nあ\n9\n1\n3\n5\n5\ntwo')
[[ "$text_array_output" == "$text_array_expected" ]]

cat > "$TMP/unicode-boundaries.qui" <<'QUI'
string text = "A😀é"
print(len(text))
print(text[1] == "😀")
print(text.slice(1, 4) == "😀é")
bin encoded = text.utf8()
print(len(encoded))
int[] points = text.codepoints()
print(points[1])
print(points[3])
print(len(""))
print("".slice(0, 0) == "")
QUI
unicode_boundary_output=$("$QUIDRA" run "$TMP/unicode-boundaries.qui")
unicode_boundary_expected=$(printf '4\ntrue\ntrue\n64\n128512\n769\n0\ntrue')
[[ "$unicode_boundary_output" == "$unicode_boundary_expected" ]]

cat > "$TMP/string-negative-index.qui" <<'QUI'
string text = "😀"
print(text[-1])
QUI
set +e
"$QUIDRA" run "$TMP/string-negative-index.qui" > "$TMP/string-negative-index.out" 2> "$TMP/string-negative-index.err"
string_negative_index_rc=$?
set -e
[[ "$string_negative_index_rc" -eq 101 ]]
grep -q 'error\[INDEX_BOUNDS\]' "$TMP/string-negative-index.err"

"$QUIDRA" llvm "$TMP/text-and-array.qui" > "$TMP/text-and-array.ll"
grep -q 'call ptr @quidra_string_concat_many' "$TMP/text-and-array.ll"

cat > "$TMP/float-sorted-order.qui" <<'QUI'
float[] values = [0.0, -0.0, 2.0, 0.0 / 0.0, -1.0]
float[] ordered = values.sorted()
for value in ordered
    print(value)
QUI
float_sorted_output=$("$QUIDRA" run "$TMP/float-sorted-order.qui")
float_sorted_expected=$(printf '%s\n' '-1.0' '-0.0' '0.0' '2.0' 'nan')
[[ "$float_sorted_output" == "$float_sorted_expected" ]]

cat > "$TMP/sorted-boundaries.qui" <<'QUI'
float[] special = [
    1.0 / 0.0,
    -1.0 / 0.0,
    0.0 / 0.0,
    0.0,
    -0.0,
    1.0 / 0.0,
    -1.0 / 0.0,
]
float[] special_ordered = special.sorted()
for value in special_ordered
    print(value)

int[6] fixed = [5, 1, 4, 1, 3, 2]
int[] fixed_ordered = fixed.sorted()
fixed[0] = 99
print(fixed_ordered[0])
print(fixed_ordered[5])

int[] large = []
for i in range(0, 5000)
    large = large.append(4999 - i)
int[] large_ordered = large.sorted()
print(large_ordered[0])
print(large_ordered[4999])
QUI
sorted_boundaries_output=$("$QUIDRA" run "$TMP/sorted-boundaries.qui")
sorted_boundaries_expected=$(printf '%s\n' '-inf' '-inf' '-0.0' '0.0' 'inf' 'inf' 'nan' '1' '5' '0' '4999')
[[ "$sorted_boundaries_output" == "$sorted_boundaries_expected" ]]

cat > "$TMP/string-index-oob.qui" <<'QUI'
string text = "日本"
print(text[2])
QUI
set +e
"$QUIDRA" run "$TMP/string-index-oob.qui" > "$TMP/string-index-oob.out" 2> "$TMP/string-index-oob.err"
string_index_oob_rc=$?
set -e
[[ "$string_index_oob_rc" -eq 101 ]]
grep -q 'error\[INDEX_BOUNDS\]' "$TMP/string-index-oob.err"

cat > "$TMP/string-growth.qui" <<'QUI'
string built = ""
for i in range(0, 2000)
    built = built + "x"
print(len(built))

string assigned = ""
for i in range(0, 2000)
    assigned += "y"
print(len(assigned))

string shared = "ab"
string snapshot = shared
shared = shared + "c"
print(snapshot)
print(shared)

string self = "xy"
self = self + self
print(self)
QUI
string_growth_output=$("$QUIDRA" run "$TMP/string-growth.qui")
string_growth_expected=$(printf '2000\n2000\nab\nabc\nxyxy')
[[ "$string_growth_output" == "$string_growth_expected" ]]
"$QUIDRA" llvm "$TMP/string-growth.qui" > "$TMP/string-growth.ll"
grep -q 'call i1 @quidra_string_can_append_move' "$TMP/string-growth.ll"
grep -q 'call ptr @quidra_string_append_move_many' "$TMP/string-growth.ll"

cat > "$TMP/fixed-array-layout.qui" <<'QUI'
int[2][3] matrix = [
    [1, 2, 3],
    [4, 5, 6],
]
print(matrix[1][2])
matrix[0][1] = 9
print(matrix[0][1])

int[2][2][] groups = [
    [[1], [2, 3]],
    [[4, 5, 6], []],
]
print(len(groups))
print(len(groups[0]))
print(len(groups[0][1]))
print(groups[1][0][2])
QUI
fixed_array_output=$("$QUIDRA" run "$TMP/fixed-array-layout.qui")
fixed_array_expected=$(printf '6\n9\n2\n2\n2\n6')
[[ "$fixed_array_output" == "$fixed_array_expected" ]]
"$QUIDRA" llvm "$TMP/fixed-array-layout.qui" > "$TMP/fixed-array-layout.ll"
grep -q 'call ptr @quidra_alloc(i64 48)' "$TMP/fixed-array-layout.ll"
grep -q 'call ptr @quidra_fixed_array_slot' "$TMP/fixed-array-layout.ll"

cat > "$TMP/fixed-array-reference.qui" <<'QUI'
int[2][4] matrix = [
    [1, 2, 3, 4],
    [5, 6, 7, 8],
]
int[4] &row = &matrix[0]
row[1] = 11
print(matrix[0][1])
int[4] replacement = [9, 10, 11, 12]
row = replacement
print(matrix[0][0])
print(matrix[0][3])
QUI
fixed_reference_output=$("$QUIDRA" run "$TMP/fixed-array-reference.qui")
fixed_reference_expected=$(printf '11\n9\n12')
[[ "$fixed_reference_output" == "$fixed_reference_expected" ]]

cat > "$TMP/fixed-to-dynamic.qui" <<'QUI'
int[2][3] fixed = [
    [1, 2, 3],
    [4, 5, 6],
]
int[][] dynamic = fixed
fixed[0][0] = 9
print(dynamic[0][0])
print(dynamic[1][2])

int[3] row = [7, 8, 9]
int[] dynamic_row = row
row[1] = 99
print(dynamic_row[1])
QUI
fixed_to_dynamic_output=$("$QUIDRA" run "$TMP/fixed-to-dynamic.qui")
fixed_to_dynamic_expected=$(printf '1\n6\n8')
[[ "$fixed_to_dynamic_output" == "$fixed_to_dynamic_expected" ]]

cat > "$TMP/value-memory.qui" <<'QUI'
float consume(float[] values)
    return values[0] + values[63]

float[] values = array(64, fill = 1.0)
float total = 0.0
for i in range(0, 100000)
    total = total + consume(values)
print(total)
QUI
"$QUIDRA" build "$TMP/value-memory.qui" -o "$TMP/value-memory"
measure_rss "$TMP/value-memory.rss" "$TMP/value-memory.out" "$TMP/value-memory"
grep -q '^200000\.0$' "$TMP/value-memory.out"
value_memory_rss="$(cat "$TMP/value-memory.rss")"
if [[ -n "$TIME_BIN" && -z "${ASAN_OPTIONS:-}" && "$value_memory_rss" -ge 80000 ]]; then
    echo "value-semantics memory regression: peak RSS ${value_memory_rss} KiB" >&2
    exit 1
fi

echo "value-semantics memory peak RSS: ${value_memory_rss} KiB"

cat > "$TMP/read-only-borrow.qui" <<'QUI'
float read_first(float[] values)
    return values[0]

void mutate_copy(float[] values)
    values[0] = 99.0

float[] values = [1.0, 2.0]
print(read_first(values))
mutate_copy(values)
print(values[0])
print(read_first([3.0, 4.0]))
QUI
read_only_borrow_output="$("$QUIDRA" run "$TMP/read-only-borrow.qui")"
read_only_borrow_expected=$(printf '1.0\n1.0\n3.0')
[[ "$read_only_borrow_output" == "$read_only_borrow_expected" ]]
"$QUIDRA" ir "$TMP/read-only-borrow.qui" > "$TMP/read-only-borrow.ir"
python3 - "$TMP/read-only-borrow.ir" <<'PY'
import sys
text=open(sys.argv[1]).read()
entry=text.split("function $entry(",1)[1].split("\nend\n",1)[0]
first=entry.index("call read_first")
mutate=entry.index("call mutate_copy")
second=entry.index("call read_first", first + 1)
assert "clone" not in entry[:first]
assert "clone" in entry[first:mutate]
assert "release" in entry[second:second + 220]
PY

cat > "$TMP/temporary-memory.qui" <<'QUI'
float[] make_values()
    return array(64, fill = 1.0)

string prefix = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
int total = 0
for i in range(0, 100000)
    make_values()
    total += len(prefix + i.string())
print(total > 0)

int[] original = [1, 2, 3]
int &kept = &original[0]
original = [4, 5, 6]
print(kept)
&kept = &original[0]
print(kept)
QUI
"$QUIDRA" build "$TMP/temporary-memory.qui" -o "$TMP/temporary-memory"
measure_rss "$TMP/temporary-memory.rss" "$TMP/temporary-memory.out" "$TMP/temporary-memory"
temporary_memory_expected=$(printf 'true\n1\n4')
[[ "$(cat "$TMP/temporary-memory.out")" == "$temporary_memory_expected" ]]
temporary_memory_rss="$(cat "$TMP/temporary-memory.rss")"
echo "temporary-value memory peak RSS: ${temporary_memory_rss} KiB"
if [[ -n "$TIME_BIN" && -z "${ASAN_OPTIONS:-}" && "$temporary_memory_rss" -ge 80000 ]]; then
    echo "temporary-value memory regression: peak RSS ${temporary_memory_rss} KiB" >&2
    exit 1
fi

cat > "$TMP/ownership-transfer.qui" <<'QUI'
string make_text()
    return "alpha" + "beta"

string source = make_text()
string copied = source.string()
source = "changed"
print(copied)

string message = make_text()
error problem = error(message)
message = "changed"
print(problem)

int[] | error make_numbers()
    int[] values = array(64, fill = 1)
    return values

int | error consume_numbers()
    int total = 0
    for value in try make_numbers()
        total += value
    return total

for i in range(0, 200000)
    consume_numbers()
print("ownership-ok")
QUI
"$QUIDRA" build "$TMP/ownership-transfer.qui" -o "$TMP/ownership-transfer"
measure_rss "$TMP/ownership-transfer.rss" "$TMP/ownership-transfer.out" "$TMP/ownership-transfer"
ownership_expected=$(printf 'alphabeta\nalphabeta\nownership-ok')
[[ "$(cat "$TMP/ownership-transfer.out")" == "$ownership_expected" ]]
ownership_rss="$(cat "$TMP/ownership-transfer.rss")"
echo "ownership-transfer memory peak RSS: ${ownership_rss} KiB"
if [[ -n "$TIME_BIN" && -z "${ASAN_OPTIONS:-}" && "$ownership_rss" -ge 80000 ]]; then
    echo "ownership-transfer memory regression: peak RSS ${ownership_rss} KiB" >&2
    exit 1
fi

cat > "$TMP/union-match-borrow.qui" <<'QUI'
class LinearKernel
    float bias = 0.0

    float apply(float value)
        return value + bias

class RbfKernel
    float gamma = 1.0

    float apply(float value)
        return value * gamma

float evaluate(LinearKernel | RbfKernel kernel, float value)
    match kernel
        LinearKernel linear_kernel
            return linear_kernel.apply(value)
        RbfKernel rbf
            return rbf.apply(value)

void mutate(LinearKernel | RbfKernel kernel)
    match kernel
        LinearKernel linear_kernel
            linear_kernel.bias = 7.0
        RbfKernel rbf
            rbf.gamma = 9.0

LinearKernel | RbfKernel kernel = RbfKernel(gamma = 2.0)
print(evaluate(kernel, 3.0))
mutate(kernel)
print(evaluate(kernel, 3.0))
QUI
union_match_borrow_output="$("$QUIDRA" run "$TMP/union-match-borrow.qui")"
union_match_borrow_expected=$(printf '6.0\n6.0')
[[ "$union_match_borrow_output" == "$union_match_borrow_expected" ]]
"$QUIDRA" ir "$TMP/union-match-borrow.qui" > "$TMP/union-match-borrow.ir"
python3 - "$TMP/union-match-borrow.ir" <<'PY'
import sys
text=open(sys.argv[1]).read()
evaluate=text.split("function evaluate(",1)[1].split("\nend\n",1)[0]
mutate=text.split("function mutate(",1)[1].split("\nend\n",1)[0]
assert "store.borrow" in evaluate
assert "clone" not in evaluate
assert "clone" in mutate
assert "call $method.RbfKernel.apply(" in evaluate
PY

cat > "$TMP/union-conversion-memory.qui" <<'QUI'
int | none maybe_number(bool present)
    if present
        return 7
    return none

int consume(bool present)
    int | string | none value = maybe_number(present)
    match value
        int number
            return number
        string text
            return len(text)
        none
            return 0

int total = 0
for i in range(0, 200000)
    total += consume(i % 2 == 0)
print(total)
QUI
"$QUIDRA" build "$TMP/union-conversion-memory.qui" -o "$TMP/union-conversion-memory"
measure_rss "$TMP/union-conversion-memory.rss" "$TMP/union-conversion-memory.out" "$TMP/union-conversion-memory"
grep -q '^700000$' "$TMP/union-conversion-memory.out"
union_conversion_rss="$(cat "$TMP/union-conversion-memory.rss")"
echo "union-conversion memory peak RSS: ${union_conversion_rss} KiB"
if [[ -n "$TIME_BIN" && -z "${ASAN_OPTIONS:-}" && "$union_conversion_rss" -ge 80000 ]]; then
    echo "union-conversion memory regression: peak RSS ${union_conversion_rss} KiB" >&2
    exit 1
fi

cat > "$TMP/out-of-bounds.qui" <<'QUI'
int[] values = [1, 2, 3]
print(values[3])
QUI
set +e
"$QUIDRA" run "$TMP/out-of-bounds.qui" > "$TMP/out-of-bounds.out" 2>&1
out_of_bounds_rc=$?
set -e
[[ "$out_of_bounds_rc" -eq 101 ]]
grep -q 'INDEX_BOUNDS' "$TMP/out-of-bounds.out"
grep -q 'at 2:7:' "$TMP/out-of-bounds.out"
grep -q 'index 3 outside length 3' "$TMP/out-of-bounds.out"

cat > "$TMP/full-array-proof.qui" <<'QUI'
int[] values = array(4, fill = 1)
print(values[2])
QUI
"$QUIDRA" llvm "$TMP/full-array-proof.qui" > "$TMP/full-array-proof.ll"
if grep -q 'call void @quidra_init_check' "$TMP/full-array-proof.ll"; then
    echo "fully initialized array retained a redundant initialization check" >&2
    exit 1
fi
[[ "$("$QUIDRA" run "$TMP/full-array-proof.qui")" == "1" ]]

cat > "$TMP/full-array-write-proof.qui" <<'QUI'
int[] values = array(4, fill = 1)
values[2] = 9
print(values[2])
QUI
"$QUIDRA" llvm "$TMP/full-array-write-proof.qui" > "$TMP/full-array-write-proof.ll"
full_array_mark_count="$(grep -c 'call void @quidra_init_mark_range' "$TMP/full-array-write-proof.ll" || true)"
[[ "$full_array_mark_count" -eq 0 ]]
[[ "$("$QUIDRA" run "$TMP/full-array-write-proof.qui")" == "9" ]]

cat > "$TMP/loop-array-proof.qui" <<'QUI'
int[] values = array(8)
for i in range(len(values))
    values[i] = i * 2
print(values[7])
QUI
"$QUIDRA" llvm "$TMP/loop-array-proof.qui" > "$TMP/loop-array-proof.ll"
if grep -q 'call void @quidra_init_check' "$TMP/loop-array-proof.ll"; then
    echo "full-range initialization loop retained a redundant init check" >&2
    exit 1
fi
[[ "$("$QUIDRA" run "$TMP/loop-array-proof.qui")" == "14" ]]

cat > "$TMP/partial-loop-array-proof.qui" <<'QUI'
int[] values = array(8)
for i in range(0, 7)
    values[i] = i
print(values[7])
QUI
"$QUIDRA" llvm "$TMP/partial-loop-array-proof.qui" > "$TMP/partial-loop-array-proof.ll"
grep -q 'call void @quidra_init_check' "$TMP/partial-loop-array-proof.ll"
set +e
"$QUIDRA" run "$TMP/partial-loop-array-proof.qui" > "$TMP/partial-loop-array-proof.out" 2>&1
partial_loop_array_rc=$?
set -e
[[ "$partial_loop_array_rc" -eq 101 ]]
grep -qi 'uninitialized' "$TMP/partial-loop-array-proof.out"


# Match cases are mutually exclusive: an initialization proof learned in one
# case must not leak into a later case.
cat > "$TMP/match-array-proof-isolation.qui" <<'QUI'
int | string choice = "text"
int[] values = array(2)
int index = 0
match choice
    int
        values = [1, 2]
    string
        print(values[index])
QUI
"$QUIDRA" llvm "$TMP/match-array-proof-isolation.qui" > "$TMP/match-array-proof-isolation.ll"
grep -q 'call void @quidra_init_check' "$TMP/match-array-proof-isolation.ll"
set +e
"$QUIDRA" run "$TMP/match-array-proof-isolation.qui" > "$TMP/match-array-proof-isolation.out" 2>&1
match_array_isolation_rc=$?
set -e
[[ "$match_array_isolation_rc" -eq 101 ]]
grep -qi 'uninitialized' "$TMP/match-array-proof-isolation.out"

# A proof may survive the join only when every continuing case establishes it.
cat > "$TMP/match-array-proof-join.qui" <<'QUI'
int | string choice = 1
int[] values = array(2)
match choice
    int
        values = [3, 4]
    string
        values = [5, 6]
print(values[1])
QUI
"$QUIDRA" llvm "$TMP/match-array-proof-join.qui" > "$TMP/match-array-proof-join.ll"
if grep -q 'call void @quidra_init_check' "$TMP/match-array-proof-join.ll"; then
    echo "match join lost a proof true in every continuing case" >&2
    exit 1
fi
[[ "$("$QUIDRA" run "$TMP/match-array-proof-join.qui")" == "4" ]]

cat > "$TMP/zero-filled-array-proof.qui" <<'QUI'
float[] values = array(1000, fill = 0.0)
print(values[999])
QUI
"$QUIDRA" ir "$TMP/zero-filled-array-proof.qui" > "$TMP/zero-filled-array-proof.ir"
if grep -q 'fill.cond' "$TMP/zero-filled-array-proof.ir"; then
    echo "zero-filled array retained a redundant element fill loop" >&2
    exit 1
fi
[[ "$("$QUIDRA" run "$TMP/zero-filled-array-proof.qui")" == "0.0" ]]

cat > "$TMP/partial-array-proof.qui" <<'QUI'
int[] values = array(4)
values[2] = 7
print(values[2])
QUI
"$QUIDRA" llvm "$TMP/partial-array-proof.qui" > "$TMP/partial-array-proof.ll"
grep -q 'call void @quidra_init_check' "$TMP/partial-array-proof.ll"
[[ "$("$QUIDRA" run "$TMP/partial-array-proof.qui")" == "7" ]]

cat > "$TMP/escaped-array-proof.qui" <<'QUI'
void reset(int[] &values)
    values = array(4)

int[] values = array(4, fill = 1)
reset(&values)
print(values[0])
QUI
"$QUIDRA" llvm "$TMP/escaped-array-proof.qui" > "$TMP/escaped-array-proof.ll"
grep -q 'call void @quidra_init_check' "$TMP/escaped-array-proof.ll"
set +e
"$QUIDRA" run "$TMP/escaped-array-proof.qui" > "$TMP/escaped-array-proof.out" 2>&1
escaped_array_rc=$?
set -e
[[ "$escaped_array_rc" -eq 101 ]]
grep -qi 'uninitialized' "$TMP/escaped-array-proof.out"

cat > "$TMP/readonly-array-proof.qui" <<'QUI'
void inspect(const int[] &values)
    print(values[0])

int[] values = array(4, fill = 1)
inspect(&values)
print(values[2])
QUI
"$QUIDRA" llvm "$TMP/readonly-array-proof.qui" > "$TMP/readonly-array-proof.ll"
readonly_init_check_count="$(grep -c 'call void @quidra_init_check' "$TMP/readonly-array-proof.ll" || true)"
[[ "$readonly_init_check_count" -eq 1 ]]
readonly_array_output=$("$QUIDRA" run "$TMP/readonly-array-proof.qui")
readonly_array_expected=$(printf '1\n1')
[[ "$readonly_array_output" == "$readonly_array_expected" ]]

cat > "$TMP/readonly-local-array-proof.qui" <<'QUI'
int[] values = array(4, fill = 2)
const int[] &view = &values
print(values[3])
print(view[1])
QUI
"$QUIDRA" llvm "$TMP/readonly-local-array-proof.qui" > "$TMP/readonly-local-array-proof.ll"
readonly_local_init_check_count="$(grep -c 'call void @quidra_init_check' "$TMP/readonly-local-array-proof.ll" || true)"
[[ "$readonly_local_init_check_count" -eq 1 ]]
readonly_local_output=$("$QUIDRA" run "$TMP/readonly-local-array-proof.qui")
readonly_local_expected=$(printf '2\n2')
[[ "$readonly_local_output" == "$readonly_local_expected" ]]

cat > "$TMP/loop-control.qui" <<'QUI'
int sum = 0
for i in range(0, 10)
    if i == 2
        continue
    if i == 6
        break
    sum = sum + i
print(sum)

int[] values = [1, 2, 3]
for &value in values
    value = value * 10
    if value == 20
        continue
    if value == 30
        break
print(values[0])
print(values[1])
print(values[2])
QUI
loop_control_output=$("$QUIDRA" run "$TMP/loop-control.qui")
loop_control_expected=$(printf '13\n10\n20\n30')
[[ "$loop_control_output" == "$loop_control_expected" ]]

cat > "$TMP/compound-assignment.qui" <<'QUI'
int next(int &calls)
    calls += 1
    return 0

int calls = 0
int[] values = [10]
values[next(&calls)] += 5
print(values[0])
print(calls)

int x = 20
x -= 3
x *= 2
x /= 17
x %= 2
print(x)

string suffix()
    return "b" + "c"

string text = "a"
text += suffix()
print(text)
QUI
compound_output=$("$QUIDRA" run "$TMP/compound-assignment.qui")
compound_expected=$(printf '15\n1\n0\nabc')
[[ "$compound_output" == "$compound_expected" ]]

cat > "$TMP/temporary-reference.qui" <<'QUI'
int &item = &array(1, fill = 1)[0]
print(item)
QUI
set +e
"$QUIDRA" check "$TMP/temporary-reference.qui" --json > "$TMP/temporary-reference.json"
temporary_reference_rc=$?
set -e
[[ "$temporary_reference_rc" -eq 1 ]]
grep -q 'REFERENCE_BINDING' "$TMP/temporary-reference.json"

cat > "$TMP/temporary-write.qui" <<'QUI'
array(1, fill = int(1))[0] = 2
QUI
set +e
"$QUIDRA" check "$TMP/temporary-write.qui" --json > "$TMP/temporary-write.json"
temporary_write_rc=$?
set -e
[[ "$temporary_write_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/temporary-write.json"

$QUIDRA check "$ROOT/examples/results.qui" --json > "$TMP/check.json"
grep -q '"ok":true' "$TMP/check.json"
$QUIDRA inspect "$ROOT/examples/functions.qui" > "$TMP/inspect.json"
grep -q '"schema_version":1' "$TMP/inspect.json"

cat > "$TMP/stack-guard.qui" <<'QUI'
int plus_one(int value)
    return value + 1

int recurse(int value)
    if value == 0
        return 0
    return recurse(value - 1)

print(plus_one(1))
QUI
$QUIDRA llvm "$TMP/stack-guard.qui" > "$TMP/stack-guard.ll"
python3 - "$TMP/stack-guard.ll" <<'PY'
import re,sys
text=open(sys.argv[1]).read()
def body(symbol):
    match=re.search(r"define [^{]+ @"+re.escape(symbol)+r"\([^)]*\) \{(.*?)\n\}", text, re.S)
    assert match, symbol
    return match.group(1)
assert "@quidra_stack_enter()" not in body("n_plus_one")
assert "@quidra_stack_leave()" not in body("n_plus_one")
assert "@quidra_stack_enter()" in body("n_recurse")
assert "@quidra_stack_leave()" in body("n_recurse")
PY

cat > "$TMP/llvm-import-lib.qui" <<'QUI'
int imported_value()
    return 7
QUI
cat > "$TMP/llvm-import-root.qui" <<'QUI'
import lib = "./llvm-import-lib.qui"
print(lib.imported_value())
QUI
$QUIDRA llvm "$TMP/llvm-import-root.qui" > "$TMP/llvm-import.ll"
grep -q 'define.*imported_value' "$TMP/llvm-import.ll"

cat > "$TMP/effects.qui" <<'QUI'
class EffectBox
    int x
    int y

    int read()
        return x

    void initialize()
        y = 7

void fill(int &value)
    value = 5

int | none maybe(bool present)
    if present
        return 1
    return none
QUI
$QUIDRA inspect "$TMP/effects.qui" > "$TMP/effects.json"
python3 - "$TMP/effects.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert any(n["kind"]=="none" for n in x["nodes"])
effects=x["effects"]
assert any("x" in e["receiver"]["requires"] for e in effects)
assert any("y" in e["receiver"]["writes"] and "y" in e["receiver"]["initializes"] for e in effects)
ref=[e for e in effects if "value" in e["references"]]
assert ref
value=ref[0]["references"]["value"]
assert "" in value["writes"]
assert "" in value["initializes"]
PY

cat > "$TMP/effect-isolation.qui" <<'QUI'
class Mutable
    int counter

    void bump()
        counter = counter + 1

class Pure
    int a
    int b

    int total()
        return a + b
QUI
"$QUIDRA" inspect "$TMP/effect-isolation.qui" > "$TMP/effect-isolation.json"
python3 - "$TMP/effect-isolation.json" <<'PY'
import json,sys
effects={e["function"]:e for e in json.load(open(sys.argv[1]))["effects"]}
mutable=effects["$method.Mutable.bump"]["receiver"]
pure=effects["$method.Pure.total"]["receiver"]
assert "counter" in mutable["writes"]
assert pure["writes"] == []
assert pure["invalidates"] == []
assert pure["requires"] == ["a","b"]
PY

cat > "$TMP/nested-effects.qui" <<'QUI'
class Inner
    int x

class Box
    Inner inner
    int y

void initialize_x(Box &box)
    box.inner.x = 7

int read_y(Box &box)
    return box.y

void initialize_x_both(Box &box, bool flag)
    if flag
        box.inner.x = 1
    else
        box.inner.x = 2

void initialize_x_one_branch(Box &box, bool flag)
    if flag
        box.inner.x = 1

Box box = Box(inner = Inner())
initialize_x(&box)
print(box.inner.x)

Box both = Box(inner = Inner())
initialize_x_both(&both, true)
print(both.inner.x)
QUI
nested_output=$("$QUIDRA" run "$TMP/nested-effects.qui")
nested_expected=$(printf '7\n1')
[[ "$nested_output" == "$nested_expected" ]]

"$QUIDRA" inspect "$TMP/nested-effects.qui" > "$TMP/nested-effects.json"
python3 - "$TMP/nested-effects.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
effects=x["effects"]
init=next(
    e for e in effects
    if "box" in e["references"] and "inner.x" in e["references"]["box"]["initializes"]
)
assert "inner.x" in init["references"]["box"]["writes"]
reader=next(
    e for e in effects
    if "box" in e["references"] and "y" in e["references"]["box"]["requires"]
)
assert "" not in reader["references"]["box"]["requires"]
both=[
    e for e in effects
    if "box" in e["references"] and "inner.x" in e["references"]["box"]["initializes"]
]
assert len(both) >= 2
PY

cat > "$TMP/nested-effect-failure.qui" <<'QUI'
class Inner
    int x

class Box
    Inner inner

void initialize_x_one_branch(Box &box, bool flag)
    if flag
        box.inner.x = 1

Box box = Box(inner = Inner())
initialize_x_one_branch(&box, false)
print(box.inner.x)
QUI
set +e
"$QUIDRA" check "$TMP/nested-effect-failure.qui" --json > "$TMP/nested-effect-failure.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/nested-effect-failure.json"

cat > "$TMP/reference-field-effects.qui" <<'QUI'
class Pair
    int x
    int y

    void initialize_x()
        x = 11

void set_x(Pair &pair)
    pair.x = 7

int read_x(Pair &pair)
    return pair.x

void initialize_through_method(Pair &pair)
    pair.initialize_x()

void choose_x(bool first, Pair &pair)
    if first
        pair.x = 3
    else
        pair.x = 4

void maybe_y(bool enabled, Pair &pair)
    if enabled
        pair.y = 9

Pair direct = Pair()
set_x(&direct)
print(read_x(&direct))

Pair through_method = Pair()
initialize_through_method(&through_method)
print(read_x(&through_method))

Pair branched = Pair()
choose_x(true, &branched)
print(read_x(&branched))
QUI
reference_output=$("$QUIDRA" run "$TMP/reference-field-effects.qui")
reference_expected=$(printf '7\n11\n3')
[[ "$reference_output" == "$reference_expected" ]]

"$QUIDRA" inspect "$TMP/reference-field-effects.qui" > "$TMP/reference-field-effects.json"
python3 - "$TMP/reference-field-effects.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
effects=x["effects"]
pair_effects=[e["references"]["pair"] for e in effects if "pair" in e["references"]]
assert any("x" in e["writes"] and "x" in e["initializes"] for e in pair_effects)
assert any("x" in e["requires"] and "" not in e["requires"] for e in pair_effects)
assert any("x" in e["initializes"] for e in pair_effects)
assert any("y" in e["writes"] and "y" not in e["initializes"] for e in pair_effects)
PY

cat > "$TMP/value.qui" <<'QUI'
void touch(int[] data)
    data[0] = 9
void change(int[] &data)
    data[0] = 9
int[] values = [1, 2, 3]
touch(values)
print(values[0])
change(&values)
print(values[0])
QUI
[[ "$($QUIDRA run "$TMP/value.qui")" == $'1\n9' ]]

cat > "$TMP/address-model.qui" <<'QUI'
void initialize(int &x)
    x = 13

class Point
    int x
    int y

class Counter
    int value

    void reset()
        value = 0

    void increment()
        value = value + 1

int a = 1
int c = 2
int &b = &a
b = 5
&b = &c
b = 8
print(a)
print(c)

int uninitialized
int &u = &uninitialized
u = 11
print(uninitialized)

int from_function
initialize(&from_function)
print(from_function)

int[] values = [1, 2, 3]
int &first = &values[0]
values = [4, 5]
print(first)
print(values[0])
first = 9
print(values[0])

Point p = Point(x = 1)
int &field = &p.y
field = 5
print(p.y)

int &old_x = &p.x
p = Point(x = 10, y = 20)
print(old_x)
print(p.x)

Point &whole = &p
p = Point(x = 30, y = 40)
print(whole.x)

Point partial = Point(x = 7)
Point copy = partial
copy.y = 6
print(copy.x)
print(copy.y)

Counter counter = Counter()
counter.reset()
counter.increment()
print(counter.value)
QUI
[[ "$($QUIDRA run "$TMP/address-model.qui")" == $'5\n8\n11\n13\n1\n4\n4\n5\n1\n10\n30\n7\n6\n1' ]]


# Rebinding heap substorage repeatedly stresses pin/unpin and deferred parent
# finalization. The final reference must still point at the pre-replacement
# right-hand storage rather than becoming dangling or retargeted.
cat > "$TMP/reference-rebind-lifetime.qui" <<'QUI'
int[] left = [1]
int[] right = [2]
int &slot = &left[0]
for i in range(0, 2000)
    &slot = &left[0]
    left = [i]
    &slot = &right[0]
    right = [i]
slot = 77
print(slot)
print(right[0])
QUI
reference_rebind_lifetime_output=$("$QUIDRA" run "$TMP/reference-rebind-lifetime.qui")
reference_rebind_lifetime_expected=$(printf '77\n1999')
[[ "$reference_rebind_lifetime_output" == "$reference_rebind_lifetime_expected" ]]

cat > "$TMP/import-shadow.qui" <<'QUI'
int file = 1
QUI
set +e
"$QUIDRA" check "$TMP/import-shadow.qui" --json > "$TMP/import-shadow.json"
import_shadow_rc=$?
set -e
[[ "$import_shadow_rc" -eq 1 ]]
grep -q 'SHADOWING' "$TMP/import-shadow.json"

cat > "$TMP/import-builtin-shadow.qui" <<'QUI'
import input = plotting
QUI
set +e
"$QUIDRA" check "$TMP/import-builtin-shadow.qui" --json > "$TMP/import-builtin-shadow.json"
import_builtin_shadow_rc=$?
set -e
[[ "$import_builtin_shadow_rc" -eq 1 ]]
grep -q 'SHADOWING' "$TMP/import-builtin-shadow.json"

mkdir -p "$TMP/project/src" "$TMP/project/lib"
cat > "$TMP/project/lib/util.qui" <<'QUI'
int bump(int value)
    return value + 1
QUI

cat > "$TMP/project/src/geometry.qui" <<'QUI'
import util = "../lib/util.qui"

class Point
    int x
    int y

int sum(Point point)
    return util.bump(point.x + point.y)
QUI

cat > "$TMP/project/shared.qui" <<'QUI'
class Box<T>
    T value

    T get()
        return value

T first<T>(T[] values)
    return values[0]

class GenericParent
    T echo<T>(T value)
        return value

class GenericChild : GenericParent
    override T echo<T>(T value)
        return super.echo<T>(value)
QUI

cat > "$TMP/project/math.qui" <<'QUI'
int square(int value)
    return value * value
QUI

cat > "$TMP/project/src/main.qui" <<'QUI'
import geo = "./geometry.qui"
import shared = "@/shared.qui"
import m = "@/math.qui"

geo.Point point = geo.Point(x = 2, y = 3)
shared.Box<int> box = shared.Box<int>(value = 7)
shared.GenericChild child = shared.GenericChild()

print(geo.sum(point))
print(box.get())
print(shared.first<int>([9, 10]))
print(m.square(4))
print(child.echo<int>(11))
QUI

[[ "$(cd "$TMP/project" && "$QUIDRA" run src/main.qui)" == $'6\n7\n9\n16\n11' ]]

cat > "$TMP/project/src/generic-inference.qui" <<'QUI'
T identity<T>(T value)
    return value

class Echo
    T echo<T>(T value)
        return value

int sample = 7
int inferred = identity(sample)
Echo instance = Echo()
int method_inferred = instance.echo(sample)
print(inferred)
print(method_inferred)
QUI
[[ "$(cd "$TMP/project" && "$QUIDRA" run src/generic-inference.qui)" == $'7\n7' ]]

cat > "$TMP/project/src/stdlib-math.qui" <<'QUI'
float pi_value = math.pi
float e_value = math.e
print(pi_value)
print(e_value)
print(math.sin(float(0.0)))
print(math.cos(float(0.0)))
print(math.pow(float(2.0), 3.0))
QUI
stdlib_math_output="$(cd "$TMP/project" && "$QUIDRA" run src/stdlib-math.qui)"
python3 - "$stdlib_math_output" <<'PY'
import math,sys
values=[float(x) for x in sys.argv[1].splitlines()]
assert abs(values[0]-math.pi) < 1e-15
assert abs(values[1]-math.e) < 1e-15
assert values[2] == 0.0
assert values[3] == 1.0
assert values[4] == 8.0
PY

cat > "$TMP/project/src/unknown-standard.qui" <<'QUI'
import definitely_not_a_standard_module
print("no")
QUI
set +e
(cd "$TMP/project" && "$QUIDRA" check src/unknown-standard.qui --json) > "$TMP/unknown-standard.json"
unknown_standard_rc=$?
set -e
[[ "$unknown_standard_rc" -eq 1 ]]
grep -q 'PACKAGE_NOT_INSTALLED' "$TMP/unknown-standard.json"
(cd "$TMP/project" && "$QUIDRA" inspect src/main.qui) > "$TMP/module-inspect.json"
python3 - "$TMP/module-inspect.json" "$TMP/project/src/main.qui" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
size=len(open(sys.argv[2],"rb").read())
assert x["ok"] is True
assert x["nodes"]
assert all(0 <= n["span"]["start"]["offset"] <= size for n in x["nodes"])
assert all(0 <= n["span"]["end"]["offset"] <= size for n in x["nodes"])
PY

cat > "$TMP/project/src/module-patch.qui" <<'QUI'
import m = "@/math.qui"
int value = 3
print(m.square(value))
QUI
(cd "$TMP/project" && "$QUIDRA" inspect src/module-patch.qui) > "$TMP/module-patch-inspect.json"
python3 - "$TMP/module-patch-inspect.json" "$TMP/module-patch-change.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
node=next(n for n in x["nodes"] if n["kind"]=="integer" and n["source"]=="3")
json.dump({
    "schema_version":1,
    "base_revision":x["revision"],
    "operations":[{
        "op":"replace_node",
        "node_id":node["node_id"],
        "expected_hash":node["source_hash"],
        "replacement":"4"
    }]
},open(sys.argv[2],"w"))
PY
(cd "$TMP/project" && "$QUIDRA" patch src/module-patch.qui "$TMP/module-patch-change.json" --write) >/dev/null
[[ "$(cd "$TMP/project" && "$QUIDRA" run src/module-patch.qui)" == "16" ]]

cat > "$TMP/project/src/bad-module.qui" <<'QUI'
print("side effect")
QUI
cat > "$TMP/project/src/bad-import.qui" <<'QUI'
import bad = "./bad-module.qui"
print("root")
QUI
set +e
(cd "$TMP/project" && "$QUIDRA" check src/bad-import.qui --json) > "$TMP/import-error.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'IMPORT_TOP_LEVEL' "$TMP/import-error.json"

cat > "$TMP/project/src/cycle-a.qui" <<'QUI'
import b = "./cycle-b.qui"
int a()
    return 1
QUI
cat > "$TMP/project/src/cycle-b.qui" <<'QUI'
import a = "./cycle-a.qui"
int b()
    return 2
QUI
cat > "$TMP/project/src/cycle-root.qui" <<'QUI'
import a = "./cycle-a.qui"
print(a.a())
QUI
set +e
(cd "$TMP/project" && "$QUIDRA" check src/cycle-root.qui --json) > "$TMP/import-cycle.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'IMPORT_CYCLE' "$TMP/import-cycle.json"

cat > "$TMP/project/src/alias-collision.qui" <<'QUI'
import geo = "./geometry.qui"
int geo()
    return 1
print(geo())
QUI
set +e
(cd "$TMP/project" && "$QUIDRA" check src/alias-collision.qui --json) > "$TMP/import-alias.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'DUPLICATE_IMPORT_ALIAS' "$TMP/import-alias.json"

cat > "$TMP/project/src/alias-shadow.qui" <<'QUI'
import geo = "./geometry.qui"
int geo = 1
print(geo)
QUI
set +e
(cd "$TMP/project" && "$QUIDRA" check src/alias-shadow.qui --json) > "$TMP/import-shadow.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'SHADOWING' "$TMP/import-shadow.json"

cat > "$TMP/class-init-summary.qui" <<'QUI'
class Model
    float bb

Model build()
    return Model(bb = 2.0)

class Data
    float[] ys

class Holder
    Data data

    float first()
        data.ys = [3.0]
        return data.ys[0]

Model model = build()
Holder holder = Holder(data = Data())
print(model.bb)
print(holder.first())
QUI
[[ "$($QUIDRA run "$TMP/class-init-summary.qui")" == $'2.0\n3.0' ]]

cat > "$TMP/constant-zero.qui" <<'QUI'
print(int(1) / 0)
QUI
set +e
$QUIDRA check "$TMP/constant-zero.qui" --json > "$TMP/constant-zero.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'DIVIDE_BY_ZERO' "$TMP/constant-zero.json"

cat > "$TMP/runtime-zero.qui" <<'QUI'
int zero = 0
print(1 / zero)
QUI
set +e
$QUIDRA run "$TMP/runtime-zero.qui" > "$TMP/runtime-zero.out" 2>&1
rc=$?
set -e
[[ "$rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[DIVISION_BY_ZERO\] at [0-9]+:[0-9]+: division by zero' "$TMP/runtime-zero.out"

cat > "$TMP/patch-target.qui" <<'QUI'
int answer = 41
print(answer)
QUI
$QUIDRA inspect "$TMP/patch-target.qui" > "$TMP/patch-inspect.json"
python3 - "$TMP/patch-inspect.json" "$TMP/change.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
node=next(n for n in x['nodes'] if n['kind']=='integer')
json.dump({'schema_version':1,'base_revision':x['revision'],'operations':[{'op':'replace_node','node_id':node['node_id'],'expected_hash':node['source_hash'],'replacement':'42'}]},open(sys.argv[2],'w'))
PY
$QUIDRA patch "$TMP/patch-target.qui" "$TMP/change.json" --write >/dev/null
[[ "$($QUIDRA run "$TMP/patch-target.qui")" == "42" ]]

cat > "$TMP/regressions.qui" <<'QUI'
// Branch-local bindings may reuse a source name with different types.
bool c = true
if c
    int x = 1234567890123
    print(x)
else
    bool x = true
print("[" + "" + "]")
QUI
[[ "$($QUIDRA run "$TMP/regressions.qui")" == $'1234567890123\n[]' ]]

cat > "$TMP/builtins.qui" <<'QUI'
int[] values = [1, 2, 3]
print(len(values))
print(float(3))
print(abs(int(-5)))
print(abs(float(-2.5)))
print(sqrt(float(9.0)))
print(min(int(4), 2))
print(max(int(4), 2))
QUI
[[ "$($QUIDRA run "$TMP/builtins.qui")" == $'3\n3.0\n5\n2.5\n3.0\n2\n4' ]]

cat > "$TMP/float-format.qui" <<'QUI'
print(float(4.0))
print(float32(4.0))
print(float(4.5))
print(float(-2.0))
print("value={float(4.0)}")
write(float(6.0))
write("|")
print(float(7.0).string())
QUI
[[ "$($QUIDRA run "$TMP/float-format.qui")" == $'4.0\n4.0\n4.5\n-2.0\nvalue=4.0\n6.0|7.0' ]]

cat > "$TMP/interpolation-format.qui" <<'QUI'
float value = 12.3456
print("{value:frac=2}")
print("{value:int=4,frac=2,zero}")
print("{value:int=4,frac=2}")
print("{value:sig=4}")
print("{int(12345):sig=4}")
print("{float(0.00123456):sig=3}")
print("{int(12):sig=4}")
QUI
expected_format="$(printf '12.35\n0012.35\n  12.35\n12.35\n12350\n0.00123\n12.00')"
[[ "$($QUIDRA run "$TMP/interpolation-format.qui")" == "$expected_format" ]]

cat > "$TMP/interpolation-format-invalid.qui" <<'QUI'
print("{float(12.3):frac=2,sig=3}")
print("{int(12):zero}")
QUI
set +e
$QUIDRA check "$TMP/interpolation-format-invalid.qui" > "$TMP/interpolation-format-invalid.out" 2>&1
status=$?
set -e
[[ $status -ne 0 ]]
grep -Eq "frac.*sig|sig.*frac" "$TMP/interpolation-format-invalid.out"
grep -Eq "zero.*requires.*int" "$TMP/interpolation-format-invalid.out"

cat > "$TMP/numeric-types.qui" <<'QUI'
int8 a = 10
int8 b = 12
uint8 u = 200
uint8 v = 20
int16 widened = int16(a)
uint32 count = 100
int total = int(count)
float exact = float(a)
float32 compact = float32(1.5)
print(a + b)
print(b - a)
print(a * b)
print(b / a)
print(b % a)
print(u + v)
print(widened)
print(total)
print(exact)
print(compact)
print(a.string())
write("x")
write("y")
print("")
QUI
numeric_output="$($QUIDRA run "$TMP/numeric-types.qui")"
[[ "$numeric_output" == "$(printf '22\n2\n120\n1\n2\n220\n10\n100\n10.0\n1.5\n10\nxy')" ]]

cat > "$TMP/compact-storage.qui" <<'QUI'
int8[] small = [1, 2, 3]
uint16[] medium = [1000, 2000, 3000]
float32[] fractions = [1.5, 2.5]

class Mixed
    int8 a
    uint16 b
    float32 c
    uint8 d

Mixed value = Mixed(a = 7, b = 500, c = 1.5, d = 9)
int8 &a = &value.a
uint16 &b = &value.b
float32 &c = &value.c
uint8 &d = &value.d

a = 8
b = 600
c = 2.5
d = 10

print(small[2])
print(medium[1])
print(fractions[0])
print(value.a)
print(value.b)
print(value.c)
print(value.d)

Mixed copy = value
copy.b = 700
print(value.b)
print(copy.b)
QUI
compact_output="$($QUIDRA run "$TMP/compact-storage.qui")"
[[ "$compact_output" == "$(printf '3\n2000\n1.5\n8\n600\n2.5\n10\n600\n700')" ]]

cat > "$TMP/numeric-overflow.qui" <<'QUI'
int8 a = 120
int8 b = 10
print(a + b)
QUI
set +e
$QUIDRA run "$TMP/numeric-overflow.qui" > "$TMP/numeric-overflow.out" 2>&1
rc=$?
set -e
[[ "$rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[INTEGER_OVERFLOW\] at [0-9]+:[0-9]+: integer overflow' "$TMP/numeric-overflow.out"

cat > "$TMP/out-of-bounds.qui" <<'QUI'
int[] values = [1]
print(values[1])
QUI
set +e
"$QUIDRA" run "$TMP/out-of-bounds.qui" > "$TMP/out-of-bounds.out" 2>&1
bounds_rc=$?
set -e
[[ "$bounds_rc" -eq 101 ]]
grep -q 'INDEX_BOUNDS' "$TMP/out-of-bounds.out"
grep -q 'index 1 outside length 1' "$TMP/out-of-bounds.out"

cat > "$TMP/negative-index.qui" <<'QUI'
bin values = bin.fill(1, 0)
int index = -1
print(values[index])
QUI
set +e
"$QUIDRA" run "$TMP/negative-index.qui" > "$TMP/negative-index.out" 2>&1
negative_bounds_rc=$?
set -e
if [[ "$negative_bounds_rc" -ne 101 ]]; then
    echo "negative-index runtime returned $negative_bounds_rc" >&2
    cat "$TMP/negative-index.out" >&2
    exit 1
fi
grep -qi 'bounds\|INDEX_BOUNDS' "$TMP/negative-index.out"

cat > "$TMP/numeric-cast-failure.qui" <<'QUI'
int value = 300
print(int8(value))
QUI
set +e
$QUIDRA run "$TMP/numeric-cast-failure.qui" > "$TMP/numeric-cast-failure.out" 2>&1
rc=$?
set -e
[[ "$rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[NUMERIC_CAST_RANGE\] at [0-9]+:[0-9]+: numeric cast outside destination range' "$TMP/numeric-cast-failure.out"

cat > "$TMP/parse.qui" <<'QUI'
int | error parsed = int.parse("123")
float32 | error fraction = float32.parse("1.5")
int | error bad = int.parse("12x")
match parsed
    int value
        print(value)
    error e
        print(e)
match fraction
    float32 value
        print(value)
    error e
        print(e)
match bad
    int value
        print(value)
    error e
        print(e)
QUI
parse_output="$($QUIDRA run "$TMP/parse.qui")"
[[ "$parse_output" == "$(printf '123\n1.5\nnumeric parse failed')" ]]

cat > "$TMP/input.qui" <<'QUI'
auto line = input()
match line
    string value
        print(value)
    none
        print("eof")
    error e
        print(e)
QUI
input_output="$(printf 'hello\n' | $QUIDRA run "$TMP/input.qui")"
[[ "$input_output" == "hello" ]]
eof_output="$($QUIDRA run "$TMP/input.qui" < /dev/null)"
[[ "$eof_output" == "eof" ]]

invalid_input_output="$(python3 -c 'import sys; sys.stdout.buffer.write(b"\xff\n")' | "$QUIDRA" run "$TMP/input.qui")"
[[ "$invalid_input_output" == "input failed" ]]
nul_input_output="$(python3 -c 'import sys; sys.stdout.buffer.write(b"A\x00B\n")' | "$QUIDRA" run "$TMP/input.qui")"
[[ "$nul_input_output" == "input failed" ]]

cat > "$TMP/cli-text.qui" <<'QUI'
cli args
    string value = argument()

print(args.value)
QUI
cat > "$TMP/cli-exact.qui" <<'QUI'
cli args
    bigint count = argument()
    bigreal ratio = option(default = 0.1)

print(args.count)
print(args.ratio == bigreal(0.125))
QUI
[[ "$("$QUIDRA" run "$TMP/cli-exact.qui" -- 123456789012345678901234567890 --ratio 0.125)" == "$(printf '123456789012345678901234567890\ntrue')" ]]
[[ "$("$QUIDRA" run "$TMP/cli-exact.qui" -- 7)" == "$(printf '7\nfalse')" ]]

set +e
"$QUIDRA" run "$TMP/cli-exact.qui" -- nope >"$TMP/cli-exact-bigint.out" 2>"$TMP/cli-exact-bigint.err"
cli_exact_bigint_rc=$?
"$QUIDRA" run "$TMP/cli-exact.qui" -- 7 --ratio nope >"$TMP/cli-exact-bigreal.out" 2>"$TMP/cli-exact-bigreal.err"
cli_exact_bigreal_rc=$?
set -e
[[ "$cli_exact_bigint_rc" -eq 2 ]]
[[ "$cli_exact_bigreal_rc" -eq 2 ]]
grep -q 'Quidra CLI error: invalid bigint value' "$TMP/cli-exact-bigint.err"
grep -q 'Quidra CLI error: invalid bigreal value' "$TMP/cli-exact-bigreal.err"

python3 - "$QUIDRA" "$TMP/cli-text.qui" <<'PY'
import os
import subprocess
import sys

command = [
    os.fsencode(sys.argv[1]),
    b"run",
    os.fsencode(sys.argv[2]),
    b"--",
    b"\xff",
]
result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
if result.returncode != 2:
    raise SystemExit(f"invalid UTF-8 CLI status: {result.returncode}")
if b"Quidra CLI error:" not in result.stderr:
    raise SystemExit(f"missing CLI diagnostic: {result.stderr!r}")
PY

cat > "$TMP/bin-value.qui" <<'QUI'
bin data = bin.fill(4, 0)
data[0] = bin.parse("1")
bin copy = data
copy[2] = bin.parse("1")
print(len(data))
print(data[0])
print(data[1])
print(data[2])
print(copy[2])
print(data == copy)
copy[2] = data[2]
print(data == copy)
for value in data
    print(value)
for &value in copy
    value = bin.parse("1")
print(copy[0])
print(data[0])
QUI
bin_output="$($QUIDRA run "$TMP/bin-value.qui")"
[[ "$bin_output" == "$(printf '4\n1\n0\n0\n1\nfalse\ntrue\n1\n0\n0\n0\n1\n1')" ]]

cat > "$TMP/bin-fill.qui" <<'QUI'
bin empty = bin.fill(0, 0)
bin zeros = bin.fill(4, 0)
print(len(empty))
print(len(zeros))
print(zeros[0])
QUI
[[ "$("$QUIDRA" run "$TMP/bin-fill.qui")" == "$(printf '0\n4\n0')" ]]

cat > "$TMP/interpolation-span.qui" <<'QUI'
void show()
    string root = "x/{ghost.field}"
QUI
set +e
$QUIDRA check "$TMP/interpolation-span.qui" --json > "$TMP/interpolation-span.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
python3 - "$TMP/interpolation-span.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
d=x["diagnostics"][0]
assert d["span"]["start"] == {"line": 2, "column": 23}, d
PY

cat > "$TMP/interpolation-parse-span.qui" <<'QUI'
void show()
    string root = "x/{ghost.}"
QUI
set +e
$QUIDRA check "$TMP/interpolation-parse-span.qui" --json > "$TMP/interpolation-parse-span.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
python3 - "$TMP/interpolation-parse-span.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
d=x["diagnostics"][0]
assert d["code"] == "PARSE_ERROR", d
assert d["span"]["start"] == {"line": 2, "column": 29}, d
PY

cat > "$TMP/multi-errors.qui" <<'QUI'
bool a = 1
int b = true
string c = 3
float d = false
QUI
set +e
$QUIDRA check "$TMP/multi-errors.qui" --json --max-errors 3 > "$TMP/multi-errors.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
python3 - "$TMP/multi-errors.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["ok"] is False
assert x["truncated"] is True
assert len(x["diagnostics"]) == 3
PY

set +e
$QUIDRA check "$TMP/multi-errors.qui" --json --max-errors 10 > "$TMP/multi-errors-all.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
python3 - "$TMP/multi-errors-all.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["truncated"] is False
assert len(x["diagnostics"]) == 4
PY

cat > "$TMP/parser-errors.qui" <<'QUI'
int a =
bool b =
string c =
QUI
set +e
$QUIDRA check "$TMP/parser-errors.qui" --json --max-errors 10 > "$TMP/parser-errors.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
python3 - "$TMP/parser-errors.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["truncated"] is False
assert len(x["diagnostics"]) == 3
assert all(d["code"] == "PARSE_ERROR" for d in x["diagnostics"])
PY


cat > "$TMP/repl-input.txt" <<'QUI'
int(1) + 2
int x = 5
x
x = 8
x
int square(int value)
    return value * value

square(6)
class Box
    int value

Box box = Box(value = 9)
box.value
T identity<T>(T value)
    return value

identity<int>(7)
int[] repl_values = [1, 2, 3]
repl_values
bin.fill(3, 1)
box
Box partial = Box()
partial
none
"hello"
bool broken = 1
1 + 2
x
int(10) / 0
x
float y = 4.0
y
:type y
:exit
QUI
set +e
"$QUIDRA" repl < "$TMP/repl-input.txt" > "$TMP/repl.out" 2> "$TMP/repl.err"
repl_rc=$?
set -e
[[ "$repl_rc" -eq 0 ]]
python3 - "$TMP/repl.out" "$TMP/repl.err" <<'PY'
import sys
out=open(sys.argv[1]).read()
err=open(sys.argv[2]).read()
expected=[
    "3","5","8","36","9","7",
    "[1, 2, 3]","111","Box(value = 9)",
    "Box(value = <uninitialized>)","none","\"hello\"",
    "8","8","4.0","float"
]
position=0
for value in expected:
    found=out.find(value, position)
    assert found >= 0, (value, out, err)
    position=found+len(value)
assert "TYPE_MISMATCH" in err
assert "AMBIGUOUS_NUMERIC_LITERAL" in err
assert "DIVIDE_BY_ZERO" in err
PY

set +e
"$QUIDRA" repl < /dev/null > "$TMP/repl-eof.out" 2> "$TMP/repl-eof.err"
repl_eof_rc=$?
set -e
[[ "$repl_eof_rc" -eq 0 ]]

# A blank line with nothing pending must not resubmit the accumulated session.
# It used to append an empty line to the accepted source and re-run the whole
# program, repeating every side effect already produced. Duplicate output is
# hidden by the REPL's prefix stripping when the program is deterministic, so
# the execution count is observed through a side effect outside stdout.
mkdir -p "$TMP/repl-blank"
cat > "$TMP/repl-blank/input.txt" <<'QUI'
void emit(string path)
    auto prev = file.read(path)
    match prev
        string
            auto saved = file.write(path, prev + "X")
        error
            auto saved = file.write(path, "X")
    print("MARK")

emit("marks.txt")

QUI
(
    cd "$TMP/repl-blank"
    "$QUIDRA" repl < input.txt > output.txt 2> error.txt
)
[[ "$(cat "$TMP/repl-blank/marks.txt")" == "X" ]]
grep -q MARK "$TMP/repl-blank/output.txt"

mkdir -p "$TMP/repl-replay-barrier"
cat > "$TMP/repl-replay-barrier/input.txt" <<'QUI'
auto saved = file.write("effect.txt", "A")
int plus_one(int value)
    return value + 1

:type plus_one(1)
int(1) + 2
:reset
int(1) + 2
:exit
QUI
(
    cd "$TMP/repl-replay-barrier"
    "$QUIDRA" repl < input.txt > output.txt 2> error.txt
)
[[ "$(cat "$TMP/repl-replay-barrier/effect.txt")" == "A" ]]
grep -q 'REPL_REPLAY_UNSAFE' "$TMP/repl-replay-barrier/error.txt"
grep -q 'int' "$TMP/repl-replay-barrier/output.txt"
grep -q 'REPL state reset.' "$TMP/repl-replay-barrier/output.txt"
grep -q '3' "$TMP/repl-replay-barrier/output.txt"

mkdir -p "$TMP/repl-failed-effect"
cat > "$TMP/repl-failed-effect/input.txt" <<'QUI'
void effect_then_fail(string path)
    auto previous = file.read(path)
    match previous
        string text
            auto saved = file.write(path, text + "X")
        error
            auto saved = file.write(path, "X")
    int zero = 0
    print(1 / zero)

effect_then_fail("effect.txt")
effect_then_fail("effect.txt")
:reset
int(1) + 2
:exit
QUI
(
    cd "$TMP/repl-failed-effect"
    "$QUIDRA" repl < input.txt > output.txt 2> error.txt
)
# The first call performs the write before its runtime failure. The second call
# must be blocked even though the failing candidate was never accepted.
[[ "$(cat "$TMP/repl-failed-effect/effect.txt")" == "X" ]]
grep -q 'DIVISION_BY_ZERO' "$TMP/repl-failed-effect/output.txt"
grep -q 'REPL_REPLAY_UNSAFE' "$TMP/repl-failed-effect/error.txt"
grep -q 'REPL state reset.' "$TMP/repl-failed-effect/output.txt"
grep -q '3' "$TMP/repl-failed-effect/output.txt"

python3 - "$QUIDRA" <<'PY'
import errno, os, pty, sys
binary=sys.argv[1]
pid, fd=pty.fork()
if pid == 0:
    os.execl(binary, binary)
os.write(fd, b"int(1) + 2\n:exit\n")
chunks=[]
while True:
    try:
        data=os.read(fd, 4096)
    except OSError as e:
        if e.errno == errno.EIO:
            break
        raise
    if not data:
        break
    chunks.append(data)
_, status=os.waitpid(pid, 0)
text=b"".join(chunks).decode(errors="replace").replace("\r", "")
assert os.waitstatus_to_exitcode(status) == 0, text
assert "Quidra " in text
assert ">>> " in text
assert "3" in text
PY

python3 - "$QUIDRA" <<'PY'
import errno, os, pty, signal, sys, time
binary=sys.argv[1]
pid, fd=pty.fork()
if pid == 0:
    os.execl(binary, binary)
os.write(fd, b"int interrupted(int x)\n")
time.sleep(0.2)
os.kill(pid, signal.SIGINT)
time.sleep(0.2)
os.write(fd, b"int(1) + 2\n:exit\n")
chunks=[]
while True:
    try:
        data=os.read(fd, 4096)
    except OSError as e:
        if e.errno == errno.EIO:
            break
        raise
    if not data:
        break
    chunks.append(data)
_, status=os.waitpid(pid, 0)
text=b"".join(chunks).decode(errors="replace").replace("\r", "")
assert os.waitstatus_to_exitcode(status) == 0, text
assert "3" in text, text
assert "error[" not in text, text
PY


cat > "$TMP/reference-aliasing.qui" <<'QUI'
void touch(int[] &values, int[] &same)
    values[0] = 7
    same[1] = values[0] + 2

void mixed(const int[] &view, int[] &sink)
    sink[0] = view[0] + 1

int[] data = [1, 2, 3]
touch(&data, &data)
print(data[0])
print(data[1])
mixed(&data, &data)
print(data[0])
QUI
reference_aliasing_output=$("$QUIDRA" run "$TMP/reference-aliasing.qui")
reference_aliasing_expected=$(printf '7\n9\n8')
[[ "$reference_aliasing_output" == "$reference_aliasing_expected" ]]

cat > "$TMP/reference-alias-call-entry.qui" <<'QUI'
void read_before_initialize(int &later, int &first)
    print(first)
    later = 1

int pending
read_before_initialize(&pending, &pending)
QUI
set +e
"$QUIDRA" check "$TMP/reference-alias-call-entry.qui" --json > "$TMP/reference-alias-call-entry.json"
reference_alias_call_entry_rc=$?
set -e
[[ "$reference_alias_call_entry_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/reference-alias-call-entry.json"

cat > "$TMP/reference-alias-initialize.qui" <<'QUI'
void initialize_both(int &left, int &right)
    left = 1
    right = 2

int pending
initialize_both(&pending, &pending)
print(pending)
QUI
[[ "$("$QUIDRA" run "$TMP/reference-alias-initialize.qui")" == "2" ]]

cat > "$TMP/receiver-alias-call-entry.qui" <<'QUI'
class Cell
    int value

    void read_before_initialize(int &later)
        print(value)
        later = 1

Cell cell = Cell()
cell.read_before_initialize(&cell.value)
QUI
set +e
"$QUIDRA" check "$TMP/receiver-alias-call-entry.qui" --json > "$TMP/receiver-alias-call-entry.json"
receiver_alias_call_entry_rc=$?
set -e
[[ "$receiver_alias_call_entry_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/receiver-alias-call-entry.json"

cat > "$TMP/receiver-alias-postcondition.qui" <<'QUI'
class State
    int value

    void initialize_then_replace(State &other)
        value = 1
        other = State()

State state = State(value = 0)
state.initialize_then_replace(&state)
print(state.value)
QUI
set +e
"$QUIDRA" check "$TMP/receiver-alias-postcondition.qui" --json > "$TMP/receiver-alias-postcondition.json"
receiver_alias_postcondition_rc=$?
set -e
[[ "$receiver_alias_postcondition_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/receiver-alias-postcondition.json"

cat > "$TMP/const-references.qui" <<'QUI'
void show(const int &value)
    print(value)

int value = 5
const int &view = &value
show(&value)
value = 7
print(view)

int &writer = &value
const int &read = &writer
writer = 9
print(read)

const int frozen = 11
const int &frozen_view = &frozen
print(frozen_view)
QUI
const_reference_output=$("$QUIDRA" run "$TMP/const-references.qui")
const_reference_expected=$(printf '5\n7\n9\n11')
[[ "$const_reference_output" == "$const_reference_expected" ]]

cat > "$TMP/const-auto.qui" <<'QUI'
int source = 5
const auto value = source
const auto &view = &source
source = 8
print(value)
print(view)
QUI
const_auto_output=$("$QUIDRA" run "$TMP/const-auto.qui")
const_auto_expected=$(printf '5\n8')
[[ "$const_auto_output" == "$const_auto_expected" ]]

cat > "$TMP/const-value-override.qui" <<'QUI'
class Base
    int echo(int value)
        return value

class Child : Base
    override int echo(const int value)
        return value

Child child = Child()
print(child.echo(7))
QUI
[[ "$("$QUIDRA" run "$TMP/const-value-override.qui")" == "7" ]]

cat > "$TMP/const-reference-override-mismatch.qui" <<'QUI'
class Base
    void touch(int &value)
        value = 1

class Child : Base
    override void touch(const int &value)
        print(value)
QUI
set +e
"$QUIDRA" check "$TMP/const-reference-override-mismatch.qui" --json > "$TMP/const-reference-override-mismatch.json"
const_reference_override_rc=$?
set -e
[[ "$const_reference_override_rc" -eq 1 ]]
grep -q 'OVERRIDE_MISMATCH' "$TMP/const-reference-override-mismatch.json"

cat > "$TMP/const-write-errors.qui" <<'QUI'
void bad(const int &value)
    value = 2

int source = 1
const int &read = &source
int &write = &read
QUI
set +e
"$QUIDRA" check "$TMP/const-write-errors.qui" --json > "$TMP/const-write-errors.json"
const_write_rc=$?
set -e
[[ "$const_write_rc" -eq 1 ]]
python3 - "$TMP/const-write-errors.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert any(d["code"] == "WRITE_CAPABILITY" for d in x["diagnostics"])
PY

cat > "$TMP/const-values.qui" <<'QUI'
const int answer = 42
print(answer)

class Item
    const int id
    int value

Item item = Item(id = 3, value = 4)
print(item.id)
QUI
const_values_output=$("$QUIDRA" run "$TMP/const-values.qui")
const_values_expected=$(printf '42\n3')
[[ "$const_values_output" == "$const_values_expected" ]]

cat > "$TMP/const-value-write.qui" <<'QUI'
const int answer = 42
answer = 43
QUI
set +e
"$QUIDRA" check "$TMP/const-value-write.qui" --json > "$TMP/const-value-write.json"
const_value_rc=$?
set -e
[[ "$const_value_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/const-value-write.json"

cat > "$TMP/const-field-write.qui" <<'QUI'
class Item
    const int id

Item item = Item(id = 1)
item.id = 2
QUI
set +e
"$QUIDRA" check "$TMP/const-field-write.qui" --json > "$TMP/const-field-write.json"
const_field_rc=$?
set -e
[[ "$const_field_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/const-field-write.json"

cat > "$TMP/const-uninitialized.qui" <<'QUI'
const int missing
int pending
const int &view = &pending
QUI
set +e
"$QUIDRA" check "$TMP/const-uninitialized.qui" --json > "$TMP/const-uninitialized.json"
const_uninitialized_rc=$?
set -e
[[ "$const_uninitialized_rc" -eq 1 ]]
python3 - "$TMP/const-uninitialized.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
codes={d["code"] for d in x["diagnostics"]}
assert "CONST_INITIALIZATION" in codes
assert "UNINITIALIZED" in codes
PY

cat > "$TMP/const-reference-rebind.qui" <<'QUI'
int first = 1
int second = 2
const int &view = &first
&view = &second
QUI
set +e
"$QUIDRA" check "$TMP/const-reference-rebind.qui" --json > "$TMP/const-reference-rebind.json"
const_rebind_rc=$?
set -e
[[ "$const_rebind_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/const-reference-rebind.json"

cat > "$TMP/const-value-parameter.qui" <<'QUI'
void change(const int value)
    value = 2
change(1)
QUI
set +e
"$QUIDRA" check "$TMP/const-value-parameter.qui" --json > "$TMP/const-value-parameter.json"
const_value_parameter_rc=$?
set -e
[[ "$const_value_parameter_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/const-value-parameter.json"

cat > "$TMP/const-method-read.qui" <<'QUI'
class Counter
    int value

    int get()
        return value

    void increment()
        value += 1

const Counter counter = Counter(value = 4)
print(counter.get())
QUI
[[ "$("$QUIDRA" run "$TMP/const-method-read.qui")" == "4" ]]

cat > "$TMP/const-method-write.qui" <<'QUI'
class Counter
    int value

    void increment()
        value += 1

const Counter counter = Counter(value = 4)
counter.increment()
QUI
set +e
"$QUIDRA" check "$TMP/const-method-write.qui" --json > "$TMP/const-method-write.json"
const_method_write_rc=$?
set -e
[[ "$const_method_write_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/const-method-write.json"

cat > "$TMP/const-field-missing.qui" <<'QUI'
class Item
    const int id
    int value

Item item = Item(value = 4)
QUI
set +e
"$QUIDRA" check "$TMP/const-field-missing.qui" --json > "$TMP/const-field-missing.json"
const_field_missing_rc=$?
set -e
[[ "$const_field_missing_rc" -eq 1 ]]
grep -q 'CONST_INITIALIZATION' "$TMP/const-field-missing.json"


mkdir -p "$TMP/repl-project"
cat > "$TMP/repl-project/math.qui" <<'QUI'
int triple(int value)
    return value * 3
QUI
cat > "$TMP/repl-project/input.txt" <<'QUI'
import local_math = "./math.qui"
local_math.triple(7)
:exit
QUI
(
    cd "$TMP/repl-project"
    "$QUIDRA" repl < input.txt > output.txt 2> error.txt
)
python3 - "$TMP/repl-project/output.txt" <<'PY'
import sys
out=open(sys.argv[1]).read()
assert "21" in out, out
PY
[[ ! -s "$TMP/repl-project/error.txt" ]]


"$QUIDRA" describe > "$TMP/describe-final.json"
python3 - "$TMP/describe-final.json" "$ROOT/quidra.manifest.json" <<'PY'
import json,sys
describe=json.load(open(sys.argv[1]))
manifest=json.load(open(sys.argv[2]))
assert describe == manifest
assert describe["call_style"] == manifest["call_style"]
assert set(describe["current_builtins"]) == set(manifest["current_builtins"])
assert describe["repl"] is True
PY


"$QUIDRA" describe llm > "$TMP/describe-llm.json"
python3 - "$TMP/describe-llm.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
tensor=x["inspection"]["type_contracts"]["tensor"]
rank=x["inspection"]["type_contracts"]["tensor_rank"]
assert tensor == "tensor<T> | tensor<T><D0, D1, ...>"
assert "exact-rank" in rank
assert "compiler-inferred" in rank
shape=x["inspection"]["type_contracts"]["tensor_shape"]
assert "integer expression" in shape
assert "captured" in shape
calls=x["calls"]
assert calls["argument_order"] == "positional_then_named"
assert calls["named_syntax"] == "name = value"
assert "duplicate_parameter" in calls["rejected"]
PY

cat > "$TMP/inspect-tensor-shape.qui" <<'QUI'
tensor<float32><2, 3> matrix = tensor.zeros<float32>([2, 3])
auto row = matrix[0]
auto dimensions = matrix.shape()
QUI
"$QUIDRA" inspect "$TMP/inspect-tensor-shape.qui" --no-source --no-effects > "$TMP/inspect-tensor-shape.json"
python3 - "$TMP/inspect-tensor-shape.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
types={n["inferred_type"] for n in x["nodes"] if n["inferred_type"]}
assert "tensor<float32><2, 3>" in types, types
assert "tensor<float32><3>" in types, types
assert "int[2]" in types, types
PY

cat > "$TMP/tensor-rank-inference.qui" <<'QUI'
tensor<float32> matrix = tensor.zeros<float32>([2, 3])
auto shape = matrix.shape()
print(shape[0])
print(shape[1])
QUI
"$QUIDRA" run "$TMP/tensor-rank-inference.qui" > "$TMP/tensor-rank-inference.out"
python3 - "$TMP/tensor-rank-inference.out" <<'PY'
import sys
text=open(sys.argv[1]).read()
assert text.splitlines() == ["2","3"], repr(text)
PY

"$QUIDRA" inspect "$ROOT/examples/classes.qui" --no-source --no-effects --kind integer > "$TMP/inspect-compact.json"
python3 - "$TMP/inspect-compact.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["nodes"]
assert all(n["kind"] == "integer" for n in x["nodes"])
assert all("source" not in n for n in x["nodes"])
assert x["effects"] == []
PY
"$QUIDRA" inspect "$ROOT/examples/classes.qui" > "$TMP/inspect-full.json"
python3 - "$TMP/inspect-full.json" "$TMP/inspect-compact.json" <<'PY'
import os,sys
assert os.path.getsize(sys.argv[2]) < os.path.getsize(sys.argv[1])
PY

"$QUIDRA" inspect "$ROOT/examples/classes.qui" --no-source --no-effects --depth 1 > "$TMP/inspect-depth.json"
python3 - "$TMP/inspect-depth.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["nodes"]
assert all(n["depth"] <= 1 for n in x["nodes"])
ids={n["node_id"] for n in x["nodes"]}
assert all(n["parent_id"] is None or n["parent_id"] in ids for n in x["nodes"])
assert any(n["parent_id"] is not None for n in x["nodes"])
PY


cat > "$TMP/uint64-literals.qui" <<'QUI'
uint64 high = 10000000000000000000
uint64 maximum = 18446744073709551615
print(high)
print(maximum)
QUI
[[ "$("$QUIDRA" run "$TMP/uint64-literals.qui")" == $'10000000000000000000\n18446744073709551615' ]]

cat > "$TMP/uint64-too-large.qui" <<'QUI'
uint64 value = 18446744073709551616
QUI
set +e
"$QUIDRA" check "$TMP/uint64-too-large.qui" --json > "$TMP/uint64-too-large.json"
uint64_large_rc=$?
set -e
[[ "$uint64_large_rc" -eq 1 ]]
python3 - "$TMP/uint64-too-large.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert any(d["code"] == "INTEGER_RANGE" for d in x["diagnostics"])
PY

cat > "$TMP/default-int-too-large.qui" <<'QUI'
auto value = 10000000000000000000
QUI
set +e
"$QUIDRA" check "$TMP/default-int-too-large.qui" --json > "$TMP/default-int-too-large.json"
default_large_rc=$?
set -e
[[ "$default_large_rc" -eq 1 ]]
python3 - "$TMP/default-int-too-large.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert any(d["code"] == "AMBIGUOUS_NUMERIC_LITERAL" for d in x["diagnostics"])
PY

cat > "$TMP/deep-recursion.qui" <<'QUI'
int deep(int n)
    if n == 0
        return 0
    return 1 + deep(n - 1)

print(deep(10000000))
QUI
set +e
"$QUIDRA" run "$TMP/deep-recursion.qui" > "$TMP/deep-recursion.out" 2>&1
deep_rc=$?
set -e
[[ "$deep_rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[CALL_DEPTH_LIMIT\] at [0-9]+:[0-9]+: call depth limit' "$TMP/deep-recursion.out"

cat > "$TMP/standalone-none.qui" <<'QUI'
none
print("ok")
QUI
[[ "$("$QUIDRA" run "$TMP/standalone-none.qui")" == "ok" ]]


cat > "$TMP/float-canonical-text.qui" <<'QUI'
print(float(0.6))
print(float(1.0 / 3.0))
print(float(1.0))
print(float(-0.0))
print(float(1.0e20))
QUI
[[ "$("$QUIDRA" run "$TMP/float-canonical-text.qui")" == $'0.6\n0.3333333333333333\n1.0\n-0.0\n1.0e+20' ]]

cat > "$TMP/float-exception-text.qui" <<'QUI'
print(float(0.0 / 0.0))
print(float(1.0 / 0.0))
print(float(-1.0 / 0.0))
QUI
[[ "$("$QUIDRA" run "$TMP/float-exception-text.qui")" == $'nan\ninf\n-inf' ]]
 "$TMP/debug-source.ll"
grep -q '!DICompileUnit' "$TMP/debug-source.ll"
grep -q '!DIFile(filename: "debug-source.qui"' "$TMP/debug-source.ll"
grep -q '!DISubprogram(name: "twice"' "$TMP/debug-source.ll"
grep -q 'line: 1, type: !5' "$TMP/debug-source.ll"
grep -Eq 'define i64 @n_twice\(.*\) !dbg ![0-9]+ \{' "$TMP/debug-source.ll"
"$QUIDRA" llvm "$TMP/debug-source.qui" > "$TMP/debug-release.ll"
! grep -q '!DICompileUnit' "$TMP/debug-release.ll"

[[ "$($QUIDRA "$ROOT/examples/hello.qui")" == "Hello from Quidra" ]]
[[ "$($QUIDRA run "$ROOT/examples/functions.qui")" == "120" ]]
[[ "$($QUIDRA run "$ROOT/examples/logic.qui")" == "positive even integer" ]]
[[ "$($QUIDRA run "$ROOT/examples/named_arguments.qui")" == "true" ]]
[[ "$($QUIDRA run "$ROOT/examples/arrays.qui")" == $'2\n4\n6' ]]
[[ "$($QUIDRA run "$ROOT/examples/results.qui")" == "42" ]]
[[ "$($QUIDRA run "$ROOT/examples/options.qui")" == "none" ]]
[[ "$($QUIDRA run "$ROOT/examples/classes.qui")" == $'1\n5\n9\n12\n7\n17\n1\n99\n2' ]]
[[ "$($QUIDRA run "$ROOT/examples/value_objects.qui")" == $'true\ntrue\n4' ]]

binary_dependencies() {
    if command -v ldd >/dev/null 2>&1; then
        ldd "$1"
    elif command -v otool >/dev/null 2>&1; then
        otool -L "$1"
    else
        echo "no supported dependency inspection tool" >&2
        return 2
    fi
}

"$QUIDRA" build "$ROOT/examples/hello.qui" -o "$TMP/hello-link"
if binary_dependencies "$TMP/hello-link" | grep -qi 'libcurl'; then
    echo "hello executable unexpectedly links libcurl" >&2
    exit 1
fi

cat > "$TMP/http-link.qui" <<'QUI'
auto result = http.get("https://example.com")
QUI
"$QUIDRA" build "$TMP/http-link.qui" -o "$TMP/http-link"
binary_dependencies "$TMP/http-link" | grep -qi 'libcurl'

mkdir -p "$TMP/file-tree/a/b"
printf 'one' > "$TMP/file-tree/root.txt"
printf 'two' > "$TMP/file-tree/a/child.txt"
printf 'three' > "$TMP/file-tree/a/b/deep.txt"
cat > "$TMP/file-tree.qui" <<QUI
string root = "$TMP/file-tree"
match file.is_directory(root)
    bool value
        print(value)
    error problem
        print(problem)
match file.is_directory(root + "/root.txt")
    bool value
        print(value)
    error problem
        print(problem)
match file.list(root)
    string[] paths
        print(len(paths))
    error problem
        print(problem)
match file.list(root, recursive = true)
    string[] paths
        print(len(paths))
    error problem
        print(problem)
QUI
file_tree_output="$("$QUIDRA" run "$TMP/file-tree.qui")"
file_tree_expected=$(printf 'true\nfalse\n2\n5')
[[ "$file_tree_output" == "$file_tree_expected" ]]

cat > "$TMP/string-controls.qui" <<'QUI'
print("literal:\n\t\r\b\f\v\a\u3042")
print("A{tab}B")
print("A{enter}B")
print("A{home}B")
print("{quote}A{quote}")
print("x{backspace}y")
print("x{page}y")
print("x{vtab}y")
print("x{bell}y")
print("nested {error("ok")}")
QUI
$QUIDRA run "$TMP/string-controls.qui" > "$TMP/string-controls.out"
python3 - "$TMP/string-controls.out" <<'PY'
import sys
actual = open(sys.argv[1], "rb").read()
expected = (
    b"literal:\\n\\t\\r\\b\\f\\v\\a\\u3042\n"
    b"A\tB\n"
    b"A\nB\n"
    b"A\rB\n"
    b"\"A\"\n"
    b"x\x08y\n"
    b"x\x0cy\n"
    b"x\x0by\n"
    b"x\x07y\n"
    b"nested ok\n"
)
assert actual == expected, (actual, expected)
PY

cat > "$TMP/text-and-array.qui" <<'QUI'
string text = "  A日本B  "
string merged = "A" + "B" + "C" + "D"
print(merged)
print(len(text))
print(text.trim())
print(text.contains("日本"))
print(text.starts_with("  A"))
print(text.ends_with("  "))
auto found = text.find("日本")
match found
    int index
        print(index)
    none
        print(int(-1))
print(text.slice(2, 6))
print(text[3])
print("A日本"[2])
string[] parts = "a,b,,c".split(",")
print(len(parts))
print(parts[0])
print(parts[2] == "")
print(parts.join("|"))
bin encoded = "A日本".utf8()
print(len(encoded))
print(encoded[0])
int[] points = "A日本".codepoints()
print(len(points))
print(points[1])
int[] unordered = [3, -1, 2]
int[] ordered = unordered.sorted()
print(ordered[0])
print(ordered[2])
string[] ordered_text = ["b", "あ", "a"].sorted()
print(ordered_text[0])
print(ordered_text[2])
int[] original = [1, 2]
int &first = &original[0]
int[] grown = original.append(3)
first = 9
print(original[0])
print(grown[0])
print(grown[2])
int[] joined = grown.concat([4, 5])
print(len(joined))
print(joined[4])
int choice = 2
if choice == 1
    print("one")
elif choice == 2
    print("two")
else
    print("other")
QUI
text_array_output=$("$QUIDRA" run "$TMP/text-and-array.qui")
text_array_expected=$(printf 'ABCD\n8\nA日本B\ntrue\ntrue\ntrue\n3\nA日本B\n日\n本\n4\na\ntrue\na|b||c\n56\n0\n3\n26085\n-1\n3\na\nあ\n9\n1\n3\n5\n5\ntwo')
[[ "$text_array_output" == "$text_array_expected" ]]

cat > "$TMP/unicode-boundaries.qui" <<'QUI'
string text = "A😀é"
print(len(text))
print(text[1] == "😀")
print(text.slice(1, 4) == "😀é")
bin encoded = text.utf8()
print(len(encoded))
int[] points = text.codepoints()
print(points[1])
print(points[3])
print(len(""))
print("".slice(0, 0) == "")
QUI
unicode_boundary_output=$("$QUIDRA" run "$TMP/unicode-boundaries.qui")
unicode_boundary_expected=$(printf '4\ntrue\ntrue\n64\n128512\n769\n0\ntrue')
[[ "$unicode_boundary_output" == "$unicode_boundary_expected" ]]

cat > "$TMP/string-negative-index.qui" <<'QUI'
string text = "😀"
print(text[-1])
QUI
set +e
"$QUIDRA" run "$TMP/string-negative-index.qui" > "$TMP/string-negative-index.out" 2> "$TMP/string-negative-index.err"
string_negative_index_rc=$?
set -e
[[ "$string_negative_index_rc" -eq 101 ]]
grep -q 'error\[INDEX_BOUNDS\]' "$TMP/string-negative-index.err"

"$QUIDRA" llvm "$TMP/text-and-array.qui" > "$TMP/text-and-array.ll"
grep -q 'call ptr @quidra_string_concat_many' "$TMP/text-and-array.ll"

cat > "$TMP/float-sorted-order.qui" <<'QUI'
float[] values = [0.0, -0.0, 2.0, 0.0 / 0.0, -1.0]
float[] ordered = values.sorted()
for value in ordered
    print(value)
QUI
float_sorted_output=$("$QUIDRA" run "$TMP/float-sorted-order.qui")
float_sorted_expected=$(printf '%s\n' '-1.0' '-0.0' '0.0' '2.0' 'nan')
[[ "$float_sorted_output" == "$float_sorted_expected" ]]

cat > "$TMP/sorted-boundaries.qui" <<'QUI'
float[] special = [
    1.0 / 0.0,
    -1.0 / 0.0,
    0.0 / 0.0,
    0.0,
    -0.0,
    1.0 / 0.0,
    -1.0 / 0.0,
]
float[] special_ordered = special.sorted()
for value in special_ordered
    print(value)

int[6] fixed = [5, 1, 4, 1, 3, 2]
int[] fixed_ordered = fixed.sorted()
fixed[0] = 99
print(fixed_ordered[0])
print(fixed_ordered[5])

int[] large = []
for i in range(0, 5000)
    large = large.append(4999 - i)
int[] large_ordered = large.sorted()
print(large_ordered[0])
print(large_ordered[4999])
QUI
sorted_boundaries_output=$("$QUIDRA" run "$TMP/sorted-boundaries.qui")
sorted_boundaries_expected=$(printf '%s\n' '-inf' '-inf' '-0.0' '0.0' 'inf' 'inf' 'nan' '1' '5' '0' '4999')
[[ "$sorted_boundaries_output" == "$sorted_boundaries_expected" ]]

cat > "$TMP/string-index-oob.qui" <<'QUI'
string text = "日本"
print(text[2])
QUI
set +e
"$QUIDRA" run "$TMP/string-index-oob.qui" > "$TMP/string-index-oob.out" 2> "$TMP/string-index-oob.err"
string_index_oob_rc=$?
set -e
[[ "$string_index_oob_rc" -eq 101 ]]
grep -q 'error\[INDEX_BOUNDS\]' "$TMP/string-index-oob.err"

cat > "$TMP/string-growth.qui" <<'QUI'
string built = ""
for i in range(0, 2000)
    built = built + "x"
print(len(built))

string assigned = ""
for i in range(0, 2000)
    assigned += "y"
print(len(assigned))

string shared = "ab"
string snapshot = shared
shared = shared + "c"
print(snapshot)
print(shared)

string self = "xy"
self = self + self
print(self)
QUI
string_growth_output=$("$QUIDRA" run "$TMP/string-growth.qui")
string_growth_expected=$(printf '2000\n2000\nab\nabc\nxyxy')
[[ "$string_growth_output" == "$string_growth_expected" ]]
"$QUIDRA" llvm "$TMP/string-growth.qui" > "$TMP/string-growth.ll"
grep -q 'call i1 @quidra_string_can_append_move' "$TMP/string-growth.ll"
grep -q 'call ptr @quidra_string_append_move_many' "$TMP/string-growth.ll"

cat > "$TMP/fixed-array-layout.qui" <<'QUI'
int[2][3] matrix = [
    [1, 2, 3],
    [4, 5, 6],
]
print(matrix[1][2])
matrix[0][1] = 9
print(matrix[0][1])

int[2][2][] groups = [
    [[1], [2, 3]],
    [[4, 5, 6], []],
]
print(len(groups))
print(len(groups[0]))
print(len(groups[0][1]))
print(groups[1][0][2])
QUI
fixed_array_output=$("$QUIDRA" run "$TMP/fixed-array-layout.qui")
fixed_array_expected=$(printf '6\n9\n2\n2\n2\n6')
[[ "$fixed_array_output" == "$fixed_array_expected" ]]
"$QUIDRA" llvm "$TMP/fixed-array-layout.qui" > "$TMP/fixed-array-layout.ll"
grep -q 'call ptr @quidra_alloc(i64 48)' "$TMP/fixed-array-layout.ll"
grep -q 'call ptr @quidra_fixed_array_slot' "$TMP/fixed-array-layout.ll"

cat > "$TMP/fixed-array-reference.qui" <<'QUI'
int[2][4] matrix = [
    [1, 2, 3, 4],
    [5, 6, 7, 8],
]
int[4] &row = &matrix[0]
row[1] = 11
print(matrix[0][1])
int[4] replacement = [9, 10, 11, 12]
row = replacement
print(matrix[0][0])
print(matrix[0][3])
QUI
fixed_reference_output=$("$QUIDRA" run "$TMP/fixed-array-reference.qui")
fixed_reference_expected=$(printf '11\n9\n12')
[[ "$fixed_reference_output" == "$fixed_reference_expected" ]]

cat > "$TMP/fixed-to-dynamic.qui" <<'QUI'
int[2][3] fixed = [
    [1, 2, 3],
    [4, 5, 6],
]
int[][] dynamic = fixed
fixed[0][0] = 9
print(dynamic[0][0])
print(dynamic[1][2])

int[3] row = [7, 8, 9]
int[] dynamic_row = row
row[1] = 99
print(dynamic_row[1])
QUI
fixed_to_dynamic_output=$("$QUIDRA" run "$TMP/fixed-to-dynamic.qui")
fixed_to_dynamic_expected=$(printf '1\n6\n8')
[[ "$fixed_to_dynamic_output" == "$fixed_to_dynamic_expected" ]]

cat > "$TMP/value-memory.qui" <<'QUI'
float consume(float[] values)
    return values[0] + values[63]

float[] values = array(64, fill = 1.0)
float total = 0.0
for i in range(0, 100000)
    total = total + consume(values)
print(total)
QUI
"$QUIDRA" build "$TMP/value-memory.qui" -o "$TMP/value-memory"
measure_rss "$TMP/value-memory.rss" "$TMP/value-memory.out" "$TMP/value-memory"
grep -q '^200000\.0$' "$TMP/value-memory.out"
value_memory_rss="$(cat "$TMP/value-memory.rss")"
if [[ -n "$TIME_BIN" && -z "${ASAN_OPTIONS:-}" && "$value_memory_rss" -ge 80000 ]]; then
    echo "value-semantics memory regression: peak RSS ${value_memory_rss} KiB" >&2
    exit 1
fi

echo "value-semantics memory peak RSS: ${value_memory_rss} KiB"

cat > "$TMP/read-only-borrow.qui" <<'QUI'
float read_first(float[] values)
    return values[0]

void mutate_copy(float[] values)
    values[0] = 99.0

float[] values = [1.0, 2.0]
print(read_first(values))
mutate_copy(values)
print(values[0])
print(read_first([3.0, 4.0]))
QUI
read_only_borrow_output="$("$QUIDRA" run "$TMP/read-only-borrow.qui")"
read_only_borrow_expected=$(printf '1.0\n1.0\n3.0')
[[ "$read_only_borrow_output" == "$read_only_borrow_expected" ]]
"$QUIDRA" ir "$TMP/read-only-borrow.qui" > "$TMP/read-only-borrow.ir"
python3 - "$TMP/read-only-borrow.ir" <<'PY'
import sys
text=open(sys.argv[1]).read()
entry=text.split("function $entry(",1)[1].split("\nend\n",1)[0]
first=entry.index("call read_first")
mutate=entry.index("call mutate_copy")
second=entry.index("call read_first", first + 1)
assert "clone" not in entry[:first]
assert "clone" in entry[first:mutate]
assert "release" in entry[second:second + 220]
PY

cat > "$TMP/temporary-memory.qui" <<'QUI'
float[] make_values()
    return array(64, fill = 1.0)

string prefix = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
int total = 0
for i in range(0, 100000)
    make_values()
    total += len(prefix + i.string())
print(total > 0)

int[] original = [1, 2, 3]
int &kept = &original[0]
original = [4, 5, 6]
print(kept)
&kept = &original[0]
print(kept)
QUI
"$QUIDRA" build "$TMP/temporary-memory.qui" -o "$TMP/temporary-memory"
measure_rss "$TMP/temporary-memory.rss" "$TMP/temporary-memory.out" "$TMP/temporary-memory"
temporary_memory_expected=$(printf 'true\n1\n4')
[[ "$(cat "$TMP/temporary-memory.out")" == "$temporary_memory_expected" ]]
temporary_memory_rss="$(cat "$TMP/temporary-memory.rss")"
echo "temporary-value memory peak RSS: ${temporary_memory_rss} KiB"
if [[ -n "$TIME_BIN" && -z "${ASAN_OPTIONS:-}" && "$temporary_memory_rss" -ge 80000 ]]; then
    echo "temporary-value memory regression: peak RSS ${temporary_memory_rss} KiB" >&2
    exit 1
fi

cat > "$TMP/ownership-transfer.qui" <<'QUI'
string make_text()
    return "alpha" + "beta"

string source = make_text()
string copied = source.string()
source = "changed"
print(copied)

string message = make_text()
error problem = error(message)
message = "changed"
print(problem)

int[] | error make_numbers()
    int[] values = array(64, fill = 1)
    return values

int | error consume_numbers()
    int total = 0
    for value in try make_numbers()
        total += value
    return total

for i in range(0, 200000)
    consume_numbers()
print("ownership-ok")
QUI
"$QUIDRA" build "$TMP/ownership-transfer.qui" -o "$TMP/ownership-transfer"
measure_rss "$TMP/ownership-transfer.rss" "$TMP/ownership-transfer.out" "$TMP/ownership-transfer"
ownership_expected=$(printf 'alphabeta\nalphabeta\nownership-ok')
[[ "$(cat "$TMP/ownership-transfer.out")" == "$ownership_expected" ]]
ownership_rss="$(cat "$TMP/ownership-transfer.rss")"
echo "ownership-transfer memory peak RSS: ${ownership_rss} KiB"
if [[ -n "$TIME_BIN" && -z "${ASAN_OPTIONS:-}" && "$ownership_rss" -ge 80000 ]]; then
    echo "ownership-transfer memory regression: peak RSS ${ownership_rss} KiB" >&2
    exit 1
fi

cat > "$TMP/union-match-borrow.qui" <<'QUI'
class LinearKernel
    float bias = 0.0

    float apply(float value)
        return value + bias

class RbfKernel
    float gamma = 1.0

    float apply(float value)
        return value * gamma

float evaluate(LinearKernel | RbfKernel kernel, float value)
    match kernel
        LinearKernel linear_kernel
            return linear_kernel.apply(value)
        RbfKernel rbf
            return rbf.apply(value)

void mutate(LinearKernel | RbfKernel kernel)
    match kernel
        LinearKernel linear_kernel
            linear_kernel.bias = 7.0
        RbfKernel rbf
            rbf.gamma = 9.0

LinearKernel | RbfKernel kernel = RbfKernel(gamma = 2.0)
print(evaluate(kernel, 3.0))
mutate(kernel)
print(evaluate(kernel, 3.0))
QUI
union_match_borrow_output="$("$QUIDRA" run "$TMP/union-match-borrow.qui")"
union_match_borrow_expected=$(printf '6.0\n6.0')
[[ "$union_match_borrow_output" == "$union_match_borrow_expected" ]]
"$QUIDRA" ir "$TMP/union-match-borrow.qui" > "$TMP/union-match-borrow.ir"
python3 - "$TMP/union-match-borrow.ir" <<'PY'
import sys
text=open(sys.argv[1]).read()
evaluate=text.split("function evaluate(",1)[1].split("\nend\n",1)[0]
mutate=text.split("function mutate(",1)[1].split("\nend\n",1)[0]
assert "store.borrow" in evaluate
assert "clone" not in evaluate
assert "clone" in mutate
assert "call $method.RbfKernel.apply(" in evaluate
PY

cat > "$TMP/union-conversion-memory.qui" <<'QUI'
int | none maybe_number(bool present)
    if present
        return 7
    return none

int consume(bool present)
    int | string | none value = maybe_number(present)
    match value
        int number
            return number
        string text
            return len(text)
        none
            return 0

int total = 0
for i in range(0, 200000)
    total += consume(i % 2 == 0)
print(total)
QUI
"$QUIDRA" build "$TMP/union-conversion-memory.qui" -o "$TMP/union-conversion-memory"
measure_rss "$TMP/union-conversion-memory.rss" "$TMP/union-conversion-memory.out" "$TMP/union-conversion-memory"
grep -q '^700000$' "$TMP/union-conversion-memory.out"
union_conversion_rss="$(cat "$TMP/union-conversion-memory.rss")"
echo "union-conversion memory peak RSS: ${union_conversion_rss} KiB"
if [[ -n "$TIME_BIN" && -z "${ASAN_OPTIONS:-}" && "$union_conversion_rss" -ge 80000 ]]; then
    echo "union-conversion memory regression: peak RSS ${union_conversion_rss} KiB" >&2
    exit 1
fi

cat > "$TMP/out-of-bounds.qui" <<'QUI'
int[] values = [1, 2, 3]
print(values[3])
QUI
set +e
"$QUIDRA" run "$TMP/out-of-bounds.qui" > "$TMP/out-of-bounds.out" 2>&1
out_of_bounds_rc=$?
set -e
[[ "$out_of_bounds_rc" -eq 101 ]]
grep -q 'INDEX_BOUNDS' "$TMP/out-of-bounds.out"
grep -q 'at 2:7:' "$TMP/out-of-bounds.out"
grep -q 'index 3 outside length 3' "$TMP/out-of-bounds.out"

cat > "$TMP/full-array-proof.qui" <<'QUI'
int[] values = array(4, fill = 1)
print(values[2])
QUI
"$QUIDRA" llvm "$TMP/full-array-proof.qui" > "$TMP/full-array-proof.ll"
if grep -q 'call void @quidra_init_check' "$TMP/full-array-proof.ll"; then
    echo "fully initialized array retained a redundant initialization check" >&2
    exit 1
fi
[[ "$("$QUIDRA" run "$TMP/full-array-proof.qui")" == "1" ]]

cat > "$TMP/full-array-write-proof.qui" <<'QUI'
int[] values = array(4, fill = 1)
values[2] = 9
print(values[2])
QUI
"$QUIDRA" llvm "$TMP/full-array-write-proof.qui" > "$TMP/full-array-write-proof.ll"
full_array_mark_count="$(grep -c 'call void @quidra_init_mark_range' "$TMP/full-array-write-proof.ll" || true)"
[[ "$full_array_mark_count" -eq 0 ]]
[[ "$("$QUIDRA" run "$TMP/full-array-write-proof.qui")" == "9" ]]

cat > "$TMP/loop-array-proof.qui" <<'QUI'
int[] values = array(8)
for i in range(len(values))
    values[i] = i * 2
print(values[7])
QUI
"$QUIDRA" llvm "$TMP/loop-array-proof.qui" > "$TMP/loop-array-proof.ll"
if grep -q 'call void @quidra_init_check' "$TMP/loop-array-proof.ll"; then
    echo "full-range initialization loop retained a redundant init check" >&2
    exit 1
fi
[[ "$("$QUIDRA" run "$TMP/loop-array-proof.qui")" == "14" ]]

cat > "$TMP/partial-loop-array-proof.qui" <<'QUI'
int[] values = array(8)
for i in range(0, 7)
    values[i] = i
print(values[7])
QUI
"$QUIDRA" llvm "$TMP/partial-loop-array-proof.qui" > "$TMP/partial-loop-array-proof.ll"
grep -q 'call void @quidra_init_check' "$TMP/partial-loop-array-proof.ll"
set +e
"$QUIDRA" run "$TMP/partial-loop-array-proof.qui" > "$TMP/partial-loop-array-proof.out" 2>&1
partial_loop_array_rc=$?
set -e
[[ "$partial_loop_array_rc" -eq 101 ]]
grep -qi 'uninitialized' "$TMP/partial-loop-array-proof.out"


# Match cases are mutually exclusive: an initialization proof learned in one
# case must not leak into a later case.
cat > "$TMP/match-array-proof-isolation.qui" <<'QUI'
int | string choice = "text"
int[] values = array(2)
int index = 0
match choice
    int
        values = [1, 2]
    string
        print(values[index])
QUI
"$QUIDRA" llvm "$TMP/match-array-proof-isolation.qui" > "$TMP/match-array-proof-isolation.ll"
grep -q 'call void @quidra_init_check' "$TMP/match-array-proof-isolation.ll"
set +e
"$QUIDRA" run "$TMP/match-array-proof-isolation.qui" > "$TMP/match-array-proof-isolation.out" 2>&1
match_array_isolation_rc=$?
set -e
[[ "$match_array_isolation_rc" -eq 101 ]]
grep -qi 'uninitialized' "$TMP/match-array-proof-isolation.out"

# A proof may survive the join only when every continuing case establishes it.
cat > "$TMP/match-array-proof-join.qui" <<'QUI'
int | string choice = 1
int[] values = array(2)
match choice
    int
        values = [3, 4]
    string
        values = [5, 6]
print(values[1])
QUI
"$QUIDRA" llvm "$TMP/match-array-proof-join.qui" > "$TMP/match-array-proof-join.ll"
if grep -q 'call void @quidra_init_check' "$TMP/match-array-proof-join.ll"; then
    echo "match join lost a proof true in every continuing case" >&2
    exit 1
fi
[[ "$("$QUIDRA" run "$TMP/match-array-proof-join.qui")" == "4" ]]

cat > "$TMP/zero-filled-array-proof.qui" <<'QUI'
float[] values = array(1000, fill = 0.0)
print(values[999])
QUI
"$QUIDRA" ir "$TMP/zero-filled-array-proof.qui" > "$TMP/zero-filled-array-proof.ir"
if grep -q 'fill.cond' "$TMP/zero-filled-array-proof.ir"; then
    echo "zero-filled array retained a redundant element fill loop" >&2
    exit 1
fi
[[ "$("$QUIDRA" run "$TMP/zero-filled-array-proof.qui")" == "0.0" ]]

cat > "$TMP/partial-array-proof.qui" <<'QUI'
int[] values = array(4)
values[2] = 7
print(values[2])
QUI
"$QUIDRA" llvm "$TMP/partial-array-proof.qui" > "$TMP/partial-array-proof.ll"
grep -q 'call void @quidra_init_check' "$TMP/partial-array-proof.ll"
[[ "$("$QUIDRA" run "$TMP/partial-array-proof.qui")" == "7" ]]

cat > "$TMP/escaped-array-proof.qui" <<'QUI'
void reset(int[] &values)
    values = array(4)

int[] values = array(4, fill = 1)
reset(&values)
print(values[0])
QUI
"$QUIDRA" llvm "$TMP/escaped-array-proof.qui" > "$TMP/escaped-array-proof.ll"
grep -q 'call void @quidra_init_check' "$TMP/escaped-array-proof.ll"
set +e
"$QUIDRA" run "$TMP/escaped-array-proof.qui" > "$TMP/escaped-array-proof.out" 2>&1
escaped_array_rc=$?
set -e
[[ "$escaped_array_rc" -eq 101 ]]
grep -qi 'uninitialized' "$TMP/escaped-array-proof.out"

cat > "$TMP/readonly-array-proof.qui" <<'QUI'
void inspect(const int[] &values)
    print(values[0])

int[] values = array(4, fill = 1)
inspect(&values)
print(values[2])
QUI
"$QUIDRA" llvm "$TMP/readonly-array-proof.qui" > "$TMP/readonly-array-proof.ll"
readonly_init_check_count="$(grep -c 'call void @quidra_init_check' "$TMP/readonly-array-proof.ll" || true)"
[[ "$readonly_init_check_count" -eq 1 ]]
readonly_array_output=$("$QUIDRA" run "$TMP/readonly-array-proof.qui")
readonly_array_expected=$(printf '1\n1')
[[ "$readonly_array_output" == "$readonly_array_expected" ]]

cat > "$TMP/readonly-local-array-proof.qui" <<'QUI'
int[] values = array(4, fill = 2)
const int[] &view = &values
print(values[3])
print(view[1])
QUI
"$QUIDRA" llvm "$TMP/readonly-local-array-proof.qui" > "$TMP/readonly-local-array-proof.ll"
readonly_local_init_check_count="$(grep -c 'call void @quidra_init_check' "$TMP/readonly-local-array-proof.ll" || true)"
[[ "$readonly_local_init_check_count" -eq 1 ]]
readonly_local_output=$("$QUIDRA" run "$TMP/readonly-local-array-proof.qui")
readonly_local_expected=$(printf '2\n2')
[[ "$readonly_local_output" == "$readonly_local_expected" ]]

cat > "$TMP/loop-control.qui" <<'QUI'
int sum = 0
for i in range(0, 10)
    if i == 2
        continue
    if i == 6
        break
    sum = sum + i
print(sum)

int[] values = [1, 2, 3]
for &value in values
    value = value * 10
    if value == 20
        continue
    if value == 30
        break
print(values[0])
print(values[1])
print(values[2])
QUI
loop_control_output=$("$QUIDRA" run "$TMP/loop-control.qui")
loop_control_expected=$(printf '13\n10\n20\n30')
[[ "$loop_control_output" == "$loop_control_expected" ]]

cat > "$TMP/compound-assignment.qui" <<'QUI'
int next(int &calls)
    calls += 1
    return 0

int calls = 0
int[] values = [10]
values[next(&calls)] += 5
print(values[0])
print(calls)

int x = 20
x -= 3
x *= 2
x /= 17
x %= 2
print(x)

string suffix()
    return "b" + "c"

string text = "a"
text += suffix()
print(text)
QUI
compound_output=$("$QUIDRA" run "$TMP/compound-assignment.qui")
compound_expected=$(printf '15\n1\n0\nabc')
[[ "$compound_output" == "$compound_expected" ]]

cat > "$TMP/temporary-reference.qui" <<'QUI'
int &item = &array(1, fill = 1)[0]
print(item)
QUI
set +e
"$QUIDRA" check "$TMP/temporary-reference.qui" --json > "$TMP/temporary-reference.json"
temporary_reference_rc=$?
set -e
[[ "$temporary_reference_rc" -eq 1 ]]
grep -q 'REFERENCE_BINDING' "$TMP/temporary-reference.json"

cat > "$TMP/temporary-write.qui" <<'QUI'
array(1, fill = int(1))[0] = 2
QUI
set +e
"$QUIDRA" check "$TMP/temporary-write.qui" --json > "$TMP/temporary-write.json"
temporary_write_rc=$?
set -e
[[ "$temporary_write_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/temporary-write.json"

$QUIDRA check "$ROOT/examples/results.qui" --json > "$TMP/check.json"
grep -q '"ok":true' "$TMP/check.json"
$QUIDRA inspect "$ROOT/examples/functions.qui" > "$TMP/inspect.json"
grep -q '"schema_version":1' "$TMP/inspect.json"

cat > "$TMP/stack-guard.qui" <<'QUI'
int plus_one(int value)
    return value + 1

int recurse(int value)
    if value == 0
        return 0
    return recurse(value - 1)

print(plus_one(1))
QUI
$QUIDRA llvm "$TMP/stack-guard.qui" > "$TMP/stack-guard.ll"
python3 - "$TMP/stack-guard.ll" <<'PY'
import re,sys
text=open(sys.argv[1]).read()
def body(symbol):
    match=re.search(r"define [^{]+ @"+re.escape(symbol)+r"\([^)]*\) \{(.*?)\n\}", text, re.S)
    assert match, symbol
    return match.group(1)
assert "@quidra_stack_enter()" not in body("n_plus_one")
assert "@quidra_stack_leave()" not in body("n_plus_one")
assert "@quidra_stack_enter()" in body("n_recurse")
assert "@quidra_stack_leave()" in body("n_recurse")
PY

cat > "$TMP/llvm-import-lib.qui" <<'QUI'
int imported_value()
    return 7
QUI
cat > "$TMP/llvm-import-root.qui" <<'QUI'
import lib = "./llvm-import-lib.qui"
print(lib.imported_value())
QUI
$QUIDRA llvm "$TMP/llvm-import-root.qui" > "$TMP/llvm-import.ll"
grep -q 'define.*imported_value' "$TMP/llvm-import.ll"

cat > "$TMP/effects.qui" <<'QUI'
class EffectBox
    int x
    int y

    int read()
        return x

    void initialize()
        y = 7

void fill(int &value)
    value = 5

int | none maybe(bool present)
    if present
        return 1
    return none
QUI
$QUIDRA inspect "$TMP/effects.qui" > "$TMP/effects.json"
python3 - "$TMP/effects.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert any(n["kind"]=="none" for n in x["nodes"])
effects=x["effects"]
assert any("x" in e["receiver"]["requires"] for e in effects)
assert any("y" in e["receiver"]["writes"] and "y" in e["receiver"]["initializes"] for e in effects)
ref=[e for e in effects if "value" in e["references"]]
assert ref
value=ref[0]["references"]["value"]
assert "" in value["writes"]
assert "" in value["initializes"]
PY

cat > "$TMP/effect-isolation.qui" <<'QUI'
class Mutable
    int counter

    void bump()
        counter = counter + 1

class Pure
    int a
    int b

    int total()
        return a + b
QUI
"$QUIDRA" inspect "$TMP/effect-isolation.qui" > "$TMP/effect-isolation.json"
python3 - "$TMP/effect-isolation.json" <<'PY'
import json,sys
effects={e["function"]:e for e in json.load(open(sys.argv[1]))["effects"]}
mutable=effects["$method.Mutable.bump"]["receiver"]
pure=effects["$method.Pure.total"]["receiver"]
assert "counter" in mutable["writes"]
assert pure["writes"] == []
assert pure["invalidates"] == []
assert pure["requires"] == ["a","b"]
PY

cat > "$TMP/nested-effects.qui" <<'QUI'
class Inner
    int x

class Box
    Inner inner
    int y

void initialize_x(Box &box)
    box.inner.x = 7

int read_y(Box &box)
    return box.y

void initialize_x_both(Box &box, bool flag)
    if flag
        box.inner.x = 1
    else
        box.inner.x = 2

void initialize_x_one_branch(Box &box, bool flag)
    if flag
        box.inner.x = 1

Box box = Box(inner = Inner())
initialize_x(&box)
print(box.inner.x)

Box both = Box(inner = Inner())
initialize_x_both(&both, true)
print(both.inner.x)
QUI
nested_output=$("$QUIDRA" run "$TMP/nested-effects.qui")
nested_expected=$(printf '7\n1')
[[ "$nested_output" == "$nested_expected" ]]

"$QUIDRA" inspect "$TMP/nested-effects.qui" > "$TMP/nested-effects.json"
python3 - "$TMP/nested-effects.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
effects=x["effects"]
init=next(
    e for e in effects
    if "box" in e["references"] and "inner.x" in e["references"]["box"]["initializes"]
)
assert "inner.x" in init["references"]["box"]["writes"]
reader=next(
    e for e in effects
    if "box" in e["references"] and "y" in e["references"]["box"]["requires"]
)
assert "" not in reader["references"]["box"]["requires"]
both=[
    e for e in effects
    if "box" in e["references"] and "inner.x" in e["references"]["box"]["initializes"]
]
assert len(both) >= 2
PY

cat > "$TMP/nested-effect-failure.qui" <<'QUI'
class Inner
    int x

class Box
    Inner inner

void initialize_x_one_branch(Box &box, bool flag)
    if flag
        box.inner.x = 1

Box box = Box(inner = Inner())
initialize_x_one_branch(&box, false)
print(box.inner.x)
QUI
set +e
"$QUIDRA" check "$TMP/nested-effect-failure.qui" --json > "$TMP/nested-effect-failure.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/nested-effect-failure.json"

cat > "$TMP/reference-field-effects.qui" <<'QUI'
class Pair
    int x
    int y

    void initialize_x()
        x = 11

void set_x(Pair &pair)
    pair.x = 7

int read_x(Pair &pair)
    return pair.x

void initialize_through_method(Pair &pair)
    pair.initialize_x()

void choose_x(bool first, Pair &pair)
    if first
        pair.x = 3
    else
        pair.x = 4

void maybe_y(bool enabled, Pair &pair)
    if enabled
        pair.y = 9

Pair direct = Pair()
set_x(&direct)
print(read_x(&direct))

Pair through_method = Pair()
initialize_through_method(&through_method)
print(read_x(&through_method))

Pair branched = Pair()
choose_x(true, &branched)
print(read_x(&branched))
QUI
reference_output=$("$QUIDRA" run "$TMP/reference-field-effects.qui")
reference_expected=$(printf '7\n11\n3')
[[ "$reference_output" == "$reference_expected" ]]

"$QUIDRA" inspect "$TMP/reference-field-effects.qui" > "$TMP/reference-field-effects.json"
python3 - "$TMP/reference-field-effects.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
effects=x["effects"]
pair_effects=[e["references"]["pair"] for e in effects if "pair" in e["references"]]
assert any("x" in e["writes"] and "x" in e["initializes"] for e in pair_effects)
assert any("x" in e["requires"] and "" not in e["requires"] for e in pair_effects)
assert any("x" in e["initializes"] for e in pair_effects)
assert any("y" in e["writes"] and "y" not in e["initializes"] for e in pair_effects)
PY

cat > "$TMP/value.qui" <<'QUI'
void touch(int[] data)
    data[0] = 9
void change(int[] &data)
    data[0] = 9
int[] values = [1, 2, 3]
touch(values)
print(values[0])
change(&values)
print(values[0])
QUI
[[ "$($QUIDRA run "$TMP/value.qui")" == $'1\n9' ]]

cat > "$TMP/address-model.qui" <<'QUI'
void initialize(int &x)
    x = 13

class Point
    int x
    int y

class Counter
    int value

    void reset()
        value = 0

    void increment()
        value = value + 1

int a = 1
int c = 2
int &b = &a
b = 5
&b = &c
b = 8
print(a)
print(c)

int uninitialized
int &u = &uninitialized
u = 11
print(uninitialized)

int from_function
initialize(&from_function)
print(from_function)

int[] values = [1, 2, 3]
int &first = &values[0]
values = [4, 5]
print(first)
print(values[0])
first = 9
print(values[0])

Point p = Point(x = 1)
int &field = &p.y
field = 5
print(p.y)

int &old_x = &p.x
p = Point(x = 10, y = 20)
print(old_x)
print(p.x)

Point &whole = &p
p = Point(x = 30, y = 40)
print(whole.x)

Point partial = Point(x = 7)
Point copy = partial
copy.y = 6
print(copy.x)
print(copy.y)

Counter counter = Counter()
counter.reset()
counter.increment()
print(counter.value)
QUI
[[ "$($QUIDRA run "$TMP/address-model.qui")" == $'5\n8\n11\n13\n1\n4\n4\n5\n1\n10\n30\n7\n6\n1' ]]


# Rebinding heap substorage repeatedly stresses pin/unpin and deferred parent
# finalization. The final reference must still point at the pre-replacement
# right-hand storage rather than becoming dangling or retargeted.
cat > "$TMP/reference-rebind-lifetime.qui" <<'QUI'
int[] left = [1]
int[] right = [2]
int &slot = &left[0]
for i in range(0, 2000)
    &slot = &left[0]
    left = [i]
    &slot = &right[0]
    right = [i]
slot = 77
print(slot)
print(right[0])
QUI
reference_rebind_lifetime_output=$("$QUIDRA" run "$TMP/reference-rebind-lifetime.qui")
reference_rebind_lifetime_expected=$(printf '77\n1999')
[[ "$reference_rebind_lifetime_output" == "$reference_rebind_lifetime_expected" ]]

cat > "$TMP/import-shadow.qui" <<'QUI'
int file = 1
QUI
set +e
"$QUIDRA" check "$TMP/import-shadow.qui" --json > "$TMP/import-shadow.json"
import_shadow_rc=$?
set -e
[[ "$import_shadow_rc" -eq 1 ]]
grep -q 'SHADOWING' "$TMP/import-shadow.json"

cat > "$TMP/import-builtin-shadow.qui" <<'QUI'
import input = plotting
QUI
set +e
"$QUIDRA" check "$TMP/import-builtin-shadow.qui" --json > "$TMP/import-builtin-shadow.json"
import_builtin_shadow_rc=$?
set -e
[[ "$import_builtin_shadow_rc" -eq 1 ]]
grep -q 'SHADOWING' "$TMP/import-builtin-shadow.json"

mkdir -p "$TMP/project/src" "$TMP/project/lib"
cat > "$TMP/project/lib/util.qui" <<'QUI'
int bump(int value)
    return value + 1
QUI

cat > "$TMP/project/src/geometry.qui" <<'QUI'
import util = "../lib/util.qui"

class Point
    int x
    int y

int sum(Point point)
    return util.bump(point.x + point.y)
QUI

cat > "$TMP/project/shared.qui" <<'QUI'
class Box<T>
    T value

    T get()
        return value

T first<T>(T[] values)
    return values[0]

class GenericParent
    T echo<T>(T value)
        return value

class GenericChild : GenericParent
    override T echo<T>(T value)
        return super.echo<T>(value)
QUI

cat > "$TMP/project/math.qui" <<'QUI'
int square(int value)
    return value * value
QUI

cat > "$TMP/project/src/main.qui" <<'QUI'
import geo = "./geometry.qui"
import shared = "@/shared.qui"
import m = "@/math.qui"

geo.Point point = geo.Point(x = 2, y = 3)
shared.Box<int> box = shared.Box<int>(value = 7)
shared.GenericChild child = shared.GenericChild()

print(geo.sum(point))
print(box.get())
print(shared.first<int>([9, 10]))
print(m.square(4))
print(child.echo<int>(11))
QUI

[[ "$(cd "$TMP/project" && "$QUIDRA" run src/main.qui)" == $'6\n7\n9\n16\n11' ]]

cat > "$TMP/project/src/generic-inference.qui" <<'QUI'
T identity<T>(T value)
    return value

class Echo
    T echo<T>(T value)
        return value

int sample = 7
int inferred = identity(sample)
Echo instance = Echo()
int method_inferred = instance.echo(sample)
print(inferred)
print(method_inferred)
QUI
[[ "$(cd "$TMP/project" && "$QUIDRA" run src/generic-inference.qui)" == $'7\n7' ]]

cat > "$TMP/project/src/stdlib-math.qui" <<'QUI'
float pi_value = math.pi
float e_value = math.e
print(pi_value)
print(e_value)
print(math.sin(float(0.0)))
print(math.cos(float(0.0)))
print(math.pow(float(2.0), 3.0))
QUI
stdlib_math_output="$(cd "$TMP/project" && "$QUIDRA" run src/stdlib-math.qui)"
python3 - "$stdlib_math_output" <<'PY'
import math,sys
values=[float(x) for x in sys.argv[1].splitlines()]
assert abs(values[0]-math.pi) < 1e-15
assert abs(values[1]-math.e) < 1e-15
assert values[2] == 0.0
assert values[3] == 1.0
assert values[4] == 8.0
PY

cat > "$TMP/project/src/unknown-standard.qui" <<'QUI'
import definitely_not_a_standard_module
print("no")
QUI
set +e
(cd "$TMP/project" && "$QUIDRA" check src/unknown-standard.qui --json) > "$TMP/unknown-standard.json"
unknown_standard_rc=$?
set -e
[[ "$unknown_standard_rc" -eq 1 ]]
grep -q 'PACKAGE_NOT_INSTALLED' "$TMP/unknown-standard.json"
(cd "$TMP/project" && "$QUIDRA" inspect src/main.qui) > "$TMP/module-inspect.json"
python3 - "$TMP/module-inspect.json" "$TMP/project/src/main.qui" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
size=len(open(sys.argv[2],"rb").read())
assert x["ok"] is True
assert x["nodes"]
assert all(0 <= n["span"]["start"]["offset"] <= size for n in x["nodes"])
assert all(0 <= n["span"]["end"]["offset"] <= size for n in x["nodes"])
PY

cat > "$TMP/project/src/module-patch.qui" <<'QUI'
import m = "@/math.qui"
int value = 3
print(m.square(value))
QUI
(cd "$TMP/project" && "$QUIDRA" inspect src/module-patch.qui) > "$TMP/module-patch-inspect.json"
python3 - "$TMP/module-patch-inspect.json" "$TMP/module-patch-change.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
node=next(n for n in x["nodes"] if n["kind"]=="integer" and n["source"]=="3")
json.dump({
    "schema_version":1,
    "base_revision":x["revision"],
    "operations":[{
        "op":"replace_node",
        "node_id":node["node_id"],
        "expected_hash":node["source_hash"],
        "replacement":"4"
    }]
},open(sys.argv[2],"w"))
PY
(cd "$TMP/project" && "$QUIDRA" patch src/module-patch.qui "$TMP/module-patch-change.json" --write) >/dev/null
[[ "$(cd "$TMP/project" && "$QUIDRA" run src/module-patch.qui)" == "16" ]]

cat > "$TMP/project/src/bad-module.qui" <<'QUI'
print("side effect")
QUI
cat > "$TMP/project/src/bad-import.qui" <<'QUI'
import bad = "./bad-module.qui"
print("root")
QUI
set +e
(cd "$TMP/project" && "$QUIDRA" check src/bad-import.qui --json) > "$TMP/import-error.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'IMPORT_TOP_LEVEL' "$TMP/import-error.json"

cat > "$TMP/project/src/cycle-a.qui" <<'QUI'
import b = "./cycle-b.qui"
int a()
    return 1
QUI
cat > "$TMP/project/src/cycle-b.qui" <<'QUI'
import a = "./cycle-a.qui"
int b()
    return 2
QUI
cat > "$TMP/project/src/cycle-root.qui" <<'QUI'
import a = "./cycle-a.qui"
print(a.a())
QUI
set +e
(cd "$TMP/project" && "$QUIDRA" check src/cycle-root.qui --json) > "$TMP/import-cycle.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'IMPORT_CYCLE' "$TMP/import-cycle.json"

cat > "$TMP/project/src/alias-collision.qui" <<'QUI'
import geo = "./geometry.qui"
int geo()
    return 1
print(geo())
QUI
set +e
(cd "$TMP/project" && "$QUIDRA" check src/alias-collision.qui --json) > "$TMP/import-alias.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'DUPLICATE_IMPORT_ALIAS' "$TMP/import-alias.json"

cat > "$TMP/project/src/alias-shadow.qui" <<'QUI'
import geo = "./geometry.qui"
int geo = 1
print(geo)
QUI
set +e
(cd "$TMP/project" && "$QUIDRA" check src/alias-shadow.qui --json) > "$TMP/import-shadow.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'SHADOWING' "$TMP/import-shadow.json"

cat > "$TMP/class-init-summary.qui" <<'QUI'
class Model
    float bb

Model build()
    return Model(bb = 2.0)

class Data
    float[] ys

class Holder
    Data data

    float first()
        data.ys = [3.0]
        return data.ys[0]

Model model = build()
Holder holder = Holder(data = Data())
print(model.bb)
print(holder.first())
QUI
[[ "$($QUIDRA run "$TMP/class-init-summary.qui")" == $'2.0\n3.0' ]]

cat > "$TMP/constant-zero.qui" <<'QUI'
print(int(1) / 0)
QUI
set +e
$QUIDRA check "$TMP/constant-zero.qui" --json > "$TMP/constant-zero.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'DIVIDE_BY_ZERO' "$TMP/constant-zero.json"

cat > "$TMP/runtime-zero.qui" <<'QUI'
int zero = 0
print(1 / zero)
QUI
set +e
$QUIDRA run "$TMP/runtime-zero.qui" > "$TMP/runtime-zero.out" 2>&1
rc=$?
set -e
[[ "$rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[DIVISION_BY_ZERO\] at [0-9]+:[0-9]+: division by zero' "$TMP/runtime-zero.out"

cat > "$TMP/patch-target.qui" <<'QUI'
int answer = 41
print(answer)
QUI
$QUIDRA inspect "$TMP/patch-target.qui" > "$TMP/patch-inspect.json"
python3 - "$TMP/patch-inspect.json" "$TMP/change.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
node=next(n for n in x['nodes'] if n['kind']=='integer')
json.dump({'schema_version':1,'base_revision':x['revision'],'operations':[{'op':'replace_node','node_id':node['node_id'],'expected_hash':node['source_hash'],'replacement':'42'}]},open(sys.argv[2],'w'))
PY
$QUIDRA patch "$TMP/patch-target.qui" "$TMP/change.json" --write >/dev/null
[[ "$($QUIDRA run "$TMP/patch-target.qui")" == "42" ]]

cat > "$TMP/regressions.qui" <<'QUI'
// Branch-local bindings may reuse a source name with different types.
bool c = true
if c
    int x = 1234567890123
    print(x)
else
    bool x = true
print("[" + "" + "]")
QUI
[[ "$($QUIDRA run "$TMP/regressions.qui")" == $'1234567890123\n[]' ]]

cat > "$TMP/builtins.qui" <<'QUI'
int[] values = [1, 2, 3]
print(len(values))
print(float(3))
print(abs(int(-5)))
print(abs(float(-2.5)))
print(sqrt(float(9.0)))
print(min(int(4), 2))
print(max(int(4), 2))
QUI
[[ "$($QUIDRA run "$TMP/builtins.qui")" == $'3\n3.0\n5\n2.5\n3.0\n2\n4' ]]

cat > "$TMP/float-format.qui" <<'QUI'
print(float(4.0))
print(float32(4.0))
print(float(4.5))
print(float(-2.0))
print("value={float(4.0)}")
write(float(6.0))
write("|")
print(float(7.0).string())
QUI
[[ "$($QUIDRA run "$TMP/float-format.qui")" == $'4.0\n4.0\n4.5\n-2.0\nvalue=4.0\n6.0|7.0' ]]

cat > "$TMP/interpolation-format.qui" <<'QUI'
float value = 12.3456
print("{value:frac=2}")
print("{value:int=4,frac=2,zero}")
print("{value:int=4,frac=2}")
print("{value:sig=4}")
print("{int(12345):sig=4}")
print("{float(0.00123456):sig=3}")
print("{int(12):sig=4}")
QUI
expected_format="$(printf '12.35\n0012.35\n  12.35\n12.35\n12350\n0.00123\n12.00')"
[[ "$($QUIDRA run "$TMP/interpolation-format.qui")" == "$expected_format" ]]

cat > "$TMP/interpolation-format-invalid.qui" <<'QUI'
print("{float(12.3):frac=2,sig=3}")
print("{int(12):zero}")
QUI
set +e
$QUIDRA check "$TMP/interpolation-format-invalid.qui" > "$TMP/interpolation-format-invalid.out" 2>&1
status=$?
set -e
[[ $status -ne 0 ]]
grep -Eq "frac.*sig|sig.*frac" "$TMP/interpolation-format-invalid.out"
grep -Eq "zero.*requires.*int" "$TMP/interpolation-format-invalid.out"

cat > "$TMP/numeric-types.qui" <<'QUI'
int8 a = 10
int8 b = 12
uint8 u = 200
uint8 v = 20
int16 widened = int16(a)
uint32 count = 100
int total = int(count)
float exact = float(a)
float32 compact = float32(1.5)
print(a + b)
print(b - a)
print(a * b)
print(b / a)
print(b % a)
print(u + v)
print(widened)
print(total)
print(exact)
print(compact)
print(a.string())
write("x")
write("y")
print("")
QUI
numeric_output="$($QUIDRA run "$TMP/numeric-types.qui")"
[[ "$numeric_output" == "$(printf '22\n2\n120\n1\n2\n220\n10\n100\n10.0\n1.5\n10\nxy')" ]]

cat > "$TMP/compact-storage.qui" <<'QUI'
int8[] small = [1, 2, 3]
uint16[] medium = [1000, 2000, 3000]
float32[] fractions = [1.5, 2.5]

class Mixed
    int8 a
    uint16 b
    float32 c
    uint8 d

Mixed value = Mixed(a = 7, b = 500, c = 1.5, d = 9)
int8 &a = &value.a
uint16 &b = &value.b
float32 &c = &value.c
uint8 &d = &value.d

a = 8
b = 600
c = 2.5
d = 10

print(small[2])
print(medium[1])
print(fractions[0])
print(value.a)
print(value.b)
print(value.c)
print(value.d)

Mixed copy = value
copy.b = 700
print(value.b)
print(copy.b)
QUI
compact_output="$($QUIDRA run "$TMP/compact-storage.qui")"
[[ "$compact_output" == "$(printf '3\n2000\n1.5\n8\n600\n2.5\n10\n600\n700')" ]]

cat > "$TMP/numeric-overflow.qui" <<'QUI'
int8 a = 120
int8 b = 10
print(a + b)
QUI
set +e
$QUIDRA run "$TMP/numeric-overflow.qui" > "$TMP/numeric-overflow.out" 2>&1
rc=$?
set -e
[[ "$rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[INTEGER_OVERFLOW\] at [0-9]+:[0-9]+: integer overflow' "$TMP/numeric-overflow.out"

cat > "$TMP/out-of-bounds.qui" <<'QUI'
int[] values = [1]
print(values[1])
QUI
set +e
"$QUIDRA" run "$TMP/out-of-bounds.qui" > "$TMP/out-of-bounds.out" 2>&1
bounds_rc=$?
set -e
[[ "$bounds_rc" -eq 101 ]]
grep -q 'INDEX_BOUNDS' "$TMP/out-of-bounds.out"
grep -q 'index 1 outside length 1' "$TMP/out-of-bounds.out"

cat > "$TMP/negative-index.qui" <<'QUI'
bin values = bin.fill(1, 0)
int index = -1
print(values[index])
QUI
set +e
"$QUIDRA" run "$TMP/negative-index.qui" > "$TMP/negative-index.out" 2>&1
negative_bounds_rc=$?
set -e
if [[ "$negative_bounds_rc" -ne 101 ]]; then
    echo "negative-index runtime returned $negative_bounds_rc" >&2
    cat "$TMP/negative-index.out" >&2
    exit 1
fi
grep -qi 'bounds\|INDEX_BOUNDS' "$TMP/negative-index.out"

cat > "$TMP/numeric-cast-failure.qui" <<'QUI'
int value = 300
print(int8(value))
QUI
set +e
$QUIDRA run "$TMP/numeric-cast-failure.qui" > "$TMP/numeric-cast-failure.out" 2>&1
rc=$?
set -e
[[ "$rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[NUMERIC_CAST_RANGE\] at [0-9]+:[0-9]+: numeric cast outside destination range' "$TMP/numeric-cast-failure.out"

cat > "$TMP/parse.qui" <<'QUI'
int | error parsed = int.parse("123")
float32 | error fraction = float32.parse("1.5")
int | error bad = int.parse("12x")
match parsed
    int value
        print(value)
    error e
        print(e)
match fraction
    float32 value
        print(value)
    error e
        print(e)
match bad
    int value
        print(value)
    error e
        print(e)
QUI
parse_output="$($QUIDRA run "$TMP/parse.qui")"
[[ "$parse_output" == "$(printf '123\n1.5\nnumeric parse failed')" ]]

cat > "$TMP/input.qui" <<'QUI'
auto line = input()
match line
    string value
        print(value)
    none
        print("eof")
    error e
        print(e)
QUI
input_output="$(printf 'hello\n' | $QUIDRA run "$TMP/input.qui")"
[[ "$input_output" == "hello" ]]
eof_output="$($QUIDRA run "$TMP/input.qui" < /dev/null)"
[[ "$eof_output" == "eof" ]]

invalid_input_output="$(python3 -c 'import sys; sys.stdout.buffer.write(b"\xff\n")' | "$QUIDRA" run "$TMP/input.qui")"
[[ "$invalid_input_output" == "input failed" ]]
nul_input_output="$(python3 -c 'import sys; sys.stdout.buffer.write(b"A\x00B\n")' | "$QUIDRA" run "$TMP/input.qui")"
[[ "$nul_input_output" == "input failed" ]]

cat > "$TMP/cli-text.qui" <<'QUI'
cli args
    string value = argument()

print(args.value)
QUI
cat > "$TMP/cli-exact.qui" <<'QUI'
cli args
    bigint count = argument()
    bigreal ratio = option(default = 0.1)

print(args.count)
print(args.ratio == bigreal(0.125))
QUI
[[ "$("$QUIDRA" run "$TMP/cli-exact.qui" -- 123456789012345678901234567890 --ratio 0.125)" == "$(printf '123456789012345678901234567890\ntrue')" ]]
[[ "$("$QUIDRA" run "$TMP/cli-exact.qui" -- 7)" == "$(printf '7\nfalse')" ]]

set +e
"$QUIDRA" run "$TMP/cli-exact.qui" -- nope >"$TMP/cli-exact-bigint.out" 2>"$TMP/cli-exact-bigint.err"
cli_exact_bigint_rc=$?
"$QUIDRA" run "$TMP/cli-exact.qui" -- 7 --ratio nope >"$TMP/cli-exact-bigreal.out" 2>"$TMP/cli-exact-bigreal.err"
cli_exact_bigreal_rc=$?
set -e
[[ "$cli_exact_bigint_rc" -eq 2 ]]
[[ "$cli_exact_bigreal_rc" -eq 2 ]]
grep -q 'Quidra CLI error: invalid bigint value' "$TMP/cli-exact-bigint.err"
grep -q 'Quidra CLI error: invalid bigreal value' "$TMP/cli-exact-bigreal.err"

python3 - "$QUIDRA" "$TMP/cli-text.qui" <<'PY'
import os
import subprocess
import sys

command = [
    os.fsencode(sys.argv[1]),
    b"run",
    os.fsencode(sys.argv[2]),
    b"--",
    b"\xff",
]
result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
if result.returncode != 2:
    raise SystemExit(f"invalid UTF-8 CLI status: {result.returncode}")
if b"Quidra CLI error:" not in result.stderr:
    raise SystemExit(f"missing CLI diagnostic: {result.stderr!r}")
PY

cat > "$TMP/bin-value.qui" <<'QUI'
bin data = bin.fill(4, 0)
data[0] = bin.parse("1")
bin copy = data
copy[2] = bin.parse("1")
print(len(data))
print(data[0])
print(data[1])
print(data[2])
print(copy[2])
print(data == copy)
copy[2] = data[2]
print(data == copy)
for value in data
    print(value)
for &value in copy
    value = bin.parse("1")
print(copy[0])
print(data[0])
QUI
bin_output="$($QUIDRA run "$TMP/bin-value.qui")"
[[ "$bin_output" == "$(printf '4\n1\n0\n0\n1\nfalse\ntrue\n1\n0\n0\n0\n1\n1')" ]]

cat > "$TMP/bin-fill.qui" <<'QUI'
bin empty = bin.fill(0, 0)
bin zeros = bin.fill(4, 0)
print(len(empty))
print(len(zeros))
print(zeros[0])
QUI
[[ "$("$QUIDRA" run "$TMP/bin-fill.qui")" == "$(printf '0\n4\n0')" ]]

cat > "$TMP/interpolation-span.qui" <<'QUI'
void show()
    string root = "x/{ghost.field}"
QUI
set +e
$QUIDRA check "$TMP/interpolation-span.qui" --json > "$TMP/interpolation-span.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
python3 - "$TMP/interpolation-span.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
d=x["diagnostics"][0]
assert d["span"]["start"] == {"line": 2, "column": 23}, d
PY

cat > "$TMP/interpolation-parse-span.qui" <<'QUI'
void show()
    string root = "x/{ghost.}"
QUI
set +e
$QUIDRA check "$TMP/interpolation-parse-span.qui" --json > "$TMP/interpolation-parse-span.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
python3 - "$TMP/interpolation-parse-span.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
d=x["diagnostics"][0]
assert d["code"] == "PARSE_ERROR", d
assert d["span"]["start"] == {"line": 2, "column": 29}, d
PY

cat > "$TMP/multi-errors.qui" <<'QUI'
bool a = 1
int b = true
string c = 3
float d = false
QUI
set +e
$QUIDRA check "$TMP/multi-errors.qui" --json --max-errors 3 > "$TMP/multi-errors.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
python3 - "$TMP/multi-errors.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["ok"] is False
assert x["truncated"] is True
assert len(x["diagnostics"]) == 3
PY

set +e
$QUIDRA check "$TMP/multi-errors.qui" --json --max-errors 10 > "$TMP/multi-errors-all.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
python3 - "$TMP/multi-errors-all.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["truncated"] is False
assert len(x["diagnostics"]) == 4
PY

cat > "$TMP/parser-errors.qui" <<'QUI'
int a =
bool b =
string c =
QUI
set +e
$QUIDRA check "$TMP/parser-errors.qui" --json --max-errors 10 > "$TMP/parser-errors.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
python3 - "$TMP/parser-errors.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["truncated"] is False
assert len(x["diagnostics"]) == 3
assert all(d["code"] == "PARSE_ERROR" for d in x["diagnostics"])
PY


cat > "$TMP/repl-input.txt" <<'QUI'
int(1) + 2
int x = 5
x
x = 8
x
int square(int value)
    return value * value

square(6)
class Box
    int value

Box box = Box(value = 9)
box.value
T identity<T>(T value)
    return value

identity<int>(7)
int[] repl_values = [1, 2, 3]
repl_values
bin.fill(3, 1)
box
Box partial = Box()
partial
none
"hello"
bool broken = 1
1 + 2
x
int(10) / 0
x
float y = 4.0
y
:type y
:exit
QUI
set +e
"$QUIDRA" repl < "$TMP/repl-input.txt" > "$TMP/repl.out" 2> "$TMP/repl.err"
repl_rc=$?
set -e
[[ "$repl_rc" -eq 0 ]]
python3 - "$TMP/repl.out" "$TMP/repl.err" <<'PY'
import sys
out=open(sys.argv[1]).read()
err=open(sys.argv[2]).read()
expected=[
    "3","5","8","36","9","7",
    "[1, 2, 3]","111","Box(value = 9)",
    "Box(value = <uninitialized>)","none","\"hello\"",
    "8","8","4.0","float"
]
position=0
for value in expected:
    found=out.find(value, position)
    assert found >= 0, (value, out, err)
    position=found+len(value)
assert "TYPE_MISMATCH" in err
assert "AMBIGUOUS_NUMERIC_LITERAL" in err
assert "DIVIDE_BY_ZERO" in err
PY

set +e
"$QUIDRA" repl < /dev/null > "$TMP/repl-eof.out" 2> "$TMP/repl-eof.err"
repl_eof_rc=$?
set -e
[[ "$repl_eof_rc" -eq 0 ]]

# A blank line with nothing pending must not resubmit the accumulated session.
# It used to append an empty line to the accepted source and re-run the whole
# program, repeating every side effect already produced. Duplicate output is
# hidden by the REPL's prefix stripping when the program is deterministic, so
# the execution count is observed through a side effect outside stdout.
mkdir -p "$TMP/repl-blank"
cat > "$TMP/repl-blank/input.txt" <<'QUI'
void emit(string path)
    auto prev = file.read(path)
    match prev
        string
            auto saved = file.write(path, prev + "X")
        error
            auto saved = file.write(path, "X")
    print("MARK")

emit("marks.txt")

QUI
(
    cd "$TMP/repl-blank"
    "$QUIDRA" repl < input.txt > output.txt 2> error.txt
)
[[ "$(cat "$TMP/repl-blank/marks.txt")" == "X" ]]
grep -q MARK "$TMP/repl-blank/output.txt"

mkdir -p "$TMP/repl-replay-barrier"
cat > "$TMP/repl-replay-barrier/input.txt" <<'QUI'
auto saved = file.write("effect.txt", "A")
int plus_one(int value)
    return value + 1

:type plus_one(1)
int(1) + 2
:reset
int(1) + 2
:exit
QUI
(
    cd "$TMP/repl-replay-barrier"
    "$QUIDRA" repl < input.txt > output.txt 2> error.txt
)
[[ "$(cat "$TMP/repl-replay-barrier/effect.txt")" == "A" ]]
grep -q 'REPL_REPLAY_UNSAFE' "$TMP/repl-replay-barrier/error.txt"
grep -q 'int' "$TMP/repl-replay-barrier/output.txt"
grep -q 'REPL state reset.' "$TMP/repl-replay-barrier/output.txt"
grep -q '3' "$TMP/repl-replay-barrier/output.txt"

mkdir -p "$TMP/repl-failed-effect"
cat > "$TMP/repl-failed-effect/input.txt" <<'QUI'
void effect_then_fail(string path)
    auto previous = file.read(path)
    match previous
        string text
            auto saved = file.write(path, text + "X")
        error
            auto saved = file.write(path, "X")
    int zero = 0
    print(1 / zero)

effect_then_fail("effect.txt")
effect_then_fail("effect.txt")
:reset
int(1) + 2
:exit
QUI
(
    cd "$TMP/repl-failed-effect"
    "$QUIDRA" repl < input.txt > output.txt 2> error.txt
)
# The first call performs the write before its runtime failure. The second call
# must be blocked even though the failing candidate was never accepted.
[[ "$(cat "$TMP/repl-failed-effect/effect.txt")" == "X" ]]
grep -q 'DIVISION_BY_ZERO' "$TMP/repl-failed-effect/output.txt"
grep -q 'REPL_REPLAY_UNSAFE' "$TMP/repl-failed-effect/error.txt"
grep -q 'REPL state reset.' "$TMP/repl-failed-effect/output.txt"
grep -q '3' "$TMP/repl-failed-effect/output.txt"

python3 - "$QUIDRA" <<'PY'
import errno, os, pty, sys
binary=sys.argv[1]
pid, fd=pty.fork()
if pid == 0:
    os.execl(binary, binary)
os.write(fd, b"int(1) + 2\n:exit\n")
chunks=[]
while True:
    try:
        data=os.read(fd, 4096)
    except OSError as e:
        if e.errno == errno.EIO:
            break
        raise
    if not data:
        break
    chunks.append(data)
_, status=os.waitpid(pid, 0)
text=b"".join(chunks).decode(errors="replace").replace("\r", "")
assert os.waitstatus_to_exitcode(status) == 0, text
assert "Quidra " in text
assert ">>> " in text
assert "3" in text
PY

python3 - "$QUIDRA" <<'PY'
import errno, os, pty, signal, sys, time
binary=sys.argv[1]
pid, fd=pty.fork()
if pid == 0:
    os.execl(binary, binary)
os.write(fd, b"int interrupted(int x)\n")
time.sleep(0.2)
os.kill(pid, signal.SIGINT)
time.sleep(0.2)
os.write(fd, b"int(1) + 2\n:exit\n")
chunks=[]
while True:
    try:
        data=os.read(fd, 4096)
    except OSError as e:
        if e.errno == errno.EIO:
            break
        raise
    if not data:
        break
    chunks.append(data)
_, status=os.waitpid(pid, 0)
text=b"".join(chunks).decode(errors="replace").replace("\r", "")
assert os.waitstatus_to_exitcode(status) == 0, text
assert "3" in text, text
assert "error[" not in text, text
PY


cat > "$TMP/reference-aliasing.qui" <<'QUI'
void touch(int[] &values, int[] &same)
    values[0] = 7
    same[1] = values[0] + 2

void mixed(const int[] &view, int[] &sink)
    sink[0] = view[0] + 1

int[] data = [1, 2, 3]
touch(&data, &data)
print(data[0])
print(data[1])
mixed(&data, &data)
print(data[0])
QUI
reference_aliasing_output=$("$QUIDRA" run "$TMP/reference-aliasing.qui")
reference_aliasing_expected=$(printf '7\n9\n8')
[[ "$reference_aliasing_output" == "$reference_aliasing_expected" ]]

cat > "$TMP/reference-alias-call-entry.qui" <<'QUI'
void read_before_initialize(int &later, int &first)
    print(first)
    later = 1

int pending
read_before_initialize(&pending, &pending)
QUI
set +e
"$QUIDRA" check "$TMP/reference-alias-call-entry.qui" --json > "$TMP/reference-alias-call-entry.json"
reference_alias_call_entry_rc=$?
set -e
[[ "$reference_alias_call_entry_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/reference-alias-call-entry.json"

cat > "$TMP/reference-alias-initialize.qui" <<'QUI'
void initialize_both(int &left, int &right)
    left = 1
    right = 2

int pending
initialize_both(&pending, &pending)
print(pending)
QUI
[[ "$("$QUIDRA" run "$TMP/reference-alias-initialize.qui")" == "2" ]]

cat > "$TMP/receiver-alias-call-entry.qui" <<'QUI'
class Cell
    int value

    void read_before_initialize(int &later)
        print(value)
        later = 1

Cell cell = Cell()
cell.read_before_initialize(&cell.value)
QUI
set +e
"$QUIDRA" check "$TMP/receiver-alias-call-entry.qui" --json > "$TMP/receiver-alias-call-entry.json"
receiver_alias_call_entry_rc=$?
set -e
[[ "$receiver_alias_call_entry_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/receiver-alias-call-entry.json"

cat > "$TMP/receiver-alias-postcondition.qui" <<'QUI'
class State
    int value

    void initialize_then_replace(State &other)
        value = 1
        other = State()

State state = State(value = 0)
state.initialize_then_replace(&state)
print(state.value)
QUI
set +e
"$QUIDRA" check "$TMP/receiver-alias-postcondition.qui" --json > "$TMP/receiver-alias-postcondition.json"
receiver_alias_postcondition_rc=$?
set -e
[[ "$receiver_alias_postcondition_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/receiver-alias-postcondition.json"

cat > "$TMP/const-references.qui" <<'QUI'
void show(const int &value)
    print(value)

int value = 5
const int &view = &value
show(&value)
value = 7
print(view)

int &writer = &value
const int &read = &writer
writer = 9
print(read)

const int frozen = 11
const int &frozen_view = &frozen
print(frozen_view)
QUI
const_reference_output=$("$QUIDRA" run "$TMP/const-references.qui")
const_reference_expected=$(printf '5\n7\n9\n11')
[[ "$const_reference_output" == "$const_reference_expected" ]]

cat > "$TMP/const-auto.qui" <<'QUI'
int source = 5
const auto value = source
const auto &view = &source
source = 8
print(value)
print(view)
QUI
const_auto_output=$("$QUIDRA" run "$TMP/const-auto.qui")
const_auto_expected=$(printf '5\n8')
[[ "$const_auto_output" == "$const_auto_expected" ]]

cat > "$TMP/const-value-override.qui" <<'QUI'
class Base
    int echo(int value)
        return value

class Child : Base
    override int echo(const int value)
        return value

Child child = Child()
print(child.echo(7))
QUI
[[ "$("$QUIDRA" run "$TMP/const-value-override.qui")" == "7" ]]

cat > "$TMP/const-reference-override-mismatch.qui" <<'QUI'
class Base
    void touch(int &value)
        value = 1

class Child : Base
    override void touch(const int &value)
        print(value)
QUI
set +e
"$QUIDRA" check "$TMP/const-reference-override-mismatch.qui" --json > "$TMP/const-reference-override-mismatch.json"
const_reference_override_rc=$?
set -e
[[ "$const_reference_override_rc" -eq 1 ]]
grep -q 'OVERRIDE_MISMATCH' "$TMP/const-reference-override-mismatch.json"

cat > "$TMP/const-write-errors.qui" <<'QUI'
void bad(const int &value)
    value = 2

int source = 1
const int &read = &source
int &write = &read
QUI
set +e
"$QUIDRA" check "$TMP/const-write-errors.qui" --json > "$TMP/const-write-errors.json"
const_write_rc=$?
set -e
[[ "$const_write_rc" -eq 1 ]]
python3 - "$TMP/const-write-errors.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert any(d["code"] == "WRITE_CAPABILITY" for d in x["diagnostics"])
PY

cat > "$TMP/const-values.qui" <<'QUI'
const int answer = 42
print(answer)

class Item
    const int id
    int value

Item item = Item(id = 3, value = 4)
print(item.id)
QUI
const_values_output=$("$QUIDRA" run "$TMP/const-values.qui")
const_values_expected=$(printf '42\n3')
[[ "$const_values_output" == "$const_values_expected" ]]

cat > "$TMP/const-value-write.qui" <<'QUI'
const int answer = 42
answer = 43
QUI
set +e
"$QUIDRA" check "$TMP/const-value-write.qui" --json > "$TMP/const-value-write.json"
const_value_rc=$?
set -e
[[ "$const_value_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/const-value-write.json"

cat > "$TMP/const-field-write.qui" <<'QUI'
class Item
    const int id

Item item = Item(id = 1)
item.id = 2
QUI
set +e
"$QUIDRA" check "$TMP/const-field-write.qui" --json > "$TMP/const-field-write.json"
const_field_rc=$?
set -e
[[ "$const_field_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/const-field-write.json"

cat > "$TMP/const-uninitialized.qui" <<'QUI'
const int missing
int pending
const int &view = &pending
QUI
set +e
"$QUIDRA" check "$TMP/const-uninitialized.qui" --json > "$TMP/const-uninitialized.json"
const_uninitialized_rc=$?
set -e
[[ "$const_uninitialized_rc" -eq 1 ]]
python3 - "$TMP/const-uninitialized.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
codes={d["code"] for d in x["diagnostics"]}
assert "CONST_INITIALIZATION" in codes
assert "UNINITIALIZED" in codes
PY

cat > "$TMP/const-reference-rebind.qui" <<'QUI'
int first = 1
int second = 2
const int &view = &first
&view = &second
QUI
set +e
"$QUIDRA" check "$TMP/const-reference-rebind.qui" --json > "$TMP/const-reference-rebind.json"
const_rebind_rc=$?
set -e
[[ "$const_rebind_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/const-reference-rebind.json"

cat > "$TMP/const-value-parameter.qui" <<'QUI'
void change(const int value)
    value = 2
change(1)
QUI
set +e
"$QUIDRA" check "$TMP/const-value-parameter.qui" --json > "$TMP/const-value-parameter.json"
const_value_parameter_rc=$?
set -e
[[ "$const_value_parameter_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/const-value-parameter.json"

cat > "$TMP/const-method-read.qui" <<'QUI'
class Counter
    int value

    int get()
        return value

    void increment()
        value += 1

const Counter counter = Counter(value = 4)
print(counter.get())
QUI
[[ "$("$QUIDRA" run "$TMP/const-method-read.qui")" == "4" ]]

cat > "$TMP/const-method-write.qui" <<'QUI'
class Counter
    int value

    void increment()
        value += 1

const Counter counter = Counter(value = 4)
counter.increment()
QUI
set +e
"$QUIDRA" check "$TMP/const-method-write.qui" --json > "$TMP/const-method-write.json"
const_method_write_rc=$?
set -e
[[ "$const_method_write_rc" -eq 1 ]]
grep -q 'WRITE_CAPABILITY' "$TMP/const-method-write.json"

cat > "$TMP/const-field-missing.qui" <<'QUI'
class Item
    const int id
    int value

Item item = Item(value = 4)
QUI
set +e
"$QUIDRA" check "$TMP/const-field-missing.qui" --json > "$TMP/const-field-missing.json"
const_field_missing_rc=$?
set -e
[[ "$const_field_missing_rc" -eq 1 ]]
grep -q 'CONST_INITIALIZATION' "$TMP/const-field-missing.json"


mkdir -p "$TMP/repl-project"
cat > "$TMP/repl-project/math.qui" <<'QUI'
int triple(int value)
    return value * 3
QUI
cat > "$TMP/repl-project/input.txt" <<'QUI'
import local_math = "./math.qui"
local_math.triple(7)
:exit
QUI
(
    cd "$TMP/repl-project"
    "$QUIDRA" repl < input.txt > output.txt 2> error.txt
)
python3 - "$TMP/repl-project/output.txt" <<'PY'
import sys
out=open(sys.argv[1]).read()
assert "21" in out, out
PY
[[ ! -s "$TMP/repl-project/error.txt" ]]


"$QUIDRA" describe > "$TMP/describe-final.json"
python3 - "$TMP/describe-final.json" "$ROOT/quidra.manifest.json" <<'PY'
import json,sys
describe=json.load(open(sys.argv[1]))
manifest=json.load(open(sys.argv[2]))
assert describe == manifest
assert describe["call_style"] == manifest["call_style"]
assert set(describe["current_builtins"]) == set(manifest["current_builtins"])
assert describe["repl"] is True
PY


"$QUIDRA" describe llm > "$TMP/describe-llm.json"
python3 - "$TMP/describe-llm.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
tensor=x["inspection"]["type_contracts"]["tensor"]
rank=x["inspection"]["type_contracts"]["tensor_rank"]
assert tensor == "tensor<T> | tensor<T><D0, D1, ...>"
assert "exact-rank" in rank
assert "compiler-inferred" in rank
shape=x["inspection"]["type_contracts"]["tensor_shape"]
assert "integer expression" in shape
assert "captured" in shape
calls=x["calls"]
assert calls["argument_order"] == "positional_then_named"
assert calls["named_syntax"] == "name = value"
assert "duplicate_parameter" in calls["rejected"]
PY

cat > "$TMP/inspect-tensor-shape.qui" <<'QUI'
tensor<float32><2, 3> matrix = tensor.zeros<float32>([2, 3])
auto row = matrix[0]
auto dimensions = matrix.shape()
QUI
"$QUIDRA" inspect "$TMP/inspect-tensor-shape.qui" --no-source --no-effects > "$TMP/inspect-tensor-shape.json"
python3 - "$TMP/inspect-tensor-shape.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
types={n["inferred_type"] for n in x["nodes"] if n["inferred_type"]}
assert "tensor<float32><2, 3>" in types, types
assert "tensor<float32><3>" in types, types
assert "int[2]" in types, types
PY

cat > "$TMP/tensor-rank-inference.qui" <<'QUI'
tensor<float32> matrix = tensor.zeros<float32>([2, 3])
auto shape = matrix.shape()
print(shape[0])
print(shape[1])
QUI
"$QUIDRA" run "$TMP/tensor-rank-inference.qui" > "$TMP/tensor-rank-inference.out"
python3 - "$TMP/tensor-rank-inference.out" <<'PY'
import sys
text=open(sys.argv[1]).read()
assert text.splitlines() == ["2","3"], repr(text)
PY

"$QUIDRA" inspect "$ROOT/examples/classes.qui" --no-source --no-effects --kind integer > "$TMP/inspect-compact.json"
python3 - "$TMP/inspect-compact.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["nodes"]
assert all(n["kind"] == "integer" for n in x["nodes"])
assert all("source" not in n for n in x["nodes"])
assert x["effects"] == []
PY
"$QUIDRA" inspect "$ROOT/examples/classes.qui" > "$TMP/inspect-full.json"
python3 - "$TMP/inspect-full.json" "$TMP/inspect-compact.json" <<'PY'
import os,sys
assert os.path.getsize(sys.argv[2]) < os.path.getsize(sys.argv[1])
PY

"$QUIDRA" inspect "$ROOT/examples/classes.qui" --no-source --no-effects --depth 1 > "$TMP/inspect-depth.json"
python3 - "$TMP/inspect-depth.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert x["nodes"]
assert all(n["depth"] <= 1 for n in x["nodes"])
ids={n["node_id"] for n in x["nodes"]}
assert all(n["parent_id"] is None or n["parent_id"] in ids for n in x["nodes"])
assert any(n["parent_id"] is not None for n in x["nodes"])
PY


cat > "$TMP/uint64-literals.qui" <<'QUI'
uint64 high = 10000000000000000000
uint64 maximum = 18446744073709551615
print(high)
print(maximum)
QUI
[[ "$("$QUIDRA" run "$TMP/uint64-literals.qui")" == $'10000000000000000000\n18446744073709551615' ]]

cat > "$TMP/uint64-too-large.qui" <<'QUI'
uint64 value = 18446744073709551616
QUI
set +e
"$QUIDRA" check "$TMP/uint64-too-large.qui" --json > "$TMP/uint64-too-large.json"
uint64_large_rc=$?
set -e
[[ "$uint64_large_rc" -eq 1 ]]
python3 - "$TMP/uint64-too-large.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert any(d["code"] == "INTEGER_RANGE" for d in x["diagnostics"])
PY

cat > "$TMP/default-int-too-large.qui" <<'QUI'
auto value = 10000000000000000000
QUI
set +e
"$QUIDRA" check "$TMP/default-int-too-large.qui" --json > "$TMP/default-int-too-large.json"
default_large_rc=$?
set -e
[[ "$default_large_rc" -eq 1 ]]
python3 - "$TMP/default-int-too-large.json" <<'PY'
import json,sys
x=json.load(open(sys.argv[1]))
assert any(d["code"] == "AMBIGUOUS_NUMERIC_LITERAL" for d in x["diagnostics"])
PY

cat > "$TMP/deep-recursion.qui" <<'QUI'
int deep(int n)
    if n == 0
        return 0
    return 1 + deep(n - 1)

print(deep(10000000))
QUI
set +e
"$QUIDRA" run "$TMP/deep-recursion.qui" > "$TMP/deep-recursion.out" 2>&1
deep_rc=$?
set -e
[[ "$deep_rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[CALL_DEPTH_LIMIT\] at [0-9]+:[0-9]+: call depth limit' "$TMP/deep-recursion.out"

cat > "$TMP/standalone-none.qui" <<'QUI'
none
print("ok")
QUI
[[ "$("$QUIDRA" run "$TMP/standalone-none.qui")" == "ok" ]]


cat > "$TMP/float-canonical-text.qui" <<'QUI'
print(float(0.6))
print(float(1.0 / 3.0))
print(float(1.0))
print(float(-0.0))
print(float(1.0e20))
QUI
[[ "$("$QUIDRA" run "$TMP/float-canonical-text.qui")" == $'0.6\n0.3333333333333333\n1.0\n-0.0\n1.0e+20' ]]

cat > "$TMP/float-exception-text.qui" <<'QUI'
print(float(0.0 / 0.0))
print(float(1.0 / 0.0))
print(float(-1.0 / 0.0))
QUI
[[ "$("$QUIDRA" run "$TMP/float-exception-text.qui")" == $'nan\ninf\n-inf' ]]
