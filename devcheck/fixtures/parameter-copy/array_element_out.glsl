void set9(out float x) {
	x = 9.0;
}
void main() {
	float a[2];
	a[0] = 1.0;
	a[1] = 2.0;
	set9(a[1]);
	gl_FragColor = vec4(a[0], a[1], 0.0, 1.0);
}
