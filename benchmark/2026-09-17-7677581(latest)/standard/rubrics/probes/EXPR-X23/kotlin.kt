import java.nio.ByteBuffer
fun main() { val buf = ByteBuffer.allocate(4); val view = buf.slice(1, 2)
  view.put(0, 99); view.put(1, 99); println("X23 ${buf.get(1)} ${buf.get(2)}") }
