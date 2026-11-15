Offline packaging adapters
==========================

The two upstream dependencies `github.com/unixpickle/essentials` v1.3.0 and
`github.com/unixpickle/splaytree` v1.2.0 do not ship a license at those tags.
Their implementation files are NOT included in this seed.

The local `essentials` and `splaytree` modules here are independently authored,
MIT-licensed standard-library compatibility adapters for the public APIs used
by the fixed model3d source. The ordered-tree adapter uses AVL balancing;
it does not purport to contain the upstream splay implementation. The
concurrency adapter uses a shared atomic work queue and serialized reductions.
These adapters supply no distributed contouring or target functionality.

`pkgerrors` contains the original MIT-licensed github.com/pkg/errors v0.9.1
files from the recorded source patch. Root go.mod uses explicit local replace
directives; vendor/modules.txt records these replacements. The copies inside
vendor are generated from these local modules, not downloaded or relabelled
upstream source. Each module contains its applicable LICENSE.

The fixed upstream model3d production Go files are unchanged. The only packaging
change is the replace block in go.mod.
