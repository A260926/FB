void tweak(inout vec3 v) {
	v.y = v.y + 10.0;
}
void main() {
	vec3 v = vec3(1.0, 2.0, 3.0);
	tweak(v);
	gl_FragColor = vec4(v, 1.0);
}
