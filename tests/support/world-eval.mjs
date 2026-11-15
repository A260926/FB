/**
 * Minimal CPU evaluation of the vertex stage used by the headless tests.
 *
 * The functions below are independent implementations of the world position
 * and world normal transform. They intentionally do not use the instance
 * batch matrices to validate themselves: given an explicit 4x4 matrix they
 * compute the transformed point or normal from scratch, so the result can be
 * compared against the unbatched, per-object draw path.
 */

/**
 * Multiply a column-major 4x4 matrix with a 3d point.
 * @param {ArrayLike<number>} m - Column-major 4x4 matrix.
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @returns {number[]} The transformed point.
 */
function transformPoint(m, x, y, z) {
	const w = m[3] * x + m[7] * y + m[11] * z + m[15];
	const rx = m[0] * x + m[4] * y + m[8] * z + m[12];
	const ry = m[1] * x + m[5] * y + m[9] * z + m[13];
	const rz = m[2] * x + m[6] * y + m[10] * z + m[14];
	const inv = w !== 0 ? 1 / w : 1;
	return [rx * inv, ry * inv, rz * inv];
}

/**
 * Multiply a column-major 4x4 matrix with a direction (w = 0).
 * @param {ArrayLike<number>} m
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @returns {number[]} The transformed direction.
 */
function transformDirection(m, x, y, z) {
	return [
		m[0] * x + m[4] * y + m[8] * z,
		m[1] * x + m[5] * y + m[9] * z,
		m[2] * x + m[6] * y + m[10] * z
	];
}

/**
 * Compute the inverse-transpose of the upper 3x3 of a column-major 4x4 matrix.
 *
 * The full linear part is inverted before transposing, so shear, non-uniform
 * scaling and reflections are handled correctly.
 *
 * The 3x3 matrix is stored column-major as
 *   A = [a00 a01 a02; a10 a11 a12; a20 a21 a22]
 * where aij is row i, column j.
 * @param {ArrayLike<number>} m - Column-major 4x4 matrix.
 * @returns {number[]} Column-major 3x3 inverse-transpose.
 */
function normalMatrix(m) {
	const a00 = m[0], a10 = m[1], a20 = m[2];
	const a01 = m[4], a11 = m[5], a21 = m[6];
	const a02 = m[8], a12 = m[9], a22 = m[10];

	// Cofactor C_ij uses the row/column product cross pattern. Cofactor C_ij
	// (row i, column j) is (-1)^(i+j) times the minor of row i / column j.
	const c00 = a11 * a22 - a12 * a21;
	const c01 = a12 * a20 - a10 * a22;
	const c02 = a10 * a21 - a11 * a20;
	const c10 = a02 * a21 - a01 * a22;
	const c11 = a00 * a22 - a02 * a20;
	const c12 = a01 * a20 - a00 * a21;
	const c20 = a01 * a12 - a02 * a11;
	const c21 = a02 * a10 - a00 * a12;
	const c22 = a00 * a11 - a01 * a10;

	const det = a00 * c00 + a01 * c01 + a02 * c02;
	const invDet = det !== 0 ? 1 / det : 0;

	// inverse-transpose = cofactor / det. Return it column-major: entry [i][j]
	// of the inverse-transpose is C_ji / det.
	return [
		c00 * invDet, c10 * invDet, c20 * invDet,
		c01 * invDet, c11 * invDet, c21 * invDet,
		c02 * invDet, c12 * invDet, c22 * invDet
	];
}

/**
 * Transform a normal by the inverse-transpose of a model matrix, then normalize.
 * @param {ArrayLike<number>} m - Column-major 4x4 matrix.
 * @param {number} x
 * @param {number} y
 * @param {number} z
 * @returns {number[]} The normalized world normal.
 */
function transformNormal(m, x, y, z) {
	const n = normalMatrix(m);
	// n is column-major 3x3; entry [i][j] is n[j * 3 + i].
	const rx = n[0] * x + n[3] * y + n[6] * z;
	const ry = n[1] * x + n[4] * y + n[7] * z;
	const rz = n[2] * x + n[5] * y + n[8] * z;
	const length = Math.sqrt(rx * rx + ry * ry + rz * rz) || 1;
	return [rx / length, ry / length, rz / length];
}

/**
 * Compute the normal (cross product of the two triangle edges) of a world-space triangle.
 * @param {number[][]} a
 * @param {number[][]} b
 * @param {number[][]} c
 * @returns {number[]} The unnormalized face normal.
 */
function faceNormal(a, b, c) {
	const abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
	const acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
	return [
		aby * acz - abz * acy,
		abz * acx - abx * acz,
		abx * acy - aby * acx
	];
}

export { transformPoint, transformDirection, normalMatrix, transformNormal, faceNormal };