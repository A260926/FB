# Root cause: render context was applied after the template cache key

## Where the context is established

`Brakeman::RenderHelper#process_template` (`lib/brakeman/processors/lib/render_helper.rb`)
builds a `template_env` from the caller's instance variables and request
variables (`only_ivars(:include_request_vars)`). The `:locals` and
`:collection`/`:as` options of the `render` call are the second half of that
context: they are, from the template's point of view, indistinguishable from
instance variables.

## When it was lost

The code computed the cache digest *before* adding those options:

```ruby
template_env = only_ivars(:include_request_vars)
digest = SHA1(template_env["@env"].to_a.sort.to_s << name)   # <- locals absent
if @tracker.template_cache.include? digest
  return                                                      # <- second render skipped
end
options = get_options args
template_env[...] = options[:locals][...]                     # too late
```

The digest therefore described only the caller's ivars plus the template name,
not the arguments that actually reach the template. Two controller actions
that render the same partial with different `:locals` produce identical
digests whenever their ivars match — which is the common case for a plain
action. Whichever action is processed first wins; later renders return at the
cache check and are never run through the alias processor or the template
processor. That is why the result depended on action order and why
`unsafe_only=1, safe_first=0, unsafe_first=1`: with `safe` first, the constant
`hello` render populated the digest, and the `params[:q]` render was silently
dropped.

The same applies at every depth: a wrapper that forwards a local under a new
name produces the same digest for the safe and unsafe roots, so nested
partials inherited the first root's environment. `:collection` had the same
problem, and worse, the collection value is model-typed (`Model.new`), so a
lost collection context either hid a model warning or leaked one into an
unrelated constant render.

## The fix

Locals and collection values are now inserted into `template_env` before the
digest is computed, so the digest covers the full context that the template is
processed under. The cache is still used — identical contexts still share a
result — but contexts that differ in locals, collection variable, or `:as`
name are processed and reported separately. Values are `deep_clone`d into the
environment, and `process_safely` clones the template source, so the original
`Template#src` Sexp is never rewritten by one render's alias substitution. The
tracker (and therefore `template_cache`) is still created fresh per
`Brakeman.run`, so sequential scans in one process do not share state. The
digest input is built from `inspect`ed pairs because raw `Sexp` pairs are not
mutually comparable and `Array#sort` could raise once locals were included.

## Why the warning belongs to the template output

`CheckCrossSiteScripting` reports on the template's `output`/`escaped_output`
node; `Warning#initialize` derives `called_from` from `template.render_path`,
and the file/line come from the rendered template copy. The controller is only
the source of the value, not the location of the sink. Correct attribution
therefore requires the *per-render* template copy to carry the environment of
the action that produced it. Fixing the cache key restores that: the reported
file/line is the template line of `<%= raw(value) %>`, and `render_path` still
ends at `CardsController#unsafe` (through `Template:...` entries for nested
partials), never at the safe action that happened to be scanned first.
