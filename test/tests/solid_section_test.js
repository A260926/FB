import assert from 'node:assert/strict';
import * as OV from '../../source/engine/main.js';
import * as THREE from 'three';
import { MakeSectionFixture } from '../../checks/section-fixture.mjs';
import { FileFormat } from '../../source/engine/io/fileutils.js';
import { ExporterStl } from '../../source/engine/export/exporterstl.js';
import { ExporterObj } from '../../source/engine/export/exporterobj.js';

const Eps = 1e-7;

function ApproxEqual (a, b, eps)
{
    return Math.abs (a - b) < (eps || 1e-9);
}

function TriangleArea3D (v0, v1, v2)
{
    const e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
    const e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;
    const cx = e1y * e2z - e1z * e2y;
    const cy = e1z * e2x - e1x * e2z;
    const cz = e1x * e2y - e1y * e2x;
    return Math.sqrt (cx * cx + cy * cy + cz * cz) / 2.0;
}

function SignedArea2D (points)
{
    let area = 0.0;
    for (let i = 0; i < points.length; i++) {
        const current = points[i];
        const next = points[(i + 1) % points.length];
        area += current[0] * next[1] - next[0] * current[1];
    }
    return area / 2.0;
}

function PositionKey (coord, eps)
{
    const quantize = (value) => Math.round (value / (eps || 1e-6)).toString ();
    return quantize (coord.x) + '|' + quantize (coord.y) + '|' + quantize (coord.z);
}

// Independent closure check: every geometric edge must appear exactly twice,
// once in each direction. Positions are compared geometrically, not by index.
function FindOpenEdges (mesh)
{
    const directedCounts = new Map ();
    mesh.EnumerateTriangleVertices ((v0, v1, v2) => {
        const points = [v0, v1, v2];
        for (let edge = 0; edge < 3; edge++) {
            const key = PositionKey (points[edge]) + '>' + PositionKey (points[(edge + 1) % 3]);
            directedCounts.set (key, (directedCounts.get (key) || 0) + 1);
        }
    });
    const openEdges = [];
    for (const [key, count] of directedCounts) {
        const reversed = key.split ('>').reverse ().join ('>');
        if (directedCounts.get (reversed) !== count) {
            openEdges.push (key);
        }
    }
    return openEdges;
}

function CountTrianglesWithMaterial (mesh, materialIndex)
{
    let count = 0;
    for (let i = 0; i < mesh.TriangleCount (); i++) {
        if (mesh.GetTriangle (i).mat === materialIndex) {
            count += 1;
        }
    }
    return count;
}

function SumMaterialArea (mesh, materialIndex)
{
    let area = 0.0;
    for (let i = 0; i < mesh.TriangleCount (); i++) {
        const triangle = mesh.GetTriangle (i);
        if (triangle.mat !== materialIndex) {
            continue;
        }
        area += TriangleArea3D (
            mesh.GetVertex (triangle.v0),
            mesh.GetVertex (triangle.v1),
            mesh.GetVertex (triangle.v2)
        );
    }
    return area;
}

function PointInTriangle3D (point, v0, v1, v2, eps)
{
    const normal = OV.CrossVector3D (OV.SubCoord3D (v1, v0), OV.SubCoord3D (v2, v0));
    const normalLength = normal.Length ();
    if (normalLength < 1e-12) {
        return false;
    }
    const toPoint = OV.SubCoord3D (point, v0);
    if (Math.abs (OV.DotVector3D (normal, toPoint)) > (eps || 1e-6) * normalLength) {
        return false;
    }
    const cross0 = OV.CrossVector3D (OV.SubCoord3D (v1, v0), OV.SubCoord3D (point, v0));
    const cross1 = OV.CrossVector3D (OV.SubCoord3D (v2, v1), OV.SubCoord3D (point, v1));
    const cross2 = OV.CrossVector3D (OV.SubCoord3D (v0, v2), OV.SubCoord3D (point, v2));
    const dot0 = OV.DotVector3D (cross0, normal);
    const dot1 = OV.DotVector3D (cross1, normal);
    const dot2 = OV.DotVector3D (cross2, normal);
    return dot0 >= -(eps || 1e-6) && dot1 >= -(eps || 1e-6) && dot2 >= -(eps || 1e-6);
}

