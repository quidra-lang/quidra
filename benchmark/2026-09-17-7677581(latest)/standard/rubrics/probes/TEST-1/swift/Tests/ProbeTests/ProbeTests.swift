// TEST-1 probe, Swift. Exactly 3 tests using XCTest, which ships with the Swift
// toolchain: two assert a true condition, one asserts a false condition.
import XCTest
@testable import Probe

final class ProbeTests: XCTestCase {
    func testPassOne() { XCTAssertTrue(sum(1, 1) == 2) }
    func testPassTwo() { XCTAssertTrue(sum(2, 3) == 5) }
    func testFailOne() { XCTAssertTrue(sum(1, 1) == 3) }
}
