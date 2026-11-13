---
sidebar_position: 23
---

# File choosers

`Node#select_file` works when you already have the file input, e.g. one rendered in the markup. Some pages only
create the input when a button is clicked: a hidden input is appended to the DOM and the browser's native file
chooser opens immediately, so there is no stable selector to find. `capture_file_chooser` intercepts that native
chooser and hands back a `Ferrum::FileChooser` for the input the browser opened it for.

```ruby
chooser = page.capture_file_chooser { page.at_css("#attach").click }
chooser.accept("/tmp/invoice.pdf")
```

The interception and the event listener are installed before the block runs and removed when it ends, so the block
is the only place that opens the chooser. It runs exactly once and is never interrupted; the timeout only bounds the
wait from interception being ready to the chooser opening.

## Multiple files

`accept` takes one path or a non-empty array of paths. Whether several files are allowed comes from the captured
input itself: a single-file input raises `ArgumentError` when given more than one.

```ruby
chooser = page.capture_file_chooser { page.at_css("#attach").click }
chooser.accept(["/tmp/invoice.pdf", "/tmp/receipt.pdf"]) if chooser.multiple?
```

Paths are expanded to absolute paths, kept in input order and each checked to be a readable regular file before
anything reaches the browser, so an invalid path never uploads the files before it. A failed validation leaves the
chooser usable: fix the paths and call `accept` again. The browser and the Ruby process share the same file paths,
exactly as with `select_file`.

The assignment goes through `DOM.setFileInputFiles`, the same call `Node#select_file` uses, so the browser fires the
real `input` and `change` events and the page's own handlers run. The input's `multiple`, `accept` or `form`
attributes are never modified, and no `FileList` is synthesized from JavaScript.

## Frames

Pass the frame the chooser opens in. Only that frame's next chooser is captured; a chooser opened by the top frame
or a sibling frame can never impersonate it. Same-origin nested iframes work, including ones nested several levels
deep.

```ruby
frame = page.frames.find { |f| f.url.end_with?("/upload") }
chooser = page.capture_file_chooser(frame: frame) { frame.at_css("#attach").click }
chooser.accept("/tmp/invoice.pdf")
```

## Chooser metadata

A captured chooser exposes read-only metadata about the input it was opened for:

* `frame_id` (`String`) - id of the frame the input lives in
* `backend_node_id` (`Integer`) - Chrome's backend id of the input, taken from the capture event
* `multiple?` (`Boolean`) - whether the input accepts several files

## Stale choosers

The chooser keeps the identity of the input's document at capture time. If the input is replaced or removed, its
frame is detached, or its document navigates, `accept` raises `Ferrum::NodeNotFoundError` instead of uploading to a
different node that happens to match the same selector. A same-document change (only the URL hash changes) or a
navigation in a sibling frame does not invalidate it.

One successful `accept` consumes the chooser: a second call raises `Ferrum::Error` and never fires a second round of
`input`/`change`. Only one capture can run on a page at a time (a nested capture raises `Ferrum::Error` before its
block runs); different pages capture independently.
