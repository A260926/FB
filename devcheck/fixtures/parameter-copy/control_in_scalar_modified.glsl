void bump(float x) {
	x = x + 3.0;
}
void main() {
	float x = 1.0;
	bump(x);
	gl_FragColor = vec4(x, 2.0, 3.0, 1.0);
}
