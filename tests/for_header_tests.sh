#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
expect() {
    local name=$1 expected=$2
    cat > "$TMP/$name.qui"
    local actual
    actual="$("$QUIDRA" "$TMP/$name.qui")"
    [[ "$actual" == "$expected" ]] || { printf '%s: expected %s; got %s\n' "$name" "$expected" "$actual"; exit 1; }
}
rejects() {
    local name=$1 code=$2
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" --json > "$TMP/$name.json"; then exit 1; fi
    python3 - "$TMP/$name.json" "$code" <<'JSON'
import json,sys
assert any(d['code']==sys.argv[2] for d in json.load(open(sys.argv[1]))['diagnostics'])
JSON
}
fails() {
    local name=$1 code=$2
    cat > "$TMP/$name.qui"
    local actual rc=0
    actual="$("$QUIDRA" "$TMP/$name.qui" 2> "$TMP/$name.err")" || rc=$?
    [[ $rc == 101 && "$actual" == before ]]
    grep -Fq "runtime error[$code]" "$TMP/$name.err"
}
# Independent enumeration follows the mathematical progression directly;
# it never uses the implementation's distance, remainder, LAST or word tags.
python3 - "$TMP" <<'ORACLE'
from pathlib import Path
import sys,itertools
p=Path(sys.argv[1]);source=[];expected=[];ordinal=0
for descending,inclusive_start,inclusive_end in itertools.product((False,True),repeat=3):
    for start,end,magnitude in itertools.product(range(-2,3),range(-2,3),(1,2,3)):
        step=-magnitude if descending else magnitude
        op1=('>=' if inclusive_start else '>') if descending else ('<=' if inclusive_start else '<')
        op2=('>=' if inclusive_end else '>') if descending else ('<=' if inclusive_end else '<')
        source += [f'int8 s{ordinal} = {start}',f'int8 e{ordinal} = {end}',f'int8 k{ordinal} = {step}',
                   f'for s{ordinal} {op1} int8 v{ordinal} {op2} e{ordinal} step k{ordinal}',
                   f'    print(v{ordinal})','    print(NL)',f'print("end{ordinal}")','print(NL)']
        value=start if inclusive_start else start+step
        while (value>=end if inclusive_end else value>end) if descending else (value<=end if inclusive_end else value<end):
            expected.append(str(value));value+=step
        expected.append(f'end{ordinal}');ordinal+=1
(p/'oracle.qui').write_text('\n'.join(source)+'\n');(p/'oracle.expected').write_text('\n'.join(expected)+'\n')
ORACLE
"$QUIDRA" "$TMP/oracle.qui" > "$TMP/oracle.actual"
cmp "$TMP/oracle.expected" "$TMP/oracle.actual"
expect extrema $'-128\n-1\n126\n0\n18446744073709551614\n18446744073709551615\n4611686018427387904\n4611686018427387905\n-4611686018427387905\n-4611686018427387904' <<'QUI'
for -128 <= int8 x <= 127 step 127
    print(x)
    print(NL)
for 0 <= nat8 x <= 255 step 256
    print(x)
    print(NL)
for 18446744073709551614 <= nat64 x <= 18446744073709551615
    print(x)
    print(NL)
for 4611686018427387904 <= int x <= 4611686018427387905
    print(x)
    print(NL)
for -4611686018427387905 <= int x <= -4611686018427387904
    print(x)
    print(NL)
QUI
expect evaluated_once $'0\n1\n2\n3' <<'QUI'
int bound(int[] &order, int value)
    order = order.append(value)
    return value
int[] order := []
for bound(&order, 0) <= int x < bound(&order, 3) step bound(&order, 1)
    print(x)
    print(NL)
print(len(order))
print(NL)
QUI
expect stable_bounds $'0\n1\n2\n9' <<'QUI'
int start := 0
int end := 3
int step := 1
for start <= int x < end step step
    print(x)
    print(NL)
    start = 100
    end = 0
    step = 0
int old := 9
for 5 < old <= 5
    print(int(999))
print(old)
print(NL)
QUI
expect forms $'1\n2\n3\n3\n0\n2\n1\n1\n2\n0' <<'QUI'
int[] values := [1, 2, 3]
int previous := 9
for previous = values[]
    print(previous)
    print(NL)
print(previous)
print(NL)
for value := values[len(values)>:0][nat index]
    print(index)
    print(NL)
    print(value - 1)
    print(NL)
    if index == 0
        if &index != &index
            print(int(999))
QUI
expect snapshot $'1\n2\n3\n77' <<'QUI'
int[] values := [1, 2, 3]
for int value := values[]
    values[1] = 77
    print(value)
    print(NL)
print(values[1])
print(NL)
QUI
expect live_elements $'1\n77\n3' <<'QUI'
void fill(int[] &values)
    values[1] = 77
int[] values := [1, 2, 3]
for int &value := &values[nat index]
    if index == 0
        fill(&values)
    print(value)
    print(NL)
QUI
expect true_reference $'true\n5\n5\n6' <<'QUI'
int[] values := [1, 2]
for int &value := &values[nat index]
    if index == 0
        print(&value == &values[0])
        print(NL)
        value = 5
        print(values[0])
        print(NL)
    else
        value = 6
