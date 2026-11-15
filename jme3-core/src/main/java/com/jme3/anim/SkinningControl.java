/*
 * Copyright (c) 2009-2026 jMonkeyEngine
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of 'jMonkeyEngine' nor the names of its contributors
 *   may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
package com.jme3.anim;

import com.jme3.export.InputCapsule;
import com.jme3.export.JmeExporter;
import com.jme3.export.JmeImporter;
import com.jme3.export.OutputCapsule;
import com.jme3.material.MatParamOverride;
import com.jme3.math.Matrix3f;
import com.jme3.math.Matrix4f;
import com.jme3.math.Quaternion;
import com.jme3.math.Transform;
import com.jme3.math.Vector3f;
import com.jme3.renderer.RenderManager;
import com.jme3.renderer.RendererException;
import com.jme3.renderer.ViewPort;
import com.jme3.scene.Geometry;
import com.jme3.scene.Mesh;
import com.jme3.scene.Node;
import com.jme3.scene.Spatial;
import com.jme3.scene.VertexBuffer;
import com.jme3.scene.VertexBuffer.Type;
import com.jme3.scene.control.AbstractControl;
import com.jme3.scene.control.PostLogicalControl;
import com.jme3.shader.VarType;
import com.jme3.util.SafeArrayList;
import com.jme3.util.TempVars;
import com.jme3.util.clone.Cloner;
import com.jme3.util.clone.JmeCloneable;

import java.io.IOException;
import java.nio.Buffer;
import java.nio.ByteBuffer;
import java.nio.FloatBuffer;
import java.nio.IntBuffer;
import java.nio.ShortBuffer;
import java.util.HashMap;
import java.util.Map;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * The `SkinningControl` deforms a 3D model according to an {@link Armature}. It manages the
 * computation of deformation matrices and applies these transformations to the mesh,
 * supporting both software and hardware-accelerated skinning.
 *
 * <p>
 * **Software Skinning:** Performed on the CPU, offering broader compatibility but
 * potentially lower performance for complex models.
 * <p>
 * **Hardware Skinning:** Utilizes the GPU for deformation, providing significantly
 * better performance but requiring shader support and having a limit on the number
 * of bones (typically 255 in common shaders).
 *
 * @author Nehon
 */
public class SkinningControl extends AbstractControl implements JmeCloneable, PostLogicalControl {

    private static final Logger logger = Logger.getLogger(SkinningControl.class.getName());

    /**
     * The maximum number of bones supported for hardware skinning in common shaders.
     */
    private static final int MAX_BONES_HW_SKINNING_SUPPORT = 255;

    /**
     * Minimum length of the (b-a) edge for a surface attachment frame to be
     * considered valid.
     */
    private static final float MIN_SURFACE_EDGE_LENGTH = 1e-6f;

    /**
     * Minimum length of the (b-a) x (c-a) cross product for a surface
     * attachment frame to be considered valid.
     */
    private static final float MIN_SURFACE_CROSS_LENGTH = 1e-10f;

    /**
     * The armature of the model.
     */
    private Armature armature;

    /**
     * A list of geometries that this control will deform.
     */
    private SafeArrayList<Geometry> targets = new SafeArrayList<>(Geometry.class);

    /**
     * Used to track when a mesh needs to be updated. Meshes are only updated if they
     * are visible in at least one camera.
     */
    private boolean meshUpdateRequired = true;

    /**
     * Indicates whether hardware skinning is preferred. If `true` and the GPU
     * supports it, hardware skinning will be enabled.
     */
    private transient boolean hwSkinningPreferred = true;

    /**
     * Indicates if hardware skinning is currently active and being used.
     */
    private transient boolean hwSkinningEnabled = false;

    /**
     * Flag indicating whether hardware skinning compatibility has been tested
     * on the current GPU. Results are stored in {@link #hwSkinningSupported}.
     */
    private transient boolean hwSkinningTested = false;

    /**
     * Stores the result of the hardware skinning compatibility test. `true` if
     * supported, `false` otherwise. This is only valid after
     * {@link #hwSkinningTested} is `true`.
     */
    private transient boolean hwSkinningSupported = false;

    /**
     * Bone offset matrices, computed each frame to deform the mesh based on
     * the armature's current pose.
     */
    private transient Matrix4f[] boneOffsetMatrices;

    private MatParamOverride numberOfJointsParam = new MatParamOverride(VarType.Int, "NumberOfBones", null);
    private MatParamOverride jointMatricesParam = new MatParamOverride(VarType.Matrix4Array, "BoneMatrices", null);

    /**
     * Surface attachments, keyed by name. These follow a deforming triangle of
     * one of the {@link #targets} rather than a single bone.
     */
    private transient Map<String, SurfaceAttachment> surfaceAttachments;

    /**
     * Serialization only. Do not use.
     */
    protected SkinningControl() {
    }

    /**
     * Creates a new `SkinningControl` for the given armature.
     *
     * @param armature The armature that drives the deformation (not null).
     */
    public SkinningControl(Armature armature) {
        if (armature == null) {
            throw new IllegalArgumentException("armature cannot be null.");
        }
        this.armature = armature;
    }

    /**
     * Configures the material parameters and meshes for hardware skinning.
     */
    private void enableHardwareSkinning() {
        numberOfJointsParam.setEnabled(true);
        jointMatricesParam.setEnabled(true);

        // Calculate the number of bones rounded up to the nearest multiple of 10.
        // This is often required by shaders for array uniform declarations.
        int numBones = ((armature.getJointCount() / 10) + 1) * 10;
        numberOfJointsParam.setValue(numBones);

        for (Geometry geometry : targets) {
            Mesh mesh = geometry.getMesh();
            if (mesh != null && mesh.isAnimated()) {
                mesh.prepareForAnim(false);
            }
        }
    }

    /**
     * Configures the material parameters and meshes for software skinning.
     */
    private void enableSoftwareSkinning() {
        numberOfJointsParam.setEnabled(false);
        jointMatricesParam.setEnabled(false);

        for (Geometry geometry : targets) {
            Mesh mesh = geometry.getMesh();
            if (mesh != null && mesh.isAnimated()) {
                mesh.prepareForAnim(true);
            }
        }
    }

    /**
     * Tests if hardware skinning is supported by the GPU for the current spatial.
     *
     * @param renderManager the RenderManager instance
     * @return true if hardware skinning is supported, false otherwise
     */
    private boolean testHardwareSupported(RenderManager renderManager) {
        // Only 255 bones max supported with hardware skinning in common shaders.
        if (armature.getJointCount() > MAX_BONES_HW_SKINNING_SUPPORT) {
            logger.log(Level.INFO, "Hardware skinning not supported for {0}: Too many bones ({1} > 255).",
                    new Object[]{spatial, armature.getJointCount()});
            return false;
        }

        // Temporarily enable hardware skinning to test shader compilation.
        enableHardwareSkinning();
        boolean hwSkinningEngaged = false;

        try {
            renderManager.preloadScene(spatial);
            hwSkinningEngaged = true;

        } catch (RendererException ex) {
            logger.log(Level.WARNING, "Could not enable HW skinning due to shader compile error: ", ex);
        }

        return hwSkinningEngaged;
    }

