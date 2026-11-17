import java.util.List;
public class LocalFixture { public static int inspect(List<String> names) { int count=names.size(); if(count>1) { String first=names.get(0); count+=first.length(); } return count; } }
