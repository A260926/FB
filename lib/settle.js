'use strict'

/**
 * Export `settle`
 */

module.exports = settle

/**
 * Read the `then` member of `value` when it could
 * belong to a thenable. Anything else yields
 * `undefined` without touching the value.
 *
 * @param {Mixed} value
 * @return {Function|Undefined}
 */

function thenOf (value) {
  if (value !== null && (typeof value === 'object' || typeof value === 'function')) {
    return value.then
  }
}

/**
 * Adopt a thenable into a native Promise. `then` is
 * read once by the caller and invoked once here, so
 * foreign thenables settle exactly once and native
 * promises fully unwrap any nested thenables.
 *
 * @param {Mixed} thenable
 * @param {Function} then
 * @return {Promise}
 */

function adopt (thenable, then) {
  return new Promise(function (resolve, reject) {
    then.call(thenable, resolve, reject)
  })
}

/**
 * Continue with a maybe-thenable `value`. Plain values
 * reach `onValue` synchronously, keeping synchronous
 * filter chains synchronous. Thenables are adopted once
 * and settle `onValue` or `onError` asynchronously. A
 * throwing `then` getter is routed to `onError`.
 *
 * @param {Mixed} value
 * @param {Function} onValue
 * @param {Function} onError
 */

function settle (value, onValue, onError) {
  var then
  try {
    then = thenOf(value)
  } catch (err) {
    return onError(err)
  }
  if (typeof then !== 'function') return onValue(value)
  adopt(value, then).then(onValue, onError)
}

settle.thenOf = thenOf
settle.adopt = adopt
