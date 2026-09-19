import java.util.regex.*;
public class Main { public static void main(String[] a){
  Matcher m = Pattern.compile("(\\d{4})-(\\d{2})").matcher("date 2026-09-17");
  if (m.find()) System.out.println("X21 " + m.group(1) + " " + m.group(2)); } }
