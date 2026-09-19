package probe

import kotlin.test.Test
import kotlin.test.assertEquals

class ProjectTests {
    @Test
    fun scaleWorks() { assertEquals(6, scale(2)) }

    @Test
    fun scaleAndOffsetWorks() { assertEquals(7, scaleAndOffset(2)) }
}
