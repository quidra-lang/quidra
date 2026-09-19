import Foundation
final class Boxed: @unchecked Sendable { var v = 0 }
let box = Boxed()
let t = Thread { box.v = 42 }
let sem = DispatchSemaphore(value: 0)
let t2 = Thread { box.v = 42; sem.signal() }
_ = t
t2.start()
sem.wait()
print("X19", box.v)
