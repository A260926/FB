import { RenderCollector } from './RenderCollector.js';
import { Plane } from '../math/Plane.js';
import { Vector3 } from '../math/Vector3.js';
import { Sphere } from '../math/Sphere.js';

/**
 * World-space tolerance used for polygon clipping and plane tangency.
 * @type {number}
 */
const EPS = 1e-7;

const _eye = new Vector3();
const _normal = new Vector3();
const _edge = new Vector3();
const _toEye = new Vector3();
const _cross = new Vector3();
const _dir = new Vector3();
const _w = new Vector3();
const _sphere = new Sphere();

/**
 * PortalRenderCollector extends {@link RenderCollector} with room/portal based
 * visibility collection. Objects behind walls are only collected when they are
 * reachable from the camera's room through a chain of portal polygons.
 *
 * Rooms are non-nested {@link Object3D} subtrees inside one {@link Scene}.
 * A portal is a directed edge between two rooms, described by a convex polygon
 * on a door node. Visible volumes (sets of normalized clipping half-spaces,
 * interior convention `n·p + d >= 0`) are propagated room by room, and a mesh
 * is queued only when its world bounding sphere satisfies every half-space of
 * at least one reaching volume, in addition to the ordinary
 * {@link RenderCollector#checkVisibility} result.
 *
 * Only perspective cameras are supported. Cameras without an assigned room
 * fall back to ordinary {@link RenderCollector} behavior.
 * @extends RenderCollector
 */
class PortalRenderCollector extends RenderCollector {

	/**
	 * Creates a new PortalRenderCollector instance. Replace `scene.collector`
	 * with it so that {@link Scene#updateRenderStates} and
	 * {@link Scene#updateRenderQueue} keep working unchanged.
	 */
	constructor() {
		super();

		// registered rooms: root Object3D -> RoomInfo
		this._rooms = new Map();

		// registered portals: handle -> Portal
		this._portals = new Map();

		// camera -> room root; enumerable so removing a room can invalidate
		// every camera still assigned to it
		this._cameraRooms = new Map();

		// cameras still selecting a removed room; cleared by setCameraRoom
		this._invalidCameras = new WeakSet();

		this._portalId = 0;

		/**
		 * Number of convex-polygon-against-half-space clipping operations
		 * performed during the latest collection. Reset on every collection,
		 * including ordinary (non-portal) ones.
		 * @type {number}
		 * @default 0
		 */
		this.portal_clips = 0;

		/**
		 * Number of room bounding-sphere against visible-volume half-space
		 * tests performed during the latest collection. Each reaching volume
		 * of a room counts as one test; testing stops at the first passing
		 * volume. Reset on every collection, including ordinary (non-portal)
		 * ones.
		 * @type {number}
		 * @default 0
		 */
		this.sphere_tests = 0;
	}

	/**
	 * Register a room subtree.
	 * @param {Object3D} root - The room root. It must be an Object3D (not a
	 * Scene), registered only once, and neither contain nor be contained by
	 * another registered room.
	 * @returns {Object3D} The passed room root.
	 * @throws {TypeError} If `root` is not an Object3D or is a Scene.
	 * @throws {Error} If `root` is already registered or nested with another
	 * registered room.
	 */
	addRoom(root) {
		if (!root || !root.isObject3D) {
			throw new TypeError('PortalRenderCollector.addRoom: root must be an Object3D.');
		}
		if (root.isScene) {
			throw new Error('PortalRenderCollector.addRoom: a Scene cannot be used as a room.');
		}
		if (this._rooms.has(root)) {
			throw new Error('PortalRenderCollector.addRoom: room root is already registered.');
		}
		for (const other of this._rooms.keys()) {
			if (_isAncestor(other, root) || _isAncestor(root, other)) {
				throw new Error('PortalRenderCollector.addRoom: rooms must not be nested.');
			}
		}

		this._rooms.set(root, { root: root, outgoing: [], meshes: [] });

		return root;
	}

