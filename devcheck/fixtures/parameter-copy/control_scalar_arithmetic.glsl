float twice(float x) {
	return x * 2.0;
}
void main() {
	gl_FragColor = vec4(twice(1.5), twice(2.0), 0.0, 1.0);
}
