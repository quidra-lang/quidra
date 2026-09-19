interface Speaker { fun speak(): String }
class Dog : Speaker { override fun speak() = "woof" }
class Cat : Speaker { override fun speak() = "meow" }
fun pick(n: Int): Speaker = if (n == 0) Dog() else Cat()
fun main() { val s = "01"; println("X04 ${pick(s[0] - '0').speak()} ${pick(s[1] - '0').speak()}") }
