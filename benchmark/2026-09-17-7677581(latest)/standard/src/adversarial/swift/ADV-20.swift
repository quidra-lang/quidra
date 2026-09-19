import Foundation

func f(_ n: Int64) -> Int64 {
    if n == 1000000 { return 0 }
    return 1 + f(n + 1)
}

print("ADV-START"); fflush(stdout)
let n = Int64(readLine()!)!
let r = f(n)
print("OBS=R:\(r)"); fflush(stdout)
print("ADV-END"); fflush(stdout)
