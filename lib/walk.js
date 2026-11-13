/**
 * Module Dependencies
 */

var isObject = require('./util').isObject
var Batch = require('batch')

var has = Object.prototype.hasOwnProperty

/**
 * Walk
 */

module.exports = walk

/**
 * Walk recursively, providing
 * callbacks for each step.
 *
 * Object keys are assembled in declaration
 * order, no matter which order asynchronous
 * steps settle in.
 *
 * @param {Mixed} value
 * @param {Function} fn
 * @param {Function} done
 * @param {String} key (private)
 */

function walk (value, fn, done, key) {
  var batch = Batch()
  var out
  var keys
  var resolved

  if (isObject(value)) {
    keys = Object.keys(value)
    resolved = {}
    keys.forEach(function (k) {
      var v = value[k]
      batch.push(function (next) {
        walk(v, fn, function (err, value) {
          if (err) return next(err)
          // ignore undefined values
          if (undefined !== value && value !== '') {
            resolved[k] = value
          }
          next()
        }, k)
      })
    })
  } else {
    out = null
    batch.push(function (next) {
      fn(value, key, function (err, v) {
        if (err) return next(err)
        out = v
        next()
      })
    })
  }

  batch.end(function (err) {
    if (err) return done(err)
    if (keys) {
      out = {}
      keys.forEach(function (k) {
        if (has.call(resolved, k)) out[k] = resolved[k]
      })
    }
    return done(null, out)
  })
}
