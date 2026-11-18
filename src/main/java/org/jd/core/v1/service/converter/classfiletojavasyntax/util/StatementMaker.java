/*
 * Copyright (c) 2008, 2019 Emmanuel Dupuy.
 * This project is distributed under the GPLv3 license.
 * This is a Copyleft license that gives the user the right to use,
 * copy and modify the code freely for non-commercial purposes.
 */

package org.jd.core.v1.service.converter.classfiletojavasyntax.util;

import org.jd.core.v1.model.classfile.ClassFile;
import org.jd.core.v1.model.classfile.Constants;
import org.jd.core.v1.model.classfile.attribute.AttributeCode;
import org.jd.core.v1.model.javasyntax.AbstractJavaSyntaxVisitor;
import org.jd.core.v1.model.javasyntax.declaration.FieldDeclarator;
import org.jd.core.v1.model.javasyntax.declaration.MethodDeclaration;
import org.jd.core.v1.model.javasyntax.expression.*;
import org.jd.core.v1.model.javasyntax.statement.*;
import org.jd.core.v1.model.javasyntax.type.BaseType;
import org.jd.core.v1.model.javasyntax.type.ObjectType;
import org.jd.core.v1.model.javasyntax.type.PrimitiveType;
import org.jd.core.v1.model.javasyntax.type.Type;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.cfg.BasicBlock;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.cfg.ControlFlowGraph;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.javasyntax.declaration.ClassFileBodyDeclaration;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.javasyntax.declaration.ClassFileConstructorOrMethodDeclaration;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.javasyntax.declaration.ClassFileFieldDeclaration;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.javasyntax.expression.ClassFileLocalVariableReferenceExpression;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.javasyntax.statement.ClassFileBreakContinueStatement;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.javasyntax.statement.ClassFileTryStatement;
import org.jd.core.v1.service.converter.classfiletojavasyntax.model.localvariable.AbstractLocalVariable;
import org.jd.core.v1.service.converter.classfiletojavasyntax.visitor.*;
import org.jd.core.v1.util.DefaultList;
import org.jd.core.v1.util.DefaultStack;

import java.util.*;

import static org.jd.core.v1.model.javasyntax.expression.Expression.UNKNOWN_LINE_NUMBER;
import static org.jd.core.v1.model.javasyntax.expression.NoExpression.NO_EXPRESSION;
import static org.jd.core.v1.model.javasyntax.type.ObjectType.TYPE_UNDEFINED_OBJECT;
import static org.jd.core.v1.model.javasyntax.type.PrimitiveType.*;
import static org.jd.core.v1.service.converter.classfiletojavasyntax.model.cfg.BasicBlock.*;

public class StatementMaker {
    protected static final SwitchCaseComparator SWITCH_CASE_COMPARATOR = new SwitchCaseComparator();
    protected static final NullExpression FINALLY_EXCEPTION_EXPRESSION = new NullExpression(new ObjectType("java/lang/Exception", "java.lang.Exception", "Exception"));
    protected static final MergeTryWithResourcesStatementVisitor MERGE_TRY_WITH_RESOURCES_STATEMENT_VISITOR = new MergeTryWithResourcesStatementVisitor();

    protected TypeMaker typeMaker;
    protected Map<String, BaseType> typeBounds;
    protected LocalVariableMaker localVariableMaker;
    protected ByteCodeParser byteCodeParser;
    protected int majorVersion;
    protected String internalTypeName;
    protected ClassFileBodyDeclaration bodyDeclaration;
    protected DefaultStack<Expression> stack = new DefaultStack<>();
    protected RemoveFinallyStatementsVisitor removeFinallyStatementsVisitor;
    protected RemoveBinaryOpReturnStatementsVisitor removeBinaryOpReturnStatementsVisitor;
    protected UpdateIntegerConstantTypeVisitor updateIntegerConstantTypeVisitor;
    protected SearchFirstLineNumberVisitor searchFirstLineNumberVisitor = new SearchFirstLineNumberVisitor();
    protected MemberVisitor memberVisitor = new MemberVisitor();
    protected boolean removeFinallyStatementsFlag = false;
    protected boolean mergeTryWithResourcesStatementFlag = false;
    // > 0 while the statements of a switch expression arm are being built:
    // the reducer's SWITCH_BREAK marker then means "arm end", not a break.
    protected int switchExpressionArmDepth = 0;

    protected static final int ARM_INVALID = 0;
    protected static final int ARM_THROW = 1;
    protected static final int ARM_VALUE = 2;

    public StatementMaker(TypeMaker typeMaker, LocalVariableMaker localVariableMaker, ClassFileConstructorOrMethodDeclaration comd) {
        ClassFile classFile = comd.getClassFile();

        this.typeMaker = typeMaker;
        this.typeBounds = comd.getTypeBounds();
        this.localVariableMaker = localVariableMaker;
        this.majorVersion = classFile.getMajorVersion();
        this.internalTypeName = classFile.getInternalTypeName();
        this.bodyDeclaration = comd.getBodyDeclaration();
        this.byteCodeParser = new ByteCodeParser(typeMaker, localVariableMaker, classFile, this.bodyDeclaration, comd);
        this.removeFinallyStatementsVisitor = new RemoveFinallyStatementsVisitor(localVariableMaker);
        this.removeBinaryOpReturnStatementsVisitor = new RemoveBinaryOpReturnStatementsVisitor(localVariableMaker);
        this.updateIntegerConstantTypeVisitor = new UpdateIntegerConstantTypeVisitor(comd.getReturnedType());
    }

    public Statements make(ControlFlowGraph cfg) {
        Statements statements = new Statements();
        Statements jumps = new Statements();
        WatchDog watchdog = new WatchDog();

        localVariableMaker.pushFrame(statements);

        // Generate statements
        makeStatements(watchdog, cfg.getStart(), statements, jumps);

        // Remove 'finally' statements
        if (removeFinallyStatementsFlag) {
            removeFinallyStatementsVisitor.init();
            statements.accept(removeFinallyStatementsVisitor);
        }

        // Merge 'try-with-resources' statements
        if (mergeTryWithResourcesStatementFlag) {
            statements.accept(MERGE_TRY_WITH_RESOURCES_STATEMENT_VISITOR);
        }

        // Replace pattern "synthetic_local_var = ...; return synthetic_local_var;" with "return ...;"
        statements.accept(removeBinaryOpReturnStatementsVisitor);

        // Remove last 'return' statement
        if (!statements.isEmpty() && statements.getLast().isReturnStatement()) {
            statements.removeLast();
        }

        localVariableMaker.popFrame();

        // Update integer constant type to 'byte', 'char', 'short' or 'int'
        statements.accept(updateIntegerConstantTypeVisitor);

        // Change ++i; with i++;
        replacePreOperatorWithPostOperator(statements);

        if (!jumps.isEmpty()) {
            updateJumpStatements(jumps);
        }

        return statements;
    }