function CapCoversPoint (mesh, capMaterialIndex, point)
{
    for (let i = 0; i < mesh.TriangleCount (); i++) {
        const triangle = mesh.GetTriangle (i);
        if (triangle.mat !== capMaterialIndex) {
            continue;
        }
        if (PointInTriangle3D (point,
            mesh.GetVertex (triangle.v0),
            mesh.GetVertex (triangle.v1),
            mesh.GetVertex (triangle.v2), 1e-6)) {
            return true;
        }
    }
    return false;
}

function Cross2D (a, b, c)
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

// Independent ear-clipping triangulation for a single simple polygon. Works
// for both orientations by triangulating a counter clockwise copy.
function TriangulatePolygon2D (polygon)
{
    const isClockwise = SignedArea2D (polygon) < 0.0;
    const orientation = isClockwise ? polygon.map ((point, index) => index).reverse () : polygon.map ((point, index) => index);
    const remaining = orientation.slice ();
    const triangles = [];
    let guard = 0;
    while (remaining.length > 3 && guard < 10000) {
        guard += 1;
        let clipped = false;
        for (let i = 0; i < remaining.length; i++) {
            const prevIndex = remaining[(i + remaining.length - 1) % remaining.length];
            const currentIndex = remaining[i];
            const nextIndex = remaining[(i + 1) % remaining.length];
            if (Cross2D (polygon[prevIndex], polygon[currentIndex], polygon[nextIndex]) <= 1e-12) {
                continue;
            }
            let containsOther = false;
            for (const otherIndex of remaining) {
                if (otherIndex === prevIndex || otherIndex === currentIndex || otherIndex === nextIndex) {
                    continue;
                }
                const p = polygon[otherIndex];
                const c0 = Cross2D (polygon[prevIndex], polygon[currentIndex], p);
                const c1 = Cross2D (polygon[currentIndex], polygon[nextIndex], p);
                const c2 = Cross2D (polygon[nextIndex], polygon[prevIndex], p);
                if (c0 >= -1e-12 && c1 >= -1e-12 && c2 >= -1e-12) {
                    containsOther = true;
                    break;
                }
            }
            if (containsOther) {
                continue;
            }
            triangles.push ([prevIndex, currentIndex, nextIndex]);
            remaining.splice (i, 1);
            clipped = true;
            break;
        }
        if (!clipped) {
            break;
        }
    }
    if (remaining.length === 3) {
        triangles.push ([remaining[0], remaining[1], remaining[2]]);
    }
    return triangles;
}

// Builds a closed prism for one polygon loop. Outer loops must be counter
// clockwise, hole loops clockwise, so the generated faces point outward.
function AddPrismLoop (mesh, loop, zMin, zMax, materialIndex)
{
    const bottom = [];
    const top = [];
    for (const [x, y] of loop) {
        bottom.push (mesh.AddVertex (new OV.Coord3D (x, y, zMin)));
        top.push (mesh.AddVertex (new OV.Coord3D (x, y, zMax)));
    }
    for (let i = 0; i < loop.length; i++) {
        const j = (i + 1) % loop.length;
        const triangle0 = new OV.Triangle (bottom[i], bottom[j], top[j]).SetMaterial (materialIndex);
        const triangle1 = new OV.Triangle (bottom[i], top[j], top[i]).SetMaterial (materialIndex);
        mesh.AddTriangle (triangle0);
        mesh.AddTriangle (triangle1);
    }
    const isOuter = SignedArea2D (loop) > 0.0;
    const triangles = TriangulatePolygon2D (loop);
    for (const [i0, i1, i2] of triangles) {
        if (isOuter) {
            mesh.AddTriangle (new OV.Triangle (top[i0], top[i1], top[i2]).SetMaterial (materialIndex));
            mesh.AddTriangle (new OV.Triangle (bottom[i0], bottom[i2], bottom[i1]).SetMaterial (materialIndex));
        } else {
            mesh.AddTriangle (new OV.Triangle (top[i0], top[i2], top[i1]).SetMaterial (materialIndex));
            mesh.AddTriangle (new OV.Triangle (bottom[i0], bottom[i1], bottom[i2]).SetMaterial (materialIndex));
        }
    }
}

