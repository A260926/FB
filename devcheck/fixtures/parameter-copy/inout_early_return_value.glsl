float f(inout vec2 v) {
	v.y += 2.0;
	if (v.x > 0.0)
		return 7.0;
	return 3.0;
}
void main() {
	vec2 v = vec2(1.0, 4.0);
	float r = f(v);
	gl_FragColor = vec4(v, r, 1.0);
}
