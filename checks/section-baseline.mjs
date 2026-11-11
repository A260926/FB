import assert from 'node:assert/strict';
import { MakeSectionFixture } from './section-fixture.mjs';
import { CalculateVolume } from '../source/engine/model/quantities.js';
import { GetBoundingBox, IsTwoManifold } from '../source/engine/model/modelutils.js';
import { Coord3D } from '../source/engine/geometry/coord3d.js';
import { ExporterModel, ExporterSettings } from '../source/engine/export/exportermodel.js';
import { ExporterStl } from '../source/engine/export/exporterstl.js';
import { FileFormat } from '../source/engine/io/fileutils.js';
import { ConvertModelToThreeObject, ModelToThreeConversionParams, ModelToThreeConversionOutput } from '../source/engine/threejs/threeconverter.js';
import * as THREE from 'three';

const { model, mesh, stretched } = MakeSectionFixture ();
assert.equal (mesh.TriangleCount (), 32);
assert.ok (IsTwoManifold (mesh));
assert.ok (Math.abs (CalculateVolume (mesh) - 80) < 1e-8);
assert.ok (Math.abs (CalculateVolume (model) - 240) < 1e-8);
const bounds = GetBoundingBox (model);
assert.equal (bounds.min.z, -6); assert.equal (bounds.max.z, 9);
console.log ('BASELINE_TUBE triangles=32 closed=true localVolume=80 instanceVolume=240 worldZ=-6..9');

// Probe a proposed option on real existing entry points. It is not an existing promise
// of the upstream API: this records the starting state for a feature request.
const sectionPlane = { normal : new Coord3D (0, 0, 1), constant : -5, capMaterialIndex : 1 };
const settings = new ExporterSettings ();
settings.sectionPlane = sectionPlane;
const exporterModel = new ExporterModel (model, settings);
let maximumZ = -Infinity;
let enumerated = 0;
exporterModel.EnumerateTrianglesWithNormals ((a, b, c) => {
    maximumZ = Math.max (maximumZ, a.z, b.z, c.z); enumerated++;
});
assert.equal (maximumZ, 9); assert.equal (enumerated, 64);
const files = await new Promise ((resolve) => new ExporterStl ().Export (exporterModel, FileFormat.Binary, resolve));
const stl = new DataView (files[0].GetBufferContent ());
assert.equal (stl.getUint32 (80, true), 64);
assert.equal (stl.byteLength, 84 + 64 * 50);
console.log ('BASELINE_EXPORT sectionRequestedZ=5 actualMaxZ=9 triangles=64 stlBytes=3284');

const params = new ModelToThreeConversionParams ();
params.sectionPlane = sectionPlane;
const object = await new Promise ((resolve) => ConvertModelToThreeObject (
    model, params, new ModelToThreeConversionOutput (), { onTextureLoaded () {}, onModelLoaded : resolve }
));
object.updateMatrixWorld (true);
const cast = (x) => new THREE.Raycaster (new THREE.Vector3 (x, 0, 20), new THREE.Vector3 (0, 0, -1)).intersectObject (object, true);
const wallHits = cast (2);
const holeHits = cast (0);
assert.ok (wallHits.length > 0);
assert.equal (wallHits[0].point.z, 9);
assert.equal (holeHits.length, 0);
assert.equal (wallHits[0].object.userData.originalMeshInstance.GetId ().nodeId, stretched.GetId ());
console.log ('BASELINE_DISPLAY requestedZ=5 firstWallHitZ=9 boreHits=0 originalInstance=true');
