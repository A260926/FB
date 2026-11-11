"use strict"

const cheerio = require("cheerio")
const req = require("cheerio-req")
const coreScrapeHTML = require("scrape-it-core")

/**
 * isPlainObject
 * Checks whether the value is a schema-like plain object (functions, arrays
 * and Cheerio instances are left untouched while walking schemas).
 *
 * @param {Anything} value The value to inspect.
 * @returns {Boolean} `true` for plain objects.
 */
function isPlainObject (value) {
    return value !== null && typeof value === "object" && !Array.isArray(value)
}

/**
 * schemaHasFollow
 * Walks a schema tree (frozen or reused schemas are only read, never mutated)
 * and reports whether any field declares a `follow` option.
 *
 * @param {Object} schema The scraping schema.
 * @returns {Boolean} `true` when at least one field follows a link.
 */
function schemaHasFollow (schema) {
    if (!isPlainObject(schema)) {
        return false
    }
    for (const key of Object.keys(schema)) {
        const value = schema[key]
        // A plain-object `follow` is the linked-extraction directive; any
        // other value (e.g. a string-shorthand field named "follow") is a
        // normal field.
        if (key === "follow" && isPlainObject(value)) {
            return true
        }
        if (schemaHasFollow(value)) {
            return true
        }
    }
    return false
}

/**
 * validateFollowSchema
 * Verifies follow-related configuration before any network request is made.
 * `how`/`texteq` cannot be combined with `follow`, and `follow.data` must be
 * a schema object.
 *
 * @param {Object} schema The scraping schema.
 * @param {String} path The field path accumulated while walking.
 */
function validateFollowSchema (schema, path) {
    if (!isPlainObject(schema)) {
        return
    }

    if (Object.prototype.hasOwnProperty.call(schema, "follow")
        && isPlainObject(schema.follow)) {
        const fieldPath = path || "<root>"
        if (!isPlainObject(schema.follow.data)) {
            const err = new Error(`The follow option of "${fieldPath}" needs a "data" schema object.`)
            err.code = "FOLLOW_CONFIG_INVALID"
            err.field = fieldPath
            throw err
        }
        if (Object.prototype.hasOwnProperty.call(schema, "how")) {
            const err = new Error(`The "how" option cannot be combined with "follow" on "${fieldPath}".`)
            err.code = "FOLLOW_CONFIG_INVALID"
            err.field = fieldPath
            throw err
        }
        if (Object.prototype.hasOwnProperty.call(schema, "texteq")) {
            const err = new Error(`The "texteq" option cannot be combined with "follow" on "${fieldPath}".`)
            err.code = "FOLLOW_CONFIG_INVALID"
            err.field = fieldPath
            throw err
        }
    }

    for (const key of Object.keys(schema)) {
        validateFollowSchema(schema[key], path ? `${path}.${key}` : key)
    }
}

/**
 * stripHash
 * Removes the fragment from an URL and returns the normalized href.
 *
 * @param {String} href An absolute URL.
 * @returns {String} The URL without its fragment.
 */
function stripHash (href) {
    const url = new URL(href)
    url.hash = ""
    return url.href
}

/**
 * finalUrlOf
 * Extracts the final response URL (after redirects) from an axios response.
 * The native Node response carries `responseUrl` thanks to `follow-redirects`;
 * the fallback covers environments where it is not exposed.
 *
 * @param {Object} res The axios response.
 * @param {String} fallbackHref The requested URL.
 * @returns {String} The final URL, fragment stripped.
 */
function finalUrlOf (res, fallbackHref) {
    const native = res && res.request
    const raw = (native && native.res && native.res.responseUrl)
        || (native && native._redirectable && native._redirectable._currentUrl)
        || fallbackHref
    try {
        return stripHash(raw)
    } catch (err) {
        return fallbackHref
    }
}

/**
 * rootRequestHref
 * Computes the href requested for the root page, regardless of whether the
 * caller passed a URL string or an axios request options object.
 *
 * @param {String|Object} url The page url or request options.
 * @returns {String} The normalized root request URL, fragment stripped.
 */
function rootRequestHref (url) {
    let raw = url
    if (typeof url !== "string") {
        try {
            raw = req.request.getUri(url)
        } catch (err) {
            raw = url && url.url || ""
        }
    }
    const parsed = new URL(raw, "http://localhost/")
    parsed.hash = ""
    return parsed.href
}

/**
 * hitsAncestorChain
 * Cycle guard: an href is a cycle only when it matches the request or final
 * response URL of a page on the current ancestor chain. URLs visited by
 * sibling records are not part of that chain.
 *
 * @param {String} href The URL to test.
 * @param {Array} ancestors The ancestor pages.
 * @returns {Boolean} `true` when the URL is an ancestor.
 */
function hitsAncestorChain (href, ancestors) {
    return ancestors.some(ancestor =>
        ancestor.requestHref === href || ancestor.finalHref === href
    )
}

/**
 * followError
 * Wraps any failure raised while following a link with the field path so the
 * caller can tell which record triggered it, while keeping the original error
 * available as `cause`.
 *
 * @param {Error} cause The original error.
 * @param {String} fieldPath The failing field path (e.g. `rows[1].detail`).
 * @returns {Error} The wrapped error.
 */
