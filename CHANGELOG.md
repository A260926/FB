# MERKLE changelog

## HEAD

* Add `proof.UpdateInclusion`, which converts an inclusion proof from an older
  tree size into one for a newer size using a consistency proof, without
  re-reading leaf data or asking the log for a new proof
  * Proof digests are tracked by the leaf interval they commit to, so ephemeral
    nodes whose covered range changes as the tree grows are handled correctly
* Breaking change: consistency proofs from `size1 = 0` to `size2 != 0` now always fail
  * Previously, this could succeed if the empty proof was provided
* Bump Go version from 1.19 to 1.20

## v0.0.2

* Fuzzing support
* Dependency updates, notably to go1.19

## v0.0.1

Initial release
