#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/point.qui" <<'QUI'
class Point point(real64 x, real64 y, local real64 scale = 2.0)
    x *= scale
    y *= scale
    real64 length_squared = x * x + y * y
    real64 sum()
        return x + y
    Point copy()
        return point
    void construct()
        print("ordinary{NL}")
Point p = Point(3.0, 4.0)
Point q = p.copy()
p.x = 10.0
print("{q.x}{NL}")
print("{q.y}{NL}")
print("{q.length_squared}{NL}")
print("{q.sum()}{NL}")
Point scaled = Point(3.0, 4.0, scale = 10.0)
print("{scaled.x}{NL}")
print("{scaled.y}{NL}")
q.construct()
QUI
[[ "$("$QUIDRA" "$TMP/point.qui")" == $'6.0\n8.0\n100.0\n14.0\n30.0\n40.0\nordinary' ]]
cat > "$TMP/local.qui" <<'QUI'
class Point(int x)
    local int scale = 2
    if x < 0
        scale = 3
        int inner = 4
        x += inner
    x *= scale
    int used_scale = scale
Point first = Point(-1)
Point second = Point(2)
print("{first.x}{NL}")
print("{first.used_scale}{NL}")
print("{second.x}{NL}")
print("{second.used_scale}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/local.qui")" == $'9\n3\n4\n2' ]]
cat > "$TMP/ordinary-construct.qui" <<'QUI'
class Value()
    T construct<T>(T input)
        return input
Value value = Value()
print("{value.construct<int>(7)}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/ordinary-construct.qui")" == '7' ]]
cat > "$TMP/function-member.qui" <<'QUI'
int increment(int input)
    return input + 1
class Callback(func<int>(int) operation)
    int run(int input)
        func<int>(int) target = operation
        return target(input)
Callback callback = Callback(increment)
print("{callback.run(6)}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/function-member.qui")" == '7' ]]
cat > "$TMP/reference-initialization.qui" <<'QUI'
class Explicit self(local int &input)
    int &value
    &self.value = &input
    void .bind(int &replacement)
        &self.value = &replacement
class Bare(local int &input)
    int &value
    &value = &input
class Header(int &value)
class Inline(local int &input)
    int &value = &input
class LocalReference(local int &input)
    local int &temporary
    &temporary = &input
    int captured = temporary
int original = 1
Explicit explicit = Explicit(&original)
Explicit explicit_copy = explicit
explicit_copy.value = 2
print("{explicit.value} {original}{NL}")
int replacement = 9
.explicit_copy.bind(&replacement)
explicit_copy.value = 10
print("{explicit.value} {original} {replacement}{NL}")
Bare bare = Bare(&original)
Bare bare_copy = bare
bare_copy.value = 3
print("{bare.value} {original}{NL}")
Header header = Header(&original)
Header header_copy = header
header_copy.value = 4
print("{header.value} {original}{NL}")
Inline inline = Inline(&original)
Inline inline_copy = inline
inline_copy.value = 5
print("{inline.value} {original}{NL}")
LocalReference local_reference = LocalReference(&original)
print("{local_reference.captured}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/reference-initialization.qui")" == $'2 2\n2 2 10\n3 3\n4 4\n5 5\n5' ]]
cat > "$TMP/prototype.qui" <<'QUI'
class Scaled scaled(real64 x, local real64 scale = 2.0);
class Scaled scaled(real64 x, local real64 scale)
    x *= scale
Scaled value = Scaled(3.0)
print("{value.x}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/prototype.qui")" == '6.0' ]]
cat > "$TMP/errors.qui" <<'QUI'
string | error read(bool accept)
    if not accept
        return error("refused")
    return "ready"
class Document document(local bool accept) | error
    string header = "header"
    local string temporary = "temporary"
    string text = try read(accept)
    Document copy()
        return document
Document | error rejected = Document(false)
match rejected
    Document value
        print("{value.text}{NL}")
    error failure
        print("{failure.message}{NL}")
Document | error accepted = Document(true)
match accepted
    Document value
        print("{value.text}{NL}")
    error failure
        print("{failure.message}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/errors.qui")" == $'refused\nready' ]]
