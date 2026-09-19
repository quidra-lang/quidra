let w = Int32.max &+ 1
let c = Int32.max.addingReportingOverflow(1)
print("X24", w, c.overflow ? "none" : "some")
