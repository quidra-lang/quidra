import java.beans.XMLEncoder
import java.beans.XMLDecoder
import java.io.ByteArrayOutputStream
import java.io.ByteArrayInputStream
class Person { var name: String = ""; var age: Int = 0 }
fun main() {
  val p = Person(); p.name = "alice"; p.age = 30
  val bo = ByteArrayOutputStream()
  XMLEncoder(bo).use { it.writeObject(p) }
  val q = XMLDecoder(ByteArrayInputStream(bo.toByteArray())).use { it.readObject() as Person }
  println("X20 ${q.name} ${q.age}") }