cat > "$TMP/standard-generation.qui" <<'QUI'
map<int, int> explicit_map = map<int, int>()
map<int, int> bare_map
distinct<int> explicit_distinct = distinct<int>()
distinct<int> bare_distinct
print("{explicit_map.size()} {bare_map.size()} {explicit_distinct.size()} {bare_distinct.size()}{NL}")
.explicit_map.set(1, 10)
.bare_map.set(2, 20)
.explicit_distinct.add(1)
.bare_distinct.add(2)
print("{explicit_map.has(1)} {bare_map.has(2)} {explicit_distinct.has(1)} {bare_distinct.has(2)}{NL}")
map<int, int> copied_map = explicit_map
distinct<int> copied_distinct = explicit_distinct
.copied_map.set(3, 30)
.copied_distinct.add(3)
print("{explicit_map.has(3)} {copied_map.has(3)} {explicit_distinct.has(3)} {copied_distinct.has(3)}{NL}")
print("{explicit_map.size()} {bare_map.size()} {explicit_distinct.size()} {bare_distinct.size()}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/standard-generation.qui")" == $'0 0 0 0\ntrue true true true\nfalse true false true\n1 1 1 1' ]]
reject() {
    local code="$1"
    printf '%s\n' "$2" > "$TMP/reject.qui"
    if "$QUIDRA" check "$TMP/reject.qui" --json > "$TMP/reject.json"; then exit 1; fi
    python3 - "$TMP/reject.json" "$code" <<'PY'
import json,sys
assert any(d['code']==sys.argv[2] for d in json.load(open(sys.argv[1]))['diagnostics'])
PY
}
reject UNINITIALIZED $'class Partial()\n    int value = 3\nPartial partial\nprint(partial.value)'
reject CONSTRUCTOR_SIGNATURE $'class Empty\n    int value'
reject PROTOTYPE_MISMATCH $'class Entry(int[] values);\nclass Entry(int[2] values)'
reject PROTOTYPE_MISMATCH $'class Entry(func<int>(int) operation);\nclass Entry(func<int>(real64) operation)'
reject DUPLICATE_NAME $'class Point(int x)\n    int x = 1'
reject SHADOWING $'class Point(int x)\n    local int x = 1'
reject SHADOWING $'class Point point(int x)\n    int read(int point)\n        return x'
reject UNKNOWN_MEMBER $'class Point(local int scale)\n    int value = scale\nPoint p = Point(2)\nprint(p.scale)'
reject UNKNOWN_NAME $'class Point(int x)\n    local int scale = 2\n    int read()\n        return scale'
reject UNKNOWN_NAME $'class Point(int x)\n    if x < 0\n        int scale = 2\n    int value = scale'
reject WRITE_CAPABILITY $'class Point point(int x)\n    void .replace(Point other)\n        point = other'
reject REFERENCE_BINDING $'class Holder(local int &input)\n    int &value = &input\nint first = 1\nint second = 2\nHolder holder = Holder(&first)\n&holder.value = &second'
reject REFERENCE_BINDING $'class Holder self(local int &input)\n    int &value = &input\n    void .bind_other(Holder &other, int &replacement)\n        &other.value = &replacement'
reject REFERENCE_TARGET $'class Interior(int value)\n    int &alias = &value'
reject UNINITIALIZED $'class Early self(local int &input)\n    int &value = &input\n    local Early leaked = self'
reject WRITE_CAPABILITY $'class Fixed self(int value)\n    void .bind(Fixed &replacement)\n        &self = &replacement'
reject UNINITIALIZED $'class Point point(int x)\n    local Point leaked = point'
reject UNINITIALIZED $'class Point point(int x)\n    local Point leaked = copy()\n    Point copy()\n        return point'
reject NOT_CALLABLE $'class Callback(func<int>(int) operation)\n    int value = operation(1)'
reject NOT_CALLABLE $'class Callback callback(func<int>(int) operation)\n    int run(int input)\n        return callback.operation(input)'

python3 - "$QUIDRA" "$TMP" <<'PYLSP'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
uri=(tmp/'class-scopes.qui').as_uri()
source=("class Point point(int x, local int factor = 2)\n"
        "    local int scale = factor\n"
        "    x *= scale\n"
        "    int read()\n"
        "        return x\n"
        "    Point copy()\n"
        "        return point\n"
        "Point p = Point(3)\n")
incomplete=source.replace("return point\n","return point.\n")
messages=[
 {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":tmp.as_uri()}},
 {"jsonrpc":"2.0","method":"initialized","params":{}},
 {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":uri,"languageId":"quidra","version":1,"text":source}}},
 {"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":uri},"position":{"line":4,"character":15}}},
 {"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{"textDocument":{"uri":uri},"position":{"line":6,"character":17}}},
 {"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":uri,"version":2},"contentChanges":[{"text":incomplete}]}},
 {"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{"textDocument":{"uri":uri},"position":{"line":6,"character":21}}},
 {"jsonrpc":"2.0","id":5,"method":"shutdown","params":None},
 {"jsonrpc":"2.0","method":"exit","params":None}]
payload=b''
for message in messages:
    body=json.dumps(message,separators=(',',':')).encode()
    payload+=f'Content-Length: {len(body)}\r\n\r\n'.encode()+body
process=subprocess.run([quidra,'lsp'],input=payload,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
assert process.returncode==0,process.stderr.decode()
responses={};data=process.stdout;pos=0
while pos<len(data):
    end=data.index(b'\r\n\r\n',pos)
    headers=data[pos:end].decode().split('\r\n')
    length=int(next(line.split(':',1)[1] for line in headers if line.lower().startswith('content-length:')))
    start=end+4;message=json.loads(data[start:start+length]);pos=start+length
    if 'id' in message: responses[message['id']]=message
labels={item['label'] for item in responses[2]['result']}
assert {'x','point','read','copy'}<=labels,labels
assert not {'factor','scale'}&labels,labels
assert responses[3]['result']['range']['start']=={'line':0,'character':12},responses[3]
assert {item['label'] for item in responses[4]['result']}=={'x','read','copy'},responses[4]
PYLSP
