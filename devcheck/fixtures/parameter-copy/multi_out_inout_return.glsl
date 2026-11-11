float f(out float a, out vec2 b, inout vec2 c) {
	a = 0.25;
	b = vec2(2.0, 3.0);
	c = b;
	return 0.5;
}
void main() {
	float a = 0.0;
	vec2 b = vec2(0.0, 0.0);
	vec2 c = vec2(9.0, 9.0);
	float r = f(a, b, c);
	gl_FragColor = vec4(a, c.x, c.y, r);
}