    /**
     * A recursive, next neighbour first, statements builder from basic blocks.
     *
     * @param basicBlock Current basic block
     * @param statements List to populate
     */
    @SuppressWarnings("unchecked")
    protected void makeStatements(WatchDog watchdog, BasicBlock basicBlock, Statements statements, Statements jumps) {
        Statements subStatements, elseStatements;
        Expression condition, exp1, exp2;

        switch (basicBlock.getType()) {
            case TYPE_START:
                watchdog.check(basicBlock, basicBlock.getNext());
                makeStatements(watchdog, basicBlock.getNext(), statements, jumps);
                break;
            case TYPE_END:
                break;
            case TYPE_STATEMENTS:
                watchdog.check(basicBlock, basicBlock.getNext());
            case TYPE_THROW:
                parseByteCode(basicBlock, statements);
                makeStatements(watchdog, basicBlock.getNext(), statements, jumps);
                break;
            case TYPE_RETURN:
                statements.add(ReturnStatement.RETURN);
                break;
            case TYPE_RETURN_VALUE:
            case TYPE_GOTO_IN_TERNARY_OPERATOR:
                parseByteCode(basicBlock, statements);
                break;
            case TYPE_SWITCH:
                parseSwitch(watchdog, basicBlock, statements, jumps);
                break;
            case TYPE_SWITCH_BREAK:
                // Inside a switch expression arm this marker only closes the arm.
                if (switchExpressionArmDepth == 0) {
                    statements.add(BreakStatement.BREAK);
                }
                break;
            case TYPE_TRY:
                parseTry(watchdog, basicBlock, statements, jumps, false, false);
                break;
            case TYPE_TRY_JSR:
                parseTry(watchdog, basicBlock, statements, jumps, true, false);
                break;
            case TYPE_TRY_ECLIPSE:
                parseTry(watchdog, basicBlock, statements, jumps, false, true);
                break;
            case TYPE_JSR:
                parseJSR(watchdog, basicBlock, statements, jumps);
                break;
            case TYPE_RET:
                parseByteCode(basicBlock, statements);
                break;
            case TYPE_IF:
                parseIf(watchdog, basicBlock, statements, jumps);
                break;
            case TYPE_IF_ELSE:
                watchdog.check(basicBlock, basicBlock.getCondition());
                makeStatements(watchdog, basicBlock.getCondition(), statements, jumps);
                condition = stack.pop();
                DefaultStack<Expression> backup = new DefaultStack<>(stack);
                watchdog.check(basicBlock, basicBlock.getSub1());
                subStatements = makeSubStatements(watchdog, basicBlock.getSub1(), statements, jumps);
                if (!basicBlock.getSub2().matchType(TYPE_LOOP_END|TYPE_LOOP_CONTINUE|TYPE_LOOP_START) && (stack.size() != backup.size())) {
                    stack.copy(backup);
                }
                watchdog.check(basicBlock, basicBlock.getSub2());
                elseStatements = makeSubStatements(watchdog, basicBlock.getSub2(), statements, jumps);
                statements.add(new IfElseStatement(condition, subStatements, elseStatements));
                watchdog.check(basicBlock, basicBlock.getNext());
                makeStatements(watchdog, basicBlock.getNext(), statements, jumps);
                break;
            case TYPE_CONDITION:
                if (basicBlock.getSub1() != END) {
                    stack.push(makeExpression(watchdog, basicBlock.getSub1(), statements, jumps));
                }
                if (basicBlock.getSub2() != END) {
                    stack.push(makeExpression(watchdog, basicBlock.getSub2(), statements, jumps));
                }
                parseByteCode(basicBlock, statements);
                break;
            case TYPE_CONDITION_OR:
                watchdog.check(basicBlock, basicBlock.getSub1());
                exp1 = makeExpression(watchdog, basicBlock.getSub1(), statements, jumps);
                watchdog.check(basicBlock, basicBlock.getSub2());
                exp2 = makeExpression(watchdog, basicBlock.getSub2(), statements, jumps);
                stack.push(new BinaryOperatorExpression(basicBlock.getFirstLineNumber(), PrimitiveType.TYPE_BOOLEAN, exp1, "||", exp2, 14));
                break;
            case TYPE_CONDITION_AND:
                watchdog.check(basicBlock, basicBlock.getSub1());
                exp1 = makeExpression(watchdog, basicBlock.getSub1(), statements, jumps);
                watchdog.check(basicBlock, basicBlock.getSub2());
                exp2 = makeExpression(watchdog, basicBlock.getSub2(), statements, jumps);
                stack.push(new BinaryOperatorExpression(basicBlock.getFirstLineNumber(), PrimitiveType.TYPE_BOOLEAN, exp1, "&&", exp2, 13));
                break;
            case TYPE_CONDITION_TERNARY_OPERATOR:
                watchdog.check(basicBlock, basicBlock.getCondition());
                makeStatements(watchdog, basicBlock.getCondition(), statements, jumps);
                condition = stack.pop();
                backup = new DefaultStack<>(stack);
                watchdog.check(basicBlock, basicBlock.getSub1());
                exp1 = makeExpression(watchdog, basicBlock.getSub1(), statements, jumps);
                if (stack.size() != backup.size()) {
                    stack.copy(backup);
                }
                watchdog.check(basicBlock, basicBlock.getSub2());
                exp2 = makeExpression(watchdog, basicBlock.getSub2(), statements, jumps);
                stack.push(parseTernaryOperator(basicBlock.getFirstLineNumber(), condition, exp1, exp2));
                parseByteCode(basicBlock, statements);
                break;
            case TYPE_TERNARY_OPERATOR:
                watchdog.check(basicBlock, basicBlock.getCondition());
                makeStatements(watchdog, basicBlock.getCondition(), statements, jumps);
                condition = stack.pop();
                backup = new DefaultStack<>(stack);
                watchdog.check(basicBlock, basicBlock.getSub1());
                exp1 = makeExpression(watchdog, basicBlock.getSub1(), statements, jumps);
                if (stack.size() != backup.size()) {
                    stack.copy(backup);
                }
                watchdog.check(basicBlock, basicBlock.getSub2());
                exp2 = makeExpression(watchdog, basicBlock.getSub2(), statements, jumps);
                stack.push(parseTernaryOperator(basicBlock.getFirstLineNumber(), condition, exp1, exp2));
                watchdog.check(basicBlock, basicBlock.getNext());
                makeStatements(watchdog, basicBlock.getNext(), statements, jumps);
                break;
            case TYPE_LOOP:
                parseLoop(watchdog, basicBlock, statements, jumps);
                break;
            case TYPE_LOOP_START:
            case TYPE_LOOP_CONTINUE:
                statements.add(ContinueStatement.CONTINUE);
                break;
            case TYPE_LOOP_END:
                statements.add(BreakStatement.BREAK);
                break;
            case TYPE_JUMP:
                Statement jump = new ClassFileBreakContinueStatement(basicBlock.getFromOffset(), basicBlock.getToOffset());
                statements.add(jump);
                jumps.add(jump);
                break;
            case TYPE_INFINITE_GOTO:
                statements.add(new WhileStatement(BooleanExpression.TRUE, null));
                break;
            default:
                assert false : "Unexpected basic block: " + basicBlock.getTypeName() + ':' + basicBlock.getIndex();
                break;
        }
    }

    protected Statements makeSubStatements(WatchDog watchdog, BasicBlock basicBlock, Statements statements, Statements jumps, BasicBlock updateBasicBlock) {
        Statements subStatements = makeSubStatements(watchdog, basicBlock, statements, jumps);

        if (updateBasicBlock != null) {
            subStatements.addAll(makeSubStatements(watchdog, updateBasicBlock, statements, jumps));
        }

        return subStatements;
    }

    protected Statements makeSubStatements(WatchDog watchdog, BasicBlock basicBlock, Statements statements, Statements jumps) {
        Statements subStatements = new Statements();

        if (!statements.isEmpty() && statements.getLast().isMonitorEnterStatement()) {
            subStatements.add(statements.removeLast());
        }

        localVariableMaker.pushFrame(subStatements);
        makeStatements(watchdog, basicBlock, subStatements, jumps);
        localVariableMaker.popFrame();
        replacePreOperatorWithPostOperator(subStatements);

        if (!subStatements.isEmpty() && subStatements.getFirst().isMonitorEnterStatement()) {
            statements.add(subStatements.removeFirst());
        }

        return subStatements;
    }

