struct S {
	vec2 c;
};
void set9(out float x) {
	x = 9.0;
}
void main() {
	S s;
	s.c = vec2(1.0, 2.0);
	set9(s.c.y);
	gl_FragColor = vec4(s.c, 0.0, 1.0);
}
