// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "test1-swift",
    targets: [
        .target(name: "Probe"),
        .testTarget(name: "ProbeTests", dependencies: ["Probe"]),
    ]
)
