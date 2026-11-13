/**
 * Module Dependencies
 */

var debug = require('debug')('resolve')
var isArray = require('./util').isArray
var settle = require('./settle')
var parse = require('x-ray-parse')

var thenOf = settle.thenOf
var adopt = settle.adopt

/**
 * Export `resolve`
 */

module.exports = resolve

/**
 * Initialize `resolve`
 *
 * Returns a plain value when every filter is
 * synchronous, otherwise a native Promise for
 * the settled value.
 *
 * @param {$} cheerio object
 * @param {String} scope
 * @param {String|Array} selector
 * @param {Object} filters
 * @return {Array|String|Promise}
 */

function resolve ($, scope, selector, filters) {
  debug('resolve($j, %j)', scope, selector)
  filters = filters || {}
  var array = isArray(selector)
  var obj = parse(array ? selector[0] : selector)
  obj.attribute = obj.attribute || 'text'

  if (!obj.selector) {
    obj.selector = scope
    scope = null
  }

  var value = find($, scope, array ? [obj.selector] : obj.selector, obj.attribute)
  debug('resolved($j, %j) => %j', scope, selector, value)

  if (array && typeof value.map === 'function') {
    var values = value.map(function (v) {
      return filter(obj, $, scope, selector, v, filters)
    })
    return someThenable(values) ? Promise.all(values) : values
  } else {
    return filter(obj, $, scope, selector, value, filters)
  }
}

/**
 * Check if any value is thenable. Values come from
 * `filter`, so they are either plain or native
 * promises already adopted exactly once.
 *
 * @param {Array} values
 * @return {Boolean}
 */

function someThenable (values) {
  for (var i = 0; i < values.length; i++) {
    if (typeof thenOf(values[i]) === 'function') return true
  }
  return false
}

/**
 * Find the node(s)
 *
 * @param {Cheerio} $
 * @param {String} scope
 * @param {String|Array} selector
 * @param {String} attr
 * @return {Array|String}
 */

function find ($, scope, selector, attr) {
  if (scope && isArray(selector)) {
    var $scope = select($, scope)
    var out = []
    $scope.map(function (i) {
      var $el = $scope.eq(i)
      var $children = select($el, selector[0])
      $children.map(function (i) {
        out.push(attribute($children.eq(i), attr))
      })
    })
    return out
  } else if (scope) {
    $scope = select($, scope)
    return attribute($scope.find(selector).eq(0), attr)
  } else {
    var $selector
    if (isArray(selector)) {
      $selector = select($, selector[0])
      out = []
      $selector.map(function (i) {
        out.push(attribute($selector.eq(i), attr))
      })
      return out
    } else {
      $selector = select($, selector)
      return attribute($selector.eq(0), attr)
    }
  }
}

/**
 * Selector abstraction, deals
 * with various instances of $
 *
 * @param {Cheerio} $
 * @param {String} selector
 * @return {Cheerio}
 */

function select ($, selector) {
  if ($.is && $.is(selector)) return $
  return $.find ? $.find(selector) : $(selector)
}

/**
 * Select the attribute based on `attr`
 *
 * @param {Cheerio} $
 * @param {String} attr
 * @return {String}
 */

function attribute ($el, attr) {
  switch (attr) {
    case 'html':
      return $el.html()
    case 'text':
      return $el.text()
    default:
      return $el.attr(attr)
  }
}

/**
 * Filter the value(s)
 *
 * Filters run in declaration order and each filter
 * receives the settled value of the previous one.
 * Filters returning plain values keep the chain
 * synchronous; a promise or thenable is adopted
 * exactly once and the rest of the chain continues
 * asynchronously. The result is either a plain value
 * or a native Promise, never a bare thenable.
 *
 * @param {Object} obj
 * @param {Cheerio} $
 * @param {String} scope
 * @param {String|Array} selector
 * @param {Object} filters
 * @return {Array|String|Promise}
 */

function filter (obj, $, scope, selector, value, filters) {
  var ctx = { $: $, selector: obj.selector, attribute: obj.attribute }
  var list = obj.filters || []

  return step(0, value)

  function step (i, input) {
    var out = input
    while (i < list.length) {
      var current = list[i++]
      var fn = filters[current.name]
      if (typeof fn !== 'function') {
        throw new Error('Invalid filter: ' + current.name)
      }
      var args = [out].concat(current.args || [])
      out = fn.apply(ctx, args)
      debug('%s.apply(ctx, %j) => %j', current.name, args, out)
      var then = thenOf(out)
      if (typeof then === 'function') {
        return adopt(out, then).then(function (value) {
          return step(i, value)
        })
      }
    }
    return out
  }
}
