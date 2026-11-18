package task_support;

import java.nio.file.*;
import java.util.Iterator;
import java.util.List;
import org.jd.core.v1.api.loader.*;
import org.jd.core.v1.model.javasyntax.CompilationUnit;
import org.jd.core.v1.model.javasyntax.declaration.*;
import org.jd.core.v1.model.javasyntax.expression.*;
import org.jd.core.v1.model.javasyntax.statement.*;
import org.jd.core.v1.model.message.Message;
import org.jd.core.v1.service.converter.classfiletojavasyntax.ClassFileToJavaSyntaxProcessor;
import org.jd.core.v1.service.deserializer.classfile.DeserializeClassFileProcessor;

/**
 * Regression probe: runs the production deserializer + converter and prints
 * machine readable facts about switch expressions and a flattened outline of
 * every method so the Go tests can assert on the converted syntax tree rather
 * than on emitted source text.
 */
public final class SwitchAstProbe {
    public static void main(String[] args) throws Exception {
        final Path root = Paths.get(args[0]);
        Loader loader = new Loader() {
            public boolean canLoad(String name) { return Files.isRegularFile(root.resolve(name + ".class")); }
            public byte[] load(String name) throws LoaderException {
                try { return Files.readAllBytes(root.resolve(name + ".class")); }
                catch (java.io.IOException e) { throw new LoaderException(e.toString()); }
            }
        };
        Message message = new Message();
        message.setHeader("mainInternalTypeName", args[1]);
        message.setHeader("loader", loader);
        new DeserializeClassFileProcessor().process(message);
        new ClassFileToJavaSyntaxProcessor().process(message);
        CompilationUnit unit = message.getBody();
        unit.getTypeDeclarations().accept(new ProbeVisitor());
    }

    static String typeName(Expression expression) {
        if (expression == null) return "null";
        return expression.getType() == null ? "null" : expression.getType().toString();
    }

    static String labelName(SwitchExpression.Label label) {
        if (label instanceof SwitchExpression.DefaultLabel) {
            return "default";
        }
        SwitchExpression.ExpressionLabel expressionLabel = (SwitchExpression.ExpressionLabel)label;
        Expression e = expressionLabel.getExpression();
        if (e instanceof IntegerConstantExpression) {
            return String.valueOf(((IntegerConstantExpression)e).getIntegerValue());
        }
        return e.toString();
    }

    static class ProbeVisitor extends org.jd.core.v1.model.javasyntax.AbstractJavaSyntaxVisitor {
        int indent;

        void line(String text) {
            for (int i = 0; i < indent; i++) System.out.print("  ");
            System.out.println(text);
        }

        @Override
        public void visit(MethodDeclaration declaration) {
            line("METHOD " + declaration.getName());
            indent++;
            safeAccept(declaration.getStatements());
            indent--;
        }

        @Override
        public void visit(ConstructorDeclaration declaration) {
            line("CONSTRUCTOR");
            indent++;
            safeAccept(declaration.getStatements());
            indent--;
        }

        @Override
        public void visit(Statements list) {
            for (Statement statement : list) {
                statement.accept(this);
            }
        }

        @Override
        public void visit(SwitchStatement statement) {
            line("STMT_SWITCH");
            indent++;
            safeAccept(statement.getCondition());
            for (SwitchStatement.Block block : statement.getBlocks()) {
                block.accept(this);
            }
            indent--;
        }

        @Override
        public void visit(SwitchExpression expression) {
            line("SWITCH_EXPR type=" + typeName(expression) + " arms=" + expression.getBlocks().size());
            indent++;
            line("SELECTOR " + expression.getCondition().getClass().getSimpleName());
            for (SwitchExpression.Block block : expression.getBlocks()) {
                block.accept(this);
            }
            indent--;
        }

        @Override
        public void visit(SwitchExpression.LabelBlock expression) {
            emitArm(List.of(labelName(expression.getLabel())), expression.getStatements());
        }

        @Override
        public void visit(SwitchExpression.MultiLabelsBlock expression) {
            StringBuilder sb = new StringBuilder();
            Iterator<SwitchExpression.Label> iterator = expression.getLabels().iterator();
            while (iterator.hasNext()) {
                sb.append(labelName(iterator.next()));
                if (iterator.hasNext()) sb.append(',');
            }
            emitArm(List.of(sb.toString()), expression.getStatements());
        }

