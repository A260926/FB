struct S {
	vec2 c;
};
S touch(S s) {
	s.c.x = 9.0;
	return s;
}
void main() {
	S a = S(vec2(1.0, 2.0));
	S b = touch(a);
	b.c.y = 7.0;
	gl_FragColor = vec4(a.c, b.c);
}
