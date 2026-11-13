var Xray = require('../..')

// Filters may return plain values, promises or thenables.
// Each filter in a chain receives the settled value of the
// previous one, and asynchronous filters can be used anywhere
// a selector is allowed, including `paginate` and `a@href`
// detail selectors.
var x = Xray({
  filters: {
    lookup: function (value) {
      return new Promise(function (resolve) {
        setTimeout(function () {
          var map = {
            '/a': 'http://example.com/a',
            '/b': 'http://example.com/b'
          }
          resolve(map[value] || value)
        }, 10)
      })
    },
    shout: function (value) {
      return Promise.resolve(String(value).toUpperCase())
    }
  }
})

var html = '<ul>' +
  '<li><a href="/a">alpha</a></li>' +
  '<li><a href="/b">beta</a></li>' +
  '</ul>'

x(html, 'li', [{
  text: 'a | shout',
  href: 'a@href | lookup'
}])(function (err, arr) {
  if (err) throw err
  console.log(arr)
  // => [ { text: 'ALPHA', href: 'http://example.com/a' },
  //      { text: 'BETA', href: 'http://example.com/b' } ]
})
