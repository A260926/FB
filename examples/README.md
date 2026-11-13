# Offline DXE dependency analysis example

`depex_offline_example.cpp` demonstrates the Qt-free `Depex::analyze()`
analyzer without any firmware image, PE machine code inspection or driver
loading.

## What it shows

The TreeModel firmware volume, the driver file nodes and the DXE_DEPEX byte
streams are all generated in C++. The protocol GUIDs every driver publishes
after successful dispatch are explicit assumptions passed by the caller.

The scenario is the NOT-window case:

- driver A depends on `NOT P` and publishes `Q`
- driver B depends on `TRUE` and publishes `P`
- driver C depends on `Q`

If A is selected first all three drivers dispatch; if B is selected first the
`NOT P` window closes and A/C can never run. The exhaustive analysis therefore
reports B as `must` and A/C as `may`, with an independently reconstructable
witness for every claim.

## Build and run

Only the in-tree `common/` sources are compiled, no Qt is involved:

```sh
cmake -S examples -B build-example
cmake --build build-example
./build-example/depex_offline_example
```
