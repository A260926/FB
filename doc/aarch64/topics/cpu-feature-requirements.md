# CPU feature requirements

VIXL can tell you which CPU features A64 instructions depend on. Most
instructions need one fixed set of features, but some encodings can be provided
by more than one feature. For example, `ldr d0, [x1]` can be implemented by
either FP or NEON.

Two interfaces describe this, and they mean different things:

## Legacy sets: `CPUFeatures`

`CPUFeaturesAuditor` provides:

* `GetInstructionFeatures()`: the features of the most recently decoded
  instruction.
* `GetSeenFeatures()`: the union of the features seen since construction or
  the last `ResetSeenFeatures()`.
* `InstructionIsAvailable()`: whether the last instruction's recorded features
  are all in the available set.

Because a plain `CPUFeatures` set cannot express "either FP or NEON", these
queries choose one feature using the `available` hint passed to the auditor (or
record both when the hint does not decide). That choice is a property of the
particular `available` set in use. It is convenient for "what does this code
use" reports, but it cannot answer "would this code run on a different
configuration?" after the fact. `InstructionIsAvailable()` follows the same
hint-selected set.

`CPUFeaturesAuditor` and the simulator continue to use these queries, and their
meaning is unchanged.

## Exact requirements: `CPUFeatureRequirements`

`CPUFeatureRequirements` (in `src/cpu-features.h`) represents a disjunction of
alternatives:

* `GetAlternatives()` returns `std::vector<CPUFeatures>`. Each element is a set
  of features that is sufficient on its own, and the instruction is satisfied
  if any one element is available. The empty set means "no extra features are
  needed".
* `IsSatisfiedBy(available)` returns true if some alternative is a subset of
  `available`.

Requirements are normalised: duplicates are removed, any alternative that is a
strict superset of another is dropped, and alternatives are ordered by feature
enumeration order, so equal requirements always have the same representation.

`CPUFeaturesAuditor` exposes:

* `GetInstructionRequirements()`: the requirements of the most recently decoded
  instruction. This is replaced with each instruction.
* `GetSeenRequirements()`: the requirements that satisfy *every* instruction
  seen since construction or the last `ResetSeenFeatures()`. Alternatives are
  conjoined across instructions (not concatenated), so `(FP) | (NEON)` followed
  by `(FP)` becomes `(FP)`.

Both return self-contained values, so a saved copy is not affected by later
decoding, resetting or changing the available features. Neither query is
influenced by the `available` hint.

These queries only interpret the features that the auditor already recognises.
They do not make any claim about the validity of unallocated encodings.

Two details are worth noting:

* HINT-space instructions such as `esb` or `paciasp` decode and execute as NOPs
  when their extension is absent, so their exact requirements are empty. The
  legacy queries still record the extension when it is available.
* Multiple requirements recorded for one instruction are simultaneous: for
  example, `fmov v0.D[1], x13` needs FP *and* NEON.

## Disassembly annotations

`PrintDisassembler` annotates instructions using a registered
`CPUFeaturesAuditor`. By default it uses the legacy set (minus the available
features). Calling `SetExactCPUFeatures(true)` switches it to
`CPUFeatureRequirements`:

```c++
PrintDisassembler dis(stdout);
dis.RegisterCPUFeaturesAuditor(&auditor);
dis.SetExactCPUFeatures(true);
```

With no available features, a shared instruction is printed as:

```
ldr d0, [x1]                            // Needs: (FP) | (NEON)
```

Each alternative is wrapped in parentheses, alternatives are separated by
` | `, and feature names use the usual spelling. Alternatives that are already
satisfied are omitted; an instruction whose remaining alternatives are all
empty is not annotated. The prefix and suffix can be customised as before,
including in exact mode.
