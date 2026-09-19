import java.util.Iterator;
public class Main {
  record Upto(int n) implements Iterable<Integer> {
    public Iterator<Integer> iterator(){ return new Iterator<>() { int i = 0;
      public boolean hasNext(){ return i < n; } public Integer next(){ return i++; } }; } }
  public static void main(String[] a){ StringBuilder sb = new StringBuilder("X07");
    for (int v : new Upto(3)) sb.append(" ").append(v); System.out.println(sb); } }