	/**
	 * Unregister a room. Every portal edge incident to the room is removed as
	 * well. A camera still assigned to this room will throw on the next
	 * collection until {@link PortalRenderCollector#setCameraRoom} is called
	 * again.
	 * @param {Object3D} root - The room root previously passed to
	 * {@link PortalRenderCollector#addRoom}.
	 * @returns {void}
	 * @throws {Error} If `root` is not a registered room.
	 */
	removeRoom(root) {
		if (!this._rooms.has(root)) {
			throw new Error('PortalRenderCollector.removeRoom: room root is not registered.');
		}

		for (const [handle, portal] of this._portals) {
			if (portal.from === root || portal.to === root) {
				this._portals.delete(handle);
			}
		}

		// A camera still selecting this room stays invalid until it is reset,
		// even if the same root is registered again later.
		for (const [camera, assigned] of this._cameraRooms) {
			if (assigned === root) {
				this._invalidCameras.add(camera);
			}
		}

		this._rooms.delete(root);
	}

	/**
	 * Register a directed portal edge from one room to another.
	 *
	 * The polygon is given in the local space of `node` as at least three
	 * finite Vector3 points, in winding order, forming a convex, coplanar,
	 * non-zero-area polygon. The winding normal points toward the source room
	 * (`from`); the camera eye must be on that positive side for the edge to
	 * be traversable, so bidirectional doors need two reverse registrations.
	 * The vertices are copied; the caller may freely reuse or mutate its array
	 * afterwards.
	 * @param {Object3D} from - The registered room root the portal leaves from.
	 * @param {Object3D} to - The registered room root the portal enters.
	 * @param {Object3D} node - The door node in the same Scene. Its latest
	 * worldMatrix is used at collection time.
	 * @param {Vector3[]} vertices - The portal polygon in `node` local space.
	 * @returns {object} An opaque handle to pass to
	 * {@link PortalRenderCollector#removePortal}.
	 * @throws {TypeError} If an argument has the wrong type, or `vertices` is
	 * not an array of at least three finite Vector3 values.
	 * @throws {Error} If the rooms are unknown/identical, or the polygon is
	 * degenerate (zero area), non-coplanar or non-convex.
	 */
	addPortal(from, to, node, vertices) {
		if (!this._rooms.has(from)) {
			throw new Error('PortalRenderCollector.addPortal: "from" room is not registered.');
		}
		if (!this._rooms.has(to)) {
			throw new Error('PortalRenderCollector.addPortal: "to" room is not registered.');
		}
		if (from === to) {
			throw new Error('PortalRenderCollector.addPortal: "from" and "to" must be different rooms.');
		}
		if (!node || !node.isObject3D) {
			throw new TypeError('PortalRenderCollector.addPortal: node must be an Object3D.');
		}
		if (!Array.isArray(vertices) || vertices.length < 3) {
			throw new TypeError('PortalRenderCollector.addPortal: vertices must be an array of at least three Vector3.');
		}

		const points = [];
		for (let i = 0; i < vertices.length; i++) {
			const v = vertices[i];
			if (!v || !v.isVector3 || !_isFinitePoint(v)) {
				throw new TypeError('PortalRenderCollector.addPortal: every vertex must be a finite Vector3.');
			}
			points.push(v.clone()); // copy: do not hold the caller's array
		}

		const normal = _polygonNormal(points);
		if (!normal) {
			throw new Error('PortalRenderCollector.addPortal: portal polygon must have a non-zero area.');
		}
		if (!_isCoplanar(points, normal)) {
			throw new Error('PortalRenderCollector.addPortal: portal polygon must be coplanar.');
		}
		if (!_isConvex(points, normal)) {
			throw new Error('PortalRenderCollector.addPortal: portal polygon must be convex in winding order.');
		}

		const handle = { __portalHandle: true, id: ++this._portalId };
		this._portals.set(handle, {
			handle: handle,
			from: from,
			to: to,
			node: node,
			localPoints: points
		});

		return handle;
	}

	/**
	 * Remove a portal edge previously registered with
	 * {@link PortalRenderCollector#addPortal}.
	 * @param {object} handle - The handle returned by `addPortal`.
	 * @returns {void}
	 * @throws {Error} If the handle is unknown or has already been removed
	 * (including edges removed together with a room).
	 */
	removePortal(handle) {
		if (!handle || !this._portals.has(handle)) {
			throw new Error('PortalRenderCollector.removePortal: unknown portal handle.');
		}
		this._portals.delete(handle);
	}

