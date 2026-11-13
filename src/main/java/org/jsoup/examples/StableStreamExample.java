package org.jsoup.examples;

import org.jsoup.nodes.Element;
import org.jsoup.parser.Parser;
import org.jsoup.parser.StreamParser;

import java.io.IOException;
import java.io.StringReader;

/**
 Example of {@link StreamParser#selectNextStable(String)}: streaming the second-to-last item of every list without
 buffering the whole document. Unlike {@link StreamParser#selectNext(String)}, a candidate that only matches the
 provisional tree is held back until its parent group closes, so position-dependent selectors such as
 {@code :nth-last-child} return exactly the elements a full parse would match.
 <p>To invoke from the command line, assuming you've downloaded the jsoup-examples jar to your current directory:</p>
 <p><code>java -cp jsoup-examples.jar org.jsoup.examples.StableStreamExample</code></p>
 */
public class StableStreamExample {
    public static void main(String[] args) throws IOException {
        // imagine this is a long directory page being downloaded, with many independent lists:
        String html = "<ul id=one><li id=a>Alpha</li><li id=b>Bravo</li><li id=c>Charlie</li></ul>"
            + "<ul id=two><li id=d>Delta</li><li id=e>Echo</li><li id=f>Foxtrot</li></ul>";

        StreamParser parser = new StreamParser(Parser.htmlParser()).parse(new StringReader(html), "");
        try {
            Element penultimate;
            // the query is fixed for this parse; repeat calls keep consuming the same candidate stream
            while ((penultimate = parser.selectNextStable("li:nth-last-child(2)")) != null) {
                // b is handed over when <ul id=one> closes, before the second list is read
                System.out.printf("%s -> %s%n", penultimate.parent().id(), penultimate.text());
            }
        } finally {
            parser.close();
        }
    }
}
