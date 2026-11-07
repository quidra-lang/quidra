#!/usr/bin/env python3
"""Integration contracts for materialized arrays and static extension lookup."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap

compiler = str(Path(sys.argv[1]).resolve())
check_only = '--check-only' in sys.argv[2:]
failures = []
with tempfile.TemporaryDirectory(prefix='quidra-array-extensions-') as directory:
    root = Path(directory)
    env = dict(os.environ, QUIDRA_CACHE_DIR=str(root / 'cache'))

    def write(name, source):
        path = root / (name + '.qui')
        path.write_text(textwrap.dedent(source).lstrip(), encoding='utf-8')
        return path

    def case(name, source, code=None, expected='true'):
        path = write(name, source)
        command = [compiler, 'check', str(path), '--json'] if code or check_only else [compiler, str(path)]
        result = subprocess.run(command, text=True, capture_output=True, env=env, timeout=120)
        if code:
            import json
            try:
                diagnostics = json.loads(result.stdout)['diagnostics']
            except (ValueError, KeyError):
                diagnostics = []
            passed = result.returncode != 0 and any(item['code'] == code for item in diagnostics)
        else:
            passed = result.returncode == 0 and (check_only or result.stdout.strip() == expected)
        if not passed:
            failures.append(name)
            print(f'FAIL {name}: exit {result.returncode}\n{result.stdout}{result.stderr}', flush=True)
        else:
            print(f'PASS {name}', flush=True)

    case('contextual-array', '''
        int[] a := [1, 2] + [3, 4]
        int[] b := [1, 2] * 2
        int[] c := 2 * [1, 2]
        int[] sum := [4, 6]
        int[] product := [2, 4]
        print(a == sum and b == product and c == product)
    ''')
    case('typed-literal-comparison', '''
        bool[] mask := [real32(1.0)] < [real32(2.0)]
        bool[] expected := [true]
        print(mask == expected)
    ''')
    case('contextual-comparison', '''
        bool[] mask := [1, 5, 3] < [2, 4, 3]
        bool[] expected := [true, false, false]
        print(mask == expected)
    ''')
    case('mixed-index-literals', '''
        nat i := 1
        int[] a := [10, 20]
        int[] b := a[[0, i]]
        int[] expected := [10, 20]
        print(b == expected)
    ''')
    case('negative-range', '''
        int[] a := -3:0
        int[] expected := [-3, -2, -1, 0]
        print(a == expected)
    ''')
    case('signed-range-power', '''
        int[] a := 0:-3 ^ 2
        int[] b := -3:0 ^ 2
        int[] c := 0:-3:-2
        int[] expected_a := [0, 1, 4, 9]
        int[] expected_b := [9, 4, 1, 0]
        int[] expected_c := [0, -2]
        print(a == expected_a and b == expected_b and c == expected_c)
    ''')
    case('equal-end-negative-step', '''
        int begin := 3
        int step := -2
        int[] values := begin:begin:step
        int[] expected := [3]
        print(values == expected)
    ''')
    case('array-rhs-mutation', '''
        int mutate(int[] &a)
            a = [8, 9]
            return 1
        int[] a := [1, 2]
        int[] result := a + mutate(&a)
        int[] expected := [2, 3]
        print(result == expected)
    ''')
    case('selector-mutation', '''
        int[] selector(int[] &value)
            value = [8, 9]
            return [1, 0, 1]
        int[] value := [1, 2]
        int[] selected := value[selector(&value)]
        int[] expected := [2, 1, 2]
        print(selected == expected)
    ''')
    case('large-step', '''
        int[] a := 0:18446744073709551616:18446744073709551616
        int[] expected := [0, 18446744073709551616]
        print(a == expected)
    ''')
    case('empty-marked-range', '''
        int[] a := 3:<0
        int[] b := 0:>3
        print(a.len() == 0 and b.len() == 0)
    ''')
    case('uninitialized-arithmetic', '''
        int[1] a
        int[] b := a + 1
    ''', 'UNINITIALIZED')
    case('array-comparison-chain', '''
        int[] a := [1, 2]
        bool result := 0 < a < 10
    ''', 'CHAINED_COMPARISON')

    write('stats', '''
        int total(extend int[] values)
            int result := 0
            for int value := values[]
                result += value
            return result
        int twice(extend int value)
            return value * 2
    ''')
    case('gather-method', '''
        import stats = "./stats.qui"
        int[] values := [1, 2, 3]
        int[] idx := [2, 0]
        print(values[idx].total() == 4 and values[values > 1].total() == 5)
    ''')
    case('slice-method', '''
        import stats = "./stats.qui"
        int[] values := [1, 2, 3]
        print(values[1:].total() == 5)
    ''')
    case('chain-method', '''
        import stats = "./stats.qui"
        int[] values := [1, 2, 3]
        print(values.total().twice() == 12)
    ''')
    case('inferred-numeric-string-comprehension', '''
        import stats = "./stats.qui"
        number := 17
        text := string(number)
        digits := [for digit := text.codepoints()[] then digit - 48]
        print(digits.total() == 8 and string(number).codepoints().total() == 104)
    ''')
    case('primitive-string-return-types', '''
        int width(extend string value)
            return int(value.len())
        ready := true
        bits := bin.fill(2, 1)
        error problem := error("sample")
        print(string(ready).width() == 4 and string(bits).width() == 2 and string(problem).width() > 0)
    ''')
    case('contextual-extension-arguments', '''
        int first(extend int[] value)
            return value[0]
        int plus(extend int value, int[] other)
            return value + other[0]
        int value := 2
        print([1, 2].first() == first([1, 2]) and value.plus([3, 4]) == 5)
    ''')
    case('native-chain-and-unary', '''
        import stats = "./stats.qui"
        int[] values := [1, 2]
        int value := 3
        print(values.append(3).total() == 6 and (-value).twice() == -6 and
              (values + value).total() == 9)
    ''')
    case('scalar-array-specializations', '''
        T twice<T: numeric>(extend T value)
            return value + value
        T[] twice<T: numeric>(extend T[] value)
            return value + value
        int value := 3
        int[] values := [1, 2]
        int[] expected := [2, 4]
        print(value.twice() == 6 and values.twice() == expected)
    ''')
    case('const-explicit-and-named', '''
        T plus<T: numeric>(extend const T value, T other := T(2))
            return value + other
        int value := 3
        print(value.plus<int>() == 5 and value.plus(other = 4) == 7)
    ''')
    write('private_extension', '''
        private int total(extend int[] value)
            return value[0]
        int run(int[] value)
            return value.total()
    ''')
    case('private-extension-local', '''
        import lib = "./private_extension.qui"
        print(lib.run([4, 5]) == 4)
    ''')
    case('private-extension-hidden', '''
        import lib = "./private_extension.qui"
        int[] value := [4, 5]
        print(value.total())
    ''', 'UNKNOWN_MEMBER')
    write('internal_user', '''
        import stats = "./stats.qui"
        int run(int[] values)
            return values.total()
    ''')
    case('module-local-import', '''
        import lib = "./internal_user.qui"
        int[] values := [1, 2, 3]
        print(lib.run(values) == 6)
    ''')
    case('no-transitive-extension', '''
        import lib = "./internal_user.qui"
        int[] values := [1, 2, 3]
        print(values.total())
    ''', 'UNKNOWN_MEMBER')
    write('public_stats', 'public import stats = "./stats.qui"\n')
    case('public-extension-import', '''
        import lib = "./public_stats.qui"
        int[] values := [1, 2, 3]
        print(values.total() == 6)
    ''')
    write('int_ext', '''
        int tagged(extend tensor<int32> value)
            return 1
    ''')
    write('real_ext', '''
        int tagged(extend tensor<real32> value)
            return 2
    ''')
    case('tensor-element-resolution', '''
        import ints = "./int_ext.qui"
        import reals = "./real_ext.qui"
        tensor<int32> a := tensor.zeros<int32>([1])
        tensor<real32> b := tensor.zeros<real32>([1])
        print(a.tagged() == 1 and b.tagged() == 2)
    ''')
    write('integer_ext', '''
        int tagged<T: integer>(extend T value)
            return 1
    ''')
    write('floating_ext', '''
        int tagged<T: floating>(extend T value)
            return 2
    ''')
    case('constrained-extension-resolution', '''
        import ints = "./integer_ext.qui"
        import reals = "./floating_ext.qui"
        int a := 1
        real32 b := 1.0
        print(a.tagged() == 1 and b.tagged() == 2)
    ''')
    write('numeric_argument', '''
        int tagged<T: numeric>(extend T value, int other)
            return 1
    ''')
    write('text_argument', '''
        int tagged<T: numeric>(extend T value, string other)
            return 2
    ''')
    case('generic-literal-argument-resolution', '''
        import nums = "./numeric_argument.qui"
        import texts = "./text_argument.qui"
        int value := 1
        print(value.tagged(2) == 1 and value.tagged<int>(2) == 1 and
              value.tagged("text") == 2)
    ''')
    case('generic-whole-array', '''
        nat count<T>(extend T values)
            return values.len()
        int[] a := [1, 2]
        print(a.count() == 2)
    ''')
    case('string-method-collision', '''
        string trim(extend string value)
            return value
        print(" x ".trim())
    ''', 'EXTENSION_COLLISION')
    case('tensor-method-collision', '''
        int device(extend tensor<real32> value)
            return 1
        tensor<real32> a := tensor.zeros<real32>([1])
        print(a.device())
    ''', 'EXTENSION_COLLISION')
    case('autograd-method-collision', '''
        bool has_grad(extend autograd.Target value)
            return true
        autograd.Target target := autograd.target()
        print(target.has_grad())
    ''', 'EXTENSION_COLLISION')
    case('file-method-collision', '''
        bool line(extend file.write &value)
            return true
        file.write handle := file.write("unused")
        .handle.line()
    ''', 'EXTENSION_COLLISION')
    case('receiver-once-order', '''
        int receiver()
            print("r")
            return 1
        int argument()
            print("a")
            return 2
        int plus(extend int value, int other)
            return value + other
        print(receiver().plus(argument()))
    ''', expected='ra3')

    # Every compound form shares the binary array operator's numeric/shape rules.
    operations = [('+', [15, 24], [15, 21]), ('-', [9, 12], [9, 15]),
                  ('*', [36, 108], [36, 54]), ('/', [4, 3], [4, 6]),
                  ('%', [0, 0], [0, 0])]
    for index, (operator, pair, scalar) in enumerate(operations):
        case(f'array-compound-{index}', f'''
            int64[] pair := [12, 18]
            int64[] scalar := [12, 18]
            pair {operator}= [3, 6]
            scalar {operator}= 3
            int64[] expected_pair := [{pair[0]}, {pair[1]}]
            int64[] expected_scalar := [{scalar[0]}, {scalar[1]}]
            print(pair == expected_pair and scalar == expected_scalar)
        ''')
    case('array-compound-fixed-nested-and-common-type', '''
        int32[2] fixed := [1, 2]
        fixed += [3, 4]
        int16[2] small := [1, 2]
        fixed += small
        int32[2] expected_fixed := [5, 8]
        int64[][] nested := [[1], [2, 3]]
        nested += [[4], [5, 6]]
        nested *= 2
        int64[][] expected_nested := [[10], [14, 18]]
        print(fixed == expected_fixed and nested == expected_nested)
    ''')
    case('array-compound-exact-real', '''
        real[] values := [real(1), real(2)]
        values += real(1) / real(2)
        print(values[0] == real(3) / real(2) and values[1] == real(5) / real(2))
    ''')
    case('array-compound-complex-literal', '''
        com32[] values := [1.0 + 1.0i, 2.0 + 2.0i]
        values += 1.0i
        com32[] expected := [1.0 + 2.0i, 2.0 + 3.0i]
        print(values == expected)
    ''')
    case('array-compound-root-replacement', '''
        int64 change(int64[] &values)
            values = [90, 91, 92]
            return 3
        int64[] values := [1, 2]
        int64[] independent := values
        values += change(&values)
        int64[] expected := [4, 5]
        int64[] original := [1, 2]
        print(values == expected and independent == original)
    ''')
    case('array-compound-rhs-element-mutation', '''
        int64 change(int64[] &values)
            values[0] = 99
            return 1
        int64[] values := [1, 2]
        values += change(&values)
        int64[] expected := [2, 3]
        print(values == expected)
    ''')
    case('array-compound-index-once-current-storage', '''
        int next(int &calls)
            calls += 1
            print("i")
            return 0
        int64[] replace(int64[][] &rows)
            print("r")
            rows = [[90, 91], [80, 81]]
            return [3, 4]
        int64[][] rows := [[1, 2]]
        int calls := 0
        rows[next(&calls)] += replace(&rows)
        print("={calls}:{rows[0][0]}:{rows[0][1]}:{rows[1][0]}")
    ''', expected='ir=1:4:6:80')
    case('array-compound-current-object-field', '''
        class Box(int64[] values, int64 stamp)
        int64[] replace(Box &box)
            box = Box([90, 91], 7)
            return [3, 4]
        box := Box([1, 2], 0)
        box.values += replace(&box)
        int64[] expected := [4, 6]
        print(box.values == expected and box.stamp == 7)
    ''')
    case('array-compound-fallible-rhs', '''
        int32[] | error increments()
            return [int32(2), int32(3)]
        int64 | error scalar()
            return int64(1)
        int64[] values := [1, 2]
        values += increments()
        values += scalar()
        int64[] expected := [4, 6]
        print(values == expected)
    ''')
    case('array-compound-try-error', '''
        int64[] | error fail()
            return error("array RHS failed")
        bool | error attempt()
            int64[] values := [1, 2]
            values += try fail()
            return true
        match attempt()
            bool value
                print(false)
            error problem
                print(true)
    ''')
    case('array-compound-const', '''
        const int64[] values := [1, 2]
        values += 1
    ''', 'WRITE_CAPABILITY')
    case('array-compound-mask-read-only', '''
        int64[] values := [1, 2]
        bool[] mask := [true, false]
        values[mask] += 1
    ''', 'INVALID_ASSIGNMENT')
    case('array-compound-slice-not-whole-storage', '''
        int64[] values := [1, 2]
        values[0:1] += [3, 4]
    ''', 'INVALID_ASSIGNMENT')
    case('array-compound-fixed-shape', '''
        int64[2] values := [1, 2]
        int64[3] other := [3, 4, 5]
        values += other
    ''', 'ARRAY_SHAPE')
    case('array-compound-rank', '''
        int64[][] values := [[1, 2]]
        int64[] other := [3, 4]
        values += other
    ''', 'ARRAY_SHAPE')
    case('array-compound-widening-result', '''
        int32[] values := [1, 2]
        real64[] other := [real64(3), real64(4)]
        values += other
    ''', 'TYPE_MISMATCH')
    case('array-compound-remainder-real', '''
        real64[] values := [real64(1), real64(2)]
        values %= 2
    ''', 'TYPE_MISMATCH')
    case('array-compound-uninitialized-elements', '''
        int64[2] values
        values += 1
    ''', 'UNINITIALIZED')

    def runtime_failure(name, source, code):
        path = write(name, source)
        if check_only:
            result = subprocess.run([compiler, 'check', str(path), '--json'],
                                    text=True, capture_output=True, env=env, timeout=120)
            passed = result.returncode == 0
        else:
            result = subprocess.run([compiler, str(path)], text=True,
                                    capture_output=True, env=env, timeout=120)
            passed = result.returncode == 101 and f'[{code}]' in result.stderr
        if not passed:
            failures.append(name)
            print(f'FAIL {name}: exit {result.returncode}\n{result.stdout}{result.stderr}', flush=True)
        else:
            print(f'PASS {name}', flush=True)

    runtime_failure('array-compound-runtime-shape', '''
        int64[][] values := [[1], [2, 3]]
        int64[][] other := [[4], [5]]
        values += other
    ''', 'SHAPE_MISMATCH')
    runtime_failure('array-compound-overflow', '''
        int8[] values := [127]
        values += 1
    ''', 'INTEGER_OVERFLOW')
    runtime_failure('array-compound-zero-divisor', '''
        int64[] values := [1, 2]
        values /= 0
    ''', 'DIVISION_BY_ZERO')
    runtime_failure('array-compound-operand-range', '''
        int32[] values := [1]
        nat64 large := 2147483648
        values += large
    ''', 'NUMERIC_CONVERSION')
    runtime_failure('array-compound-current-storage-bounds', '''
        int64[] shrink(int64[][] &rows)
            rows = []
            return [3, 4]
        int64[][] rows := [[1, 2]]
        rows[0] += shrink(&rows)
    ''', 'INDEX_BOUNDS')

if failures:
    raise SystemExit(f'{len(failures)} array/extension contracts failed: ' + ', '.join(failures))
print('array/extension contracts: ok')
