float pick(out vec2 o, float flag) {
	if (flag > 0.0) {
		o = vec2(7.0, 8.0);
		return 7.0;
	}
	o = vec2(1.0, 1.0);
	return 3.0;
}
void main() {
	vec2 o = vec2(0.0, 0.0);
	float r = pick(o, 1.0);
	gl_FragColor = vec4(o.x, o.y, r, 1.0);
}
