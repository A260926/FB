# Detects "RegExp(variable)", which might allow an attacker to DOS your server with a long-running regular expression (`security/detect-non-literal-regexp`)

⚠️ This rule _warns_ in the ✅ `recommended` config.

<!-- end auto-generated rule header -->

More information: [Regular Expression DoS and Node.js](../regular-expression-dos-and-node.md)

This rule resolves the argument of `RegExp(...)` / `new RegExp(...)` calls through a flow-sensitive value analysis; see [Flow-sensitive RegExp constructor checks](../regexp-flow.md).
