void pair(inout vec2 p) {
	p = vec2(7.0, 8.0);
}
void main() {
	vec3 v = vec3(1.0, 2.0, 3.0);
	pair(v.zx);
	gl_FragColor = vec4(v, 1.0);
}
