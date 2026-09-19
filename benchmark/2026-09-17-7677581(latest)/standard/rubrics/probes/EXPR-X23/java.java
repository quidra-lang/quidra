import java.nio.*;
public class Main { public static void main(String[] a){
  ByteBuffer buf = ByteBuffer.allocate(4);
  ByteBuffer view = buf.slice(1, 2);
  view.put(0, (byte)99); view.put(1, (byte)99);
  System.out.println("X23 " + buf.get(1) + " " + buf.get(2)); } }
