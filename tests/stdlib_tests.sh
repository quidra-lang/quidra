#!/usr/bin/env bash
set -euo pipefail
set -x
QUIDRA="$1"
ROOT="$2"
TMP="$(mktemp -d)"
trap 'if [[ -n "${HTTP_PID:-}" ]]; then kill "$HTTP_PID" 2>/dev/null || true; fi; rm -rf "$TMP"' EXIT

cat > "$TMP/cli.qui" <<'QUI'
cli args
    string source = argument()
    int count = option(default = 1)
    bool verbose = flag()

print(args.source)
print(NL)
print(args.count)
print(NL)
print(args.verbose)
print(NL)
QUI

[[ "$("$QUIDRA" "$TMP/cli.qui" image.jpg --count 5 --verbose)" == $'image.jpg\n5\ntrue' ]]
[[ "$("$QUIDRA" "$TMP/cli.qui" image.jpg)" == $'image.jpg\n1\nfalse' ]]
[[ "$("$QUIDRA" run "$TMP/cli.qui" -- image.jpg --count 2)" == $'image.jpg\n2\nfalse' ]]

"$QUIDRA" inspect "$TMP/cli.qui" --kind cli > "$TMP/cli-inspect.json"
python3 - "$TMP/cli-inspect.json" <<'PY'
import json, sys
x = json.load(open(sys.argv[1]))
assert x["ok"] is True
assert len(x["nodes"]) == 1
assert x["nodes"][0]["kind"] == "cli"
PY

for mode in missing invalid unknown duplicate; do
    set +e
    case "$mode" in
        missing)
            "$QUIDRA" "$TMP/cli.qui" >"$TMP/$mode.out" 2>"$TMP/$mode.err"
            ;;
        invalid)
            "$QUIDRA" "$TMP/cli.qui" image.jpg --count nope >"$TMP/$mode.out" 2>"$TMP/$mode.err"
            ;;
        unknown)
            "$QUIDRA" "$TMP/cli.qui" image.jpg --wat >"$TMP/$mode.out" 2>"$TMP/$mode.err"
            ;;
        duplicate)
            "$QUIDRA" "$TMP/cli.qui" image.jpg --count 2 --count 3 >"$TMP/$mode.out" 2>"$TMP/$mode.err"
            ;;
    esac
    rc=$?
    set -e
    [[ "$rc" -eq 2 ]]
    grep -q 'Quidra CLI error:' "$TMP/$mode.err"
done

cat > "$TMP/file.qui" <<QUI
auto | error written = file.write("$TMP/source.txt", "hello")
match written
    void
        print("write")
        print(NL)
    error e
        print(e)
        print(NL)

auto | error read = file.read("$TMP/source.txt")
match read
    string value
        print(value)
        print(NL)
    error e
        print(e)
        print(NL)

auto | error present = file.exists("$TMP/source.txt")
match present
    bool value
        print(value)
        print(NL)
    error e
        print(e)
        print(NL)

auto | error copied = file.copy("$TMP/source.txt", "$TMP/copied.txt")
match copied
    void
        print("copy")
        print(NL)
    error e
        print(e)
        print(NL)

auto | error moved = file.move("$TMP/copied.txt", "$TMP/moved.txt")
match moved
    void
        print("move")
        print(NL)
    error e
        print(e)
        print(NL)

auto | error removed = file.remove("$TMP/moved.txt")
match removed
    void
        print("remove")
        print(NL)
    error e
        print(e)
        print(NL)

auto | error absent = file.exists("$TMP/moved.txt")
match absent
    bool value
        print(value)
        print(NL)
    error e
        print(e)
        print(NL)

auto | error directory = file.mkdir("$TMP/new-directory")
match directory
    void
        print("mkdir")
        print(NL)
    error e
        print(e)
        print(NL)
QUI

[[ "$("$QUIDRA" "$TMP/file.qui")" == $'write\nhello\ntrue\ncopy\nmove\nremove\nfalse\nmkdir' ]]
[[ "$(cat "$TMP/source.txt")" == "hello" ]]
[[ -d "$TMP/new-directory" ]]

cat > "$TMP/file-handle.qui" <<QUI
auto | error opened = file.open("$TMP/source.txt")
match opened
    file.Handle handle
        auto | error content = handle.read()
        match content
            string value
                print(value)
                print(NL)
            error problem
                print("read-error")
                print(NL)
        handle.close()
        auto | error closed = handle.read()
        match closed
            string value
                print("unexpected")
                print(NL)
            error problem
                print("closed")
                print(NL)
    error problem
        print("open-error")
        print(NL)
QUI
file_handle_expected=$(printf 'hello\nclosed')
[[ "$("$QUIDRA" "$TMP/file-handle.qui")" == "$file_handle_expected" ]]

cat > "$TMP/file-handle-copy.qui" <<QUI
auto | error opened = file.open("$TMP/source.txt")
match opened
    file.Handle first
        file.Handle second = first
        first.close()
        auto | error content = second.read()
        match content
            string value
                print(value)
                print(NL)
            error problem
                print("copy-error")
                print(NL)
    error problem
        print("open-error")
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/file-handle-copy.qui")" == "hello" ]]

printf 'old-resource' > "$TMP/resource-source.txt"
cat > "$TMP/file-handle-resource-identity.qui" <<QUI
auto | error opened = file.open("$TMP/resource-source.txt")
match opened
    file.Handle first
        auto | error moved = file.move("$TMP/resource-source.txt", "$TMP/resource-original.txt")
        match moved
            void
                auto | error written = file.write("$TMP/resource-source.txt", "new-resource")
                match written
                    void
                        file.Handle second = first
                        auto | error a = first.read()
                        auto | error b = second.read()
                        match a
                            string left
                                match b
                                    string right
                                        print(left)
                                        print(NL)
                                        print(right)
                                        print(NL)
                                    error problem
                                        print("copy-read-error")
                                        print(NL)
                            error problem
                                print("first-read-error")
                                print(NL)
                    error problem
                        print("write-error")
                        print(NL)
            error problem
                print("move-error")
                print(NL)
    error problem
        print("open-error")
        print(NL)
QUI
file_resource_identity_expected=$(printf 'old-resource\nold-resource')
[[ "$("$QUIDRA" "$TMP/file-handle-resource-identity.qui")" == "$file_resource_identity_expected" ]]

cat > "$TMP/file-handle-closed-copy.qui" <<QUI
auto | error opened = file.open("$TMP/source.txt")
match opened
    file.Handle first
        first.close()
        auto | error removed = file.remove("$TMP/source.txt")
        match removed
            void
                file.Handle second = first
                auto | error content = second.read()
                match content
                    string value
                        print("unexpected")
                        print(NL)
                    error problem
                        print("closed-copy")
                        print(NL)
            error problem
                print("remove-error")
                print(NL)
    error problem
        print("open-error")
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/file-handle-closed-copy.qui")" == "closed-copy" ]]

cat > "$TMP/file-handle-write.qui" <<QUI
auto | error created = file.create("$TMP/incremental.txt")
match created
    file.Handle handle
        match handle.write("alpha")
            void
                match handle.write_line(" beta")
                    void
                        match handle.flush()
                            void
                                match handle.seek(0)
                                    void
                                        match handle.read()
                                            string content
                                                print(content)
                                                print(NL)
                                            error problem
                                                print("read-error")
                                                print(NL)
                                    error problem
                                        print("seek-error")
                                        print(NL)
                            error problem
                                print("flush-error")
                                print(NL)
                    error problem
                        print("line-error")
                        print(NL)
            error problem
                print("write-error")
                print(NL)
    error problem
        print("create-error")
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/file-handle-write.qui")" == "alpha beta" ]]

cat > "$TMP/file-handle-read-line.qui" <<QUI
auto | error opened = file.open("$TMP/incremental.txt")
match opened
    file.Handle handle
        auto | error first = handle.read_line()
        match first
            string line
                print(line)
                print(NL)
            none
                print("unexpected-eof")
                print(NL)
            error problem
                print("read-line-error")
                print(NL)
        auto | error eof = handle.read_line()
        match eof
            string line
                print("unexpected-line")
                print(NL)
            none
                print("eof")
                print(NL)
            error problem
                print("read-line-error")
                print(NL)
    error problem
        print("open-error")
        print(NL)
QUI
file_read_line_expected=$(printf 'alpha beta\neof')
[[ "$("$QUIDRA" "$TMP/file-handle-read-line.qui")" == "$file_read_line_expected" ]]

printf 'one\ntwo\nthree\n' > "$TMP/interleaved-lines.txt"
cat > "$TMP/file-handle-interleaved-copy.qui" <<QUI
auto | error opened = file.open("$TMP/interleaved-lines.txt")
match opened
    file.Handle first
        file.Handle second = first
        match first.read_line()
            string line
                print(line)
                print(NL)
            none
                print("unexpected-eof")
                print(NL)
            error problem
                print("read-error")
                print(NL)
        match first.read_line()
            string line
                print(line)
                print(NL)
            none
                print("unexpected-eof")
                print(NL)
            error problem
                print("read-error")
                print(NL)
        match second.read_line()
            string line
                print(line)
                print(NL)
            none
                print("unexpected-eof")
                print(NL)
            error problem
                print("read-error")
                print(NL)
        match first.read_line()
            string line
                print(line)
                print(NL)
            none
                print("unexpected-eof")
                print(NL)
            error problem
                print("read-error")
                print(NL)
        match second.read_line()
            string line
                print(line)
                print(NL)
            none
                print("unexpected-eof")
                print(NL)
            error problem
                print("read-error")
                print(NL)
    error problem
        print("open-error")
        print(NL)
QUI
file_interleaved_copy_expected=$(printf 'one\ntwo\none\nthree\ntwo')
[[ "$("$QUIDRA" "$TMP/file-handle-interleaved-copy.qui")" == "$file_interleaved_copy_expected" ]]

printf 'last' > "$TMP/line-no-newline.txt"
printf '\377\n' > "$TMP/line-invalid-utf8.bin"
cat > "$TMP/file-handle-read-line-edge.qui" <<QUI
auto | error tail_opened = file.open("$TMP/line-no-newline.txt")
match tail_opened
    file.Handle handle
        match handle.read_line()
            string line
                print(line)
                print(NL)
            none
                print("unexpected-eof")
                print(NL)
            error problem
                print("read-line-error")
                print(NL)
        match handle.read_line()
            string line
                print("unexpected-line")
                print(NL)
            none
                print("eof")
                print(NL)
            error problem
                print("read-line-error")
                print(NL)
    error problem
        print("open-error")
        print(NL)

auto | error invalid_opened = file.open("$TMP/line-invalid-utf8.bin")
match invalid_opened
    file.Handle handle
        match handle.read_line()
            string line
                print("unexpected-text")
                print(NL)
            none
                print("unexpected-eof")
                print(NL)
            error problem
                print("invalid")
                print(NL)
    error problem
        print("open-error")
        print(NL)
