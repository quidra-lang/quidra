fun main() { val m = Regex("""(\d{4})-(\d{2})""").find("date 2026-09-17")!!
  println("X21 ${m.groupValues[1]} ${m.groupValues[2]}") }
