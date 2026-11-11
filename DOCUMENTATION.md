## Documentation

You can see below the API reference of this module.

### `scrapeIt(url, opts, cb)`
A scraping module for humans.

#### Params

- **String|Object** `url`: The page url or request options.
- **Object** `opts`: The options passed to `scrapeHTML` method.
- **Function** `cb`: The callback function.

#### Return
- **Promise** A promise object resolving with:
  - `data` (Object): The scraped data.
  - `$` (Function): The Cheeerio function. This may be handy to do some other manipulation on the DOM, if needed.
  - `response` (Object): The response object.
  - `body` (String): The raw body as a string.

### `scrapeIt.scrapeHTML($, opts)`
Scrapes the data in the provided element.

For the format of the selector, please refer to the [Selectors section of the Cheerio library](https://github.com/cheeriojs/cheerio#-selector-context-root-)

#### Params

- **Cheerio** `$`: The input element.
- **Object** `opts`: An object containing the scraping information.
  If you want to scrape a list, you have to use the `listItem` selector:

   - `listItem` (String): The list item selector.
   - `data` (Object): The fields to include in the list objects:
      - `<fieldName>` (Object|String): The selector or an object containing:
         - `selector` (String): The selector.
         - `convert` (Function): An optional function to change the value.
         - `how` (Function|String): A function or function name to access the
           value.
         - `attr` (String): If provided, the value will be taken based on
           the attribute name.
         - `trimValue` (Boolean): If `false`, the value will *not* be trimmed
           (default: `true`).
         - `closest` (String): If provided, returns the first ancestor of
           the given element.
         - `eq` (Number): If provided, it will select the *nth* element.
         - `texteq` (Number): If provided, it will select the *nth* direct text child.
           Deep text child selection is not possible yet.
           Overwrites the `how` key.
         - `follow` (Object): Declares linked extraction. When present, the
           field value is no longer the selected attribute: the first link
           (selected with `selector`, `eq`, `closest`; `attr` defaults to
           `href`) is fetched with a new GET request and the field becomes the
           object extracted from that page:
            - `data` (Object): The schema applied to the linked page. It may
              contain nested `data`, `listItem` and further `follow` fields.
         - `listItem` (Object): An object, keeping the recursive schema of
           the `listItem` object. This can be used to create nested lists.

  **Example**:
  ```js
  {
     articles: {
         listItem: ".article"
       , data: {
             createdAt: {
                 selector: ".date"
               , convert: x => new Date(x)
             }
           , title: "a.article-title"
           , tags: {
                 listItem: ".tags > span"
             }
           , content: {
                 selector: ".article-content"
               , how: "html"
             }
           , traverseOtherNode: {
                 selector: ".upperNode"
               , closest: "div"
               , convert: x => x.length
             }
         }
     }
  }
  ```

  If you want to collect specific data from the page, just use the same
  schema used for the `data` field.

  **Example**:
  ```js
  {
       title: ".header h1"
     , desc: ".header h2"
     , avatar: {
           selector: ".header img"
         , attr: "src"
       }
  }
  ```

  **Linked extraction example** — two catalog records, each carrying the
  object extracted from its own detail page:
  ```js
  const { data } = await scrapeIt("https://example.com/articles", {
      articles: {
          listItem: "article"
        , data: {
              title: "h2"
            , detail: {
                  selector: "a.detail"
                , attr: "href"
                , follow: {
                      data: {
                          title: "h1"
                        , tags: { listItem: ".tag" }
                      }
                  }
              }
          }
      }
  })
  // { articles: [
  //     { title: "First",  detail: { title: "First article",  tags: ["js"] } },
  //     { title: "Second", detail: { title: "Second article", tags: [] } }
  // ] }
  ```

  Linked extraction rules:

   - Relative links are resolved against the final response URL of the page
     that contains the link (after redirects), never against the initially
     requested URL or the HTML `<base>` tag. Fragments are removed before the
     request and only `http:`/`https:` links are followed. An empty link or a
     selector that matches nothing produces `null`.
   - Every detail page is a brand new GET request; a POST body on the root
     request is never inherited. If any detail request fails, the whole call
     rejects with an error carrying the field path (e.g. `articles[1].detail`)
     and the original error as `cause`.
   - The same URL used by different sibling records is still fetched once per
     record — there is no global de-duplication. Cycle detection only compares
     against the request and final response URLs of the current ancestor
     chain: a link back to an ancestor is rejected before the request, while a
     redirect back to an ancestor is rejected after the response but before
     extracting it.
   - `follow` cannot be combined with `how` or `texteq`; such schemas fail
     before any request is made. A field `convert` runs once after the detail
     object is complete and still receives the original link node.
   - `scrapeHTML` is synchronous and never performs requests: schemas
     containing `follow` make it throw an error with the code
     `ASYNC_FOLLOW_REQUIRED`. Use the async `scrapeIt(url, opts)` instead.

#### Return
- **Object** The scraped data.

