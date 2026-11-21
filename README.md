# Spectral transfer material (`spectralmix`)

Standard pbrt materials combine light with surfaces by per-channel
multiplication of `Spectrum` values.  That cannot express a coating
that moves energy *between* channels — for example redirecting
incident blue energy into the green channel.  This extension adds a
discrete, linear channel operator to the BxDF/BSDF and transport
paths and a material that uses it.

## Channel order

`Spectrum` holds `COLOR_SAMPLES = 3` samples indexed `0, 1, 2` in
the repository's fixed red/green/blue sample order (the same order
used by `Spectrum::c` and the CIE `XWeight/YWeight/ZWeight` tables).
All matrices below act on these three channels in that order.

## The transfer matrix and energy convention

The material parameter `transfer` is a row-major
`COLOR_SAMPLES x COLOR_SAMPLES` matrix `C` with exactly
`COLOR_SAMPLES*COLOR_SAMPLES` (9) elements.  Entry `C(i,j)` — row
`i`, column `j` — is the coefficient from **incident channel `j`**
to **outgoing channel `i`**:

```
out[i] = sum_j C(i,j) * in[j]
```

The reflection function of the transfer lobe is the Lambertian-shaped
operator `C / pi`, and it is sampled with the same cosine-weighted
hemisphere sampling as an ordinary diffuse lobe.  Because rows and
columns distinguish source and destination channels, `C` is allowed
to be asymmetric and fully off-diagonal; an off-diagonal-only response
is a real reflection, not absorption.

`Kd` is an additional, ordinary Lambertian `Spectrum` that defaults
to zero.  The transfer lobe and the `Kd` lobe live in one real BSDF
and are mixed by the standard BSDF component selection and mixture
density.

Energy is conserved per incident channel.  For every column `j`:

```
sum_i C(i,j) + Kd[j] <= 1
```

The column sum is the total fraction of incident channel-`j` energy
that is reflected (redistributed across outgoing channels); any
remainder is absorbed.  The factory additionally requires every
coefficient and `Kd` component to be non-negative and finite, and
`transfer` to have exactly nine entries.  Any violation makes
`CreateSpectralMixMaterial` return `NULL`.

Only constant `C` and `Kd` are supported; continuous-wavelength
sampling is not extended.

## Linear operator plumbing

* `core/spectral_transfer.h/.cpp` defines `SpectralTransfer`, a
  value-owning 3x3 operator supporting diagonal adaptation from a
  `Spectrum`, addition, scalar scaling, matrix composition, action on
  a `Spectrum` and an all-zero test.
* `BxDF` gains `fTransfer` / `Sample_fTransfer` alongside the existing
  `f` / `Sample_f`.  Ordinary lobes treat their `Spectrum` response as
  the diagonal operator `diag(f)`, so the old sampling directions,
  pdfs, flags and `Spectrum` results are unchanged.  For the transfer
  lobe the old `Spectrum` query is explicitly the diagonal part of the
  operator; a purely off-diagonal, non-zero response is therefore
  never classified as absorption.
* `BSDF` mirrors the two `Sample_f` overloads with operator forms: the
  explicit three-dimensional-sample form returns `F` and writes the
  mixture pdf, while the automatic random-sample form returns
  `F / pdf` using the same random numbers.  Component selection, the
  geometric reflection/transmission test and the mixture density are
  shared with the `Spectrum` path; non-specular events sum the
  operators of all matching lobes and specular events keep the
  discrete sampled-lobe rule.
* The path integrator accumulates an eye-to-light throughput operator
  `T`; each new scattering operator updates it as `T = T * F`, and
  emission and direct lighting are acted on by `T`.  Volume
  transmittance remains a `Spectrum` and is inserted as a diagonal
  operator at the point where it physically occurs, never moved to the
  other side of the product.  The light-sampling and BSDF-sampling MIS
  branches in `EstimateDirect`, and the `all` / `one` / `weighted`
  direct-lighting strategies, all apply the local operator to the
  incident light.  No new random numbers are consumed, so pure
  legacy-material results are unchanged for fixed samples.

## Files

* `core/spectral_transfer.h`, `core/spectral_transfer.cpp` — operator.
* `materials/spectralmix.cpp` — material and
  `extern "C" Material *CreateSpectralMixMaterial(const Transform&,
  const TextureParams&)`.
* `tests/spectral_transfer.cpp` — `algebra`, `mixture` and
  `transport` task groups (no `main`; registered via
  `task_cases` / `task_case_count` and `TASK_CHECK`).
* `examples/spectral_transfer.cpp` — renders a transfer coating to
  `spectral.ppm` in the directory given by `argv[1]`.

## Building and running

```
bash task_support/run.sh target
```

compiles the new sources, runs the three test groups and the example,
and runs the legacy texture/BSDF production baseline.  All numerical
comparisons use a fixed tolerance of `2e-5`; the transport cases use
explicitly fixed sample values (no Monte Carlo randomness) against
independently computed analytic references.
