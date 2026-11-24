package task_support;

import java.nio.file.*;
import java.util.*;
import org.jd.core.v1.api.loader.*;
import org.jd.core.v1.model.javasyntax.CompilationUnit;
import org.jd.core.v1.model.javasyntax.AbstractJavaSyntaxVisitor;
import org.jd.core.v1.model.javasyntax.declaration.MethodDeclaration;
import org.jd.core.v1.model.javasyntax.expression.Expression;
import org.jd.core.v1.model.javasyntax.expression.SwitchExpression;
import org.jd.core.v1.model.javasyntax.statement.*;
import org.jd.core.v1.model.javasyntax.type.Type;
import org.jd.core.v1.model.message.Message;
import org.jd.core.v1.service.converter.classfiletojavasyntax.ClassFileToJavaSyntaxProcessor;
import org.jd.core.v1.service.deserializer.classfile.DeserializeClassFileProcessor;

public final class SwitchAstProbe {
    public static void main(String[] args) throws Exception {
        if (args.length != 3) throw new IllegalArgumentException("class directory, internal name and method name required");
        Path root = Paths.get(args[0]);
        Loader loader = new Loader() {
            public boolean canLoad(String name) { return Files.isRegularFile(root.resolve(name + ".class")); }
            public byte[] load(String name) throws LoaderException {
                try { return Files.readAllBytes(root.resolve(name + ".class")); }
                catch (java.io.IOException e) { throw new LoaderException(e.toString()); }
            }
        };

        Message message = new Message();
        message.setHeader("loader", loader);
        message.setHeader("mainInternalTypeName", args[1]);
        new DeserializeClassFileProcessor().process(message);
        new ClassFileToJavaSyntaxProcessor().process(message);

        CompilationUnit cu = message.getBody();
        MethodFinder finder = new MethodFinder(args[2]);
        finder.visit(cu);
        MethodDeclaration method = finder.found;
        if (method == null) throw new IllegalStateException("method not found: " + args[2]);

        ProbeVisitor visitor = new ProbeVisitor();
        if (method.getStatements() != null) method.getStatements().accept(visitor);
        System.out.println("METHOD name=" + method.getName() + " switches=" + visitor.switchCount
                + " statementSwitches=" + visitor.statementSwitchCount
                + " yields=" + visitor.yieldCount
                + " breaks=" + visitor.breakCount
                + " continues=" + visitor.continueCount);
        for (String line : visitor.lines) System.out.println(line);
    }

    private static final class MethodFinder extends AbstractJavaSyntaxVisitor {
        final String name;
        MethodDeclaration found;

        MethodFinder(String name) { this.name = name; }

        @Override
        public void visit(MethodDeclaration declaration) {
            if (name.equals(declaration.getName()) && found == null) found = declaration;
            super.visit(declaration);
        }
    }

    private static String typeName(Type type) {
        return type == null ? "<null>" : type.getDescriptor();
    }

    private static final class ProbeVisitor extends AbstractJavaSyntaxVisitor {
        final List<String> lines = new ArrayList<>();
        final Deque<Integer> switchIds = new ArrayDeque<>();
        int nextId;
        int switchCount;
        int statementSwitchCount;
        int yieldCount;
        int breakCount;
        int continueCount;

        @Override
        public void visit(SwitchExpression expression) {
            int id = nextId++;
            Integer parent = switchIds.peek();
            switchCount++;
            lines.add("SWITCH id=" + id + " parent=" + (parent == null ? -1 : parent)
                    + " type=" + typeName(expression.getType()) + " arms=" + expression.getArms().size());

            expression.getCondition().accept(this);

            int armIndex = 0;
            for (SwitchExpression.Arm arm : expression.getArms()) {
                StringBuilder labels = new StringBuilder();
                for (SwitchStatement.Label label : arm.getLabels()) {
                    if (labels.length() > 0) labels.append('|');
                    if (label == SwitchStatement.DEFAULT_LABEL) {
                        labels.append("default");
                    } else if (label instanceof SwitchStatement.ExpressionLabel) {
                        Expression value = ((SwitchStatement.ExpressionLabel) label).getExpression();
                        if (value == null) labels.append("?");
                        else if (value.isIntegerConstantExpression()) labels.append(value.getIntegerValue());
                        else if (value.isStringConstantExpression()) labels.append('"').append(value.getStringValue()).append('"');
                        else labels.append(value.getClass().getSimpleName());
                    } else {
                        labels.append(label.getClass().getSimpleName());
                    }
                }

                BaseStatement body = arm.getStatements();
                String bodyKind = "other";
                if (body instanceof YieldStatement) bodyKind = "yield";
                else if (body != null && body.isStatements() && body.size() == 1 && body.getFirst() instanceof YieldStatement) bodyKind = "yield";
                else if (body instanceof ThrowStatement) bodyKind = "throw";
                else if (body != null && body.isStatements() && body.size() == 1 && body.getFirst().isThrowStatement()) bodyKind = "throw";
                else if (body != null && body.isStatements()) bodyKind = "block";

                StringBuilder top = new StringBuilder();
                if (body != null && body.isStatements()) {
                    for (Statement statement : body) {
                        if (top.length() > 0) top.append(',');
                        top.append(statement.getClass().getSimpleName());
                    }
                } else if (body != null) {
                    top.append(body.getClass().getSimpleName());
                }

                lines.add("ARM switch=" + id + " index=" + armIndex++ + " labels=" + labels
                        + " kind=" + bodyKind + " top=" + top);

                switchIds.push(id);
                if (body != null) body.accept(this);
                switchIds.pop();
            }
        }

        @Override
        public void visit(YieldStatement statement) {
            yieldCount++;
            lines.add("YIELD switch=" + (switchIds.peek() == null ? -1 : switchIds.peek())
                    + " type=" + typeName(statement.getType())
                    + " expression=" + statement.getExpression().getClass().getSimpleName());
            super.visit(statement);
        }

        @Override
        public void visit(SwitchStatement statement) {
            statementSwitchCount++;
            lines.add("STATEMENT_SWITCH blocks=" + statement.getBlocks().size());
            super.visit(statement);
        }

        @Override
        public void visit(BreakStatement statement) {
            breakCount++;
            super.visit(statement);
        }

        @Override
        public void visit(ContinueStatement statement) {
            continueCount++;
            super.visit(statement);
        }
    }
}
