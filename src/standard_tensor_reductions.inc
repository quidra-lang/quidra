R"QUI(
int element_count(int[] shape, string operation)
    count := 1
    for extent := shape[]
        if extent != 0 and count > 9223372036854775807 / extent
            error("tensor." + operation + " element count overflows int", code = "MATH_SIZE")
        count = count * extent
    return count

// Describe the result shape and the logical input strides of its slices.
// Without an axis, the whole input is one row in logical order.
(int[], int, int, int) layout(nat[] shape, nat | none dim, string operation)
    int[] extents := int(shape)
    match dim
        nat axis
            if axis >= extents.len()
                error("tensor." + operation + " dimension is outside the tensor rank", code = "MATH_SHAPE")
            int axis_index := int(axis)
            int rank := int(extents.len())
            int[] output := array(nat(rank - 1), fill = 0)
            for: 0 <= int index < rank - 1
                if index < axis_index
                    output[index] = extents[index]
                else
                    output[index] = extents[index + 1]
            rows := element_count(output, operation)
            inner := 1
            if rows != 0
                for: axis_index + 1 <= int index < rank
                    inner = inner * extents[index]
            return (output, extents[axis_index], rows, inner)
        none
            int[] scalar := []
            return (scalar, element_count(extents, operation), 1, 1)

int row_base(int row, int width, int inner)
    return (row / inner) * width * inner + row % inner

// Put each slice into a row, retaining a differentiable view or gather.
tensor<T> pack_rows<T: numeric>(tensor<T> value, int width, int rows, int inner)
    if inner == 1 and value.is_contiguous()
        return value.reshape([nat(rows), nat(width)])
    indices := array(nat(rows * width), fill = 0)
    for: 0 <= int row < rows
        base := row_base(row, width, inner)
        for: 0 <= int offset < width
            indices[row * width + offset] = base + offset * inner
    return value.gather(indices, [rows, width])

// Pairwise sums with the same odd-tail parenthesization as Math's whole
// tensor sum. A tail must stay out of the arithmetic until combined: padding
// with an expression such as tail * 0 would turn infinities into NaNs.
tensor<T> sum_rows<T: numeric>(tensor<T> packed, int width, int rows)
    current := packed
    length := width
    tensor<T>[] tails := []
    while length > 1
        pairs := length / 2
        left_indices := array(nat(rows * pairs), fill = 0)
        right_indices := array(nat(rows * pairs), fill = 0)
        for: 0 <= int row < rows
            for: 0 <= int pair < pairs
                index := row * pairs + pair
                left_indices[index] = row * length + pair * 2
                right_indices[index] = left_indices[index] + 1
        if length % 2 == 1
            tail_indices := array(nat(rows), fill = 0)
            for: 0 <= int row < rows
                tail_indices[row] = row * length + length - 1
            tails = tails.append(current.gather(tail_indices, [rows, 1]))
        left := current.gather(left_indices, [rows, pairs])
        right := current.gather(right_indices, [rows, pairs])
        current = left + right
        length = pairs
    if tails.len() > 0
        remaining := tails[0]
        for: 1 <= int index < int(tails.len())
            remaining = tails[index] + remaining
        current = current + remaining
    return current

// Build the divisor in the tensor dtype and placement. This retains Math's
// typed counting and avoids a device-to-host read for scalar arithmetic.
tensor<T> count_tensor<T: floating>(int count, int device)
    tensor<T> result
    tensor<T> power
    if device >= 0
        result = tensor.zeros<T>([1, 1], gpu = nat(device))
        power = tensor.ones<T>([1, 1], gpu = nat(device))
    else
        result = tensor.zeros<T>([1, 1])
        power = tensor.ones<T>([1, 1])
    remaining := count
    while remaining > 0
        if remaining % 2 == 1
            result = result + power
        remaining = remaining / 2
        if remaining > 0
            power = power + power
    return result

tensor<T> sum<T: ordered>(extend tensor<T> value, nat | none dim := none)
    (output, width, rows, inner) := layout(value.shape(), dim, "sum")
    if width == 0 or rows == 0
        // Empty scatter produces typed zeros and preserves the zero Jacobian.
        return value.scatter([], output)
    packed := pack_rows<T>(value, width, rows, inner)
    return sum_rows<T>(packed, width, rows).reshape(nat(output))

