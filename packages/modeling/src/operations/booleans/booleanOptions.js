/*
 * Internal options for the 3D boolean pipeline.
 *
 * The public boolean API is unchanged. These options exist only so the 2D
 * boolean path (which builds temporary 3D walls) can keep the historical
 * absolute tolerances / spatial resolution, while the 3D path uses scale aware
 * tolerances.
 */
const legacyOptions = Object.freeze({ legacy: true })

/*
 * True when the given value is the internal legacy options marker.
 */
const isLegacyOptions = (value) => value === legacyOptions

module.exports = {
  legacyOptions,
  isLegacyOptions
}
