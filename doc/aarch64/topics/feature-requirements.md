CPU feature requirements
========================

VIXL's decoder-based tools can report the CPU features that a piece of A64 code
requires. Historically this information has been exposed as plain `CPUFeatures`
sets, which cannot express encodings that *any one of several* extensions can
provide. This page describes the two interfaces, when to use each, and the exact
annotation mode of `PrintDisassembler`.

`CPUFeatureRequirements`
------------------------

`vixl::CPUFeatureRequirements` (in
[src/cpu-features.h](/src/cpu-features.h)) represents a requirement as a list of
*alternatives*. Each alternative is a `CPUFeatures` set that is, on its own,
sufficient. The instruction can run if the CPU supports every feature in at
least one alternative. An empty alternative means that no additional features
are needed.

`GetAlternatives()` returns the normalised list:

* duplicates are removed,
* an alternative that is a strict superset of another is removed (it carries no
  extra information), and
* the remaining alternatives are sorted lexicographically by their feature
  enumeration sequence.

The normalisation means that two values describing the same requirement compare
equal, and `IsSatisfiedBy(available)` is the single test for "can this
configuration run the code?".

For example, `ldr d0, [x1]` can be provided by either FP or NEON, so its
requirement is `{ {FP}, {NEON} }`. `fmov x0, v0.D[1]` requires both at once, so
its requirement is `{ {FP, NEON} }`. Requirements combine with a logical AND:
`Combine` takes the Cartesian product of alternatives, because a sequence needs
one choice from every instruction.

### Historical `CPUFeatures` vs exact requirements

`CPUFeaturesAuditor` still provides the original queries:

* `GetInstructionFeatures()` and `GetSeenFeatures()` return `CPUFeatures` sets,
* `InstructionIsAvailable()` tests the last instruction against the auditor's
  `available` list.

These keep their historical meaning. In particular, when an instruction can be
provided by more than one feature, the `available` list is used as a *hint*: if
exactly one candidate is available, the auditor records only that one; otherwise
it records all of them. This is convenient for reporting "what is likely to be
missing", but it makes the result depend on the hint, and it cannot distinguish
"FP or NEON" from "FP and NEON".

The new queries are exact and never look at `available`:

* `GetInstructionRequirements()` returns the requirement of the most recently
  decoded instruction. It is replaced by each instruction.
* `GetSeenRequirements()` returns the combined requirement of every instruction
  since construction or `ResetSeenFeatures()`. This is a true logical AND across
  instructions, not a concatenation of their alternative lists.

Both return values are `CPUFeatureRequirements` objects by value, so they can be
copied and saved. Later decoding, `ResetSeenFeatures()` or `SetAvailableFeatures()`
does not change a saved value. This makes it possible to analyse a code block
once and then query the saved requirement against several hardware
configurations.

The exact representation only describes features that the auditor already
recognised. It does not make unallocated or otherwise invalid encodings valid,
and it does not add new feature detection.

HINT instructions are a special case: they decode and execute as NOPs even if
the extension they relate to is absent. The exact requirement therefore records
nothing for them. The historical interface still reports the feature when it is
available, and other system instructions are audited as before.

`PrintDisassembler`
-------------------

By default, `PrintDisassembler` annotates instructions with the historical
`CPUFeatures` information, reduced by the available features:

    ldr d0, [x1]                            // Needs: FP, NEON

Calling `SetExactCPUFeatures(true)` switches to the exact requirement instead. It
prints the minimal supplement required by the current instruction, given the
current available features. Each alternative is enclosed in parentheses, the
features within an alternative are separated by `, `, and alternatives are
separated by ` | `:

    ldr d0, [x1]                            // Needs: (FP) | (NEON)

The supplement is reduced: alternatives that are already satisfied are dropped,
and combinations covered by a smaller alternative are not listed. If any
alternative is already satisfied, no annotation is printed. The prefix, suffix
and indentation are the same as in the default mode, so custom prefixes and
suffixes continue to work:

    ldr d0, [x1]                            ### {(FP) | (NEON)} ###

The default mode, custom prefixes and suffixes, and the instruction text itself
are unchanged when exact mode is disabled.

See [/examples/aarch64/feature-requirements.cc](/examples/aarch64/feature-requirements.cc)
for a complete example.