	/**
	 * Assign a camera to a room. The assignment is kept per camera, so two
	 * cameras may collect different rooms without interfering. Pass `null` to
	 * clear the assignment; that camera then uses ordinary collection.
	 * @param {Camera} camera - The perspective camera.
	 * @param {Object3D|null} rootOrNull - A registered room root, or `null` to
	 * clear the assignment.
	 * @returns {void}
	 * @throws {TypeError} If `camera` is not a Camera.
	 * @throws {Error} If `rootOrNull` is not a registered room.
	 */
	setCameraRoom(camera, rootOrNull) {
		if (!camera || !camera.isCamera) {
			throw new TypeError('PortalRenderCollector.setCameraRoom: first argument must be a Camera.');
		}
		if (rootOrNull === null) {
			this._cameraRooms.delete(camera);
			this._invalidCameras.delete(camera);
			return;
		}
		if (!this._rooms.has(rootOrNull)) {
			throw new Error('PortalRenderCollector.setCameraRoom: room root is not registered.');
		}
		this._cameraRooms.set(camera, rootOrNull);
		this._invalidCameras.delete(camera);
	}

	/**
	 * Traverse the scene and collect renderable objects, lights and skeletons.
	 *
	 * When the camera has an assigned room, portal propagation is used.
	 * Otherwise the ordinary {@link RenderCollector} traversal runs unchanged.
	 * @param {Scene} scene - The scene to traverse.
	 * @param {Camera} camera - The render camera.
	 * @returns {import('./RenderQueue.js').RenderQueue} The collected render queue.
	 * @throws {Error} If portal collection is requested with a non-perspective
	 * camera, the assigned room has been removed or is hidden/detached, or a
	 * camera still points at a room that was removed.
	 */
	traverseAndCollect(scene, camera) {
		const lightingData = this.lightingData;
		const lightingNeedsUpdate = this._lightingNeedsUpdate;
		const renderQueue = this.getRenderQueue(camera);

		const cameraRoom = this._cameraRooms.has(camera) ? this._cameraRooms.get(camera) : undefined;
		const usePortals = cameraRoom !== undefined && cameraRoom !== null && this._rooms.size > 0;

		if (this._invalidCameras.has(camera)) {
			throw new Error('PortalRenderCollector: camera room was removed; call setCameraRoom again before collecting.');
		}

		if (usePortals) {
			if (camera.projectionMatrix.elements[11] !== -1) {
				throw new Error('PortalRenderCollector: only perspective cameras are supported.');
			}
			if (!this._isObjectActiveInScene(cameraRoom, scene)) {
				throw new Error('PortalRenderCollector: camera room is hidden or detached from the scene.');
			}
		}

		if (lightingNeedsUpdate) {
			lightingData.begin();
		}
		renderQueue.begin();

		this.portal_clips = 0;
		this.sphere_tests = 0;

		if (usePortals) {
			this._collectWithPortals(scene, camera, cameraRoom, renderQueue);
		} else {
			this._ordinaryTraverse(scene, camera, renderQueue);
		}

		renderQueue.end();

		if (lightingNeedsUpdate) {
			lightingData.end(this.sceneData);
			this._lightingNeedsUpdate = false;
		}

		// Same versioning contract as RenderCollector: a shared skeleton is
		// updated once per skeleton version, and skeletons collected in later
		// frames catch up on their first collection.
		const skeletonVersion = this._skeletonVersion;
		for (const skeleton of renderQueue.skeletons) {
			if (skeleton._version !== skeletonVersion) {
				skeleton.updateBones(this.sceneData);
				skeleton._version = skeletonVersion;
			}
		}

		return renderQueue;
	}