QUI
file_read_line_edge_expected=$(printf 'last\neof\ninvalid')
[[ "$("$QUIDRA" "$TMP/file-handle-read-line-edge.qui")" == "$file_read_line_edge_expected" ]]

cat > "$TMP/file-handle-append.qui" <<QUI
auto | error appended = file.append("$TMP/incremental.txt")
match appended
    file.Handle handle
        match handle.write_line("tail")
            void
                match handle.flush()
                    void
                        handle.close()
                    error problem
                        print("flush-error")
                        print(NL)
            error problem
                print("append-error")
                print(NL)
    error problem
        print("open-error")
        print(NL)
auto | error loaded = file.read("$TMP/incremental.txt")
match loaded
    string content
        print(content)
        print(NL)
    error problem
        print("read-error")
        print(NL)
QUI
file_append_expected=$(printf 'alpha beta\ntail')
[[ "$("$QUIDRA" "$TMP/file-handle-append.qui")" == "$file_append_expected" ]]

printf 'hello' > "$TMP/source.txt"

cat > "$TMP/file-handle-fail-fast.qui" <<QUI
file.Handle handle = file.open("$TMP/does-not-exist.txt")
print("unreachable")
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/file-handle-fail-fast.qui" >"$TMP/file-handle-fail-fast.out" 2>&1
file_handle_fail_fast_rc=$?
set -e
[[ "$file_handle_fail_fast_rc" -eq 101 ]]
grep -q 'UNHANDLED_ERROR' "$TMP/file-handle-fail-fast.out"
grep -q 'file operation failed' "$TMP/file-handle-fail-fast.out"

cat > "$TMP/file-handle-auto-close.qui" <<QUI
void | error open_and_return(string path)
    file.Handle handle = try file.open(path)
    return

int i = 0
while i < 256
    auto | error result = open_and_return("$TMP/source.txt")
    match result
        void
            void
        error problem
            print("open-error")
            print(NL)
    i += 1
print("released")
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/file-handle-auto-close.qui")" == "released" ]]

cat > "$TMP/bin-string.qui" <<'QUI'
bin allocated = bin.fill(5, 1)
print(len(allocated))
print(NL)
print(allocated)
print(NL)

auto | error parsed_result = bin.parse("01010000")
match parsed_result
    bin parsed
        print(parsed[0])
        print(NL)
        print(parsed[1])
        print(NL)
    error problem
        print("unexpected parse error")
        print(NL)

int8 signed = -1
bin packed = bin(signed)
print(packed)
print(NL)
int8 restored = int8(packed)
print(restored)
print(NL)

auto | error flag_result = bin.parse("1")
match flag_result
    bin bit
        bool flag = bool(bit)
        print(flag)
        print(NL)
    error problem
        print("unexpected parse error")
        print(NL)

string repeated = string.repeat("a", 3)
print(repeated)
print(NL)

string original = "héllo"
bin encoded = original.utf8()
match string.from_utf8(encoded)
    string decoded
        print(decoded)
        print(NL)
    error problem
        print("unexpected valid UTF-8 error")
        print(NL)

uint8[] invalid_bytes = [255]
bin invalid = bin(invalid_bytes)
match string.from_utf8(invalid)
    string decoded
        print("unexpected invalid UTF-8 success")
        print(NL)
    error problem
        print("invalid")
        print(NL)

uint8[] nul_bytes = [97, 0, 98]
bin with_nul = bin(nul_bytes)
match string.from_utf8(with_nul)
    string decoded
        print("unexpected NUL text success")
        print(NL)
    error problem
        print("nul")
        print(NL)

bin partial = bin.fill(7, 0)
match string.from_utf8(partial)
    string decoded
        print("unexpected partial-byte success")
        print(NL)
    error problem
        print("partial")
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/bin-string.qui")" == $'5\n11111\n0\n1\n11111111\n-1\ntrue\naaa\nhéllo\ninvalid\nnul\npartial' ]]

cat > "$TMP/bin-parse-error.qui" <<'QUI'
string invalid_text = "012"
auto | error parsed = bin.parse(invalid_text)
match parsed
    bin value
        print("unexpected")
        print(NL)
    error problem
        print("error")
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/bin-parse-error.qui")" == "error" ]]

python3 - "$TMP/source.bin" <<'PY'
import sys
open(sys.argv[1], "wb").write(bytes([0, 255, 65, 10, 128]))
PY

cat > "$TMP/file-handle-bin.qui" <<QUI
auto | error opened = file.open("$TMP/source.bin")
match opened
    file.Handle handle
        auto | error raw = handle.read_bin()
        match raw
            bin value
                print(len(value))
                print(NL)
                uint8[] bytes = uint8[](value)
                print(bytes[1])
                print(NL)
            error problem
                print("read-error")
                print(NL)
    error problem
        print("open-error")
        print(NL)
QUI
file_handle_bin_expected=$(printf '40\n255')
[[ "$("$QUIDRA" "$TMP/file-handle-bin.qui")" == "$file_handle_bin_expected" ]]

cat > "$TMP/file-bin.qui" <<QUI
auto | error raw = file.read_bin("$TMP/source.bin")
match raw
    bin value
        print(len(value))
        print(NL)
        uint8[] values = uint8[](value)
        print(values[0])
        print(NL)
        print(values[1])
        print(NL)
        values[2] = 66
        bin changed = bin(values)
        auto | error saved = file.write_bin("$TMP/copied.bin", changed)
        match saved
            void
                print("bin")
                print(NL)
            error problem
                print(problem)
                print(NL)
    error problem
        print(problem)
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/file-bin.qui")" == $'40\n0\n255\nbin' ]]
python3 - "$TMP/copied.bin" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
assert data == bytes([0, 255, 66, 10, 128]), data
PY

cat > "$TMP/file-bin-unaligned.qui" <<QUI
bin value = bin.fill(3, 1)
auto | error saved = file.write_bin("$TMP/unaligned.bin", value)
match saved
    void
        print("unexpected")
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/file-bin-unaligned.qui")" == "file operation failed" ]]
[[ ! -e "$TMP/unaligned.bin" ]]

printf 'b' > "$TMP/new-directory/b.txt"
printf 'a' > "$TMP/new-directory/a.txt"
cat > "$TMP/file-list.qui" <<QUI
auto | error listed = file.list("$TMP/new-directory")
match listed
    string[] entries
        print(len(entries))
        print(NL)
        print(entries[0].ends_with("/a.txt"))
        print(NL)
        print(entries[1].ends_with("/b.txt"))
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/file-list.qui")" == $'2\ntrue\ntrue' ]]

cat > "$TMP/file-list-missing.qui" <<QUI
auto | error listed = file.list("$TMP/no-such-directory")
match listed
    string[] entries
        print(len(entries))
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/file-list-missing.qui")" == "file operation failed" ]]

cat > "$TMP/missing-file.qui" <<QUI
auto | error read = file.read("$TMP/does-not-exist.txt")
match read
    string value
        print(value)
        print(NL)
    error e
        print(e)
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/missing-file.qui")" == "file operation failed" ]]

python3 - "$TMP/invalid-utf8.txt" <<'PY'
import sys
open(sys.argv[1], "wb").write(b"\xc0\xaf")
PY
cat > "$TMP/invalid-utf8-file.qui" <<QUI
auto | error read = file.read("$TMP/invalid-utf8.txt")
match read
    string value
        print("unexpected")
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/invalid-utf8-file.qui")" == "file operation failed" ]]

python3 - "$TMP/nul-text.txt" <<'PY'
import sys
open(sys.argv[1], "wb").write(b"A\x00B")
PY
cat > "$TMP/nul-text-file.qui" <<QUI
auto | error read = file.read("$TMP/nul-text.txt")
match read
    string value
        print("unexpected")
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/nul-text-file.qui")" == "file operation failed" ]]

cat > "$TMP/local-file.qui" <<'QUI'
int answer()
    return 42
QUI
cat > "$TMP/local-import.qui" <<'QUI'
import local = "./local-file.qui"
print(local.answer())
print(NL)
QUI
(
  cd "$TMP"
  [[ "$("$QUIDRA" local-import.qui)" == "42" ]]
)

cat > "$TMP/not-standard.qui" <<'QUI'
int answer()
    return 99
QUI
cat > "$TMP/bare-local-import.qui" <<'QUI'
import not_standard
print("unreachable")
print(NL)
QUI
set +e
(
  cd "$TMP"
  "$QUIDRA" check bare-local-import.qui --json
) >"$TMP/bare-local.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'PACKAGE_NOT_INSTALLED' "$TMP/bare-local.json"

cat > "$TMP/environment.qui" <<'QUI'
auto configured = environment.get("QUIDRA_TEST_ENV")
match configured
    string value
        print(value)
        print(NL)
    none
        print("missing")
        print(NL)

print(environment.has("QUIDRA_TEST_ENV"))
print(NL)

auto absent = environment.get("QUIDRA_TEST_ENV_DEFINITELY_MISSING")
match absent
    string value
        print(value)
        print(NL)
    none
        print("none")
        print(NL)

print(environment.has("QUIDRA_TEST_ENV_DEFINITELY_MISSING"))
print(NL)
QUI

environment_output="$(QUIDRA_TEST_ENV=hello "$QUIDRA" "$TMP/environment.qui")"
environment_expected="$(printf 'hello\ntrue\nnone\nfalse')"
[[ "$environment_output" == "$environment_expected" ]]

python3 - "$QUIDRA" "$TMP/environment.qui" <<'PY'
import os
import subprocess
import sys

env = os.environb.copy()
env[b"QUIDRA_TEST_ENV"] = b"\xff"
result = subprocess.run(
    [os.fsencode(sys.argv[1]), os.fsencode(sys.argv[2])],
    env=env,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
if result.returncode != 101:
    raise SystemExit(f"invalid UTF-8 environment status: {result.returncode}")
if b"environment value must be valid UTF-8 text without NUL" not in result.stderr:
    raise SystemExit(f"missing environment text diagnostic: {result.stderr!r}")
PY

cat > "$TMP/test-module.qui" <<'QUI'
test.check(true)
test.equal(int(2) + 3, 5)
test.equal("Quidra", "Quidra")
int[] actual = [1, 2, 3]
int[] expected = [1, 2, 3]
test.equal(actual, expected)
print("test-ok")
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/test-module.qui")" == "test-ok" ]]

cat > "$TMP/test-failure.qui" <<'QUI'
test.check(false)
print("unreachable")
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/test-failure.qui" >"$TMP/test-failure.out" 2>"$TMP/test-failure.err"
test_failure_rc=$?
set -e
[[ "$test_failure_rc" -eq 1 ]]
grep -q 'Quidra test assertion failed' "$TMP/test-failure.err"
"$QUIDRA" inspect "$TMP/test-failure.qui" >"$TMP/test-failure.inspect.json"
python3 - "$TMP/test-failure.inspect.json" "$TMP/test-failure.err" <<'PY'
import json, pathlib, sys
inspection = json.load(open(sys.argv[1]))
failure = pathlib.Path(sys.argv[2]).read_text()
nodes = [
    node for node in inspection["nodes"]
    if node["span"]["start"]["line"] == 1 and node["kind"] == "expression_statement"
]
assert len(nodes) == 1, nodes
node = nodes[0]
assert f"source_revision={inspection['revision']}" in failure, failure
assert f"node_id={node['node_id']}" in failure, failure
assert f"node_kind={node['kind']}" in failure, failure
PY
[[ ! -s "$TMP/test-failure.out" ]]

