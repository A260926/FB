'use strict';

//
// Surface fairing: drag a few parameter samples and let the rest of the
// surface follow smoothly.
//
// createFairingPlan(spline, { anchors, lambda }) snapshots a reference
// surface and pre-assembles the metric energy operator and the fixed
// constraint factorization. The returned plan can be solved repeatedly with
// new target positions:
//
//   var plan = createFairingPlan(surface, { anchors: [[0.2, 0.5]], lambda: 1e-3 });
//   var result = plan.solve([[[0.1, 0.2, 0.6]]]);
//
// Each solve() returns { status, surface, energy, constraintResidual,
// stationarityResidual }. The surface is an independent spline built with
// the original nurbs constructor; only its control point positions differ
// from the reference.
//

var createFairingPlan = require('../src/variational/fairing-plan');

// The entry point is callable directly and is also available as a named
// `createFairingPlan` property for destructuring consumers.
module.exports = createFairingPlan;
module.exports.createFairingPlan = createFairingPlan;