function followError (cause, fieldPath) {
    const err = new Error(`Failed to follow link at "${fieldPath}": ${cause.message}`)
    err.cause = cause
    err.code = cause.code || "FOLLOW_FAILED"
    err.field = fieldPath
    return err
}

/**
 * fetchDetail
 * Every detail page is fetched with a brand new GET request: the root request
 * options (including a possible POST body) are never inherited.
 *
 * @param {String} href The detail URL.
 * @returns {Object} The axios response, augmented with the Cheerio `$` loader.
 */
async function fetchDetail (href) {
    const res = await req.request({
        method: "GET"
      , url: href
    })
    res.$ = cheerio.load(res.data)
    return res
}

/**
 * extract
 * Recursively scrapes one page. It mirrors the semantics of `scrape-it-core`
 * (selector/eq/closest/attr/trimValue/how/convert, nested data and lists) and
 * additionally resolves `follow` fields by fetching the linked pages.
 *
 * Nothing is stored on the (possibly frozen or reused) schema objects, and no
 * state is shared between sibling records or independent `scrapeIt` calls.
 *
 * @param {Cheerio} $ The Cheerio loader of the current page.
 * @param {Cheerio|undefined} context The current scoped element.
 * @param {Object} schema The schema for this level.
 * @param {String} baseUrl The final response URL of this page.
 * @param {Array} ancestors Ancestor pages (`{ requestHref, finalHref }`).
 * @param {String} path The dotted/indexed field path for error messages.
 * @returns {Promise<Object>} The scraped data for this level.
 */
async function extract ($, context, schema, baseUrl, ancestors, path) {
    const pageData = {}

    for (const name of Object.keys(schema)) {
        const fieldPath = path ? `${path}.${name}` : name
        const opt = typeof schema[name] === "string"
            ? { selector: schema[name] }
            : schema[name]

        if (context === undefined && !opt.selector && !opt.listItem) {
            const err = new Error(
                `There is no element selected for the '${fieldPath}' field. Please provide a selector, list item or use nested object structure.`
            )
            err.code = "NO_ELEMENT_SELECTED"
            err.field = fieldPath
            throw err
        }

        let $elm = opt.selector ? $(opt.selector, context) : context

        // Handle lists, in DOM order
        if (opt.listItem) {
            const itemSchema = opt.data && Object.keys(opt.data).length
                ? opt.data
                : { ___raw: {} }
            const $items = $(opt.listItem, context)
            const docs = []

            for (let i = 0; i < $items.length; ++i) {
                const item = await extract(
                    $
                  , $items.eq(i)
                  , itemSchema
                  , baseUrl
                  , ancestors
                  , `${fieldPath}[${i}]`
                )
                docs.push(opt.convert ? opt.convert(item.___raw || item) : (item.___raw || item))
            }

            pageData[name] = docs
            continue
        }

        if (typeof opt.eq === "number") {
            $elm = $elm.eq(opt.eq)
        }

        let texteqValue
        let hasTexteq = false
        if (typeof opt.texteq === "number") {
            hasTexteq = true
            const children = $elm.contents()
            let textCounter = 0
            for (let i = 0; i < children.length; ++i) {
                const child = children[i]
                if (child.type === "text") {
                    if (textCounter === opt.texteq) {
                        texteqValue = child.data
                        break
                    }
                    textCounter++
                }
            }
        }

        if (opt.closest) {
            $elm = $elm.closest(opt.closest)
        }

        // Nested data objects
        if (opt.data && Object.keys(opt.data).length) {
            pageData[name] = await extract($, $elm, opt.data, baseUrl, ancestors, fieldPath)
            continue
        }

        // Linked extraction (a field merely named "follow" stays a normal field)
        if (isPlainObject(opt.follow) && isPlainObject(opt.follow.data)) {
            pageData[name] = await followLink($, opt, $elm, baseUrl, ancestors, fieldPath)
            continue
        }

        // Plain leaf value, keeping scrape-it-core semantics
        let value
        if (hasTexteq) {
            value = texteqValue
        } else if (opt.attr) {
            value = $elm.attr(opt.attr)
        } else {
            const how = opt.how === undefined ? "text" : opt.how
            value = typeof how === "function" ? how($elm) : $elm[how]()
        }

        value = value === undefined ? "" : value
        if (opt.trimValue !== false && typeof value === "string") {
            value = value.trim()
        }

        if (opt.convert) {
            value = opt.convert(value, $elm)
        }

        pageData[name] = value
    }

    return pageData
}

/**
 * followLink
 * Resolves one `follow` field: selects the first link node, resolves the href
 * against the current page's final URL, guards against ancestor cycles, fetches
 * the detail page and extracts it with the follow schema.
 *
 * @param {Cheerio} $ The Cheerio loader of the current page.
 * @param {Object} opt The field options.
 * @param {Cheerio} $elm The selected link node(s).
 * @param {String} baseUrl The final response URL of the current page.
 * @param {Array} ancestors Ancestor pages.
 * @param {String} fieldPath The field path for error messages.
 * @returns {Promise<Object|null>} The detail data, or `null` when there is no link.
 */
