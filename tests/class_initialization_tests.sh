#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/point.qui" <<'QUI'
class Point point(real64 x, real64 y, local real64 scale := 2.0)
    x *= scale
    y *= scale
    real64 length_squared := x * x + y * y
    real64 sum()
        return x + y
    Point copy()
        return point
    void construct()
        print("ordinary{NL}")
Point p := Point(3.0, 4.0)
Point q := p.copy()
p.x = 10.0
print("{q.x}{NL}")
print("{q.y}{NL}")
print("{q.length_squared}{NL}")
print("{q.sum()}{NL}")
Point scaled := Point(3.0, 4.0, scale = 10.0)
print("{scaled.x}{NL}")
print("{scaled.y}{NL}")
q.construct()
QUI
[[ "$("$QUIDRA" "$TMP/point.qui")" == $'6.0\n8.0\n100.0\n14.0\n30.0\n40.0\nordinary' ]]
cat > "$TMP/local.qui" <<'QUI'
class Point(int x)
    local int scale := 2
    if x < 0
        scale = 3
        int inner := 4
        x += inner
    x *= scale
    int used_scale := scale
Point first := Point(-1)
Point second := Point(2)
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
Value value := Value()
print("{value.construct<int>(7)}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/ordinary-construct.qui")" == '7' ]]
cat > "$TMP/function-member.qui" <<'QUI'
int increment(int input)
    return input + 1
class Callback(func<int>(int) operation)
    int run(int input)
        func<int>(int) target := operation
        return target(input)
Callback callback := Callback(increment)
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
    int &value := &input
class LocalReference(local int &input)
    local int &temporary
    &temporary = &input
    int captured := temporary
int original := 1
Explicit explicit := Explicit(&original)
Explicit explicit_copy := explicit
explicit_copy.value = 2
print("{explicit.value} {original}{NL}")
int replacement := 9
.explicit_copy.bind(&replacement)
explicit_copy.value = 10
print("{explicit.value} {original} {replacement}{NL}")
Bare bare := Bare(&original)
Bare bare_copy := bare
bare_copy.value = 3
print("{bare.value} {original}{NL}")
Header header := Header(&original)
Header header_copy := header
header_copy.value = 4
print("{header.value} {original}{NL}")
Inline inline := Inline(&original)
Inline inline_copy := inline
inline_copy.value = 5
print("{inline.value} {original}{NL}")
LocalReference local_reference := LocalReference(&original)
print("{local_reference.captured}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/reference-initialization.qui")" == $'2 2\n2 2 10\n3 3\n4 4\n5 5\n5' ]]
cat > "$TMP/prototype.qui" <<'QUI'
class Scaled scaled(real64 x, local real64 scale := 2.0);
class Scaled scaled(real64 x, local real64 scale)
    x *= scale
Scaled value := Scaled(3.0)
print("{value.x}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/prototype.qui")" == '6.0' ]]
cat > "$TMP/errors.qui" <<'QUI'
string | error read(bool accept)
    if not accept
        return error("refused")
    return "ready"
class Document document(local bool accept) | error
    string header := "header"
    local string temporary := "temporary"
    string text := try read(accept)
    Document copy()
        return document
Document | error rejected := Document(false)
match rejected
    Document value
        print("{value.text}{NL}")
    error failure
        print("{failure.message}{NL}")
Document | error accepted := Document(true)
match accepted
    Document value
        print("{value.text}{NL}")
    error failure
        print("{failure.message}{NL}")
QUI
[[ "$("$QUIDRA" "$TMP/errors.qui")" == $'refused\nready' ]]
cat > "$TMP/standard-generation.qui" <<'QUI'
map<int, int> explicit_map := map<int, int>()
map<int, int> bare_map
distinct<int> explicit_distinct := distinct<int>()
distinct<int> bare_distinct
print("{explicit_map.size()} {bare_map.size()} {explicit_distinct.size()} {bare_distinct.size()}{NL}")
.explicit_map.set(1, 10)
.bare_map.set(2, 20)
.explicit_distinct.add(1)
.bare_distinct.add(2)
print("{explicit_map.has(1)} {bare_map.has(2)} {explicit_distinct.has(1)} {bare_distinct.has(2)}{NL}")
map<int, int> copied_map := explicit_map
distinct<int> copied_distinct := explicit_distinct
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
reject UNINITIALIZED $'class Partial()\n    int value := 3\nPartial partial\nprint(partial.value)'
reject CONSTRUCTOR_SIGNATURE $'class Empty\n    int value'
reject PROTOTYPE_MISMATCH $'class Entry(int[] values);\nclass Entry(int[2] values)'
reject PROTOTYPE_MISMATCH $'class Entry(func<int>(int) operation);\nclass Entry(func<int>(real64) operation)'
reject DUPLICATE_NAME $'class Point(int x)\n    int x := 1'
reject SHADOWING $'class Point(int x)\n    local int x := 1'
reject SHADOWING $'class Point point(int x)\n    int read(int point)\n        return x'
reject UNKNOWN_MEMBER $'class Point(local int scale)\n    int value := scale\nPoint p := Point(2)\nprint(p.scale)'
reject UNKNOWN_NAME $'class Point(int x)\n    local int scale := 2\n    int read()\n        return scale'
reject UNKNOWN_NAME $'class Point(int x)\n    if x < 0\n        int scale := 2\n    int value := scale'
reject WRITE_CAPABILITY $'class Point point(int x)\n    void .replace(Point other)\n        point = other'
reject REFERENCE_BINDING $'class Holder(local int &input)\n    int &value := &input\nint first := 1\nint second := 2\nHolder holder := Holder(&first)\n&holder.value = &second'
reject REFERENCE_BINDING $'class Holder self(local int &input)\n    int &value := &input\n    void .bind_other(Holder &other, int &replacement)\n        &other.value = &replacement'
reject REFERENCE_TARGET $'class Interior(int value)\n    int &alias := &value'
reject UNINITIALIZED $'class Early self(local int &input)\n    int &value := &input\n    local Early leaked := self'
reject WRITE_CAPABILITY $'class Fixed self(int value)\n    void .bind(Fixed &replacement)\n        &self = &replacement'
reject UNINITIALIZED $'class Point point(int x)\n    local Point leaked := point'
reject UNINITIALIZED $'class Point point(int x)\n    local Point leaked := copy()\n    Point copy()\n        return point'
reject NOT_CALLABLE $'class Callback(func<int>(int) operation)\n    int value := operation(1)'
reject NOT_CALLABLE $'class Callback callback(func<int>(int) operation)\n    int run(int input)\n        return callback.operation(input)'

python3 - "$QUIDRA" "$TMP" <<'PYLSP'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
uri=(tmp/'class-scopes.qui').as_uri()
source=("class Point point(int x, local int factor := 2)\n"
        "    local int scale := factor\n"
        "    x *= scale\n"
        "    int read()\n"
        "        return x\n"
        "    Point copy()\n"
        "        return point\n"
        "Point p := Point(3)\n")
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

# Complete-value uses through writable parameters infer requirements on the
# caller, while writes of partial replacements invalidate earlier entry facts.
python3 - "$QUIDRA" "$TMP" <<'PYINIT'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
complete='class Box()\n    value := 7\n'
partial='class Box()\n    int value\n'
reader='int read_value(const Box input)\n    return input.value\n'
forward='int forward(Box &input)\n    return read_value(input)\n'
call='box := Box()\nprintln(forward(&box))\n'
cases={
 'value-forwarding':(complete+reader+forward+call,None),
 'transitive-value-forwarding':(complete+reader+forward+'int outer(Box &input)\n    return forward(&input)\nbox := Box()\nprintln(outer(&box))\n',None),
 'write-before-value-forwarding':(partial+reader+'int forward(Box &input)\n    input.value = 7\n    return read_value(input)\n'+call,None),
 'nested-value-forwarding':('class Inner()\n    value := 7\nclass Outer()\n    child := Inner()\nint read_value(const Inner input)\n    return input.value\nint forward(Outer &input)\n    return read_value(input.child)\nbox := Outer()\nprintln(forward(&box))\n',None),
 'array-forwarding':(complete+'Box[] collect(Box &input)\n    return [input]\nbox := Box()\nprintln(collect(&box).len())\n',None),
 'equality-forwarding':(complete+'bool same(Box &left, const Box right)\n    return left == right\nbox := Box()\nprintln(same(&box, box))\n',None),
 'partial-value-forwarding':(partial+reader+forward+call,'UNINITIALIZED'),
 'partial-direct-value':(partial+reader+'box := Box()\nprintln(read_value(box))\n','UNINITIALIZED_ARGUMENT'),
 'partial-array-forwarding':(partial+'Box[] collect(Box &input)\n    return [input]\nbox := Box()\nprintln(collect(&box).len())\n','UNINITIALIZED'),
 'partial-equality-forwarding':(partial+'bool same(Box &left, const Box right)\n    return left == right\nbox := Box()\nother := Box()\nother.value = 7\nprintln(same(&box, other))\n','UNINITIALIZED'),
 'partial-overwrite':(partial+reader+'int forward(Box &input)\n    input = Box()\n    return read_value(input)\nbox := Box()\nbox.value = 7\nprintln(forward(&box))\n','UNINITIALIZED_ARGUMENT'),
 'prior-read-partial-overwrite':(partial+reader+'int forward(Box &input)\n    old := input.value\n    input = Box()\n    return read_value(input) + old\nbox := Box()\nbox.value = 7\nprintln(forward(&box))\n','UNINITIALIZED_ARGUMENT'),
 'conditional-partial-overwrite':(partial+reader+'int forward(Box &input, bool replace)\n    if replace\n        input = Box()\n    return read_value(input)\nbox := Box()\nbox.value = 7\nprintln(forward(&box, true))\n','UNINITIALIZED_ARGUMENT'),
 'direct-read-partial-overwrite':(partial+'int forward(Box &input)\n    old := input.value\n    input = Box()\n    return input.value + old\nbox := Box()\nbox.value = 7\nprintln(forward(&box))\n','UNINITIALIZED'),
 'receiver-partial-overwrite':('class Inner()\n    int value\nclass Outer self()\n    child := Inner()\n    Outer .replace()\n        self.child = Inner()\n        return self\nbox := Outer()\nbox.child.value = 7\ncopy := .box.replace()\nprintln(copy.child.value)\n','UNINITIALIZED'),
 'receiver-restored-overwrite':('class Inner()\n    int value\nclass Outer self()\n    child := Inner()\n    Outer .replace()\n        self.child = Inner()\n        self.child.value = 9\n        return self\nbox := Outer()\nbox.child.value = 7\ncopy := .box.replace()\nprintln(copy.child.value)\n',None),
 'restored-partial-overwrite':(partial+reader+'int forward(Box &input)\n    old := input.value\n    input = Box()\n    input.value = 9\n    return read_value(input) + old\nbox := Box()\nbox.value = 7\nprintln(forward(&box))\n',None),
}
for name,(source,code) in cases.items():
    path=tmp/(name+'.qui');path.write_text(source)
    result=subprocess.run([quidra,'check',str(path),'--json'],capture_output=True,text=True)
    response=json.loads(result.stdout)
    if code is None:
        assert result.returncode==0,(name,result.stdout,result.stderr)
    else:
        assert result.returncode!=0 and any(d['code']==code for d in response['diagnostics']),(name,result.stdout,result.stderr)
PYINIT

