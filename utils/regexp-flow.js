/**
 * Flow-sensitive string value analysis for `RegExp(...)` / `new RegExp(...)` calls.
 *
 * For every RegExp constructor call in a file, this module computes what the
 * first argument may evaluate to at that call site. The analyzed program is
 * never executed: values are produced by a fixpoint dataflow analysis over a
 * control-flow graph built independently for the program and for each
 * function. See docs/regexp-flow.md for the supported subset, the merge
 * rules, and the conservative degradation boundaries.
 *
 * A value has the shape `{ strings, statik, dynamic, overflowed }`:
 * - `strings`: known candidate patterns (at most MAX_CANDIDATES, each at most
 *   MAX_PATTERN_LENGTH UTF-16 code units).
 * - `statik`: the expression may be a static value that cannot be evaluated
 *   locally (e.g. `path.join()`, `require.resolve()`, `import.meta`, or a
 *   read-only binding of static origin). Never reported by itself.
 * - `dynamic`: the expression may be a dynamic/unknown value. This is what
 *   `detect-non-literal-regexp` reports.
 * - `overflowed`: the value exceeded a budget and collapsed to unknown. It
 *   stays unknown through later merges and only a definite overwrite
 *   assignment can recover it.
 */

'use strict';

const { isStaticExpression } = require('./is-static-expression');
const { findVariable } = require('./find-variable');

/** Maximum number of distinct candidate strings kept per value. */
const MAX_CANDIDATES = 8;
/** Maximum length (in UTF-16 code units) of a single candidate string. */
const MAX_PATTERN_LENGTH = 4096;
/**
 * Maximum number of program points per function. Pathological nesting of
 * `try`/`finally` could otherwise duplicate blocks without bound; when the
 * budget is exceeded the function falls back to the syntactic behavior.
 */
const MAX_POINTS_PER_FUNCTION = 20000;

/** Thrown when a function's control-flow graph exceeds the point budget. */
class AnalysisBudgetError extends Error {}

/** Node types that start an independently analyzed function-like scope. */
const FUNCTION_LIKE_TYPES = new Set(['FunctionDeclaration', 'FunctionExpression', 'ArrowFunctionExpression', 'StaticBlock']);
/** Scope types that bound the scopes owned by a function-like analysis. */
const FUNCTION_BOUNDARY_SCOPE_TYPES = new Set(['function', 'function-expression-name', 'class-static-block', 'class-field-initializer']);

/**
 * @typedef {Object} PatternValue
 * @property {string[]} strings Known candidate patterns.
 * @property {boolean} statik May be a static value that cannot be evaluated locally.
 * @property {boolean} dynamic May be a dynamic/unknown value.
 * @property {boolean} overflowed Exceeded a budget and collapsed to unknown.
 */

/**
 * @param {string[]} strings
 * @param {boolean} statik
 * @param {boolean} dynamic
 * @param {boolean} overflowed
 * @returns {PatternValue}
 */
function makeValue(strings, statik, dynamic, overflowed) {
  return Object.freeze({ strings: Object.freeze(strings.slice()), statik, dynamic, overflowed });
}

/** The value of a fully unknown, dynamic expression. */
const DYNAMIC = makeValue([], false, true, false);
/** The value of a static expression that cannot be evaluated locally. */
const STATIC_UNKNOWN = makeValue([], true, false, false);

/**
 * @param {string} pattern A single known pattern.
 * @returns {PatternValue} The singleton value, degrading to unknown when the length budget is exceeded.
 */
function knownString(pattern) {
  if (pattern.length > MAX_PATTERN_LENGTH) {
    return makeValue([], false, true, true);
  }
  return makeValue([pattern], false, false, false);
}

/**
 * Joins two values, e.g. at a control-flow merge. Known candidates from both
 * sides survive; an unknown side never washes known candidates away. An
 * overflowed value contributes no candidates and stays unknown.
 * @param {PatternValue} a
 * @param {PatternValue} b
 * @returns {PatternValue}
 */
function mergeValues(a, b) {
  if (a === b) {
    return a;
  }
  const statik = a.statik || b.statik;
  const overflowed = a.overflowed || b.overflowed;
  const strings = [];
  for (const value of [a, b]) {
    if (value.overflowed) {
      continue;
    }
    for (const pattern of value.strings) {
      if (!strings.includes(pattern)) {
        strings.push(pattern);
      }
    }
  }
  if (overflowed || strings.length > MAX_CANDIDATES) {
    return makeValue([], statik, true, true);
  }
  return makeValue(strings, statik, a.dynamic || b.dynamic, false);
}

/**
 * Concatenates two values (the `+` operator and template literal parts).
 * @param {PatternValue} a
 * @param {PatternValue} b
 * @returns {PatternValue}
 */