	/**
	 * Portal-aware collection. Builds the active room/portal graph from the
	 * current tree, propagates visible volumes and queues accepted meshes
	 * through the ordinary RenderQueue.
	 * @private
	 * @param {Scene} scene - The scene.
	 * @param {Camera} camera - The render camera.
	 * @param {Object3D} cameraRoom - The camera's room root.
	 * @param {import('./RenderQueue.js').RenderQueue} renderQueue - The target queue.
	 */
	_collectWithPortals(scene, camera, cameraRoom, renderQueue) {
		const activeRooms = this._buildActiveGraph(scene);
		const cameraInfo = activeRooms.get(cameraRoom);

		const context = {
			camera: camera,
			renderQueue: renderQueue,
			activeRooms: activeRooms,
			outside: [],
			lights: [],
			volumesByRoom: new Map(),
			pushed: new Set()
		};

		// Full-scene walk: lights keep whole-scene visibility rules, meshes are
		// bucketed by their current room ancestry or the ordinary path.
		this._walkScene(scene, null, context);

		for (const light of context.lights) {
			this.lightingData.collect(light);
		}

		// Seed propagation with the camera's original frustum and room.
		const seedPlanes = [];
		const frustumPlanes = camera.frustum.planes;
		for (let i = 0; i < frustumPlanes.length; i++) {
			seedPlanes.push(frustumPlanes[i].clone());
		}
		this._propagate(cameraInfo, seedPlanes, new Set(), context);

		// Room meshes: pass at least one reaching volume's half-spaces, then
		// intersect with the ordinary checkVisibility result.
		for (const [roomInfo, volumes] of context.volumesByRoom) {
			const meshes = roomInfo.meshes;
			for (let i = 0; i < meshes.length; i++) {
				const object = meshes[i];
				if (context.pushed.has(object)) {
					continue;
				}
				if (!this._testRoomMesh(object, volumes)) {
					continue;
				}
				if (!this.checkVisibility(object, camera)) {
					continue;
				}
				context.pushed.add(object);
				renderQueue.push(object, camera);
			}
		}

		// Room-exterior meshes use the ordinary path untouched.
		for (let i = 0; i < context.outside.length; i++) {
			const object = context.outside[i];
			if (context.pushed.has(object)) {
				continue;
			}
			if (this.checkVisibility(object, camera)) {
				context.pushed.add(object);
				renderQueue.push(object, camera);
			}
		}
	}

	/**
	 * Ordinary traversal used when no portal collection applies. It mirrors the
	 * base collector behavior but tracks skeletons through the render queue, so
	 * the base class's module-level skeleton set is never touched.
	 * @private
	 * @param {Object3D} object - Current subtree.
	 * @param {Camera} camera - The render camera.
	 * @param {import('./RenderQueue.js').RenderQueue} renderQueue - The target queue.
	 */
	_ordinaryTraverse(object, camera, renderQueue) {
		if (!object.visible) {
			return;
		}

		if (object.isMesh) {
			if (this.checkVisibility(object, camera)) {
				renderQueue.push(object, camera);
			}
		} else if (object.isLight) {
			this.lightingData.collect(object);
		}

		const children = object.children;
		for (let i = 0; i < children.length; i++) {
			this._ordinaryTraverse(children[i], camera, renderQueue);
		}
	}

	/**
	 * Rebuild the active graph from the current scene tree. Hidden or detached
	 * rooms and door nodes do not participate, so reparenting, moving door
	 * nodes and detach/reattach take effect on the next collection.
	 * @private
	 * @param {Scene} scene - The scene.
	 * @returns {Map<Object3D, object>} Active room infos keyed by root.
	 */
	_buildActiveGraph(scene) {
		const activeRooms = new Map();

		for (const info of this._rooms.values()) {
			info.outgoing.length = 0;
			info.meshes.length = 0;
			if (this._isObjectActiveInScene(info.root, scene)) {
				activeRooms.set(info.root, info);
			}
		}

		for (const portal of this._portals.values()) {
			if (!activeRooms.has(portal.from) || !activeRooms.has(portal.to)) {
				continue;
			}
			if (!this._isObjectActiveInScene(portal.node, scene)) {
				continue;
			}
			activeRooms.get(portal.from).outgoing.push(portal);
		}

		return activeRooms;
	}

