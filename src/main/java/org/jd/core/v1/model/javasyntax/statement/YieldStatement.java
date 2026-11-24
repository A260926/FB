/*
 * Copyright (c) 2024 Emmanuel Dupuy.
 * This project is distributed under the GPLv3 license.
 * This is a Copyleft license that gives you the right to use,
 * copy and modify the code freely for non-commercial purposes.
 */

package org.jd.core.v1.model.javasyntax.statement;

import org.jd.core.v1.model.javasyntax.expression.Expression;
import org.jd.core.v1.model.javasyntax.type.Type;

public class YieldStatement implements Statement {
    protected int lineNumber;
    protected Expression expression;
    protected Type type;

    public YieldStatement(Expression expression) {
        this(expression.getLineNumber(), expression, expression.getType());
    }

    public YieldStatement(int lineNumber, Expression expression) {
        this(lineNumber, expression, expression.getType());
    }

    public YieldStatement(int lineNumber, Expression expression, Type type) {
        this.lineNumber = lineNumber;
        this.expression = expression;
        this.type = type;
    }

    @Override
    public int getLineNumber() {
        return lineNumber;
    }

    public void setLineNumber(int lineNumber) {
        this.lineNumber = lineNumber;
    }

    @Override
    public Expression getExpression() {
        return expression;
    }

    public Type getType() {
        return type != null ? type : (expression == null ? null : expression.getType());
    }

    public void setType(Type type) {
        this.type = type;
    }

    public void setExpression(Expression expression) {
        this.expression = expression;
    }

    @Override
    public boolean isYieldStatement() {
        return true;
    }

    @Override
    public void accept(StatementVisitor visitor) {
        visitor.visit(this);
    }

    @Override
    public String toString() {
        return "YieldStatement{yield " + expression + "}";
    }
}