# Copy-time field dependencies remain independent from later source writes.
python3 - "$QUIDRA" "$TMP" <<'PYCOPY'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
cases=[
('19_ref_array_nested_red', True, '''class Inner(int value)
class Outer(Inner child)
class State()
    Outer[] items := [Outer(Inner(7))]
int read_value(const Inner input)
    return input.value
int forward(State &state)
    return read_value(state.items[0].child)
state := State()
println(forward(&state))
'''),
('20_ref_nested_copy_red', True, '''class Inner(int value)
class Outer(Inner child)
int forward(Outer &input)
    copy := input.child
    return copy.value
outer := Outer(Inner(7))
println(forward(&outer))
'''),
('21_partial_copy_positive', True, '''class Box()
    int first
    int second
int initialize_copy(Box &input)
    copy := input
    copy.first = 7
    return copy.first
box := Box()
println(initialize_copy(&box))
'''),
('22_value_array_nested_positive', True, '''class Inner(int value)
class Outer(Inner child)
class State()
    Outer[] items := [Outer(Inner(7))]
int read_value(const Inner input)
    return input.value
int forward(const State state)
    return read_value(state.items[0].child)
state := State()
println(forward(state))
'''),
('23_copy_ref_parameter_red', True, '''class Box(int value)
int forward(Box &input)
    copy := input
    return copy.value
box := Box(7)
println(forward(&box))
'''),
('25_array_partial_replacement_negative', False, '''class Inner()
    int value
class Outer(Inner child)
class State()
    Outer[] items := []
int read_value(const Inner input)
    return input.value
int forward(State &state)
    state.items[0].child = Inner()
    return read_value(state.items[0].child)
inner := Inner()
inner.value = 7
state := State()
state.items = [Outer(inner)]
println(forward(&state))
'''),
('26_array_refill_positive', True, '''class Inner()
    int value
class Outer(Inner child)
class State()
    Outer[] items := []
int read_value(const Inner input)
    return input.value
int forward(State &state)
    state.items[0].child = Inner()
    state.items[0].child.value = 9
    return read_value(state.items[0].child)
inner := Inner()
inner.value = 7
state := State()
state.items = [Outer(inner)]
println(forward(&state))
'''),
('27_array_unset_alias_negative', False, '''class Box(int value)
Box[] items := array(1)
&item := &items[0]
println(item.value)
'''),
('28_ref_copy_after_source_overwrite_red', True, '''class Box()
    int value
int forward(Box &input)
    copy := input
    input = Box()
    return copy.value
box := Box()
box.value = 7
println(forward(&box))
'''),
('29_ref_copy_after_invalid_source_negative', False, '''class Box()
    int value
int forward(Box &input)
    input = Box()
    copy := input
    return copy.value
box := Box()
box.value = 7
println(forward(&box))
'''),
('30_ref_copy_partial_caller_negative', False, '''class Box()
    int value
int forward(Box &input)
    copy := input
    return copy.value
box := Box()
println(forward(&box))
'''),
('31_ref_copy_source_later_init_negative', False, '''class Box()
    int value
int forward(Box &input)
    copy := input
    input.value = 9
    return copy.value
box := Box()
println(forward(&box))
'''),
('branch_same_origin', True, '''class Box()
    int value
int forward(Box &input, bool flag)
    copy := input
    if flag
        copy = input
    return copy.value
box := Box()
box.value = 7
println(forward(&box, true))
'''),
('branch_different_origins', True, '''class Box()
    int value
int forward(Box &left, Box &right, bool flag)
    copy := left
    if flag
        copy = right
    return copy.value
left := Box()
left.value = 7
right := Box()
right.value = 9
println(forward(&left, &right, true))
'''),
('branch_partial_origin', False, '''class Box()
    int value
int forward(Box &left, Box &right, bool flag)
    copy := left
    if flag
        copy = right
    return copy.value
left := Box()
left.value = 7
right := Box()
println(forward(&left, &right, true))
'''),
('branch_partial_replacement', False, '''class Box()
    int value
int forward(Box &input, bool flag)
    copy := input
    if flag
        copy = Box()
    return copy.value
box := Box()
box.value = 7
println(forward(&box, true))
'''),
('nested_copy_then_write_other_leaf', True, '''class Inner()
    int first
    int second
class Outer()
    child := Inner()
int forward(Inner &input)
    copy := Outer()
    copy.child = input
    copy.child.first = 9
    return copy.child.second
inner := Inner()
inner.second = 7
println(forward(&inner))
'''),
('nested_copy_then_partial_replacement', False, '''class Inner()
    int first
    int second
class Outer()
    child := Inner()
int forward(Inner &input)
    copy := Outer()
    copy.child = input
    copy.child = Inner()
    return copy.child.second
inner := Inner()
inner.second = 7
println(forward(&inner))
'''),
('identity_copy_back', True, '''class Box()
    int value
void preserve(Box &input)
    copy := input
    input = Box()
    input = copy
box := Box()
box.value = 7
preserve(&box)
println(box.value)
'''),
('identity_copy_back_partial', False, '''class Box()
    int value
void preserve(Box &input)
    copy := input
    input = Box()
    input = copy
box := Box()
preserve(&box)
println(box.value)
'''),
('nested_identity_preserves_one_leaf', True, '''class Inner()
    int first
    int second
class Outer()
    child := Inner()
void preserve(Outer &input)
    copy := Outer()
    copy.child = input.child
    input = copy
outer := Outer()
outer.child.first = 7
preserve(&outer)
println(outer.child.first)
'''),
('nested_identity_does_not_init_other_leaf', False, '''class Inner()
    int first
    int second
class Outer()
    child := Inner()
void preserve(Outer &input)
    copy := Outer()
    copy.child = input.child
    input = copy
outer := Outer()
outer.child.first = 7
preserve(&outer)
println(outer.child.second)
'''),
('return_path_partial_replacement', False, '''class Box()
    int value
void preserve(Box &input, bool flag)
    copy := input
    input = Box()
    if flag
        return
    input = copy
box := Box()
box.value = 7
preserve(&box, true)
println(box.value)
'''),
('callee_invalidates_copy', False, '''class Box()
    int value
void clear(Box &input)
    input = Box()
int forward(Box &input)
    copy := input
    clear(&copy)
    return copy.value
box := Box()
box.value = 7
println(forward(&box))
'''),
('callee_invalidates_source_after_copy', True, '''class Box()
    int value
void clear(Box &input)
    input = Box()
int forward(Box &input)
    copy := input
    clear(&input)
    return copy.value
box := Box()
box.value = 7
println(forward(&box))
'''),
('unknown_alias_can_invalidate_copy', False, '''class Box()
    int value
int forward(Box &input, bool flag)
    copy := input
    other := Box()
    &alias := &copy
    if flag
        &alias = &other
    alias = Box()
    return copy.value
box := Box()
box.value = 7
println(forward(&box, false))
'''),
('whole_copy_self_assignment', True, '''class Box()
    int value
int forward(Box &input)
    copy := input
    copy = copy
    return copy.value
box := Box()
box.value = 7
println(forward(&box))
'''),
('ref_whole_self_assignment', True, '''class Box()
    int value
void preserve(Box &input)
    input = input
box := Box()
box.value = 7
preserve(&box)
println(box.value)
'''),
('partial_unused_copy_back', True, '''class Box()
    int value
void preserve(Box &input)
    copy := input
    input = copy
box := Box()
preserve(&box)
'''),
('receiver_field_copy', True, '''class State(int value)
int read_value(const State state)
    return state.value
class Owner()
    state := State(7)
    int read()
        copy := state
        return read_value(copy)
owner := Owner()
println(owner.read())
'''),
('receiver_copy_source_overwrite', True, '''class Box()
    int value
class Owner()
    state := Box()
    int .read()
        copy := state
        state = Box()
        return copy.value
owner := Owner()
owner.state.value = 7
println(.owner.read())
'''),
('receiver_copy_source_later_init', False, '''class Box()
    int value
class Owner()
    state := Box()
    int .read()
        copy := state
        state.value = 9
        return copy.value
owner := Owner()
println(.owner.read())
'''),
('error_data_return_keeps_invalidation', False, '''class Box()
    int value
Box | error attempt(Box &input, bool flag)
    copy := input
    input = Box()
    if flag
        return error("bad")
    input = copy
    return input
box := Box()
box.value = 7
Box | error result := attempt(&box, true)
match result
    Box value
        void
    error issue
        void
println(box.value)
'''),
('constructor_error_data_keeps_invalidation', False, '''class Box()
    int value
class Trial(local Box &input, local bool flag) | error
    local Box copy := input
    input = Box()
    if flag
        return error("bad")
    input = copy
box := Box()
box.value = 7
Trial | error result := Trial(&box, true)
match result
    Trial value
        void
    error issue
        void
println(box.value)
'''),
('constructor_error_before_copy', False, '''class Box()
    int value
class Trial(local Box &input, local bool flag) | error
    if flag
        input = Box()
        return error("bad")
    local Box copy := input
    input = Box()
    input = copy
box := Box()
box.value = 7
Trial | error result := Trial(&box, true)
match result
    Trial value
        void
    error issue
        void
println(box.value)
'''),
('constructor_error_expression_mutates_input', False, '''class Box()
    int value
error overwrite(Box &input)
    input = Box()
    return error("bad")
class Trial(local Box &input, local bool flag) | error
    if flag
        return overwrite(&input)
    local Box copy := input
    input = Box()
    input = copy
box := Box()
box.value = 7
Trial | error result := Trial(&box, true)
match result
    Trial value
        void
    error issue
        void
println(box.value)
'''),
('constructor_receiver_success_only', True, '''class Trial(local bool flag) | error
    int value
    if flag
        return error("bad")
    value = 7
Trial result := Trial(false)
println(result.value)
'''),
('try_error_data_keeps_invalidation', False, '''class Box()
    int value
int | error attempt(Box &input, string text)
    copy := input
    input = Box()
    int n := try int(text)
    input = copy
    return n
box := Box()
box.value = 7
int | error result := attempt(&box, "bad")
match result
    int value
        void
    error issue
        void
println(box.value)
'''),
('try_scan_error_data_keeps_invalidation', False, '''class Box()
    int value
void | error attempt(Box &input)
    copy := input
    input = Box()
    try scan(&input.value)
    input = copy
    return void
box := Box()
box.value = 7
void | error result := attempt(&box)
match result
    void
        void
    error issue
        void
println(box.value)
'''),
('constructor_try_receiver_success_only', True, '''class Trial(local string text) | error
    int value
    value = try int(text)
Trial result := Trial("7")
println(result.value)
'''),
('demand_then_unknown_alias_invalidates_copy', False, '''class Box()
    int value
int forward(Box &input, bool flag)
    copy := input
    old := copy.value
    other := Box()
    &alias := &copy
    if flag
        &alias = &other
    alias = Box()
    return copy.value + old
box := Box()
box.value = 7
println(forward(&box, false))
'''),
('demand_then_loop_may_replace_copy', False, '''class Box()
    int value
int forward(Box &input, bool flag)
    copy := input
    old := copy.value
    while flag
        copy = Box()
        break
    return copy.value + old
box := Box()
box.value = 7
println(forward(&box, true))
'''),
('aliased_restore_partial_does_not_initialize', False, '''class Box()
    int value
void restore(Box &left, Box &right)
    copy := left
    right.value = 9
    left = copy
box := Box()
restore(&box, &box)
println(box.value)
'''),
('aliased_restore_complete_preserves', True, '''class Box()
    int value
void restore(Box &left, Box &right)
    copy := left
    right.value = 9
    left = copy
box := Box()
box.value = 7
restore(&box, &box)
println(box.value)
'''),
('aliased_class_write_only_initializes', True, '''class Box()
    int value
void initialize(Box &left, Box &right)
    left.value = 1
    right.value = 2
box := Box()
initialize(&box, &box)
println(box.value)
'''),
('aliased_scalar_write_only_initializes', True, '''void initialize(int &left, int &right)
    left = 1
    right = 2
int value
initialize(&value, &value)
println(value)
'''),
('ordinary_unknown_alias_stale_fact', False, '''class Box()
    int value
int forward(const Box input, bool flag)
    copy := input
    other := Box()
    &alias := &copy
    if flag
        &alias = &other
    alias = Box()
    return copy.value
box := Box()
box.value = 7
println(forward(box, false))
'''),
('recopy_then_unknown_alias_invalidates', False, '''class Box()
    int value
int forward(Box &input, bool flag)
    copy := input
    old := copy.value
    second := copy
    other := Box()
    &alias := &second
    if flag
        &alias = &other
    alias = Box()
    return second.value + old
box := Box()
box.value = 7
println(forward(&box, false))
'''),
('recopy_then_loop_invalidates', False, '''class Box()
    int value
int forward(Box &input, bool flag)
    copy := input
    old := copy.value
    second := copy
    while flag
        second = Box()
        break
    return second.value + old
box := Box()
box.value = 7
println(forward(&box, true))
'''),
('complete_call_then_unknown_alias_invalidates', False, '''class Box()
    int value
Box identity(const Box input)
    return input
int forward(Box &input, bool flag)
    copy := input
    old := copy.value
    second := identity(copy)
    other := Box()
    &alias := &second
    if flag
        &alias = &other
    alias = Box()
    return second.value + old
box := Box()
box.value = 7
println(forward(&box, false))
'''),
('complete_call_then_loop_invalidates', False, '''class Box()
    int value
Box identity(const Box input)
    return input
int forward(Box &input, bool flag)
    copy := input
    old := copy.value
    second := identity(copy)
    while flag
        second = Box()
        break
    return second.value + old
box := Box()
box.value = 7
println(forward(&box, true))
'''),
('copy_demand_before_scalar_loop', True, '''class Box()
    int value
int forward(Box &input)
    copy := input
    old := copy.value
    for: i < 2
        input.value = i
    return old
box := Box()
box.value = 7
println(forward(&box))
'''),
('extent_call_loop_replacement', False, '''class Box()
    int value
Box identity(const Box input)
    return input
nat reset(Box &input)
    input = Box()
    return 1
int forward(Box &input, bool flag)
    copy := input
    old := copy.value
    second := identity(copy)
    while flag
        int[reset(&second)] values := [0]
        flag = false
    return second.value + old
box := Box()
box.value = 7
println(forward(&box, true))
'''),
('complete_call_then_loop_method_invalidates', False, '''class Box()
    int value
class Holder()
    child := Box()
    void .clear()
        child = Box()
Holder identity(const Holder input)
    return input
int forward(Holder &input, bool flag)
    copy := input
    old := copy.child.value
    second := identity(copy)
    while flag
        .second.clear()
        flag = false
    return second.child.value + old
input := Holder()
input.child.value = 7
println(forward(&input, true))
'''),
('complete_call_then_loop_method_argument_invalidates', False, '''class Box()
    int value
Box identity(const Box input)
    return input
class Clearer()
    void clear_other(Box &target)
        target = Box()
int forward(Box &input, bool flag)
    copy := input
    old := copy.value
    second := identity(copy)
    clearer := Clearer()
    while flag
        clearer.clear_other(&second)
        flag = false
    return second.value + old
input := Box()
input.value = 7
println(forward(&input, true))
'''),
('comprehension_value_replaces_class', False, '''class Box()
    int value
Box identity(const Box input)
    return input
int read_clear(Box &input)
    old := input.value
    input = Box()
    return old
int forward(Box &input)
    copy := input
    old := copy.value
    second := identity(copy)
    int[] generated := [for i := [0, 1][] then read_clear(&second)]
    return old + int(generated.len())
input := Box()
input.value = 7
println(forward(&input))
'''),
('comprehension_condition_replaces_class', False, '''class Box()
    int value
Box identity(const Box input)
    return input
bool read_clear(Box &input)
    old := input.value
    input = Box()
    return old > 0
int forward(Box &input)
    copy := input
    old := copy.value
    second := identity(copy)
    int[] generated := [for i := [0, 1][] if read_clear(&second) then i]
    return old + int(generated.len())
input := Box()
input.value = 7
println(forward(&input))
'''),
('while_condition_replaces_class', False, '''class Box()
    int value
Box identity(const Box input)
    return input
bool read_clear(Box &input)
    old := input.value
    input = Box()
    return old > 0
int forward(Box &input)
    copy := input
    old := copy.value
    second := identity(copy)
    count := 0
    while read_clear(&second)
        count += 1
    return old + count
input := Box()
input.value = 7
println(forward(&input))
'''),
('for_iterable_class_mutation_once', True, '''class Box()
    int value
Box identity(const Box input)
    return input
int[] source(Box &input)
    old := input.value
    input = Box()
    return [old]
int forward(Box &input)
    copy := input
    old := copy.value
    second := identity(copy)
    for i := source(&second)[]
        println(i)
    return old
input := Box()
input.value = 7
println(forward(&input))
'''),
('comprehension_source_class_mutation_once', True, '''class Box()
    int value
Box identity(const Box input)
    return input
int[] source(Box &input)
    old := input.value
    input = Box()
    return [old]
int forward(Box &input)
    copy := input
    old := copy.value
    second := identity(copy)
    int[] generated := [for i := source(&second)[] then i]
    return old + int(generated.len())
input := Box()
input.value = 7
println(forward(&input))
'''),
('early_unchanged_exit_preserves_identity', True, '''class Box()
    int value
bool preserve(Box &input, bool flag)
    if flag
        return false
    copy := input
    input = Box()
    input = copy
    return true
box := Box()
box.value = 7
preserve(&box, false)
println(box.value)
'''),
('early_partial_exit_blocks_identity', False, '''class Box()
    int value
bool preserve(Box &input, bool flag)
    if flag
        input = Box()
        return false
    copy := input
    input = Box()
    input = copy
    return true
box := Box()
box.value = 7
preserve(&box, true)
println(box.value)
'''),
 ]
for name,want,source in cases:
    path=tmp/("copy-"+name+".qui");path.write_text(source)
    result=subprocess.run([quidra,"check",str(path),"--json"],capture_output=True,text=True)
    assert (result.returncode==0)==want,(name,result.stdout,result.stderr)
    if not want:
        assert any(d["code"].startswith("UNINITIALIZED") for d in json.loads(result.stdout)["diagnostics"]),(name,result.stdout)
PYCOPY
cat > "$TMP/copy-entry-runtime.qui" <<'QUI'
class Box()
    int value
int snapshot(Box &input)
    copy := input
    input.value = 9
    return copy.value
void preserve(Box &input)
    copy := input
    input = Box()
    input = copy
box := Box()
box.value = 7
println(snapshot(&box))
println(box.value)
preserve(&box)
println(box.value)
QUI
[[ "$("$QUIDRA" run "$TMP/copy-entry-runtime.qui")" == $'7\n9\n9' ]]

