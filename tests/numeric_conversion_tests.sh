#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
python3 - "$TMP" <<'PYGEN'
from pathlib import Path
import sys
root=Path(sys.argv[1]);program=[];expected=[]
integer=['int8','int16','int32','int64','int','nat8','nat16','nat32','nat64','nat']
real=['real16','real16b','real32','real64','real']
ordered=integer+real
precision={'real16':11,'real16b':8,'real32':24,'real64':53}
max_exponent={'real16':16,'real16b':128,'real32':128,'real64':1024}
def integer_range(kind):
    if kind=='int':return None,None
    if kind=='nat':return 0,None
    width=int(kind[3:]);return (-(1<<(width-1)),(1<<(width-1))-1) if kind.startswith('int') else (0,(1<<width)-1)
def fits(kind,value):
    if kind in integer:
        low,high=integer_range(kind);return (low is None or value>=low) and (high is None or value<=high)
    return kind=='real' or abs(value)<(1<<max_exponent[kind])
def spelling(kind,value):
    return str(value)+('.0' if kind in real else '')
case=0
def value_case(source,target,value,want):
    global case
    name=f'source_{case}';out=f'converted_{case}';case+=1
    program.extend([f'{source} {name} := {value}',f'{target} {out} := {target}({name})',f'print({out} == {want})','print(NL)'])
    expected.append('true')
def failure_case(source,target,value,reason):
    global case
    name=f'source_{case}';case+=1
    program.extend([f'{source} {name} := {value}',f'match {target}({name})',f'    {target} converted','        print("unexpected" + NL)','    error problem','        match problem.reason','            string reason','                print(reason + NL)','            none','                print("missing reason" + NL)'])
    expected.append(reason)
for source in ordered:
    for target in ordered:
        # Each ordered pair has an in-range case, independently of fallibility.
        value_case(source,target,spelling(source,1),spelling(target,1))
        if source in real and target in integer:
            failure_case(source,target,'1.5','NOT_INTEGRAL')
            value_case(source,target,'-0.0',spelling(target,0))
        if target in integer:
            low,high=integer_range(target)
            candidate=high+1 if high is not None else -1 if low==0 else None
            if candidate is not None and fits(source,candidate):
                failure_case(source,target,spelling(source,candidate),'OUT_OF_RANGE')
            if low==0 and fits(source,-1):
                failure_case(source,target,spelling(source,-1),'OUT_OF_RANGE')
        elif target in precision:
            # Values that cannot overflow retain a plain destination result.
            overflow=1<<max_exponent[target]
            if source in precision and max_exponent[source]==max_exponent[target] and precision[source]>precision[target]:
                overflow-=(1<<(max_exponent[target]-precision[target]-1))
            if fits(source,overflow):
                failure_case(source,target,spelling(source,overflow),'OUT_OF_RANGE')
            if source in integer:
                midpoint=(1<<precision[target])+1
                if fits(source,midpoint):
                    value_case(source,target,str(midpoint),str(1<<precision[target])+'.0')
            elif source=='real' or precision.get(source,0)>precision[target]:
                # Halfway between 1 and its next value: ties-to-even selects 1.
                midpoint='1.0 + 1.0 / '+str(1<<precision[target])+'.0'
                value_case(source,target,midpoint,'1.0')
program += ['real64 signed_edge := 9223372036854775808.0','real64 unsigned_edge := 18446744073709551616.0']
for target,name in [('int64','signed_edge'),('nat64','unsigned_edge')]:
    program += [f'match {target}({name})',f'    {target} value','        print("unexpected" + NL)','    error problem','        match problem.reason','            string reason','                print(reason + NL)','            none','                print("missing reason" + NL)'];expected.append('OUT_OF_RANGE')
program += ['real64[] original := [1.0, 2.5, 3.0]', 'match int8(original)', '    int8[] values','        print("unexpected" + NL)','    error problem','        print(problem.code + NL)','print("{original[0]} {original[1]} {original[2]}" + NL)', 'real16[] zeros := [-0.0, 2.0]', 'int[] exact := int(zeros)', 'nat[] natural := nat(zeros)', 'print("{exact[0]} {exact[1]} {natural[0]} {natural[1]}" + NL)']
expected += ['NUMERIC_CONVERSION','1.0 2.5 3.0','0 2 0 2']
(root/'matrix.qui').write_text('\n'.join(program)+'\n');(root/'expected').write_text('\n'.join(expected)+'\n')
PYGEN
"$QUIDRA" run "$TMP/matrix.qui" > "$TMP/actual"
diff -u "$TMP/expected" "$TMP/actual"

python3 - "$TMP" <<'PYTENSOR'
from pathlib import Path
import sys
root=Path(sys.argv[1]);program=['cli args','    int device = option(default = -1)'];expected=[]
for source in ['real16','real16b','real32','real64']:
    name='source_'+source
    program += [f'tensor<{source}> {name} := tensor.zeros<{source}>([3])',f'{name}[0] = 1.0',f'{name}[1] = 2.5',f'{name}[2] = -0.0']
    # Existing Metal real64 arithmetic capability limits are separate from the new casts.
    if source!='real64':program += ['if args.device >= 0',f'    {name} = {name}.gpu(nat(args.device))']
    for target in ['int8','int64','nat8','nat64']:
        program += [f'match {target}({name})',f'    tensor<{target}> values','        print("unexpected" + NL)','    error problem','        match problem.reason','            string reason','                print(reason + NL)','            none','                print("missing reason" + NL)']
        expected.append('NOT_INTEGRAL')
    program += [f'print({name}.cpu()[1].item() == 2.5)','print(NL)',f'{name}[1] = 2.0']
    expected.append('true')
    for target in ['int8','int64','nat8','nat64']:
        name_out=f'{source}_{target}'
        program += [f'tensor<{target}> {name_out} := {target}({name})',f'print({name_out}.cpu()[0].item() == 1 and {name_out}.cpu()[1].item() == 2 and {name_out}.cpu()[2].item() == 0)','print(NL)']
        expected.append('true')
(root/'tensors.qui').write_text('\n'.join(program)+'\n');(root/'tensor-expected').write_text('\n'.join(expected)+'\n')
PYTENSOR
"$QUIDRA" run "$TMP/tensors.qui" > "$TMP/tensor-actual"
diff -u "$TMP/tensor-expected" "$TMP/tensor-actual"
if [[ -n "${2:-}" ]]; then
    "$QUIDRA" run "$TMP/tensors.qui" --device "$2" > "$TMP/tensor-device-actual"
    diff -u "$TMP/tensor-expected" "$TMP/tensor-device-actual"
fi

cat > "$TMP/constraints.qui" <<'QUI'
int domain<T: numeric>(T value)
    return 1
int domain<T: ordered>(T value)
    return 2
com32 z := 1.0 + 2.0i
real16 h := 2.0
print(domain(z))
print(NL)
print(domain(h))
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/constraints.qui")" == "$(printf '1\n2')" ]]