function CreateCavityIslandModel ()
{
    const model = new OV.Model ();
    const wall = new OV.PhongMaterial ();
    wall.name = 'wall';
    const cap = new OV.PhongMaterial ();
    cap.name = 'cap';
    model.AddMaterial (wall);
    model.AddMaterial (cap);

    // L-shaped concave outer profile.
    const outer = [[0.0, 0.0], [4.0, 0.0], [4.0, 4.0], [3.0, 4.0], [3.0, 1.0], [1.0, 1.0], [1.0, 3.0], [0.0, 3.0]];
    if (SignedArea2D (outer) < 0.0) {
        outer.reverse ();
    }
    // Rectangular cavity inside the bottom band of the L.
    let cavity = [[1.5, 0.3], [2.5, 0.3], [2.5, 0.8], [1.5, 0.8]];
    if (SignedArea2D (cavity) > 0.0) {
        cavity.reverse ();
    }
    const outerMesh = new OV.Mesh ();
    outerMesh.SetName ('concave-with-cavity');
    AddPrismLoop (outerMesh, outer, 0.0, 4.0, 0);
    AddPrismLoop (outerMesh, cavity, 0.0, 4.0, 0);

    // Solid island floating inside the cavity: a separate closed shell.
    const island = [[1.8, 0.4], [2.2, 0.4], [2.2, 0.7], [1.8, 0.7]];
    if (SignedArea2D (island) < 0.0) {
        island.reverse ();
    }
    const islandMesh = new OV.Mesh ();
    islandMesh.SetName ('island');
    AddPrismLoop (islandMesh, island, 0.0, 4.0, 0);

    model.AddMeshToRootNode (outerMesh);
    model.AddMeshToRootNode (islandMesh);
    OV.FinalizeModel (model, {});
    return {
        model : model,
        outer : outer,
        cavity : cavity,
        island : island
    };
}

function CreateSeamMesh ()
{
    // A closed unit cube where every face stores its own copy of the shared
    // corner positions, each with face-specific UVs and colors. Geometric
    // seams therefore carry several attribute sets that must not be welded.
    // On the first face one corner normal points inward, so interpolating it
    // at the cut produces a zero normal that has to be kept as zero.
    const mesh = new OV.Mesh ();
    mesh.SetName ('seam-cube');
    const z0 = 0.0;
    const z1 = 1.0;
    const faces = [
        { corners : [[0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]], normal : [0, 0, 1] },
        { corners : [[0, 0, 0], [0, 1, 0], [1, 1, 0], [1, 0, 0]], normal : [0, 0, -1] },
        { corners : [[0, 1, 0], [0, 1, 1], [1, 1, 1], [1, 1, 0]], normal : [0, 1, 0] },
        { corners : [[0, 0, 0], [1, 0, 0], [1, 0, 1], [0, 0, 1]], normal : [0, -1, 0] },
        { corners : [[1, 0, 0], [1, 1, 0], [1, 1, 1], [1, 0, 1]], normal : [1, 0, 0] },
        { corners : [[0, 0, 0], [0, 0, 1], [0, 1, 1], [0, 1, 0]], normal : [-1, 0, 0] }
    ];
    faces.forEach ((face, faceIndex) => {
        const vertexIndices = [];
        const colorIndices = [];
        const normalIndices = [];
        const uvIndices = [];
        face.corners.forEach ((corner, index) => {
            vertexIndices.push (mesh.AddVertex (new OV.Coord3D (corner[0], corner[1], corner[2])));
            const top = corner[2] === z1;
            colorIndices.push (mesh.AddVertexColor (new OV.RGBColor (
                faceIndex * 30 + (top ? 40 : 10),
                top ? 200 : 10,
                top ? 0 : 100
            )));
            let normalValues = face.normal;
            if (faceIndex === 3 && index === 0) {
                normalValues = [0, 1, 0];
            }
            normalIndices.push (mesh.AddNormal (new OV.Coord3D (normalValues[0], normalValues[1], normalValues[2])));
            uvIndices.push (mesh.AddTextureUV (new OV.Coord2D (
                (index % 2) + faceIndex * 10.0,
                top ? 1.0 : 0.0
            )));
        });
        for (const cornerIndices of [[0, 1, 2], [0, 2, 3]]) {
            const triangle = new OV.Triangle (
                vertexIndices[cornerIndices[0]],
                vertexIndices[cornerIndices[1]],
                vertexIndices[cornerIndices[2]]
            );
            triangle.SetVertexColors (
                colorIndices[cornerIndices[0]],
                colorIndices[cornerIndices[1]],
                colorIndices[cornerIndices[2]]
            );
            triangle.SetNormals (
                normalIndices[cornerIndices[0]],
                normalIndices[cornerIndices[1]],
                normalIndices[cornerIndices[2]]
            );
            triangle.SetTextureUVs (
                uvIndices[cornerIndices[0]],
                uvIndices[cornerIndices[1]],
                uvIndices[cornerIndices[2]]
            );
            triangle.SetMaterial (0);
            mesh.AddTriangle (triangle);
        }
    });
    return mesh;
}

