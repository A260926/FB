const flatten = require('../../utils/flatten')

const retessellate = require('../modifiers/retessellate')
const { isLegacyOptions } = require('./booleanOptions')

const unionSub = require('./unionGeom3Sub')

/*
 * Return a new 3D geometry representing the space in the given 3D geometries.
 * @param {...objects} geometries - list of geometries to union
 * @returns {geom3} new 3D geometry
 */
const union = (...geometries) => {
  // the last argument may be the internal options marker
  const options = isLegacyOptions(geometries[geometries.length - 1]) ? geometries.pop() : undefined
  geometries = flatten(geometries)

  // combine geometries in a way that forms a balanced binary tree pattern
  let i
  for (i = 1; i < geometries.length; i += 2) {
    geometries.push(unionSub(geometries[i - 1], geometries[i], options))
  }
  let newgeometry = geometries[i - 1]
  newgeometry = retessellate(newgeometry, options)
  return newgeometry
}

module.exports = union
