void pick(out vec3 o, float flag) {
	if (flag > 0.0) {
		o = vec3(7.0, 8.0, 9.0);
		return;
	}
	o = vec3(1.0, 1.0, 1.0);
}
void main() {
	vec3 a = vec3(2.0, 2.0, 2.0);
	pick(a, 1.0);
	gl_FragColor = vec4(a, 1.0);
}
