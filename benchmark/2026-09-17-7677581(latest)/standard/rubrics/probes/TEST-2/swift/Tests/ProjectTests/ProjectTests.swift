import XCTest
import ModA
import ModB

final class ProjectTests: XCTestCase {
    func testScale() { XCTAssertEqual(scale(2), 6) }
    func testScaleAndOffset() { XCTAssertEqual(scaleAndOffset(2), 7) }
}