    /**
     * Specifies if hardware skinning is preferred. If it is preferred and
     * supported by GPU, it shall be enabled.  If it's not preferred, or not
     * supported by GPU, then it shall be disabled.
     *
     * @param preferred true to prefer hardware skinning, false to prefer 
     * software skinning (default=true)
     * @see #isHardwareSkinningUsed()
     */
    public void setHardwareSkinningPreferred(boolean preferred) {
        hwSkinningPreferred = preferred;
    }

    /**
     * @return True if hardware skinning is preferable to software skinning.
     * Set to false by default.
     * @see #setHardwareSkinningPreferred(boolean)
     */
    public boolean isHardwareSkinningPreferred() {
        return hwSkinningPreferred;
    }

    /**
     * @return True is hardware skinning is activated and is currently used, false otherwise.
     */
    public boolean isHardwareSkinningUsed() {
        return hwSkinningEnabled;
    }

    /**
     * Recursively finds and adds animated geometries to the targets list.
     *
     * @param sp The spatial to search within.
     */
    private void collectAnimatedGeometries(Spatial sp) {
        if (sp instanceof Geometry) {
            Geometry geo = (Geometry) sp;
            Mesh mesh = geo.getMesh();
            if (mesh != null && mesh.isAnimated()) {
                targets.add(geo);
            }
        } else if (sp instanceof Node) {
            for (Spatial child : ((Node) sp).getChildren()) {
                collectAnimatedGeometries(child);
            }
        }
    }

    @Override
    public void setSpatial(Spatial spatial) {
        Spatial oldSpatial = this.spatial;
        super.setSpatial(spatial);
        updateAnimationTargets(spatial);

        if (oldSpatial != null) {
            // Ensure parameters are removed from the old spatial to prevent memory leaks
            oldSpatial.removeMatParamOverride(numberOfJointsParam);
            oldSpatial.removeMatParamOverride(jointMatricesParam);
        }

        if (spatial != null) {
            // Add parameters to the new spatial. No need to remove first if they are not already present.
            spatial.addMatParamOverride(numberOfJointsParam);
            spatial.addMatParamOverride(jointMatricesParam);
        }
    }

    /**
     * Performs software skinning updates.
     */
    private void controlRenderSoftware() {
        resetToBind(); // reset morph meshes to bind pose

        boneOffsetMatrices = armature.computeSkinningMatrices();

        for (Geometry geometry : targets) {
            Mesh mesh = geometry.getMesh();
            // NOTE: This assumes code higher up has already ensured this mesh is animated.
            // Otherwise, a crash will happen in skin update.
            applySoftwareSkinning(mesh, boneOffsetMatrices);
        }
    }

    /**
     * Prepares parameters for hardware skinning.
     */
    private void controlRenderHardware() {
        boneOffsetMatrices = armature.computeSkinningMatrices();
        jointMatricesParam.setValue(boneOffsetMatrices);
    }

    @Override
    protected void controlRender(RenderManager rm, ViewPort vp) {
        if (meshUpdateRequired) {
            updateAnimationTargets(spatial);

            // Prevent illegal cases. These should never happen.
            assert hwSkinningTested || (!hwSkinningSupported && !hwSkinningEnabled);
            assert !hwSkinningEnabled || (hwSkinningTested && hwSkinningSupported);

            if (hwSkinningPreferred && !hwSkinningTested) {
                // If hardware skinning is preferred and hasn't been tested yet, test it.
                hwSkinningTested = true;
                hwSkinningSupported = testHardwareSupported(rm);

                if (hwSkinningSupported) {
                    hwSkinningEnabled = true;
                } else {
                    enableSoftwareSkinning();
                }
            } else if (hwSkinningPreferred && hwSkinningSupported && !hwSkinningEnabled) {
                // If hardware skinning is preferred, supported, but not yet enabled, enable it.
                enableHardwareSkinning();
                hwSkinningEnabled = true;

            } else if (!hwSkinningPreferred && hwSkinningEnabled) {
                // If hardware skinning is no longer preferred but is enabled, switch to software.
                enableSoftwareSkinning();
                hwSkinningEnabled = false;
            }

            if (hwSkinningEnabled) {
                controlRenderHardware();
            } else {
                controlRenderSoftware();
            }

            meshUpdateRequired = false; // Reset flag after update
        }
    }

    @Override
    protected void controlUpdate(float tpf) {
        meshUpdateRequired = true; // Mark for mesh update on next render pass
        armature.update();
    }

    /**
     * Resets the vertex, normal, and tangent buffers of animated meshes to their
     * original bind pose. This is crucial for software skinning to ensure
     * transformations are applied from a consistent base.
     * This method is only applied when performing software updates.
     */
    void resetToBind() {
        for (Geometry geometry : targets) {
            Mesh mesh = geometry.getMesh();
            if (mesh != null && mesh.isAnimated()) {
                Buffer bwBuff = mesh.getBuffer(Type.BoneWeight).getData();
                Buffer biBuff = mesh.getBuffer(Type.BoneIndex).getData();
                if (!biBuff.hasArray() || !bwBuff.hasArray()) {
                    mesh.prepareForAnim(true); // prepare for software animation
                }
                VertexBuffer bindPos = mesh.getBuffer(Type.BindPosePosition);
                VertexBuffer bindNorm = mesh.getBuffer(Type.BindPoseNormal);
                VertexBuffer pos = mesh.getBuffer(Type.Position);
                FloatBuffer pb = (FloatBuffer) pos.getData();
                FloatBuffer bpb = (FloatBuffer) bindPos.getData();
                pb.clear();
                bpb.clear();

                // reset bind normals if there is a BindPoseNormal buffer
                if (bindNorm != null) {
                    VertexBuffer norm = mesh.getBuffer(Type.Normal);
                    FloatBuffer nb = (FloatBuffer) norm.getData();
                    FloatBuffer bnb = (FloatBuffer) bindNorm.getData();
                    nb.clear();
                    bnb.clear();
                    nb.put(bnb).clear();
                }

                //resetting bind tangents if there is a bind tangent buffer
                VertexBuffer bindTangents = mesh.getBuffer(Type.BindPoseTangent);
                if (bindTangents != null) {
                    VertexBuffer tangents = mesh.getBuffer(Type.Tangent);
                    FloatBuffer tb = (FloatBuffer) tangents.getData();
                    FloatBuffer btb = (FloatBuffer) bindTangents.getData();
                    tb.clear();
                    btb.clear();
                    tb.put(btb).clear();
                }

                pb.put(bpb).clear();
            }
        }
    }

    @Override
    public Object jmeClone() {
        return super.jmeClone();
    }

    @Override
    public void cloneFields(Cloner cloner, Object original) {
        super.cloneFields(cloner, original);

        this.armature = cloner.clone(armature);

        // If the targets were cloned then this will clone them.  If the targets
        // were shared then this will share them.
        this.targets = cloner.clone(targets);

        this.numberOfJointsParam = cloner.clone(numberOfJointsParam);
        this.jointMatricesParam = cloner.clone(jointMatricesParam);

        // Rebuild surface attachments against the cloned geometry/graph. The
        // cloner is used for every mutable piece so that the clone owns its
        // source, node, offset, and degeneracy-fallback state outright.
        Map<String, SurfaceAttachment> clonedAttachments = null;
        if (surfaceAttachments != null && !surfaceAttachments.isEmpty()) {
            clonedAttachments = new HashMap<>();
            for (Map.Entry<String, SurfaceAttachment> entry : surfaceAttachments.entrySet()) {
                SurfaceAttachment originalAttachment = entry.getValue();
                SurfaceAttachment copy = originalAttachment.cloneFor(cloner);
                clonedAttachments.put(entry.getKey(), copy);
            }
        }
        this.surfaceAttachments = clonedAttachments;
    }

