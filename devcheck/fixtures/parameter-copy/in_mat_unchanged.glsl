void touch(mat2 m) {
	m[0][0] = 9.0;
	m[1][1] = 8.0;
}
void main() {
	mat2 m = mat2(1.0, 2.0, 3.0, 4.0);
	touch(m);
	gl_FragColor = vec4(m[0], m[1]);
}
