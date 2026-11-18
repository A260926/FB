/*
 * ALACProjectChannels.h
 *
 * Channel projection ("--project") support for alacconvert.
 *
 * A projection takes an ALAC in a CAF container, selects a subset of the
 * decoded channel slots (in an arbitrary order) and writes a new ALAC/CAF
 * file that contains only those channels.  Audio frames are neither
 * truncated nor resampled.  Elements whose complete channel set is retained
 * in its original order are copied bit for bit (only the 4-bit
 * element_instance_tag is renumbered); all other elements are encoded from
 * the real PCM of the current packet.
 */

#ifndef ALAC_PROJECT_CHANNELS_H
#define ALAC_PROJECT_CHANNELS_H

#include <stdint.h>
#include <vector>

// Parses a projection list such as "0,2,1".  Returns 0 on success.  Slots are
// zero based input channel indices.  A list must contain between 1 and 8
// unique entries, each in the range 0..7.  On failure a human readable reason
// is stored in *error (when error is not NULL).
int32_t ParseProjectSlots(const char *text, std::vector<uint32_t> *slots,
                          const char **error);

// Runs a channel projection from inputPath to outputPath.  Both files are
// ALAC/CAF.  Returns 0 on success; on failure a message has been printed to
// stderr and no complete output file is published.
int32_t RunChannelProjection(const char *inputPath, const char *outputPath,
                             const std::vector<uint32_t> &slots);

#endif // ALAC_PROJECT_CHANNELS_H