	/**
	 * Walk the current tree, bucketing meshes into their current room and
	 * collecting lights regardless of room membership.
	 * @private
	 * @param {Object3D} object - Current subtree.
	 * @param {object|null} roomInfo - The room currently containing the subtree.
	 * @param {object} context - Collection context.
	 */
	_walkScene(object, roomInfo, context) {
		if (!object.visible) {
			return;
		}

		if (object.isMesh) {
			if (roomInfo) {
				roomInfo.meshes.push(object);
			} else {
				context.outside.push(object);
			}
		} else if (object.isLight) {
			context.lights.push(object);
		}

		const nextRoom = roomInfo || context.activeRooms.get(object) || null;
		const children = object.children;
		for (let i = 0; i < children.length; i++) {
			this._walkScene(children[i], nextRoom, context);
		}
	}

	/**
	 * Depth-first propagation of a visible volume. Each directed edge is used
	 * at most once *per path* (terminating cycles), while different paths may
	 * reuse it. Reaching the same room through another path adds another
	 * volume instead of discarding the later window.
	 * @private
	 * @param {object} roomInfo - The room being entered.
	 * @param {Plane[]} planes - Normalized half-spaces of the reaching volume.
	 * @param {Set<object>} usedEdges - Edges already on this propagation path.
	 * @param {object} context - Collection context.
	 */
	_propagate(roomInfo, planes, usedEdges, context) {
		let volumes = context.volumesByRoom.get(roomInfo);
		if (!volumes) {
			volumes = [];
			context.volumesByRoom.set(roomInfo, volumes);
		}
		volumes.push(planes);

		const outgoing = roomInfo.outgoing;
		for (let i = 0; i < outgoing.length; i++) {
			const portal = outgoing[i];
			if (usedEdges.has(portal.handle)) {
				continue;
			}

			const worldPoints = this._portalWorldPoints(portal);
			const clipped = this._clipToVolume(worldPoints, planes);

			if (!_hasNonCollinearTriplet(clipped)) {
				continue;
			}

			// Derive the world normal from the transformed polygon itself so
			// non-uniformly scaled door nodes keep a winding-consistent normal.
			if (!_worldPolygonNormal(worldPoints, _normal)) {
				continue;
			}

			_eye.setFromMatrixPosition(context.camera.worldMatrix);
			const eyeDistance = _normal.dot(_eye) - _normal.dot(clipped[0]);
			// The eye must be strictly on the winding-normal (source) side.
			if (eyeDistance <= EPS) {
				continue;
			}

			const nextPlanes = planes.slice();

			// Side planes through the eye and each edge of the clipped boundary.
			for (let e = 0; e < clipped.length; e++) {
				const a = clipped[e];
				const b = clipped[(e + 1) % clipped.length];
				_edge.subVectors(b, a);
				if (_edge.getLength() <= EPS) {
					continue;
				}
				_toEye.subVectors(_eye, a);
				// cross(toEye, edge) points into the volume: winding edges see
				// the eye on their positive half-space side.
				_cross.crossVectors(_toEye, _edge);
				if (_cross.getLength() <= EPS) {
					continue;
				}
				_cross.normalize();
				nextPlanes.push(new Plane().setFromNormalAndCoplanarPoint(_cross, _eye));
			}

			// Back plane: keep the door's far side, inherited limits preserved.
			_normal.multiplyScalar(-1);
			nextPlanes.push(new Plane().setFromNormalAndCoplanarPoint(_normal, clipped[0]));
			_normal.multiplyScalar(-1);

			const nextUsed = new Set(usedEdges);
			nextUsed.add(portal.handle);
			this._propagate(this._rooms.get(portal.to), nextPlanes, nextUsed, context);
		}
	}

	/**
	 * Transform a portal's copied local polygon into world space.
	 * @private
	 * @param {object} portal - The portal.
	 * @returns {Vector3[]} World-space polygon points.
	 */
	_portalWorldPoints(portal) {
		const matrix = portal.node.worldMatrix;
		const result = [];
		for (let i = 0; i < portal.localPoints.length; i++) {
			result.push(portal.localPoints[i].clone().applyMatrix4(matrix));
		}
		return result;
	}

