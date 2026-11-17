# Changelog

This file contains notable libpdb changes.


## Unreleased

* Parse the DBI ModInfo substream and publish the module registry in physical
  order via `pdb_get_modules`.
* Add `pdb_get_module_symbols` to enumerate a module stream's symbol region,
  stopping at `SymByteSize`.
* Add `pdb_resolve_procedure_ref` to resolve `S_PROCREF`/`S_LPROCREF` records to
  `S_GPROC32`/`S_LPROC32` definitions.
* Accept PDBs without a GSI, PSI, or global record stream; public symbol
  enumeration and lookup fall back to the global record stream.
* Add `pdbparse --modules`.


## v0.1.0 - 2020-04-07

Initial release.
