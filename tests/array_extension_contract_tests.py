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
        int[] a = [1, 2] + [3, 4]
        int[] b = [1, 2] * 2
        int[] c = 2 * [1, 2]
        int[] sum = [4, 6]
        int[] product = [2, 4]
        print(a == sum and b == product and c == product)
    ''')
    case('typed-literal-comparison', '''
        bool[] mask = [real32(1.0)] < [real32(2.0)]
        bool[] expected = [true]
        print(mask == expected)
    ''')
    case('contextual-comparison', '''
        bool[] mask = [1, 5, 3] < [2, 4, 3]
        bool[] expected = [true, false, false]
        print(mask == expected)
    ''')
    case('mixed-index-literals', '''
        nat i = 1
        int[] a = [10, 20]
        int[] b = a[[0, i]]
        int[] expected = [10, 20]
        print(b == expected)
    ''')
    case('negative-range', '''
        int[] a = -3:0
        int[] expected = [-3, -2, -1, 0]
        print(a == expected)
    ''')
    case('signed-range-power', '''
        int[] a = 0:-3 ^ 2
        int[] b = -3:0 ^ 2
        int[] c = 0:-3:-2
        int[] expected_a = [0, 1, 4, 9]
        int[] expected_b = [9, 4, 1, 0]
        int[] expected_c = [0, -2]
        print(a == expected_a and b == expected_b and c == expected_c)
    ''')
    case('equal-end-negative-step', '''
        int begin = 3
        int step = -2
        int[] values = begin:begin:step
        int[] expected = [3]
        print(values == expected)
    ''')
    case('array-rhs-mutation', '''
        int mutate(int[] &a)
            a = [8, 9]
            return 1
        int[] a = [1, 2]
        int[] result = a + mutate(&a)
        int[] expected = [2, 3]
        print(result == expected)
    ''')
    case('selector-mutation', '''
        int[] selector(int[] &value)
            value = [8, 9]
            return [1, 0, 1]
        int[] value = [1, 2]
        int[] selected = value[selector(&value)]
        int[] expected = [2, 1, 2]
        print(selected == expected)
    ''')
    case('large-step', '''
        int[] a = 0:18446744073709551616:18446744073709551616
        int[] expected = [0, 18446744073709551616]
        print(a == expected)
    ''')
    case('empty-marked-range', '''
        int[] a = 3:<0
        int[] b = 0:>3
        print(len(a) == 0 and len(b) == 0)
    ''')
    case('uninitialized-arithmetic', '''
        int[1] a
        int[] b = a + 1
    ''', 'UNINITIALIZED')
    case('array-comparison-chain', '''
        int[] a = [1, 2]
        bool result = 0 < a < 10
    ''', 'CHAINED_COMPARISON')

    write('stats', '''
        int total(extend int[] values)
            int result = 0
            for int value = values[]
                result += value
            return result
        int twice(extend int value)
            return value * 2
    ''')
    case('gather-method', '''
        import stats = "./stats.qui"
        int[] values = [1, 2, 3]
        int[] idx = [2, 0]
        print(values[idx].total() == 4 and values[values > 1].total() == 5)
    ''')
    case('slice-method', '''
        import stats = "./stats.qui"
        int[] values = [1, 2, 3]
        print(values[1:].total() == 5)
    ''')
    case('chain-method', '''
        import stats = "./stats.qui"
        int[] values = [1, 2, 3]
        print(values.total().twice() == 12)
    ''')
    case('contextual-extension-arguments', '''
        int first(extend int[] value)
            return value[0]
        int plus(extend int value, int[] other)
            return value + other[0]
        int value = 2
        print([1, 2].first() == first([1, 2]) and value.plus([3, 4]) == 5)
    ''')
    case('native-chain-and-unary', '''
        import stats = "./stats.qui"
        int[] values = [1, 2]
        int value = 3
        print(values.append(3).total() == 6 and (-value).twice() == -6 and
              (values + value).total() == 9)
    ''')
    case('scalar-array-specializations', '''
        T twice<T: numeric>(extend T value)
            return value + value
        T[] twice<T: numeric>(extend T[] value)
            return value + value
        int value = 3
        int[] values = [1, 2]
        int[] expected = [2, 4]
        print(value.twice() == 6 and values.twice() == expected)
    ''')
    case('const-explicit-and-named', '''
        T plus<T: numeric>(extend const T value, T other = T(2))
            return value + other
        int value = 3
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
        int[] value = [4, 5]
        print(value.total())
    ''', 'UNKNOWN_MEMBER')
    write('internal_user', '''
        import stats = "./stats.qui"
        int run(int[] values)
            return values.total()
    ''')
    case('module-local-import', '''
        import lib = "./internal_user.qui"
        int[] values = [1, 2, 3]
        print(lib.run(values) == 6)
    ''')
    case('no-transitive-extension', '''
        import lib = "./internal_user.qui"
        int[] values = [1, 2, 3]
        print(values.total())
    ''', 'UNKNOWN_MEMBER')
    write('public_stats', 'public import stats = "./stats.qui"\n')
    case('public-extension-import', '''
        import lib = "./public_stats.qui"
        int[] values = [1, 2, 3]
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
        tensor<int32> a = tensor.zeros<int32>([1])
        tensor<real32> b = tensor.zeros<real32>([1])
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
        int a = 1
        real32 b = 1.0
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
        int value = 1
        print(value.tagged(2) == 1 and value.tagged<int>(2) == 1 and
              value.tagged("text") == 2)
    ''')
    case('generic-whole-array', '''
        nat count<T>(extend T values)
            return len(values)
        int[] a = [1, 2]
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
        tensor<real32> a = tensor.zeros<real32>([1])
        print(a.device())
    ''', 'EXTENSION_COLLISION')
    case('autograd-method-collision', '''
        bool has_grad(extend autograd.Target value)
            return true
        autograd.Target target = autograd.target()
        print(target.has_grad())
    ''', 'EXTENSION_COLLISION')
    case('file-method-collision', '''
        void close(extend file.Handle &value)
            return
        file.Handle handle = file.open("unused")
        .handle.close()
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

if failures:
    raise SystemExit(f'{len(failures)} array/extension contracts failed: ' + ', '.join(failures))
print('array/extension contracts: ok')
