// Input geometry only: a closed rectangular tube, without a clipping implementation.
import { Coord2D } from '../source/engine/geometry/coord2d.js';
import { Coord3D } from '../source/engine/geometry/coord3d.js';
import { Matrix } from '../source/engine/geometry/matrix.js';
import { Transformation } from '../source/engine/geometry/transformation.js';
import { Mesh } from '../source/engine/model/mesh.js';
import { Triangle } from '../source/engine/model/triangle.js';
import { Model } from '../source/engine/model/model.js';
import { Node } from '../source/engine/model/node.js';
import { PhongMaterial } from '../source/engine/model/material.js';
import { RGBColor } from '../source/engine/model/color.js';
import { FinalizeModel } from '../source/engine/model/modelfinalization.js';

export function MakeSectionFixture () {
    const mesh = new Mesh ();
    mesh.SetName ('rectangular-tube');
    const loops = [[[-3, -2], [3, -2], [3, 2], [-3, 2]],
                   [[-1, -1], [1, -1], [1, 1], [-1, 1]]];
    for (const z of [0, 4]) {
        for (const loop of loops) {
            for (const [x, y] of loop) { mesh.AddVertex (new Coord3D (x, y, z)); }
        }
    }
    const quad = (a, b, c, d, face) => {
        for (const corners of [[a, b, c], [a, c, d]]) {
            const triangle = new Triangle (...corners).SetMaterial (0).SetCurve (face);
            // Each face has its own UV domain, independent of shared position indices.
            const uvs = corners.map ((v) => {
                const p = mesh.GetVertex (v);
                return mesh.AddTextureUV (new Coord2D (face * 10 + p.x + p.z, p.y + 2 * p.z));
            });
            triangle.SetTextureUVs (...uvs);
            mesh.AddTriangle (triangle);
        }
    };
    for (let i = 0; i < 4; i++) {
        const j = (i + 1) % 4;
        quad (i, j, j + 8, i + 8, i);
        quad (i + 4, i + 12, j + 12, j + 4, i + 4);
        quad (i + 8, j + 8, j + 12, i + 12, 8);
        quad (i + 4, j + 4, j, i, 9);
    }
    const model = new Model ();
    const wall = new PhongMaterial ();
    wall.name = 'wall'; wall.color = new RGBColor (80, 140, 190);
    const cap = new PhongMaterial ();
    cap.name = 'section-cap'; cap.color = new RGBColor (240, 170, 40);
    model.AddMaterial (wall); model.AddMaterial (cap);
    const meshIndex = model.AddMesh (mesh);
    const parent = new Node ();
    parent.SetName ('translated-parent');
    parent.SetTransformation (new Transformation (new Matrix ().CreateTranslation (0, 0, 1)));
    model.GetRootNode ().AddChildNode (parent);
    const stretched = new Node ();
    stretched.SetName ('stretched-instance');
    stretched.SetTransformation (new Transformation (new Matrix ().CreateScale (1, 1, 2)));
    parent.AddChildNode (stretched); stretched.AddMeshIndex (meshIndex);
    const below = new Node ();
    below.SetName ('below-plane-instance');
    below.SetTransformation (new Transformation (new Matrix ().CreateTranslation (0, 0, -6)));
    model.GetRootNode ().AddChildNode (below); below.AddMeshIndex (meshIndex);
    FinalizeModel (model, {});
    return { model, mesh, stretched, below };
}