    /**
     * Provides access to the attachment node for a specific joint in the armature.
     * If an attachment node does not already exist for the named joint, one will be
     * created and attached to the scene graph. Models or effects attached to this
     * node will follow the motion of the corresponding bone.
     *
     * @param jointName the name of the joint
     * @return the attachments node of the joint
     */
    public Node getAttachmentsNode(String jointName) {
        Joint joint = armature.getJoint(jointName);
        if (joint == null) {
            throw new IllegalArgumentException(
                    "Given joint name '" + jointName + "' does not exist in the armature.");
        }

        updateAnimationTargets(spatial);
        int jointIndex = armature.getJointIndex(joint);
        Node attachNode = joint.getAttachmentsNode(jointIndex, targets);

        // Determine the appropriate parent for the attachment node.
        Node parent;
        if (spatial instanceof Node) {
            parent = (Node) spatial; // the usual case
        } else {
            parent = spatial.getParent();
        }
        parent.attachChild(attachNode);

        return attachNode;
    }

    /**
     * Returns the armature associated with this skinning control.
     *
     * @return The pre-existing `Armature` instance.
     */
    public Armature getArmature() {
        return armature;
    }

    /**
     * Returns an array containing all the target meshes that this control
     * is currently affecting.
     *
     * @return A new array of `Mesh` objects.
     */
    public Mesh[] getTargets() {
        Mesh[] result = new Mesh[targets.size()];
        int i = 0;
        for (Geometry geometry : targets) {
            Mesh mesh = geometry.getMesh();
            result[i] = mesh;
            i++;
        }

        return result;
    }

    /**
     * Creates a node that follows a deforming triangle of one of the meshes
     * deformed by this control. Unlike a joint attachment node, the surface
     * attachment tracks the skinned surface itself: its position is the
     * barycentric combination of the three skinned triangle vertices and its
     * orientation is derived from the deformed triangle's edges.
     *
     * <p>The returned node is attached directly to this control's spatial,
     * which must be a {@link Node}. Callers are expected to add their content
     * to the returned node, not to re-parent it.</p>
     *
     * <p>The triangle is identified the same way as
     * {@link Mesh#getTriangle(int, int[])}. The barycentric coordinates must be
     * non-negative and sum to one. The offset is copied and applied on top of
     * the surface frame; it may contain a translation, a rotation, and a
     * positive uniform scale.</p>
     *
     * @param name the name of the new attachment (not null, must not already
     * be in use)
     * @param source one of the animated geometries deformed by this control
     * (not null)
     * @param triangle the triangle index (&ge;0, &lt; triangle count)
     * @param barycentric weights of the triangle's three vertices (not null,
     * length=3, each &ge;0, sum=1)
     * @param offset the offset from the surface frame (not null, copied)
     * @return the new attachment node (not null)
     * @throws IllegalArgumentException if any argument is invalid, if the name
     * is already in use, if the triangle is out of range or degenerate in the
     * bind pose, if the mesh has morph targets, or if the source geometry is
     * not deformed by this control
     */
    public Node addSurfaceAttachment(String name, Geometry source, int triangle,
            Vector3f barycentric, Transform offset) {
        if (name == null || name.isEmpty()) {
            throw new IllegalArgumentException("Attachment name cannot be null or empty.");
        }
        if (source == null) {
            throw new IllegalArgumentException("Source geometry cannot be null.");
        }
        if (barycentric == null) {
            throw new IllegalArgumentException("Barycentric coordinates cannot be null.");
        }
        if (offset == null) {
            throw new IllegalArgumentException("Offset cannot be null.");
        }
        if (!(spatial instanceof Node)) {
            throw new IllegalArgumentException(
                    "Surface attachments require the controlled Spatial to be a Node.");
        }
        if (surfaceAttachments != null && surfaceAttachments.containsKey(name)) {
            throw new IllegalArgumentException(
                    "A surface attachment named '" + name + "' already exists.");
        }

        // Refresh the target list so that a geometry whose mesh changed since
        // the last render is still recognized as belonging to this control.
        updateAnimationTargets(spatial);
        if (!isTargetGeometry(source)) {
            throw new IllegalArgumentException(
                    "The given geometry is not deformed by this control.");
        }

        Mesh mesh = source.getMesh();
        validateSurfaceMesh(mesh);

        int triangleCount = mesh.getTriangleCount();
        if (triangle < 0 || triangle >= triangleCount) {
            throw new IllegalArgumentException("Triangle index " + triangle
                    + " is out of range [0, " + triangleCount + ").");
        }

        float[] bary = {barycentric.x, barycentric.y, barycentric.z};
        validateBarycentric(bary);

        Transform offsetCopy = offset.clone();
        validateOffset(offsetCopy);

        // The triangle winding determines the front face, so keep the index
        // order exactly as the mesh stores it.
        int[] indices = new int[3];
        readTriangleIndices(mesh, triangle, indices);
        validateVertexIndex(mesh, indices[0]);
        validateVertexIndex(mesh, indices[1]);
        validateVertexIndex(mesh, indices[2]);

        VertexBuffer bindPosBuffer = mesh.getBuffer(Type.BindPosePosition);
        FloatBuffer bindPos = requireFloatPositionBuffer(bindPosBuffer, "BindPosePosition");
        for (int index : indices) {
            if ((long) index * 3 + 2 >= bindPos.limit()) {
                throw new IllegalArgumentException(
                        "Triangle index " + triangle + " references vertex " + index
                        + ", which is outside the bind-pose positions.");
            }
        }

        // Validate the bind triangle before creating any scene-graph node.
        Vector3f bindA = readVector(bindPos, indices[0], new Vector3f());
        Vector3f bindB = readVector(bindPos, indices[1], new Vector3f());
        Vector3f bindC = readVector(bindPos, indices[2], new Vector3f());
        Vector3f edge1 = bindB.subtract(bindA);
        Vector3f edge2 = bindC.subtract(bindA);
        if (isDegenerate(edge1, edge2)) {
            throw new IllegalArgumentException(
                    "The bind-pose triangle " + triangle + " is degenerate.");
        }
        if (!isFinite(bindA) || !isFinite(bindB) || !isFinite(bindC)) {
            throw new IllegalArgumentException(
                    "The bind-pose triangle " + triangle + " contains non-finite positions.");
        }

        // The chain from the source geometry to the controlled node must be
        // finite and have non-zero scale.
        Matrix4f sourceToNode = computeSourceToNodeTransform(source);

        SurfaceAttachment attachment = new SurfaceAttachment();
        attachment.name = name;
        attachment.source = source;
        attachment.mesh = mesh;
        attachment.triangle = triangle;
        attachment.indices = indices;
        attachment.barycentric = new Vector3f(bary[0], bary[1], bary[2]);
        attachment.offset = offsetCopy;
        attachment.offsetScale = offsetCopy.getScale().x;

        // Compute the initial transform before publishing anything, so a
        // failure cannot leave a new node in the scene graph.
        armature.update();
        Matrix4f[] palette = armature.computeSkinningMatrices();
        Transform initial = new Transform();
        try {
            computeSurfaceTransform(attachment, sourceToNode, palette, initial);
        } catch (RuntimeException ex) {
            throw new IllegalArgumentException(
                    "Could not compute the initial surface attachment frame.", ex);
        }

        Node node = new Node(name);
        node.setLocalTransform(initial);

        if (surfaceAttachments == null) {
            surfaceAttachments = new HashMap<>();
        }
        surfaceAttachments.put(name, attachment);
        attachment.node = node;

        try {
            ((Node) spatial).attachChild(node);
        } catch (RuntimeException ex) {
            surfaceAttachments.remove(name);
            attachment.node = null;
            throw ex;
        }

        return node;
    }

