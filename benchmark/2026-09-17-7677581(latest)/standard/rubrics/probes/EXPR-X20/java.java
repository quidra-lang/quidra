import java.beans.*; import java.io.*; import java.util.*;
public class Main {
  public static class Person { private String name = ""; private int age;
    public String getName(){ return name; } public void setName(String n){ name = n; }
    public int getAge(){ return age; } public void setAge(int v){ age = v; } }
  public static void main(String[] a) throws Exception {
    Person p = new Person(); p.setName("alice"); p.setAge(30);
    ByteArrayOutputStream bo = new ByteArrayOutputStream();
    try (XMLEncoder e = new XMLEncoder(bo)) { e.writeObject(p); }
    String xml = bo.toString("UTF-8");
    if (!xml.contains("alice")) { System.out.println("X20 FAIL"); return; }
    Person q; try (XMLDecoder d = new XMLDecoder(new ByteArrayInputStream(bo.toByteArray()))) { q = (Person) d.readObject(); }
    System.out.println("X20 " + q.getName() + " " + q.getAge()); } }
