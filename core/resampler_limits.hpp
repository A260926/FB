#ifndef CORE_RESAMPLER_LIMITS_HPP
#define CORE_RESAMPLER_LIMITS_HPP

/* Maximum number of samples to pad on the ends of a buffer for resampling.
 * Note that the padding is symmetric (half at the beginning and half at the
 * end)!
 */
inline constexpr auto MaxResamplerPadding = 48u;

inline constexpr auto MaxResamplerEdge = MaxResamplerPadding >> 1u;

#endif /* CORE_RESAMPLER_LIMITS_HPP */
