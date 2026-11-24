# Offline regression contract

All inputs are inert simulated log text/metadata. No target execution, mutation, signal delivery, debugger, symbolizer, network, or input construction is required. Source context includes DrFuzz and native module logging; this exported closure builds only the offline Perl/C++ path. Original dynamic instrumentation is not buildable from this subset.

## Public C++17 API

Create `drmemory/regression.h` with the following declarations in namespace `drreg`. Implement in `regression.cpp` and `regression_store.cpp` (private helpers may be added). Class may use a private implementation; it owns all strings and vectors, has a destructor, and need not be copyable. No external libraries.

```
enum class Outcome { Complete, Timeout, ToolError };
struct Image {
  std::string worker, token, build;
  uint64_t generation, base, size, begin, end;
};
struct Error { std::string type; std::vector<uint64_t> pcs; };
struct Trial {
  std::string worker;
  uint64_t tick;
  Outcome outcome;
  std::vector<Error> errors;
};
class Ledger {
public:
  Ledger();
  ~Ledger();
  bool AddImage(const Image &);
  bool Begin(const std::string &id, uint64_t generation, size_t repeats);
  bool Observe(const std::string &id, uint64_t generation, size_t slot,
               const Trial &);
  std::string Report() const;
  bool Save(const std::string &path) const;
  bool Load(const std::string &path);
};
```

Include `<string>`, `<vector>`, `<cstdint>`, `<cstddef>` in the header. Every bool operation reports failure by false, leaving the old object intact. Allocation/IO errors must not publish partial state. Inputs are copied. No concurrency requirement.

Identifiers (`id`, worker, token, build) are 1..128 ASCII characters from `[A-Za-z0-9_.-]`; generation is positive, repeats is 1..64. Addresses/ticks are uint64. Image's size>0, base+size cannot wrap, begin<end; lifetime and address range are half open. Identity is worker/token/generation. An identical full record succeeds idempotently; an identity conflict fails. Within one worker, records may not overlap in both time AND address; records with the same token may not overlap in time even at different bases. For the same worker/token, generation order must agree strictly with begin order (late arrival of an older declaration is legal). Build and token are separate identities; build is a trusted artifact identity, not a pathname. Never dereference PCs.

Begin identical id/generation/repeats succeeds, higher generation replaces that candidate's entire trial state, lower generation or same generation/different repeats fails. Images remain global. Observe requires the current generation, slot<repeats, valid worker, valid Outcome. Complete allows zero or up to1024 errors. Error.type is exactly `UNADDRESSABLE ACCESS`, `UNINITIALIZED READ`, or `INVALID HEAP ARGUMENT`; each has 1..64 PCs. Other outcomes require an empty errors vector. An identical slot record succeeds without increasing counts; a conflicting slot fails. Equality here includes worker, tick, outcome and the complete error/PC vectors in supplied order. No retry replacement within a generation.

For each error at its trial's tick, first PC is an instruction PC except INVALID HEAP ARGUMENT (the offline mode assumes wrapped allocation APIs); all other PCs are return addresses. For return addresses subtract one before module range lookup, including the first HEAP frame. Zero return address is unresolved. Find the unique image with matching worker, lifetime, address; any unresolved frame makes the trial unresolved. Signature consists only of type and ordered `(build, adjusted_pc-base)` frames. Map actual types to UNADDR/UNINIT/HEAP. Text signature is `CODE|build@offset,build@offset` with lowercase hex offsets without `0x` or leading zeroes (zero is `0`). Token, generation, absolute PC, worker, time, thread, header/details and symbol text do not enter identity.

For a complete trial, normalize its errors to a sorted unique SET of signatures; multiplicity/order of errors does not affect consensus, but order of frames does. Compute afresh after late image declarations; do not permanently cache unresolved or earlier mappings. Report state: missing slots => PENDING; otherwise any Timeout/ToolError/unresolved => INCONCLUSIVE; otherwise all empty sets => CLEAN; identical nonempty sets => STABLE; any different sets => FLAKY (including one empty and one nonempty). A case with sets `{A,B}` and `{A,C}` is FLAKY and contributes to no group. Stable case with `{A,B}` contributes once to both groups. Higher case generation immediately removes all old contributions.

Report is UTF-8 ASCII lines: first all `CASE id generation STATE` sorted by id, then all `GROUP signature id1,id2` sorted lexicographically by full signature, members sorted unique. Each line ends LF, empty ledger => empty string. The returned string is independently owned. Save/Load preserve images, unfinished candidates and raw trials, all conflict facts and version. Format is implementation-defined, must have a format version and integrity check, no process addresses. Reject corrupt/truncated/extra data, invalid records or duplicate identities with conflicts. On failed Load retain the old object. Saving uses a sibling temporary and atomic rename; never truncate the destination first, clean only self-created temporary files. Failed Save leaves destination bytes unchanged.

