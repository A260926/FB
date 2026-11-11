const EventEmitter = require("node:events");
const util = require("node:util");
const { removeQuotes, symbols } = require("../lib/util");

const eventHandler = new EventEmitter();
const descEvent = new EventEmitter();

eventHandler.on("uncaughtException", (err) => {
  console.error(err);
});

descEvent.on("success", (desc) => {
  if (
    process.env.TAIKO_ENABLE_ACTION_OUTPUT &&
    process.env.TAIKO_ENABLE_ACTION_OUTPUT.toLowerCase() === "true"
  ) {
    const outputDesc = removeQuotes(
      util.inspect(symbols.pass + desc, { colors: true }),
      symbols.pass + desc,
    );
    console.log(outputDesc);
  }
});

/*'createdSession' is a valid event that needs more than 10 listeners as all handlers
have to listen to it */
eventHandler.setMaxListeners(100);

// One-shot registrations for javascript dialogs (alert/prompt/confirm).
// Each call to the public dialog APIs pushes an independent record; a single
// javascriptDialogOpening event is dispatched to exactly one record.
const dialogRegistrations = {
  alert: [],
  prompt: [],
  confirm: [],
};

const isDialogType = (type) =>
  Object.prototype.hasOwnProperty.call(dialogRegistrations, type);

// String messages keep the historical new RegExp(message) semantics. RegExp
// objects keep their flags, but matching must never mutate the caller's
// lastIndex, so global/sticky patterns cannot skip a legitimate match.
const dialogMessageMatches = (matcher, message) => {
  if (typeof matcher === "string") {
    return new RegExp(matcher).test(message);
  }
  const sticky = matcher.global || matcher.sticky;
  if (!sticky) {
    return matcher.test(message);
  }
  const savedLastIndex = matcher.lastIndex;
  try {
    matcher.lastIndex = 0;
    return matcher.test(message);
  } finally {
    matcher.lastIndex = savedLastIndex;
  }
};

// Records carrying a matcher win over callback-only fallbacks regardless of
// registration order; within each group the earliest registration is used.
const pickDialogRegistration = (type, message) => {
  const registrations = dialogRegistrations[type] || [];
  let fallback = null;
  for (const registration of registrations) {
    if (registration.matcher === null) {
      fallback = fallback || registration;
      continue;
    }
    if (dialogMessageMatches(registration.matcher, message)) {
      return registration;
    }
  }
  return fallback;
};

const addDialogRegistration = (type, matcher, callback) => {
  dialogRegistrations[type].push({ type, matcher, callback });
};

const removeDialogRegistration = (registration) => {
  const registrations = dialogRegistrations[registration.type];
  if (!registrations) {
    return;
  }
  const index = registrations.indexOf(registration);
  if (index !== -1) {
    registrations.splice(index, 1);
  }
};

const clearDialogRegistrations = () => {
  for (const registrations of Object.values(dialogRegistrations)) {
    registrations.length = 0;
  }
};

module.exports = {
  eventHandler,
  descEvent,
  dialogRegistrations,
  isDialogType,
  pickDialogRegistration,
  addDialogRegistration,
  removeDialogRegistration,
  clearDialogRegistrations,
};
