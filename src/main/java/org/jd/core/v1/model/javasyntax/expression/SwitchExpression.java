/*
 * Copyright (c) 2024 Emmanuel Dupuy.
 * This project is distributed under the GPLv3 license.
 * This is a Copyleft license that gives you the right to use,
 * copy and modify the code freely for non-commercial purposes.
 */

package org.jd.core.v1.model.javasyntax.expression;

import org.jd.core.v1.model.javasyntax.statement.BaseStatement;
import org.jd.core.v1.model.javasyntax.statement.Statement;
import org.jd.core.v1.model.javasyntax.statement.SwitchStatement;
import org.jd.core.v1.model.javasyntax.type.Type;
import org.jd.core.v1.util.DefaultList;

public class SwitchExpression extends AbstractLineNumberTypeExpression {
    protected Expression condition;
    protected DefaultList<Arm> arms;

    public SwitchExpression(Type type, Expression condition, DefaultList<Arm> arms) {
        super(type);
        this.condition = condition;
        this.arms = arms;
    }

    public SwitchExpression(int lineNumber, Type type, Expression condition, DefaultList<Arm> arms) {
        super(lineNumber, type);
        this.condition = condition;
        this.arms = arms;
    }

    public Expression getCondition() {
        return condition;
    }

    public void setCondition(Expression condition) {
        this.condition = condition;
    }

    public DefaultList<Arm> getArms() {
        return arms;
    }

    public void setArms(DefaultList<Arm> arms) {
        this.arms = arms;
    }

    @Override
    public int getPriority() {
        // Switch arms contain block-like braces. Treat it as a low-precedence
        // expression so the fragment printer parenthesizes receiver/operand uses
        // that would otherwise be ambiguous to javac.
        return 17;
    }

    @Override
    public boolean isSwitchExpression() {
        return true;
    }

    @Override
    public void accept(ExpressionVisitor visitor) {
        visitor.visit(this);
    }

    @Override
    public String toString() {
        return "SwitchExpression{" + condition + ", arms=" + arms + "}";
    }

    public static class Arm {
        protected DefaultList<SwitchStatement.Label> labels;
        protected BaseStatement statements;

        public Arm(SwitchStatement.Label label, BaseStatement statements) {
            this.labels = new DefaultList<>();
            this.labels.add(label);
            this.statements = statements;
        }

        public Arm(DefaultList<SwitchStatement.Label> labels, BaseStatement statements) {
            this.labels = labels;
            this.statements = statements;
        }

        public DefaultList<SwitchStatement.Label> getLabels() {
            return labels;
        }

        public void setLabels(DefaultList<SwitchStatement.Label> labels) {
            this.labels = labels;
        }

        public BaseStatement getStatements() {
            return statements;
        }

        public void setStatements(BaseStatement statements) {
            this.statements = statements;
        }

        public boolean isDefault() {
            if (labels == null) {
                return false;
            }
            for (SwitchStatement.Label label : labels) {
                if (label == SwitchStatement.DEFAULT_LABEL) {
                    return true;
                }
            }
            return false;
        }

        @Override
        public String toString() {
            return "Arm{" + labels + " -> " + statements + '}';
        }
    }
}