cat > "$TMP/time.qui" <<'QUI'
time.Instant start = time.now()
time.Duration pause = time.seconds(0.001)
time.sleep(pause)
time.Duration elapsed = time.since(start)
print(elapsed.seconds() >= 0.0)
print(NL)

time.Instant explicit_async_start = time.now(sync = false)
time.Duration explicit_async_elapsed = time.since(explicit_async_start, sync = false)
print(explicit_async_elapsed.seconds() >= 0.0)
print(NL)

time.Instant synchronized_start = time.now(sync = true)
time.sleep(pause)
time.Duration synchronized_elapsed = time.since(synchronized_start, sync = true)
print(synchronized_elapsed.seconds() >= 0.0)
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/time.qui")" == "$(printf 'true\ntrue\ntrue')" ]]

cat > "$TMP/time-sync-must-be-named.qui" <<'QUI'
time.Instant invalid = time.now(true)
QUI
set +e
"$QUIDRA" check "$TMP/time-sync-must-be-named.qui" --json >"$TMP/time-sync-must-be-named.json"
time_sync_named_rc=$?
set -e
[[ "$time_sync_named_rc" -eq 1 ]]
grep -q 'time.now accepts only optional sync = bool' "$TMP/time-sync-must-be-named.json"


cat > "$TMP/time-now-sync-must-be-bool.qui" <<'QUI'
time.Instant invalid = time.now(sync = 1)
QUI
set +e
"$QUIDRA" check "$TMP/time-now-sync-must-be-bool.qui" --json >"$TMP/time-now-sync-must-be-bool.json"
time_now_sync_bool_rc=$?
set -e
[[ "$time_now_sync_bool_rc" -eq 1 ]]

cat > "$TMP/time-since-sync-must-be-bool.qui" <<'QUI'
time.Instant start = time.now()
time.Duration invalid = time.since(start, sync = 1)
QUI
set +e
"$QUIDRA" check "$TMP/time-since-sync-must-be-bool.qui" --json >"$TMP/time-since-sync-must-be-bool.json"
time_since_sync_bool_rc=$?
set -e
[[ "$time_since_sync_bool_rc" -eq 1 ]]

cat > "$TMP/gpu-sync-check.qui" <<'QUI'
gpu.sync(0)
QUI
"$QUIDRA" check "$TMP/gpu-sync-check.qui" >/dev/null

cat > "$TMP/gpu-sync-negative.qui" <<'QUI'
gpu.sync(-1)
QUI
set +e
"$QUIDRA" check "$TMP/gpu-sync-negative.qui" --json >"$TMP/gpu-sync-negative.json"
gpu_sync_negative_rc=$?
set -e
[[ "$gpu_sync_negative_rc" -eq 1 ]]
grep -q 'gpu.sync(index) requires a non-negative GPU index' "$TMP/gpu-sync-negative.json"

cat > "$TMP/time-direct-construction.qui" <<'QUI'
time.Instant impossible = time.Instant()
QUI
set +e
"$QUIDRA" check "$TMP/time-direct-construction.qui" --json >"$TMP/time-direct-construction.json"
time_direct_rc=$?
set -e
[[ "$time_direct_rc" -eq 1 ]]
grep -q 'Standard library value types cannot be constructed directly' "$TMP/time-direct-construction.json"

cat > "$TMP/random.qui" <<'QUI'
random.Generator a = random.generator(seed = 42)
random.Generator b = random.generator(seed = 42)
print(a.int(1, 10) == b.int(1, 10))
print(NL)
print(a.float() == b.float())
print(NL)
print(a.bool() == b.bool())
print(NL)

random.Generator original = random.generator(seed = 7)
random.Generator copy = original
print(original.int(-1000, 1000) == copy.int(-1000, 1000))
print(NL)
QUI
random_output="$("$QUIDRA" "$TMP/random.qui")"
random_expected="$(printf 'true\ntrue\ntrue\ntrue')"
[[ "$random_output" == "$random_expected" ]]

cat > "$TMP/random-invalid.qui" <<'QUI'
random.Generator rng = random.generator(seed = 1)
print(rng.int(5, 5))
print(NL)
QUI
set +e
ASAN_OPTIONS=detect_leaks=0 "$QUIDRA" "$TMP/random-invalid.qui" >"$TMP/random-invalid.out" 2>"$TMP/random-invalid.err"
random_invalid_rc=$?
set -e
[[ "$random_invalid_rc" -eq 101 ]]
grep -q 'invalid random range' "$TMP/random-invalid.err"

cat > "$TMP/process.qui" <<'QUI'
process.Result completed = process.run("/bin/sh", ["-c", "printf out; printf err >&2; exit 3"])
print(completed.started)
print(NL)
print(completed.status)
print(NL)
print(completed.output)
print(NL)
print(completed.error)
print(NL)

process.Result missing = process.run("/definitely/not/a/real/quidra-program", [])
print(missing.started)
print(NL)
print(missing.status)
print(NL)
print(missing.error != "")
print(NL)
QUI
process_output="$("$QUIDRA" "$TMP/process.qui")"
process_expected="$(printf 'true\n3\nout\nerr\nfalse\n-1\ntrue')"
[[ "$process_output" == "$process_expected" ]]

cat > "$TMP/process-shell.qui" <<'QUI'
process.Result result = process.shell("printf shell-out; printf shell-err >&2; exit 7")
print(result.started)
print(NL)
print(result.status)
print(NL)
print(result.output)
print(NL)
print(result.error)
print(NL)

process.Result braces = process.shell("printf '{{}}'")
print(braces.output)
print(NL)
QUI
process_shell_output="$("$QUIDRA" "$TMP/process-shell.qui")"
process_shell_expected="$(printf 'true\n7\nshell-out\nshell-err\n{}')"
[[ "$process_shell_output" == "$process_shell_expected" ]]

cat > "$TMP/process-invalid-text.qui" <<'QUI'
process.Result result = process.run("/bin/sh", ["-c", "printf '\377'"])
print(result.output)
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/process-invalid-text.qui" >"$TMP/process-invalid-text.out" 2>"$TMP/process-invalid-text.err"
process_invalid_text_rc=$?
set -e
[[ "$process_invalid_text_rc" -eq 101 ]]
grep -q 'process stdout/stderr must be valid UTF-8 text without NUL' "$TMP/process-invalid-text.err"

cat > "$TMP/process-nul-text.qui" <<'QUI'
process.Result result = process.run("/bin/sh", ["-c", "printf 'A\000B'"])
print(result.output)
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/process-nul-text.qui" >"$TMP/process-nul-text.out" 2>"$TMP/process-nul-text.err"
process_nul_text_rc=$?
set -e
[[ "$process_nul_text_rc" -eq 101 ]]
grep -q 'process stdout/stderr must be valid UTF-8 text without NUL' "$TMP/process-nul-text.err"

cat > "$TMP/process-exit.qui" <<'QUI'
process.exit(7)
QUI
set +e
"$QUIDRA" "$TMP/process-exit.qui" >"$TMP/process-exit.out" 2>"$TMP/process-exit.err"
process_exit_rc=$?
set -e
if [[ "$process_exit_rc" -ne 7 ]]; then
    echo "process.exit regression: expected status 7, got $process_exit_rc" >&2
    cat "$TMP/process-exit.err" >&2
    exit 1
fi
[[ ! -s "$TMP/process-exit.out" ]]
[[ ! -s "$TMP/process-exit.err" ]]

"$QUIDRA" build "$TMP/process-exit.qui" -o "$TMP/process-exit-bin"
set +e
"$TMP/process-exit-bin" >"$TMP/process-exit-bin.out" 2>"$TMP/process-exit-bin.err"
process_exit_bin_rc=$?
set -e
if [[ "$process_exit_bin_rc" -ne 7 ]]; then
    echo "native process.exit regression: expected status 7, got $process_exit_bin_rc" >&2
    cat "$TMP/process-exit-bin.err" >&2
    exit 1
fi
[[ ! -s "$TMP/process-exit-bin.out" ]]
[[ ! -s "$TMP/process-exit-bin.err" ]]

cat > "$TMP/process-direct-construction.qui" <<'QUI'
process.Result impossible = process.Result()
QUI
set +e
"$QUIDRA" check "$TMP/process-direct-construction.qui" --json >"$TMP/process-direct-construction.json"
process_direct_rc=$?
set -e
[[ "$process_direct_rc" -eq 1 ]]
grep -q 'Standard library value types cannot be constructed directly' "$TMP/process-direct-construction.json"

cat > "$TMP/map.qui" <<'QUI'
map.Map<string, int> counts = map.Map<string, int>()
print(counts.size())
print(NL)
print(counts.has("apple"))
print(NL)

auto missing = counts.get("apple")
match missing
    int value
        print(value)
        print(NL)
    none
        print("none")
        print(NL)

counts.set("apple", 2)
counts.set("banana", 1)
counts.set("apple", 3)
print(counts.size())
print(NL)

auto apple = counts.get("apple")
match apple
    int value
        print(value)
        print(NL)
    none
        print("missing")
        print(NL)

string[] keys = counts.keys()
int[] values = counts.values()
print(keys[0])
print(NL)
print(keys[1])
print(NL)
print(values[0])
print(NL)
print(values[1])
print(NL)

map.Map<string, int> copied = counts
copied.set("cherry", 4)
print(counts.has("cherry"))
print(NL)
print(copied.has("cherry"))
print(NL)
QUI
map_output="$("$QUIDRA" "$TMP/map.qui")"
map_expected="$(printf '0\nfalse\nnone\n2\n3\napple\nbanana\n3\n1\nfalse\ntrue')"
[[ "$map_output" == "$map_expected" ]]

cat > "$TMP/map-get-set-cache.qui" <<'QUI'
map.Map<int, int> values = map.Map<int, int>()
values.set(1, 10)

auto present = values.get(1)
match present
    int value
        values.set(1, value + 5)
    none
        process.exit(1)

auto absent = values.get(2)
match absent
    int value
        process.exit(2)
    none
        values.set(2, 20)

print(values.size())
print(NL)
auto one = values.get(1)
match one
    int value
        print(value)
        print(NL)
    none
        process.exit(3)
auto two = values.get(2)
match two
    int value
        print(value)
        print(NL)
    none
        process.exit(4)
QUI
map_cache_output="$("$QUIDRA" "$TMP/map-get-set-cache.qui")"
map_cache_expected="$(printf '2\n15\n20')"
[[ "$map_cache_output" == "$map_cache_expected" ]]

cat > "$TMP/array-append-cache.qui" <<'QUI'
int[] values = []
for i in range(0, 10000)
    values = values.append(i)
