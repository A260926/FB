package com.strobel.decompiler;
import java.nio.file.*;
import java.util.*;
import com.strobel.assembler.InputTypeLoader;
import com.strobel.decompiler.languages.Languages;
import com.strobel.core.StringUtilities;
public class OldAnnotationHarness {
    protected static DecompilerSettings defaultSettings() {
        DecompilerSettings settings = new DecompilerSettings();
        settings.setLanguage(Languages.java());
        settings.setTypeLoader(new InputTypeLoader());
        return settings;
    }
    protected void verifyOutput(Class<?> type, DecompilerSettings settings, String expected) {
        try {
            Path root = Paths.get(type.getProtectionDomain().getCodeSource().getLocation().toURI());
            PlainTextOutput output = new PlainTextOutput();
            output.setUnicodeOutputEnabled(settings.isUnicodeOutputEnabled());
            Decompiler.decompile(root.resolve(type.getName().replace('.', '/') + ".class").toString(), output, settings);
            List<String> lines = StringUtilities.split(output.toString(), true, '\n');
            int first = 0;
            while (first < lines.size() && (lines.get(first).startsWith("import ") || lines.get(first).startsWith("package "))) first++;
            if (first >= lines.size()) throw new AssertionError("no decompiled code");
            String actual = StringUtilities.join(" ", lines.subList(first, lines.size()));
            if (!expected.trim().replaceAll("\\s+", " ").equals(actual.replaceAll("\\s+", " ")))
                throw new AssertionError("upstream expected: " + expected + "\nactual: " + actual);
        } catch (Exception e) { throw new RuntimeException(e); }
    }
    public static void main(String[] args) {
        AnnotationTests tests = new AnnotationTests();
        tests.testAnnotationWithDefault();
        tests.testComplexAnnotation();
        tests.testAnnotationWithInnerClassAndConstants();
        System.out.println("OLD_ANNOTATION_TESTS_OK cases=3");
    }
}
