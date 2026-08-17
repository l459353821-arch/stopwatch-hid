// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "DeepSeekBalanceClient",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "ds-balance", targets: ["DeepSeekBalanceClient"]),
    ],
    targets: [
        .executableTarget(name: "DeepSeekBalanceClient"),
    ]
)
