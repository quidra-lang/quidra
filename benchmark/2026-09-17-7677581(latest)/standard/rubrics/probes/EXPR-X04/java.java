public class Main {
  interface Speaker { String speak(); }
  static class Dog implements Speaker { public String speak(){ return "woof"; } }
  static class Cat implements Speaker { public String speak(){ return "meow"; } }
  static Speaker pick(int n){ return n == 0 ? new Dog() : new Cat(); }
  public static void main(String[] a){ String s = "01";
    System.out.println("X04 " + pick(s.charAt(0)-'0').speak() + " " + pick(s.charAt(1)-'0').speak()); } }