    protected Expression makeExpression(WatchDog watchdog, BasicBlock basicBlock, Statements statements, Statements jumps) {
        int initialStatementCount = statements.size();

        makeStatements(watchdog, basicBlock, statements, jumps);

        if (stack.isEmpty()) {
            // Interesting... Kotlin pattern.
            // https://github.com/JetBrains/intellij-community/blob/master/platform/built-in-server/src/org/jetbrains/builtInWebServer/SingleConnectionNetService.kt
            // final override fun connectToProcess(...)
            return new StringConstantExpression("JD-Core does not support Kotlin");
        } else {
            Expression expression = stack.pop();

            if (statements.size() > initialStatementCount) {
                // Is a multi-assignment ?
                Expression boe = statements.getLast().getExpression();

                if (boe != NO_EXPRESSION) {
                    if (boe.getRightExpression() == expression) {
                        // Pattern matched -> Multi-assignment
                        statements.removeLast();
                        expression = boe;
                    } else if (expression.isNewArray()) {
                        expression = NewArrayMaker.make(statements, expression);
                    }
                }
            }

            return expression;
        }
    }

    protected void parseSwitch(WatchDog watchdog, BasicBlock basicBlock, Statements statements, Statements jumps) {
        parseByteCode(basicBlock, statements);

        List<SwitchCase> switchCases = basicBlock.getSwitchCases();
        SwitchStatement switchStatement = (SwitchStatement)statements.getLast();
        Expression condition = switchStatement.getCondition();
        Type conditionType = condition.getType();
        DefaultStack<Expression> localStack = new DefaultStack<Expression>(stack);

        switchCases.sort(SWITCH_CASE_COMPARATOR);

        if (isSwitchExpressionPattern(basicBlock, switchCases)) {
            // Java 14+ switch expression: every arm leaves one value on the
            // stack or throws; values are merged on basicBlock.getNext().
            statements.removeLast();

            BasicBlock merge = basicBlock.getNext();
            SwitchCase recoveredDefault = recoverDefaultSwitchCase(basicBlock, switchCases);
            // A recovered default arm may target the merge block itself
            // (typical for nested switch expressions): its byte code is then
            // consumed by the arm and must not be traversed a second time.
            boolean mergeConsumedAsArm = (recoveredDefault != null) && (recoveredDefault.getBasicBlock() == merge);

            SwitchExpression switchExpression = makeSwitchExpression(
                    watchdog, basicBlock, switchCases, condition, conditionType, localStack, jumps, recoveredDefault);

            stack.copy(localStack);
            stack.push(switchExpression);

            if (!mergeConsumedAsArm && (merge != BasicBlock.SWITCH_BREAK)) {
                makeStatements(watchdog, merge, statements, jumps);
            }
            return;
        }

        List<SwitchStatement.Block> blocks = switchStatement.getBlocks();

        for (int i=0, len=switchCases.size(); i<len; i++) {
            SwitchCase sc = switchCases.get(i);
            BasicBlock bb = sc.getBasicBlock();
            int j = i + 1;

            while ((j < len) && (bb == switchCases.get(j).getBasicBlock())) {
                j++;
            }

            Statements subStatements = new Statements();

            stack.copy(localStack);
            makeStatements(watchdog, bb, subStatements, jumps);
            replacePreOperatorWithPostOperator(subStatements);

            if (sc.isDefaultCase()) {
                blocks.add(new SwitchStatement.LabelBlock(SwitchStatement.DEFAULT_LABEL, subStatements));
            } else if (j == i + 1) {
                SwitchStatement.Label label = new SwitchStatement.ExpressionLabel(new IntegerConstantExpression(conditionType, sc.getValue()));
                blocks.add(new SwitchStatement.LabelBlock(label, subStatements));
            } else {
                DefaultList<SwitchStatement.Label> labels = new DefaultList<>(j - i);

                for (; i<j; i++) {
                    labels.add(new SwitchStatement.ExpressionLabel(new IntegerConstantExpression(conditionType, switchCases.get(i).getValue())));
                }

                blocks.add(new SwitchStatement.MultiLabelsBlock(labels, subStatements));
                i--;
            }
        }

        int size = statements.size();

        if ((size > 3) && condition.isLocalVariableReferenceExpression() && statements.get(size-2).isSwitchStatement()) {
            // Check pattern & make 'switch-string'
            SwitchStatementMaker.makeSwitchString(localVariableMaker, statements, switchStatement);
        } else if (condition.isArrayExpression()) {
            // Check pattern & make 'switch-enum'
            SwitchStatementMaker.makeSwitchEnum(bodyDeclaration, switchStatement);
        }

        makeStatements(watchdog, basicBlock.getNext(), statements, jumps);
    }

    /**
     * A switch is a switch expression when every case target is either a block
     * sequence that produces exactly one stacked value and jumps to the switch
     * merge point, or a block that throws, and the merge point consumes the
     * produced value.
     */
    protected boolean isSwitchExpressionPattern(BasicBlock basicBlock, List<SwitchCase> switchCases) {
        BasicBlock merge = basicBlock.getNext();

        if ((merge == BasicBlock.END) || (merge == BasicBlock.SWITCH_BREAK) || switchCases.isEmpty()) {
            return false;
        }

        boolean valueArmFound = false;

        // Each arm gets a fresh visited set: distinct labels may share the same
        // target block and must be classified independently.
        for (SwitchCase switchCase : switchCases) {
            int kind = classifySwitchArmBlock(switchCase.getBasicBlock(), merge, new HashSet<BasicBlock>());

            if (kind == ARM_INVALID) {
                return false;
            }

            if (kind == ARM_VALUE) {
                valueArmFound = true;
            }
        }

        return valueArmFound;
    }

    /**
     * Classify a switch arm target block. The SWITCH_BREAK marker produced by
     * the reducer represents the switch merge point inside a switch expression.
     */
    protected int classifySwitchArmBlock(BasicBlock bb, BasicBlock merge, HashSet<BasicBlock> visited) {
        if (bb == BasicBlock.END) {
            return ARM_INVALID;
        }

        if (bb == BasicBlock.SWITCH_BREAK) {
            // Reaching the merge point without producing a value is a statement-style break.
            return ARM_INVALID;
        }

        // A throwing arm is valid even though TYPE_THROW belongs to GROUP_END.
        if (bb.getType() == TYPE_THROW) {
            return ARM_THROW;
        }

        if (bb.matchType(BasicBlock.GROUP_END)) {
            return ARM_INVALID;
        }

        if (!visited.add(bb)) {
            return ARM_INVALID;
        }

        switch (bb.getType()) {
            case TYPE_GOTO_IN_TERNARY_OPERATOR:
                // Block carries the arm value, the goto jumps to the merge point.
                return (ByteCodeUtil.evalStackDepth(bb) == 1) ? ARM_VALUE : ARM_INVALID;
            case TYPE_STATEMENTS:
                if (bb.getNext() == BasicBlock.SWITCH_BREAK) {
                    return (ByteCodeUtil.evalStackDepth(bb) == 1) ? ARM_VALUE : ARM_INVALID;
                } else if (bb.getNext() == merge) {
                    return (ByteCodeUtil.evalStackDepth(bb) == 1) ? ARM_VALUE : ARM_INVALID;
                } else if (bb.getNext().getType() == TYPE_GOTO_IN_TERNARY_OPERATOR
                        && (ByteCodeUtil.evalStackDepth(bb.getNext()) == 1)) {
                    // Statement-only branch (e.g. an assignment) whose resulting
                    // value is read by a following goto merge block.
                    return ARM_VALUE;
                } else {
                    return classifySwitchArmBlock(bb.getNext(), merge, visited);
                }
            case TYPE_IF: {
                int sub1 = classifySwitchArmBlock(bb.getSub1(), merge, visited);
                int sub2 = classifySwitchArmBlock(bb.getNext(), merge, visited);

                if ((sub1 == ARM_INVALID) || (sub2 == ARM_INVALID)) {
                    return ARM_INVALID;
                }

                return (sub1 == ARM_VALUE || sub2 == ARM_VALUE) ? ARM_VALUE : ARM_THROW;
            }
            case TYPE_IF_ELSE: {
                // 'if/else' whose branches are statements (e.g. assignments) and
                // whose value is carried by a following goto block: valid when the
                // branch blocks flow into a value-carrying merge block.
                int sub1 = classifySwitchArmBlock(bb.getSub1(), merge, visited);
                int sub2 = classifySwitchArmBlock(bb.getSub2(), merge, visited);

                if ((sub1 != ARM_INVALID) && (sub2 != ARM_INVALID)) {
                    return (sub1 == ARM_VALUE || sub2 == ARM_VALUE) ? ARM_VALUE : ARM_THROW;
                }

                BasicBlock join = bb.getNext();

                if ((join != BasicBlock.END) && (join.getType() == TYPE_GOTO_IN_TERNARY_OPERATOR)
                        && (ByteCodeUtil.evalStackDepth(join) == 1)) {
                    return ARM_VALUE;
                }

                return ARM_INVALID;
            }
            case TYPE_SWITCH: {
                // Nested switch expression: validate it against its own merge block.
                BasicBlock nestedMerge = bb.getNext();

                if (nestedMerge == BasicBlock.END) {
                    return ARM_INVALID;
                }

                boolean nestedValueArmFound = false;

                for (SwitchCase nestedCase : bb.getSwitchCases()) {
                    int nestedKind = classifySwitchArmBlock(nestedCase.getBasicBlock(), nestedMerge, new HashSet<BasicBlock>());

                    if (nestedKind == ARM_INVALID) {
                        return ARM_INVALID;
                    }

                    if (nestedKind == ARM_VALUE) {
                        nestedValueArmFound = true;
                    }
                }

                return nestedValueArmFound ? ARM_VALUE : ARM_INVALID;
            }
            default:
                return ARM_INVALID;
        }
    }

