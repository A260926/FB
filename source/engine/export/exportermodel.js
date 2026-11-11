import { CopyObjectAttributes } from '../core/core.js';
import { Transformation } from '../geometry/transformation.js';
import { CheckSectionPlane, CheckSectionTransformation, ClipMeshByPlane, TransformSectionPlaneToLocal } from '../model/meshsection.js';
import { CalculateTriangleNormal, IsEmptyMesh, TransformMesh } from '../model/meshutils.js';
import { Model } from '../model/model.js';
import { Node } from '../model/node.js';

/**
 * Settings of an export operation.
 */
export class ExporterSettings
{
    constructor (settings)
    {
        this.transformation = new Transformation ();
        /**
         * Optional solid section plane in model world coordinates. When set,
         * visible mesh instances are cut separately with their full node
         * transformation, closed with cap faces, and exported as independent
         * meshes. The plane description needs a normal (not necessarily unit
         * length), a constant and a valid cap material index. Sectioning runs
         * before the export transformation, which is applied afterwards.
         * @type {object|null}
         */
        this.sectionPlane = null;
        this.isMeshVisible = (meshInstanceId) => {
            return true;
        };

        CopyObjectAttributes (settings, this);
    }
}

function CreateSectionedModel (model, settings)
{
    const sectionPlane = CheckSectionPlane (settings.sectionPlane, model.MaterialCount ());
    CheckSectionTransformation (settings.transformation.GetMatrix ());

    let sectionedModel = new Model ();
    sectionedModel.SetUnit (model.GetUnit ());
    for (let materialIndex = 0; materialIndex < model.MaterialCount (); materialIndex++) {
        sectionedModel.AddMaterial (model.GetMaterial (materialIndex));
    }
    model.EnumerateMeshInstances ((meshInstance) => {
        if (!settings.isMeshVisible (meshInstance.GetId ())) {
            return;
        }
        let transformation = meshInstance.GetTransformation ();
        CheckSectionTransformation (transformation.GetMatrix ());
        let localPlane = TransformSectionPlaneToLocal (sectionPlane.normal, sectionPlane.constant, transformation.GetMatrix ());
        let sectionedMesh = ClipMeshByPlane (meshInstance.GetMesh (), {
            normal : localPlane.normal,
            constant : localPlane.constant,
            capMaterialIndex : sectionPlane.capMaterialIndex
        });
        if (IsEmptyMesh (sectionedMesh)) {
            return;
        }
        let meshIndex = sectionedModel.AddMesh (sectionedMesh);
        let node = new Node ();
        node.SetName (meshInstance.node.GetName ());
        node.SetTransformation (transformation);
        sectionedModel.GetRootNode ().AddChildNode (node);
        node.AddMeshIndex (meshIndex);
    });
    return sectionedModel;
}

export class ExporterModel
{
    constructor (model, settings)
    {
        this.settings = settings || new ExporterSettings ();
        this.hasSection = (this.settings.sectionPlane !== null && this.settings.sectionPlane !== undefined);
        if (this.hasSection) {
            this.model = CreateSectionedModel (model, this.settings);
        } else {
            this.model = model;
        }
        this.visibleMeshes = null;
        this.meshToVisibleMeshIndex = null;
    }

    GetModel ()
    {
        return this.model;
    }

    MaterialCount ()
    {
        return this.model.MaterialCount ();
    }

    GetMaterial (index)
    {
        return this.model.GetMaterial (index);
    }

    VertexCount ()
    {
        let vertexCount = 0;
        this.EnumerateMeshInstances ((meshInstance) => {
            vertexCount += meshInstance.VertexCount ();
        });
        return vertexCount;
    }

    TriangleCount ()
    {
        let triangleCount = 0;
        this.EnumerateMeshInstances ((meshInstance) => {
            triangleCount += meshInstance.TriangleCount ();
        });
        return triangleCount;
    }

