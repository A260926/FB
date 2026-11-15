#include "Interpolation.h"

std::vector<float> interpolateSingleFloats(float from, float to, size_t numberOfValues) {
	std::vector<float> result;
	if (numberOfValues == 0)
		return result;
	if (numberOfValues == 1) {
		result.push_back(from);
		return result;
	}
	float step = (to - from) / (numberOfValues - 1);
	for (size_t i = 0; i < numberOfValues; i++) {
		result.push_back(from + (i * step));
	}
	return result;
}

std::vector<glm::vec3> interpolateThreeElementValues(const glm::vec3 &from, const glm::vec3 &to, int numberOfValues) {
	std::vector<glm::vec3> result;
	if (numberOfValues <= 0)
		return result;
	if (numberOfValues == 1) {
		result.push_back(from);
		return result;
	}
	float stepSize = 1.0f / (numberOfValues - 1);
	for (int i = 0; i < numberOfValues; i++) {
		float t = stepSize * i;
		result.push_back(from + (t * (to - from)));
	}
	return result;
}

float interpolation(float x, float x1, float x2, float y1, float y2) {
	if (x2 == x1)
		return y1;
	return y1 + (x - x1) * ((y2 - y1) / (x2 - x1));
}