    /**
     * Build the switch expression model, one arm per distinct case target.
     * Case labels sharing a target are grouped into a MultiLabelsBlock.
     */
    protected SwitchExpression makeSwitchExpression(WatchDog watchdog, BasicBlock basicBlock, List<SwitchCase> switchCases,
                                                    Expression condition, Type conditionType,
                                                    DefaultStack<Expression> localStack,
                                                    Statements jumps, SwitchCase recoveredDefault) {
        List<SwitchExpression.Block> blocks = new DefaultList<>();
        DefaultList<Expression> armValues = new DefaultList<>();

        for (int i=0, len=switchCases.size(); i<len; i++) {
            SwitchCase sc = switchCases.get(i);
            BasicBlock bb = sc.getBasicBlock();
            int j = i + 1;

            while ((j < len) && (bb == switchCases.get(j).getBasicBlock())) {
                j++;
            }

            ArmResult result = buildSwitchArm(watchdog, bb, localStack, jumps);
            armValues.addAll(result.values);
            blocks.add(makeSwitchExpressionBlock(conditionType, result, sc, switchCases, i, j));
            i = j - 1;
        }

        if (recoveredDefault != null) {
            ArmResult result = buildSwitchArm(watchdog, recoveredDefault.getBasicBlock(), localStack, jumps);
            armValues.addAll(result.values);
            blocks.add(makeSwitchExpressionBlock(conditionType, result, recoveredDefault, switchCases, -1, -1));
        }

        Type type = null;

        for (Expression armValue : armValues) {
            if (type == null) {
                type = armValue.getType();
            } else if (!armValue.isNullExpression()) {
                type = combineExpressionTypes(new NullExpression(type), armValue);
            }
        }

        if (type == null) {
            type = TYPE_UNDEFINED_OBJECT;
        }

        // A 'null' arm must carry the switch result type, not the undefined
        // object type produced by aconst_null.
        for (Expression armValue : armValues) {
            if (armValue.isNullExpression() && TYPE_UNDEFINED_OBJECT.equals(armValue.getType())) {
                ((NullExpression)armValue).setType(type);
            }
        }

        return new SwitchExpression(basicBlock.getFirstLineNumber(), type, condition, blocks);
    }

    protected SwitchExpression.Block makeSwitchExpressionBlock(Type conditionType, ArmResult result,
                                                                SwitchCase sc, List<SwitchCase> switchCases, int i, int j) {
        BaseStatement armBody = buildSwitchArmBody(result);

        if (sc.isDefaultCase()) {
            return new SwitchExpression.LabelBlock(SwitchExpression.DefaultLabel.DEFAULT_LABEL, armBody);
        }

        if ((i >= 0) && (j - i > 1)) {
            DefaultList<SwitchExpression.Label> labels = new DefaultList<>(j - i);

            for (int k=i; k<j; k++) {
                labels.add(new SwitchExpression.ExpressionLabel(
                        new IntegerConstantExpression(conditionType, switchCases.get(k).getValue())));
            }

            return new SwitchExpression.MultiLabelsBlock(labels, armBody);
        }

        SwitchExpression.Label label = new SwitchExpression.ExpressionLabel(
                new IntegerConstantExpression(conditionType, sc.getValue()));

        return new SwitchExpression.LabelBlock(label, armBody);
    }

    /**
     * Result of building one switch arm: its statements, whether it produces a
     * value or throws, the single produced value (null for 'if' arms and throw
     * arms) and every value contributed to the switch result (nested yields).
     */
    protected static class ArmResult {
        protected Statements statements = new Statements();
        protected int kind = ARM_INVALID;
        protected Expression value;
        protected DefaultList<Expression> values = new DefaultList<>();
    }

    /**
     * Build the statements of one arm. On entry and exit the shared stack
     * contains exactly the values stacked before the switch.
     */
    protected ArmResult buildSwitchArm(WatchDog watchdog, BasicBlock bb, DefaultStack<Expression> localStack, Statements jumps) {
        ArmResult result = new ArmResult();

        stack.copy(localStack);
        localVariableMaker.pushFrame(result.statements);
        switchExpressionArmDepth++;

        try {
            switch (bb.getType()) {
                case TYPE_IF:
                case TYPE_IF_ELSE:
                    buildSwitchArmIf(watchdog, bb, localStack, result, jumps);
                    break;
                case TYPE_SWITCH:
                    // Nested switch expression: its result is the arm value.
                    parseSwitch(watchdog, bb, result.statements, jumps);
                    result.kind = ARM_VALUE;
                    result.value = stack.pop();
                    result.values.add(result.value);
                    break;
                case TYPE_THROW:
                    makeStatements(watchdog, bb, result.statements, jumps);
                    result.kind = ARM_THROW;
                    break;
                default:
                    makeStatements(watchdog, bb, result.statements, jumps);

                    if (stack.size() > localStack.size()) {
                        result.kind = ARM_VALUE;
                        result.value = stack.pop();
                        result.values.add(result.value);
                    } else {
                        result.kind = ARM_THROW;
                    }
                    break;
            }
        } finally {
            switchExpressionArmDepth--;
            localVariableMaker.popFrame();
        }

        assert stack.size() == localStack.size() : "Switch arm must not alter the shared stack";

        return result;
    }