	/**
	 * Clip a convex world polygon against every half-space of a volume.
	 * The near/far and earlier door limits travel with the volume, so a door
	 * straddling the near plane keeps its clipped portion; no perspective
	 * divide is performed.
	 * @private
	 * @param {Vector3[]} points - World-space convex polygon.
	 * @param {Plane[]} planes - Normalized clipping half-spaces.
	 * @returns {Vector3[]} The clipped convex polygon (may be degenerate).
	 */
	_clipToVolume(points, planes) {
		let current = points;
		for (let i = 0; i < planes.length; i++) {
			if (current.length === 0) {
				break;
			}
			this.portal_clips++;
			current = _clipPolygon(current, planes[i]);
		}
		return current;
	}

	/**
	 * Test one room mesh against all reaching volumes' half-spaces.
	 * @private
	 * @param {Object3D} object - The mesh.
	 * @param {Plane[][]} volumes - All volumes reaching the mesh's room.
	 * @returns {boolean} True when one volume contains the tangent-inclusive sphere.
	 */
	_testRoomMesh(object, volumes) {
		_sphere.copy(object.geometry.boundingSphere).applyMatrix4(object.worldMatrix);

		for (let v = 0; v < volumes.length; v++) {
			this.sphere_tests++;
			const planes = volumes[v];
			let pass = true;
			for (let i = 0; i < planes.length; i++) {
				const distance = planes[i].distanceToPoint(_sphere.center);
				if (distance < -_sphere.radius - EPS) {
					pass = false;
					break;
				}
			}
			if (pass) {
				return true;
			}
		}
		return false;
	}

	/**
	 * Check whether an object is visible and still connected to the given
	 * scene through its ancestor chain.
	 * @private
	 * @param {Object3D} object - The object.
	 * @param {Scene} scene - The scene.
	 * @returns {boolean} True when the whole ancestor chain is visible and ends at `scene`.
	 */
	_isObjectActiveInScene(object, scene) {
		let current = object;
		while (current) {
			if (!current.visible) {
				return false;
			}
			if (current === scene) {
				return true;
			}
			current = current.parent;
		}
		return false;
	}

}

/**
 * Whether `ancestor` contains `object` in its subtree (excluding identity).
 * @private
 * @param {Object3D} ancestor - Potential ancestor.
 * @param {Object3D} object - Potential descendant.
 * @returns {boolean} True when `ancestor` is a strict ancestor.
 */
function _isAncestor(ancestor, object) {
	let current = object.parent;
	while (current) {
		if (current === ancestor) {
			return true;
		}
		current = current.parent;
	}
	return false;
}

/**
 * Finite Vector3 check.
 * @private
 * @param {Vector3} v - The point.
 * @returns {boolean} True when all components are finite.
 */
function _isFinitePoint(v) {
	return Number.isFinite(v.x) && Number.isFinite(v.y) && Number.isFinite(v.z);
}

/**
 * Compute the unit winding normal of a world-space polygon from its first
 * non-collinear edge pair.
 * @private
 * @param {Vector3[]} points - World-space polygon points.
 * @param {Vector3} target - Receives the unit normal.
 * @returns {boolean} False when the polygon is fully collinear.
 */
function _worldPolygonNormal(points, target) {
	const p0 = points[0];
	for (let i = 1; i < points.length - 1; i++) {
		_dir.subVectors(points[i], p0);
		_w.subVectors(points[i + 1], p0);
		target.crossVectors(_dir, _w);
		if (target.getLength() > EPS) {
			target.normalize();
			return true;
		}
	}
	return false;
}

/**
 * Compute the winding normal of a polygon's first non-collinear triple.
 * @private
 * @param {Vector3[]} points - Polygon points.
 * @returns {Vector3|null} Unit normal, or null when fully collinear.
 */
function _polygonNormal(points) {
	const p0 = points[0];
	for (let i = 1; i < points.length - 1; i++) {
		_dir.subVectors(points[i], p0);
		_w.subVectors(points[i + 1], p0);
		_cross.crossVectors(_dir, _w);
		if (_cross.getLength() > EPS) {
			return _cross.normalize().clone();
		}
	}
	return null;
}

