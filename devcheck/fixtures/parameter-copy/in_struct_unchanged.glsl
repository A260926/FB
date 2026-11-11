struct S {
	vec2 c;
	float s;
};
void touch(S v) {
	v.c.x = 9.0;
	v.s = 8.0;
}
void main() {
	S a;
	a.c = vec2(1.0, 2.0);
	a.s = 3.0;
	touch(a);
	gl_FragColor = vec4(a.c, a.s, 1.0);
}