int[] copied = values
values = values.append(10000)
print(len(values))
print(NL)
print(len(copied))
print(NL)
print(values[10000])
print(NL)
print(copied[9999])
print(NL)
QUI
array_append_cache_output="$("$QUIDRA" "$TMP/array-append-cache.qui")"
array_append_cache_expected="$(printf '10001\n10000\n10000\n9999')"
[[ "$array_append_cache_output" == "$array_append_cache_expected" ]]

cat > "$TMP/map-remove.qui" <<'QUI'
map.Map<string, int> values = map.Map<string, int>()
values.set("a", 1)
values.set("i", 2)
values.set("q", 3)
values.set("z", 4)
print(values.remove("i"))
print(NL)
print(values.remove("i"))
print(NL)
print(values.size())
print(NL)
print(values.has("i"))
print(NL)
auto q = values.get("q")
match q
    int value
        print(value)
        print(NL)
    none
        print(int(-1))
        print(NL)
string[] remaining_keys = values.keys()
int[] remaining_values = values.values()
for key in remaining_keys
    print(key)
    print(NL)
for value in remaining_values
    print(value)
    print(NL)
values.set("i", 5)
string[] reinserted_keys = values.keys()
print(reinserted_keys[len(reinserted_keys) - 1])
print(NL)
map.Map<string, int> copied_values = values
print(copied_values.remove("q"))
print(NL)
print(values.has("q"))
print(NL)
print(copied_values.has("q"))
print(NL)
QUI
map_remove_output="$("$QUIDRA" "$TMP/map-remove.qui")"
map_remove_expected="$(printf 'true\nfalse\n3\nfalse\n3\na\nq\nz\n1\n3\n4\ni\ntrue\ntrue\nfalse')"
[[ "$map_remove_output" == "$map_remove_expected" ]]

cat > "$TMP/set.qui" <<'QUI'
set.Set<string> tags = set.Set<string>()
tags.add("compiler")
tags.add("ai")
tags.add("compiler")
print(tags.size())
print(NL)
print(tags.has("compiler"))
print(NL)
print(tags.has("other"))
print(NL)
string[] values = tags.values()
print(values[0])
print(NL)
print(values[1])
print(NL)

set.Set<string> copied = tags
copied.add("new")
print(tags.has("new"))
print(NL)
print(copied.has("new"))
print(NL)
QUI
set_output="$("$QUIDRA" "$TMP/set.qui")"
set_expected="$(printf '2\ntrue\nfalse\ncompiler\nai\nfalse\ntrue')"
[[ "$set_output" == "$set_expected" ]]

cat > "$TMP/set-remove.qui" <<'QUI'
set.Set<string> values = set.Set<string>()
values.add("a")
values.add("i")
values.add("q")
values.add("z")
print(values.remove("i"))
print(NL)
print(values.remove("i"))
print(NL)
print(values.size())
print(NL)
print(values.has("i"))
print(NL)
string[] remaining = values.values()
for value in remaining
    print(value)
    print(NL)
values.add("i")
string[] reinserted = values.values()
print(reinserted[len(reinserted) - 1])
print(NL)
set.Set<string> copied = values
print(copied.remove("q"))
print(NL)
print(values.has("q"))
print(NL)
print(copied.has("q"))
print(NL)
QUI
set_remove_output="$("$QUIDRA" "$TMP/set-remove.qui")"
set_remove_expected="$(printf 'true\nfalse\n3\nfalse\na\nq\nz\ni\ntrue\ntrue\nfalse')"
[[ "$set_remove_output" == "$set_remove_expected" ]]

cat > "$TMP/hash-collection-tombstones.qui" <<'QUI'
map.Map<int, int> values = map.Map<int, int>()
set.Set<int> unique = set.Set<int>()
for i in range(0, 32)
    values.set(i, i)
    unique.add(i)
    if not values.remove(i)
        process.exit(1)
    if not unique.remove(i)
        process.exit(1)
values.set(1000, 7)
unique.add(1000)
print(values.size())
print(NL)
print(unique.size())
print(NL)
print(values.has(1000))
print(NL)
print(unique.has(1000))
print(NL)
QUI
tombstone_output="$("$QUIDRA" "$TMP/hash-collection-tombstones.qui")"
tombstone_expected="$(printf '1\n1\ntrue\ntrue')"
[[ "$tombstone_output" == "$tombstone_expected" ]]

cat > "$TMP/hash-collections.qui" <<'QUI'
map.Map<string, int> many = map.Map<string, int>()
for i in range(0, 100)
    many.set("k{i}", i * 3)
print(many.size())
print(NL)
auto found = many.get("k73")
match found
    int value
        print(value)
        print(NL)
    none
        print(int(-1))
        print(NL)
many.set("k73", 999)
auto replaced = many.get("k73")
match replaced
    int value
        print(value)
        print(NL)
    none
        print(int(-1))
        print(NL)
string[] ordered_keys = many.keys()
print(ordered_keys[0])
print(NL)
print(ordered_keys[99])
print(NL)
print(many.has("missing"))
print(NL)

map.Map<int, string> numbers = map.Map<int, string>()
numbers.set(-7, "negative")
numbers.set(42, "answer")
auto number_value = numbers.get(42)
match number_value
    string value
        print(value)
        print(NL)
    none
        print("missing")
        print(NL)

set.Set<string> unique = set.Set<string>()
for i in range(0, 100)
    unique.add("v{i}")
for i in range(0, 100)
    unique.add("v{i}")
print(unique.size())
print(NL)
print(unique.has("v73"))
print(NL)
print(unique.has("absent"))
print(NL)
string[] ordered_values = unique.values()
print(ordered_values[0])
print(NL)
print(ordered_values[99])
print(NL)
QUI
hash_collections_output="$("$QUIDRA" "$TMP/hash-collections.qui")"
hash_collections_expected="$(printf '100\n219\n999\nk0\nk99\nfalse\nanswer\n100\ntrue\nfalse\nv0\nv99')"
[[ "$hash_collections_output" == "$hash_collections_expected" ]]
"$QUIDRA" llvm "$TMP/hash-collections.qui" > "$TMP/hash-collections.ll"
grep -q 'append.field.move' "$TMP/hash-collections.ll"
grep -q '@quidra_array_grow_move' "$TMP/hash-collections.ll"

# Cross many rehash thresholds and verify lookup, replacement, insertion order,
# set uniqueness, and value-copy isolation at a scale large enough to catch
# accidental quadratic regressions in generated collection code.
cat > "$TMP/hash-collections-stress.qui" <<'QUI'
map.Map<int, int> large = map.Map<int, int>()
for i in range(0, 5000)
    large.set(i, i * 2)
print(large.size())
print(NL)
auto middle = large.get(4097)
match middle
    int value
        print(value)
        print(NL)
    none
        print(int(-1))
        print(NL)
large.set(4097, 123456)
auto replaced_large = large.get(4097)
match replaced_large
    int value
        print(value)
        print(NL)
    none
        print(int(-1))
        print(NL)
int[] large_keys = large.keys()
print(large_keys[0])
print(NL)
print(large_keys[4999])
print(NL)
print(large.has(6000))
print(NL)

map.Map<int, int> copied_large = large
copied_large.set(6000, 42)
print(large.has(6000))
print(NL)
print(copied_large.has(6000))
print(NL)

set.Set<int> large_set = set.Set<int>()
for i in range(0, 5000)
    large_set.add(i)
for i in range(0, 5000)
    large_set.add(i)
print(large_set.size())
print(NL)
print(large_set.has(4097))
print(NL)
print(large_set.has(6000))
print(NL)
int[] large_values = large_set.values()
print(large_values[0])
print(NL)
print(large_values[4999])
print(NL)
QUI
hash_stress_output="$("$QUIDRA" "$TMP/hash-collections-stress.qui")"
hash_stress_expected="$(printf '5000\n8194\n123456\n0\n4999\nfalse\nfalse\ntrue\n5000\ntrue\nfalse\n0\n4999')"
[[ "$hash_stress_output" == "$hash_stress_expected" ]]

cat > "$TMP/hash-collection-removal-stress.qui" <<'QUI'
map.Map<int, int> values = map.Map<int, int>()
set.Set<int> unique = set.Set<int>()
for i in range(0, 5000)
    values.set(i, i * 3)
    unique.add(i)
for i in range(0, 3500)
    if not values.remove(i)
        print("map remove failed")
        print(NL)
        process.exit(1)
    if not unique.remove(i)
        print("set remove failed")
        print(NL)
        process.exit(1)
print(values.size())
print(NL)
print(unique.size())
print(NL)
print(values.has(3499))
print(NL)
print(values.has(3500))
print(NL)
print(unique.has(3499))
print(NL)
print(unique.has(3500))
print(NL)
int[] keys = values.keys()
int[] set_values = unique.values()
print(keys[0])
print(NL)
print(keys[len(keys) - 1])
print(NL)
print(set_values[0])
print(NL)
print(set_values[len(set_values) - 1])
print(NL)
values.set(100, 7)
unique.add(100)
int[] reinserted_keys = values.keys()
int[] reinserted_values = unique.values()
print(reinserted_keys[len(reinserted_keys) - 1])
print(NL)
print(reinserted_values[len(reinserted_values) - 1])
print(NL)
QUI
removal_stress_output="$("$QUIDRA" "$TMP/hash-collection-removal-stress.qui")"
removal_stress_expected="$(printf '1500\n1500\nfalse\ntrue\nfalse\ntrue\n3500\n4999\n3500\n4999\n100\n100')"
[[ "$removal_stress_output" == "$removal_stress_expected" ]]

# "a", "i", and "q" have the same initial bucket modulo the default capacity 8,
# so this directly exercises deterministic open-addressing collision handling.
cat > "$TMP/hash-collection-domains.qui" <<'QUI'
map.Map<string, int> collisions = map.Map<string, int>()
collisions.set("a", 1)
collisions.set("i", 2)
collisions.set("q", 3)
auto collision_a = collisions.get("a")
match collision_a
    int value
        print(value)
        print(NL)
    none
        print(int(-1))
        print(NL)
auto collision_i = collisions.get("i")
match collision_i
    int value
        print(value)
        print(NL)
    none
        print(int(-1))
        print(NL)
auto collision_q = collisions.get("q")
match collision_q
    int value
        print(value)
        print(NL)
    none
        print(int(-1))
        print(NL)
string[] collision_keys = collisions.keys()
print(collision_keys[0])
print(NL)
print(collision_keys[1])
print(NL)
print(collision_keys[2])
print(NL)

map.Map<bool, string> flags = map.Map<bool, string>()
flags.set(false, "off")
flags.set(true, "on")
auto flag_false = flags.get(false)
match flag_false
    string value
        print(value)
        print(NL)
    none
        print("missing")
        print(NL)
auto flag_true = flags.get(true)
match flag_true
    string value
        print(value)
        print(NL)
    none
        print("missing")
        print(NL)

