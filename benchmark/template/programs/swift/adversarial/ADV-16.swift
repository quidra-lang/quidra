import Foundation

func body() {
    let v: Int64 = 10
    v = 20
    print("OBS=V:\(v)"); fflush(stdout)
}

print("ADV-START"); fflush(stdout)
body()
print("ADV-END"); fflush(stdout)
