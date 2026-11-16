#!/bin/bash
# End-to-end command line check for the symmetry option.
#
# Builds an OBJ and a matching symmetry file, really executes the built
# `simplify` binary, then reads the OBJ and the written `<output>.symmetry`
# back and verifies them independently.
set -euo pipefail
cd "$(dirname "$0")/.."

bin=.task-build/simplify
if [[ ! -x $bin ]]; then
  echo "missing $bin (run task_support/build.sh first)" >&2
  exit 1
fi

work=.task-build/cli
rm -rf "$work"
mkdir -p "$work"

python3 - "$work" <<'PY'
import sys
work = sys.argv[1]
n = 6
verts = []
faces = []
left = {}
right = {}
def seam(j, i):  # x = 0 column, shared by both halves
    return left[(j, i)]
for j in range(n + 1):
    for i in range(n + 1):
        left[(j, i)] = len(verts)
        x = -i / n
        y = j / n
        z = 0.30 * (1.0 - x * x) * (0.4 + 0.6 * y)
        verts.append((x, y, z))
for j in range(n + 1):
    for i in range(1, n + 1):
        right[(j, i)] = len(verts)
        x = i / n
        y = j / n
        z = 0.30 * (1.0 - x * x) * (0.4 + 0.6 * y)
        verts.append((x, y, z))
for j in range(n):
    for i in range(n):
        a = left[(j, i)]; b = left[(j, i + 1)]
        c = left[(j + 1, i)]; d = left[(j + 1, i + 1)]
        faces.append((a, b, d))
        faces.append((a, d, c))
for j in range(n):
    for i in range(n):
        a = left[(j, 0)] if i == 0 else right[(j, i)]
        b = right[(j, i + 1)]
        c = left[(j + 1, 0)] if i == 0 else right[(j + 1, i)]
        d = right[(j + 1, i + 1)]
        faces.append((a, d, b))
        faces.append((a, c, d))
mate = [-1] * len(verts)
for j in range(n + 1):
    for i in range(n + 1):
        l = left[(j, i)]
        r = l if i == 0 else right[(j, i)]
        mate[l] = r
        mate[r] = l
with open(work + "/in.obj", "w") as f:
    for (x, y, z) in verts:
        f.write("v %.17g %.17g %.17g\n" % (x, y, z))
    for t in faces:
        f.write("f %d %d %d\n" % (t[0] + 1, t[1] + 1, t[2] + 1))
with open(work + "/in.sym", "w") as f:
    f.write("1 0 0\n0\n1e-12\n%d\n" % len(verts))
    for i, m in enumerate(mate):
        f.write("%d%c" % (m, "\n" if (i + 1) % 16 == 0 else " "))
    f.write("\n")
print("wrote %d vertices %d faces" % (len(verts), len(faces)))
PY

"$bin" "$work/in.obj" "$work/out.obj" 0.2 7 --symmetry "$work/in.sym" > "$work/log.txt"

python3 - "$work" <<'PY'
import sys
work = sys.argv[1]

def read_obj(path):
    verts = []
    faces = []
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "v":
            verts.append(tuple(float(v) for v in p[1:4]))
        elif p[0] == "f":
            idx = []
            for tok in p[1:4]:
                idx.append(int(tok.split("/")[0]) - 1)
            faces.append(tuple(idx))
    return verts, faces

def read_sym(path):
    toks = open(path).read().split()
    nx, ny, nz = float(toks[0]), float(toks[1]), float(toks[2])
    off = float(toks[3])
    tol = float(toks[4])
    count = int(toks[5])
    mate = [int(t) for t in toks[6:6 + count]]
    return (nx, ny, nz), off, tol, count, mate

verts, faces = read_obj(work + "/out.obj")
assert faces, "no output faces"
normal, off, tol, count, mate = read_sym(work + "/out.obj.symmetry")
assert count == len(verts), (count, len(verts))
assert tol == 1e-12, tol
assert abs(normal[0] - 1) < 1e-12 and abs(normal[1]) < 1e-12 and abs(normal[2]) < 1e-12
assert off == 0.0

# involution and exact reflection after compaction
for i, m in enumerate(mate):
    assert 0 <= m < count, m
    assert mate[m] == i, (i, m)
    t = normal[0] * verts[i][0] + normal[1] * verts[i][1] + normal[2] * verts[i][2] + off
    r = (verts[i][0] - 2 * t * normal[0],
         verts[i][1] - 2 * t * normal[1],
         verts[i][2] - 2 * t * normal[2])
    d = sum((r[k] - verts[m][k]) ** 2 for k in range(3)) ** 0.5
    assert d <= 1e-9, (i, m, d)
    if m == i:
        assert abs(t) <= 1e-9, (i, t)

# face set invariance under reflection with reversed winding
faceset = set(tuple(sorted(f)) for f in faces)
for f in faces:
    a, b, c = f
    key = tuple(sorted((mate[c], mate[b], mate[a])))
    assert key in faceset, f
assert len(faceset) == len(faces), "duplicate faces"

# manifold edge usage and positive area
edges = {}
for f in faces:
    for k in range(3):
        x, y = f[k], f[(k + 1) % 3]
        e = (min(x, y), max(x, y))
        edges[e] = edges.get(e, 0) + 1
        a, b, c = verts[f[0]], verts[f[1]], verts[f[2]]
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        cr = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        assert sum(x * x for x in cr) > 0
for e, c in edges.items():
    assert c <= 2, e

# both halves survived and the plane is exercised
assert any(v[0] < -1e-9 for v in verts)
assert any(v[0] > 1e-9 for v in verts)
print("CLI_OK faces=%d vertices=%d self_mates=%d" %
      (len(faces), len(verts), sum(1 for i, m in enumerate(mate) if i == m)))
PY

# Bad symmetry files must exit non-zero and leave no output.
bad=0
for payload in \
    "1 0 0
0
1e-9
1
5" \
    "2 0 0
0
1e-9
1
0" \
    "1 0 0
0
-1
1
0" \
    "1 0 0
nan
1e-9
1
0" \
    "1 0 0
0
1e-9
2
0 0"; do
  printf '%s\n' "$payload" > "$work/bad.sym"
  rm -f "$work/bad.obj" "$work/bad.obj.symmetry"
  if "$bin" "$work/in.obj" "$work/bad.obj" 0.5 7 --symmetry "$work/bad.sym" \
      > "$work/bad.log" 2>&1; then
    echo "expected failure for: $payload" >&2
    bad=1
  fi
  [[ -e $work/bad.obj.symmetry ]] && { echo "unexpected symmetry output" >&2; bad=1; }
done
[[ $bad == 0 ]] || exit 1
echo "CLI_BAD_INPUT_REJECTED"
