# Deep-shadow lookup coordinates

Faces, in order: +X, -X, +Y, -Y, +Z, -Z. Set s=2u-1 and t=2v-1.
The inverse directions are (1,-t,-s), (-1,-t,s), (s,1,t),
(s,-1,-t), (s,-t,1), (-s,-t,-1). Normalize when casting build rays.
Select the largest absolute component; break ties X before Y before Z.
Build at u=(x+0.5)/N, v=(y+0.5)/N. Store radial distance from the light.

At lookup, first map receiver direction to its face and u/v. Let
qx=u*N-0.5, qy=v*N-0.5. The four integer neighbours of floor(qx),floor(qy)
have ordinary bilinear weights from their fractional parts. For an out-of-face
neighbour, turn that neighbour's texel-centre u/v back into a direction using
the original face, select its new face, and floor the new u*N,v*N to get the
stored texel (clamp only at this last step). Each texel evaluates its own
transmittance staircase at the receiver's radial distance minus bias.
Average the four transmittances with the bilinear weights; do not average
hit distances before looking up the staircases.

A staircase starts at T=1 and multiplies by (1-clamp(Os,0,1)) at every hit.
Hits at identical radial distance are one breakpoint with all factors applied.
T(d) includes breakpoints at distance <=d. Distinct (instance,primitive) pairs
are distinct layers, even at identical distance. Repeated references to one
pair inside grid cells count once. Build rays cover (0,maxDistance].