function concatValues(a, b) {
  const statik = a.statik || b.statik;
  if (a.dynamic || b.dynamic || a.overflowed || b.overflowed) {
    return makeValue([], statik, true, a.overflowed || b.overflowed);
  }
  if (statik) {
    return STATIC_UNKNOWN;
  }
  const strings = [];
  for (const left of a.strings) {
    for (const right of b.strings) {
      const pattern = left + right;
      if (pattern.length > MAX_PATTERN_LENGTH) {
        return makeValue([], false, true, true);
      }
      if (!strings.includes(pattern)) {
        strings.push(pattern);
      }
      if (strings.length > MAX_CANDIDATES) {
        return makeValue([], false, true, true);
      }
    }
  }
  return makeValue(strings, false, false, false);
}

/**
 * @param {PatternValue} a
 * @param {PatternValue} b
 * @returns {boolean} Whether both values describe the same possibilities.
 */
function valuesEqual(a, b) {
  return (
    a.statik === b.statik &&
    a.dynamic === b.dynamic &&
    a.overflowed === b.overflowed &&
    a.strings.length === b.strings.length &&
    a.strings.every((pattern) => b.strings.includes(pattern))
  );
}

/**
 * @typedef {Map<import("eslint").Scope.Variable, PatternValue>} VariableState
 */

/**
 * @param {VariableState|null} a
 * @param {VariableState|null} b
 * @returns {VariableState|null} The join of two states (`null` means unreachable).
 */
function mergeStates(a, b) {
  if (a === null) {
    return b;
  }
  if (b === null) {
    return a;
  }
  const merged = new Map(a);
  for (const [variable, value] of b) {
    const previous = merged.get(variable);
    merged.set(variable, previous === undefined ? value : mergeValues(previous, value));
  }
  return merged;
}

/**
 * @param {VariableState|null} a
 * @param {VariableState|null} b
 * @returns {boolean} Whether both states map every variable to an equal value.
 */
function statesEqual(a, b) {
  if (a === null || b === null) {
    return a === b;
  }
  if (a.size !== b.size) {
    return false;
  }
  for (const [variable, value] of a) {
    const other = b.get(variable);
    if (other === undefined || !valuesEqual(value, other)) {
      return false;
    }
  }
  return true;
}

/**
 * Collects the function-like nodes of a file (the program itself plus every
 * function and class static block, including nested ones).
 * @param {import("eslint").SourceCode} sourceCode
 * @returns {Array<import("estree").Node>}
 */
function collectFunctionLikeNodes(sourceCode) {
  const nodes = [sourceCode.ast];
  const visitorKeys = sourceCode.visitorKeys;
  const stack = [sourceCode.ast];
  while (stack.length > 0) {
    const node = stack.pop();
    for (const key of visitorKeys[node.type] || []) {
      const child = node[key];
      if (Array.isArray(child)) {
        for (const item of child) {
          if (item && typeof item.type === 'string') {
            if (FUNCTION_LIKE_TYPES.has(item.type)) {
              nodes.push(item);
            }
            stack.push(item);
          }
        }
      } else if (child && typeof child.type === 'string') {
        if (FUNCTION_LIKE_TYPES.has(child.type)) {
          nodes.push(child);
        }
        stack.push(child);
      }
    }
  }
  return nodes;
}

/**
 * Collects the scopes owned by a function-like node: its own scope plus all
 * descendant scopes that do not belong to a nested function.
 * @param {import("eslint").Scope.Scope} rootScope The scope of the function-like node.
 * @returns {Array<import("eslint").Scope.Scope>}
 */
function collectOwnScopes(rootScope) {
  const scopes = [rootScope];
  for (let index = 0; index < scopes.length; index++) {
    for (const child of scopes[index].childScopes) {
      if (!FUNCTION_BOUNDARY_SCOPE_TYPES.has(child.type)) {
        scopes.push(child);
      }
    }
  }
  return scopes;
}

/**
 * Checks whether a function-like subtree contains a direct `eval(...)` call
 * that resolves to the global `eval`. Such a call can read and write any
 * binding in scope, so every local of the function is treated as unknown.
 * @param {import("eslint").SourceCode} sourceCode
 * @param {import("estree").Node} funcNode
 * @returns {boolean}
 */