    /**
     * Build an arm containing an 'if' (or 'if/else') whose normal exits all
     * produce the value of the enclosing switch expression. The exits belong to
     * the same switch, so their values become 'yield' statements rather than
     * being merged on the stack. A throwing exit keeps its throw statement.
     */
    protected void buildSwitchArmIf(WatchDog watchdog, BasicBlock ifBlock, DefaultStack<Expression> localStack,
                                    ArmResult result, Statements jumps) {
        BasicBlock conditionBlock = ifBlock.getCondition();

        makeStatements(watchdog, conditionBlock, result.statements, jumps);
        Expression condition = stack.pop();

        ArmResult thenResult = buildSwitchArm(watchdog, ifBlock.getSub1(), localStack, jumps);

        result.values.addAll(thenResult.values);
        result.kind = ARM_VALUE;

        BaseStatement thenBody = buildSwitchArmBranch(thenResult);

        if (ifBlock.getType() == TYPE_IF_ELSE) {
            ArmResult elseResult = buildSwitchArm(watchdog, ifBlock.getSub2(), localStack, jumps);

            result.values.addAll(elseResult.values);

            BaseStatement elseBody = buildSwitchArmBranch(elseResult);
            result.statements.add(new IfElseStatement(condition, thenBody, elseBody));

            // A value-carrying goto block may follow the if/else (the merged
            // arm result); it becomes the arm's 'yield'.
            BasicBlock join = ifBlock.getNext();

            if ((join != BasicBlock.END) && (join.getType() == TYPE_GOTO_IN_TERNARY_OPERATOR)) {
                ArmResult joinResult = buildSwitchArm(watchdog, join, localStack, jumps);
                result.values.addAll(joinResult.values);

                if (joinResult.value != null) {
                    result.statements.add(new YieldStatement(joinResult.value));
                }
            }
        } else {
            ArmResult elseResult = buildSwitchArm(watchdog, ifBlock.getNext(), localStack, jumps);

            result.values.addAll(elseResult.values);

            BaseStatement elseBody = buildSwitchArmBranch(elseResult);
            result.statements.add(new IfStatement(condition, thenBody));

            if (elseBody instanceof Statements) {
                result.statements.addAll((Statements)elseBody);
            } else {
                result.statements.add((Statement)elseBody);
            }
        }
    }

    /**
     * Build the body of one branch of an 'if' located inside a switch arm.
     * Normal exits always become 'yield' statements here, throw exits keep
     * their throw statement.
     */
    protected BaseStatement buildSwitchArmBranch(ArmResult branch) {
        if (branch.kind == ARM_THROW) {
            if (branch.statements.size() == 1) {
                return branch.statements.getFirst();
            }

            return branch.statements;
        }

        if (branch.value != null) {
            branch.statements.add(new YieldStatement(branch.value));
        }

        return branch.statements;
    }

    /**
     * Convert the parsed statements of an arm into its modeled body: a bare
     * expression for direct values, a throw statement for throwing arms, or a
     * block ending with 'yield' for arms containing local statements or an
     * embedded 'if'.
     */
    protected BaseStatement buildSwitchArmBody(ArmResult result) {
        Statements subStatements = result.statements;
        replacePreOperatorWithPostOperator(subStatements);

        if (result.kind == ARM_THROW) {
            if (subStatements.size() == 1) {
                return subStatements.getFirst();
            }

            return subStatements;
        }

        // 'if' arm: the yield statements are already part of the statements.
        if (result.value == null) {
            return subStatements;
        }

        if (subStatements.isEmpty()) {
            // Direct value arm: model the bare expression (printed without a semicolon).
            return new ExpressionStatement(result.value);
        }

        subStatements.add(new YieldStatement(result.value));

        return subStatements;
    }

    /**
     * Locate the default target of a switch instruction directly from the byte
     * code when the reducer removed the default case (it pointed at the merge).
     */
    protected SwitchCase recoverDefaultSwitchCase(BasicBlock basicBlock, List<SwitchCase> switchCases) {
        for (SwitchCase switchCase : switchCases) {
            if (switchCase.isDefaultCase()) {
                return null;
            }
        }

        int[] offsets = ByteCodeUtil.readSwitchOffsets(basicBlock);
        int defaultOffset = offsets[0];

        for (BasicBlock bb : basicBlock.getControlFlowGraph().getBasicBlocks()) {
            if ((bb.getType() != BasicBlock.TYPE_DELETED) && (bb.getFromOffset() == defaultOffset)) {
                return new SwitchCase(bb);
            }
        }

        return null;
    }

    @SuppressWarnings("unchecked")
    protected void parseTry(WatchDog watchdog, BasicBlock basicBlock, Statements statements, Statements jumps, boolean jsr, boolean eclipse) {
        Statements tryStatements;
        DefaultList<TryStatement.CatchClause> catchClauses = new DefaultList<>();
        Statements finallyStatements = null;
        int assertStackSize = stack.size();

        tryStatements = makeSubStatements(watchdog, basicBlock.getSub1(), statements, jumps);

        for (ExceptionHandler exceptionHandler : basicBlock.getExceptionHandlers()) {
            assert stack.size() == assertStackSize : "parseTry : problem with stack";

            if (exceptionHandler.getInternalThrowableName() == null) {
                // 'finally' handler
                stack.push(FINALLY_EXCEPTION_EXPRESSION);

                finallyStatements = makeSubStatements(watchdog, exceptionHandler.getBasicBlock(), statements, jumps);

                if (!finallyStatements.getFirst().isMonitorEnterStatement()) {
                    removeFinallyStatementsFlag |= (jsr == false);

                    Expression leftExpression = finallyStatements.getFirst().getExpression().getLeftExpression();

                    if (leftExpression.isLocalVariableReferenceExpression()) {
                        Statement statement = finallyStatements.getLast();

                        if (statement.isThrowStatement()) {
                            // Remove synthetic local variable
                            Expression expression = statement.getExpression();

                            if (expression.isLocalVariableReferenceExpression()) {
                                ClassFileLocalVariableReferenceExpression vre1 = (ClassFileLocalVariableReferenceExpression) expression;
                                ClassFileLocalVariableReferenceExpression vre2 = (ClassFileLocalVariableReferenceExpression) leftExpression;

                                if (vre1.getLocalVariable() == vre2.getLocalVariable()) {
                                    localVariableMaker.removeLocalVariable(vre2.getLocalVariable());
                                    // Remove first statement (storage of finally localVariable)
                                    finallyStatements.removeFirst();
                                }
                            }
                        }
                    }

                    // Remove last statement (throw finally localVariable)
                    finallyStatements.removeLast();
                }
            } else {
                stack.push(new NullExpression(typeMaker.makeFromInternalTypeName(exceptionHandler.getInternalThrowableName())));

                Statements catchStatements = new Statements();
                localVariableMaker.pushFrame(catchStatements);

                BasicBlock bb = exceptionHandler.getBasicBlock();
                int lineNumber = bb.getControlFlowGraph().getLineNumber(bb.getFromOffset());
                int index = ByteCodeParser.getExceptionLocalVariableIndex(bb);
                ObjectType ot = typeMaker.makeFromInternalTypeName(exceptionHandler.getInternalThrowableName());
                int offset = bb.getFromOffset();
                byte[] code = bb.getControlFlowGraph().getMethod().<AttributeCode>getAttribute("Code").getCode();

                if (code[offset] == 58) {
                    offset += 2; // ASTORE
                } else {
                    offset++;    // POP, ASTORE_1 ... ASTORE_3
                }

                AbstractLocalVariable exception = localVariableMaker.getExceptionLocalVariable(index, offset, ot);

                makeStatements(watchdog, bb, catchStatements, jumps);
                localVariableMaker.popFrame();
                removeExceptionReference(catchStatements);

                if (lineNumber != UNKNOWN_LINE_NUMBER) {
                    searchFirstLineNumberVisitor.init();
                    searchFirstLineNumberVisitor.visit(catchStatements);
                    if (searchFirstLineNumberVisitor.getLineNumber() == lineNumber) {
                        lineNumber = UNKNOWN_LINE_NUMBER;
                    }
                }

                replacePreOperatorWithPostOperator(catchStatements);

                ClassFileTryStatement.CatchClause cc = new ClassFileTryStatement.CatchClause(lineNumber, ot, exception, catchStatements);

                if (exceptionHandler.getOtherInternalThrowableNames() != null) {
                    for (String name : exceptionHandler.getOtherInternalThrowableNames()) {
                        cc.addType(typeMaker.makeFromInternalTypeName(name));
                    }
                }

                catchClauses.add(cc);
            }
        }

        // 'try', 'try-with-resources' or 'synchronized' ?
        Statement statement = null;

        if ((finallyStatements != null) && (finallyStatements.size() > 0) && finallyStatements.getFirst().isMonitorExitStatement()) {
            statement = SynchronizedStatementMaker.make(localVariableMaker, statements, tryStatements);
        } else {
            if (majorVersion >= 51) { // (majorVersion >= Java 7)
                assert jsr == false;
                statement = TryWithResourcesStatementMaker.make(localVariableMaker, statements, tryStatements, catchClauses, finallyStatements);
            }
            if (statement == null) {
                statement = new ClassFileTryStatement(tryStatements, catchClauses, finallyStatements, jsr, eclipse);
            } else {
                mergeTryWithResourcesStatementFlag = true;
            }
        }

        statements.add(statement);
        makeStatements(watchdog, basicBlock.getNext(), statements, jumps);
    }

