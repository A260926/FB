float f(out float o, out vec2 v) {
	o = 2.5;
	v = vec2(4.0, 6.0);
	return 9.0;
}
void main() {
	float o = 0.0;
	vec2 v = vec2(0.0, 0.0);
	float r = f(o, v);
	gl_FragColor = vec4(o, v.x, v.y, r);
}
