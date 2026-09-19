import Foundation

func scale(_ n: Int64) -> Int64 {
    return n * 3
}

print("ADV-START"); fflush(stdout)
let s = readLine()!
let r = scale(s)
print("OBS=R:\(r)"); fflush(stdout)
print("ADV-END"); fflush(stdout)
