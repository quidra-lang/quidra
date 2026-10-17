# Meaning-counting checklist

Count the distinct facts guaranteed by the visible code and the applicable language or library specification, assuming a complete, error-free program. Apply every applicable item at the stated observation point: 1 per guaranteed fact, 0 if unknown or inapplicable, and no duplicate points for the same fact. Each parameter, element, axis, or other target is assessed separately. Examples are independent [Quidra](../docs/spec/language.md) fragments.

| Scoring item | Syntax that establishes the guarantee |
| --- | --- |
| Name of each ordinary variable (excluding parameters and fields) | `int count = 3` |
| Declared or inferred type of each ordinary variable (excluding parameters and fields) | `int[] original = [-3, 1, -2, 4, 0, 5]` |
| Initial value of each scalar variable | `int count = 3` |
| Initial value at each collection position (one fact per position) | `int[] original = [-3, 1, -2, 4, 0, 5]` |
| Initial value of each object field | `class Counter()`<br>`    int count = 0`<br>`Counter counter = Counter()` |
| Collection length at initialization | `int[] original = [-3, 1, -2, 4, 0, 5]` |
| Fixed-length constraint on a collection | `int[6] values = [-3, 1, -2, 4, 0, 5]` |
| Collection rank | `tensor<real32>{2, 3} matrix` |
| Extent of each axis (one fact per axis) | `tensor<real32>{2, 3} matrix` |
| Name bound to each axis extent | `tensor<real32>{nat N, N} matrix = tensor.zeros<real32>([2, 2])` |
| Equality or arithmetic relationship between axis extents | `tensor<real32>{nat N, N} matrix = tensor.zeros<real32>([2, 2])` |
| Name of each declared class | `class Point(real64 x, real64 y)` |
| Name of each object field | `class Point(real64 x, real64 y)` |
| Type of each object field | `class Point(real64 x, real64 y)` |
| Visibility of a declaration outside its scope or module | `private int twice(int value);` |
| Lexical scope of a variable | `if true`<br>`    local int count = 3` |
| Whether a binding can be reassigned after initialization | `const int limit = 6` |
| Destination storage of an assignment | `int[] values = [1, 2]`<br>`values[0] = 7` |
| Source value or expression used by an assignment | `int[] original = [-3, 1]`<br>`int[] edited = original` |
| Assignment model: value binding, reference binding, or the language's equivalent | `int source = 3`<br>`int &alias = &source` |
| Target storage of a reference binding | `int source = 3`<br>`int &alias = &source` |
| Observable independence of nested values after copying | `int[][] original = [[1, 2], [3, 4]]`<br>`int[][] edited = original` |
| Sharing relationship of reference fields after copying an object | `class Holder(local int &source)`<br>`    int &target`<br>`    &target = &source`<br>`int value = 3`<br>`Holder original = Holder(&value)`<br>`Holder edited = original` |
| Function name | `void inspect(int[] img);` |
| Function result type on normal return | `void inspect(int[] img);` |
| Name of each parameter | `void inspect(int[] img);` |
| Type of each parameter | `void inspect(int[] img);` |
| Required argument count | `int add(int left, int right = 1);` |
| Default value of each optional parameter | `int add(int left, int right = 1);` |
| Parameter-passing model: value, reference, or the language's equivalent | `void inspect(int[] img);` / `void update(int[] &img);` |
| Whether a parameter grants write access to the caller's storage | `void update(int[] &img);` |
| Whether a parameter's own value can be modified | `void inspect(const int[] img);` |
| Argument-to-parameter correspondence | `int subtract(int left, int right);`<br>`int result = subtract(right = 2, left = 7)` |
| Evaluation order of call arguments | `int subtract(int left, int right);`<br>`int result = subtract(right = 2, left = 7)` |
| Observable independence of mutable default values between calls | `void inspect(int[] values = [1, 2]);` |
| Equality relationship between generic parameter and result types | `T identity<T>(T value)`<br>`    return value` |
| Write authority over hidden pre-existing program state (external I/O excluded) | `void .update();` |
| Whether a function always writes or never writes caller-visible memory | `void inspect(int[] values);` / `void clear(int &value)`<br>`    value = 0` |
| Value produced by a return statement | `int answer()`<br>`    return 42` |
| Control transfer out of a function | `int answer()`<br>`    return 42` |
| Arithmetic operation performed | `int total = 2 + 3` |
| Source expression of each operand, including comparison thresholds (one fact per operand) | `int count = 3`<br>`int total = count + 2` |
| Comparison relation | `int count = 3`<br>`bool large = count > 2` |
| Boolean operation (one fact per operation) | `bool accepted = true and not false` |
| Short-circuit evaluation condition | `bool positive_ratio(int numerator, int denominator)`<br>`    return denominator != 0 and numerator / denominator > 0` |
| Elementwise application of an operation | `int[] values = [1, 2, 3]`<br>`int[] doubled = values * 2` |
| Predicate selecting elements | `int[] values = [-2, 3, 0, 4]`<br>`int[] positive = values[values > 0]` |
| Order of selected output elements | `int[] values = [-2, 3, 0, 4]`<br>`int[] positive = values[values > 0]` |
| Indices selecting output elements, including repeated positions | `int[] values = [10, 20, 30]`<br>`int[] indices = [2, 0, 2]`<br>`int[] selected = values[indices]` |
| Reduction operation | `import math`<br>`int[] values = [1, 2, 3]`<br>`int total = math.sum(values)` |
| Conversion target type | `int value = 3`<br>`real64 converted = real64(value)` |
| Branch condition | `int choose(bool ready)`<br>`    return if ready then 1 else 0` |
| Value selected on each branch | `int choose(bool ready)`<br>`    return if ready then 1 else 0` |
| Loop start bound | `for 0 <= int i < 6 step 2` |
| Loop end bound | `for 0 <= int i < 6 step 2` |
| Inclusion or exclusion of the start bound | `for 0 <= int i < 6 step 2` |
| Inclusion or exclusion of the end bound | `for 0 <= int i < 6 step 2` |
| Loop step | `for 0 <= int i < 6 step 2` |
| Source collection of an iteration | `int[] values = [1, 2, 3]`<br>`for int value = values[]`<br>`    continue` |
| Value or reference binding of each iterated element | `int[] values = [1, 2, 3]`<br>`for int &value = &values[]`<br>`    continue` |
| Position correspondence when iterating multiple collections | `int[] left = [1, 2]`<br>`int[] right = [3, 4]`<br>`for int a = left[], int b = right[]`<br>`    continue` |
| Condition for continuing a while loop | `int count = 0`<br>`while count < 3`<br>`    count += 1` |
| Exit from the innermost loop | `for int i < 3`<br>`    break` |
| Skip to the next iteration | `for int i < 3`<br>`    continue` |
| Slice start and end positions (one fact per endpoint) | `int[] values = [0, 1, 2, 3, 4]`<br>`int[] part = values[1:<4]` |
| Inclusion or exclusion of each slice endpoint | `int[] values = [0, 1, 2, 3, 4]`<br>`int[] part = values[1:<4]` |
| Slice step | `int[] values = [0, 1, 2, 3, 4]`<br>`int[] part = values[0:4:2]` |
| Behavior when an index is out of bounds | `int at(int[] values, int index)`<br>`    return values[index]` |
| Preservation of a recoverable error alternative | `int \| error parse_value(string text)`<br>`    return int.parse(text)` |
| Propagation of an error to the caller | `int \| error parse_value(string text)`<br>`    return try int.parse(text)` |
| Termination instead of continuing with an error value | `int parse_value(string text)`<br>`    return int.parse(text)` |
| Coverage of every union alternative by a match | `int \| none value = none`<br>`match value`<br>`    int number`<br>`        print(number)`<br>`    none`<br>`        print("none")` |
| Output destination | `print("ready")` |
| Output value | `print("ready")` |
| Completion of parallel tasks before subsequent statements | `int first`<br>`int second`<br>`parallel`<br>`    first = 20`<br>`    second = 22`<br>`print(first + second)` |
| Calling convention at a foreign-language boundary | `export "C" int32 add(int32 left, int32 right)`<br>`    return left + right` |