## Native Perl adapter and CLI

Add options `-regression_manifest PATH -regression_engine PATH -regression_report PATH [-regression_state PATH]` to `drmemory/postprocess.pl`. All three required together, they are incompatible with legacy aggregate/live options. Dispatch before legacy argument validation and before creating results.txt/results-summary.txt/suppress.txt. Ordinary options without this mode keep behavior. Use list-form process invocation, never shell interpolation. Both Perl parent and C++ child must propagate failure. These paths may contain spaces. Relative log paths below resolve relative to manifest's parent, not current working directory; absolute paths are also accepted. Paths in manifest use the rest of their line verbatim (except CR/LF), not token splitting.

Manifest directives, one per line; blank/# comment lines ignored:

```
CASE sample 1 2
IMAGE worker token 1 artifact 4096 4096 10 20
RUN sample 1 0 worker 12 COMPLETE
STACK shared stack.log
LOG thread.1.log
ENDRUN
```

IMAGE fields are worker token generation build base size begin end. All numeric manifest/protocol values are unsigned DECIMAL, bounded to their API type; no negative/overflow/trailing junk accepted. RUN fields are case generation slot worker tick outcome. Outcomes COMPLETE/TIMEOUT/TOOLERROR. RUN blocks cannot nest; at least one LOG, any number of STACK directives before its LOGs. RUN ends at ENDRUN; EOF within a RUN fails. Each RUN is one whole trial, so errors from all its LOGs form one vector. A complete empty/LOG END-only file gives an empty vector. Timeout/ToolError logs must be empty of errors. STACK maps a display label (single identifier, ignored for identity) to the remaining filename; call `process_callstack_log` to expand `callstack=N` in LOGs. Reset shared stack indices per RUN; references must exist in that RUN. Each LOG goes through the original `process_all_errors` and error handling through `process_one_error`/`parse_error` (a mode-specific emission branch is expected). No copy/paste parser. Reject incomplete blocks rather than silently ignoring EOF; reject a nested error start before error end, unsupported error types, nonnumeric PCs and malformed shared references. Ignore unrelated informational lines. New mode does not call lookup_addr or generate suppressions, process FORK/EXEC, or run symbolizers. Preserve the native parser's frame order, and translate its three accepted types to the CLI codes. A STACK id must be defined completely before referenced, repeated conflicting definitions fail. Do not reuse the legacy global error cache to drop repeated blocks: repeated blocks remain raw errors and dedup is by mapped signature set in C++.

C++ executable `drmemory/regression_cli.cpp` takes exactly `EVENTS REPORT [STATE]`, reads structured events only. Perl creates an owned temporary event file, invokes the executable, removes only that temporary, and returns its exit status. Structured format is the manifest's CASE/IMAGE plus:

```
TRIAL sample 1 0 worker 12 COMPLETE 1
ERROR UNADDR 2 4100 4121
ENDTRIAL
```

TRIAL fields match RUN plus error count. Exactly that many ERROR lines follow, then ENDTRIAL. ERROR contains code, pc count and decimal PCs. No raw headers, log paths or shared refs enter this protocol. The CLI rejects invalid structure/API refusal/trailing noncomment junk with nonzero. Parse and apply an invocation to a temporary ledger first. With STATE, load it if it exists, require success if present, otherwise start empty, then apply input; don't automatically complete cases. Produce Report to a sibling temporary and rename atomically. On any parse/load/write/save failure leave old REPORT unchanged. If STATE requested, save successfully before publishing REPORT; simultaneous atomicity of REPORT and STATE is not required, but failed state saving cannot replace REPORT. Never treat an existing output file as a new result.

## Required integration checks

`checks/regression_tests.cpp` registers groups timeline/consensus/resume via `task_support/check.h`. Cover native direct frames/shared stacks, terminal truncation and unknown reference rejection, return address at end boundary, unload/reload/build change, equivalent ASLR stacks, late images and same-token chronology conflicts; full error-set equality (order/multiplicity immaterial), missing slots, normal/timeout/error combinations, generation replacement; resume unfinished ledger in another process, integrity corruption and file failures preserving old state/output. Exercise both the Perl entry and C++ APIs. `examples/regression_replay.cpp` takes an output directory, launches two own subprocesses which save/restore unfinished state; second adds images and completes observations, checks final Report and writes transcript.txt. No artificial faults or target executables. The task runner runs these plus an independent fixed API/Perl probe and legacy baseline. It supplies no production implementation or goal tests.