    protected void removeExceptionReference(Statements catchStatements) {
        if ((catchStatements.size() > 0) && catchStatements.getFirst().isExpressionStatement()) {
            Expression exp = catchStatements.getFirst().getExpression();

            if (exp.isBinaryOperatorExpression()) {
                if (exp.getLeftExpression().isLocalVariableReferenceExpression() && exp.getRightExpression().isNullExpression()) {
                    catchStatements.removeFirst();
                }
            } else if (exp.isNullExpression()) {
                catchStatements.removeFirst();
            }
        }
    }

    protected void parseJSR(WatchDog watchdog, BasicBlock basicBlock, Statements statements, Statements jumps) {
        int statementCount = statements.size();

        parseByteCode(basicBlock, statements);
        makeStatements(watchdog, basicBlock.getBranch(), statements, jumps);
        makeStatements(watchdog, basicBlock.getNext(), statements, jumps);

        // Remove synthetic local variable
        ExpressionStatement es = (ExpressionStatement)statements.get(statementCount);
        BinaryOperatorExpression boe = (BinaryOperatorExpression)es.getExpression();
        ClassFileLocalVariableReferenceExpression vre = (ClassFileLocalVariableReferenceExpression)boe.getLeftExpression();

        localVariableMaker.removeLocalVariable(vre.getLocalVariable());
        // Remove first statement (storage of JSR return offset)
        statements.remove(statementCount);
    }

    @SuppressWarnings("unchecked")
    protected void parseIf(WatchDog watchdog, BasicBlock basicBlock, Statements statements, Statements jumps) {
        BasicBlock condition = basicBlock.getCondition();

        if (condition.getType() == BasicBlock.TYPE_CONDITION_AND) {
            condition = condition.getSub1();
        }

        if (ByteCodeParser.isAssertCondition(internalTypeName, condition)) {
            Expression cond;

            if (condition == basicBlock.getCondition()) {
                cond = new BooleanExpression(condition.getFirstLineNumber(), false);
            } else {
                condition = basicBlock.getCondition().getSub2();
                condition.inverseCondition();
                makeStatements(watchdog, condition, statements, jumps);
                cond = stack.pop();
            }

            Statements subStatements = makeSubStatements(watchdog, basicBlock.getSub1(), statements, jumps);
            Expression message = null;

            if (subStatements.getFirst().isThrowStatement()) {
                Expression e = subStatements.getFirst().getExpression();
                if (e.isNewExpression()) {
                    BaseExpression parameters = e.getParameters();
                    if ((parameters != null) && !parameters.isList()) {
                        message = parameters.getFirst();
                    }
                }
            }

            statements.add(new AssertStatement(cond, message));
            makeStatements(watchdog, basicBlock.getNext(), statements, jumps);
        } else {
            makeStatements(watchdog, basicBlock.getCondition(), statements, jumps);
            Expression cond = stack.pop();
            DefaultStack<Expression> backup = new DefaultStack<Expression>(stack);
            Statements subStatements = makeSubStatements(watchdog, basicBlock.getSub1(), statements, jumps);
            if (stack.size() != backup.size()) {
                stack.copy(backup);
            }
            statements.add(new IfStatement(cond, subStatements));
            int index = statements.size();
            makeStatements(watchdog, basicBlock.getNext(), statements, jumps);

            if ((subStatements.size() == 1) &&
                    (index+1 == statements.size()) &&
                    (subStatements.getFirst().isReturnExpressionStatement()) &&
                    (statements.get(index).isReturnExpressionStatement())) {
                Statement cfres1 = subStatements.getFirst();

                if (cond.getLineNumber() >= cfres1.getLineNumber()) {
                    Statement cfres2 = statements.get(index);

                    if (cfres1.getLineNumber() == cfres2.getLineNumber()) {
                        statements.subList(index-1, statements.size()).clear();
                        statements.add(new ReturnExpressionStatement(newTernaryOperatorExpression(
                            cfres1.getLineNumber(), cond, cfres1.getExpression(), cfres2.getExpression())));
                    }
                }
            }
        }
    }

    @SuppressWarnings("unchecked")
    protected void parseLoop(WatchDog watchdog, BasicBlock basicBlock, Statements statements, Statements jumps) {
        BasicBlock sub1 = basicBlock.getSub1();
        BasicBlock updateBasicBlock = null;

        if ((sub1.getType() == TYPE_IF) && (sub1.getCondition() == END)) {
            updateBasicBlock = sub1.getNext();
            sub1 = sub1.getSub1();
        }

        if (sub1.getType() == TYPE_IF) {
            BasicBlock ifBB = sub1;

            if (ifBB.getNext() == LOOP_END) {
                // 'while' or 'for' loop
                makeStatements(watchdog, ifBB.getCondition(), statements, jumps);
                statements.add(LoopStatementMaker.makeLoop(
                    majorVersion, typeBounds, localVariableMaker, basicBlock, statements, stack.pop(),
                    makeSubStatements(watchdog, ifBB.getSub1(), statements, jumps, updateBasicBlock), jumps));
                makeStatements(watchdog, basicBlock.getNext(), statements, jumps);
                return;
            }

            if (ifBB.getSub1() == LOOP_END) {
                if (ifBB.getNext() == LOOP_START) {
                    // 'do-while' pattern
                    ifBB.getCondition().inverseCondition();

                    Statements subStatements = new Statements();

                    makeStatements(watchdog, ifBB.getCondition(), subStatements, jumps);
                    replacePreOperatorWithPostOperator(subStatements);
                    statements.add(LoopStatementMaker.makeDoWhileLoop(basicBlock, stack.pop(), subStatements, jumps));
                } else {
                    // 'while' or 'for' loop
                    ifBB.getCondition().inverseCondition();
                    makeStatements(watchdog, ifBB.getCondition(), statements, jumps);
                    statements.add(LoopStatementMaker.makeLoop(
                        majorVersion, typeBounds, localVariableMaker, basicBlock, statements, stack.pop(),
                        makeSubStatements(watchdog, ifBB.getNext(), statements, jumps, updateBasicBlock), jumps));
                }

                makeStatements(watchdog, basicBlock.getNext(), statements, jumps);
                return;
            }
        }

        BasicBlock next = sub1.getNext();
        BasicBlock last = sub1;

        while (next.matchType(GROUP_SINGLE_SUCCESSOR) && (next.getPredecessors().size() == 1)) {
            last = next;
            next = next.getNext();
        }

        if ((next == LOOP_START) && (last.getType() == TYPE_IF) && (last.getSub1() == LOOP_END) && (countStartLoop(sub1) == 1)) {
            // 'do-while'
            Statements subStatements;

            last.getCondition().inverseCondition();
            last.setType(TYPE_END);

            if ((sub1.getType() == TYPE_LOOP) && (sub1.getNext() == last) && (countStartLoop(sub1.getSub1()) == 0)) {
                changeEndLoopToStartLoop(new BitSet(), sub1.getSub1());
                subStatements = makeSubStatements(watchdog, sub1.getSub1(), statements, jumps, updateBasicBlock);

                assert subStatements.getLast() == ContinueStatement.CONTINUE : "StatementMaker.parseLoop(...) : unexpected basic block for create a do-while loop";

                subStatements.removeLast();
            } else {
                createDoWhileContinue(last);
                subStatements = makeSubStatements(watchdog, sub1, statements, jumps, updateBasicBlock);
            }

            makeStatements(watchdog, last.getCondition(), subStatements, jumps);
            statements.add(LoopStatementMaker.makeDoWhileLoop(basicBlock, stack.pop(), subStatements, jumps));
        } else {
            // Infinite loop
            statements.add(LoopStatementMaker.makeLoop(
                localVariableMaker, basicBlock, statements, makeSubStatements(watchdog, sub1, statements, jumps, updateBasicBlock), jumps));
        }

        makeStatements(watchdog, basicBlock.getNext(), statements, jumps);
    }

