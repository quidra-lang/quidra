import java.util.*;
public class Main { public static void main(String[] a){
  List<Integer> l = new ArrayList<>(List.of(1,2,3));
  Map<String,Integer> m = new HashMap<>(Map.of("a",1,"b",2));
  Set<Integer> s = new HashSet<>(List.of(1,2,3));
  System.out.println("X14 " + l.size() + " " + m.get("b") + " " + s.size()); } }
