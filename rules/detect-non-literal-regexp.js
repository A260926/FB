/**
 * Tries to detect RegExp's created from non-literal strings.
 * @author Jon Lamendola
 */

'use strict';

const { isStaticExpression } = require('../utils/is-static-expression');
const { getRegExpCallValues } = require('../utils/regexp-flow');

//------------------------------------------------------------------------------
// Rule Definition
//------------------------------------------------------------------------------

module.exports = {
  meta: {
    type: 'problem',
    docs: {
      description: 'Detects "RegExp(variable)", which might allow an attacker to DOS your server with a long-running regular expression.',
      category: 'Possible Security Vulnerability',
      recommended: true,
      url: 'https://github.com/eslint-community/eslint-plugin-security/blob/main/docs/rules/detect-non-literal-regexp.md',
    },
  },
  create(context) {
    const sourceCode = context.sourceCode || context.getSourceCode();

    return {
      'NewExpression, CallExpression'(node) {
        // `RegExp(pattern)` without `new` creates a regular expression too
        if (node.callee.type !== 'Identifier' || node.callee.name !== 'RegExp') {
          return;
        }
        const args = node.arguments;
        if (!args || args.length === 0) {
          return;
        }
        const value = getRegExpCallValues(sourceCode).get(node);
        if (value === null) {
          // `RegExp` is shadowed by a parameter or local binding.
          return;
        }
        if (value === undefined) {
          // The call is not part of the flow analysis (e.g. in a computed
          // class key): keep the previous syntactic behavior.
          const scope = sourceCode.getScope ? sourceCode.getScope(node) : context.getScope();
          if (!isStaticExpression({ node: args[0], scope })) {
            return context.report({ node: node, message: 'Found non-literal argument to RegExp Constructor' });
          }
          return;
        }
        // Report when the flow-sensitive value at this call site may be
        // dynamic. Fully known patterns and recognized static expressions
        // (e.g. `path.join()`) are not reported.
        if (value.dynamic) {
          return context.report({ node: node, message: 'Found non-literal argument to RegExp Constructor' });
        }
      },
    };
  },
};
