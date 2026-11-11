void add(inout float x) {
	x = x + 5.0;
}
void main() {
	float x = 1.0;
	add(x);
	gl_FragColor = vec4(x, 0.0, 0.0, 1.0);
}