set.Set<int> numbers_set = set.Set<int>()
numbers_set.add(-1)
numbers_set.add(42)
numbers_set.add(-1)
print(numbers_set.size())
print(NL)
print(numbers_set.has(42))
print(NL)

set.Set<bool> bool_set = set.Set<bool>()
bool_set.add(false)
bool_set.add(true)
bool_set.add(false)
print(bool_set.size())
print(NL)
bool[] bool_values = bool_set.values()
print(bool_values[0])
print(NL)
print(bool_values[1])
print(NL)
QUI
hash_domain_output="$("$QUIDRA" "$TMP/hash-collection-domains.qui")"
hash_domain_expected="$(printf '1\n2\n3\na\ni\nq\noff\non\n2\ntrue\n2\nfalse\ntrue')"
[[ "$hash_domain_output" == "$hash_domain_expected" ]]

cat > "$TMP/map-invalid-key.qui" <<'QUI'
map.Map<float, int> values = map.Map<float, int>()
QUI
set +e
"$QUIDRA" check "$TMP/map-invalid-key.qui" --json >"$TMP/map-invalid-key.json"
map_key_rc=$?
set -e
[[ "$map_key_rc" -eq 1 ]]
grep -q 'STANDARD_KEY_TYPE' "$TMP/map-invalid-key.json"

cat > "$TMP/map-hidden-field.qui" <<'QUI'
map.Map<string, int> values = map.Map<string, int>()
print(values.__keys)
print(NL)
QUI
set +e
"$QUIDRA" check "$TMP/map-hidden-field.qui" --json >"$TMP/map-hidden-field.json"
map_hidden_rc=$?
set -e
[[ "$map_hidden_rc" -eq 1 ]]
grep -q 'Standard collection internals are not source-visible' "$TMP/map-hidden-field.json"



cat > "$TMP/uninitialized-array.qui" <<'QUI'
int[] dynamic = array(3)
dynamic[1] = 7
print(dynamic[1])
print(NL)

int[2] fixed
fixed[0] = 9
print(fixed[0])
print(NL)
QUI
uninitialized_array_output="$("$QUIDRA" "$TMP/uninitialized-array.qui")"
[[ "$uninitialized_array_output" == "$(printf '7\n9')" ]]

cat > "$TMP/uninitialized-array-read.qui" <<'QUI'
int[] values = array(2)
values[0] = 1
print(values[1])
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/uninitialized-array-read.qui" >"$TMP/uninitialized-array-read.out" 2>"$TMP/uninitialized-array-read.err"
uninitialized_array_rc=$?
set -e
[[ "$uninitialized_array_rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[UNINITIALIZED\] at [0-9]+:[0-9]+: value is uninitialized' "$TMP/uninitialized-array-read.err"

cat > "$TMP/tensor.qui" <<'QUI'
tensor<float32> zeros = tensor.zeros<float32>([2, 3])
tensor<float32> ones = tensor.ones<float32>([1, 3])
tensor<float32> combined = zeros + ones
int[] combined_shape = combined.shape()
print(combined_shape[0])
print(NL)
print(combined_shape[1])
print(NL)
print(combined[1, 2].item())
print(NL)

tensor<float32> view = combined[:, 1:3]
int[] view_shape = view.shape()
print(view_shape[0])
print(NL)
print(view_shape[1])
print(NL)
view[0, 0] = 9.0
print(view[0, 0].item())
print(NL)
print(combined[0, 1].item())
print(NL)

tensor<float32> reshaped = combined.reshape([3, 2])
int[] reshaped_shape = reshaped.shape()
print(reshaped_shape[0])
print(NL)
print(reshaped_shape[1])
print(NL)

tensor<float> exact_source = tensor.ones<float>([1])
tensor<float32> exact_cast = float32(exact_source)
print(exact_cast[0].item())
print(NL)
QUI
tensor_output="$("$QUIDRA" "$TMP/tensor.qui")"
tensor_expected="$(printf '2\n3\n1.0\n2\n2\n9.0\n1.0\n3\n2\n1.0')"
[[ "$tensor_output" == "$tensor_expected" ]]


cat > "$TMP/stats-name-reuse.qui" <<'QUI'
int stats = 1
print(stats)
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/stats-name-reuse.qui")" == "1" ]]

cat > "$TMP/linear-name-reuse.qui" <<'QUI'
int linear = 1
print(linear)
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/linear-name-reuse.qui")" == "1" ]]

cat > "$TMP/tensor-rank-mismatch.qui" <<'QUI'
tensor<float32> a = tensor.ones<float32>([2, 3])
tensor<float32> b = tensor.ones<float32>([3])
tensor<float32> c = a + b
print(c.shape()[0])
print(NL)
QUI
set +e
"$QUIDRA" check "$TMP/tensor-rank-mismatch.qui" --json >"$TMP/tensor-rank-mismatch.json"
tensor_rank_rc=$?
set -e
[[ "$tensor_rank_rc" -eq 1 ]]
grep -q 'TYPE_MISMATCH' "$TMP/tensor-rank-mismatch.json"
grep -q 'identical rank' "$TMP/tensor-rank-mismatch.json"

cat > "$TMP/tensor-float-int-cast.qui" <<'QUI'
tensor<float> source = tensor.ones<float>([1]) * 1.5
tensor<int> converted = int(source)
print(converted[0].item())
print(NL)
QUI
set +e
"$QUIDRA" check "$TMP/tensor-float-int-cast.qui" --json >"$TMP/tensor-float-int-cast.json"
tensor_cast_rc=$?
set -e
[[ "$tensor_cast_rc" -eq 1 ]]
grep -qi 'floating-point to integer conversion requires' "$TMP/tensor-float-int-cast.json"

cat > "$TMP/tensor-uninitialized.qui" <<'QUI'
tensor<float32> values = tensor<float32>([2])
values[0] = 3.0
print(values[0].item())
print(NL)
print(values[1].item())
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/tensor-uninitialized.qui" >"$TMP/tensor-uninitialized.out" 2>"$TMP/tensor-uninitialized.err"
tensor_uninitialized_rc=$?
set -e
[[ "$tensor_uninitialized_rc" -eq 101 ]]
[[ "$(cat "$TMP/tensor-uninitialized.out")" == "3.0" ]]
grep -Eq 'Quidra runtime error\[UNINITIALIZED\] at [0-9]+:[0-9]+: value is uninitialized' "$TMP/tensor-uninitialized.err"


cat > "$TMP/autograd-dtype-precision.qui" <<'QUI'
tensor<float32> source32 = tensor<float32>([1])
source32[0] = 16777216.0
tensor<float32> value32 = source32.track()
tensor<float32> plus32 = value32 + float32(1)
tensor<float32> plus32_again = plus32 + float32(1)
print(plus32_again.untrack()[0].item() == float32(16777216))
print(NL)

tensor<float> source64 = tensor<float>([1])
source64[0] = 16777216.0
tensor<float> value64 = source64.track()
tensor<float> plus64 = value64 + 1.0
tensor<float> plus64_again = plus64 + 1.0
print(plus64_again.untrack()[0].item() == float(16777218))
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/autograd-dtype-precision.qui")" == "$(printf 'true\ntrue')" ]]


cat > "$TMP/json-data.json" <<'JSON'
{"name":"Quidra","items":[1,2],"nothing":null,"ok":true,"pi":3.5}
JSON

cat > "$TMP/json.qui" <<QUI
auto | error loaded = file.read("$TMP/json-data.json")
match loaded
    string source
        auto | error parsed = json.parse(source)
        match parsed
            json.Value root
                print(root.kind())
                print(NL)
                auto | error size = root.size()
                match size
                    int value
                        print(value)
                        print(NL)
                    error problem
                        print(problem)
                        print(NL)

                auto | error name = root.get("name")
                match name
                    json.Value value
                        auto | error text = value.text()
                        match text
                            string content
                                print(content)
                                print(NL)
                            error problem
                                print(problem)
                                print(NL)
                    none
                        print("missing-name")
                        print(NL)
                    error problem
                        print(problem)
                        print(NL)

                auto | error missing = root.get("missing")
                match missing
                    json.Value value
                        print(value.kind())
                        print(NL)
                    none
                        print("none")
                        print(NL)
                    error problem
                        print(problem)
                        print(NL)

                auto | error nothing = root.get("nothing")
                match nothing
                    json.Value value
                        print(value.kind())
                        print(NL)
                    none
                        print("missing-null")
                        print(NL)
                    error problem
                        print(problem)
                        print(NL)

                auto | error items = root.get("items")
                match items
                    json.Value items_value
                        auto | error second = items_value.at(1)
                        match second
                            json.Value value
                                auto | error integer = value.integer()
                                match integer
                                    int number
                                        print(number)
                                        print(NL)
                                    error problem
                                        print(problem)
                                        print(NL)
                            none
                                print("missing-index")
                                print(NL)
                            error problem
                                print(problem)
                                print(NL)
                    none
                        print("missing-items")
                        print(NL)
                    error problem
                        print(problem)
                        print(NL)

                auto | error ok = root.get("ok")
                match ok
                    json.Value value
                        auto | error boolean = value.boolean()
                        match boolean
                            bool bit
                                print(bit)
                                print(NL)
                            error problem
                                print(problem)
                                print(NL)
                    none
                        print("missing-ok")
                        print(NL)
                    error problem
                        print(problem)
                        print(NL)

                auto | error pi = root.get("pi")
                match pi
                    json.Value value
                        auto | error number = value.number()
                        match number
                            float scalar
                                print(scalar)
                                print(NL)
                            error problem
                                print(problem)
                                print(NL)
                    none
                        print("missing-pi")
                        print(NL)
                    error problem
                        print(problem)
                        print(NL)

                string encoded = root.encode()
                print(encoded)
                print(NL)
                auto | error reparsed = json.parse(encoded)
                match reparsed
                    json.Value other
                        print(root.equal(other))
                        print(NL)
                    error problem
                        print(problem)
                        print(NL)
            error problem
                print(problem)
                print(NL)
    error problem
        print(problem)
        print(NL)
QUI

json_output="$("$QUIDRA" "$TMP/json.qui")"
json_source='{"name":"Quidra","items":[1,2],"nothing":null,"ok":true,"pi":3.5}'
json_expected="$(printf 'object\n5\nQuidra\nnone\nnull\n2\ntrue\n3.5\n%s\ntrue' "$json_source")"
[[ "$json_output" == "$json_expected" ]]

cat > "$TMP/json-kind-error.qui" <<'QUI'
auto | error parsed = json.parse("1")
match parsed
    json.Value value
        auto | error text = value.text()
        match text
            string content
                print(content)
                print(NL)
            error problem
                print(problem)
                print(NL)
    error problem
        print(problem)
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/json-kind-error.qui")" == "JSON value has incompatible kind" ]]

cat > "$TMP/json-invalid.qui" <<'QUI'
auto | error parsed = json.parse("[")
match parsed
    json.Value value
        print(value.kind())
        print(NL)
    error problem
        print("error")
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/json-invalid.qui")" == "error" ]]

