void bump(inout float x) {
	x += 3.0;
}
void main() {
	vec2 a[2];
	a[0] = vec2(1.0, 2.0);
	a[1] = vec2(3.0, 4.0);
	int k = 0;
	bump(a[k++].y);
	gl_FragColor = vec4(a[0], float(k), a[1].y);
}
