/**
 * Check if the regex is evil or not using the safe-regex module
 * @author Adam Baldwin
 */

'use strict';

//-----------------------------------------------------------------------------
// Requirements
//-----------------------------------------------------------------------------

const safe = require('safe-regex');
const { getRegExpCallValues } = require('../utils/regexp-flow');

//------------------------------------------------------------------------------
// Rule Definition
//------------------------------------------------------------------------------

module.exports = {
  meta: {
    type: 'problem',
    docs: {
      description: 'Detects potentially unsafe regular expressions, which may take a very long time to run, blocking the event loop.',
      category: 'Possible Security Vulnerability',
      recommended: true,
      url: 'https://github.com/eslint-community/eslint-plugin-security/blob/main/docs/rules/detect-unsafe-regex.md',
    },
  },
  create(context) {
    const sourceCode = context.sourceCode || context.getSourceCode();
    return {
      Literal: function (node) {
        const token = sourceCode.getTokens(node)[0];
        const nodeType = token.type;
        const nodeValue = token.value;

        if (nodeType === 'RegularExpression') {
          if (!safe(nodeValue)) {
            context.report({ node: node, message: 'Unsafe Regular Expression' });
          }
        }
      },
      // `RegExp(pattern)` without `new` creates a regular expression too
      'NewExpression, CallExpression': function (node) {
        if (node.callee.type !== 'Identifier' || node.callee.name !== 'RegExp' || !node.arguments || node.arguments.length === 0) {
          return;
        }
        const value = getRegExpCallValues(sourceCode).get(node);
        if (value === null) {
          // `RegExp` is shadowed by a parameter or local binding.
          return;
        }
        const firstArgument = node.arguments[0];
        if (firstArgument.type === 'Literal') {
          if (!safe(firstArgument.value)) {
            context.report({ node: node, message: 'Unsafe Regular Expression (new RegExp)' });
          }
          return;
        }
        // Non-literal argument: report when a known candidate pattern of the
        // flow-sensitive value at this call site is unsafe.
        if (value && value.strings.some((pattern) => !safe(pattern))) {
          context.report({ node: node, message: 'Unsafe Regular Expression (new RegExp)' });
        }
      },
    };
  },
};