    /**
     * Removes a surface attachment created by
     * {@link #addSurfaceAttachment(String, Geometry, int, Vector3f, Transform)}.
     * The attachment node is detached from the scene graph and is no longer
     * driven by this control.
     *
     * @param name the name of the attachment to remove
     * @return the detached attachment node, or null if no attachment had that
     * name
     */
    public Node removeSurfaceAttachment(String name) {
        if (surfaceAttachments == null) {
            return null;
        }
        SurfaceAttachment attachment = surfaceAttachments.remove(name);
        if (attachment == null) {
            return null;
        }
        Node node = attachment.node;
        if (node != null) {
            node.removeFromParent();
            attachment.node = null;
        }
        return node;
    }

    /**
     * Returns the node of a surface attachment created by
     * {@link #addSurfaceAttachment(String, Geometry, int, Vector3f, Transform)}.
     *
     * @param name the name of the attachment
     * @return the pre-existing node, or null if no attachment has that name
     */
    public Node getSurfaceAttachment(String name) {
        if (surfaceAttachments == null) {
            return null;
        }
        SurfaceAttachment attachment = surfaceAttachments.get(name);
        return attachment == null ? null : attachment.node;
    }

    /**
     * Tests whether the given geometry is one of this control's deformation
     * targets.
     *
     * @param geometry the geometry to test (may be null)
     * @return true if the geometry is in {@link #targets}
     */
    private boolean isTargetGeometry(Geometry geometry) {
        if (geometry == null) {
            return false;
        }
        for (Geometry target : targets.getArray()) {
            if (target == geometry) {
                return true;
            }
        }
        return false;
    }

    /**
     * Re-evaluates the skeleton pose and drives every surface attachment.
     *
     * <p>All new transforms are computed and validated before any attachment
     * node is modified, so a failure leaves every attachment at its previous
     * transform.</p>
     *
     * @param tpf time per frame (in seconds)
     */
    @Override
    public void postLogicalUpdate(float tpf) {
        if (!enabled) {
            return;
        }
        if (surfaceAttachments == null || surfaceAttachments.isEmpty()) {
            return;
        }

        // Re-confirm the skeleton pose: animation controls may have run after
        // this control's ordinary update in the same frame.
        armature.update();
        Matrix4f[] palette = armature.computeSkinningMatrices();

        SurfaceAttachment[] attachments = surfaceAttachments.values()
                .toArray(new SurfaceAttachment[0]);
        Transform[] computed = new Transform[attachments.length];

        // Snapshot the fallback state of every attachment before evaluating
        // anything, so a failure cannot partially publish new fallback state.
        boolean[] hadRotation = new boolean[attachments.length];
        Quaternion[] oldRotation = new Quaternion[attachments.length];
        for (int i = 0; i < attachments.length; i++) {
            hadRotation[i] = attachments[i].hasValidRotation;
            oldRotation[i] = attachments[i].hasValidRotation
                    ? attachments[i].lastValidRotation.clone() : null;
        }

        try {
            for (int i = 0; i < attachments.length; i++) {
                SurfaceAttachment attachment = attachments[i];

                // The ancestor chain is re-evaluated every frame so that plain
                // parent motion is picked up even while the animation is idle.
                Matrix4f sourceToNode = computeSourceToNodeTransform(attachment.source);

                Transform result = new Transform();
                computeSurfaceTransform(attachment, sourceToNode, palette, result);
                computed[i] = result;
            }
        } catch (RuntimeException ex) {
            for (int i = 0; i < attachments.length; i++) {
                attachments[i].hasValidRotation = hadRotation[i];
                attachments[i].lastValidRotation = oldRotation[i];
            }
            throw ex;
        }

        // Publish only after every attachment evaluated successfully.
        for (int i = 0; i < attachments.length; i++) {
            attachments[i].node.setLocalTransform(computed[i]);
        }
    }

    /**
     * Evaluates the skinned position of a single vertex. This is the shared
     * production path used both by software skinning and by surface
     * attachment sampling.
     *
     * <p>Absolute buffer access is used so that neither direct nor heap
     * buffers have their position or limit modified.</p>
     *
     * @param vertexIndex the vertex index
     * @param bindX the vertex's X coordinate in the bind pose
     * @param bindY the vertex's Y coordinate in the bind pose
     * @param bindZ the vertex's Z coordinate in the bind pose
     * @param boneIndices the mesh's bone-index reader (not null)
     * @param boneWeights the mesh's bone-weight buffer (not null)
     * @param maxWeightsPerVert the number of weights to consider per vertex
     * @param offsetMatrices the armature's current skinning palette (not null)
     * @param store storage for the result (modified)
     */
    private static void skinPosition(int vertexIndex, float bindX, float bindY, float bindZ,
            BoneIndexReader boneIndices, FloatBuffer boneWeights, int maxWeightsPerVert,
            Matrix4f[] offsetMatrices, Vector3f store) {
        int base = vertexIndex * 4;
        if (boneWeights.get(base) == 0f) {
            // Mirror the software-skinning rule: a vertex whose first weight
            // is zero stays at its bind-pose position.
            store.set(bindX, bindY, bindZ);
            return;
        }

        float rx = 0f, ry = 0f, rz = 0f;
        for (int w = 0; w < maxWeightsPerVert; ++w) {
            int weightIndex = base + w;
            float weight = boneWeights.get(weightIndex);
            int boneIndex = boneIndices.get(weightIndex);
            if (boneIndex < 0 || boneIndex >= offsetMatrices.length) {
                throw new IllegalArgumentException("Bone index " + boneIndex
                        + " at vertex " + vertexIndex + " is out of range [0, "
                        + offsetMatrices.length + ").");
            }
            Matrix4f mat = offsetMatrices[boneIndex];
            rx += (mat.m00 * bindX + mat.m01 * bindY + mat.m02 * bindZ + mat.m03) * weight;
            ry += (mat.m10 * bindX + mat.m11 * bindY + mat.m12 * bindZ + mat.m13) * weight;
            rz += (mat.m20 * bindX + mat.m21 * bindY + mat.m22 * bindZ + mat.m23) * weight;
        }
        store.set(rx, ry, rz);
    }

