struct S {
	float a;
	float b;
};
void mixv(S v) {
	v.a = 9.0;
	v.b = 8.0;
}
void main() {
	S s;
	s.a = 1.0;
	s.b = 2.0;
	mixv(s);
	gl_FragColor = vec4(s.a, s.b, 0.0, 1.0);
}
