# Detects potentially unsafe regular expressions, which may take a very long time to run, blocking the event loop (`security/detect-unsafe-regex`)

⚠️ This rule _warns_ in the ✅ `recommended` config.

<!-- end auto-generated rule header -->

More information: [Regular Expression DoS and Node.js](../regular-expression-dos-and-node.md)

This rule resolves the pattern of `RegExp(...)` / `new RegExp(...)` calls through a flow-sensitive value analysis; see [Flow-sensitive RegExp constructor checks](../regexp-flow.md).
