struct S {
	vec2 c;
	float a[2];
};
void tweak(inout S v) {
	v.c.y = 9.0;
	v.a[0] = 8.0;
}
void main() {
	S s;
	s.c = vec2(1.0, 2.0);
	s.a[0] = 3.0;
	s.a[1] = 4.0;
	tweak(s);
	gl_FragColor = vec4(s.c, s.a[0], s.a[1]);
}
