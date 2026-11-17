#include "qaws_internal_validation.h"

qaws_status qaws_internal_validate_dimension(qaws_dimension dimension)
{
	if (dimension == QAWS_DIMENSION_2D || dimension == QAWS_DIMENSION_3D)
		return QAWS_STATUS_OK;
	return QAWS_STATUS_INVALID_DIMENSION;
}

qaws_status qaws_internal_validate_knot_vector(
	qaws_scalar const* knots,
	unsigned int knot_count,
	unsigned int degree,
	unsigned int control_point_count)
{
	if (!knots || knot_count == 0)
		return QAWS_STATUS_INVALID_KNOT_COUNT;

	if (knot_count != control_point_count + degree + 1)
		return QAWS_STATUS_INVALID_KNOT_COUNT;

	for (unsigned int i = 1; i < knot_count; i++) {
		if (knots[i] < knots[i - 1])
			return QAWS_STATUS_INVALID_KNOT_VECTOR;
	}

	return QAWS_STATUS_OK;
}

qaws_status qaws_internal_validate_weights(
	qaws_scalar const* weights,
	unsigned int weight_count)
{
	if (!weights || weight_count == 0)
		return QAWS_STATUS_INVALID_WEIGHT_COUNT;

	for (unsigned int i = 0; i < weight_count; i++) {
		if (weights[i] <= (qaws_scalar)0)
			return QAWS_STATUS_INVALID_WEIGHT_COUNT;
	}

	return QAWS_STATUS_OK;
}