print(values[0])
print(NL)
print(values[1])
print(NL)
QUI
expect nested_paths $'1\n11\n12\n3' <<'QUI'
int once(int &calls)
    calls += 1
    return 0
int[][] matrix := [[1, 2], [3, 4]]
int calls := 0
for int &value := &matrix[once(&calls)][]
    value += 10
print(calls)
print(NL)
print(matrix[0][0])
print(NL)
print(matrix[0][1])
print(NL)
for int[] &row := &matrix[]
    for int &value := &row[]
        if value == 3
            print(value)
            print(NL)
QUI
expect bit_places $'true\ntrue\ntrue' <<'QUI'
bin | error parsed_bits := bin.parse("101")
bin | error parsed_one := bin.parse("1")
match parsed_bits
    bin bits
        match parsed_one
            bin one
                for bin &bit := &bits[]
                    bit = one
                    print(bit == one)
                    print(NL)
            error problem
                process.exit(1)
    error problem
        process.exit(1)
QUI
expect unrelated_row $'1\n2\n9' <<'QUI'
int[][] matrix := [[1, 2], [3, 4]]
int selected := 0
int other := 1
for int &value := &matrix[selected][]
    matrix[other] = [9]
    print(value)
    print(NL)
print(matrix[1][0])
print(NL)
QUI
fails child_alias FOR_ITERATION <<'QUI'
int[][] matrix := [[1, 2], [3, 4]]
int selected := 0
int other := 0
print("before")
for int &value := &matrix[selected][]
    matrix[other] = [9]
QUI
expect nested_snapshot $'1\n2\n77' <<'QUI'
int[][] matrix := [[1, 2]]
for int[] &row := &matrix[]
    for int value := row[]
        row[1] = 77
        print(value)
        print(NL)
print(matrix[0][1])
print(NL)
QUI
expect return_cleanup $'4' <<'QUI'
void visit(int[] &values)
    for int &value := &values[]
        return
int[] values := [1, 2, 3]
visit(&values)
values = values.append(4)
print(values[3])
print(NL)
QUI
expect existing_initialization $'1\n0' <<'QUI'
int count := 2
int existing
for 0 <= existing < count
    print("")
print(existing)
print(NL)
int stopped
for 0 <= stopped < count
    break
print(stopped)
print(NL)
QUI
fails empty_initialization UNINITIALIZED <<'QUI'
int count := 0
int existing
print("before")
for 0 <= existing < count
    print(int(999))
print(existing)
QUI
fails alias_reshape FOR_ITERATION <<'QUI'
void walk(int[] &a, int[] &b)
    for int &value := &a[]
        b = b.append(9)
int[] values := [1, 2]
print("before")
walk(&values, &values)
QUI
fails zero_step FOR_STEP <<'QUI'
int step := 0
print("before")
for 3 <= int x < 0 step step
    print(x)
QUI
fails wrong_direction FOR_STEP <<'QUI'
int step := 1
print("before")
for 3 >= int x >= 0 step step
    print(x)
QUI
rejects readonly_range LOOP_VARIABLE_WRITE <<'QUI'
for int x < 3
    x += 1
QUI
rejects readonly_index LOOP_VARIABLE_WRITE <<'QUI'
int[] values := [1]
for int value := values[nat index]
    index += 1
QUI
rejects bit_address WRITE_CAPABILITY <<'QUI'
bin | error parsed := bin.parse("1")
match parsed
    bin values
        for bin &value := &values[]
            print(&value)
    error problem
        process.exit(1)
QUI
rejects replace_path WRITE_CAPABILITY <<'QUI'
int[][] values := [[1]]
for int &value := &values[0][]
    values = [[2]]
QUI
rejects index_type FOR_HEADER <<'QUI'
int[] values := [1]
for int value := values[int index]
    print(value)
QUI
rejects suffix_outside ELEMENT_ITERATION <<'QUI'
int[] values := [1]
print(values[])
QUI
# The inspect major schema remains compatible; the minor and additive kinds
# expose source bounds only, never a synthetic zero as a replaceable node.
cat > "$TMP/tools.qui" <<'QUI'
for int value < 3
    print(value)
    print(NL)
QUI
"$QUIDRA" fmt "$TMP/tools.qui"
"$QUIDRA" inspect "$TMP/tools.qui" > "$TMP/inspect.json"
python3 - "$TMP/inspect.json" "$TMP/patch.json" <<'JSON'
import json,sys
report=json.load(open(sys.argv[1]));assert report['schema_version']==1 and report['schema_minor']==3
loop=next(n for n in report['nodes'] if n['kind']=='ForRange');assert loop['start_inclusive'] and not loop['end_inclusive']
node=next(n for n in report['nodes'] if n['kind']=='integer' and n['source']=='3')
assert not any(n['kind']=='integer' and n['source']=='value' for n in report['nodes'])
json.dump({'schema_version':3,'base_revision':report['revision'],'operations':[{'op':'replace_node','node_id':node['node_id'],'expected_hash':node['source_hash'],'expected_kind':'integer','replacement_node':{'kind':'integer','source':'2'}}]},open(sys.argv[2],'w'))
JSON
"$QUIDRA" patch "$TMP/tools.qui" "$TMP/patch.json" --write
[[ "$("$QUIDRA" "$TMP/tools.qui")" == $'0\n1' ]]
