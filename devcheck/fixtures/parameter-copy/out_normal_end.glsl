void fill(out vec3 o) {
	o = vec3(5.0, 6.0, 7.0);
}
void main() {
	vec3 a = vec3(1.0, 2.0, 3.0);
	fill(a);
	gl_FragColor = vec4(a, 1.0);
}