    /**
     * Absolute-index reader for a mesh's bone-index buffer. Unlike a wrapped
     * index buffer, this never alters the underlying buffer's position or
     * limit, and it supports 8-, 16-, and 32-bit index storage.
     */
    private static final class BoneIndexReader {
        private final ByteBuffer bytes;
        private final ShortBuffer shorts;
        private final IntBuffer ints;
        private final int size;

        private BoneIndexReader(VertexBuffer buffer) {
            if (buffer == null) {
                throw new IllegalArgumentException("The mesh has no bone-index buffer.");
            }
            Buffer data = buffer.getData();
            int componentSize = buffer.getFormat().getComponentSize();
            if (data instanceof ByteBuffer && componentSize == 1) {
                bytes = (ByteBuffer) data;
                shorts = null;
                ints = null;
            } else if (data instanceof ShortBuffer && componentSize == 2) {
                bytes = null;
                shorts = (ShortBuffer) data;
                ints = null;
            } else if (data instanceof IntBuffer && componentSize == 4) {
                bytes = null;
                shorts = null;
                ints = (IntBuffer) data;
            } else {
                throw new IllegalArgumentException(
                        "Unsupported bone-index buffer type: " + data.getClass()
                        + " format=" + buffer.getFormat());
            }
            size = data.limit();
        }

        int get(int index) {
            if (bytes != null) {
                return bytes.get(index) & 0xFF;
            }
            if (shorts != null) {
                return shorts.get(index) & 0xFFFF;
            }
            return ints.get(index);
        }

        int size() {
            return size;
        }
    }

    /**
     * Computes the transform from the source geometry's local space to this
     * control's node local space by composing every local transform on the
     * ancestor chain, including the source geometry itself.
     *
     * @param source one of this control's target geometries (not null)
     * @return the composed matrix (a new instance)
     */
    private Matrix4f computeSourceToNodeTransform(Geometry source) {
        Matrix4f chain = new Matrix4f();
        Spatial current = source;
        while (current != null && current != spatial) {
            Transform local = current.getLocalTransform();
            validateLocalTransform(local, current);
            chain = local.toTransformMatrix().mult(chain);
            current = current.getParent();
        }
        if (current != spatial) {
            throw new IllegalArgumentException(
                    "The source geometry is not a descendant of the controlled node.");
        }
        return chain;
    }

    /**
     * Computes the local transform of a surface attachment from the current
     * armature palette. The position follows the barycentric combination of
     * the three deformed triangle vertices at all times. The orientation is
     * derived from the deformed edges whenever they are non-degenerate and
     * falls back to the last valid orientation otherwise.
     *
     * @param attachment the attachment to evaluate (not null)
     * @param sourceToNode the source-to-node transform (not null)
     * @param palette the current skinning palette (not null)
     * @param store storage for the resulting local transform (modified)
     */
    private void computeSurfaceTransform(SurfaceAttachment attachment, Matrix4f sourceToNode,
            Matrix4f[] palette, Transform store) {
        Mesh mesh = attachment.source.getMesh();
        if (mesh != attachment.mesh) {
            throw new IllegalStateException("The mesh of surface attachment '"
                    + attachment.name + "' was replaced.");
        }

        VertexBuffer bindPosBuffer = mesh.getBuffer(Type.BindPosePosition);
        FloatBuffer bindPos = requireFloatPositionBuffer(bindPosBuffer, "BindPosePosition");
        VertexBuffer boneIndexBuffer = mesh.getBuffer(Type.BoneIndex);
        VertexBuffer boneWeightBuffer = mesh.getBuffer(Type.BoneWeight);
        if (boneIndexBuffer == null || boneWeightBuffer == null) {
            throw new IllegalStateException(
                    "The mesh is missing bone index/weight buffers.");
        }
        int maxWeights = mesh.getMaxNumWeights();
        if (maxWeights <= 0) {
            maxWeights = 4;
        }
        // The engine stores at most four weights per vertex.
        maxWeights = Math.min(maxWeights, 4);
        BoneIndexReader boneIndices = new BoneIndexReader(boneIndexBuffer);
        if (boneWeightBuffer.getFormat() != VertexBuffer.Format.Float) {
            throw new IllegalStateException("Bone weights are not stored as floats.");
        }
        FloatBuffer boneWeights = (FloatBuffer) boneWeightBuffer.getData();

        int[] indices = attachment.indices;
        for (int index : indices) {
            validateVertexIndex(mesh, index);
            if ((long) index * 3 + 2 >= bindPos.limit()
                    || (long) index * 4 + 3 >= boneWeights.limit()
                    || (long) index * 4 + 3 >= boneIndices.size()) {
                throw new IllegalStateException(
                        "A triangle vertex is outside the mesh's skinning data.");
            }
        }

        Vector3f deformedA = skinVertex(mesh, indices[0], bindPos, boneIndices,
                boneWeights, maxWeights, palette, sourceToNode);
        Vector3f deformedB = skinVertex(mesh, indices[1], bindPos, boneIndices,
                boneWeights, maxWeights, palette, sourceToNode);
        Vector3f deformedC = skinVertex(mesh, indices[2], bindPos, boneIndices,
                boneWeights, maxWeights, palette, sourceToNode);

        if (!isFinite(deformedA) || !isFinite(deformedB) || !isFinite(deformedC)) {
            throw new IllegalArgumentException(
                    "The deformed triangle contains non-finite positions.");
        }

        Vector3f bary = attachment.barycentric;
        Vector3f centroid = new Vector3f(
                deformedA.x * bary.x + deformedB.x * bary.y + deformedC.x * bary.z,
                deformedA.y * bary.x + deformedB.y * bary.y + deformedC.y * bary.z,
                deformedA.z * bary.x + deformedB.z * bary.y + deformedC.z * bary.z);

        Vector3f edge1 = deformedB.subtract(deformedA);
        Vector3f edge2 = deformedC.subtract(deformedA);

        Quaternion rotation;
        if (isDegenerate(edge1, edge2)) {
            if (attachment.hasValidRotation) {
                rotation = attachment.lastValidRotation;
            } else {
                rotation = computeBindPoseRotation(attachment, bindPos, sourceToNode);
            }
        } else {
            Vector3f xAxis = edge1.normalizeLocal();
            Vector3f zAxis = edge1.cross(edge2).normalizeLocal();
            if (!isFinite(xAxis) || !isFinite(zAxis)) {
                throw new IllegalArgumentException(
                        "The deformed triangle produced a non-finite frame.");
            }
            Vector3f yAxis = zAxis.cross(xAxis);
            rotation = quaternionFromAxes(xAxis, yAxis, zAxis);
            if (!Quaternion.isValidQuaternion(rotation)) {
                throw new IllegalArgumentException(
                        "The deformed triangle produced a non-finite rotation.");
            }
            attachment.lastValidRotation = rotation.clone();
            attachment.hasValidRotation = true;
        }

        float scale = attachment.offsetScale;
        // local = frame * offset, where frame = T(centroid) * R(frame) and
        // offset = T(offsetTranslation) * R(offset) * S(scale). The offset
        // translation is rotated by the frame but not scaled by the offset's
        // own scale.
        Vector3f offsetTranslation = attachment.offset.getTranslation();
        Vector3f translated = rotation.mult(offsetTranslation, null);
        Vector3f translation = centroid.addLocal(translated);
        Quaternion combined = rotation.mult(attachment.offset.getRotation(), new Quaternion());
        combined.normalizeLocal();

        if (!isFinite(translation) || !Quaternion.isValidQuaternion(combined)
                || !Float.isFinite(scale)) {
            throw new IllegalArgumentException(
                    "The surface attachment produced a non-finite transform.");
        }

        store.setTranslation(translation);
        store.setRotation(combined);
        store.setScale(scale, scale, scale);
    }