function containsDirectEval(sourceCode, funcNode) {
  const visitorKeys = sourceCode.visitorKeys;
  const stack = [funcNode];
  while (stack.length > 0) {
    const node = stack.pop();
    if (node.type === 'CallExpression' && node.callee.type === 'Identifier' && node.callee.name === 'eval') {
      const variable = findVariable(sourceCode.getScope(node.callee), 'eval');
      if (!variable || variable.defs.length === 0) {
        return true;
      }
    }
    for (const key of visitorKeys[node.type] || []) {
      const child = node[key];
      if (Array.isArray(child)) {
        for (const item of child) {
          if (item && typeof item.type === 'string') {
            stack.push(item);
          }
        }
      } else if (child && typeof child.type === 'string') {
        stack.push(child);
      }
    }
  }
  return false;
}

/**
 * Builds the information about the bindings local to one function-like node.
 * @param {import("eslint").SourceCode} sourceCode
 * @param {import("estree").Node} funcNode
 * @returns {{locals: Set<import("eslint").Scope.Variable>, contaminated: Set<import("eslint").Scope.Variable>, evalPoisoned: boolean}}
 */
function collectFunctionInfo(sourceCode, funcNode) {
  const ownScopes = collectOwnScopes(sourceCode.getScope(funcNode));
  const ownScopeSet = new Set(ownScopes);
  const locals = new Set();
  for (const scope of ownScopes) {
    for (const variable of scope.variables) {
      // Only bindings actually declared in this function are tracked.
      // Implicit globals (e.g. `__dirname`) keep their special static meaning.
      if (variable.defs.length > 0) {
        locals.add(variable);
      }
    }
  }
  const contaminated = new Set();
  for (const variable of locals) {
    // A binding written by any nested function is unknown everywhere in this
    // function, because the closure may run at any time.
    if (variable.references.some((reference) => reference.isWrite() && !ownScopeSet.has(reference.from))) {
      contaminated.add(variable);
    }
  }
  return { locals, contaminated, evalPoisoned: containsDirectEval(sourceCode, funcNode) };
}

/**
 * Collects the identifier leaves bound by a destructuring pattern.
 * @param {import("estree").Node} pattern
 * @returns {Array<import("estree").Identifier>}
 */
function collectPatternIdentifiers(pattern) {
  switch (pattern.type) {
    case 'Identifier':
      return [pattern];
    case 'RestElement':
      return collectPatternIdentifiers(pattern.argument);
    case 'AssignmentPattern':
      return collectPatternIdentifiers(pattern.left);
    case 'ArrayPattern':
      return pattern.elements.flatMap((element) => (element ? collectPatternIdentifiers(element) : []));
    case 'ObjectPattern':
      return pattern.properties.flatMap((property) => collectPatternIdentifiers(property.type === 'Property' ? property.value : property.argument));
    default:
      return [];
  }
}

/**
 * Creates the control-flow graph of one function-like node and records the
 * RegExp constructor calls to observe per program point.
 * @param {import("eslint").SourceCode} sourceCode
 * @param {import("estree").Node} funcNode
 * @param {{locals: Set<import("eslint").Scope.Variable>, contaminated: Set<import("eslint").Scope.Variable>}} info
 * @returns {{points: Array<Object>, entry: Object}}
 */