        void emitArm(List<String> labels, BaseStatement body) {
            StringBuilder labelText = new StringBuilder();
            for (int i = 0; i < labels.size(); i++) {
                if (i > 0) labelText.append(',');
                labelText.append(labels.get(i));
            }
            line("ARM label=" + labelText);
            indent++;
            line("BODY " + bodyKind(body));
            indent++;
            body.accept(this);
            indent--;
            indent--;
        }

        String bodyKind(BaseStatement body) {
            if (body.isExpressionStatement()) return "value";
            if (body.isThrowStatement()) return "throw";
            if (body.isIfStatement() || body.isIfElseStatement()) return "if";
            if (body.isStatements()) {
                for (Statement statement : (Statements)body) {
                    if (statement.isYieldStatement()) return "block-yield";
                }
                return "block";
            }
            return body.getClass().getSimpleName();
        }

        @Override
        public void visit(YieldStatement statement) {
            line("YIELD type=" + typeName(statement.getExpression()) + " expr=" + statement.getExpression().getClass().getSimpleName());
            indent++;
            statement.getExpression().accept(this);
            indent--;
        }

        @Override
        public void visit(ThrowStatement statement) {
            line("THROW expr=" + statement.getExpression().getClass().getSimpleName());
        }

        @Override
        public void visit(IfStatement statement) {
            line("IF");
            indent++;
            safeAccept(statement.getStatements());
            indent--;
        }

        @Override
        public void visit(IfElseStatement statement) {
            line("IF_ELSE");
            indent++;
            safeAccept(statement.getStatements());
            safeAccept(statement.getElseStatements());
            indent--;
        }

        @Override
        public void visit(ReturnExpressionStatement statement) {
            line("RETURN");
            indent++;
            statement.getExpression().accept(this);
            indent--;
        }

        @Override
        public void visit(ExpressionStatement statement) {
            line("EXPRESSION_STMT");
            indent++;
            statement.getExpression().accept(this);
            indent--;
        }

        @Override
        public void visit(LocalVariableDeclarationStatement statement) {
            line("LOCAL_VAR " + statement.getType());
        }

        @Override
        public void visit(BinaryOperatorExpression expression) {
            line("BINARY op=" + expression.getOperator() + " type=" + typeName(expression));
            indent++;
            expression.getLeftExpression().accept(this);
            expression.getRightExpression().accept(this);
            indent--;
        }

        @Override
        public void visit(MethodInvocationExpression expression) {
            line("METHOD_CALL name=" + expression.getName() + " type=" + typeName(expression));
            indent++;
            expression.getExpression().accept(this);
            safeAccept(expression.getParameters());
            indent--;
        }

        @Override
        public void visit(IntegerConstantExpression expression) {
            line("INT_CONST value=" + expression.getIntegerValue() + " type=" + typeName(expression));
        }

        @Override
        public void visit(BooleanExpression expression) {
            line("BOOL_CONST value=" + expression.isTrue() + " type=" + typeName(expression));
        }

        @Override
        public void visit(DoubleConstantExpression expression) {
            line("DOUBLE_CONST value=" + expression.getDoubleValue() + " type=" + typeName(expression));
        }

        @Override
        public void visit(LongConstantExpression expression) {
            line("LONG_CONST value=" + expression.getLongValue() + "L type=" + typeName(expression));
        }

        @Override
        public void visit(StringConstantExpression expression) {
            line("STRING_CONST value=" + expression.getStringValue() + " type=" + typeName(expression));
        }

        @Override
        public void visit(NullExpression expression) {
            line("NULL type=" + typeName(expression));
        }

        @Override
        public void visit(LocalVariableReferenceExpression expression) {
            line("VAR_REF name=" + expression.getName() + " type=" + typeName(expression));
        }

        @Override
        public void visit(ThisExpression expression) {
            line("THIS type=" + typeName(expression));
        }

        @Override
        public void visit(NewExpression expression) {
            line("NEW type=" + expression.getType());
            indent++;
            safeAccept(expression.getParameters());
            indent--;
        }
    }
}
