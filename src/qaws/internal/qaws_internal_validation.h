#ifndef QAWS_INTERNAL_VALIDATION_H
#define QAWS_INTERNAL_VALIDATION_H

#include "../qaws_types.h"
#include "../qaws_status.h"

qaws_status qaws_internal_validate_dimension(qaws_dimension dimension);

qaws_status qaws_internal_validate_knot_vector(
	qaws_scalar const* knots,
	unsigned int knot_count,
	unsigned int degree,
	unsigned int control_point_count);

qaws_status qaws_internal_validate_weights(
	qaws_scalar const* weights,
	unsigned int weight_count);

#endif /* QAWS_INTERNAL_VALIDATION_H */
