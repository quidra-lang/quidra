import Foundation

func pick(_ b: Bool) -> Int64 {
    if b { return 1 }
}

print("ADV-START"); fflush(stdout)
let b = readLine()! == "1"
let r = pick(b) * 2
print("OBS=R:\(r)"); fflush(stdout)
print("ADV-END"); fflush(stdout)
