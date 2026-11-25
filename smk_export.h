#ifndef SMK_EXPORT_H
#define SMK_EXPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Export an SMK2/SMK4 movie as an interleaved 32-bit RIFF AVI.
 *
 * audio_mask may only contain bits 0..6.  Set, existing tracks are placed in
 * ascending original track order at AVI audio stream numbers 1, 2, ...
 *
 * Returns 0 on success.  On failure, a partially created output is removed
 * and a non-zero value is returned.
 */
int smk_export_avi(const char *input, const char *output, unsigned audio_mask);

#ifdef __cplusplus
}
#endif

#endif
