# Input checkedness, radio groups and form ownership

This document describes the offline form replay support added to the HTML
library: the current checkedness of `input` elements, radio button group
exclusivity, form ownership and form reset.  Browsers, events and HTTP
submission are out of scope.

## Why a separate state?

The `checked` content attribute is the *default* checkedness.  It cannot
express the state a user (or a replay driver) changed at runtime, and several
controls of one radio group may keep the attribute while only one of them is
actually selected.  The library therefore keeps:

- **current checkedness** — the live selection state, independent of the
  attribute, and
- **dirty checkedness** — whether the live state was set explicitly.

At parse time the current checkedness comes from the `checked` attribute.
While a control is clean, adding or removing the attribute syncs the current
value.  After an explicit set the control is dirty, and the attribute only
changes the default (as required by the `defaultChecked` / `checked`
distinction).  The setter never modifies the content attribute, so the default
remains readable from the DOM.

Form reset clears the dirty checkedness of the inputs owned by the form and
restores their current state from the checked attribute default that is
present *at reset time*.

## Public API

`source/lexbor/html/interfaces/input_element.h`:

```c
bool lxb_html_input_element_checked(lxb_html_input_element_t *input);
lxb_status_t lxb_html_input_element_set_checked(lxb_html_input_element_t *input,
                                                bool checked);
lxb_html_form_element_t *
lxb_html_input_element_form_owner(lxb_html_input_element_t *input);
```

`source/lexbor/html/interfaces/form_element.h`:

```c
lxb_status_t lxb_html_form_element_reset(lxb_html_form_element_t *form);
```

- `checked()` returns the live state.  It never changes the selection; it
  may resolve the form owner of that single control if it was never resolved
  before.
- `set_checked()` always marks the control dirty, even when the value does
  not change.  Setting a radio button to true unchecks the other checked
  members of its group.  It returns `LXB_STATUS_ERROR_OBJECT_IS_NULL` for
  a NULL input.
- `form_owner()` returns the owning form or `NULL`.
- `reset()` returns `LXB_STATUS_ERROR_OBJECT_IS_NULL` for a NULL form.

All parameters use the types already present in the repository.

## Radio button groups

Two radio buttons belong to the same group when all of the following hold:

- their `type` attribute value is `radio` (ASCII case-insensitive),
- they have the same tree root (controls without a form owner are only
  grouped inside their own tree root, not across the whole owner document),
- they have the same form owner,
- they have exactly the same non-empty `name`.

Controls without a `name`, with an empty `name`, and checkboxes are never
part of group exclusivity.  A control with a radio type whose `name` is
missing takes no part in exclusivity either.

When a radio button becomes checked, the other checked members of its group
have their **current** checkedness cleared; their `checked` attributes and
dirty flags stay untouched.  When several checked radio buttons of one group
appear at once (during parsing, when an subtree is inserted, when `type`,
`name` or `form` change, or during reset), they are processed in tree order
and the **last** one wins.

Changing `type` to a non-radio value keeps the current state; changing it
back to `radio` reapplies the current state to the group.  `disabled` does
not prevent the programmatic setter or reset.

## Form ownership

The owner is computed as follows:

1. `form` attribute absent: the nearest ancestor `form` element (in the
   element's own tree, detached or not).
2. `form` attribute present: look up the first element in document order
   with that ID **in the same tree root**; it associates only if it is an
   HTML `form` element.  If nothing matches, there is no fallback to the
   ancestor form.
3. An empty `form` attribute value means no owner.
4. A tree detached from the Document does not resolve form IDs.  A control
   without a `form` attribute still finds ancestor forms of its own tree.

Explicit association can point to a form outside the subtree (for example an
input outside the form element carrying `form="the-form-id"`).

The owner is cached on the input and refreshed by the mutation steps when
any relevant id changes, when association targets are inserted or removed,
or when a subtree moves.  The steps are wired into the existing element and
attribute mutation dispatch chains, for plain HTML documents as well as
style-enabled HTML documents, without replacing the existing style or option
callbacks.  Attribute changes through `lxb_dom_element_set_attribute` and
through `lxb_dom_attr_set_value` produce the same result.  Moving nodes
through the DOM insert/remove entry points is enough; callers do not need an
extra refresh.  The `_wo_events` low-level entry points and arbitrary
cross-document adoption are out of scope.

## Reset order

Reset restores the inputs whose owner is the reset form, including inputs
associated through their `form` attribute outside the form subtree, in the
order of the tree that contains the form.  It clears their dirty checkedness
first and then normalizes the resulting radio groups in tree order, so the
last checked default wins.

## Out of scope

Text values, `select`, event dispatch and submission are not handled.

## Offline replay example

`examples/form_replay.c` implements a replay driven exclusively through the
API above.  It parses two HTML forms shipped in
`test/files/lexbor/html/form_replay/` (`form_a.html`, `form_b.html`), applies
an action transcript, and exports, in tree order, the selected radio
`name=value` pairs of every form.

Build and run:

```sh
cc -DLEXBOR_STATIC -Isource examples/form_replay.c liblexbor_static.a \
   -lm -o form_replay
./form_replay test/files/lexbor/html/form_replay/form_a.html \
               test/files/lexbor/html/form_replay/form_b.html \
               test/files/lexbor/html/form_replay/actions.txt
```

With CMake, the example is picked up automatically from `examples/` when
`-DLEXBOR_BUILD_EXAMPLES=ON` is set (the CMake build registers it as a test
without arguments).

Action lines are `<command> <document> <arguments...>`; blank lines and
lines starting with `#` are ignored.

| Command  | Arguments                  | Effect |
|----------|----------------------------|--------|
| `select` | `<doc> <input-id>`         | `set_checked(true)` |
| `unselect` | `<doc> <input-id>`       | `set_checked(false)` |
| `name`   | `<doc> <input-id> <name>`  | set the `name` attribute |
| `form`   | `<doc> <input-id> <form-id>` | set the `form` attribute |
| `move`   | `<doc> <node-id> <parent-id>` | DOM move of a node (and its subtree) |
| `attr`   | `<doc> <node-id> <name> [value]` | set an attribute; `-` removes it |
| `reset`  | `<doc> <form-id>`          | `lxb_html_form_element_reset()` |

Output records, one per line:

```
ENTRY <form-id> <name>=<value>
FORM  <form-id> empty
REPLAY_OK
```

Only radio entries are exported; checkboxes, text values, `select`, events
and submission are intentionally omitted.

The contract tests in `test/radio_contract.cpp` call
`lxb_form_replay_run()` in-process and assert hand-written expected record
blocks, including repeatability, external controls, reset results and
subtree moves.
