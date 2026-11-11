void pair(inout vec2 p) {
	p = vec2(7.0, 8.0);
}
void main() {
	mat2 m = mat2(1.0, 2.0, 3.0, 4.0);
	pair(m[1]);
	gl_FragColor = vec4(m[0], m[1]);
}