    MeshCount ()
    {
        let meshCount = 0;
        this.EnumerateMeshes ((mesh) => {
            meshCount += 1;
        });
        return meshCount;
    }

    EnumerateMeshes (onMesh)
    {
        this.FillVisibleMeshCache ();
        for (let meshIndex = 0; meshIndex < this.model.MeshCount (); meshIndex++) {
            if (this.visibleMeshes.has (meshIndex)) {
                let mesh = this.model.GetMesh (meshIndex);
                onMesh (mesh);
            }
        }
    }

    MapMeshIndex (meshIndex)
    {
        this.FillVisibleMeshCache ();
        return this.meshToVisibleMeshIndex.get (meshIndex);
    }

    IsMeshInstanceVisible (meshInstanceId)
    {
        if (this.hasSection) {
            // visibility was already applied on the original mesh instance ids
            return true;
        }
        return this.settings.isMeshVisible (meshInstanceId);
    }

    MeshInstanceCount ()
    {
        let meshInstanceCount = 0;
        this.EnumerateMeshInstances ((meshInstance) => {
            meshInstanceCount += 1;
        });
        return meshInstanceCount;
    }

    EnumerateMeshInstances (onMeshInstance)
    {
        this.model.EnumerateMeshInstances ((meshInstance) => {
            if (this.IsMeshInstanceVisible (meshInstance.GetId ())) {
                onMeshInstance (meshInstance);
            }
        });
    }

    EnumerateTransformedMeshInstances (onMesh)
    {
        this.EnumerateMeshInstances ((meshInstance) => {
            let transformation = meshInstance.GetTransformation ();
            if (!this.settings.transformation.IsIdentity ()) {
                transformation.Append (this.settings.transformation);
            }

            let mesh = meshInstance.GetMesh ();
            let transformed = mesh.Clone ();
            if (!transformation.IsIdentity ()) {
                TransformMesh (transformed, transformation);
            }

            onMesh (transformed);
        });
    }

    EnumerateVerticesAndTriangles (callbacks)
    {
        let transformedMeshes = [];
        this.EnumerateTransformedMeshInstances ((mesh) => {
            transformedMeshes.push (mesh);
        });

        for (let mesh of transformedMeshes) {
            mesh.EnumerateVertices ((vertex) => {
                callbacks.onVertex (vertex.x, vertex.y, vertex.z);
            });
        }

        let vertexOffset = 0;
        for (let mesh of transformedMeshes) {
            mesh.EnumerateTriangleVertexIndices ((v0, v1, v2) => {
                callbacks.onTriangle (v0 + vertexOffset, v1 + vertexOffset, v2 + vertexOffset);
            });
            vertexOffset += mesh.VertexCount ();
        }
    }

    EnumerateTrianglesWithNormals (onTriangle)
    {
        this.EnumerateTransformedMeshInstances ((mesh) => {
            mesh.EnumerateTriangleVertices ((v0, v1, v2) => {
                let normal = CalculateTriangleNormal (v0, v1, v2);
                onTriangle (v0, v1, v2, normal);
            });
        });
    }

    FillVisibleMeshCache ()
    {
        if (this.visibleMeshes !== null && this.meshToVisibleMeshIndex !== null) {
            return;
        }

        this.visibleMeshes = new Set ();
        this.model.EnumerateMeshInstances ((meshInstance) => {
            let meshInstanceId = meshInstance.GetId ();
            if (this.IsMeshInstanceVisible (meshInstanceId)) {
                this.visibleMeshes.add (meshInstanceId.meshIndex);
            }
        });

        this.meshToVisibleMeshIndex = new Map ();
        let visibleMeshIndex = 0;
        for (let meshIndex = 0; meshIndex < this.model.MeshCount (); meshIndex++) {
            if (this.visibleMeshes.has (meshIndex)) {
                this.meshToVisibleMeshIndex.set (meshIndex, visibleMeshIndex);
                visibleMeshIndex += 1;
            }
        }
    }
}
