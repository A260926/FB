const test = require("node:test");
const assert = require("node:assert/strict");
const rewire = require("rewire");

process.env.TAIKO_ENABLE_TEST_HOOKS = "1";

const taiko = require("../packages/taiko/lib/taiko");
const pageHandler = rewire("../packages/taiko/lib/handlers/pageHandler.js");
const {
  eventHandler,
  clearDialogRegistrations,
} = require("../packages/taiko/lib/eventBus");

let opening;
const wire = [];
const page = {
  javascriptDialogOpening(listener) {
    opening = listener;
  },
  async handleJavaScriptDialog(value) {
    wire.push(value);
  },
};

const revertPage = pageHandler.__set__("page", page);
taiko.__set__("validate", () => {});
taiko.__set__("pageHandler", pageHandler);
pageHandler.addJavascriptDialogOpeningListener();

const flush = () => new Promise((resolve) => setImmediate(resolve));

const unhandledRejections = [];
process.on("unhandledRejection", (reason) => {
  unhandledRejections.push(reason);
});

test.beforeEach(() => {
  clearDialogRegistrations();
  wire.length = 0;
  unhandledRejections.length = 0;
});

test.after(() => {
  revertPage();
});

test("one_registration", async () => {
  const calls = [];
  let promptDialog;

  taiko.confirm("Continue", async (dialog) => {
    calls.push(`first:${dialog.message}`);
    await taiko.accept();
  });
  taiko.confirm("Continue", async (dialog) => {
    calls.push(`second:${dialog.message}`);
    await taiko.accept();
  });

  opening({
    type: "confirm",
    message: "Continue",
    url: "https://local.invalid/form",
  });
  await flush();

  // Only one registration handles each popup, even though both share a message.
  assert.deepEqual(calls, ["first:Continue"]);
  assert.equal(wire.length, 1);
  assert.deepEqual(wire[0], { accept: true, promptText: "" });

  opening({
    type: "confirm",
    message: "Continue",
    url: "https://local.invalid/form",
  });
  await flush();

  assert.deepEqual(calls, ["first:Continue", "second:Continue"]);
  assert.equal(wire.length, 2);
  assert.ok(wire.every((entry) => entry.accept === true));

  // The original popup payload reaches the callback unchanged.
  taiko.prompt("Name", async (dialog) => {
    promptDialog = dialog;
    await taiko.accept("Ada");
  });
  opening({
    type: "prompt",
    message: "Name",
    defaultPrompt: "Harry Potter",
    url: "https://local.invalid/name",
  });
  await flush();

  assert.deepEqual(promptDialog, {
    type: "prompt",
    message: "Name",
    defaultPrompt: "Harry Potter",
    url: "https://local.invalid/name",
  });
  assert.equal(wire.length, 3);
  assert.deepEqual(wire[2], { accept: true, promptText: "Ada" });
});