    protected int countStartLoop(BasicBlock bb) {
        int count = 0;

        while (bb.matchType(GROUP_SINGLE_SUCCESSOR)) {
            switch (bb.getType()) {
                case TYPE_SWITCH:
                    for (SwitchCase switchCase : bb.getSwitchCases()) {
                        count += countStartLoop(switchCase.getBasicBlock());
                    }
                    break;
                case TYPE_TRY:
                case TYPE_TRY_JSR:
                case TYPE_TRY_ECLIPSE:
                    count += countStartLoop(bb.getSub1());

                    for (ExceptionHandler exceptionHandler : bb.getExceptionHandlers()) {
                        count += countStartLoop(exceptionHandler.getBasicBlock());
                    }
                    break;
                case TYPE_IF_ELSE:
                case TYPE_TERNARY_OPERATOR:
                    count += countStartLoop(bb.getSub2());
                case TYPE_IF:
                    count += countStartLoop(bb.getSub1());
                    break;
            }

            bb = bb.getNext();
        }

        if (bb.getType() == TYPE_LOOP_START) {
            count++;
        }

        return count;
    }

    protected void createDoWhileContinue(BasicBlock last) {
        boolean change;

        do {
            change = false;

            for (BasicBlock predecessor : last.getPredecessors()) {
                if (predecessor.getType() == TYPE_IF) {
                    BasicBlock l = predecessor.getSub1();

                    if (l.matchType(GROUP_SINGLE_SUCCESSOR)) {
                        BasicBlock n = l.getNext();

                        while (n.matchType(GROUP_SINGLE_SUCCESSOR)) {
                            l = n;
                            n = n.getNext();
                        }

                        if (n == END) {
                            // Transform 'if' to 'if-continue'
                            predecessor.getCondition().inverseCondition();
                            predecessor.setNext(predecessor.getSub1());
                            last.getPredecessors().remove(predecessor);
                            last.getPredecessors().add(l);
                            l.setNext(last);
                            predecessor.setSub1(LOOP_START);
                            change = true;
                        }
                    }
                }
            }
        } while (change);
    }

    protected static void changeEndLoopToStartLoop(BitSet visited, BasicBlock basicBlock) {
        if (!basicBlock.matchType(GROUP_END| TYPE_LOOP_END) && (visited.get(basicBlock.getIndex()) == false)) {
            visited.set(basicBlock.getIndex());

            switch (basicBlock.getType()) {
                case TYPE_CONDITIONAL_BRANCH:
                case TYPE_JSR:
                case TYPE_CONDITION:
                    if (basicBlock.getBranch() == LOOP_END) {
                        basicBlock.setBranch(LOOP_START);
                    } else {
                        changeEndLoopToStartLoop(visited, basicBlock.getBranch());
                    }
                case TYPE_START:
                case TYPE_STATEMENTS:
                case TYPE_GOTO:
                case TYPE_GOTO_IN_TERNARY_OPERATOR:
                case TYPE_LOOP:
                    if (basicBlock.getNext() == LOOP_END) {
                        basicBlock.setNext(LOOP_START);
                    } else {
                        changeEndLoopToStartLoop(visited, basicBlock.getNext());
                    }
                    break;
                case TYPE_TRY_DECLARATION:
                case TYPE_TRY:
                case TYPE_TRY_JSR:
                case TYPE_TRY_ECLIPSE:
                    for (BasicBlock.ExceptionHandler exceptionHandler : basicBlock.getExceptionHandlers()) {
                        if (exceptionHandler.getBasicBlock() == LOOP_END) {
                            exceptionHandler.setBasicBlock(LOOP_START);
                        } else {
                            changeEndLoopToStartLoop(visited, exceptionHandler.getBasicBlock());
                        }
                    }
                    break;
                case TYPE_IF_ELSE:
                case TYPE_TERNARY_OPERATOR:
                    if (basicBlock.getSub2() == LOOP_END) {
                        basicBlock.setSub2(LOOP_START);
                    } else {
                        changeEndLoopToStartLoop(visited, basicBlock.getSub2());
                    }
                case TYPE_IF:
                    if (basicBlock.getSub1() == LOOP_END) {
                        basicBlock.setSub1(LOOP_START);
                    } else {
                        changeEndLoopToStartLoop(visited, basicBlock.getSub1());
                    }
                    if (basicBlock.getNext() == LOOP_END) {
                        basicBlock.setNext(LOOP_START);
                    } else {
                        changeEndLoopToStartLoop(visited, basicBlock.getNext());
                    }
                    break;
                case TYPE_SWITCH:
                case TYPE_SWITCH_DECLARATION:
                    for (SwitchCase switchCase : basicBlock.getSwitchCases()) {
                        if (switchCase.getBasicBlock() == LOOP_END) {
                            switchCase.setBasicBlock(LOOP_START);
                        } else {
                            changeEndLoopToStartLoop(visited, switchCase.getBasicBlock());
                        }
                    }
                    break;
            }
        }
    }

    protected Expression parseTernaryOperator(int lineNumber, Expression condition, Expression exp1, Expression exp2) {
        if (ObjectType.TYPE_CLASS.equals(exp1.getType()) && ObjectType.TYPE_CLASS.equals(exp2.getType()) && condition.isBinaryOperatorExpression()) {

            if (condition.getLeftExpression().isFieldReferenceExpression() && condition.getRightExpression().isNullExpression()) {
                FieldReferenceExpression freCond = (FieldReferenceExpression) condition.getLeftExpression();

                if (freCond.getInternalTypeName().equals(internalTypeName)) {
                    String fieldName = freCond.getName();

                    if (fieldName.startsWith("class$")) {
                        if (condition.getOperator().equals("==") && exp1.isBinaryOperatorExpression() && checkFieldReference(fieldName, exp2)) {
                            if (exp1.getRightExpression().isMethodInvocationExpression() && checkFieldReference(fieldName, exp1.getLeftExpression())) {
                                MethodInvocationExpression mie = (MethodInvocationExpression) exp1.getRightExpression();

                                if (mie.getParameters().isStringConstantExpression() && mie.getName().equals("class$") && mie.getInternalTypeName().equals(internalTypeName)) {
                                    // JDK 1.4.2 '.class' found ==> Convert '(class$java$lang$String == null) ? (class$java$lang$String = TestDotClass.class$("java.lang.String") : class$java$lang$String)' to 'String.class'
                                    return createObjectTypeReferenceDotClassExpression(lineNumber, fieldName, mie);
                                }
                            }
                        } else if (condition.getOperator().equals("!=") && exp2.isBinaryOperatorExpression() && checkFieldReference(fieldName, exp1)) {
                            if (exp2.getRightExpression().isMethodInvocationExpression() && checkFieldReference(fieldName, exp2.getLeftExpression())) {
                                MethodInvocationExpression mie = (MethodInvocationExpression) exp2.getRightExpression();

                                if (mie.getParameters().isStringConstantExpression() && mie.getName().equals("class$") && mie.getInternalTypeName().equals(internalTypeName)) {
                                    // JDK 1.1.8 '.class' found ==> Convert '(class$java$lang$String != null) ? class$java$lang$String : (class$java$lang$String = TestDotClass.class$("java.lang.String"))' to 'String.class'
                                    return createObjectTypeReferenceDotClassExpression(lineNumber, fieldName, mie);
                                }
                            }
                        }
                    }
                }
            }
        }

        return newTernaryOperatorExpression(lineNumber, condition, exp1, exp2);
    }