function ConvertToThree (model, params)
{
    return new Promise ((resolve) => {
        OV.ConvertModelToThreeObject (
            model, params, new OV.ModelToThreeConversionOutput (),
            { onTextureLoaded () {}, onModelLoaded : resolve }
        );
    });
}

function ExportFiles (exporter, exporterModel, format)
{
    return new Promise ((resolve) => {
        exporter.Export (exporterModel, format, resolve);
    });
}

describe ('Solid Section', function () {
    this.timeout (20000);

    it ('section-solid-half', function () {
        const fixture = MakeSectionFixture ();
        const originalVertexSnapshot = fixture.mesh.GetVertex (0).Clone ();

        const clipped = OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 1.0),
            constant : -2.0,
            capMaterialIndex : 1
        });

        // Independently computed expectations for the rectangular tube.
        assert.ok (ApproxEqual (OV.CalculateVolume (clipped), 40.0, 1e-8));
        assert.ok (ApproxEqual (SumMaterialArea (clipped, 1), 20.0, 1e-8));
        assert.deepEqual (FindOpenEdges (clipped), []);
        assert.ok (OV.IsTwoManifold (clipped));

        // The center bore must stay open: the cap must not cover (0, 0, 2).
        assert.equal (CapCoversPoint (clipped, 1, new OV.Coord3D (0.0, 0.0, 2.0)), false);
        // But the wall ring must be covered, here at (2, 0, 2).
        assert.equal (CapCoversPoint (clipped, 1, new OV.Coord3D (2.0, 0.0, 2.0)), true);

        // Everything is clipped away below the solid.
        const empty = OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 1.0),
            constant : 1.0,
            capMaterialIndex : 1
        });
        assert.equal (empty.TriangleCount (), 0);

        // A plane above the solid keeps an independent, equivalent mesh.
        const full = OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 1.0),
            constant : -10.0,
            capMaterialIndex : 1
        });
        assert.equal (full.TriangleCount (), fixture.mesh.TriangleCount ());
        assert.ok (ApproxEqual (OV.CalculateVolume (full), 80.0, 1e-8));
        full.GetVertex (0).x = 123.0;
        assert.equal (fixture.mesh.GetVertex (0).x, originalVertexSnapshot.x);

        // Non-unit normals are normalized together with the constant.
        const scaledNormal = OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 2.0),
            constant : -4.0,
            capMaterialIndex : 1
        });
        assert.ok (ApproxEqual (OV.CalculateVolume (scaledNormal), 40.0, 1e-8));

        // The source mesh must stay untouched.
        assert.equal (fixture.mesh.TriangleCount (), 32);
        assert.ok (OV.CoordIsEqual3D (fixture.mesh.GetVertex (0), originalVertexSnapshot));

        assert.throws (() => OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 0.0), constant : -2.0, capMaterialIndex : 1
        }), RangeError);
        assert.throws (() => OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, Number.NaN), constant : -2.0, capMaterialIndex : 1
        }), RangeError);
        assert.throws (() => OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 1.0), constant : -2.0, capMaterialIndex : -1
        }), RangeError);
        assert.throws (() => OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 1.0), constant : -2.0, capMaterialIndex : 1.5
        }), RangeError);
    });

    it ('section-cavity-and-island', function () {
        const fixture = CreateCavityIslandModel ();
        const expectedCapArea = Math.abs (SignedArea2D (fixture.outer)) -
            Math.abs (SignedArea2D (fixture.cavity)) + Math.abs (SignedArea2D (fixture.island));

        const clippedMeshes = [];
        fixture.model.EnumerateMeshInstances ((meshInstance) => {
            clippedMeshes.push (OV.ClipMeshByPlane (meshInstance.GetMesh (), {
                normal : new OV.Coord3D (0.0, 0.0, 1.0),
                constant : -2.0,
                capMaterialIndex : 1
            }));
        });

        let capArea = 0.0;
        for (const clipped of clippedMeshes) {
            assert.deepEqual (FindOpenEdges (clipped), []);
            assert.ok (OV.IsTwoManifold (clipped));
            capArea += SumMaterialArea (clipped, 1);
            for (let i = 0; i < clipped.TriangleCount (); i++) {
                const triangle = clipped.GetTriangle (i);
                for (const vertexIndex of [triangle.v0, triangle.v1, triangle.v2]) {
                    const vertex = clipped.GetVertex (vertexIndex);
                    assert.ok (vertex.z <= 2.0 + Eps);
                }
            }
        }
        assert.ok (ApproxEqual (capArea, expectedCapArea, 1e-8));

        // The cavity stays empty, the island stays filled.
        const combined = clippedMeshes[0];
        assert.equal (CapCoversPoint (combined, 1, new OV.Coord3D (2.0, 0.55, 2.0)), false);
        assert.equal (CapCoversPoint (combined, 1, new OV.Coord3D (0.5, 0.5, 2.0)), true);
        assert.equal (CapCoversPoint (clippedMeshes[1], 1, new OV.Coord3D (2.0, 0.55, 2.0)), true);

        // A non-coaxial plane: the result stays closed and cap faces follow the plane.
        const planeNormal = new OV.Coord3D (0.0, 0.6, 0.8);
        const length = planeNormal.Length ();
        planeNormal.MultiplyScalar (1.0 / length);
        const slanted = OV.ClipMeshByPlane (fixture.model.GetMesh (0), {
            normal : new OV.Coord3D (0.0, 0.6, 0.8),
            constant : -2.4,
            capMaterialIndex : 1
        });
        assert.deepEqual (FindOpenEdges (slanted), []);
        assert.ok (OV.IsTwoManifold (slanted));
        for (let i = 0; i < slanted.TriangleCount (); i++) {
            const triangle = slanted.GetTriangle (i);
            for (const vertexIndex of [triangle.v0, triangle.v1, triangle.v2]) {
                const vertex = slanted.GetVertex (vertexIndex);
                assert.ok (0.6 * vertex.y + 0.8 * vertex.z - 2.4 <= Eps);
            }
            if (triangle.mat === 1) {
                const geometricNormal = OV.CalculateTriangleNormal (
                    slanted.GetVertex (triangle.v0),
                    slanted.GetVertex (triangle.v1),
                    slanted.GetVertex (triangle.v2)
                );
                assert.ok (ApproxEqual (OV.DotVector3D (geometricNormal, planeNormal), 1.0, 1e-7));
            }
        }
    });

    it ('section-seam-attributes', function () {
        const seamMesh = CreateSeamMesh ();
        const clipped = OV.ClipMeshByPlane (seamMesh, {
            normal : new OV.Coord3D (0.0, 0.0, 1.0),
            constant : -0.5,
            capMaterialIndex : 0
        });
        assert.deepEqual (FindOpenEdges (clipped), []);

        // Collect the cut corners at the shared seam position (1, 0, 0.5).
        const cutPointKeys = new Map ();
        for (let triangleIndex = 0; triangleIndex < clipped.TriangleCount (); triangleIndex++) {
            const triangle = clipped.GetTriangle (triangleIndex);
            for (const corner of [
                { v : triangle.v0, c : triangle.c0, n : triangle.n0, u : triangle.u0 },
                { v : triangle.v1, c : triangle.c1, n : triangle.n1, u : triangle.u1 },
                { v : triangle.v2, c : triangle.c2, n : triangle.n2, u : triangle.u2 }
            ]) {
                const vertex = clipped.GetVertex (corner.v);
                if (!ApproxEqual (vertex.z, 0.5, 1e-9) ||
                    !ApproxEqual (vertex.x, 1.0, 1e-9) || !ApproxEqual (vertex.y, 0.0, 1e-9)) {
                    continue;
                }
                // Cap corners intentionally have no attributes; only side
                // corners participate in the seam attribute checks.
                if (corner.u === null || corner.c === null) {
                    continue;
                }
                const key = PositionKey (vertex, 1e-9);
                if (!cutPointKeys.has (key)) {
                    cutPointKeys.set (key, []);
                }
                cutPointKeys.get (key).push (corner);
            }
        }
        assert.ok (cutPointKeys.size >= 1);
        for (const corners of cutPointKeys.values ()) {
            // The geometric seam is one position, but the attributes stay split.
            assert.ok (corners.length >= 2);
            const uvSet = new Set (corners.map ((corner) => {
                const uv = clipped.GetTextureUV (corner.u);
                return uv.x.toFixed (6) + ',' + uv.y.toFixed (6);
            }));
            assert.ok (uvSet.size >= 2);
            const colorSet = new Set (corners.map ((corner) => {
                const color = clipped.GetVertexColor (corner.c);
                return color.r + ',' + color.g + ',' + color.b;
            }));
            assert.ok (colorSet.size >= 2);

            // UV interpolation is linear along the original edge: v goes 0 -> 1.
            for (const corner of corners) {
                const uv = clipped.GetTextureUV (corner.u);
                assert.ok (ApproxEqual (uv.y, 0.5, 1e-9));
            }
            // RGB interpolation is linear and rounded to integers.
            for (const corner of corners) {
                const color = clipped.GetVertexColor (corner.c);
                assert.ok (Number.isInteger (color.r) && Number.isInteger (color.g) && Number.isInteger (color.b));
            }
        }

        // Opposite normals interpolated at the midpoint produce a zero normal,
        // which is kept as zero instead of being normalized into garbage.
        let zeroNormalFound = false;
        for (let i = 0; i < clipped.NormalCount (); i++) {
            const normal = clipped.GetNormal (i);
            if (normal.Length () === 0.0) {
                zeroNormalFound = true;
            }
        }
        assert.ok (zeroNormalFound);

        // The cap carries no UVs or vertex colors.
        for (let i = 0; i < clipped.TriangleCount (); i++) {
            const triangle = clipped.GetTriangle (i);
            const normal = clipped.GetNormal (triangle.n0);
            if (ApproxEqual (normal.z, 1.0, 1e-9) && ApproxEqual (normal.x, 0.0, 1e-9) && ApproxEqual (normal.y, 0.0, 1e-9)) {
                assert.equal (triangle.HasTextureUVs (), false);
                assert.equal (triangle.HasVertexColors (), false);
            }
        }
    });

    it ('section-instances-and-picking', async function () {
        const fixture = MakeSectionFixture ();
        const sectionPlane = {
            normal : new OV.Coord3D (0.0, 0.0, 1.0),
            constant : -5.0,
            capMaterialIndex : 1
        };
        const stretchedMatrix = fixture.stretched.GetTransformation ().GetMatrix ().Get ().slice ();
        const belowMatrix = fixture.below.GetTransformation ().GetMatrix ().Get ().slice ();

        const params = new OV.ModelToThreeConversionParams ();
        params.sectionPlane = sectionPlane;
        const object = await ConvertToThree (fixture.model, params);
        object.updateMatrixWorld (true);

        const cast = (x) => new THREE.Raycaster (
            new THREE.Vector3 (x, 0.0, 20.0),
            new THREE.Vector3 (0.0, 0.0, -1.0)
        ).intersectObject (object, true);

        const wallHits = cast (2.0);
        assert.ok (wallHits.length > 0);
        assert.ok (ApproxEqual (wallHits[0].point.z, 5.0, 1e-6));
        assert.equal (wallHits[0].object.userData.originalMeshInstance.GetId ().nodeId, fixture.stretched.GetId ());
        const boreHits = cast (0.0);
        assert.equal (boreHits.length, 0);

        // The raycaster and bounding box see the clipped geometry, including caps.
        let maximumZ = -Infinity;
        object.traverse ((child) => {
            if (child.isMesh) {
                child.geometry.computeBoundingBox ();
                const box = child.geometry.boundingBox.clone ().applyMatrix4 (child.matrixWorld);
                maximumZ = Math.max (maximumZ, box.max.z);
            }
        });
        assert.ok (ApproxEqual (maximumZ, 5.0, 1e-6));

        // Per-instance volumes: stretched tube keeps 80 in world space, the
        // fully retained lower instance also keeps 80.
        const settings = new OV.ExporterSettings ();
        settings.sectionPlane = sectionPlane;
        const exporterModel = new OV.ExporterModel (fixture.model, settings);
        let instanceCount = 0;
        exporterModel.EnumerateTransformedMeshInstances ((mesh) => {
            assert.ok (ApproxEqual (OV.CalculateVolume (mesh), 80.0, 1e-7));
            instanceCount += 1;
        });
        assert.equal (instanceCount, 2);

        // Source mesh, node matrices and material objects are not modified.
        assert.equal (fixture.mesh.TriangleCount (), 32);
        assert.deepEqual (fixture.stretched.GetTransformation ().GetMatrix ().Get (), stretchedMatrix);
        assert.deepEqual (fixture.below.GetTransformation ().GetMatrix ().Get (), belowMatrix);

        // No reuse: a later conversion without sectioning shows the full solid.
        const fullObject = await ConvertToThree (fixture.model, new OV.ModelToThreeConversionParams ());
        fullObject.updateMatrixWorld (true);
        const fullHits = new THREE.Raycaster (
            new THREE.Vector3 (2.0, 0.0, 20.0),
            new THREE.Vector3 (0.0, 0.0, -1.0)
        ).intersectObject (fullObject, true);
        assert.ok (ApproxEqual (fullHits[0].point.z, 9.0, 1e-6));
    });

    it ('section-export-counts', async function () {
        const fixture = MakeSectionFixture ();
        const sectionPlane = {
            normal : new OV.Coord3D (0.0, 0.0, 1.0),
            constant : -5.0,
            capMaterialIndex : 1
        };
        const settings = new OV.ExporterSettings ({
            sectionPlane : sectionPlane,
            isMeshVisible : (meshInstanceId) => {
                return meshInstanceId.nodeId !== fixture.below.GetId ();
            },
            transformation : new OV.Transformation (new OV.Matrix ().CreateTranslation (10.0, 0.0, 0.0))
        });
        const exporterModel = new OV.ExporterModel (fixture.model, settings);

        assert.equal (exporterModel.MeshInstanceCount (), 1);
        const triangleCount = exporterModel.TriangleCount ();
        assert.ok (triangleCount > 0);

        // Sectioning happens before the export transformation: cap stays at z = 5,
        // and the translation is applied afterwards on every vertex.
        let enumerated = 0;
        let maximumZ = -Infinity;
        let minimumX = Infinity;
        exporterModel.EnumerateTrianglesWithNormals ((a, b, c) => {
            maximumZ = Math.max (maximumZ, a.z, b.z, c.z);
            minimumX = Math.min (minimumX, a.x, b.x, c.x);
            enumerated += 1;
        });
        assert.equal (enumerated, triangleCount);
        assert.ok (ApproxEqual (maximumZ, 5.0, 1e-7));
        assert.ok (minimumX >= 7.0 - 1e-7);

        // Binary STL: header face count and byte length must agree with the bytes.
        const stlFiles = await ExportFiles (new ExporterStl (), exporterModel, FileFormat.Binary);
        const stlView = new DataView (stlFiles[0].GetBufferContent ());
        const headerCount = stlView.getUint32 (80, true);
        assert.equal (headerCount, triangleCount);
        assert.equal (stlView.byteLength, 84 + triangleCount * 50);
        assert.equal ((stlView.byteLength - 84) / 50, triangleCount);

        // OBJ: every v/vt/vn reference and every material must be valid.
        const objFiles = await ExportFiles (new ExporterObj (), exporterModel, FileFormat.Text);
        const objText = new TextDecoder ().decode (objFiles.find ((file) => file.GetName () === 'model.obj').GetBufferContent ());
        const mtlText = new TextDecoder ().decode (objFiles.find ((file) => file.GetName () === 'model.mtl').GetBufferContent ());
        const lines = objText.split ('\n');
        let vertexCount = 0;
        let uvCount = 0;
        let normalCount = 0;
        let faceCount = 0;
        const materials = new Set ();
        for (const line of lines) {
            const parts = line.trim ().split (/\s+/);
            if (parts[0] === 'v') {
                vertexCount += 1;
            } else if (parts[0] === 'vt') {
                uvCount += 1;
            } else if (parts[0] === 'vn') {
                normalCount += 1;
            } else if (parts[0] === 'usemtl') {
                materials.add (parts[1]);
            } else if (parts[0] === 'f') {
                faceCount += 1;
                assert.equal (parts.length, 4);
                for (let i = 1; i < parts.length; i++) {
                    const [vIndex, uvIndex, normalIndex] = parts[i].split ('/').map ((value) => parseInt (value, 10));
                    assert.ok (vIndex >= 1 && vIndex <= vertexCount);
                    assert.ok (normalIndex >= 1 && normalIndex <= normalCount);
                    if (uvIndex) {
                        assert.ok (uvIndex >= 1 && uvIndex <= uvCount);
                    }
                }
            }
        }
        assert.equal (faceCount, triangleCount);
        assert.ok (materials.has ('section-cap'));
        for (const materialName of materials) {
            assert.ok (mtlText.includes ('newmtl ' + materialName));
        }

        // Invalid section parameters are rejected at the entry points.
        assert.throws (() => new OV.ExporterModel (fixture.model, new OV.ExporterSettings ({
            sectionPlane : { normal : new OV.Coord3D (0.0, 0.0, 1.0), constant : -5.0, capMaterialIndex : 99 }
        })), RangeError);
        assert.throws (() => new OV.ExporterModel (fixture.model, new OV.ExporterSettings ({
            sectionPlane : sectionPlane,
            transformation : new OV.Transformation (new OV.Matrix ().CreateScale (1.0, 1.0, -1.0))
        })), RangeError);
    });

    it ('section-degenerate-and-reset', function () {
        const fixture = MakeSectionFixture ();
        const snapshot = fixture.mesh.vertices.map ((vertex) => vertex.Clone ());

        // Plane through the original top vertices: no double cap layer, no
        // zero-thickness slab, the original end faces are simply reused.
        const throughTop = OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 1.0),
            constant : -4.0,
            capMaterialIndex : 1
        });
        assert.equal (CountTrianglesWithMaterial (throughTop, 1), 0);
        let trianglesAtTop = 0;
        for (let i = 0; i < throughTop.TriangleCount (); i++) {
            const triangle = throughTop.GetTriangle (i);
            const vertices = [triangle.v0, triangle.v1, triangle.v2].map ((index) => throughTop.GetVertex (index));
            if (vertices.every ((vertex) => ApproxEqual (vertex.z, 4.0, 1e-9))) {
                trianglesAtTop += 1;
            }
        }
        assert.equal (trianglesAtTop, 8);
        assert.ok (ApproxEqual (OV.CalculateVolume (throughTop), 80.0, 1e-8));
        assert.deepEqual (FindOpenEdges (throughTop), []);

        // Plane coinciding with an exterior side face.
        const throughSide = OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (1.0, 0.0, 0.0),
            constant : -3.0,
            capMaterialIndex : 1
        });
        assert.equal (CountTrianglesWithMaterial (throughSide, 1), 0);
        assert.ok (ApproxEqual (OV.CalculateVolume (throughSide), 80.0, 1e-8));
        assert.deepEqual (FindOpenEdges (throughSide), []);

        // Everything removed yields an empty mesh.
        const removed = OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 1.0),
            constant : 0.5,
            capMaterialIndex : 1
        });
        assert.equal (removed.TriangleCount (), 0);
        assert.equal (removed.VertexCount (), 0);

        // Repeated runs with different planes and a disabled section never
        // reuse results or mutate the shared source mesh.
        const first = OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 1.0), constant : -2.0, capMaterialIndex : 1
        });
        const second = OV.ClipMeshByPlane (fixture.mesh, {
            normal : new OV.Coord3D (0.0, 0.0, 1.0), constant : -3.0, capMaterialIndex : 1
        });
        assert.ok (ApproxEqual (OV.CalculateVolume (first), 40.0, 1e-8));
        assert.ok (ApproxEqual (OV.CalculateVolume (second), 60.0, 1e-8));
        assert.notEqual (first, second);
        assert.equal (fixture.mesh.TriangleCount (), 32);
        assert.deepEqual (fixture.mesh.vertices, snapshot);
    });
});