    /**
     * Evaluates one triangle vertex: bind-pose position through the shared
     * skinning path, then through the source-to-node ancestor chain.
     */
    private static Vector3f skinVertex(Mesh mesh, int vertexIndex, FloatBuffer bindPos,
            BoneIndexReader boneIndices, FloatBuffer boneWeights, int maxWeights,
            Matrix4f[] palette, Matrix4f sourceToNode) {
        int offset = vertexIndex * 3;
        float bx = bindPos.get(offset);
        float by = bindPos.get(offset + 1);
        float bz = bindPos.get(offset + 2);
        Vector3f skinned = new Vector3f();
        skinPosition(vertexIndex, bx, by, bz, boneIndices, boneWeights, maxWeights,
                palette, skinned);
        return sourceToNode.mult(skinned, skinned);
    }

    /**
     * Computes the fallback rotation from the bind-pose triangle after applying
     * the current source-to-node ancestor chain.
     */
    private Quaternion computeBindPoseRotation(SurfaceAttachment attachment, FloatBuffer bindPos,
            Matrix4f sourceToNode) {
        int[] indices = attachment.indices;
        Vector3f a = sourceToNode.mult(readVector(bindPos, indices[0], new Vector3f()), new Vector3f());
        Vector3f b = sourceToNode.mult(readVector(bindPos, indices[1], new Vector3f()), new Vector3f());
        Vector3f c = sourceToNode.mult(readVector(bindPos, indices[2], new Vector3f()), new Vector3f());

        Vector3f edge1 = b.subtract(a);
        Vector3f edge2 = c.subtract(a);
        if (isDegenerate(edge1, edge2)) {
            return new Quaternion();
        }
        Vector3f xAxis = edge1.normalizeLocal();
        Vector3f zAxis = edge1.cross(edge2).normalizeLocal();
        Vector3f yAxis = zAxis.cross(xAxis);
        Quaternion rotation = quaternionFromAxes(xAxis, yAxis, zAxis);
        return Quaternion.isValidQuaternion(rotation) ? rotation : new Quaternion();
    }

    /**
     * Builds a quaternion from a right-handed, orthonormal set of axis
     * column vectors.
     */
    private static Quaternion quaternionFromAxes(Vector3f xAxis, Vector3f yAxis, Vector3f zAxis) {
        Matrix3f matrix = new Matrix3f();
        matrix.fromAxes(xAxis, yAxis, zAxis);
        Quaternion rotation = new Quaternion();
        rotation.fromRotationMatrix(matrix);
        rotation.normalizeLocal();
        return rotation;
    }

    private static void validateBarycentric(float[] bary) {
        float sum = 0f;
        for (float value : bary) {
            if (!Float.isFinite(value) || value < 0f) {
                throw new IllegalArgumentException(
                        "Barycentric coordinates must be finite and non-negative.");
            }
            sum += value;
        }
        if (Math.abs(sum - 1f) > 1e-4f) {
            throw new IllegalArgumentException(
                    "Barycentric coordinates must sum to 1 (got " + sum + ").");
        }
    }

    /**
     * Resolves the vertex indices of one triangle without altering any buffer's
     * position or limit. Unlike {@link Mesh#getTriangle(int, int[])}, this
     * method reads the index data absolutely and also supports strip and fan
     * modes.
     *
     * @param mesh the mesh to read from (not null)
     * @param triangle the triangle index (&ge;0, &lt; triangle count)
     * @param out storage for the three vertex indices (length=3, modified)
     */
    private static void readTriangleIndices(Mesh mesh, int triangle, int[] out) {
        Mesh.Mode mode = mesh.getMode();
        VertexBuffer indexBuffer = mesh.getBuffer(Type.Index);
        int base = triangle * 3;
        if (mode == Mesh.Mode.Triangles) {
            for (int k = 0; k < 3; k++) {
                out[k] = readRawIndex(indexBuffer, base + k);
            }
            return;
        }
        // TriangleStrip / TriangleFan: map the list position to a "virtual"
        // sequential index, then optionally resolve through the raw buffer.
        for (int k = 0; k < 3; k++) {
            int listIndex = base + k;
            int superIndex;
            if (mode == Mesh.Mode.TriangleStrip) {
                int triIndex = listIndex / 3;
                int vertIndex = listIndex % 3;
                boolean isBack = triIndex % 2 == 1;
                if (!isBack) {
                    superIndex = triIndex + vertIndex;
                } else {
                    switch (vertIndex) {
                        case 0:
                            superIndex = triIndex + 1;
                            break;
                        case 1:
                            superIndex = triIndex;
                            break;
                        default:
                            superIndex = triIndex + 2;
                            break;
                    }
                }
            } else if (mode == Mesh.Mode.TriangleFan) {
                int vertIndex = listIndex % 3;
                superIndex = vertIndex == 0 ? 0 : (listIndex / 3) + vertIndex;
            } else {
                throw new IllegalArgumentException(
                        "Surface attachments require a triangle mesh mode.");
            }
            out[k] = readRawIndex(indexBuffer, superIndex);
        }
    }

    /**
     * Reads one raw index value absolutely. A null buffer means the mesh has no
     * index buffer, in which case indices are sequential.
     */
    private static int readRawIndex(VertexBuffer indexBuffer, int index) {
        if (indexBuffer == null) {
            return index;
        }
        Buffer data = indexBuffer.getData();
        if (data instanceof ByteBuffer) {
            // Mesh indices are stored unsigned; mask off sign extension.
            return ((ByteBuffer) data).get(index) & 0xFF;
        } else if (data instanceof ShortBuffer) {
            return ((ShortBuffer) data).get(index) & 0xFFFF;
        } else if (data instanceof IntBuffer) {
            return ((IntBuffer) data).get(index);
        }
        throw new IllegalArgumentException(
                "Unsupported index buffer type: " + data.getClass());
    }

    private static void validateSurfaceMesh(Mesh mesh) {
        if (mesh == null) {
            throw new IllegalArgumentException("The source geometry has no mesh.");
        }
        if (mesh.hasMorphTargets()) {
            throw new IllegalArgumentException(
                    "Surface attachments are not supported for morph meshes.");
        }
        if (!mesh.isAnimated()) {
            throw new IllegalArgumentException("The source mesh is not animated.");
        }
        Mesh.Mode mode = mesh.getMode();
        if (mode != Mesh.Mode.Triangles && mode != Mesh.Mode.TriangleStrip
                && mode != Mesh.Mode.TriangleFan) {
            throw new IllegalArgumentException(
                    "Surface attachments require a triangle mesh mode.");
        }
        if (mesh.getBuffer(Type.BindPosePosition) == null
                || mesh.getBuffer(Type.BoneIndex) == null
                || mesh.getBuffer(Type.BoneWeight) == null) {
            throw new IllegalArgumentException(
                    "The source mesh lacks bind-pose or bone data.");
        }
    }

