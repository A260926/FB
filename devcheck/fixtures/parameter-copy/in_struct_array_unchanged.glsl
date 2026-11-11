struct S {
	vec2 c;
	float a[2];
};
void touch(S v) {
	v.c.y = 9.0;
	v.a[0] = 8.0;
	v.a[1] = 7.0;
}
void main() {
	S a;
	a.c = vec2(1.0, 2.0);
	a.a[0] = 3.0;
	a.a[1] = 4.0;
	touch(a);
	gl_FragColor = vec4(a.c, a.a[0], a.a[1]);
}