cat > "$TMP/json-duplicate.json" <<'JSON'
{"a":1,"a":2}
JSON
cat > "$TMP/json-duplicate.qui" <<QUI
auto | error loaded = file.read("$TMP/json-duplicate.json")
match loaded
    string source
        auto | error parsed = json.parse(source)
        match parsed
            json.Value value
                print(value.kind())
                print(NL)
            error problem
                print("error")
                print(NL)
    error problem
        print(problem)
        print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/json-duplicate.qui")" == "error" ]]

cat > "$TMP/json-direct-construction.qui" <<'QUI'
json.Value impossible = json.Value()
QUI
set +e
"$QUIDRA" check "$TMP/json-direct-construction.qui" --json >"$TMP/json-direct-construction.json"
json_direct_rc=$?
set -e
[[ "$json_direct_rc" -eq 1 ]]
grep -q 'Standard library value types cannot be constructed directly' "$TMP/json-direct-construction.json"

cat > "$TMP/json-equality.qui" <<'QUI'
auto | error left = json.parse("1")
auto | error right = json.parse("1")
match left
    json.Value a
        match right
            json.Value b
                print(a == b)
                print(NL)
            error problem
                print(problem)
                print(NL)
    error problem
        print(problem)
        print(NL)
QUI
set +e
"$QUIDRA" check "$TMP/json-equality.qui" --json >"$TMP/json-equality.json"
json_equality_rc=$?
set -e
[[ "$json_equality_rc" -eq 1 ]]
grep -q 'Equality is not defined for this type' "$TMP/json-equality.json"

# Math is an ordinary package, not a Core standard namespace. Core owns only
# the import boundary here; Math owns the function semantics and their tests.
cat > "$TMP/math-requires-import.qui" <<'QUI'
float value = math.sqrt(4.0)
print(value)
print(NL)
QUI
set +e
"$QUIDRA" check "$TMP/math-requires-import.qui" --json >"$TMP/math-requires-import.json"
math_without_import_rc=$?
set -e
[[ "$math_without_import_rc" -eq 1 ]]
grep -Eq 'UNKNOWN|unknown|namespace|name' "$TMP/math-requires-import.json"

cat > "$TMP/tensor-math-methods-removed.qui" <<'QUI'
tensor<float> value = tensor.ones<float>([1])
tensor<float> result = value.sqrt()
print(result[0].item())
print(NL)
QUI
set +e
"$QUIDRA" check "$TMP/tensor-math-methods-removed.qui" --json >"$TMP/tensor-math-methods-removed.json"
tensor_math_method_rc=$?
set -e
[[ "$tensor_math_method_rc" -eq 1 ]]
grep -Eq 'UNKNOWN|unknown|method' "$TMP/tensor-math-methods-removed.json"

cat > "$TMP/scalar-power.qui" <<'QUI'
int integer_base = 2
int integer_power = integer_base ^ 10
print(integer_power)
print(NL)

float float_base = 4.0
float float_power = float_base ^ 0.5
print(float_power)
print(NL)
QUI
scalar_power_output="$("$QUIDRA" run "$TMP/scalar-power.qui")"
[[ "$scalar_power_output" == "$(printf '1024\n2.0')" ]]

cat > "$TMP/tensor-power.qui" <<'QUI'
tensor<int> integer_base = tensor.ones<int>([2]) * 2
tensor<int> integer_power = integer_base ^ 3
print(integer_power[0].item())
print(NL)
print(integer_power[1].item())
print(NL)

tensor<float> float_base = tensor.ones<float>([1]) * 4.0
tensor<float> float_power = float_base ^ 0.5
print(float_power[0].item())
print(NL)

tensor<float> root = (tensor.ones<float>([1]) * 2.0).track()
tensor<float> loss = root ^ 3.0
loss.backward(&root)
print(loss.untrack()[0].item())
print(NL)
print(root.grad[0].item())
print(NL)
QUI
tensor_power_output="$("$QUIDRA" run "$TMP/tensor-power.qui")"
[[ "$tensor_power_output" == "$(printf '8\n8\n2.0\n8.0\n12.0')" ]]

cat > "$TMP/tensor-power-negative-runtime.qui" <<'QUI'
tensor<int> base = tensor.ones<int>([1]) * 2
int exponent = -1
tensor<int> result = base ^ exponent
print(result[0].item())
print(NL)
QUI
set +e
"$QUIDRA" run "$TMP/tensor-power-negative-runtime.qui" >"$TMP/tensor-power-negative-runtime.out" 2>"$TMP/tensor-power-negative-runtime.err"
tensor_power_negative_rc=$?
set -e
[[ "$tensor_power_negative_rc" -eq 101 ]]
grep -q 'invalid tensor power domain' "$TMP/tensor-power-negative-runtime.err"

cat > "$TMP/task-bounded-workers.qui" <<'QUI'
void noop()
    int value = 1

fn<void>()[] operations = array(4096, fill = noop)
task.all(operations)
print("bounded")
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/task-bounded-workers.qui")" == "bounded" ]]

cat > "$TMP/task-worker-lifetime.qui" <<'QUI'
void slow()
    time.Duration pause = time.seconds(0.05)
    time.sleep(pause)

fn<void>()[] operations = array(128, fill = slow)
task.all(operations)
print("joined")
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/task-worker-lifetime.qui")" == "joined" ]]

cat > "$TMP/task-result-worker-lifetime.qui" <<'QUI'
int slow_value()
    time.Duration pause = time.seconds(0.05)
    time.sleep(pause)
    return 7

fn<int>()[] operations = array(128, fill = slow_value)
int[] values = task.all(operations)
print(len(values))
print(NL)
print(values[127])
print(NL)
QUI
task_result_lifetime_expected="$(printf '128\n7')"
[[ "$("$QUIDRA" run "$TMP/task-result-worker-lifetime.qui")" == "$task_result_lifetime_expected" ]]

cat > "$TMP/task-results.qui" <<'QUI'
int first_value()
    return 20

int second_value()
    return 22

int[] values = task.all([first_value, second_value])
print(values[0] + values[1])
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/task-results.qui")" == "42" ]]

cat > "$TMP/task-result-order.qui" <<'QUI'
int slow_first()
    time.Duration pause = time.seconds(0.05)
    time.sleep(pause)
    return 20

int fast_second()
    return 22

int[] values = task.all([slow_first, fast_second])
print(values[0])
print(NL)
print(values[1])
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/task-result-order.qui")" == "$(printf '20\n22')" ]]

cat > "$TMP/atomic-counter-race.qui" <<'QUI'
void increment(atomic.Counter counter)
    for index in range(1000)
        counter.add(1)

atomic.Counter counter = atomic.counter(0)
task.all([increment, increment], counter)
print(counter.load())
print(NL)
QUI
for _ in $(seq 1 20); do
    [[ "$("$QUIDRA" run "$TMP/atomic-counter-race.qui")" == "2000" ]]
done

cat > "$TMP/atomic-counter-copy.qui" <<'QUI'
atomic.Counter first = atomic.counter(5)
atomic.Counter second = first
second.add(7)
print(first.load())
print(NL)
print(second.load())
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/atomic-counter-copy.qui")" == "$(printf '12\n12')" ]]

cat > "$TMP/ref-cell-identity.qui" <<'QUI'
ref.Cell<int32> first = ref.Cell<int32>(value = int32(5))
ref.Cell<int32>[] cells = [first]
ref.Cell<int32> second = cells[0]
ref.Cell<int32> equal_value = ref.Cell<int32>(value = int32(5))
print(first.same(second))
print(NL)
print(first.same(equal_value))
print(NL)
second.value = int32(9)
print(first.value)
print(NL)
print(second.value)
print(NL)
QUI
ref_cell_expected="$(printf 'true\nfalse\n9\n9')"
[[ "$("$QUIDRA" run "$TMP/ref-cell-identity.qui")" == "$ref_cell_expected" ]]

cat > "$TMP/reflect-collect.qui" <<'QUI'
class Marker
    int value = 0

class Inner
    Marker marker

class Outer
    Inner inner
    Marker direct

Marker inner_marker
inner_marker.value = 7
Inner inner
inner.marker = inner_marker
Marker direct
direct.value = 11
Outer model
model.inner = inner
model.direct = direct
Marker[] markers = reflect.collect<Marker>(model)
print(len(markers))
print(NL)
print(markers[0].value)
print(NL)
print(markers[1].value)
print(NL)
QUI
reflect_collect_expected="$(printf '2\n7\n11')"
[[ "$("$QUIDRA" run "$TMP/reflect-collect.qui")" == "$reflect_collect_expected" ]]

cat > "$TMP/reflect-collect-fixed-array.qui" <<'QUI'
class Marker
    int value = 0

class Model
    Marker[2] markers

Marker first
first.value = 13
Marker second
second.value = 17
Model model
model.markers = [first, second]
Marker[] markers = reflect.collect<Marker>(model)
print(len(markers))
print(NL)
print(markers[0].value)
print(NL)
print(markers[1].value)
print(NL)
QUI
reflect_collect_fixed_expected="$(printf '2\n13\n17')"
[[ "$("$QUIDRA" run "$TMP/reflect-collect-fixed-array.qui")" == "$reflect_collect_fixed_expected" ]]

cat > "$TMP/reflect-collect-runtime-array.qui" <<'QUI'
class Marker
    int value = 0

class Block
    Marker marker

class Model
    Block[] blocks

Marker first
first.value = 19
Block first_block
first_block.marker = first
Marker second
second.value = 29
Block second_block
second_block.marker = second

Block[] blocks = []
blocks = blocks.append(first_block)
blocks = blocks.append(second_block)
Model model
model.blocks = blocks

Marker[] markers = reflect.collect<Marker>(model)
print(len(markers))
print(NL)
print(markers[0].value)
print(NL)
print(markers[1].value)
print(NL)
QUI
reflect_collect_runtime_expected="$(printf '2\n19\n29')"
[[ "$("$QUIDRA" run "$TMP/reflect-collect-runtime-array.qui")" == "$reflect_collect_runtime_expected" ]]

cat > "$TMP/reflect-metadata.qui" <<'QUI'
class Marker
    int value = 0

class Block
    Marker marker

class Model
    Block[] blocks
    Marker direct

Marker first
first.value = 31
Block first_block
first_block.marker = first
Marker second
second.value = 37
Block second_block
second_block.marker = second
Block[] blocks = []
blocks = blocks.append(first_block)
blocks = blocks.append(second_block)
Marker direct
direct.value = 41
Model model
model.blocks = blocks
model.direct = direct

string[] paths = reflect.paths<Marker>(model)
print(reflect.type_name(model))
print(NL)
print(len(paths))
print(NL)
print(paths[0])
print(NL)
print(paths[1])
print(NL)
print(paths[2])
print(NL)
Marker[] values = reflect.collect<Marker>(model)
print(values[0].value)
print(NL)
print(values[1].value)
print(NL)
print(values[2].value)
print(NL)
QUI
reflect_metadata_expected="$(printf 'Model\n3\nblocks[0].marker\nblocks[1].marker\ndirect\n31\n37\n41')"
[[ "$("$QUIDRA" run "$TMP/reflect-metadata.qui")" == "$reflect_metadata_expected" ]]