async function followLink ($, opt, $elm, baseUrl, ancestors, fieldPath) {
    const attrName = opt.attr || "href"
    const rawHref = $elm.first().attr(attrName)

    // Empty link (including whitespace-only) or no matching node -> null
    if (rawHref === undefined || rawHref === null || rawHref.trim() === "") {
        return null
    }

    let target
    try {
        // Relative links use the final response URL; the HTML base is ignored.
        target = new URL(rawHref, baseUrl)
    } catch (err) {
        return null
    }
    target.hash = ""

    // Only HTTP(S) links are followed.
    if (target.protocol !== "http:" && target.protocol !== "https:") {
        return null
    }

    const requestHref = target.href

    // Link loop: the href points at a page on the ancestor chain.
    if (hitsAncestorChain(requestHref, ancestors)) {
        const err = new Error(`Link loop detected at "${fieldPath}": ${requestHref} is already an ancestor page.`)
        err.code = "FOLLOW_CYCLE"
        err.field = fieldPath
        err.url = requestHref
        throw err
    }

    let res
    try {
        res = await fetchDetail(requestHref)
    } catch (err) {
        throw followError(err, fieldPath)
    }

    const finalHref = finalUrlOf(res, requestHref)

    // Redirect loop: the response landed on an ancestor page. Check after the
    // response but before extracting anything from it.
    if (hitsAncestorChain(finalHref, ancestors)) {
        const err = new Error(`Redirect loop detected at "${fieldPath}": ${finalHref} redirects back to an ancestor page.`)
        err.code = "FOLLOW_CYCLE"
        err.field = fieldPath
        err.url = finalHref
        throw err
    }

    let detailData
    try {
        detailData = await extract(
            res.$
          , undefined
          , opt.follow.data
          , finalHref
          , ancestors.concat({ requestHref, finalHref })
          , fieldPath
        )
    } catch (err) {
        if (err.field) {
            throw err
        }
        throw followError(err, fieldPath)
    }

    // The convert function runs once, after the detail object is complete, and
    // still receives the original link node.
    if (opt.convert) {
        detailData = opt.convert(detailData, $elm)
    }

    return detailData
}

/**
 * scrapeIt
 * A scraping module for humans.
 *
 * @name scrapeIt
 * @function
 * @param {String|Object} url The page url or request options.
 * @param {Object} opts The options passed to the scraping engine.
 * @return {Promise} A promise object resolving with:
 *
 *   - `data` (Object): The scraped data.
 *   - `$` (Function): The Cheeerio function. This may be handy to do some other manipulation on the DOM, if needed.
 *   - `response` (Object): The response object.
 *   - `body` (String): The raw body as a string.
 *
 * Fields may declare a `follow` option:
 *
 *   - `follow` (Object): When provided, the field value (normally the selected
 *     link's attribute) is replaced by the data extracted from the linked page:
 *      - `data` (Object): The schema applied to the detail page. It may contain
 *        further `follow` fields at any depth.
 *
 * **Example**:
 * ```js
 * const { data } = await scrapeIt("https://example.com/articles", {
 *     articles: {
 *         listItem: "article"
 *       , data: {
 *             title: "h2"
 *           , detail: {
 *                 selector: "a.detail"
 *               , attr: "href"
 *               , follow: {
 *                     data: {
 *                         title: "h1"
 *                       , tags: { listItem: ".tag" }
 *                     }
 *                 }
 *             }
 *         }
 *     }
 * })
 * ```
 */
async function scrapeIt (url, opts) {
    // Configuration problems are reported before the first network request.
    validateFollowSchema(opts, "")

    const res = await req(url)

    if (!schemaHasFollow(opts)) {
        return Object.assign(res, {
            data: coreScrapeHTML(res.$, opts)
          , body: res.data
        })
    }

    const rootRequest = rootRequestHref(url)
    const rootFinal = finalUrlOf(res, rootRequest)
    const data = await extract(
        res.$
      , undefined
      , opts
      , rootFinal
      , [{ requestHref: rootRequest, finalHref: rootFinal }]
      , ""
    )

    return Object.assign(res, {
        data
      , body: res.data
    })
}

/**
 * scrapeIt.scrapeHTML
 * Scrapes the data in the provided element, synchronously.
 *
 * Schemas containing `follow` fields are rejected: linked extraction needs the
 * async `scrapeIt` entry point, so no request must be started from here.
 *
 * @name scrapeIt.scrapeHTML
 * @function
 * @param {Cheerio} $ The input element.
 * @param {Object} opts An object containing the scraping information.
 * @returns {Object} The scraped data.
 */
scrapeIt.scrapeHTML = function scrapeHTML ($, opts) {
    if (schemaHasFollow(opts)) {
        const err = new Error(
            "The schema contains follow fields; use the async scrapeIt(url, opts) function instead of scrapeHTML."
        )
        err.code = "ASYNC_FOLLOW_REQUIRED"
        throw err
    }
    return coreScrapeHTML($, opts)
}

module.exports = scrapeIt
