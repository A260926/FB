// Copyright 2000-2024 JetBrains s.r.o. and contributors. Use of this source code is governed by the Apache 2.0 license.

import org.jetbrains.java.decompiler.main.ClassesProcessor;
import org.jetbrains.java.decompiler.main.DecompilerContext;
import org.jetbrains.java.decompiler.main.IdentityRenamerFactory;
import org.jetbrains.java.decompiler.main.collectors.BytecodeMappingTracer;
import org.jetbrains.java.decompiler.main.collectors.ImportCollector;
import org.jetbrains.java.decompiler.main.extern.IBytecodeProvider;
import org.jetbrains.java.decompiler.main.extern.IFernflowerLogger;
import org.jetbrains.java.decompiler.main.extern.IFernflowerPreferences;
import org.jetbrains.java.decompiler.main.extern.IResultSaver;
import org.jetbrains.java.decompiler.modules.decompiler.typeann.TargetInfo;
import org.jetbrains.java.decompiler.modules.decompiler.typeann.TypeAnnotation;
import org.jetbrains.java.decompiler.struct.IDecompiledData;
import org.jetbrains.java.decompiler.struct.StructClass;
import org.jetbrains.java.decompiler.struct.StructContext;
import org.jetbrains.java.decompiler.struct.StructMethod;
import org.jetbrains.java.decompiler.struct.StructTypePathEntry;
import org.jetbrains.java.decompiler.struct.attr.StructGeneralAttribute;
import org.jetbrains.java.decompiler.struct.attr.StructTypeAnnotationAttribute;
import org.jetbrains.java.decompiler.struct.lazy.LazyLoader;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.jar.Manifest;

/**
 * Read-only inspection tool for the local variable type annotation contract tests.
 * Class files are parsed through the production Struct* API only; they are never loaded,
 * initialized or executed, and no annotation processor is invoked.
 *
 * Output lines (stable prefixes parsed by the C++ contract driver):
 *   CLASS  &lt;qualified name&gt;
 *   METHOD &lt;name&gt; &lt;descriptor&gt;
 *   MATTR  &lt;V|I&gt; &lt;target type&gt; &lt;rendered annotation&gt;
 *   CATTR  &lt;V|I&gt; &lt;target type&gt; &lt;rendered annotation&gt;
 *   LOCAL  &lt;V|I&gt; &lt;slot&gt; &lt;startPc&gt; &lt;length&gt; &lt;type path&gt; &lt;rendered annotation&gt;
 * A type path is "-" when empty, otherwise the dot-separated type_path_kind ids.
 * One LOCAL line is emitted per localvar target table entry, so an annotation covering
 * several ranges yields several lines with identical rendered text.
 */
public final class LocalAnnotationProbe {

  public static void main(String[] args) throws IOException {
    if (args.length != 2 || !"attrs".equals(args[0])) {
      System.err.println("usage: LocalAnnotationProbe attrs <class-file-or-directory>");
      System.exit(2);
    }
    File input = new File(args[1]);
    if (!input.exists()) {
      System.err.println("missing input: " + input);
      System.exit(2);
    }

    IBytecodeProvider provider = (externalPath, internalPath) -> Files.readAllBytes(new File(externalPath).toPath());
    IResultSaver saver = new NoopResultSaver();
    IDecompiledData decompiledData = new IDecompiledData() {
      @Override
      public String getClassEntryName(StructClass cl, String entryname) {
        return null;
      }

      @Override
      public String getClassContent(StructClass cl) {
        return null;
      }
    };

    StructContext structContext = new StructContext(saver, decompiledData, new LazyLoader(provider));
    ClassesProcessor classesProcessor = new ClassesProcessor(structContext);
    IFernflowerLogger logger = new IFernflowerLogger() {
      @Override
      public void writeMessage(String message, Severity severity) { }

      @Override
      public void writeMessage(String message, Severity severity, Throwable t) { }
    };
    DecompilerContext context = new DecompilerContext(
      new HashMap<String, Object>(IFernflowerPreferences.DEFAULTS),
      logger,
      structContext,
      classesProcessor,
      null,
      null,
      new IdentityRenamerFactory());
    DecompilerContext.setCurrentContext(context);

    if (input.isDirectory()) {
      try (var paths = Files.walk(input.toPath())) {
        paths.filter(path -> path.toString().endsWith(".class"))
          .map(path -> path.toFile())
          .sorted()
          .forEach(file -> structContext.addClassFile(file, true));
      }
    }
    else {
      structContext.addClassFile(input, true);
    }
    classesProcessor.loadClasses(null);

    for (StructClass structClass : structContext.getClasses().values()) {
      ClassesProcessor.ClassNode node = classesProcessor.getMapRootClasses().get(structClass.qualifiedName);
      if (node != null) {
        DecompilerContext.startClass(new ImportCollector(node));
      }
      System.out.println("CLASS " + structClass.qualifiedName);
      for (StructMethod method : structClass.getMethods()) {
        dumpMethod(method);
      }
    }
  }