    protected TernaryOperatorExpression newTernaryOperatorExpression(int lineNumber, Expression condition, Expression expressionTrue, Expression expressionFalse) {
        Type type = combineExpressionTypes(expressionTrue, expressionFalse);

        return new TernaryOperatorExpression(lineNumber, type, condition, expressionTrue, expressionFalse);
    }

    /**
     * Compute the type of a value merged from several expressions (ternary
     * operator, switch expression arms).
     */
    protected Type combineExpressionTypes(Expression expressionTrue, Expression expressionFalse) {
        Type expressionTrueType = expressionTrue.getType();
        Type expressionFalseType = expressionFalse.getType();
        Type type;

        if (expressionTrue.isNullExpression()) {
            type = expressionFalseType;
        } else if (expressionFalse.isNullExpression()) {
            type = expressionTrueType;
        } else if (expressionTrueType.equals(expressionFalseType)) {
            type = expressionTrueType;
        } else if (expressionTrueType.isPrimitiveType() && expressionFalseType.isPrimitiveType()) {
            int flags = ((PrimitiveType)expressionTrueType).getFlags() | ((PrimitiveType)expressionFalseType).getFlags();

            if ((flags & FLAG_DOUBLE) != 0) {
                type = TYPE_DOUBLE;
            } else if ((flags & FLAG_FLOAT) != 0) {
                type = TYPE_FLOAT;
            } else if ((flags & FLAG_LONG) != 0) {
                type = TYPE_LONG;
            } else {
                flags = ((PrimitiveType)expressionTrueType).getFlags() & ((PrimitiveType)expressionFalseType).getFlags();

                if ((flags & FLAG_INT) != 0) {
                    type = TYPE_INT;
                } else if ((flags & FLAG_SHORT) != 0) {
                    type = TYPE_SHORT;
                } else if ((flags & FLAG_CHAR) != 0) {
                    type = TYPE_CHAR;
                } else if ((flags & FLAG_BYTE) != 0) {
                    type = TYPE_BYTE;
                } else {
                    type = MAYBE_BOOLEAN_TYPE;
                }
            }
        } else if (expressionTrueType.isObjectType() && expressionFalseType.isObjectType()) {
            ObjectType ot1 = (ObjectType)expressionTrueType;
            ObjectType ot2 = (ObjectType)expressionFalseType;

            if (typeMaker.isAssignable(typeBounds, ot1, ot2)) {
                type = getTernaryOperatorExpressionType(ot1, ot2);
            } else if (typeMaker.isAssignable(typeBounds, ot2, ot1)) {
                type = getTernaryOperatorExpressionType(ot2, ot1);
            } else {
                type = TYPE_UNDEFINED_OBJECT;
            }
        } else {
            type = TYPE_UNDEFINED_OBJECT;
        }

        return type;
    }

    protected Type getTernaryOperatorExpressionType(ObjectType ot1, ObjectType ot2) {
        if (ot1.getTypeArguments() == null) {
            return ot1;
        } else if (ot2.getTypeArguments() == null) {
            return ot1.createType(null);
        } else if (ot1.isTypeArgumentAssignableFrom(typeBounds, ot2)) {
            return ot1;
        } else if (ot2.isTypeArgumentAssignableFrom(typeBounds, ot1)) {
            return ot1.createType(ot2.getTypeArguments());
        } else {
            return ot1.createType(null);
        }
    }

    protected boolean checkFieldReference(String fieldName, Expression expression) {
        if (expression.isFieldReferenceExpression()) {
            FieldReferenceExpression fre = (FieldReferenceExpression) expression;
            return fre.getName().equals(fieldName) && fre.getInternalTypeName().equals(internalTypeName);
        }

        return false;
    }

    @SuppressWarnings("unchecked")
    protected Expression createObjectTypeReferenceDotClassExpression(int lineNumber, String fieldName, MethodInvocationExpression mie) {
        // Add SYNTHETIC flags to field
        memberVisitor.init(fieldName);

        for (ClassFileFieldDeclaration field : bodyDeclaration.getFieldDeclarations()) {
            field.getFieldDeclarators().accept(memberVisitor);
            if (memberVisitor.found()) {
                field.setFlags(field.getFlags() | Constants.ACC_SYNTHETIC);
                break;
            }
        }

        // Add SYNTHETIC flags to method named 'class$'
        memberVisitor.init("class$");

        for (ClassFileConstructorOrMethodDeclaration member : bodyDeclaration.getMethodDeclarations()) {
            member.accept(memberVisitor);
            if (memberVisitor.found()) {
                member.setFlags(member.getFlags() | Constants.ACC_SYNTHETIC);
                break;
            }
        }

        String typeName = mie.getParameters().getStringValue();
        ObjectType ot = typeMaker.makeFromInternalTypeName(typeName.replace('.', '/'));

        return new TypeReferenceDotClassExpression(lineNumber, ot);
    }

    protected void parseByteCode(BasicBlock basicBlock, Statements statements) {
        byteCodeParser.parse(basicBlock, statements, stack);
    }

    @SuppressWarnings("unchecked")
    protected void replacePreOperatorWithPostOperator(Statements statements) {
        Iterator<Statement> iterator = statements.iterator();

        while (iterator.hasNext()) {
            Statement statement = iterator.next();

            if (statement.getExpression().isPreOperatorExpression()) {
                Expression poe = statement.getExpression();
                String operator = poe.getOperator();

                if ("++".equals(operator) || "--".equals(operator)) {
                    if (statement.isExpressionStatement()) {
                        ExpressionStatement es = (ExpressionStatement) statement;
                        // Replace pre-operator statement with post-operator statement
                        es.setExpression(new PostOperatorExpression(poe.getLineNumber(), poe.getExpression(), operator));
                    } else if (statement.isReturnExpressionStatement()) {
                        ReturnExpressionStatement res = (ReturnExpressionStatement) statement;
                        // Replace pre-operator statement with post-operator statement
                        res.setExpression(new PostOperatorExpression(poe.getLineNumber(), poe.getExpression(), operator));
                    }
                }
            }
        }
    }

    @SuppressWarnings("unchecked")
    protected void updateJumpStatements(Statements jumps) {
        assert false : "StatementMaker.updateJumpStatements(stmt) : 'jumps' list is not empty";

        Iterator<Statement> iterator = jumps.iterator();

        while (iterator.hasNext()) {
            ClassFileBreakContinueStatement statement = (ClassFileBreakContinueStatement)iterator.next();

            statement.setStatement(new CommentStatement("// Byte code: goto -> " + statement.getTargetOffset()));
        }
    }

    protected static class SwitchCaseComparator implements Comparator<SwitchCase> {
        @Override
        public int compare(SwitchCase sc1, SwitchCase sc2) {
            int diff = sc1.getOffset() - sc2.getOffset();

            if (diff != 0)
                return diff;

            return sc1.getValue() - sc2.getValue();
        }
    }

    protected static class MemberVisitor extends AbstractJavaSyntaxVisitor {
        protected String name;
        protected boolean found;

        public void init(String name) {
            this.name = name;
            this.found = false;
        }

        public boolean found() {
            return found;
        }

        @Override
        public void visit(FieldDeclarator declaration) {
            found |= declaration.getName().equals(name);
        }

        @Override
        public void visit(MethodDeclaration declaration) {
            found |= declaration.getName().equals(name);
        }
    }
}