function buildControlFlowGraph(sourceCode, funcNode, info) {
  const points = [];
  const breakableStack = [];
  const finallyStack = [];
  const throwStack = [];
  const pendingLabels = [];
  let mayWriteDepth = 0;

  /**
   * @returns {Object} A new program point.
   */
  function newPoint() {
    if (points.length >= MAX_POINTS_PER_FUNCTION) {
      throw new AnalysisBudgetError();
    }
    const point = { id: points.length, preds: [], succs: [], poison: [], writes: [], observes: [], mayWrite: mayWriteDepth > 0 };
    points.push(point);
    return point;
  }

  /**
   * @param {Array<Object>} fromList
   * @param {Object} to
   */
  function connect(fromList, to) {
    for (const from of fromList) {
      from.succs.push(to);
      to.preds.push(from);
    }
  }

  /**
   * Resolves an identifier node to a tracked local variable, if any.
   * @param {import("estree").Identifier} node
   * @returns {import("eslint").Scope.Variable | null}
   */
  function resolveLocal(node) {
    const variable = findVariable(sourceCode.getScope(node), node.name);
    return variable && info.locals.has(variable) ? variable : null;
  }

  /**
   * Adds every binding of a destructuring pattern to the point's unknown
   * writes. Destructuring writes are not precisely analyzed.
   * @param {Object} point
   * @param {import("estree").Node} pattern
   */
  function poisonPattern(point, pattern) {
    for (const identifier of collectPatternIdentifiers(pattern)) {
      const variable = resolveLocal(identifier);
      if (variable) {
        point.poison.push(variable);
      }
    }
  }

  /**
   * Walks an expression that is evaluated at the given point: records RegExp
   * constructor calls to observe and poisons locals written through
   * unsupported assignment forms. Nested functions are not entered.
   * @param {Object} point
   * @param {import("estree").Node} node
   */
  function registerExpression(point, node) {
    if (!node) {
      return;
    }
    switch (node.type) {
      case 'CallExpression':
      case 'NewExpression':
        if (node.callee.type === 'Identifier' && node.callee.name === 'RegExp' && node.arguments.length > 0) {
          point.observes.push({ node, arg: node.arguments[0] });
        }
        break;
      case 'AssignmentExpression':
        if (node.left.type === 'Identifier') {
          const variable = resolveLocal(node.left);
          if (variable) {
            point.poison.push(variable);
          }
        } else if (node.left.type === 'ObjectPattern' || node.left.type === 'ArrayPattern') {
          poisonPattern(point, node.left);
        } else {
          registerExpression(point, node.left);
        }
        registerExpression(point, node.right);
        return;
      case 'UpdateExpression':
        if (node.argument.type === 'Identifier') {
          const variable = resolveLocal(node.argument);
          if (variable) {
            point.poison.push(variable);
          }
        } else {
          registerExpression(point, node.argument);
        }
        return;
      case 'FunctionDeclaration':
      case 'FunctionExpression':
      case 'ArrowFunctionExpression':
      case 'ClassDeclaration':
      case 'ClassExpression':
      case 'StaticBlock':
        return;
      default:
        break;
    }
    for (const key of sourceCode.visitorKeys[node.type] || []) {
      const child = node[key];
      if (Array.isArray(child)) {
        for (const item of child) {
          if (item && typeof item.type === 'string') {
            registerExpression(point, item);
          }
        }
      } else if (child && typeof child.type === 'string') {
        registerExpression(point, child);
      }
    }
  }

  /**
   * Returns the precise assignment of a standalone expression statement, if
   * it is a supported `=` or `+=` write to a tracked local identifier.
   * @param {import("estree").Expression} expression
   * @returns {{variable: import("eslint").Scope.Variable, kind: 'set' | 'concat', rhs: import("estree").Expression} | null}
   */
  function preciseAssignment(expression) {
    if (expression.type !== 'AssignmentExpression' || (expression.operator !== '=' && expression.operator !== '+=') || expression.left.type !== 'Identifier') {
      return null;
    }
    const variable = resolveLocal(expression.left);
    if (!variable || info.contaminated.has(variable)) {
      return null;
    }
    return { variable, kind: expression.operator === '+=' ? 'concat' : 'set', rhs: expression.right };
  }

  /**
   * Routes an abnormal exit (break/continue/return/throw) either through the
   * innermost enclosing `finally` or directly to its target.
   * @param {{kind: 'break' | 'continue' | 'exit', frame: Object | null, frontier: Array<Object>}} escape
   */
  function applyEscape(escape) {
    const pendingFinally = finallyStack[finallyStack.length - 1];
    if (pendingFinally) {
      pendingFinally.escapes.push(escape);
      return;
    }
    if (escape.kind === 'break') {
      escape.frame.breaks.push(...escape.frontier);
    } else if (escape.kind === 'continue') {
      connect(escape.frontier, escape.frame.continuePoint);
    }
    // `return` and `throw` leave the function: the frontier is dropped.
  }

  /**
   * @returns {Object} A new breakable frame for a loop or switch.
   */
  function newFrame(continuePoint) {
    return { breaks: [], continuePoint, labels: pendingLabels.splice(0) };
  }

  /**
   * @param {Array<import("estree").Statement>} statements
   * @param {Array<Object>} frontier
   * @returns {Array<Object>} The frontier after the statements.
   */
  function processStatements(statements, frontier) {
    for (const statement of statements) {
      frontier = processStatement(statement, frontier);
    }
    return frontier;
  }

  /**
   * @param {import("estree").Statement} statement
   * @param {Array<Object>} frontier
   * @returns {Array<Object>} The frontier after the statement.
   */
  function processStatement(statement, frontier) {
    switch (statement.type) {
      case 'EmptyStatement':
      case 'DebuggerStatement':
      case 'FunctionDeclaration':
      case 'ImportDeclaration':
        return frontier;
      case 'BlockStatement':
        return processStatements(statement.body, frontier);
      case 'ExpressionStatement': {
        const point = newPoint();
        const precise = preciseAssignment(statement.expression);
        if (precise) {
          point.writes.push(precise);
          registerExpression(point, precise.rhs);
        } else {
          registerExpression(point, statement.expression);
        }
        connect(frontier, point);
        return [point];
      }
      case 'VariableDeclaration': {
        for (const declarator of statement.declarations) {
          const point = newPoint();
          if (declarator.id.type === 'Identifier') {
            const [variable] = sourceCode.getDeclaredVariables(declarator);
            if (variable && info.locals.has(variable) && !info.contaminated.has(variable)) {
              if (declarator.init) {
                point.writes.push({ variable, kind: 'set', rhs: declarator.init });
              } else {
                // Uninitialized variables start unknown.
                point.writes.push({ variable, kind: 'set', value: DYNAMIC });
              }
            } else if (variable && info.locals.has(variable)) {
              point.poison.push(variable);
            }
          } else {
            poisonPattern(point, declarator.id);
          }
          if (declarator.init) {
            registerExpression(point, declarator.init);
          }
          connect(frontier, point);
          frontier = [point];
        }
        return frontier;
      }
      case 'IfStatement': {
        const point = newPoint();
        registerExpression(point, statement.test);
        connect(frontier, point);
        const thenEnds = processStatement(statement.consequent, [point]);
        const elseEnds = statement.alternate ? processStatement(statement.alternate, [point]) : [point];
        return [...thenEnds, ...elseEnds];
      }
      case 'SwitchStatement': {
        const point = newPoint();
        registerExpression(point, statement.discriminant);
        connect(frontier, point);
        const frame = newFrame(null);
        breakableStack.push(frame);
        let fallthrough = [];
        let hasDefault = false;
        for (const switchCase of statement.cases) {
          // The discriminant is not evaluated: any case may match, and a
          // completed case falls through into the next one.
          let entry = [...fallthrough, point];
          if (switchCase.test) {
            const testPoint = newPoint();
            registerExpression(testPoint, switchCase.test);
            connect(entry, testPoint);
            entry = [testPoint];
          } else {
            hasDefault = true;
          }
          fallthrough = processStatements(switchCase.consequent, entry);
        }
        breakableStack.pop();
        const exits = [...frame.breaks, ...fallthrough];
        if (!hasDefault) {
          exits.push(point);
        }
        return exits;
      }
      case 'WhileStatement': {
        const head = newPoint();
        registerExpression(head, statement.test);
        connect(frontier, head);
        const frame = newFrame(head);
        breakableStack.push(frame);
        const bodyEnds = processStatement(statement.body, [head]);
        breakableStack.pop();
        connect(bodyEnds, head);
        // The loop may run zero times, so the head state also flows out.
        return [...frame.breaks, head];
      }
      case 'DoWhileStatement': {
        const head = newPoint();
        connect(frontier, head);
        const condition = newPoint();
        const frame = newFrame(condition);
        breakableStack.push(frame);
        const bodyEnds = processStatement(statement.body, [head]);
        breakableStack.pop();
        registerExpression(condition, statement.test);
        connect(bodyEnds, condition);
        connect([condition], head);
        // The body runs at least once, so only the post-body state flows out.
        return [...frame.breaks, condition];
      }
      case 'ForStatement': {
        if (statement.init) {
          if (statement.init.type === 'VariableDeclaration') {
            frontier = processStatement(statement.init, frontier);
          } else {
            const initPoint = newPoint();
            registerExpression(initPoint, statement.init);
            connect(frontier, initPoint);
            frontier = [initPoint];
          }
        }
        const head = newPoint();
        registerExpression(head, statement.test);
        connect(frontier, head);
        const update = newPoint();
        registerExpression(update, statement.update);
        const frame = newFrame(update);
        breakableStack.push(frame);
        const bodyEnds = processStatement(statement.body, [head]);
        breakableStack.pop();
        connect(bodyEnds, update);
        connect([update], head);
        return [...frame.breaks, head];
      }
      case 'ForInStatement':
      case 'ForOfStatement': {
        const right = newPoint();
        registerExpression(right, statement.right);
        connect(frontier, right);
        const head = newPoint();
        connect([right], head);
        // The loop variable receives unknown values from the iterated source.
        if (statement.left.type === 'VariableDeclaration') {
          for (const declarator of statement.left.declarations) {
            poisonPattern(head, declarator.id);
          }
        } else if (statement.left.type === 'Identifier') {
          const variable = resolveLocal(statement.left);
          if (variable) {
            head.poison.push(variable);
          }
        } else if (statement.left.type === 'ObjectPattern' || statement.left.type === 'ArrayPattern') {
          poisonPattern(head, statement.left);
        }
        const frame = newFrame(head);
        breakableStack.push(frame);
        const bodyEnds = processStatement(statement.body, [head]);
        breakableStack.pop();
        connect(bodyEnds, head);
        return [...frame.breaks, head];
      }
      case 'ReturnStatement':
      case 'ThrowStatement': {
        const point = newPoint();
        registerExpression(point, statement.argument);
        connect(frontier, point);
        if (statement.type === 'ThrowStatement' && throwStack.length > 0) {
          throwStack[throwStack.length - 1].push(point);
        }
        applyEscape({ kind: 'exit', frame: null, frontier: [point] });
        return [];
      }
      case 'BreakStatement':
      case 'ContinueStatement': {
        const isBreak = statement.type === 'BreakStatement';
        const label = statement.label && statement.label.name;
        for (let index = breakableStack.length - 1; index >= 0; index--) {
          const frame = breakableStack[index];
          if (label ? frame.labels.includes(label) && (isBreak || frame.continuePoint) : isBreak || frame.continuePoint) {
            applyEscape({ kind: isBreak ? 'break' : 'continue', frame, frontier });
            break;
          }
        }
        return [];
      }
      case 'LabeledStatement': {
        const body = statement.body;
        if (body.type === 'WhileStatement' || body.type === 'DoWhileStatement' || body.type === 'ForStatement' || body.type === 'ForInStatement' || body.type === 'ForOfStatement' || body.type === 'SwitchStatement') {
          pendingLabels.push(statement.label.name);
          const exits = processStatement(body, frontier);
          const index = pendingLabels.indexOf(statement.label.name);
          if (index !== -1) {
            pendingLabels.splice(index, 1);
          }
          return exits;
        }
        // A labeled block can be the target of `break label`.
        const frame = { breaks: [], continuePoint: null, labels: [statement.label.name] };
        breakableStack.push(frame);
        const bodyEnds = processStatement(body, frontier);
        breakableStack.pop();
        return [...frame.breaks, ...bodyEnds];
      }
      case 'TryStatement': {
        mayWriteDepth++;
        const pendingFinally = statement.finalizer ? { escapes: [] } : null;
        if (pendingFinally) {
          finallyStack.push(pendingFinally);
        }
        const thrown = [];
        if (statement.handler) {
          throwStack.push(thrown);
        }
        const blockEnds = processStatement(statement.block, frontier);
        if (statement.handler) {
          throwStack.pop();
        }
        let handlerEnds = [];
        if (statement.handler) {
          // An exception may occur at any point of the try block.
          let handlerEntry = [...frontier, ...thrown];
          if (statement.handler.param) {
            const paramPoint = newPoint();
            poisonPattern(paramPoint, statement.handler.param);
            connect(handlerEntry, paramPoint);
            handlerEntry = [paramPoint];
          }
          handlerEnds = processStatement(statement.handler.body, handlerEntry);
        }
        if (pendingFinally) {
          finallyStack.pop();
        }
        const normalEnds = [...blockEnds, ...handlerEnds];
        if (statement.finalizer) {
          const completed = processStatement(statement.finalizer, normalEnds);
          if (pendingFinally.escapes.length > 0) {
            // Escaping paths (break/continue/return/throw) also run the
            // finally block before continuing to their target. The escape
            // frontiers are merged into a single pass so that nested
            // finally blocks cannot multiply without bound; every escape
            // target then receives the merged post-finally state.
            const escapeEnds = processStatement(
              statement.finalizer,
              pendingFinally.escapes.flatMap((escape) => escape.frontier)
            );
            for (const escape of pendingFinally.escapes) {
              applyEscape({ kind: escape.kind, frame: escape.frame, frontier: escapeEnds });
            }
          }
          mayWriteDepth--;
          return completed;
        }
        mayWriteDepth--;
        return normalEnds;
      }
      case 'WithStatement': {
        const point = newPoint();
        registerExpression(point, statement.object);
        connect(frontier, point);
        mayWriteDepth++;
        const exits = processStatement(statement.body, [point]);
        mayWriteDepth--;
        return exits;
      }
      case 'ClassDeclaration': {
        if (statement.superClass) {
          const point = newPoint();
          registerExpression(point, statement.superClass);
          connect(frontier, point);
          return [point];
        }
        return frontier;
      }
      case 'ExportNamedDeclaration':
      case 'ExportDefaultDeclaration': {
        if (statement.declaration) {
          if (statement.declaration.type.endsWith('Declaration')) {
            return processStatement(statement.declaration, frontier);
          }
          const point = newPoint();
          registerExpression(point, statement.declaration);
          connect(frontier, point);
          return [point];
        }
        return frontier;
      }
      default:
        return frontier;
    }
  }

  const entry = newPoint();
  if (funcNode.type === 'Program' || funcNode.type === 'StaticBlock') {
    processStatements(funcNode.body, [entry]);
  } else if (funcNode.body.type === 'BlockStatement') {
    processStatements(funcNode.body.body, [entry]);
  } else {
    // An arrow function with an expression body.
    const point = newPoint();
    registerExpression(point, funcNode.body);
    connect([entry], point);
  }
  return { points, entry };
}

