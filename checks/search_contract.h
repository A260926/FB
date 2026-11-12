#ifndef SEARCH_CONTRACT_H
#define SEARCH_CONTRACT_H
/* New tests implement each function. Return 0 only after its assertions pass.
 * A skip (including return 77) is a failure. The fixed entry owns the registry. */
int search_fixture_improves(void);
int search_two_slice_oracle(void);
int search_frame_snapshots(void);
int search_channels_tail(void);
int search_legacy_repeat(void);
int search_failure_atomicity(void);
#endif
