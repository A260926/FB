void set9(out float x) {
	x = 9.0;
}
void main() {
	mat2 m = mat2(1.0, 2.0, 3.0, 4.0);
	set9(m[0][1]);
	gl_FragColor = vec4(m[0], m[1]);
}
