/*
 * Copyright (c) 2008-2019 Emmanuel Dupuy.
 * This project is distributed under the GPLv3 license.
 * This is a Copyleft license that gives the user the right to use,
 * copy and modify the code freely for non-commercial purposes.
 */

package org.jd.core.v1.model.javasyntax.expression;

import org.jd.core.v1.model.javasyntax.statement.BaseStatement;
import org.jd.core.v1.model.javasyntax.type.Type;
import org.jd.core.v1.util.DefaultList;

import java.util.List;

/**
 * Java 14+ switch expression ('switch (selector) { case ... -> ...; }').<br>
 *
 * The switch selector and each arm body are fully modeled expressions / statements.
 * Arms that only contain a throw statement contribute no result value; every
 * normal arm contributes exactly one value.
 */
public class SwitchExpression extends AbstractLineNumberTypeExpression {
    protected Expression condition;
    protected List<Block> blocks;

    public SwitchExpression(int lineNumber, Type type, Expression condition, List<Block> blocks) {
        super(lineNumber, type);
        this.condition = condition;
        this.blocks = blocks;
    }

    public Expression getCondition() {
        return condition;
    }

    public void setCondition(Expression condition) {
        this.condition = condition;
    }

    public List<Block> getBlocks() {
        return blocks;
    }

    @Override
    public int getPriority() {
        // Switch expressions need no parentheses when used as a sub-expression
        return 0;
    }

    @Override
    public boolean isSwitchExpression() { return true; }

    @Override
    public void accept(ExpressionVisitor visitor) {
        visitor.visit(this);
    }

    @Override
    public String toString() {
        return "SwitchExpression{switch (" + condition + ")}";
    }

    // --- Label --- //
    public interface Label {
        void accept(ExpressionVisitor visitor);
    }

    public static class DefaultLabel implements Label {
        public static final DefaultLabel DEFAULT_LABEL = new DefaultLabel();

        protected DefaultLabel() {}

        @Override
        public void accept(ExpressionVisitor visitor) {
            visitor.visit(this);
        }

        @Override
        public String toString() {
            return "SwitchExpressionDefaultLabel";
        }
    }

    public static class ExpressionLabel implements Label {
        protected Expression expression;

        public ExpressionLabel(Expression expression) {
            this.expression = expression;
        }

        public Expression getExpression() {
            return expression;
        }

        public void setExpression(Expression expression) {
            this.expression = expression;
        }

        @Override
        public void accept(ExpressionVisitor visitor) {
            visitor.visit(this);
        }

        @Override
        public String toString() {
            return "SwitchExpressionLabel{" + expression + "}";
        }
    }

    // --- Block --- //
    public static abstract class Block {
        protected BaseStatement statements;

        protected Block(BaseStatement statements) {
            this.statements = statements;
        }

        public BaseStatement getStatements() {
            return statements;
        }

        public abstract void accept(ExpressionVisitor visitor);
    }

    public static class LabelBlock extends Block {
        protected Label label;

        public LabelBlock(Label label, BaseStatement statements) {
            super(statements);
            this.label = label;
        }

        public Label getLabel() {
            return label;
        }

        @Override
        public void accept(ExpressionVisitor visitor) {
            visitor.visit(this);
        }

        @Override
        public String toString() {
            return "SwitchExpressionLabelBlock{label=" + label + "}";
        }
    }

    public static class MultiLabelsBlock extends Block {
        protected List<Label> labels;

        public MultiLabelsBlock(List<Label> labels, BaseStatement statements) {
            super(statements);
            this.labels = labels;
        }

        public MultiLabelsBlock(DefaultList<Label> labels, BaseStatement statements) {
            super(statements);
            this.labels = labels;
        }

        public List<Label> getLabels() {
            return labels;
        }

        @Override
        public void accept(ExpressionVisitor visitor) {
            visitor.visit(this);
        }

        @Override
        public String toString() {
            return "SwitchExpressionMultiLabelsBlock{labels=" + labels + "}";
        }
    }
}
