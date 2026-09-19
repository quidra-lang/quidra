import Foundation

struct ReadFailed: Error {}
var observedFD: Int32 = -1

func readAll() throws -> Int {
    let handle = try FileHandle(forReadingFrom: URL(filePath: "data.txt"))
    defer { observedFD = handle.fileDescriptor; try? handle.close() }
    throw ReadFailed()
}

do { print(try readAll()) } catch { print("step (2) failed:", error) }
print("fd the defer closed:", observedFD)
print("fcntl(fd, F_GETFD) after scope exit:", fcntl(observedFD, F_GETFD), "errno:", errno, "(EBADF =", EBADF, ")")