    private static void validateOffset(Transform offset) {
        Vector3f translation = offset.getTranslation();
        Vector3f scale = offset.getScale();
        Quaternion rotation = offset.getRotation();
        if (!isFinite(translation) || !isFinite(scale)
                || !Quaternion.isValidQuaternion(rotation)) {
            throw new IllegalArgumentException("The offset transform is not finite.");
        }
        if (!(scale.x > 0f) || !(scale.y > 0f) || !(scale.z > 0f)) {
            throw new IllegalArgumentException(
                    "The offset scale must be positive and non-zero.");
        }
        float tolerance = 1e-5f * Math.max(1f, scale.x);
        if (Math.abs(scale.x - scale.y) > tolerance
                || Math.abs(scale.x - scale.z) > tolerance) {
            throw new IllegalArgumentException(
                    "The offset scale must be uniform.");
        }
    }

    private static void validateLocalTransform(Transform local, Spatial owner) {
        Vector3f translation = local.getTranslation();
        Vector3f scale = local.getScale();
        Quaternion rotation = local.getRotation();
        if (!isFinite(translation) || !isFinite(scale)
                || !Quaternion.isValidQuaternion(rotation)) {
            throw new IllegalArgumentException("The local transform of spatial '"
                    + owner.getName() + "' is not finite.");
        }
        if (scale.x == 0f || scale.y == 0f || scale.z == 0f) {
            throw new IllegalArgumentException("The local transform of spatial '"
                    + owner.getName() + "' has a zero scale.");
        }
    }

    private static FloatBuffer requireFloatPositionBuffer(VertexBuffer buffer, String name) {
        if (buffer == null) {
            throw new IllegalArgumentException("The mesh has no " + name + " buffer.");
        }
        if (buffer.getFormat() != VertexBuffer.Format.Float
                || buffer.getNumComponents() != 3) {
            throw new IllegalArgumentException(
                    "The " + name + " buffer is not a 3-component float buffer.");
        }
        return (FloatBuffer) buffer.getData();
    }

    private static Vector3f readVector(FloatBuffer buffer, int vertexIndex, Vector3f store) {
        int offset = vertexIndex * 3;
        return store.set(buffer.get(offset), buffer.get(offset + 1), buffer.get(offset + 2));
    }

    private static void validateVertexIndex(Mesh mesh, int vertexIndex) {
        if (vertexIndex < 0 || vertexIndex >= mesh.getVertexCount()) {
            throw new IllegalArgumentException("Vertex index " + vertexIndex
                    + " is out of range [0, " + mesh.getVertexCount() + ").");
        }
    }

    private static boolean isDegenerate(Vector3f edge1, Vector3f edge2) {
        return edge1.length() < MIN_SURFACE_EDGE_LENGTH
                || edge2.length() < MIN_SURFACE_EDGE_LENGTH
                || edge1.cross(edge2).length() < MIN_SURFACE_CROSS_LENGTH;
    }

    private static boolean isFinite(Vector3f vector) {
        return Float.isFinite(vector.x) && Float.isFinite(vector.y)
                && Float.isFinite(vector.z);
    }

    /**
     * One surface attachment: a triangle of a skinned mesh plus the offset
     * applied on top of the resulting surface frame.
     */
    private static class SurfaceAttachment {
        String name;
        Geometry source;
        Mesh mesh;
        int triangle;
        int[] indices;
        Vector3f barycentric;
        Transform offset;
        float offsetScale;
        Node node;
        Quaternion lastValidRotation;
        boolean hasValidRotation;

        /**
         * Creates an independent copy of this attachment for a cloned scene
         * graph, resolving the source and node through the cloner so they line
         * up with the rest of the cloned graph.
         *
         * @param cloner the cloner performing the deep clone (not null)
         * @return a new attachment owned by the clone (not null)
         */
        SurfaceAttachment cloneFor(Cloner cloner) {
            SurfaceAttachment copy = new SurfaceAttachment();
            copy.name = name;
            copy.source = cloner.clone(source);
            copy.mesh = copy.source == null ? null : copy.source.getMesh();
            copy.triangle = triangle;
            copy.indices = indices == null ? null : indices.clone();
            copy.barycentric = barycentric == null ? null : barycentric.clone();
            copy.offset = offset == null ? null : offset.clone();
            copy.offsetScale = offsetScale;
            copy.node = cloner.clone(node);
            copy.lastValidRotation = lastValidRotation == null
                    ? null : lastValidRotation.clone();
            copy.hasValidRotation = hasValidRotation;
            return copy;
        }
    }

    /**
     * Applies software skinning transformations to the given mesh using the
     * provided bone offset matrices.
     *
     * @param mesh           The mesh to deform.
     * @param offsetMatrices The array of transformation matrices for each bone.
     */
    private void applySoftwareSkinning(Mesh mesh, Matrix4f[] offsetMatrices) {

        VertexBuffer tb = mesh.getBuffer(Type.Tangent);
        if (tb == null) {
            // if there are no tangents use the classic skinning
            applySkinning(mesh, offsetMatrices);
        } else {
            // if there are tangents use the skinning with tangents
            applySkinningTangents(mesh, offsetMatrices, tb);
        }
    }

    /**
     * Applies skinning transformations to a mesh's position and normal buffers.
     * This method iterates through each vertex, applies the weighted sum of
     * bone transformations, and updates the vertex buffers.
     *
     * @param mesh           The mesh to apply skinning to.
     * @param offsetMatrices The bone offset matrices to use for transformation.
     */
    private void applySkinning(Mesh mesh, Matrix4f[] offsetMatrices) {
        int maxWeightsPerVert = mesh.getMaxNumWeights();
        if (maxWeightsPerVert <= 0) {
            throw new IllegalStateException("Max weights per vert is incorrectly set!");
        }
        int maxWeights = Math.min(4, maxWeightsPerVert);

        // NOTE: This code assumes the vertex buffer is in bind pose
        // resetToBind() has been called this frame
        VertexBuffer vb = mesh.getBuffer(Type.Position);
        FloatBuffer fvb = (FloatBuffer) vb.getData();

        VertexBuffer nb = mesh.getBuffer(Type.Normal);
        FloatBuffer fnb = (nb == null) ? null : (FloatBuffer) nb.getData();

        // get boneIndexes and weights for mesh
        BoneIndexReader ib = new BoneIndexReader(mesh.getBuffer(Type.BoneIndex));
        FloatBuffer wb = (FloatBuffer) mesh.getBuffer(Type.BoneWeight).getData();

        int vertexCount = fvb.limit() / 3;

        TempVars vars = TempVars.get();
        Vector3f position = vars.vect1;

        // iterate vertices and apply skinning transform for each effecting bone
        for (int vert = 0; vert < vertexCount; vert++) {
            int weightBase = vert * 4;
            // Skip this vertex if the first weight is zero.
            if (wb.get(weightBase) == 0f) {
                continue;
            }

            int idxPositions = vert * 3;
            float vtx = fvb.get(idxPositions);
            float vty = fvb.get(idxPositions + 1);
            float vtz = fvb.get(idxPositions + 2);

            // Shared production evaluation of the skinned position.
            skinPosition(vert, vtx, vty, vtz, ib, wb, maxWeights, offsetMatrices, position);

            if (fnb != null) {
                float nmx = fnb.get(idxPositions);
                float nmy = fnb.get(idxPositions + 1);
                float nmz = fnb.get(idxPositions + 2);

                float rnx = 0, rny = 0, rnz = 0;
                for (int w = maxWeights - 1; w >= 0; w--) {
                    float weight = wb.get(weightBase + w);
                    Matrix4f mat = offsetMatrices[ib.get(weightBase + w)];

                    rnx += (nmx * mat.m00 + nmy * mat.m01 + nmz * mat.m02) * weight;
                    rny += (nmx * mat.m10 + nmy * mat.m11 + nmz * mat.m12) * weight;
                    rnz += (nmx * mat.m20 + nmy * mat.m21 + nmz * mat.m22) * weight;
                }

                fnb.put(idxPositions, rnx);
                fnb.put(idxPositions + 1, rny);
                fnb.put(idxPositions + 2, rnz);
            }

            fvb.put(idxPositions, position.x);
            fvb.put(idxPositions + 1, position.y);
            fvb.put(idxPositions + 2, position.z);
        }

        vars.release();

        vb.updateData(fvb);
        if (nb != null) {
            nb.updateData(fnb);
        }
    }