cat > "$TMP/reflect-collect-reference-effects.qui" <<'QUI'
class Marker
    int value = 0

class Model
    Marker marker
    int unrelated

void print_markers<M>(M &model)
    Marker[] markers = reflect.collect<Marker>(model)
    print(len(markers))
    print(NL)
    print(markers[0].value)
    print(NL)

Marker marker
marker.value = 23
Model model
model.marker = marker
print_markers(&model)
QUI
reflect_collect_reference_expected="$(printf '1\n23')"
[[ "$("$QUIDRA" run "$TMP/reflect-collect-reference-effects.qui")" == "$reflect_collect_reference_expected" ]]

cat > "$TMP/atomic-counter-overflow.qui" <<'QUI'
atomic.Counter counter = atomic.counter(9223372036854775807)
counter.add(1)
QUI
set +e
"$QUIDRA" run "$TMP/atomic-counter-overflow.qui" >"$TMP/atomic-counter-overflow.out" 2>"$TMP/atomic-counter-overflow.err"
atomic_counter_overflow_rc=$?
set -e
[[ "$atomic_counter_overflow_rc" -eq 101 ]]
grep -q 'INTEGER_OVERFLOW' "$TMP/atomic-counter-overflow.err"

cat > "$TMP/exact-noninteger-cast.qui" <<'QUI'
bigreal value = 4.5
int converted = int(value)
print(converted)
print(NL)
QUI
set +e
ASAN_OPTIONS=detect_leaks=0 "$QUIDRA" run "$TMP/exact-noninteger-cast.qui" >"$TMP/exact-noninteger-cast.out" 2>"$TMP/exact-noninteger-cast.err"
exact_noninteger_rc=$?
set -e
[[ "$exact_noninteger_rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[UNHANDLED_ERROR\].*numeric cast outside destination range' "$TMP/exact-noninteger-cast.err"

cat > "$TMP/exact-collections.qui" <<'QUI'
bigint key = 123456789012345678901234567890
map.Map<bigint, string> table = map.Map<bigint, string>()
table.set(key, "exact")
print(table.has(key))
print(NL)
set.Set<bigint> keys = set.Set<bigint>()
keys.add(key)
print(keys.has(key))
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/exact-collections.qui")" == "$(printf 'true\ntrue')" ]]

cat > "$TMP/bigreal-map-key.qui" <<'QUI'
map.Map<bigreal, string> invalid = map.Map<bigreal, string>()
QUI
set +e
"$QUIDRA" check "$TMP/bigreal-map-key.qui" --json >"$TMP/bigreal-map-key.json"
bigreal_map_rc=$?
set -e
[[ "$bigreal_map_rc" -eq 1 ]]
grep -q 'STANDARD_KEY_TYPE' "$TMP/bigreal-map-key.json"

cat > "$TMP/bigreal-set-key.qui" <<'QUI'
set.Set<bigreal> invalid = set.Set<bigreal>()
QUI
set +e
"$QUIDRA" check "$TMP/bigreal-set-key.qui" --json >"$TMP/bigreal-set-key.json"
bigreal_set_rc=$?
set -e
[[ "$bigreal_set_rc" -eq 1 ]]
grep -q 'STANDARD_KEY_TYPE' "$TMP/bigreal-set-key.json"

cat > "$TMP/json-exact-data.json" <<'JSON'
{"huge":12345678901234567890123456789012345678901234567890,"real":1.25e1000}
JSON
cat > "$TMP/json-exact-numerics.qui" <<QUI
auto | error loaded = file.read("$TMP/json-exact-data.json")
match loaded
    string source
        auto | error parsed = json.parse(source)
        match parsed
            json.Value root
                auto | error huge_value = root.get("huge")
                match huge_value
                    json.Value value
                        auto | error huge = value.bigint()
                        match huge
                            bigint integer
                                print(integer)
                                print(NL)
                            error problem
                                print(problem)
                                print(NL)
                    none
                        print("missing-huge")
                        print(NL)
                    error problem
                        print(problem)
                        print(NL)

                auto | error real_value = root.get("real")
                match real_value
                    json.Value value
                        auto | error exact_value = value.bigreal()
                        match exact_value
                            bigreal number
                                bigreal expected = 1.25e1000
                                print(number == expected)
                                print(NL)
                            error problem
                                print(problem)
                                print(NL)

                        auto | error narrow = value.number()
                        match narrow
                            float number
                                print(number)
                                print(NL)
                            error problem
                                print("narrow-error")
                                print(NL)
                    none
                        print("missing-real")
                        print(NL)
                    error problem
                        print(problem)
                        print(NL)

                print(root.encode())
                print(NL)
            error problem
                print(problem)
                print(NL)
    error problem
        print(problem)
        print(NL)
QUI
json_exact_output="$("$QUIDRA" run "$TMP/json-exact-numerics.qui")"
json_exact_expected=$(printf '%s\n' \
'12345678901234567890123456789012345678901234567890' \
'true' \
'narrow-error' \
'{"huge":12345678901234567890123456789012345678901234567890,"real":1.25e1000}')
[[ "$json_exact_output" == "$json_exact_expected" ]]

cat > "$TMP/http-server.py" <<'PY'
import http.server
import socketserver

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/missing":
            status = 404
            body = b"missing"
            marker = "missing"
        else:
            status = 200
            body = b"A\x00B"
            marker = "present"
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Quidra", marker)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass

class LocalHTTPServer(http.server.HTTPServer):
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]

server = LocalHTTPServer(("127.0.0.1", 0), Handler)
print(server.server_port, flush=True)
for _ in range(2):
    server.handle_request()
server.server_close()
PY

python3 "$TMP/http-server.py" >"$TMP/http-port" 2>"$TMP/http-server.err" &
HTTP_PID=$!
for _ in $(seq 1 100); do
    [[ -s "$TMP/http-port" ]] && break
    if ! kill -0 "$HTTP_PID" 2>/dev/null; then
        cat "$TMP/http-server.err" >&2
        exit 1
    fi
    sleep 0.05
done
if [[ ! -s "$TMP/http-port" ]]; then
    cat "$TMP/http-server.err" >&2
    kill "$HTTP_PID" 2>/dev/null || true
    wait "$HTTP_PID" 2>/dev/null || true
    HTTP_PID=""
    echo "local HTTP test server did not start" >&2
    exit 1
fi
HTTP_PORT="$(cat "$TMP/http-port")"

cat > "$TMP/http.qui" <<QUI
auto | error successful = http.get("http://127.0.0.1:$HTTP_PORT/ok")
match successful
    http.Response ok_response
        print(ok_response.status)
        print(NL)
        print(len(ok_response.body))
        print(NL)
        print(ok_response.body[0])
        print(NL)
        print(ok_response.body[1])
        print(NL)
        auto marker = ok_response.header("X-Quidra")
        match marker
            string value
                print(value)
                print(NL)
            none
                print("none")
                print(NL)
        auto absent = ok_response.header("missing-header")
        match absent
            string value
                print(value)
                print(NL)
            none
                print("none")
                print(NL)
    error problem
        print(problem)
        print(NL)

auto | error missing = http.get("http://127.0.0.1:$HTTP_PORT/missing")
match missing
    http.Response missing_response
        print(missing_response.status)
        print(NL)
        print(len(missing_response.body))
        print(NL)
    error problem
        print(problem)
        print(NL)

auto | error transport = http.get("file:///etc/passwd")
match transport
    http.Response unexpected_response
        print(unexpected_response.status)
        print(NL)
    error problem
        print("transport-error")
        print(NL)
QUI

http_output="$("$QUIDRA" "$TMP/http.qui")"
wait "$HTTP_PID"
HTTP_PID=""
http_expected="$(printf '200\n24\n0\n1\npresent\nnone\n404\n56\ntransport-error')"
[[ "$http_output" == "$http_expected" ]]

cat > "$TMP/http-direct-construction.qui" <<'QUI'
http.Response impossible = http.Response()
QUI
set +e
"$QUIDRA" check "$TMP/http-direct-construction.qui" --json >"$TMP/http-direct-construction.json"
http_direct_rc=$?
set -e
[[ "$http_direct_rc" -eq 1 ]]
grep -q 'Standard library value types cannot be constructed directly' "$TMP/http-direct-construction.json"

cat > "$TMP/http-equality.qui" <<QUI
auto | error first = http.get("http://127.0.0.1:1/")
match first
    http.Response a
        print(a == a)
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
set +e
"$QUIDRA" check "$TMP/http-equality.qui" --json >"$TMP/http-equality.json"
http_equality_rc=$?
set -e
[[ "$http_equality_rc" -eq 1 ]]
grep -q 'Equality is not defined for this type' "$TMP/http-equality.json"

echo "stdlib integration: ok"


# Named writable arguments.
cat > "$TMP/named-writable.qui" <<'QUI'
void set_value(int &value)
    value = 9

int x = 1
set_value(&value = &x)
print(x)
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/named-writable.qui")" == "9" ]]

cat > "$TMP/named-writable-old-shape.qui" <<'QUI'
void set_value(int &value)
    value = 9

int x = 1
set_value(value = &x)
QUI
set +e
"$QUIDRA" check "$TMP/named-writable-old-shape.qui" --json >"$TMP/named-writable-old-shape.json"
named_writable_old_rc=$?
set -e
[[ "$named_writable_old_rc" -eq 1 ]]

cat > "$TMP/flush.qui" <<'QUI'
print("flush")
print(NL)
flush()
QUI
[[ "$("$QUIDRA" "$TMP/flush.qui")" == "flush" ]]

cat > "$TMP/print-no-newline.qui" <<'QUI'
print("A")
print("B")
flush()
QUI
[[ "$("$QUIDRA" "$TMP/print-no-newline.qui")" == "AB" ]]

# Practical explicit casts remain Core language semantics; rounding APIs live in Math.
cat > "$TMP/practical-casts.qui" <<'QUI'
int large = 16777217
float32 rounded = float32(large)
print(rounded)
print(NL)
float value = 1.75
float32 narrowed = float32(value)
print(narrowed)
print(NL)
tensor<float> source = tensor.ones<float>([1]) * 1.25
tensor<float32> converted = float32(source)
print(converted[0].item())
print(NL)
QUI
practical_cast_output="$("$QUIDRA" "$TMP/practical-casts.qui")"
[[ "$practical_cast_output" == "$(printf '1.6777216e+07\n1.75\n1.25')" ]]

cat > "$TMP/contextual-tensor-dtype.qui" <<'QUI'
tensor<float32> zeros = tensor.zeros([2, 3])
tensor<float32><2, 3> ones = tensor.ones([2, 3])
print(zeros.shape()[0])
print(NL)
print(zeros.shape()[1])
print(NL)
print(ones.shape()[0])
print(NL)
print(ones.shape()[1])
print(NL)
QUI
contextual_tensor_dtype_output="$("$QUIDRA" "$TMP/contextual-tensor-dtype.qui")"
[[ "$contextual_tensor_dtype_output" == "$(printf '2\n3\n2\n3')" ]]

