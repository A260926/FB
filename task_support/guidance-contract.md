# Feedback-directed request-tape mutation

Add to inc/theft.h (types may be in inc/theft_types.h):

```c
struct theft_guidance_info {
    size_t trial_id;
    enum theft_trial_res result;
    bool mutated;
    uint64_t donor_id, admitted_id;
    uint8_t mutation_arg;
    size_t mutation_request;
    uint32_t mutation_bit;
    uint64_t feature_mask, novel_mask;
    size_t corpus_size;
};
typedef void theft_guidance_post_cb(const struct theft_guidance_info *, void *);
struct theft_guidance_config {
    size_t corpus_limit;
    theft_guidance_post_cb *post;
    void *env;
};
enum theft_run_res theft_run_guided(const struct theft_run_config *,
                                  const struct theft_guidance_config *);
bool theft_cover(struct theft *, uint8_t feature);
```

The new entry uses the real theft run/generation/call/shrink machinery. It accepts 1..7 arguments, all with autoshrink enabled and no custom shrink callback, and corpus_limit 1..16. NULL configurations, invalid widths/types/limits return THEFT_RUN_ERROR_BAD_ARGS before user allocation. Legacy theft_run remains unchanged, including its random sequence. No other threads, external processes or targets are used by callers. Properties/generators are finite, read their arguments, and do not depend on mutable state across calls.

A tape records the exact low-bit-first bits actually returned to one allocator and each positive-width random request, in order. A corpus entry owns deep copies of all argument tapes, never user object pointers or the live autoshrink pools. Only consumed bits belong to it, including a partially used final word; unused PRNG padding is excluded. Maximum per argument: 4096 consumed bits and 128 positive requests. Going over either limit ends guided execution with THEFT_RUN_ERROR after the finite allocator returns, before property evaluation or publication; no assertion or silent accepted truncation. Ordinary API usage contracts still apply.

Trial IDs start at zero. With no corpus, or trial_id % 4 == 0, generate fresh arguments by the existing trial-seed path. Otherwise choose the oldest entry whose monotonic ID is at least next_donor_id, wrapping to the oldest entry if necessary, then set next_donor_id to its ID + 1. IDs start at zero; the cursor initially is zero and advances even if replay later returns SKIP or DUP. A corpus entry with no positive request is ineligible as a donor; skip these entries and use fresh generation if none is eligible.

For a donor, concatenate its positive request descriptors in argument-index/request-index order. Let R be their total number. Choose descriptor (trial_id - 1) % R, and flip bit floor((trial_id - 1) / R) % request_width within that request. All other stored bits stay unchanged. Replay the complete argument tuple through each type's actual alloc callback using the altered flat tape. The tape is a stream, not a map from old request ordinal to new ordinal: if changing a prefix changes subsequent request widths or their number, consume the stored bits continuously under the new requests. Once stored consumed bits end, append low-bit-first words from a private per-argument theft RNG seeded with trial_seed XOR (0x9e3779b97f4a7c15 * (arg_index + 1)), with uint64 wrap. Start this extension RNG at its first word; do not advance it for donor bits. Preserve the existing trial-seed schedule, and do not use shrinking's zero-fill-at-end semantics for this generation mode. Save the new actual request sequence and consumed prefix if the resulting input is admitted. SKIP, allocation errors and duplicates retain normal theft behavior and free partial argument objects. Bloom must hash the actual reconstructed tuple/pools, not the selected donor or a stale pool.

During only an actual primary guided property call, theft_cover accepts feature IDs 0..63 and returns true; repeated features are idempotent, 0 is valid, and 64..255 returns false. During allocation, fork_post, other hooks, shrinking and trial_post-requested reruns, or ordinary theft_run, it returns false and contributes nothing. Reset the mask for every actual evaluation. In fork mode the child must return its complete mask with the verdict, with full-length I/O and EINTR handling. The parent cannot publish a mask from an incomplete result or from timeout/abnormal exit. Preserve the library's timeout/exit classification, and close/reap its own resources. Do not construct abnormal targets to implement or demonstrate this feature.

Only a primary PASS can add new features: novel_mask = feature_mask & ~ever_seen. A nonzero novel mask admits the tuple immediately, before normal trial_post and any argument cleanup. Set ever_seen |= feature_mask, assign the next monotonic admission ID, append FIFO and evict the oldest if necessary. Eviction never clears ever_seen; it must not invalidate the donor while replaying or retain pointers to freed objects. PASS with no novelty, FAIL/SKIP/DUP/ERROR and generation failure do not admit. Failure shrinking, hooks and normal counters retain their purpose; shrink/repeat executions cannot add features or move the donor cursor.

If post is non-NULL, call it once in the parent for each completed generated trial, after its primary verdict (or generation SKIP/DUP) and guidance decision, before shrinking and the ordinary trial_post. No call for initialization error or pre-hook HALT. The info is borrowed, contains the actual primary feature_mask (zero for no evaluation), novel_mask only for admitted PASS, and corpus_size after the decision. admitted_id is UINT64_MAX when not admitted; donor_id is UINT64_MAX for fresh generation. For fresh generation mutation_arg/request/bit are UINT8_MAX/SIZE_MAX/UINT32_MAX. For mutation they are the donor's selected descriptor coordinates. No separate feature database or alternative allocator can substitute for these decisions. Two finite runs with identical configuration/seed must produce identical inputs and guidance info with fork on or off. Run-owned corpus and masks are discarded at the end, including every error path.