test("typed_priority", async () => {
  const calls = [];

  // The callback-only fallback is registered first, but a later specific
  // message registration must take precedence over it.
  taiko.confirm(async (dialog) => {
    calls.push(`confirm-fallback:${dialog.message}`);
    await taiko.dismiss();
  });
  taiko.confirm("Continue", async (dialog) => {
    calls.push(`confirm-matcher:${dialog.message}`);
    await taiko.accept();
  });
  taiko.alert("Continue", async (dialog) => {
    calls.push(`alert-matcher:${dialog.message}`);
    await taiko.accept();
  });
  taiko.alert(async (dialog) => {
    calls.push(`alert-fallback:${dialog.message}`);
    await taiko.dismiss();
  });

  opening({
    type: "confirm",
    message: "Continue",
    url: "https://local.invalid/confirm",
  });
  await flush();

  assert.deepEqual(calls, ["confirm-matcher:Continue"]);
  assert.equal(wire.length, 1);
  assert.deepEqual(wire[0], { accept: true, promptText: "" });

  // Type isolation: the confirm dispatch did not consume the alert records.
  opening({
    type: "alert",
    message: "Continue",
    url: "https://local.invalid/alert",
  });
  await flush();

  assert.deepEqual(calls, [
    "confirm-matcher:Continue",
    "alert-matcher:Continue",
  ]);
  assert.equal(wire.length, 2);
  assert.deepEqual(wire[1], { accept: true, promptText: "" });

  // The fallback remains available for a message no specific matcher covers.
  opening({
    type: "confirm",
    message: "Discard?",
    url: "https://local.invalid/confirm",
  });
  await flush();

  assert.deepEqual(calls, [
    "confirm-matcher:Continue",
    "alert-matcher:Continue",
    "confirm-fallback:Discard?",
  ]);
  assert.equal(wire.length, 3);
  assert.deepEqual(wire[2], { accept: false, promptText: undefined });

  // The alert fallback was retained even while a specific alert matcher existed.
  opening({
    type: "alert",
    message: "Later",
    url: "https://local.invalid/alert",
  });
  await flush();

  assert.deepEqual(calls, [
    "confirm-matcher:Continue",
    "alert-matcher:Continue",
    "confirm-fallback:Discard?",
    "alert-fallback:Later",
  ]);
  assert.equal(wire.length, 4);
  assert.deepEqual(wire[3], { accept: false, promptText: undefined });

  // With every confirm record consumed, the unregistered-popup error remains.
  assert.throws(
    () =>
      opening({
        type: "confirm",
        message: "Unknown",
        url: "https://local.invalid/unknown",
      }),
    /There is no handler registered for confirm popup displayed on the page https:\/\/local\.invalid\/unknown/,
  );
});

test("reentrant_consumption", async () => {
  const calls = [];

  taiko.confirm("Continue", async (dialog) => {
    calls.push(`outer:${dialog.message}`);
    await taiko.accept();
    // Synchronous re-entry with the same message must select the next record,
    // not invoke the record currently handling this popup again.
    opening({
      type: "confirm",
      message: "Continue",
      url: "https://local.invalid/reentrant",
    });
  });
  taiko.confirm("Continue", async (dialog) => {
    calls.push(`inner:${dialog.message}`);
    await taiko.accept();
  });

  opening({
    type: "confirm",
    message: "Continue",
    url: "https://local.invalid/reentrant",
  });
  await flush();

  assert.deepEqual(calls, ["outer:Continue", "inner:Continue"]);
  assert.equal(wire.length, 2);
  assert.ok(wire.every((entry) => entry.accept === true));

  // Registrations made from inside a callback are appended after the records
  // that were already pending.
  taiko.confirm("First", async (dialog) => {
    calls.push(`first:${dialog.message}`);
    taiko.confirm("Second", async (dialogDuringCallback) => {
      calls.push(`registered-in-callback:${dialogDuringCallback.message}`);
      await taiko.accept();
    });
    await taiko.accept();
  });
  taiko.confirm("First", async (dialog) => {
    calls.push(`second:${dialog.message}`);
    await taiko.accept();
  });

  opening({
    type: "confirm",
    message: "First",
    url: "https://local.invalid/order",
  });
  await flush();
  assert.deepEqual(calls, ["outer:Continue", "inner:Continue", "first:First"]);
  assert.equal(wire.length, 3);

  opening({
    type: "confirm",
    message: "First",
    url: "https://local.invalid/order",
  });
  await flush();
  assert.deepEqual(calls, [
    "outer:Continue",
    "inner:Continue",
    "first:First",
    "second:First",
  ]);
  assert.equal(wire.length, 4);

  opening({
    type: "confirm",
    message: "Second",
    url: "https://local.invalid/order",
  });
  await flush();
  assert.deepEqual(calls, [
    "outer:Continue",
    "inner:Continue",
    "first:First",
    "second:First",
    "registered-in-callback:Second",
  ]);
  assert.equal(wire.length, 5);

  assert.throws(
    () =>
      opening({
        type: "confirm",
        message: "Second",
        url: "https://local.invalid/order",
      }),
    /There is no handler registered for confirm popup/,
  );
});