  private static void dumpMethod(StructMethod method) {
    System.out.println("METHOD " + method.getName() + " " + method.getDescriptor());
    dumpMethodAttribute(method, true);
    dumpMethodAttribute(method, false);
    dumpCodeAttribute(method, true);
    dumpCodeAttribute(method, false);
  }

  private static void dumpMethodAttribute(StructMethod method, boolean runtimeVisible) {
    StructGeneralAttribute.Key<StructTypeAnnotationAttribute> key = runtimeVisible
      ? StructGeneralAttribute.ATTRIBUTE_RUNTIME_VISIBLE_TYPE_ANNOTATIONS
      : StructGeneralAttribute.ATTRIBUTE_RUNTIME_INVISIBLE_TYPE_ANNOTATIONS;
    StructTypeAnnotationAttribute attribute = method.getAttribute(key);
    if (attribute == null) {
      return;
    }
    for (TypeAnnotation annotation : attribute.getAnnotations()) {
      System.out.println("MATTR " + visibility(runtimeVisible) + " " + annotation.getTargetType() + " " + render(annotation));
    }
  }

  private static void dumpCodeAttribute(StructMethod method, boolean runtimeVisible) {
    List<TypeAnnotation> annotations = method.getCodeTypeAnnotations(runtimeVisible);
    for (TypeAnnotation annotation : annotations) {
      String rendered = render(annotation);
      if (annotation.getTargetType() == TypeAnnotation.LOCAL_VARIABLE
          && annotation.getTargetInfo() instanceof TargetInfo.LocalvarTarget localvarTarget) {
        for (TargetInfo.LocalvarTarget.Offsets offsets : localvarTarget.getTable()) {
          System.out.println("LOCAL " + visibility(runtimeVisible) + " " + offsets.getIndex() + " " +
                             offsets.getStartPc() + " " + offsets.getLength() + " " + renderPath(annotation) + " " + rendered);
        }
      }
      else {
        System.out.println("CATTR " + visibility(runtimeVisible) + " " + annotation.getTargetType() + " " + rendered);
      }
    }
  }

  private static String render(TypeAnnotation annotation) {
    return annotation.getAnnotationExpr().toJava(0, BytecodeMappingTracer.DUMMY).toString();
  }

  private static String renderPath(TypeAnnotation annotation) {
    List<StructTypePathEntry> paths = annotation.getPaths();
    if (paths.isEmpty()) {
      return "-";
    }
    StringBuilder builder = new StringBuilder();
    for (int i = 0; i < paths.size(); i++) {
      if (i > 0) {
        builder.append('.');
      }
      builder.append(paths.get(i).getTypePathEntryKind());
    }
    return builder.toString();
  }

  private static String visibility(boolean runtimeVisible) {
    return runtimeVisible ? "V" : "I";
  }

  private static final class NoopResultSaver implements IResultSaver {
    @Override
    public void saveFolder(String path) { }

    @Override
    public void copyFile(String source, String path, String entryName) { }

    @Override
    public void saveClassFile(String path, String qualifiedName, String entryName, String content, int[] mapping) { }

    @Override
    public void createArchive(String path, String archiveName, Manifest manifest) { }

    @Override
    public void saveDirEntry(String path, String archiveName, String entryName) { }

    @Override
    public void copyEntry(String source, String path, String archiveName, String entryName) { }

    @Override
    public void saveClassEntry(String path, String archiveName, String qualifiedName, String entryName, String content) { }

    @Override
    public void closeArchive(String path, String archiveName) { }
  }
}
