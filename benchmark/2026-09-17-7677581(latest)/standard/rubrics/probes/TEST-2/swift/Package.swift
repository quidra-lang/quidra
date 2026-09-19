// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "test2-swift",
    targets: [
        .target(name: "ModA"),
        .target(name: "ModB", dependencies: ["ModA"]),
        .testTarget(name: "ProjectTests", dependencies: ["ModA", "ModB"]),
    ]
)
