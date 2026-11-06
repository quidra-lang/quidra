R"QUI(
T sum<T: numeric>(extend T[] value)
    total := T(0)
    for element := value[]
        total = total + element
    return total

T mean<T: numeric>(extend T[] value)
    if value.len() == 0
        error("mean requires at least one element", code = "MATH_SHAPE")
    total := sum<T>(value)
    count := T(value.len())
    return total / count

real mean<T: integer>(extend T[] value)
    if value.len() == 0
        error("mean requires at least one element", code = "MATH_SHAPE")
    total := 0.0
    for element := value[]
        total = total + element
    return total / value.len()

T min<T: ordered>(extend T[] value)
    if value.len() == 0
        error("min requires at least one element", code = "MATH_SHAPE")
    best := value[0]
    for: 1 <= nat index < value.len()
        candidate := value[index]
        if candidate < best
            best = candidate
    return best

T max<T: ordered>(extend T[] value)
    if value.len() == 0
        error("max requires at least one element", code = "MATH_SHAPE")
    best := value[0]
    for: 1 <= nat index < value.len()
        candidate := value[index]
        if candidate > best
            best = candidate
    return best

nat variance_denominator(nat count, nat ddof)
    if ddof >= count
        error("variance requires ddof less than the number of elements", code = "REDUCTION_DDOF")
    return count - ddof

T var<T: ordered>(extend T[] value, nat ddof := 0)
    denominator := variance_denominator(value.len(), ddof)
    average := mean<T>(value)
    total := T(0)
    for element := value[]
        delta := element - average
        total = total + delta * delta
    return total / T(denominator)

real var<T: integer>(extend T[] value, nat ddof := 0)
    denominator := variance_denominator(value.len(), ddof)
    average := mean<T>(value)
    total := 0.0
    for element := value[]
        delta := element - average
        total = total + delta * delta
    return total / denominator

T std<T: ordered>(extend T[] value, nat ddof := 0)
    return var<T>(value, ddof) ^ T(0.5)

real std<T: integer>(extend T[] value, nat ddof := 0)
    return var<T>(value, ddof) ^ 0.5
)QUI"