    /**
     * Applies skinning transformations to a mesh's position, normal, and tangent buffers.
     * This method is specifically designed for meshes that include tangent data,
     * ensuring proper deformation of tangents alongside positions and normals.
     *
     * @param mesh           The mesh to apply skinning to.
     * @param offsetMatrices The bone offset matrices to use for transformation.
     * @param tb             The tangent `VertexBuffer`.
     */
    private void applySkinningTangents(Mesh mesh, Matrix4f[] offsetMatrices, VertexBuffer tb) {
        int maxWeightsPerVert = mesh.getMaxNumWeights();

        if (maxWeightsPerVert <= 0) {
            throw new IllegalStateException("Max weights per vert is incorrectly set!");
        }
        int maxWeights = Math.min(4, maxWeightsPerVert);

        // NOTE: This code assumes the vertex buffer is in bind pose
        // resetToBind() has been called this frame
        VertexBuffer vb = mesh.getBuffer(Type.Position);
        FloatBuffer fvb = (FloatBuffer) vb.getData();

        VertexBuffer nb = mesh.getBuffer(Type.Normal);
        FloatBuffer fnb = (nb == null) ? null : (FloatBuffer) nb.getData();

        FloatBuffer ftb = (FloatBuffer) tb.getData();

        // get boneIndexes and weights for mesh
        BoneIndexReader ib = new BoneIndexReader(mesh.getBuffer(Type.BoneIndex));
        FloatBuffer wb = (FloatBuffer) mesh.getBuffer(Type.BoneWeight).getData();

        int vertexCount = fvb.limit() / 3;

        TempVars vars = TempVars.get();
        Vector3f position = vars.vect1;

        // iterate vertices and apply skinning transform for each effecting bone
        for (int vert = 0; vert < vertexCount; vert++) {
            int weightBase = vert * 4;
            int idxPositions = vert * 3;
            int idxTangents = vert * 4;

            // Skip this vertex if the first weight is zero.
            if (wb.get(weightBase) == 0f) {
                continue;
            }

            float vtx = fvb.get(idxPositions);
            float vty = fvb.get(idxPositions + 1);
            float vtz = fvb.get(idxPositions + 2);

            // Shared production evaluation of the skinned position.
            skinPosition(vert, vtx, vty, vtz, ib, wb, maxWeights, offsetMatrices, position);

            float tnx = ftb.get(idxTangents);
            float tny = ftb.get(idxTangents + 1);
            float tnz = ftb.get(idxTangents + 2);

            float rnx = 0, rny = 0, rnz = 0, rtx = 0, rty = 0, rtz = 0;
            for (int w = maxWeights - 1; w >= 0; w--) {
                float weight = wb.get(weightBase + w);
                Matrix4f mat = offsetMatrices[ib.get(weightBase + w)];

                if (fnb != null) {
                    float nmx = fnb.get(idxPositions);
                    float nmy = fnb.get(idxPositions + 1);
                    float nmz = fnb.get(idxPositions + 2);

                    rnx += (nmx * mat.m00 + nmy * mat.m01 + nmz * mat.m02) * weight;
                    rny += (nmx * mat.m10 + nmy * mat.m11 + nmz * mat.m12) * weight;
                    rnz += (nmx * mat.m20 + nmy * mat.m21 + nmz * mat.m22) * weight;
                }

                rtx += (tnx * mat.m00 + tny * mat.m01 + tnz * mat.m02) * weight;
                rty += (tnx * mat.m10 + tny * mat.m11 + tnz * mat.m12) * weight;
                rtz += (tnx * mat.m20 + tny * mat.m21 + tnz * mat.m22) * weight;
            }

            if (fnb != null) {
                fnb.put(idxPositions, rnx);
                fnb.put(idxPositions + 1, rny);
                fnb.put(idxPositions + 2, rnz);
            }

            fvb.put(idxPositions, position.x);
            fvb.put(idxPositions + 1, position.y);
            fvb.put(idxPositions + 2, position.z);

            // skipping the 4th component of the tangent since it doesn't have to be transformed
            ftb.put(idxTangents, rtx);
            ftb.put(idxTangents + 1, rty);
            ftb.put(idxTangents + 2, rtz);
        }

        vars.release();

        vb.updateData(fvb);
        if (fnb != null) {
            nb.updateData(fnb);
        }
        tb.updateData(ftb);
    }

    /**
     * Serialize this Control to the specified exporter, for example when saving
     * to a J3O file.
     *
     * @param ex the exporter to write to (not null)
     * @throws IOException from the exporter
     */
    @Override
    public void write(JmeExporter ex) throws IOException {
        super.write(ex);
        OutputCapsule oc = ex.getCapsule(this);
        oc.write(armature, "armature", null);
    }

    /**
     * De-serialize this Control from the specified importer, for example when
     * loading from a J3O file.
     *
     * @param im the importer to read from (not null)
     * @throws IOException from the importer
     */
    @Override
    public void read(JmeImporter im) throws IOException {
        super.read(im);
        InputCapsule in = im.getCapsule(this);
        armature = (Armature) in.readSavable("armature", null);

        for (MatParamOverride mpo : spatial.getLocalMatParamOverrides().getArray()) {
            if (mpo.getName().equals("NumberOfBones") || mpo.getName().equals("BoneMatrices")) {
                spatial.removeMatParamOverride(mpo);
            }
        }
        spatial.addMatParamOverride(numberOfJointsParam);
        spatial.addMatParamOverride(jointMatricesParam);
    }

    /**
     * Update the lists of animation targets.
     *
     * @param spatial the controlled spatial
     */
    private void updateAnimationTargets(Spatial spatial) {
        targets.clear();
        collectAnimatedGeometries(spatial);
    }

}
