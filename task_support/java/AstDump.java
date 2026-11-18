package task_support;

import java.nio.file.*;
import org.jd.core.v1.api.loader.*;
import org.jd.core.v1.model.javasyntax.CompilationUnit;
import org.jd.core.v1.model.javasyntax.declaration.*;
import org.jd.core.v1.model.javasyntax.expression.*;
import org.jd.core.v1.model.javasyntax.statement.*;
import org.jd.core.v1.model.javasyntax.type.Type;
import org.jd.core.v1.model.message.Message;
import org.jd.core.v1.service.converter.classfiletojavasyntax.ClassFileToJavaSyntaxProcessor;
import org.jd.core.v1.service.deserializer.classfile.DeserializeClassFileProcessor;

/**
 * Debug probe: run the production deserializer + converter and print the
 * Java syntax tree of every method as an indented structural outline.
 */
public final class AstDump {
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
        unit.getTypeDeclarations().accept(new Printer());
    }

    static String typeName(Type type) {
        return type == null ? "null" : type.toString();
    }

    static class Printer extends org.jd.core.v1.model.javasyntax.AbstractJavaSyntaxVisitor {
        int indent = 0;

        void line(String text) {
            for (int i = 0; i < indent; i++) System.out.print("  ");
            System.out.println(text);
        }

        @Override
        public void visit(MethodDeclaration declaration) {
            line("METHOD " + declaration.getName() + " returns " + typeName(declaration.getReturnedType()));
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
        public void visit(ReturnExpressionStatement statement) {
            line("ReturnExpressionStatement");
            indent++;
            statement.getExpression().accept(this);
            indent--;
        }

        @Override
        public void visit(ReturnStatement statement) {
            line("ReturnStatement");
        }

        @Override
        public void visit(ExpressionStatement statement) {
            line("ExpressionStatement");
            indent++;
            statement.getExpression().accept(this);
            indent--;
        }

        @Override
        public void visit(ThrowStatement statement) {
            line("ThrowStatement");
            indent++;
            statement.getExpression().accept(this);
            indent--;
        }

        @Override
        public void visit(BreakStatement statement) {
            line("BreakStatement");
        }

        @Override
        public void visit(IfStatement statement) {
            line("IfStatement");
            indent++;
            line("condition:");
            indent++;
            statement.getCondition().accept(this);
            indent--;
            line("body:");
            indent++;
            safeAccept(statement.getStatements());
            indent--;
            indent--;
        }

        @Override
        public void visit(IfElseStatement statement) {
            line("IfElseStatement");
            indent++;
            line("condition:");
            indent++;
            statement.getCondition().accept(this);
            indent--;
            line("then:");
            indent++;
            safeAccept(statement.getStatements());
            indent--;
            line("else:");
            indent++;
            safeAccept(statement.getElseStatements());
            indent--;
            indent--;
        }

        @Override
        public void visit(SwitchStatement statement) {
            line("SwitchStatement");
            indent++;
            line("condition:");
            indent++;
            statement.getCondition().accept(this);
            indent--;
            for (SwitchStatement.Block block : statement.getBlocks()) {
                block.accept(this);
            }
            indent--;
        }

        @Override
        public void visit(SwitchStatement.LabelBlock statement) {
            line("LabelBlock " + statement.getLabel());
            indent++;
            statement.getStatements().accept(this);
            indent--;
        }

        @Override
        public void visit(SwitchStatement.MultiLabelsBlock statement) {
            line("MultiLabelsBlock " + statement.getLabels());
            indent++;
            statement.getStatements().accept(this);
            indent--;
        }

        @Override
        public void visit(LocalVariableDeclarationStatement statement) {
            line("LocalVariableDeclarationStatement " + statement.getType() + " " + statement.getLocalVariableDeclarators());
        }

        @Override
        public void visit(BinaryOperatorExpression expression) {
            line("BinaryOperatorExpression " + expression.getOperator() + " : " + typeName(expression.getType()));
            indent++;
            line("left:");
            indent++;
            expression.getLeftExpression().accept(this);
            indent--;
            line("right:");
            indent++;
            expression.getRightExpression().accept(this);
            indent--;
            indent--;
        }

        @Override
        public void visit(IntegerConstantExpression expression) {
            line("IntegerConstantExpression " + expression.getIntegerValue() + " : " + typeName(expression.getType()));
        }

        @Override
        public void visit(LongConstantExpression expression) {
            line("LongConstantExpression " + expression.getLongValue() + "L : " + typeName(expression.getType()));
        }

        @Override
        public void visit(NullExpression expression) {
            line("NullExpression : " + typeName(expression.getType()));
        }

        @Override
        public void visit(StringConstantExpression expression) {
            line("StringConstantExpression \"" + expression.getStringValue() + "\" : " + typeName(expression.getType()));
        }

        @Override
        public void visit(LocalVariableReferenceExpression expression) {
            line("LocalVariableReferenceExpression " + expression.getName() + " : " + typeName(expression.getType()));
        }

        @Override
        public void visit(MethodInvocationExpression expression) {
            line("MethodInvocationExpression " + expression.getName() + " : " + typeName(expression.getType()));
            indent++;
            line("receiver:");
            indent++;
            expression.getExpression().accept(this);
            indent--;
            line("parameters:");
            indent++;
            safeAccept(expression.getParameters());
            indent--;
            indent--;
        }

        @Override
        public void visit(NewExpression expression) {
            line("NewExpression " + expression.getType());
            indent++;
            safeAccept(expression.getParameters());
            indent--;
        }

        @Override
        public void visit(TernaryOperatorExpression expression) {
            line("TernaryOperatorExpression : " + typeName(expression.getType()));
            indent++;
            line("condition:");
            indent++;
            expression.getCondition().accept(this);
            indent--;
            line("true:");
            indent++;
            expression.getTrueExpression().accept(this);
            indent--;
            line("false:");
            indent++;
            expression.getFalseExpression().accept(this);
            indent--;
            indent--;
        }
    }
}