test("regex_state", async () => {
  const calls = [];
  const globalPattern = /^Continue/g;
  const stickyPattern = /^Continue/y;
  globalPattern.lastIndex = 3;
  stickyPattern.lastIndex = 4;

  taiko.confirm(globalPattern, async (dialog) => {
    calls.push(`global:${dialog.message}`);
    await taiko.accept();
  });
  taiko.confirm(stickyPattern, async (dialog) => {
    calls.push(`sticky:${dialog.message}`);
    await taiko.dismiss();
  });
  taiko.confirm("Something else", async (dialog) => {
    calls.push(`string:${dialog.message}`);
    await taiko.dismiss();
  });

  // This popup makes the dispatcher test the global/sticky patterns against a
  // non-matching message before their own messages are seen below.
  opening({
    type: "confirm",
    message: "Something else",
    url: "https://local.invalid/regex",
  });
  await flush();

  assert.deepEqual(calls, ["string:Something else"]);
  assert.equal(globalPattern.lastIndex, 3);
  assert.equal(stickyPattern.lastIndex, 4);
  assert.equal(wire.length, 1);
  assert.deepEqual(wire[0], { accept: false, promptText: undefined });

  opening({
    type: "confirm",
    message: "Continue",
    url: "https://local.invalid/regex",
  });
  await flush();

  assert.deepEqual(calls, ["string:Something else", "global:Continue"]);
  assert.equal(globalPattern.lastIndex, 3);
  assert.equal(stickyPattern.lastIndex, 4);
  assert.equal(wire.length, 2);
  assert.deepEqual(wire[1], { accept: true, promptText: "" });

  opening({
    type: "confirm",
    message: "Continue",
    url: "https://local.invalid/regex",
  });
  await flush();

  assert.deepEqual(calls, [
    "string:Something else",
    "global:Continue",
    "sticky:Continue",
  ]);
  assert.equal(globalPattern.lastIndex, 3);
  assert.equal(stickyPattern.lastIndex, 4);
  assert.equal(wire.length, 3);
  assert.deepEqual(wire[2], { accept: false, promptText: undefined });

  // String messages keep the historical new RegExp(message) semantics: "?"
  // is a quantifier, so it matches a message without the literal question mark.
  taiko.confirm("Are you sure?", async (dialog) => {
    calls.push(`string-regex:${dialog.message}`);
    await taiko.accept();
  });
  opening({
    type: "confirm",
    message: "Are you sure",
    url: "https://local.invalid/regex",
  });
  await flush();

  assert.deepEqual(calls, [
    "string:Something else",
    "global:Continue",
    "sticky:Continue",
    "string-regex:Are you sure",
  ]);
  assert.equal(wire.length, 4);
  assert.deepEqual(wire[3], { accept: true, promptText: "" });
});

test("callback_error", async () => {
  const calls = [];
  const errors = [];
  const collectError = (error) => errors.push(error);
  const originalConsoleError = console.error;
  console.error = () => {};
  eventHandler.on("uncaughtException", collectError);

  try {
    taiko.confirm("Continue", () => {
      throw new Error("sync boom");
    });
    taiko.confirm("Continue", async () => {
      throw new Error("async boom");
    });
    taiko.confirm("Continue", async (dialog) => {
      calls.push(`survivor:${dialog.message}`);
      await taiko.accept();
    });

    opening({
      type: "confirm",
      message: "Continue",
      url: "https://local.invalid/error",
    });
    await flush();

    opening({
      type: "confirm",
      message: "Continue",
      url: "https://local.invalid/error",
    });
    await flush();

    opening({
      type: "confirm",
      message: "Continue",
      url: "https://local.invalid/error",
    });
    await flush();

    // A failing callback consumes exactly its own record; the remaining rules
    // stay usable and are not invoked as a fallback for the failure.
    assert.deepEqual(calls, ["survivor:Continue"]);
    assert.equal(wire.length, 1);
    assert.deepEqual(wire[0], { accept: true, promptText: "" });

    assert.equal(errors.length, 2);
    assert.equal(errors[0].message, "sync boom");
    assert.equal(errors[1].message, "async boom");

    await flush();
    assert.equal(unhandledRejections.length, 0);
  } finally {
    eventHandler.removeListener("uncaughtException", collectError);
    console.error = originalConsoleError;
  }
});