tensor<T> mean<T: floating>(extend tensor<T> value, nat | none dim := none)
    (output, width, rows, inner) := layout(value.shape(), dim, "mean")
    if width == 0
        error("tensor.mean requires a non-empty reduction axis", code = "MATH_SHAPE")
    if rows == 0
        return value.scatter([], output)
    packed := pack_rows<T>(value, width, rows, inner)
    average := sum_rows<T>(packed, width, rows) / count_tensor<T>(width, value.device())
    return average.reshape(nat(output))

tensor<T> extrema<T: ordered>(tensor<T> value, nat | none dim, bool maximum, string operation)
    (output, width, rows, inner) := layout(value.shape(), dim, operation)
    if width == 0
        error("tensor." + operation + " requires a non-empty reduction axis", code = "MATH_SHAPE")
    if rows == 0
        return value.scatter([], output)
    if width == 1
        return pack_rows<T>(value, width, rows, inner).reshape(nat(output))

    // Core lacks an on-device index reduction. Read candidates from one
    // detached CPU snapshot, then gather winners from the original input so only the
    // first strictly winning element gets a gradient. Strict comparison also
    // retains the first element's NaN and skips later NaNs, as Math does.
    selector := value.untrack().cpu().contiguous().reshape([nat(rows * width)])
    selected := array(nat(rows), fill = 0)
    for: 0 <= int row < rows
        base := row_base(row, width, inner)
        best_index := base
        best := selector[base].item()
        for: 1 <= int offset < width
            candidate_index := base + offset * inner
            candidate := selector[candidate_index].item()
            better := candidate > best
            if not maximum
                better = candidate < best
            if better
                best = candidate
                best_index = candidate_index
        selected[row] = best_index
    return value.gather(selected, output)

tensor<T> min<T: ordered>(extend tensor<T> value, nat | none dim := none)
    return extrema<T>(value, dim, false, "min")

tensor<T> max<T: ordered>(extend tensor<T> value, nat | none dim := none)
    return extrema<T>(value, dim, true, "max")

tensor<T> variance<T: floating>(tensor<T> value, nat | none dim, nat ddof, string operation)
    (output, width, rows, inner) := layout(value.shape(), dim, operation)
    if ddof >= nat(width)
        error("tensor." + operation + " requires ddof less than the reduction size", code = "REDUCTION_DDOF")
    if rows == 0
        return value.scatter([], output)
    packed := pack_rows<T>(value, width, rows, inner)
    device := value.device()
    count := count_tensor<T>(width, device)
    average := sum_rows<T>(packed, width, rows) / count
    delta := packed - average
    denominator := count
    if ddof != 0
        denominator = count_tensor<T>(width - int(ddof), device)
    result := sum_rows<T>(delta * delta, width, rows) / denominator
    return result.reshape(nat(output))

tensor<T> var<T: floating>(extend tensor<T> value, nat | none dim := none, nat ddof := 0)
    return variance<T>(value, dim, ddof, "var")

tensor<T> std_root<T: floating>(tensor<T> value)
    return value ^ T(0.5)

// A GPU can support float32 square root without a half-precision power
// kernel. Keep the variance in its input dtype and promote only the root.
tensor<real16> std_root(tensor<real16> value)
    if value.device() < 0
        return value ^ real16(0.5)
    return real16(real32(value) ^ real32(0.5))

tensor<real16b> std_root(tensor<real16b> value)
    if value.device() < 0
        return value ^ real16b(0.5)
    return real16b(real32(value) ^ real32(0.5))

tensor<T> std<T: floating>(extend tensor<T> value, nat | none dim := none, nat ddof := 0)
    variances := variance<T>(value, dim, ddof, "std")
    shape := int(variances.shape())
    count := element_count(shape, "std")
    if count == 0
        return variances
    if not variances.is_tracked()
        return std_root(variances)

    // Choose a zero subgradient on constant slices. Do not evaluate sqrt's
    // singular backward at zero, even with an upstream gradient of zero.
    // Only the selection uses the snapshot; sqrt and the graph stay on the
    // input device. For an all-zero result the empty scatter retains the
    // dependency and propagates a zero gradient.
    selector := variances.untrack().cpu().reshape([nat(count)])
    indices := array(nat(count), fill = 0)
    kept := 0
    for: 0 <= int index < count
        if selector[index].item() != T(0)
            indices[kept] = index
            kept = kept + 1
    if kept == count
        return std_root(variances)
    selected := indices[:<kept]
    roots := variances.gather(selected, [kept])
    if kept != 0
        roots = std_root(roots)
    return roots.scatter(selected, shape)
)QUI"