/**
 * Coplanarity check against the plane through the first point with `normal`.
 * @private
 * @param {Vector3[]} points - Polygon points.
 * @param {Vector3} normal - Unit polygon normal.
 * @returns {boolean} True when all points are within EPS of the plane.
 */
function _isCoplanar(points, normal) {
	for (let i = 1; i < points.length; i++) {
		_w.subVectors(points[i], points[0]);
		if (Math.abs(normal.dot(_w)) > EPS) {
			return false;
		}
	}
	return true;
}

/**
 * Convexity check: every edge turn must point to the same side as the
 * winding normal.
 * @private
 * @param {Vector3[]} points - Polygon points.
 * @param {Vector3} normal - Unit polygon normal.
 * @returns {boolean} True when the polygon is convex.
 */
function _isConvex(points, normal) {
	const count = points.length;
	for (let i = 0; i < count; i++) {
		_dir.subVectors(points[(i + 1) % count], points[i]);
		_w.subVectors(points[(i + 2) % count], points[(i + 1) % count]);
		_cross.crossVectors(_dir, _w);
		if (_cross.getLength() <= EPS) {
			continue; // collinear consecutive edges are allowed
		}
		if (_cross.dot(normal) < -EPS) {
			return false;
		}
	}
	return true;
}

/**
 * Sutherland-Hodgman clipping of a convex polygon against one half-space.
 * The interior satisfies `n·p + d >= -EPS`.
 * @private
 * @param {Vector3[]} points - Input convex polygon.
 * @param {Plane} plane - Clipping plane.
 * @returns {Vector3[]} Clipped polygon.
 */
function _clipPolygon(points, plane) {
	const result = [];
	const count = points.length;

	let previous = points[count - 1];
	let previousDistance = plane.distanceToPoint(previous);

	for (let i = 0; i < count; i++) {
		const current = points[i];
		const currentDistance = plane.distanceToPoint(current);
		const currentInside = currentDistance >= -EPS;
		const previousInside = previousDistance >= -EPS;

		if (currentInside !== previousInside) {
			let t = previousDistance / (previousDistance - currentDistance);
			if (!Number.isFinite(t) || t < 0) {
				t = 0;
			} else if (t > 1) {
				t = 1;
			}
			const intersection = previous.clone().lerp(current, t);
			result.push(intersection);
		}

		if (currentInside) {
			result.push(current.clone());
		}

		previous = current;
		previousDistance = currentDistance;
	}

	return _removeDuplicatePoints(result);
}

/**
 * Drop cyclic duplicate points produced on clipping boundaries.
 * @private
 * @param {Vector3[]} points - Polygon points.
 * @returns {Vector3[]} De-duplicated points.
 */
function _removeDuplicatePoints(points) {
	if (points.length < 2) {
		return points;
	}
	const result = [points[0]];
	for (let i = 1; i < points.length; i++) {
		if (points[i].distanceTo(result[result.length - 1]) > EPS) {
			result.push(points[i]);
		}
	}
	if (result.length > 1 && result[result.length - 1].distanceTo(result[0]) <= EPS) {
		result.pop();
	}
	return result;
}

/**
 * Whether the polygon contains three non-collinear points, measured as a
 * perpendicular world distance larger than EPS.
 * @private
 * @param {Vector3[]} points - Polygon points.
 * @returns {boolean} True when a non-collinear triplet exists.
 */
function _hasNonCollinearTriplet(points) {
	if (points.length < 3) {
		return false;
	}
	const p0 = points[0];
	let p1 = null;
	for (let i = 1; i < points.length; i++) {
		if (points[i].distanceTo(p0) > EPS) {
			p1 = points[i];
			break;
		}
	}
	if (!p1) {
		return false;
	}
	_dir.subVectors(p1, p0).normalize();
	for (let i = 2; i < points.length; i++) {
		_w.subVectors(points[i], p0);
		const along = _w.dot(_dir);
		_cross.copy(_w).addScaledVector(_dir, -along);
		if (_cross.getLength() > EPS) {
			return true;
		}
	}
	return false;
}

export { PortalRenderCollector };
