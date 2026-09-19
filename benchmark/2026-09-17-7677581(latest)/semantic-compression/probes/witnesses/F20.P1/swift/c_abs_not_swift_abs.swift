import Darwin

typealias CAbs = @convention(c) (Int32) -> Int32
let cAbs = unsafeBitCast(dlsym(dlopen(nil, RTLD_LAZY), "abs"), to: CAbs.self)
print(cAbs(Int32.min))
