//! Support module for the independent offset reference regression.
//!
//! `geometry` stores its own directed lines/arcs and computes distances, ray crossings, winding
//! numbers and signed areas in double precision. `fixtures` builds closed shape scenarios from
//! vertex/bulge data and carries hand-derived areas and calibration points. `harness` runs the
//! real `Shape::from_plines` / `Shape::parallel_offset` path and produces an independent verdict.

#![allow(dead_code)]
// Numeric reference code intentionally reads as plain formulas: small integer -> f64 literal
// conversions, explicit float comparisons, and index casts are clearer than lint-driven rewrites.
#![allow(clippy::pedantic)]

pub mod fixtures;
pub mod geometry;
pub mod harness;