cat > "$TMP/captured-shapes.qui" <<'QUI'
int n = 3
int m = 2
tensor<float><n, 4> first = tensor.ones<float>([3, 4])
n = 5
first = tensor.ones<float>([3, 4])
tensor<float><n, 4> second = tensor.zeros()
tensor<float><n * m, 2> product = tensor.ones<float>([10, 2])
tensor<float><_, 4> explicit_shape = tensor.zeros([5, 4])
print(first.shape()[0])
print(NL)
print(second.shape()[0])
print(NL)
print(product.shape()[0])
print(NL)
print(explicit_shape.shape()[0])
print(NL)
QUI
captured_shapes_output="$("$QUIDRA" "$TMP/captured-shapes.qui")"
[[ "$captured_shapes_output" == "$(printf '3\n5\n10\n5')" ]]

cat > "$TMP/flow-shape-runtime.qui" <<'QUI'
tensor<float32> choose_shape(bool wider)
    tensor<float32> value = tensor.zeros<float32>([3, 4])
    if wider
        value = tensor.zeros<float32>([3, 5])
    return value

tensor<float32><3, 4> checked = choose_shape(false)
print(checked.shape()[1])
print(NL)
QUI
flow_shape_output="$("$QUIDRA" "$TMP/flow-shape-runtime.qui")"
[[ "$flow_shape_output" == "4" ]]

cat > "$TMP/flow-shape-runtime-fail.qui" <<'QUI'
tensor<float32> choose_shape(bool wider)
    tensor<float32> value = tensor.zeros<float32>([3, 4])
    if wider
        value = tensor.zeros<float32>([3, 5])
    return value

tensor<float32><3, 4> checked = choose_shape(true)
print(checked.shape()[1])
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/flow-shape-runtime-fail.qui" >"$TMP/flow-shape-runtime-fail.out" 2>&1
flow_shape_rc=$?
set -e
[[ "$flow_shape_rc" -eq 101 ]]
grep -q 'captured shape constraint' "$TMP/flow-shape-runtime-fail.out"

cat > "$TMP/dependent-signature-shape.qui" <<'QUI'
tensor<float><n, 2> keep_shape(int n, tensor<float><n, 2> value)
    return value
tensor<float> source = tensor.ones<float>([3, 2])
tensor<float><3, 2> checked = keep_shape(3, source)
print(checked.shape()[0])
print(NL)
QUI
dependent_signature_output="$("$QUIDRA" "$TMP/dependent-signature-shape.qui")"
[[ "$dependent_signature_output" == "3" ]]

cat > "$TMP/dependent-signature-shape-fail.qui" <<'QUI'
tensor<float><n, 2> keep_shape(int n, tensor<float><n, 2> value)
    return value
tensor<float> source = tensor.ones<float>([4, 2])
auto checked = keep_shape(3, source)
print(checked.shape()[0])
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/dependent-signature-shape-fail.qui" >"$TMP/dependent-signature-shape-fail.out" 2>"$TMP/dependent-signature-shape-fail.err"
dependent_signature_rc=$?
set -e
[[ "$dependent_signature_rc" -eq 101 ]]
grep -q 'captured shape constraint' "$TMP/dependent-signature-shape-fail.err"

cat > "$TMP/captured-shape-reassign-fail.qui" <<'QUI'
int n = 3
tensor<float><n, 4> value = tensor.ones<float>([3, 4])
n = 5
value = tensor.ones<float>([5, 4])
QUI
set +e
"$QUIDRA" "$TMP/captured-shape-reassign-fail.qui" >"$TMP/captured-shape-reassign-fail.out" 2>"$TMP/captured-shape-reassign-fail.err"
captured_shape_reassign_rc=$?
set -e
[[ "$captured_shape_reassign_rc" -eq 101 ]]
grep -q 'captured shape constraint' "$TMP/captured-shape-reassign-fail.err"

cat > "$TMP/captured-arrays.qui" <<'QUI'
int n = 2
int m = 2
int[n * m] values
values[3] = 7
print(len(values))
print(NL)
print(values[3])
print(NL)
int[][n] rows = [[1, 2], [3, 4]]
n = 3
rows = [[5, 6], [7, 8]]
print(rows[1][1])
print(NL)
QUI
captured_arrays_output="$("$QUIDRA" "$TMP/captured-arrays.qui")"
[[ "$captured_arrays_output" == "$(printf '4\n7\n8')" ]]

cat > "$TMP/captured-array-reassign-fail.qui" <<'QUI'
int n = 2
int[n] values = [1, 2]
n = 3
values = [1, 2, 3]
QUI
set +e
"$QUIDRA" "$TMP/captured-array-reassign-fail.qui" >"$TMP/captured-array-reassign-fail.out" 2>&1
captured_array_reassign_rc=$?
set -e
[[ "$captured_array_reassign_rc" -eq 101 ]]
grep -q 'Quidra runtime error' "$TMP/captured-array-reassign-fail.out"

cat > "$TMP/captured-nested-array-fail.qui" <<'QUI'
int n = 2
int[][n] rows = [[1, 2], [3, 4]]
n = 3
rows = [[1, 2, 3], [4, 5, 6]]
QUI
set +e
"$QUIDRA" "$TMP/captured-nested-array-fail.qui" >"$TMP/captured-nested-array-fail.out" 2>&1
captured_nested_array_rc=$?
set -e
[[ "$captured_nested_array_rc" -eq 101 ]]
grep -q 'Quidra runtime error' "$TMP/captured-nested-array-fail.out"

cat > "$TMP/contextual-wildcard-zero.qui" <<'QUI'
tensor<float><_, 4> value = tensor.zeros()
QUI
set +e
"$QUIDRA" check "$TMP/contextual-wildcard-zero.qui" --json >"$TMP/contextual-wildcard-zero.json"
contextual_wildcard_rc=$?
set -e
[[ "$contextual_wildcard_rc" -eq 1 ]]
grep -q 'Contextual tensor allocation cannot infer' "$TMP/contextual-wildcard-zero.json"

cat > "$TMP/container-casts.qui" <<'QUI'
int[][] dynamic = [[1, 2], [3, 4]]
float[][] dynamic_float = float(dynamic)
print(dynamic_float[1][0])
print(NL)

int[2][2] fixed = [[5, 6], [7, 8]]
float[2][2] fixed_float = float(fixed)
print(fixed_float[0][1])
print(NL)

tensor<int><2, 2> matrix = tensor.ones<int>([2, 2])
tensor<float><2, 2> matrix_float = float(matrix)
print(matrix_float[1, 1].item())
print(NL)
QUI
container_cast_output="$("$QUIDRA" "$TMP/container-casts.qui")"
[[ "$container_cast_output" == "$(printf '3.0\n6.0\n1.0')" ]]

cat > "$TMP/container-cast-range.qui" <<'QUI'
int[] values = [1, 300]
int8[] converted = int8(values)
print(converted[0])
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/container-cast-range.qui" >"$TMP/container-cast-range.out" 2>"$TMP/container-cast-range.err"
container_range_rc=$?
set -e
[[ "$container_range_rc" -eq 101 ]]
grep -q 'UNHANDLED_ERROR' "$TMP/container-cast-range.err"
grep -q 'numeric cast outside destination range' "$TMP/container-cast-range.err"

cat > "$TMP/container-cast-preserved-error.qui" <<'QUI'
int[] values = [1, 300]
auto | error converted = int8(values)
match converted
    int8[] narrowed
        print(narrowed[0])
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
container_preserved_output="$("$QUIDRA" "$TMP/container-cast-preserved-error.qui")"
[[ "$container_preserved_output" == "numeric cast outside destination range" ]]

cat > "$TMP/tensor-cast-preserved-error.qui" <<'QUI'
tensor<int> safe = tensor.ones<int>([2])
auto | error safe_result = int8(safe)
match safe_result
    tensor<int8> narrowed
        print(narrowed[0].item())
        print(NL)
    error problem
        print(problem)
        print(NL)

tensor<int> unsafe = tensor.ones<int>([2])
unsafe[1] = 300
auto | error unsafe_result = int8(unsafe)
match unsafe_result
    tensor<int8> narrowed
        print(narrowed[0].item())
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
tensor_preserved_output="$("$QUIDRA" "$TMP/tensor-cast-preserved-error.qui")"
[[ "$tensor_preserved_output" == "$(printf '1\nnumeric cast outside destination range')" ]]

cat > "$TMP/container-cast-uninitialized.qui" <<'QUI'
int[2] values
values[0] = 7
float[2] converted = float(values)
print(converted[0])
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/container-cast-uninitialized.qui" >"$TMP/container-cast-uninitialized.out" 2>"$TMP/container-cast-uninitialized.err"
container_uninitialized_rc=$?
set -e
[[ "$container_uninitialized_rc" -eq 101 ]]
grep -q 'UNINITIALIZED' "$TMP/container-cast-uninitialized.err"

cat > "$TMP/float-int-cast-rejected.qui" <<'QUI'
float value = 1.0
int converted = int(value)
print(converted)
print(NL)
QUI
set +e
"$QUIDRA" check "$TMP/float-int-cast-rejected.qui" --json >"$TMP/float-int-cast-rejected.json"
float_int_cast_rc=$?
set -e
[[ "$float_int_cast_rc" -eq 1 ]]
grep -q 'Floating-point to integer conversion requires' "$TMP/float-int-cast-rejected.json"


cat > "$TMP/bin-cast-length-fail.qui" <<'QUI'
bin value = bin.fill(3, 0)
int8 decoded = int8(value)
print(decoded)
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/bin-cast-length-fail.qui" >"$TMP/bin-cast-length-fail.out" 2>"$TMP/bin-cast-length-fail.err"
bin_cast_length_rc=$?
set -e
[[ "$bin_cast_length_rc" -eq 101 ]]
grep -q 'bin length does not match destination type width' "$TMP/bin-cast-length-fail.err"

cat > "$TMP/bin-bool-length-fail.qui" <<'QUI'
bin value = bin.fill(2, 0)
bool decoded = bool(value)
print(decoded)
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/bin-bool-length-fail.qui" >"$TMP/bin-bool-length-fail.out" 2>"$TMP/bin-bool-length-fail.err"
bin_bool_length_rc=$?
set -e
[[ "$bin_bool_length_rc" -eq 101 ]]
grep -q 'bin length does not match destination type width' "$TMP/bin-bool-length-fail.err"

cat > "$TMP/bin-array-length-fail.qui" <<'QUI'
bin value = bin.fill(3, 0)
uint8[] decoded = uint8[](value)
print(len(decoded))
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/bin-array-length-fail.qui" >"$TMP/bin-array-length-fail.out" 2>"$TMP/bin-array-length-fail.err"
bin_array_length_rc=$?
set -e
[[ "$bin_array_length_rc" -eq 101 ]]
grep -q 'bin length is not divisible by destination element width' "$TMP/bin-array-length-fail.err"


