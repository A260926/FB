void touch(vec3 v) {
	v.x = 9.0;
	v.z = 7.0;
}
void main() {
	vec3 a = vec3(1.0, 2.0, 3.0);
	touch(a);
	gl_FragColor = vec4(a, 1.0);
}