/**
 * Collects the RegExp constructor calls directly contained in a function-like
 * node (nested functions are not entered).
 * @param {import("eslint").SourceCode} sourceCode
 * @param {import("estree").Node} funcNode
 * @returns {Array<{node: import("estree").Node, arg: import("estree").Node}>}
 */
function collectRegExpCalls(sourceCode, funcNode) {
  const calls = [];
  const visitorKeys = sourceCode.visitorKeys;
  const stack = funcNode.type === 'Program' || funcNode.type === 'StaticBlock' ? [...funcNode.body] : [funcNode.body];
  while (stack.length > 0) {
    const node = stack.pop();
    if (!node || typeof node.type !== 'string') {
      continue;
    }
    if (node !== funcNode.body && FUNCTION_LIKE_TYPES.has(node.type)) {
      continue;
    }
    if ((node.type === 'CallExpression' || node.type === 'NewExpression') && node.callee.type === 'Identifier' && node.callee.name === 'RegExp' && node.arguments.length > 0) {
      calls.push({ node, arg: node.arguments[0] });
    }
    for (const key of visitorKeys[node.type] || []) {
      const child = node[key];
      if (Array.isArray(child)) {
        for (const item of child) {
          if (item && typeof item.type === 'string') {
            stack.push(item);
          }
        }
      } else if (child && typeof child.type === 'string') {
        stack.push(child);
      }
    }
  }
  return calls;
}