# Ordinary receiver aliases and guarded class-array element aliases retain
# live initialization requirements without certifying partial replacements.
python3 - "$QUIDRA" "$TMP" <<'PYALIASES'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
cases=[
('local-complete', None, '''class Box(int value)
int read()
    Box[] items := [Box(7)]
    &item := &items[0]
    return item.value
println(read())
'''),
('receiver-complete', None, '''class Box(int value)
class Owner()
    Box[] items := [Box(7)]
    int read()
        &item := &items[0]
        return item.value
println(Owner().read())
'''),
('reference-parameter-complete', None, '''class Box(int value)
class State(Box[] items)
int read(State &state)
    &item := &state.items[0]
    Box copied := item
    return copied.value
state := State([Box(7)])
println(read(&state))
'''),
('alias-rebind-complete', None, '''class Box(int value)
int read()
    Box[] first := [Box(7)]
    Box[] second := [Box(9)]
    &item := &first[0]
    &item = &second[0]
    return item.value
println(read())
'''),
('fixed-never-read', 'UNINITIALIZED', '''class Box(int value)
int read()
    Box[1] items
    &item := &items[0]
    return item.value
println(read())
'''),
('fixed-other-slot-negative', 'UNINITIALIZED', '''class Box(int value)
int read()
    Box[2] items
    &first := &items[0]
    first = Box(7)
    &second := &items[1]
    return second.value
println(read())
'''),
('fixed-write-read', None, '''class Box(int value)
int read()
    Box[1] items
    &item := &items[0]
    item = Box(7)
    return item.value
println(read())
'''),
('local-whole-replace-negative', 'UNINITIALIZED', '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
int read()
    Box[] items := [make_box()]
    &item := &items[0]
    item = Box()
    return items[0].value
println(read())
'''),
('local-child-replace-negative', 'UNINITIALIZED', '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
class Entry(Box child)
int read()
    Entry[] items := [Entry(make_box())]
    &item := &items[0]
    item.child = Box()
    return items[0].child.value
println(read())
'''),
('callee-replace-negative', 'UNINITIALIZED', '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
void overwrite(Box &input)
    input = Box()
int read()
    Box[] items := [make_box()]
    &item := &items[0]
    overwrite(&item)
    return items[0].value
println(read())
'''),
('parent-complete-to-unfilled', None, '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
int read()
    Box[] items := [make_box()]
    &item := &items[0]
    items = array(1)
    return item.value
println(read())
'''),
('parent-partial-to-complete-negative', 'UNINITIALIZED', '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
int read()
    Box[] items := [make_box()]
    &item := &items[0]
    item = Box()
    items = [make_box()]
    return item.value
println(read())
'''),
('parent-unfilled-to-complete-negative', 'UNINITIALIZED', '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
int read()
    Box[] items := array(1)
    &item := &items[0]
    items = [make_box()]
    return item.value
println(read())
'''),
('receiver-parent-complete-to-unfilled', None, '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
class Owner()
    Box[] items := [make_box()]
    int .read()
        &item := &items[0]
        items = array(1)
        return item.value
owner := Owner()
println(.owner.read())
'''),
('receiver-parent-partial-to-complete-negative', 'UNINITIALIZED', '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
class Owner()
    Box[] items := [make_box()]
    int .read()
        &item := &items[0]
        item = Box()
        items = [make_box()]
        return item.value
owner := Owner()
println(.owner.read())
'''),
('nested-index-effects', None, '''class State()
    int value
int next(State &state)
    old := state.value
    state = State()
    return old
class Box(int value)
int read()
    Box[][] rows := [[Box(7)]]
    state := State()
    state.value = 0
    &item := &rows[next(&state)][0]
    return item.value
println(read())
'''),
('unknown-array-poison-negative', 'UNINITIALIZED', '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
int read(bool choose_first)
    Box[] first := [make_box()]
    Box[] second := [make_box()]
    Box[] &items
    if choose_first
        &items = &first
    else
        &items = &second
    &item := &items[0]
    item = Box()
    return items[0].value
println(read(true))
'''),
('captured-nested-parent-refill-negative', 'UNINITIALIZED', '''class Child()
    int value
Child make_child()
    child := Child()
    child.value = 7
    return child
class Stage(Child[] children)
class State(Stage[] stages)
int read(State &state)
    &stage := &state.stages[0]
    &child := &stage.children[0]
    child = Child()
    state.stages = [Stage([make_child()])]
    state.stages[0].children[0].value = 9
    return stage.children[0].value
state := State([Stage([make_child()])])
println(read(&state))
'''),
('04_receiver_field', None, '''class Box(int value)
class Owner()
    Box box := Box(7)
    int read()
        &item := &box
        return item.value
println(Owner().read())
'''),
('20_receiver_scalar_positive', None, '''class Owner()
    int value := 7
    int read()
        &item := &value
        return item
println(Owner().read())
'''),
('21_receiver_rebind_positive', None, '''class Box(int value)
class Owner()
    Box first := Box(7)
    Box second := Box(9)
    int read()
        &item := &first
        &item = &second
        return item.value
println(Owner().read())
'''),
('15_receiver_dot_negative', 'MUTATION_MARKER', '''class Owner()
    int value := 7
    void change()
        &item := &value
        item = 9
Owner().change()
'''),
('19_receiver_const_negative', 'REFERENCE_BINDING', '''class Owner()
    const int value := 7
    void .change()
        &item := &value
        item = 9
owner := Owner()
.owner.change()
'''),
('27_own_reference_alias_replace_negative', 'REFERENCE_TARGET', '''class Box()
    int value
class Holder this(local Box &input)
    Box &target
    &target = &input
    void .erase()
        &alias := &target
        alias = Box()
box := Box()
box.value = 7
holder := Holder(&box)
.holder.erase()
'''),
('28_reference_alias_dot_negative', 'MUTATION_MARKER', '''class Holder this(local int &input)
    int &target
    &target = &input
    void change()
        &alias := &target
        alias = 9
value := 7
holder := Holder(&value)
holder.change()
'''),
('29_nested_reference_private_negative', 'PRIVATE_MEMBER', '''class Holder this(local int &input)
    private int &target
    &target = &input
class Owner(local int &input)
    Holder holder := Holder(&input)
    void .change()
        &alias := &holder.target
        alias = 9
value := 7
owner := Owner(&value)
.owner.change()
'''),
('30_reference_const_negative', 'REFERENCE_BINDING', '''class Holder this(local int &input)
    int &target
    &target = &input
value := 7
const Holder holder := Holder(&value)
&alias := &holder.target
alias = 9
'''),
('31_indirect_reference_alias_replace_negative', 'REFERENCE_TARGET', '''class Box()
    int value
class Holder this(local Box &input)
    Box &target
    &target = &input
class Owner(local Box &input)
    Holder holder := Holder(&input)
    void .erase()
        &own := &holder
        &alias := &own.target
        alias = Box()
box := Box()
box.value = 7
owner := Owner(&box)
.owner.erase()
'''),
('32_indirect_reference_rebind_negative', 'REFERENCE_TARGET', '''class Box()
    int value
class Holder this(local Box &input)
    Box &target
    &target = &input
class Owner(local Box &input)
    Holder holder := Holder(&input)
    void .erase()
        &own := &holder
        &alias := &own.target
        &alias = &own.target
        alias = Box()
box := Box()
box.value = 7
owner := Owner(&box)
.owner.erase()
'''),
('35_whole_array_alias_child_replace_negative', 'UNINITIALIZED', '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
class Entry(Box child)
class Owner()
    Entry[] items := [Entry(make_box())]
    void .change()
        &array_alias := &items
        array_alias[0].child = Box()
        println(items[0].child.value)
owner := Owner()
.owner.change()
'''),
('scalar-nested-capture-parent-refill-negative', 'UNINITIALIZED', '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
class Holder(Box child)
class Owner()
    Holder holder := Holder(make_box())
    int .read()
        &item := &holder.child
        item = Box()
        holder = Holder(make_box())
        return item.value
owner := Owner()
println(.owner.read())
'''),
('scalar-direct-field-nested-read', None, '''class Box(int value)
class Holder(Box child)
class Owner()
    Holder holder := Holder(Box(7))
    int read()
        &item := &holder
        return item.child.value
println(Owner().read())
'''),
 ]
for name,code,source in cases:
    path=tmp/("alias-"+name+".qui");path.write_text(source)
    result=subprocess.run([quidra,"check",str(path),"--json"],capture_output=True,text=True)
    assert (result.returncode==0)==(code is None),(name,result.stdout,result.stderr)
    if code is not None:
        diagnostics=json.loads(result.stdout)["diagnostics"]
        assert any(d["code"].startswith(code) for d in diagnostics),(name,result.stdout)
# These programs exercise the existing native LoadReference initialization
# guard and AddressElement bounds guard, not only checker acceptance.
runtime_cases=[
('local-complete', 0, '7\n', None, '''class Box(int value)
int read()
    Box[] items := [Box(7)]
    &item := &items[0]
    return item.value
println(read())
'''),
('receiver-complete', 0, '7\n', None, '''class Box(int value)
class Owner()
    Box[] items := [Box(7)]
    int read()
        &item := &items[0]
        return item.value
println(Owner().read())
'''),
('reference-parameter-complete', 0, '7\n', None, '''class Box(int value)
class State(Box[] items)
int read(State &state)
    &item := &state.items[0]
    Box copied := item
    return copied.value
state := State([Box(7)])
println(read(&state))
'''),
('fixed-write-read', 0, '7\n', None, '''class Box(int value)
int read()
    Box[1] items
    &item := &items[0]
    item = Box(7)
    return item.value
println(read())
'''),
('parent-complete-to-unfilled', 0, '7\n', None, '''class Box()
    int value
Box make_box()
    box := Box()
    box.value = 7
    return box
int read()
    Box[] items := [make_box()]
    &item := &items[0]
    items = array(1)
    return item.value
println(read())
'''),
('nested-index-effects', 0, '7\n', None, '''class State()
    int value
int next(State &state)
    old := state.value
    state = State()
    return old
class Box(int value)
int read()
    Box[][] rows := [[Box(7)]]
    state := State()
    state.value = 0
    &item := &rows[next(&state)][0]
    return item.value
println(read())
'''),
('bounds-dynamic', 101, '', 'INDEX_BOUNDS', '''class Box(int value)
int read(int index)
    Box[] items := [Box(7)]
    &item := &items[index]
    return item.value
println(read(1))
'''),
('parameter-unfilled-runtime', 101, '', 'UNINITIALIZED', '''class Box(int value)
int read(Box[] &items, int index)
    &item := &items[index]
    return item.value
Box[] items := array(1)
println(read(&items, 0))
'''),
 ]
for name,status,output,code,source in runtime_cases:
    path=tmp/("alias-runtime-"+name+".qui");path.write_text(source)
    executable=tmp/("alias-runtime-"+name)
    built=subprocess.run([quidra,"build",str(path),"-o",str(executable)],capture_output=True,text=True)
    assert built.returncode==0,(name,built.stdout,built.stderr)
    run=subprocess.run([str(executable)],capture_output=True,text=True)
    assert run.returncode==status and run.stdout==output,(name,run.returncode,run.stdout,run.stderr)
    if code is not None:
        assert code in run.stderr,(name,run.stderr)
PYALIASES

# Existing class initialization suite append proposal; source frozen.

cat > "$TMP/copy-safe-loop-0.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point value)
void use(Holder &holder, bool[] flags)
    Point copied := holder.value
    println(copied.x)
    for flag := flags[]
        if flag
            println(1)
    println(copied.x)
Holder holder := Holder(Point(1, 2))
use(&holder, [])
QUI
"$QUIDRA" check "$TMP/copy-safe-loop-0.qui"
"$QUIDRA" run "$TMP/copy-safe-loop-0.qui" > "$TMP/copy-safe-loop-0.out"
[[ "$(cat "$TMP/copy-safe-loop-0.out")" == $'1\n1' ]]

cat > "$TMP/copy-safe-loop-1.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point value)
class Container(Holder holder)
    int use(bool[] flags)
        Point copied := holder.value
        for flag := flags[]
            if flag
                println(1)
        return copied.x
Container container := Container(Holder(Point(1, 2)))
println(container.use([]))
QUI
"$QUIDRA" check "$TMP/copy-safe-loop-1.qui"
"$QUIDRA" run "$TMP/copy-safe-loop-1.qui" > "$TMP/copy-safe-loop-1.out"
[[ "$(cat "$TMP/copy-safe-loop-1.out")" == "1" ]]

cat > "$TMP/copy-unsafe-loop-0.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
class Holder(Point value)
void use(Holder &holder, bool[] flags)
    Point copied := holder.value
    println(copied.y)
    for flag := flags[]
        if flag
            copied = Point(9)
    println(copied.y)
Point value := Point(1)
value.y = 2
Holder holder := Holder(value)
use(&holder, [true])
QUI
set +e
"$QUIDRA" check "$TMP/copy-unsafe-loop-0.qui" --json > "$TMP/copy-unsafe-loop-0.json"
copy_loop_rc=$?
set -e
[[ "$copy_loop_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/copy-unsafe-loop-0.json"

cat > "$TMP/copy-unsafe-loop-1.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
class Holder(Point value)
void use(Holder &holder, bool[] flags)
    Point copied := holder.value
    Point &alias := &copied
    println(copied.y)
    for flag := flags[]
        if flag
            alias = Point(9)
    println(copied.y)
Point value := Point(1)
value.y = 2
Holder holder := Holder(value)
use(&holder, [true])
QUI
set +e
"$QUIDRA" check "$TMP/copy-unsafe-loop-1.qui" --json > "$TMP/copy-unsafe-loop-1.json"
copy_loop_rc=$?
set -e
[[ "$copy_loop_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/copy-unsafe-loop-1.json"

# Normal data returns and try propagation preserve external initialization facts.
python3 - "$QUIDRA" "$TMP" <<'PYFRONTIER'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
cases=[
('01_terminated_replacement', True, '''class Box()
    int value
int consume(const Box input)
    return input.value
int forward(Box &input, bool flag)
    if flag
        input = Box()
        return 0
    return consume(input)
box := Box()
box.value = 7
println(forward(&box, false))
'''),
('02_terminated_restore', True, '''class Box()
    int value
int consume(const Box input)
    return input.value
int forward(Box &input, bool flag)
    if flag
        saved := input
        input = Box()
        input = saved
        return 0
    return consume(input)
box := Box()
box.value = 7
println(forward(&box, false))
'''),
('03_try_gated_restore', True, '''class Box()
    int value
int consume(const Box input)
    return input.value
int | error forward(Box &input, bool flag, string text)
    if flag
        saved := input
        input = Box()
        input = saved
        return 0
    n := try int(text)
    return consume(input) + n
box := Box()
box.value = 7
int | error result := forward(&box, false, "0")
'''),
('04_success_init_after_try_error', False, '''class Box()
    int value
void | error attempt(Box &input, string text)
    input = Box()
    try int(text)
    input.value = 7
    return void
box := Box()
box.value = 9
void | error result := attempt(&box, "bad")
println(box.value)
'''),
('05_preloop_receiver_copy_scalar_method', True, '''class Settings(int value)
class Owner(Settings config)
    count := 0
    void .touch()
        count += 1
    int read()
        copy := config
        replay := Owner(copy)
        for i := [0, 1][]
            .replay.touch()
        return replay.count
owner := Owner(Settings(7))
println(owner.read())
'''),
('06_preloop_receiver_copy_array_method', True, '''class Settings(int value)
class Item(int value)
class Owner(Settings config)
    Item[] items := []
    count := 0
    void .touch()
        items = items.append(Item(1))
        count += 1
    int read()
        copy := config
        replay := Owner(copy)
        for i := [0, 1][]
            .replay.touch()
        return replay.count
owner := Owner(Settings(7))
println(owner.read())
'''),
('07_terminated_replacement_caller_read_negative', False, '''class Box()
    int value
int consume(const Box input)
    return input.value
int forward(Box &input, bool flag)
    if flag
        input = Box()
        return 0
    return consume(input)
box := Box()
box.value = 7
println(forward(&box, false))
println(box.value)
'''),
('08_terminated_replacement_partial_input_negative', False, '''class Box()
    int value
int consume(const Box input)
    return input.value
int forward(Box &input, bool flag)
    if flag
        input = Box()
        return 0
    return consume(input)
box := Box()
println(forward(&box, false))
'''),
('09_preloop_invalidating_method_negative', False, '''class Settings()
    int value
class Owner(Settings config)
    count := 0
    void .touch()
        config = Settings()
    int read()
        copy := config
        replay := Owner(copy)
        for i := [0, 1][]
            .replay.touch()
        return replay.config.value
settings := Settings()
settings.value = 7
owner := Owner(settings)
println(owner.read())
'''),
('11_opaque_receiver_invalidating_negative', False, '''class Settings()
    int value
class Owner(Settings config)
    count := 0
    void .touch()
        config = Settings()
    int read(bool flag)
        copy := config
        replay := if flag then Owner(copy) else Owner(copy)
        for i := [0, 1][]
            .replay.touch()
        return replay.config.value
settings := Settings()
settings.value = 7
owner := Owner(settings)
println(owner.read(true))
'''),
('12_sibling_name_invalidating_negative', False, '''class Settings()
    int value
class Pure(Settings config)
    void .touch()
        config.value += 1
class Unsafe(Settings config)
    void .touch()
        config = Settings()
class Parent(Settings config)
    int read(bool flag)
        copy := config
        if flag
            replay := Pure(copy)
            for i := [0, 1][]
                .replay.touch()
            return replay.config.value
        else
            replay := Unsafe(copy)
            for i := [0, 1][]
                .replay.touch()
            return replay.config.value
settings := Settings()
settings.value = 7
parent := Parent(settings)
println(parent.read(false))
'''),
('13_receiver_terminated_replacement', True, '''class Box()
    int value
int consume(const Box input)
    return input.value
class Owner()
    state := Box()
    int .read(bool flag)
        if flag
            state = Box()
            return 0
        return consume(state)
owner := Owner()
owner.state.value = 7
println(.owner.read(false))
'''),
('14_receiver_caller_read_negative', False, '''class Box()
    int value
int consume(const Box input)
    return input.value
class Owner()
    state := Box()
    int .read(bool flag)
        if flag
            state = Box()
            return 0
        return consume(state)
owner := Owner()
owner.state.value = 7
println(.owner.read(false))
println(owner.state.value)
'''),
('15_copy_after_try_positive', True, '''class Box()
    int value
int | error forward(Box &input, string text)
    n := try int(text)
    copy := input
    return copy.value + n
box := Box()
box.value = 7
int | error result := forward(&box, "0")
'''),
('16_identity_before_try_positive', True, '''class Box()
    int value
int | error preserve(Box &input, string text)
    copy := input
    input = Box()
    input = copy
    return try int(text)
box := Box()
box.value = 7
int | error result := preserve(&box, "bad")
println(box.value)
'''),
('17_identity_after_try_positive', True, '''class Box()
    int value
int | error preserve(Box &input, string text)
    n := try int(text)
    copy := input
    input = Box()
    input = copy
    return n
box := Box()
box.value = 7
int | error result := preserve(&box, "bad")
println(box.value)
'''),
('18_try_operand_external_mutation_negative', False, '''class Box()
    int value
int | error overwrite(Box &input)
    input = Box()
    return error("bad")
int | error preserve(Box &input)
    copy := input
    n := try overwrite(&input)
    input = copy
    return n
box := Box()
box.value = 7
int | error result := preserve(&box)
println(box.value)
'''),
('19_constructor_try_external_negative', False, '''class Box()
    int value
class Trial(local Box &input, local string text) | error
    local Box copy := input
    input = Box()
    local int n := try int(text)
    input = copy
box := Box()
box.value = 7
Trial | error result := Trial(&box, "bad")
println(box.value)
'''),
('20_constructor_identity_before_try_positive', True, '''class Box()
    int value
class Trial(local Box &input, local string text) | error
    local Box copy := input
    input = Box()
    input = copy
    local int n := try int(text)
box := Box()
box.value = 7
Trial | error result := Trial(&box, "bad")
println(box.value)
'''),
('21_receiver_try_failure_negative', False, '''class Box()
    int value
class Owner()
    state := Box()
    void | error .attempt(string text)
        state = Box()
        try int(text)
        state.value = 7
        return void
owner := Owner()
owner.state.value = 9
void | error result := .owner.attempt("bad")
println(owner.state.value)
'''),
('22_scalar_reference_try_failure_negative', False, '''void | error attempt(int &output, string text)
    try int(text)
    output = 7
    return void
int value
void | error result := attempt(&value, "bad")
println(value)
'''),
('23_try_scan_success_only_negative', False, '''class Box()
    int value
void | error attempt(Box &input)
    try scan(&input.value)
    return void
box := Box()
void | error result := attempt(&box)
println(box.value)
'''),
('24_constructor_try_receiver_success_positive', True, '''class Trial(local string text) | error
    int value
    value = try int(text)
Trial result := Trial("7")
println(result.value)
'''),
('25_scan_address_reset_negative', False, '''class Box()
    int[] values := [0]
    int value
nat reset(Box &input)
    input = Box()
    return 0
void | error attempt(Box &input)
    input.value = 7
    try scan("{&input.values[reset(&input)]} {&input.value}")
    return void
box := Box()
box.value = 9
void | error result := attempt(&box)
println(box.value)
'''),
('26_scan_unknown_alias_reset_negative', False, '''class Box()
    int[] values := [0]
    int value
nat reset(Box &input)
    input = Box()
    return 0
void | error attempt(Box &input, bool flag)
    scan(&input.value)
    other := Box()
    &alias := &input
    if flag
        &alias = &other
    try scan("{&input.values[reset(&alias)]} {&input.value}")
    return void
box := Box()
void | error result := attempt(&box, false)
println(box.value)
'''),
 ]
for name,want,source in cases:
    path=tmp/("exit-"+name+".qui");path.write_text(source)
    result=subprocess.run([quidra,"check",str(path),"--json"],capture_output=True,text=True)
    assert (result.returncode==0)==want,(name,result.stdout,result.stderr)
    if not want:
        assert any(d["code"].startswith("UNINITIALIZED") for d in json.loads(result.stdout)["diagnostics"]),(name,result.stdout)
PYFRONTIER
cat > "$TMP/try-entry-runtime.qui" <<'QUI'
class Box()
    int value
int | error preserve(Box &input, string text)
    copy := input
    input = Box()
    input = copy
    return try int(text)
class Trial(local Box &input, local string text) | error
    local Box copy := input
    input = Box()
    input = copy
    local int number := try int(text)
box := Box()
box.value = 7
int | error result := preserve(&box, "bad")
match result
    int value
        println(value)
    error issue
        println(box.value)
Trial | error attempt := Trial(&box, "bad")
match attempt
    Trial value
        println(0)
    error issue
        println(box.value)
QUI
"$QUIDRA" run "$TMP/try-entry-runtime.qui" > "$TMP/try-entry-runtime.out"
printf '7\n7\n' > "$TMP/try-entry-runtime.expected"
diff -u "$TMP/try-entry-runtime.expected" "$TMP/try-entry-runtime.out"

# Class reference arguments load initialized array slots at their use.
cat > "$TMP/array-address-complete.qui" <<'QUI'
class Box(int value)
void bump(Box &box)
    box.value += 1
items := [Box(7)]
bump(&items[0])
println(items[0].value)
QUI
cat > "$TMP/array-address-unfilled.qui" <<'QUI'
class Box(int value)
void bump(Box &box)
    box.value += 1
Box[1] items
bump(&items[0])
QUI
"$QUIDRA" check "$TMP/array-address-complete.qui"
"$QUIDRA" run "$TMP/array-address-complete.qui" > "$TMP/array-address-complete.out"
[[ "$(cat "$TMP/array-address-complete.out")" == "8" ]]
"$QUIDRA" check "$TMP/array-address-unfilled.qui"
"$QUIDRA" build "$TMP/array-address-unfilled.qui" -o "$TMP/array-address-unfilled"
set +e
"$TMP/array-address-unfilled" > "$TMP/array-address-unfilled.out" 2> "$TMP/array-address-unfilled.err"
array_address_rc=$?
set -e
[[ "$array_address_rc" -eq 101 ]]
grep -q 'UNINITIALIZED' "$TMP/array-address-unfilled.err"

# Direct reference formals use their caller's tracked array slot for both
# reads and whole writes, including primitive slots and nested Array cells.
cat > "$TMP/array-address-fixed-filled.qui" <<'QUI'
class Box(int value)
void bump(Box &box)
    box.value += 1
Box[1] items := [Box(7)]
bump(&items[0])
println(items[0].value)
QUI
"$QUIDRA" build "$TMP/array-address-fixed-filled.qui" -o "$TMP/array-address-fixed-filled"
[[ "$("$TMP/array-address-fixed-filled")" == "8" ]]
cat > "$TMP/array-address-fixed-write-only.qui" <<'QUI'
class Box(int value)
void fill(Box &box)
    box = Box(7)
void bump(Box &box)
    box.value += 1
Box[1] items
fill(&items[0])
bump(&items[0])
println(items[0].value)
QUI
"$QUIDRA" build "$TMP/array-address-fixed-write-only.qui" -o "$TMP/array-address-fixed-write-only"
[[ "$("$TMP/array-address-fixed-write-only")" == "8" ]]
cat > "$TMP/array-address-array-row-write-only.qui" <<'QUI'
void fill(int[] &row)
    row = [7]
int[][] rows := array(1)
fill(&rows[0])
println(rows[0][0])
QUI
"$QUIDRA" build "$TMP/array-address-array-row-write-only.qui" -o "$TMP/array-address-array-row-write-only"
[[ "$("$TMP/array-address-array-row-write-only")" == "7" ]]
cat > "$TMP/array-address-array-partial-fill.qui" <<'QUI'
void fill(int[] &values)
    values[0] = 7
int[] values := array(1)
fill(&values)
println(values[0])
QUI
"$QUIDRA" build "$TMP/array-address-array-partial-fill.qui" -o "$TMP/array-address-array-partial-fill"
[[ "$("$TMP/array-address-array-partial-fill")" == "7" ]]
cat > "$TMP/array-address-fixed-int-unfilled.qui" <<'QUI'
void bump(int &value)
    value += 1
int[1] items
bump(&items[0])
QUI
cat > "$TMP/array-address-heap-unfilled.qui" <<'QUI'
class Box(int value)
void bump(Box &box)
    box.value += 1
Box[] items := array(1)
bump(&items[0])
QUI
cat > "$TMP/array-address-alias-unfilled.qui" <<'QUI'
class Box(int value)
void bump(Box &box)
    Box &alias := &box
    alias.value += 1
Box[1] items
bump(&items[0])
QUI
for name in fixed-int-unfilled heap-unfilled alias-unfilled; do
    "$QUIDRA" build "$TMP/array-address-$name.qui" -o "$TMP/array-address-$name"
    set +e
    "$TMP/array-address-$name" > "$TMP/array-address-$name.out" 2> "$TMP/array-address-$name.err"
    array_address_rc=$?
    set -e
    [[ "$array_address_rc" -eq 101 && ! -s "$TMP/array-address-$name.out" ]]
    grep -q 'UNINITIALIZED' "$TMP/array-address-$name.err"
done


# Scan error states are captured after all addresses and before success marks.
# Entry receipts may be consumed inside modeled loops; repeated rebinds gate them.
python3 - "$QUIDRA" "$TMP" <<'PYBOUNDARY'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
cases=[
('27_loop_rebind_promoted_copy_negative', False, '''class Box()
    int value
int forward(Box &input)
    copy := input
    old := copy.value
    other := Box()
    &alias := &copy
    for i := [0, 1][]
        println(alias.value)
        &alias = &other
    return old
box := Box()
box.value = 7
println(forward(&box))
'''),
('28_scan_later_address_reset_negative', False, '''class Box()
    int[] values := [0]
    int value
nat reset(Box &input)
    input = Box()
    return 0
void | error attempt(Box &input)
    try scan("{&input.value} {&input.values[reset(&input)]}")
    return void
box := Box()
box.value = 9
void | error result := attempt(&box)
println(box.value)
'''),
('29_failfast_scan_initializes_positive', True, '''class Box()
    int value
int forward(Box &input)
    scan(&input.value)
    return input.value
box := Box()
println(forward(&box))
println(box.value)
'''),
('30_nested_try_scan_address_negative', False, '''class Box()
    int[] values := [0]
    int value
nat | error index(string text)
    parsed := try int(text)
    return nat(parsed)
void | error attempt(Box &input, string text)
    input = Box()
    try scan("{&input.values[try index(text)]} {&input.value}")
    return void
box := Box()
box.value = 9
void | error result := attempt(&box, "bad")
println(box.value)
'''),
('31_loop_scalar_field_rebind_negative', False, '''class Box()
    int value
int forward(Box &input)
    copy := input
    old := copy.value
    other := Box()
    &alias := &copy.value
    for i := [0, 1][]
        println(alias)
        &alias = &other.value
    return old
box := Box()
box.value = 7
println(forward(&box))
'''),
('32_loop_first_existing_receipt_positive', True, '''class Box()
    int value
int forward(Box &input)
    copy := input
    result := 0
    for i := [0, 1][]
        result += copy.value
    return result
box := Box()
box.value = 7
println(forward(&box))
'''),
('33_loop_first_existing_receipt_partial_negative', False, '''class Box()
    int value
int forward(Box &input)
    copy := input
    result := 0
    for i := [0, 1][]
        result += copy.value
    return result
box := Box()
println(forward(&box))
'''),
('34_loop_first_receipt_rebind_negative', False, '''class Box()
    int value
int forward(Box &input)
    copy := input
    other := Box()
    &alias := &copy
    for i := [0, 1][]
        println(alias.value)
        &alias = &other
    return 0
box := Box()
box.value = 7
println(forward(&box))
'''),
('35_loop_new_copy_late_init_negative', False, '''class Box()
    int value
int forward(Box &input)
    result := 0
    for i := [0, 1][]
        copy := input
        input.value = 7
        result += copy.value
    return result
box := Box()
println(forward(&box))
'''),
('36_try_scalar_write_all_exits_positive', True, '''class Parser()
    int position
int | error number(Parser &input, string text)
    input.position += 1
    return int(text)
void | error parse(Parser &input, string text)
    input.position = 0
    try number(&input, text)
    return void
int outer(Parser &input, string text)
    void | error result := parse(&input, text)
    return input.position
parser := Parser()
parser.position = 7
println(outer(&parser, "bad"))
'''),
('37_try_callee_partial_error_negative', False, '''class Parser()
    int position
int | error number(Parser &input, string text)
    input = Parser()
    return int(text)
void | error parse(Parser &input, string text)
    input.position = 0
    try number(&input, text)
    input.position = 7
    return void
int outer(Parser &input, string text)
    void | error result := parse(&input, text)
    return input.position
parser := Parser()
parser.position = 7
println(outer(&parser, "bad"))
'''),
('38_constructor_scan_receiver_success_positive', True, '''class Trial() | error
    int value
    try scan(&value)
Trial result := Trial()
println(result.value)
'''),
('39_constructor_scan_external_negative', False, '''class Box()
    int[] values := [0]
    int value
nat reset(Box &input)
    input = Box()
    return 0
class Trial(local Box &input) | error
    own := 7
    try scan("{&input.values[reset(&input)]} {&input.value}")
box := Box()
box.value = 9
Trial | error result := Trial(&box)
println(box.value)
'''),
('40_nested_try_scan_preserves_initialized_positive', True, '''class Parser()
    int[] values := [0]
    int position
nat | error index(string text)
    parsed := try int(text)
    return nat(parsed)
void | error fill(Parser &input, string text)
    input.position = 7
    try scan("{&input.values[try index(text)]} {&input.position}")
    return void
int outer(Parser &input, string text)
    void | error result := fill(&input, text)
    return input.position
parser := Parser()
println(outer(&parser, "bad"))
'''),
('41_conditional_scan_error_partial_negative', False, '''class Box()
    int value
void | error attempt(Box &input, bool flag)
    try (if flag then scan(&input.value) else error("bad"))
    return void
box := Box()
void | error result := attempt(&box, false)
println(box.value)
'''),
 ]
for name,want,source in cases:
    path=tmp/("boundary-"+name+".qui");path.write_text(source)
    result=subprocess.run([quidra,"check",str(path),"--json"],capture_output=True,text=True)
    assert (result.returncode==0)==want,(name,result.stdout,result.stderr)
    if not want:
        assert any(d["code"].startswith("UNINITIALIZED") for d in json.loads(result.stdout)["diagnostics"]),(name,result.stdout)
PYBOUNDARY
"$QUIDRA" run "$TMP/boundary-36_try_scalar_write_all_exits_positive.qui" > "$TMP/boundary-scalar-runtime.out"
printf '1\n' > "$TMP/boundary-scalar-runtime.expected"
diff -u "$TMP/boundary-scalar-runtime.expected" "$TMP/boundary-scalar-runtime.out"
"$QUIDRA" run "$TMP/boundary-40_nested_try_scan_preserves_initialized_positive.qui" > "$TMP/boundary-nested-runtime.out"
printf '7\n' > "$TMP/boundary-nested-runtime.expected"
diff -u "$TMP/boundary-nested-runtime.expected" "$TMP/boundary-nested-runtime.out"


# shared-tests-proposed.patch: initialization requirements retain only proved field facts.
python3 - "$QUIDRA" "$TMP" <<'PYSHARED'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
cases=[
('01_deep_value_read_positive', True, '''class Leaf(int value)
class Middle(Leaf child)
class Outer(Middle branch)
int consume(const Leaf input)
    return input.value
int forward(Outer &input)
    return consume(input.branch.child)
outer := Outer(Middle(Leaf(7)))
println(forward(&outer))
'''),
('02_shallow_value_read_positive', True, '''class Leaf(int value)
class Outer(Leaf child)
int consume(const Leaf input)
    return input.value
int forward(Outer &input)
    return consume(input.child)
outer := Outer(Leaf(7))
println(forward(&outer))
'''),
('03_deep_partial_input_negative', False, '''class Leaf()
    int value
class Middle()
    child := Leaf()
class Outer()
    branch := Middle()
int consume(const Leaf input)
    return input.value
int forward(Outer &input)
    return consume(input.branch.child)
outer := Outer()
println(forward(&outer))
'''),
('04_deep_parent_replacement_negative', False, '''class Leaf()
    int value
class Middle()
    child := Leaf()
class Outer()
    branch := Middle()
int consume(const Leaf input)
    return input.value
int forward(Outer &input)
    input.branch = Middle()
    return consume(input.branch.child)
outer := Outer()
outer.branch.child.value = 7
println(forward(&outer))
'''),
('05_deep_partial_storage_initialized_positive', True, '''class Leaf()
    int value
class Middle()
    child := Leaf()
class Outer()
    branch := Middle()
int consume(const Leaf input)
    return input.value
int forward(Outer &input)
    return consume(input.branch.child)
outer := Outer()
outer.branch.child.value = 7
println(forward(&outer))
'''),
('06_conditional_scalar_write_preserves_receipt_positive', True, '''class State()
    int value
void maybe_set(State &input, bool flag)
    if flag
        input.value = 9
int forward(State &input, bool flag)
    copy := input
    maybe_set(&copy, flag)
    return copy.value
state := State()
state.value = 7
println(forward(&state, false))
'''),
('07_conditional_write_partial_input_negative', False, '''class State()
    int value
void maybe_set(State &input, bool flag)
    if flag
        input.value = 9
int forward(State &input, bool flag)
    copy := input
    maybe_set(&copy, flag)
    return copy.value
state := State()
println(forward(&state, false))
'''),
('08_definite_write_initializes_partial_positive', True, '''class State()
    int value
void set(State &input)
    input.value = 9
int forward(State &input)
    copy := input
    set(&copy)
    return copy.value
state := State()
println(forward(&state))
'''),
('09_partial_class_write_invalidates_receipt_negative', False, '''class State()
    int value
void clear(State &input)
    input = State()
int forward(State &input)
    copy := input
    clear(&copy)
    return copy.value
state := State()
state.value = 7
println(forward(&state))
'''),
('10_conditional_sibling_write_preserves_other_positive', True, '''class State()
    int first
    int second
void maybe_set(State &input, bool flag)
    if flag
        input.first = 9
int forward(State &input, bool flag)
    copy := input
    maybe_set(&copy, flag)
    return copy.second
state := State()
state.second = 7
println(forward(&state, false))
'''),
('11_compose_identity_after_conditional_write_positive', True, '''class State()
    int value
void maybe_set(State &input, bool flag)
    if flag
        input.value = 9
void preserve(State &input, bool flag)
    copy := input
    input = State()
    input = copy
    maybe_set(&input, flag)
state := State()
state.value = 7
preserve(&state, false)
println(state.value)
'''),
('12_compose_partial_after_conditional_write_negative', False, '''class State()
    int value
void maybe_set(State &input, bool flag)
    if flag
        input.value = 9
void preserve(State &input, bool flag)
    copy := input
    input = State()
    input = copy
    maybe_set(&input, flag)
state := State()
preserve(&state, false)
println(state.value)
'''),
('13_new_receiver_receipt_inside_loop_positive', True, '''class State(int value)
class Owner(State state)
    int read()
        result := 0
        for i := [0, 1][]
            copy := state
            result += copy.value
        return result
owner := Owner(State(7))
println(owner.read())
'''),
('14_new_parameter_receipt_inside_loop_positive', True, '''class State()
    int value
int forward(State &input)
    result := 0
    for i := [0, 1][]
        copy := input
        result += copy.value
    return result
state := State()
state.value = 7
println(forward(&state))
'''),
('15_new_loop_receipt_cannot_use_later_scalar_init_negative', False, '''class State()
    int value
int forward(State &input)
    result := 0
    for i := [0, 1][]
        copy := input
        input.value = 7
        result += copy.value
    return result
state := State()
println(forward(&state))
'''),
('16_local_partial_constructor_loop_negative', False, '''class State()
    int value
int forward()
    result := 0
    for i := [0, 1][]
        partial := State()
        copy := partial
        result += copy.value
    return result
println(forward())
'''),
('17_opaque_class_callee_loop_negative', False, '''class State()
    int value
void clear(State &input)
    input = State()
int forward(State &input)
    clearer := clear
    result := 0
    for i := [0, 1][]
        copy := input
        clearer(&copy)
        result += copy.value
    return result
state := State()
state.value = 7
println(forward(&state))
'''),
('18_invalidated_source_loop_negative', False, '''class State()
    int value
int forward(State &input)
    input = State()
    result := 0
    for i := [0, 1][]
        copy := input
        result += copy.value
    return result
state := State()
state.value = 7
println(forward(&state))
'''),
 ]
for name,want,source in cases:
    path=tmp/("shared-"+name+".qui");path.write_text(source)
    result=subprocess.run([quidra,"check",str(path),"--json"],capture_output=True,text=True)
    assert (result.returncode==0)==want,(name,result.stdout,result.stderr)
    if not want:
        assert any(d["code"].startswith("UNINITIALIZED") for d in json.loads(result.stdout)["diagnostics"]),(name,result.stdout)
PYSHARED
"$QUIDRA" run "$TMP/shared-01_deep_value_read_positive.qui" > "$TMP/shared-deep.out"
printf '7\n' > "$TMP/shared-deep.expected"
diff -u "$TMP/shared-deep.expected" "$TMP/shared-deep.out"
"$QUIDRA" run "$TMP/shared-06_conditional_scalar_write_preserves_receipt_positive.qui" > "$TMP/shared-receipt.out"
printf '7\n' > "$TMP/shared-receipt.expected"
diff -u "$TMP/shared-receipt.expected" "$TMP/shared-receipt.out"

# loopjoin-tests-proposed.patch: initialization requirements retain only proved field facts.
python3 - "$QUIDRA" "$TMP" <<'PYSHARED'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
cases=[
('19_for_conditional_binding_complete_positive', True, '''class Point(int value)
int consume(const Point input)
    return input.value
int read(int count)
    Point point
    for: 0 <= i < count
        point = Point(int(i))
    return consume(point)
println(read(1))
'''),
('20_while_conditional_binding_complete_positive', True, '''class Point(int value)
int consume(const Point input)
    return input.value
int read(int count)
    Point point
    while count > 0
        point = Point(count)
        count -= 1
    return consume(point)
println(read(1))
'''),
('21_before_partial_binding_loop_negative', False, '''class Point()
    int value
int consume(const Point input)
    return input.value
int read(int count)
    point := Point()
    for: 0 <= i < count
        point.value = int(i)
    return consume(point)
println(read(0))
'''),
('22_body_partial_value_loop_negative', False, '''class Point()
    int value
int consume(const Point input)
    return input.value
int read(int count)
    Point point
    for: 0 <= i < count
        point = Point()
    return consume(point)
println(read(1))
'''),
('23_early_break_partial_value_loop_negative', False, '''class Point()
    int value
int consume(const Point input)
    return input.value
int read(int count, bool flag)
    Point point
    for: 0 <= i < count
        if flag
            point = Point()
            break
        point = Point()
        point.value = int(i)
    return consume(point)
println(read(1, true))
'''),
('24_zero_loop_binding_runtime_negative', True, '''class Point(int value)
int consume(const Point input)
    return input.value
int read(int count)
    Point point
    for: 0 <= i < count
        point = Point(int(i))
    return consume(point)
println(read(0))
'''),
('25_early_break_unbound_reference_runtime_negative', False, '''class Holder()
    int &target
    void .bind(int &source)
        &target = &source
int read(bool flag)
    Holder value
    source := 7
    for i := [0, 1][]
        value = Holder()
        if flag
            break
        .value.bind(&source)
    return value.target
println(read(true))
'''),
 ]
for name,want,source in cases:
    path=tmp/("shared-"+name+".qui");path.write_text(source)
    result=subprocess.run([quidra,"check",str(path),"--json"],capture_output=True,text=True)
    assert (result.returncode==0)==want,(name,result.stdout,result.stderr)
    if not want:
        assert any((d["code"].startswith("UNINITIALIZED") or d["code"]=="UNBOUND_REFERENCE") for d in json.loads(result.stdout)["diagnostics"]),(name,result.stdout)
PYSHARED
"$QUIDRA" run "$TMP/shared-19_for_conditional_binding_complete_positive.qui" > "$TMP/shared-for.out"
printf '0\n' > "$TMP/shared-for.expected"
diff -u "$TMP/shared-for.expected" "$TMP/shared-for.out"
set +e
"$QUIDRA" run "$TMP/shared-24_zero_loop_binding_runtime_negative.qui" > "$TMP/shared-zero.out" 2> "$TMP/shared-zero.err"
shared_zero_rc=$?
set -e
[[ "$shared_zero_rc" -eq 101 ]]
grep -q 'UNINITIALIZED' "$TMP/shared-zero.err"

# Whole local value-array element stores retain Class ingress guarantees.
python3 - "$QUIDRA" "$TMP" <<'PYARRAYGATE'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
cases=[
('01_scalar_index_loop_positive', True, '''class State(int value)
class Owner(State state)
    int read()
        copy := state
        numbers := [0, 1]
        for i := [0, 1][]
            numbers[0] = 7
        return copy.value
owner := Owner(State(7))
println(owner.read())
'''),
('02_scalar_tuple_index_loop_positive', True, '''class State(int value)
class Owner(State state)
    int read()
        copy := state
        numbers := [0, 1]
        for i := [0, 1][]
            (numbers[0], numbers[1]) = (numbers[1], numbers[0])
        return copy.value
owner := Owner(State(7))
println(owner.read())
'''),
('03_owned_class_array_swap_positive', True, '''class State(int value)
class Owner(State state)
    int read()
        copy := state
        values := [State(1), State(2)]
        for i := [0, 1][]
            temporary := values[0]
            values[0] = values[1]
            values[1] = temporary
        return copy.value
owner := Owner(State(7))
println(owner.read())
'''),
('04_partial_class_array_store_negative', False, '''class State()
    int value
int forward(State &input)
    copy := input
    complete := State()
    complete.value = 1
    values := [complete]
    for i := [0, 1][]
        values[0] = State()
    return copy.value
state := State()
state.value = 7
println(forward(&state))
'''),
('05_partial_class_member_store_negative', False, '''class State()
    int value
class Item(State child)
int forward()
    complete := State()
    complete.value = 1
    values := [Item(complete)]
    for i := [0, 1][]
        values[0].child = State()
    return values[0].child.value
println(forward())
'''),
('06_reference_array_partial_member_negative', False, '''class State()
    int value
class Item(State child)
int forward(Item[] &input)
    &alias := &input
    for i := [0, 1][]
        alias[0].child = State()
    return input[0].child.value
complete := State()
complete.value = 1
values := [Item(complete)]
println(forward(&values))
'''),
('07_opaque_class_call_after_swap_negative', False, '''class State()
    int value
State partial()
    return State()
int forward(State &input)
    copy := input
    complete := State()
    complete.value = 1
    values := [complete]
    func<State>() factory := partial
    for i := [0, 1][]
        values[0] = values[0]
        copy = factory()
    return copy.value
state := State()
state.value = 7
println(forward(&state))
'''),
('08_class_pattern_replacement_negative', False, '''class State()
    int value
int forward(State &input)
    copy := input
    number := 0
    for i := [0, 1][]
        (copy, number) = (State(), 0)
    return copy.value
state := State()
state.value = 7
println(forward(&state))
'''),
('09_scalar_pattern_mutating_index_negative', False, '''class State()
    int value
int clear(State &input)
    input = State()
    return 0
int forward(State &input)
    copy := input
    values := [0, 1]
    for i := [0, 1][]
        (values[clear(&copy)], values[1]) = (1, 2)
    return copy.value
state := State()
state.value = 7
println(forward(&state))
'''),
('10_local_array_copy_independent_positive', True, '''class State(int value)
int forward(State &input, const State[] original)
    copy := input
    owned := original
    for i := [0, 1][]
        owned[0] = State(9)
    return copy.value + original[0].value
values := [State(1)]
&item := &values[0]
println(forward(&item, values))
println(values[0].value)
'''),
('11_foreach_alias_partial_member_negative', False, '''class State()
    int value
class Item(State child)
int forward()
    complete := State()
    complete.value = 1
    values := [Item(complete)]
    for &item := &values[]
        item.child = State()
    return values[0].child.value
println(forward())
'''),
('12_all_rebinds_still_gate_negative', False, '''class State()
    int value
int forward(State &input)
    copy := input
    old := copy.value
    other := State()
    &alias := &copy.value
    values := [0, 1]
    for i := [0, 1][]
        (values[0], values[1]) = (values[1], values[0])
        println(alias)
        &alias = &other.value
    return old
state := State()
state.value = 7
println(forward(&state))
'''),
('13_owned_array_swap_copy_independent_positive', True, '''class State(int value)
int forward(State &input, const State[] original)
    copy := input
    owned := original
    for i := [0, 1][]
        temporary := owned[0]
        owned[0] = owned[1]
        owned[1] = temporary
    return copy.value + original[0].value
values := [State(1), State(2)]
&item := &values[0]
println(forward(&item, values))
println(values[0].value)
'''),
('14_owned_array_unfilled_slot_runtime_negative', True, '''class State(int value)
int forward(State &input, int index)
    copy := input
    State[2] values
    values[0] = State(9)
    for i := [0, 1][]
        temporary := values[0]
        values[0] = temporary
    return copy.value + values[index].value
state := State(7)
println(forward(&state, 1))
'''),
('15_owned_array_bounds_runtime_negative', True, '''class State(int value)
int forward(State &input, int index)
    copy := input
    State[2] values
    values[0] = State(9)
    for i := [0, 1][]
        temporary := values[0]
        values[0] = temporary
    return copy.value + values[index].value
state := State(7)
println(forward(&state, 2))
'''),
 ]
for name,want,source in cases:
    path=tmp/("arraygate-"+name+".qui");path.write_text(source)
    result=subprocess.run([quidra,"check",str(path),"--json"],capture_output=True,text=True)
    assert (result.returncode==0)==want,(name,result.stdout,result.stderr)
    if not want:
        assert any(d["code"].startswith("UNINITIALIZED") for d in json.loads(result.stdout)["diagnostics"]),(name,result.stdout)
PYARRAYGATE
"$QUIDRA" run "$TMP/arraygate-13_owned_array_swap_copy_independent_positive.qui" > "$TMP/arraygate-copy.out"
printf '2\n1\n' > "$TMP/arraygate-copy.expected"
diff -u "$TMP/arraygate-copy.expected" "$TMP/arraygate-copy.out"
set +e
"$QUIDRA" run "$TMP/arraygate-14_owned_array_unfilled_slot_runtime_negative.qui" > "$TMP/arraygate-slot.out" 2> "$TMP/arraygate-slot.err"
arraygate_slot_rc=$?
"$QUIDRA" run "$TMP/arraygate-15_owned_array_bounds_runtime_negative.qui" > "$TMP/arraygate-bounds.out" 2> "$TMP/arraygate-bounds.err"
arraygate_bounds_rc=$?
set -e
[[ "$arraygate_slot_rc" -eq 101 ]]
grep -q 'UNINITIALIZED' "$TMP/arraygate-slot.err"
[[ "$arraygate_bounds_rc" -eq 101 ]]
grep -q 'INDEX_BOUNDS' "$TMP/arraygate-bounds.err"

# Scalar reference joins preserve every possible entry requirement.
python3 - "$QUIDRA" "$TMP" <<'PYSCALARJOIN'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
cases=json.loads(r'''[
  {
    "name": "full-parameter-fields",
    "accept": true,
    "stdout": "one\n",
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    return destination\ndata := full()\nprintln(read(&data, true))\n"
  },
  {
    "name": "partial-selected-field-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    return destination\ndata := Data()\ndata.second = \"two\"\nprintln(read(&data, true))\n"
  },
  {
    "name": "write-only-selection",
    "accept": true,
    "stdout": "written\n",
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    destination = \"written\"\n    return destination\ndata := Data()\nprintln(read(&data, true))\n"
  },
  {
    "name": "same-slot-scalar-initialize",
    "accept": true,
    "stdout": "second\n",
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    data.first = \"first\"\n    data.second = \"second\"\n    return destination\ndata := Data()\nprintln(read(&data, false))\n"
  },
  {
    "name": "old-partial-parent-new-full-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    data = full()\n    return destination\ndata := Data()\nprintln(read(&data, true))\n"
  },
  {
    "name": "other-parameter-parent-replace-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, Data &other, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    other = full()\n    return destination\ndata := Data()\nprintln(read(&data, &data, true))\n"
  },
  {
    "name": "callee-other-parameter-replace-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nvoid reset(Data &target)\n    target = full()\nstring read(Data &data, Data &other, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    reset(&other)\n    return destination\ndata := Data()\nprintln(read(&data, &data, true))\n"
  },
  {
    "name": "callee-restore-parent-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nvoid reset(Data &target)\n    saved := target\n    target = full()\n    target = saved\nstring read(Data &data, Data &other, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    reset(&other)\n    return destination\ndata := Data()\nprintln(read(&data, &data, true))\n"
  },
  {
    "name": "rebind-unset-local-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    string unset\n    &destination = &unset\n    return destination\ndata := full()\nprintln(read(&data, true))\n"
  },
  {
    "name": "copy-unknown-scalar-alias",
    "accept": true,
    "stdout": "one\n",
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    &copied := &destination\n    return copied\ndata := full()\nprintln(read(&data, true))\n"
  },
  {
    "name": "different-parents-outside-domain-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, Data &other, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &other.second\n    return destination\ndata := full()\nother := full()\nprintln(read(&data, &other, true))\n"
  },
  {
    "name": "nested-fields-outside-domain-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nclass Wrapper(Data child)\nstring read(Wrapper &wrapper, bool choose)\n    string &destination\n    if choose\n        &destination = &wrapper.child.first\n    else\n        &destination = &wrapper.child.second\n    return destination\nwrapper := Wrapper(full())\nprintln(read(&wrapper, true))\n"
  },
  {
    "name": "four-alternatives",
    "accept": true,
    "stdout": "3\n",
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nclass Four()\n    string first := \"1\"\n    string second := \"2\"\n    string third := \"3\"\n    string fourth := \"4\"\nstring read(Four &data, int choice)\n    string &destination\n    if choice == 1\n        &destination = &data.first\n    elif choice == 2\n        &destination = &data.second\n    elif choice == 3\n        &destination = &data.third\n    else\n        &destination = &data.fourth\n    return destination\ndata := Four()\nprintln(read(&data, 3))\n"
  },
  {
    "name": "class-alternatives-outside-domain-negative",
    "accept": false,
    "stdout": null,
    "source": "class Box()\n    int value\nclass Data()\n    Box first := Box()\n    Box second := Box()\nint read(Data &data, bool choose)\n    Box &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    return destination.value\ndata := Data()\nprintln(read(&data, true))\n"
  },
  {
    "name": "repeated-rebind-outside-domain-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data)\n    string &destination\n    for: index < 2\n        if index == 0\n            &destination = &data.first\n        else\n            &destination = &data.second\n    return destination\ndata := full()\nprintln(read(&data))\n"
  },
  {
    "name": "lazy-proof-before-parent-replacement",
    "accept": true,
    "stdout": "oneone\n",
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    old := destination\n    data = Data()\n    return destination + old\ndata := full()\nprintln(read(&data, true))\n"
  },
  {
    "name": "match-rebind-unset-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose, int | none option)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    string unset\n    match option\n        int value\n            &destination = &unset\n        none\n            &destination = &unset\n    return destination\ndata := full()\nprintln(read(&data, true, none))\n"
  },
  {
    "name": "match-without-rebind",
    "accept": true,
    "stdout": "one\n",
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose, int | none option)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    match option\n        int value\n            println(value)\n        none\n            void\n    return destination\ndata := full()\nprintln(read(&data, true, none))\n"
  },
  {
    "name": "callee-hidden-parent-repair-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nvoid hidden_reset(Data &left, Data &right, bool choose)\n    Data &target\n    if choose\n        &target = &left\n    else\n        &target = &right\n    target = Data()\n    left.first = \"new\"\n    left.second = \"new\"\n    right.first = \"new\"\n    right.second = \"new\"\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    hidden_reset(&data, &data, true)\n    return destination\ndata := Data()\nprintln(read(&data, true))\n"
  },
  {
    "name": "callee-hidden-parent-caller-repair-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nvoid hidden_reset(Data &left, Data &right, bool choose)\n    Data &target\n    if choose\n        &target = &left\n    else\n        &target = &right\n    target = Data()\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    hidden_reset(&data, &data, true)\n    data.first = \"new\"\n    data.second = \"new\"\n    return destination\ndata := Data()\nprintln(read(&data, true))\n"
  },
  {
    "name": "same-slot-string-setters",
    "accept": true,
    "stdout": "written\n",
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nvoid initialize_text(string &target)\n    target = \"written\"\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    initialize_text(&data.first)\n    initialize_text(&data.second)\n    return destination\ndata := Data()\nprintln(read(&data, true))\n"
  },
  {
    "name": "parallel-parent-change-before-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    data = full()\n    parallel\n        println(0)\n    return destination\ndata := Data()\nprintln(read(&data, true))\n"
  },
  {
    "name": "parallel-parent-change-inside-negative",
    "accept": false,
    "stdout": null,
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    parallel\n        data = full()\n    return destination\ndata := Data()\nprintln(read(&data, true))\n"
  },
  {
    "name": "parallel-fresh-selection-positive",
    "accept": true,
    "stdout": "0\none\n",
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    parallel\n        println(0)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    return destination\ndata := full()\nprintln(read(&data, true))\n"
  },
  {
    "name": "parallel-fresh-write-only-positive",
    "accept": true,
    "stdout": "0\nwritten\n",
    "source": "class Data()\n    string first\n    string second\nData full()\n    data := Data()\n    data.first = \"one\"\n    data.second = \"two\"\n    return data\nstring read(Data &data, bool choose)\n    parallel\n        println(0)\n    string &destination\n    if choose\n        &destination = &data.first\n    else\n        &destination = &data.second\n    destination = \"written\"\n    return destination\ndata := Data()\nprintln(read(&data, true))\n"
  }
]''')
for case in cases:
    path=tmp/('scalar-join-'+case['name']+'.qui')
    path.write_text(case['source'])
    result=subprocess.run([quidra,'check',str(path),'--json'],capture_output=True,text=True)
    assert (result.returncode==0)==case['accept'],(case['name'],result.stdout,result.stderr)
    if case['accept']:
        run=subprocess.run([quidra,'run',str(path)],capture_output=True,text=True)
        assert run.returncode==0 and run.stdout==case['stdout'],(case['name'],run.returncode,run.stdout,run.stderr)
    else:
        codes=[d['code'] for d in json.loads(result.stdout)['diagnostics']]
        assert any(c.startswith('UNINITIALIZED') or c=='UNBOUND_REFERENCE' for c in codes),(case['name'],codes)
PYSCALARJOIN

# Snapshot only unconditional getter facts at evaluation, before later effects.

cat > "$TMP/readonly-predicate-module.qui" <<'QUI'
class Point(int x, int y)
class Enemy(Point point, bool flying)
    bool airborne()
        return flying
    Point position()
        if airborne()
            return point
        return Point(3, 4)
int read(Point point)
    return point.x + point.y
int invoke(Enemy enemy)
    return read(enemy.position())
QUI

cat > "$TMP/02-named-argument.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point point)
    Point get()
        return point
int read(Point value)
    return value.x + value.y
int invoke(Holder &holder)
    Point copied := holder.get()
    return read(copied)
Holder holder := Holder(Point(3, 4))
println(invoke(&holder))
QUI

"$QUIDRA" check "$TMP/02-named-argument.qui"
[[ "$("$QUIDRA" run "$TMP/02-named-argument.qui")" == '7' ]]

cat > "$TMP/04-named-union.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point point)
    Point get()
        return point
Point | none invoke(Holder &holder)
    Point copied := holder.get()
    return copied
Holder holder := Holder(Point(3, 4))
match invoke(&holder)
    Point value
        println(value.x)
    none
        println(0)
QUI

"$QUIDRA" check "$TMP/04-named-union.qui"
[[ "$("$QUIDRA" run "$TMP/04-named-union.qui")" == '3' ]]

cat > "$TMP/06-default-getter-constructor-union.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point point)
    Point get()
        return point
class Result(Holder holder)
    Point snapshot := holder.get()
Result | none invoke(Holder holder)
    return Result(holder)
Holder holder := Holder(Point(3, 4))
match invoke(holder)
    Result value
        println(value.snapshot.x)
    none
        println(0)
QUI

"$QUIDRA" check "$TMP/06-default-getter-constructor-union.qui"
[[ "$("$QUIDRA" run "$TMP/06-default-getter-constructor-union.qui")" == '3' ]]

cat > "$TMP/07-default-getter-constructor-named-union.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point point)
    Point get()
        return point
class Result(Holder holder)
    Point snapshot := holder.get()
Result | none invoke(Holder holder)
    Result copied := Result(holder)
    return copied
Holder holder := Holder(Point(3, 4))
match invoke(holder)
    Result value
        println(value.snapshot.x)
    none
        println(0)
QUI

"$QUIDRA" check "$TMP/07-default-getter-constructor-named-union.qui"
[[ "$("$QUIDRA" run "$TMP/07-default-getter-constructor-named-union.qui")" == '3' ]]

cat > "$TMP/08-late-receiver-init-negative.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
    int read(int ignored)
        return y
class Holder()
    Point point := Point(1)
    Point get()
        return point
int initialize(Holder &holder)
    holder.point.y = 2
    return 0
int invoke(Holder &holder)
    return holder.get().read(initialize(&holder))
Holder holder := Holder()
println(invoke(&holder))
QUI

set +e
"$QUIDRA" check "$TMP/08-late-receiver-init-negative.qui" --json > "$TMP/08-late-receiver-init-negative.qui.json"
ephemeral_rc=$?
set -e
[[ "$ephemeral_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/08-late-receiver-init-negative.qui.json"

cat > "$TMP/09-late-equality-init-negative.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
    int read(int ignored)
        return y
class Holder()
    Point point := Point(1)
    Point get()
        return point
int initialize(Holder &holder)
    holder.point.y = 2
    return 0
Point initialize_value(Holder &holder)
    holder.point.y = 2
    Point result := Point(1)
    result.y = 2
    return result
void invoke(Holder &holder)
    test.equal(holder.get(), initialize_value(&holder))
Holder holder := Holder()
invoke(&holder)
QUI

set +e
"$QUIDRA" check "$TMP/09-late-equality-init-negative.qui" --json > "$TMP/09-late-equality-init-negative.qui.json"
ephemeral_rc=$?
set -e
[[ "$ephemeral_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/09-late-equality-init-negative.qui.json"

cat > "$TMP/12-constructor-mixed-exit-negative.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point point)
    Point get()
        return point
class Result this(local Holder holder, local bool choose)
    Point snapshot
    if choose
        this.snapshot = holder.get()
Result | none invoke(Holder holder, bool choose)
    return Result(holder, choose)
Holder holder := Holder(Point(3, 4))
match invoke(holder, false)
    Result value
        println(value.snapshot.x)
    none
        println(0)
QUI

set +e
"$QUIDRA" check "$TMP/12-constructor-mixed-exit-negative.qui" --json > "$TMP/12-constructor-mixed-exit-negative.qui.json"
ephemeral_rc=$?
set -e
[[ "$ephemeral_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/12-constructor-mixed-exit-negative.qui.json"

cat > "$TMP/13-receiver-try-error-negative.qui" <<'QUI'
class Box()
    int value
class Owner()
    state := Box()
    void | error .attempt(string text)
        state = Box()
        try int(text)
        state.value = 7
        return void
owner := Owner()
owner.state.value = 9
void | error result := .owner.attempt("bad")
println(owner.state.value)
QUI

set +e
"$QUIDRA" check "$TMP/13-receiver-try-error-negative.qui" --json > "$TMP/13-receiver-try-error-negative.qui.json"
ephemeral_rc=$?
set -e
[[ "$ephemeral_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/13-receiver-try-error-negative.qui.json"

cat > "$TMP/14-zero-direct-argument.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point point)
    Point get()
        return point
int read(Point value)
    return value.x + value.y
int invoke(Holder holder)
    return read(holder.get())
Holder holder := Holder(Point(3, 4))
println(invoke(holder))
QUI

"$QUIDRA" check "$TMP/14-zero-direct-argument.qui"
[[ "$("$QUIDRA" run "$TMP/14-zero-direct-argument.qui")" == '7' ]]

cat > "$TMP/15-zero-direct-union.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point point)
    Point get()
        return point
Point | none invoke(Holder holder)
    return holder.get()
Holder holder := Holder(Point(3, 4))
match invoke(holder)
    Point value
        println(value.x)
    none
        println(0)
QUI

"$QUIDRA" check "$TMP/15-zero-direct-union.qui"
[[ "$("$QUIDRA" run "$TMP/15-zero-direct-union.qui")" == '3' ]]

cat > "$TMP/16-zero-direct-field.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point point)
    Point get()
        return point
int invoke(Holder holder)
    return holder.get().x
Holder holder := Holder(Point(3, 4))
println(invoke(holder))
QUI

"$QUIDRA" check "$TMP/16-zero-direct-field.qui"
[[ "$("$QUIDRA" run "$TMP/16-zero-direct-field.qui")" == '3' ]]

cat > "$TMP/17-zero-temporary-member.qui" <<'QUI'
class Point(int x, int y)
class Nested(Point point)
class Holder(Nested nested)
    Nested get()
        return nested
int read(Point point)
    return point.x + point.y
int invoke(Holder holder)
    return read(holder.get().point)
Holder holder := Holder(Nested(Point(3, 4)))
println(invoke(holder))
QUI

"$QUIDRA" check "$TMP/17-zero-temporary-member.qui"
[[ "$("$QUIDRA" run "$TMP/17-zero-temporary-member.qui")" == '7' ]]

cat > "$TMP/18-zero-if-argument.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point first, Point second)
    Point get(bool choose)
        return if choose then first else second
int read(Point point)
    return point.x + point.y
int invoke(Holder holder, bool choose)
    return read(if choose then holder.get(true) else holder.get(false))
Holder holder := Holder(Point(3, 4), Point(5, 6))
println(invoke(holder, true))
QUI

"$QUIDRA" check "$TMP/18-zero-if-argument.qui"
[[ "$("$QUIDRA" run "$TMP/18-zero-if-argument.qui")" == '7' ]]

cat > "$TMP/19-two-getter-arguments.qui" <<'QUI'
class Point(int x, int y)
class Holder(Point point)
    Point get()
        return point
int add(Point first, Point second)
    return first.x + second.y
int invoke(Holder holder)
    return add(holder.get(), holder.get())
Holder holder := Holder(Point(3, 4))
println(invoke(holder))
QUI

"$QUIDRA" check "$TMP/19-two-getter-arguments.qui"
[[ "$("$QUIDRA" run "$TMP/19-two-getter-arguments.qui")" == '7' ]]

cat > "$TMP/20-readonly-bool-condition.qui" <<'QUI'
class Point(int x, int y)
class Enemy(Point point, bool flying)
    bool airborne()
        return flying
    Point position()
        if airborne()
            return point
        return Point(3, 4)
int read(Point point)
    return point.x + point.y
int invoke(Enemy enemy)
    return read(enemy.position())
Enemy enemy := Enemy(Point(3, 4), true)
println(invoke(enemy))
QUI

"$QUIDRA" check "$TMP/20-readonly-bool-condition.qui"
[[ "$("$QUIDRA" run "$TMP/20-readonly-bool-condition.qui")" == '7' ]]

cat > "$TMP/21-mutating-predicate-negative.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
class Holder()
    Point point := Point(1)
    bool .predicate()
        point = Point(1)
        return true
    Point .get()
        if .predicate()
            return point
        return Point(1)
Holder holder := Holder()
holder.point.y = 2
Point copied := .holder.get()
println(copied.y)
QUI

set +e
"$QUIDRA" check "$TMP/21-mutating-predicate-negative.qui" --json > "$TMP/21-mutating-predicate-negative.qui.json"
ephemeral_rc=$?
set -e
[[ "$ephemeral_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/21-mutating-predicate-negative.qui.json"

cat > "$TMP/22-default-predicate-negative.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
class Holder()
    Point point := Point(1)
    bool predicate(bool choose := true)
        return choose
    Point get()
        if predicate()
            return point
        return Point(1)
Holder holder := Holder()
Point copied := holder.get()
println(copied.y)
QUI

set +e
"$QUIDRA" check "$TMP/22-default-predicate-negative.qui" --json > "$TMP/22-default-predicate-negative.qui.json"
ephemeral_rc=$?
set -e
[[ "$ephemeral_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/22-default-predicate-negative.qui.json"

cat > "$TMP/23-late-binary-init-negative.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
    int read(int ignored)
        return y
class Holder()
    Point point := Point(1)
    Point get()
        return point
int initialize(Holder &holder)
    holder.point.y = 2
    return 0
Point initialize_value(Holder &holder)
    holder.point.y = 2
    Point result := Point(1)
    result.y = 2
    return result
bool invoke(Holder &holder)
    return holder.get() == initialize_value(&holder)
Holder holder := Holder()
println(invoke(&holder))
QUI

set +e
"$QUIDRA" check "$TMP/23-late-binary-init-negative.qui" --json > "$TMP/23-late-binary-init-negative.qui.json"
ephemeral_rc=$?
set -e
[[ "$ephemeral_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/23-late-binary-init-negative.qui.json"

cat > "$TMP/24-late-chain-init-negative.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
    int read(int ignored)
        return y
class Holder()
    Point point := Point(1)
    Point get()
        return point
int initialize(Holder &holder)
    holder.point.y = 2
    return 0
Point initialize_value(Holder &holder)
    holder.point.y = 2
    Point result := Point(1)
    result.y = 2
    return result
bool invoke(Holder &holder)
    return holder.get() == initialize_value(&holder) == initialize_value(&holder)
Holder holder := Holder()
println(invoke(&holder))
QUI

set +e
"$QUIDRA" check "$TMP/24-late-chain-init-negative.qui" --json > "$TMP/24-late-chain-init-negative.qui.json"
ephemeral_rc=$?
set -e
[[ "$ephemeral_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/24-late-chain-init-negative.qui.json"

cat > "$TMP/25-default-effect-predicate-negative.qui" <<'QUI'
bool default_choice()
    println("default")
    return true
class Point this(local int initial)
    int x
    int y
    this.x = initial
class Holder()
    Point point := Point(1)
    bool predicate(bool choose := default_choice())
        return choose
    Point get()
        if predicate()
            return point
        return Point(1)
Holder holder := Holder()
Point copied := holder.get()
println(copied.y)
QUI

set +e
"$QUIDRA" check "$TMP/25-default-effect-predicate-negative.qui" --json > "$TMP/25-default-effect-predicate-negative.qui.json"
ephemeral_rc=$?
set -e
[[ "$ephemeral_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/25-default-effect-predicate-negative.qui.json"

cat > "$TMP/26-unmarked-predicate-write-negative.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
class Holder()
    Point point := Point(1)
    bool predicate()
        point = Point(1)
        return true
    Point .get()
        if predicate()
            return point
        return Point(1)
Holder holder := Holder()
holder.point.y = 2
Point copied := .holder.get()
println(copied.y)
QUI

set +e
"$QUIDRA" check "$TMP/26-unmarked-predicate-write-negative.qui" --json > "$TMP/26-unmarked-predicate-write-negative.qui.json"
ephemeral_rc=$?
set -e
[[ "$ephemeral_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/26-unmarked-predicate-write-negative.qui.json"

cat > "$TMP/27-imported-predicate.qui" <<'QUI'
import source = "./readonly-predicate-module.qui"
bool airborne()
    return false
source.Enemy enemy := source.Enemy(source.Point(8, 9), true)
println(source.invoke(enemy))
QUI

"$QUIDRA" check "$TMP/27-imported-predicate.qui"
[[ "$("$QUIDRA" run "$TMP/27-imported-predicate.qui")" == '17' ]]

# Recover an exact current-class method target before a guarded call is checked.
python3 - "$QUIDRA" "$TMP" <<'PYCURRENTMETHOD'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
cases=[
('01_conditional_current_method_positive', True, '''class State(int value)
class Owner(State state)
    int .finish(State chosen)
        state = chosen
        return chosen.value
    int .run()
        copied := state
        for i := [0, 1][]
            if copied.value == i
                return .finish(copied)
        return copied.value
owner := Owner(State(7))
println(.owner.run())
'''),
('02_partial_current_method_negative', False, '''class State()
    int value
class Owner(State state)
    int .finish(State chosen)
        state = State()
        return 0
    int .run()
        copied := state
        for i := [0, 1][]
            if copied.value == i
                .finish(copied)
            println(state.value)
        return copied.value
state := State()
state.value = 7
owner := Owner(state)
println(.owner.run())
'''),
('03_partial_caller_negative', False, '''class State()
    int value
class Owner(State state)
    int .finish(State chosen)
        state = chosen
        return chosen.value
    int .run()
        copied := state
        for i := [0, 1][]
            if copied.value == i
                return .finish(copied)
        return copied.value
owner := Owner(State())
println(.owner.run())
'''),
('04_reference_descriptor_fallback_negative', False, '''class State(int value)
class Holder()
    int &target
    void .bind(int &source)
        &target = &source
class Owner(State state)
    held := Holder()
    int .finish(State chosen)
        held = Holder()
        return chosen.value
    int .run(int &source)
        .held.bind(&source)
        copied := state
        for i := [0, 1][]
            if copied.value == i
                .finish(copied)
            println(held.target)
        return copied.value
owner := Owner(State(0))
source := 7
println(.owner.run(&source))
'''),
('05_writable_argument_partial_negative', False, '''class State()
    int value
class Owner(State state)
    int .finish(State chosen, State &output)
        output = State()
        return 0
    int .run(State &output)
        copied := state
        for i := [0, 1][]
            if copied.value == i
                .finish(copied, &output)
            println(output.value)
        return copied.value
state := State()
state.value = 7
output := state
owner := Owner(state)
println(.owner.run(&output))
'''),
 ]
for name,want,source in cases:
    path=tmp/("current-method-"+name+".qui");path.write_text(source)
    result=subprocess.run([quidra,"check",str(path),"--json"],capture_output=True,text=True)
    assert (result.returncode==0)==want,(name,result.stdout,result.stderr)
    if not want:
        assert any(d["code"].startswith("UNINITIALIZED") for d in json.loads(result.stdout)["diagnostics"]),(name,result.stdout)
PYCURRENTMETHOD
"$QUIDRA" run "$TMP/current-method-01_conditional_current_method_positive.qui" > "$TMP/current-method.out"
printf '7\n' > "$TMP/current-method.expected"
diff -u "$TMP/current-method.expected" "$TMP/current-method.out"

# Complete receiver-array stores and bare default collections retain copy proofs.
cat > "$TMP/class-loop-tail-receiver-array-store-loop.qui" <<'QUI'
class Params()
    int[] values := [7]
class Item(int value)
class Data()
    Params params := Params()
class Owner()
    Data data := Data()
    Item[] items := [Item(1)]
    int .read()
        p := data.params
        for: index < int(items.len())
            item := items[index]
            item.value += 1
            items[index] = item
        return p.values[0]
owner := Owner()
println(.owner.read())
QUI

"$QUIDRA" check "$TMP/class-loop-tail-receiver-array-store-loop.qui"
[[ "$("$QUIDRA" run "$TMP/class-loop-tail-receiver-array-store-loop.qui")" == '7' ]]

cat > "$TMP/class-loop-tail-receiver-partial-rhs-negative.qui" <<'QUI'
class Child()
    int value
Child full_child()
    child := Child()
    child.value = 1
    return child
class Item(Child child)
class Params()
    int[] values := [7]
class Data()
    Params params := Params()
class Owner()
    Data data := Data()
    Item[] items := [Item(full_child())]
    int .read()
        p := data.params
        for: index < int(items.len())
            items[index] = Item(Child())
        return p.values[0]
owner := Owner()
println(.owner.read())
QUI

set +e
"$QUIDRA" check "$TMP/class-loop-tail-receiver-partial-rhs-negative.qui" --json > "$TMP/class-loop-tail-receiver-partial-rhs-negative.qui.json"
class_loop_tail_rc=$?
set -e
[[ "$class_loop_tail_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/class-loop-tail-receiver-partial-rhs-negative.qui.json"

cat > "$TMP/class-loop-tail-receiver-slot-alias-partial-negative.qui" <<'QUI'
class Child()
    int value
Child full_child()
    child := Child()
    child.value = 1
    return child
class Item(Child child)
class Params()
    int[] values := [7]
class Data()
    Params params := Params()
class Owner()
    Data data := Data()
    Item[] items := [Item(full_child())]
    int .read()
        p := data.params
        for: index < int(items.len())
            &slot := &items[index]
            slot.child = Child()
        return p.values[0]
owner := Owner()
println(.owner.read())
QUI

set +e
"$QUIDRA" check "$TMP/class-loop-tail-receiver-slot-alias-partial-negative.qui" --json > "$TMP/class-loop-tail-receiver-slot-alias-partial-negative.qui.json"
class_loop_tail_rc=$?
set -e
[[ "$class_loop_tail_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/class-loop-tail-receiver-slot-alias-partial-negative.qui.json"

cat > "$TMP/class-loop-tail-receiver-other-alias-callee-before-negative.qui" <<'QUI'
class Child()
    int value
Child full_child()
    child := Child()
    child.value = 1
    return child
class Item(Child child)
class Params()
    int[] values := [7]
class Data()
    Params params := Params()
void clear(Child &target)
    target = Child()
class Owner()
    Data data := Data()
    Item[] items := [Item(full_child())]
    int .read(Child &other)
        p := data.params
        for: index < int(items.len())
            clear(&other)
            item := items[index]
            items[index] = item
        return p.values[0]
owner := Owner()
child := full_child()
println(.owner.read(&child))
QUI

set +e
"$QUIDRA" check "$TMP/class-loop-tail-receiver-other-alias-callee-before-negative.qui" --json > "$TMP/class-loop-tail-receiver-other-alias-callee-before-negative.qui.json"
class_loop_tail_rc=$?
set -e
[[ "$class_loop_tail_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/class-loop-tail-receiver-other-alias-callee-before-negative.qui.json"

cat > "$TMP/class-loop-tail-receiver-other-alias-callee-after-negative.qui" <<'QUI'
class Child()
    int value
Child full_child()
    child := Child()
    child.value = 1
    return child
class Item(Child child)
class Params()
    int[] values := [7]
class Data()
    Params params := Params()
void clear(Child &target)
    target = Child()
class Owner()
    Data data := Data()
    Item[] items := [Item(full_child())]
    int .read(Child &other)
        p := data.params
        for: index < int(items.len())
            item := items[index]
            items[index] = item
            clear(&other)
        return p.values[0]
owner := Owner()
child := full_child()
println(.owner.read(&child))
QUI

set +e
"$QUIDRA" check "$TMP/class-loop-tail-receiver-other-alias-callee-after-negative.qui" --json > "$TMP/class-loop-tail-receiver-other-alias-callee-after-negative.qui.json"
class_loop_tail_rc=$?
set -e
[[ "$class_loop_tail_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/class-loop-tail-receiver-other-alias-callee-after-negative.qui.json"

cat > "$TMP/class-loop-tail-receiver-captured-old-partial-negative.qui" <<'QUI'
class Child()
    int value
Child full_child()
    child := Child()
    child.value = 1
    return child
class Item(Child child)
class Params()
    int[] values := [7]
class Data()
    Params params := Params()
class Owner()
    Data data := Data()
    Item[] items := [Item(full_child())]
    int .read()
        &old := &items[0].child
        old = Child()
        for: index < int(items.len())
            items[index] = Item(full_child())
        return old.value
owner := Owner()
println(.owner.read())
QUI

set +e
"$QUIDRA" check "$TMP/class-loop-tail-receiver-captured-old-partial-negative.qui" --json > "$TMP/class-loop-tail-receiver-captured-old-partial-negative.qui.json"
class_loop_tail_rc=$?
set -e
[[ "$class_loop_tail_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/class-loop-tail-receiver-captured-old-partial-negative.qui.json"

cat > "$TMP/class-loop-tail-receiver-fixed-unfilled-runtime.qui" <<'QUI'
class Params()
    int[] values := [7]
class Item(int value)
class Data()
    Params params := Params()
class Owner()
    Data data := Data()
    Item[1] items := array(1)
    int .read()
        p := data.params
        for: index < 1
            item := items[index]
            items[index] = item
        return p.values[0]
owner := Owner()
println(.owner.read())
QUI

"$QUIDRA" check "$TMP/class-loop-tail-receiver-fixed-unfilled-runtime.qui"
set +e
"$QUIDRA" run "$TMP/class-loop-tail-receiver-fixed-unfilled-runtime.qui" > "$TMP/class-loop-tail-receiver-fixed-unfilled-runtime.qui.out" 2> "$TMP/class-loop-tail-receiver-fixed-unfilled-runtime.qui.err"
class_loop_tail_rc=$?
set -e
[[ "$class_loop_tail_rc" -eq 101 ]]

cat > "$TMP/class-loop-tail-reference-array-field-outside-domain-negative.qui" <<'QUI'
class Params()
    int[] values := [7]
class Item(int value)
class Data()
    Params params := Params()
class Owner this(local Item[] &input)
    Data data := Data()
    Item[] &items
    &items = &input
    int .read()
        p := data.params
        for: index < int(items.len())
            item := items[index]
            items[index] = item
        return p.values[0]
Item[] items := [Item(1)]
owner := Owner(&items)
println(.owner.read())
QUI

set +e
"$QUIDRA" check "$TMP/class-loop-tail-reference-array-field-outside-domain-negative.qui" --json > "$TMP/class-loop-tail-reference-array-field-outside-domain-negative.qui.json"
class_loop_tail_rc=$?
set -e
[[ "$class_loop_tail_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/class-loop-tail-reference-array-field-outside-domain-negative.qui.json"

cat > "$TMP/class-loop-tail-bare-map-guarded-copy.qui" <<'QUI'
class Vocabulary()
    int candidate_count := 7
    index := map<string, int>()
class Owner()
    Vocabulary source := Vocabulary()
    int read()
        copy := source
        map<string, int> local_counts
        for term := ["term"][]
            if not copy.index.has(term)
                .local_counts.set(term, 1)
        return copy.candidate_count
println(Owner().read())
QUI

"$QUIDRA" check "$TMP/class-loop-tail-bare-map-guarded-copy.qui"
[[ "$("$QUIDRA" run "$TMP/class-loop-tail-bare-map-guarded-copy.qui")" == '7' ]]

cat > "$TMP/class-loop-tail-bare-distinct-guarded-copy.qui" <<'QUI'
class Vocabulary()
    int candidate_count := 7
    index := map<string, int>()
class Owner()
    Vocabulary source := Vocabulary()
    int read()
        copy := source
        distinct<string> local_counts
        for term := ["term"][]
            if not copy.index.has(term)
                .local_counts.add(term)
        return copy.candidate_count
println(Owner().read())
QUI

"$QUIDRA" check "$TMP/class-loop-tail-bare-distinct-guarded-copy.qui"
[[ "$("$QUIDRA" run "$TMP/class-loop-tail-bare-distinct-guarded-copy.qui")" == '7' ]]

cat > "$TMP/class-loop-tail-constructor-map-guarded-copy.qui" <<'QUI'
class Vocabulary()
    int candidate_count := 7
    index := map<string, int>()
class Owner()
    Vocabulary source := Vocabulary()
    int read()
        copy := source
        local_counts := map<string, int>()
        for term := ["term"][]
            if not copy.index.has(term)
                .local_counts.set(term, 1)
        return copy.candidate_count
println(Owner().read())
QUI

"$QUIDRA" check "$TMP/class-loop-tail-constructor-map-guarded-copy.qui"
[[ "$("$QUIDRA" run "$TMP/class-loop-tail-constructor-map-guarded-copy.qui")" == '7' ]]

cat > "$TMP/class-loop-tail-bare-user-class-uninitialized-negative.qui" <<'QUI'
class Worker()
    void .touch()
        return
class Vocabulary()
    int candidate_count := 7
    index := map<string, int>()
class Owner()
    Vocabulary source := Vocabulary()
    int read()
        copy := source
        Worker worker
        for term := ["term"][]
            if not copy.index.has(term)
                .worker.touch()
        return copy.candidate_count
println(Owner().read())
QUI

set +e
"$QUIDRA" check "$TMP/class-loop-tail-bare-user-class-uninitialized-negative.qui" --json > "$TMP/class-loop-tail-bare-user-class-uninitialized-negative.qui.json"
class_loop_tail_rc=$?
set -e
[[ "$class_loop_tail_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/class-loop-tail-bare-user-class-uninitialized-negative.qui.json"

cat > "$TMP/class-loop-tail-bare-map-partial-class-call-negative.qui" <<'QUI'
class Child()
    int value
void clear(Child &target)
    target = Child()
class Vocabulary()
    int candidate_count := 7
    index := map<string, int>()
class Owner()
    Vocabulary source := Vocabulary()
    int read(Child &other)
        copy := source
        map<string, int> local_counts
        for term := ["term"][]
            if not copy.index.has(term)
                .local_counts.set(term, 1)
            clear(&other)
        return copy.candidate_count
child := Child()
child.value = 1
println(Owner().read(&child))
QUI

set +e
"$QUIDRA" check "$TMP/class-loop-tail-bare-map-partial-class-call-negative.qui" --json > "$TMP/class-loop-tail-bare-map-partial-class-call-negative.qui.json"
class_loop_tail_rc=$?
set -e
[[ "$class_loop_tail_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/class-loop-tail-bare-map-partial-class-call-negative.qui.json"

# A fresh value foreach binder has no owned state carried across iterations.

cat > "$TMP/01-fresh-value-member-getter.qui" <<'QUI'
class Point(int x, int y)
class Enemy(Point point)
    Point position()
        return point
class Shot(Point aim)
    bool ready()
        return aim.x > 0
void invoke(Enemy[] enemies, Shot[] shots, bool choose)
    for shot := shots[]
        if choose
            shot.aim = enemies[0].position()
        println(shot.ready())
Enemy[] enemies := [Enemy(Point(3, 4))]
Shot[] shots := [Shot(Point(1, 2))]
invoke(enemies, shots, true)
QUI

"$QUIDRA" check "$TMP/01-fresh-value-member-getter.qui"
[[ "$("$QUIDRA" run "$TMP/01-fresh-value-member-getter.qui")" == 'true' ]]

cat > "$TMP/02-no-loop-control.qui" <<'QUI'
class Point(int x, int y)
class Enemy(Point point)
    Point position()
        return point
class Shot(Point aim)
    bool ready()
        return aim.x > 0
void invoke(Enemy[] enemies, Shot[] shots, bool choose)
    Shot shot := shots[0]
    if choose
        shot.aim = enemies[0].position()
    println(shot.ready())
Enemy[] enemies := [Enemy(Point(3, 4))]
Shot[] shots := [Shot(Point(1, 2))]
invoke(enemies, shots, true)
QUI

"$QUIDRA" check "$TMP/02-no-loop-control.qui"
[[ "$("$QUIDRA" run "$TMP/02-no-loop-control.qui")" == 'true' ]]

cat > "$TMP/03-constructor-control.qui" <<'QUI'
class Point(int x, int y)
class Enemy(Point point)
    Point position()
        return point
class Shot(Point aim)
    bool ready()
        return aim.x > 0
void invoke(Enemy[] enemies, Shot[] shots, bool choose)
    for shot := shots[]
        if choose
            shot.aim = Point(3, 4)
        println(shot.ready())
Enemy[] enemies := [Enemy(Point(3, 4))]
Shot[] shots := [Shot(Point(1, 2))]
invoke(enemies, shots, true)
QUI

"$QUIDRA" check "$TMP/03-constructor-control.qui"
[[ "$("$QUIDRA" run "$TMP/03-constructor-control.qui")" == 'true' ]]

cat > "$TMP/04-fresh-partial-read-negative.qui" <<'QUI'
class Point this(local int initial, local bool full)
    int x
    int y
    this.x = initial
    if full
        this.y = initial
Point complete(int initial)
    Point result := Point(initial, false)
    result.y = initial
    return result
class Shot(Point aim)
    int value()
        return aim.y
void invoke(Shot[] shots)
    for shot := shots[]
        shot.aim = Point(1, false)
        println(shot.value())
invoke([Shot(complete(1))])
QUI

set +e
"$QUIDRA" check "$TMP/04-fresh-partial-read-negative.qui" --json > "$TMP/04-fresh-partial-read-negative.qui.json"
fresh_foreach_rc=$?
set -e
[[ "$fresh_foreach_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/04-fresh-partial-read-negative.qui.json"

cat > "$TMP/17-fresh-scalar-method.qui" <<'QUI'
class Point(int x, int y)
class Enemy(Point point)
    Point position()
        return point
class Shot(Point aim, Point position)
    int count := 0
    bool .fly()
        count += 1
        return aim.x > 0
void invoke(Enemy[] enemies, Shot[] shots, bool choose)
    for shot := shots[]
        if choose
            shot.aim = enemies[0].position()
        println(.shot.fly())
Enemy[] enemies := [Enemy(Point(3, 4))]
Shot[] shots := [Shot(Point(1, 2), Point(1, 2))]
invoke(enemies, shots, true)
QUI

"$QUIDRA" check "$TMP/17-fresh-scalar-method.qui"
[[ "$("$QUIDRA" run "$TMP/17-fresh-scalar-method.qui")" == 'true' ]]

cat > "$TMP/18-fresh-full-replace-method.qui" <<'QUI'
class Point(int x, int y)
class Enemy(Point point)
    Point position()
        return point
class Shot(Point aim, Point position)
    bool .fly()
        position = Point(1, 2)
        return aim.x > 0
void invoke(Enemy[] enemies, Shot[] shots, bool choose)
    for shot := shots[]
        if choose
            shot.aim = enemies[0].position()
        println(.shot.fly())
Enemy[] enemies := [Enemy(Point(3, 4))]
Shot[] shots := [Shot(Point(1, 2), Point(1, 2))]
invoke(enemies, shots, true)
QUI

"$QUIDRA" check "$TMP/18-fresh-full-replace-method.qui"
[[ "$("$QUIDRA" run "$TMP/18-fresh-full-replace-method.qui")" == 'true' ]]

cat > "$TMP/22-inline-owned-array-fly.qui" <<'QUI'
class Point(int x, int y)
class Advance(Point point, bool arrived)
Advance advance(Point position, Point goal)
    return Advance(goal, true)
class Enemy(Point point)
    Point position()
        return point
class Shot(Point aim, Point position, int speed)
    bool .fly()
        if speed == 0
            return true
        movement := advance(position, aim)
        position = movement.point
        return movement.arrived
class State(Enemy[] enemies, Shot[] shots)
    void .run(bool choose)
        for shot := shots[]
            if choose
                shot.aim = enemies[0].position()
            println(.shot.fly())
State state := State([Enemy(Point(3, 4))], [Shot(Point(1, 2), Point(1, 2), 1)])
.state.run(true)
QUI

"$QUIDRA" check "$TMP/22-inline-owned-array-fly.qui"
[[ "$("$QUIDRA" run "$TMP/22-inline-owned-array-fly.qui")" == 'true' ]]

cat > "$TMP/23-nested-dependent-while-negative.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
Point complete(int initial)
    Point result := Point(initial)
    result.y = initial
    return result
class Holder()
    Point point := Point(1)
class Shot(Point aim)
int read(Point point)
    return point.y
void invoke(Shot[] shots, Holder &source)
    for shot := shots[]
        shot.aim = source.point
        int pass := 0
        while pass < 2
            Point copied := shot.aim
            println(read(copied))
            shot.aim = Point(1)
            pass += 1
Holder source := Holder()
source.point.y = 2
invoke([Shot(complete(1))], &source)
QUI

set +e
"$QUIDRA" check "$TMP/23-nested-dependent-while-negative.qui" --json > "$TMP/23-nested-dependent-while-negative.qui.json"
fresh_foreach_rc=$?
set -e
[[ "$fresh_foreach_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/23-nested-dependent-while-negative.qui.json"

cat > "$TMP/24-nested-dependent-for-negative.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
Point complete(int initial)
    Point result := Point(initial)
    result.y = initial
    return result
class Holder()
    Point point := Point(1)
class Shot(Point aim)
int read(Point point)
    return point.y
void invoke(Shot[] shots, Holder &source)
    for shot := shots[]
        shot.aim = source.point
        for pass := [1, 2][]
            Point copied := shot.aim
            println(read(copied))
            shot.aim = Point(1)
Holder source := Holder()
source.point.y = 2
invoke([Shot(complete(1))], &source)
QUI

set +e
"$QUIDRA" check "$TMP/24-nested-dependent-for-negative.qui" --json > "$TMP/24-nested-dependent-for-negative.qui.json"
fresh_foreach_rc=$?
set -e
[[ "$fresh_foreach_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/24-nested-dependent-for-negative.qui.json"

cat > "$TMP/25-reference-dependent-negative.qui" <<'QUI'
class Point this(local int initial)
    int x
    int y
    this.x = initial
Point complete(int initial)
    Point result := Point(initial)
    result.y = initial
    return result
class Holder()
    Point point := Point(1)
class Shot(Point aim)
int read(Point point)
    return point.y
void invoke(Shot[] &shots, Holder &source)
    for &shot := &shots[]
        shot.aim = source.point
        int pass := 0
        while pass < 2
            Point copied := shot.aim
            println(read(copied))
            shot.aim = Point(1)
            pass += 1
Holder source := Holder()
source.point.y = 2
Shot[] shots := [Shot(complete(1))]
invoke(&shots, &source)
QUI

set +e
"$QUIDRA" check "$TMP/25-reference-dependent-negative.qui" --json > "$TMP/25-reference-dependent-negative.qui.json"
fresh_foreach_rc=$?
set -e
[[ "$fresh_foreach_rc" -eq 1 ]]
grep -q 'UNINITIALIZED' "$TMP/25-reference-dependent-negative.qui.json"

# A getter must not assume complete fields after a future partial replacement.
python3 - "$QUIDRA" "$TMP" <<'PYGETTERGUARDS'
import json,pathlib,subprocess,sys
quidra,tmp=sys.argv[1],pathlib.Path(sys.argv[2])
for name,source in json.loads(r'''[
  [
    "01-value-source-late-partial-negative.qui",
    "class Point this(local int initial)\n    int x\n    int y\n    this.x = initial\nPoint complete(int initial)\n    Point result := Point(initial)\n    result.y = initial\n    return result\nclass Enemy(Point point)\n    Point position()\n        return point\nvoid consume(Point point)\n    println(point.y)\nvoid invoke(Enemy enemy)\n    for pass := [1, 2][]\n        Point point := enemy.position()\n        consume(point)\n        enemy.point = Point(1)\ninvoke(Enemy(complete(1)))\n"
  ],
  [
    "02-array-source-late-partial-negative.qui",
    "class Point this(local int initial)\n    int x\n    int y\n    this.x = initial\nPoint complete(int initial)\n    Point result := Point(initial)\n    result.y = initial\n    return result\nclass Enemy(Point point)\n    Point position()\n        return point\nvoid consume(Point point)\n    println(point.y)\nvoid invoke(Enemy[] enemies)\n    for pass := [1, 2][]\n        Point point := enemies[0].position()\n        consume(point)\n        enemies[0].point = Point(1)\ninvoke([Enemy(complete(1))])\n"
  ],
  [
    "03-owned-array-late-partial-negative.qui",
    "class Point this(local int initial)\n    int x\n    int y\n    this.x = initial\nPoint complete(int initial)\n    Point result := Point(initial)\n    result.y = initial\n    return result\nclass Enemy(Point point)\n    Point position()\n        return point\nvoid consume(Point point)\n    println(point.y)\nclass State(Enemy[] enemies)\n    void .run()\n        for pass := [1, 2][]\n            Point point := enemies[0].position()\n            consume(point)\n            enemies[0].point = Point(1)\nState state := State([Enemy(complete(1))])\n.state.run()\n"
  ],
  [
    "04-owned-array-restore-after-loop-negative.qui",
    "class Point this(local int initial)\n    int x\n    int y\n    this.x = initial\nPoint complete(int initial)\n    Point result := Point(initial)\n    result.y = initial\n    return result\nclass Enemy(Point point)\n    Point position()\n        return point\nvoid consume(Point point)\n    println(point.y)\nclass State(Enemy[] enemies)\n    void .run()\n        for pass := [1, 2][]\n            Point point := enemies[0].position()\n            consume(point)\n            enemies[0].point = Point(1)\n        enemies[0].point = complete(1)\nState state := State([Enemy(complete(1))])\n.state.run()\n"
  ],
  [
    "05-owned-array-aliased-poison-negative.qui",
    "class Point this(local int initial)\n    int x\n    int y\n    this.x = initial\nPoint complete(int initial)\n    Point result := Point(initial)\n    result.y = initial\n    return result\nclass Enemy(Point point)\n    Point position()\n        return point\nvoid consume(Point point)\n    println(point.y)\nvoid poison(Enemy &enemy)\n    enemy.point = Point(1)\nclass State(Enemy[] enemies)\n    void .run()\n        for pass := [1, 2][]\n            Point point := enemies[0].position()\n            consume(point)\n            poison(&enemies[0])\nState state := State([Enemy(complete(1))])\n.state.run()\n"
  ]
]'''):
    path=tmp/("getter-"+name);path.write_text(source)
    result=subprocess.run([quidra,"check",str(path),"--json"],capture_output=True,text=True)
    assert result.returncode==1,(name,result.stdout,result.stderr)
    assert any(d["code"].startswith("UNINITIALIZED") for d in json.loads(result.stdout)["diagnostics"]),(name,result.stdout)
PYGETTERGUARDS
