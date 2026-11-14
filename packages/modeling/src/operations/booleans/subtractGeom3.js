const flatten = require('../../utils/flatten')

const retessellate = require('../modifiers/retessellate')
const { isLegacyOptions } = require('./booleanOptions')

const subtractSub = require('./subtractGeom3Sub')

/*
 * Return a new 3D geometry representing space in this geometry but not in the given geometries.
 * Neither this geometry nor the given geometries are modified.
 * @param {...geom3} geometries - list of geometries
 * @returns {geom3} new 3D geometry
 */
const subtract = (...geometries) => {
  // the last argument may be the internal options marker
  const options = isLegacyOptions(geometries[geometries.length - 1]) ? geometries.pop() : undefined
  geometries = flatten(geometries)

  let newgeometry = geometries.shift()
  geometries.forEach((geometry) => {
    newgeometry = subtractSub(newgeometry, geometry, options)
  })

  newgeometry = retessellate(newgeometry, options)
  return newgeometry
}

module.exports = subtract