/**
 * Analyzes one function-like node and records the value of the first argument
 * of every RegExp constructor call it directly contains.
 * @param {import("eslint").SourceCode} sourceCode
 * @param {import("estree").Node} funcNode
 * @param {ReturnType<typeof collectFunctionInfo>} info
 * @param {Map<import("estree").Node, PatternValue | null>} results
 */
function analyzeFunction(sourceCode, funcNode, info, results) {
  let graph;
  try {
    graph = buildControlFlowGraph(sourceCode, funcNode, info);
  } catch (error) {
    if (!(error instanceof AnalysisBudgetError)) {
      throw error;
    }
    graph = null;
  }

  /**
   * Reads an identifier under the given state. Bindings that are not tracked
   * locals of this function (outer function variables, globals) are unknown;
   * they keep their static meaning when `isStaticExpression` recognizes them.
   * @param {import("estree").Identifier} node
   * @param {VariableState | null} state `null` when the point is unreachable.
   * @returns {PatternValue}
   */
  function readIdentifier(node, state) {
    const scope = sourceCode.getScope(node);
    const variable = findVariable(scope, node.name);
    if (state !== null && variable && info.locals.has(variable)) {
      if (info.evalPoisoned || info.contaminated.has(variable)) {
        return DYNAMIC;
      }
      return state.get(variable) || DYNAMIC;
    }
    return isStaticExpression({ node, scope }) ? STATIC_UNKNOWN : DYNAMIC;
  }

  /**
   * Evaluates an expression to a pattern value under the given state.
   * @param {import("estree").Node} node
   * @param {VariableState | null} state
   * @returns {PatternValue}
   */
  function evaluate(node, state) {
    switch (node.type) {
      case 'Literal':
        return typeof node.value === 'string' ? knownString(node.value) : STATIC_UNKNOWN;
      case 'TemplateLiteral': {
        let result = knownString('');
        for (let index = 0; index < node.quasis.length; index++) {
          const cooked = node.quasis[index].value.cooked;
          result = concatValues(result, cooked === null ? STATIC_UNKNOWN : knownString(cooked));
          if (index < node.expressions.length) {
            result = concatValues(result, evaluate(node.expressions[index], state));
          }
        }
        return result;
      }
      case 'BinaryExpression': {
        const left = evaluate(node.left, state);
        const right = evaluate(node.right, state);
        if (node.operator === '+') {
          return concatValues(left, right);
        }
        return left.dynamic || right.dynamic ? DYNAMIC : STATIC_UNKNOWN;
      }
      case 'Identifier':
        return readIdentifier(node, state);
      default:
        // Everything else (calls, member access, `await`, ...) is unknown;
        // recognized static expressions keep their static meaning.
        return isStaticExpression({ node, scope: sourceCode.getScope(node) }) ? STATIC_UNKNOWN : DYNAMIC;
    }
  }

  /**
   * Records the value of one RegExp constructor call, unless `RegExp` is
   * shadowed by a parameter or local binding there.
   * @param {import("estree").Node} node The call node.
   * @param {import("estree").Node} arg The first argument.
   * @param {VariableState | null} mid The state at the call (`null` falls back to the syntactic behavior).
   */
  function recordObservation(node, arg, mid) {
    const scope = sourceCode.getScope(node);
    const regExpVariable = findVariable(scope, 'RegExp');
    if (regExpVariable && regExpVariable.defs.length > 0) {
      // A parameter or local binding shadows the RegExp constructor.
      results.set(node, null);
      return;
    }
    const value = evaluate(arg, mid);
    const previous = results.get(node);
    results.set(node, previous === undefined || previous === null ? value : mergeValues(previous, value));
  }

  if (graph === null) {
    // The control-flow graph exceeded the point budget: degrade to the
    // syntactic behavior for this function instead of the flow analysis.
    for (const { node, arg } of collectRegExpCalls(sourceCode, funcNode)) {
      recordObservation(node, arg, null);
    }
    return;
  }

  const { points, entry } = graph;

  /**
   * Applies the unknown writes of a point to a state.
   * @param {Object} point
   * @param {VariableState} state
   * @returns {VariableState}
   */
  function applyPoison(point, state) {
    if (point.poison.length === 0) {
      return state;
    }
    const poisoned = new Map(state);
    for (const variable of point.poison) {
      poisoned.set(variable, DYNAMIC);
    }
    return poisoned;
  }

  /**
   * Computes the output state of a point: unsupported writes poison first,
   * then supported writes read the old state and overwrite (or merge, inside
   * try/catch/finally/with where a write may not complete).
   * @param {Object} point
   * @param {VariableState} state
   * @returns {VariableState}
   */
  function transfer(point, state) {
    const mid = applyPoison(point, state);
    if (point.writes.length === 0) {
      return mid;
    }
    const out = new Map(mid);
    for (const write of point.writes) {
      let value = write.value || evaluate(write.rhs, mid);
      if (write.kind === 'concat') {
        value = concatValues(mid.get(write.variable) || DYNAMIC, value);
      }
      if (point.mayWrite) {
        value = mergeValues(mid.get(write.variable) || DYNAMIC, value);
      }
      out.set(write.variable, value);
    }
    return out;
  }

  // Fixpoint computation. Every transfer function is monotonic and the value
  // lattice has finite height (the budgets collapse large values to a sticky
  // unknown), so the worklist iteration terminates at the least fixpoint.
  const inStates = new Array(points.length).fill(null);
  const outStates = new Array(points.length).fill(null);
  const initial = new Map();
  for (const variable of info.locals) {
    initial.set(variable, DYNAMIC);
  }
  inStates[entry.id] = initial;
  const queue = [entry];
  const queued = new Set([entry]);
  while (queue.length > 0) {
    const point = queue.shift();
    queued.delete(point);
    const inState = inStates[point.id];
    const outState = inState === null ? null : transfer(point, inState);
    if (statesEqual(outState, outStates[point.id])) {
      continue;
    }
    outStates[point.id] = outState;
    for (const successor of point.succs) {
      const merged = mergeStates(inStates[successor.id], outState);
      if (!statesEqual(merged, inStates[successor.id])) {
        inStates[successor.id] = merged;
        if (!queued.has(successor)) {
          queued.add(successor);
          queue.push(successor);
        }
      }
    }
  }

  // Observe the RegExp constructor calls with the stable state of their
  // point. A call inside a `finally` is observed once per processed copy, so
  // duplicate observations of the same node are merged.
  for (const point of points) {
    if (point.observes.length === 0) {
      continue;
    }
    const inState = inStates[point.id];
    const mid = inState === null ? null : applyPoison(point, inState);
    for (const { node, arg } of point.observes) {
      recordObservation(node, arg, mid);
    }
  }
}

/**
 * Computes the value of the first argument of every `RegExp(...)` and
 * `new RegExp(...)` call in the file. Calls where `RegExp` is shadowed by a
 * parameter or local binding map to `null`.
 * @param {import("eslint").SourceCode} sourceCode
 * @returns {Map<import("estree").Node, PatternValue | null>}
 */
function analyze(sourceCode) {
  const results = new Map();
  for (const funcNode of collectFunctionLikeNodes(sourceCode)) {
    analyzeFunction(sourceCode, funcNode, collectFunctionInfo(sourceCode, funcNode), results);
  }
  return results;
}

/** Per-file analysis cache, keyed by the AST so separate lint runs never share state. */
const cache = new WeakMap();

/**
 * @param {import("eslint").SourceCode} sourceCode
 * @returns {Map<import("estree").Node, PatternValue | null>} The argument value per RegExp constructor call (`null` when `RegExp` is shadowed).
 */
function getRegExpCallValues(sourceCode) {
  let results = cache.get(sourceCode.ast);
  if (!results) {
    results = analyze(sourceCode);
    cache.set(sourceCode.ast, results);
  }
  return results;
}

module.exports = { getRegExpCallValues };
