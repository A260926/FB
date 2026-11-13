import org.jsoup.Jsoup;
import org.jsoup.nodes.Document;
import org.jsoup.nodes.Element;
import org.jsoup.parser.Parser;
import org.jsoup.parser.StreamParser;
import java.util.ArrayList;
import java.util.List;
public class Baseline {
 public static void main(String[] args) throws Exception {
  String html="<ul id=one><li id=a>A</li><li id=b>B</li><li id=c>C</li></ul><ul id=two><li id=d>D</li></ul>";
  Document doc=Jsoup.parse(html);
  if(!doc.select("li:nth-last-child(2)").eachAttr("id").toString().equals("[b]"))throw new AssertionError("ordinary nth matching");
  List<String> actual=new ArrayList<>();
  try(StreamParser p=new StreamParser(Parser.htmlParser()).parse(html,"https://capture.invalid/list")){
   Element e;while((e=p.selectNext("li:nth-last-child(2)"))!=null)actual.add(e.id());
  }
  System.out.println("COMPLETE_TREE_MATCHES "+doc.select("li:nth-last-child(2)").eachAttr("id"));
  System.out.println("STREAM_TRANSIENT_MATCHES "+actual);
  boolean missing=false;try{StreamParser.class.getMethod("selectNextStable",String.class);}catch(NoSuchMethodException e){missing=true;}
  System.out.println("STABLE_STREAM_API_MISSING "+missing);
 }
}
