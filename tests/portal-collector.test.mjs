import { Scene } from '../src/scenes/Scene.js';
import { Object3D } from '../src/scenes/Object3D.js';
import { Camera } from '../src/scenes/Camera.js';
import { Mesh } from '../src/scenes/Mesh.js';
import { SkinnedMesh } from '../src/scenes/SkinnedMesh.js';
import { Bone } from '../src/scenes/Bone.js';
import { Skeleton } from '../src/resources/Skeleton.js';
import { Geometry } from '../src/resources/geometries/Geometry.js';
import { BasicMaterial } from '../src/resources/materials/BasicMaterial.js';
import { AmbientLight } from '../src/scenes/lights/AmbientLight.js';
import { Matrix4 } from '../src/math/Matrix4.js';
import { Vector3 } from '../src/math/Vector3.js';
import { PortalRenderCollector } from '../src/render/PortalRenderCollector.js';

/*
 * PortalRenderCollector test cases.
 *
 * Three groups are exported:
 *   geometry    - polygon/plane construction, validation and tolerances
 *   membership  - programmatic room graphs checked with an independent
 *                 ray-vs-portal and per-plane scalar reference
 *   integration - real Scene entry points, queues, layers, skeletons, lights
 *
 * Only Node built-in modules are used.
 */

const EPS = 1e-7;

// ---------------------------------------------------------------------------
// Shared builders
// ---------------------------------------------------------------------------

function makeCamera(pos, target = new Vector3(0, 0, -5), near = 1, far = 100, fov = Math.PI / 3) {
	const camera = new Camera();
	camera.setPerspective(fov, 1, near, far);
	camera.position.copy(pos);
	camera.lookAt(target, new Vector3(0, 1, 0));
	return camera;
}

function makeGeometry(radius = 0.2, grouped = false) {
	const geometry = new Geometry();
	geometry.boundingSphere.center.set(0, 0, 0);
	geometry.boundingSphere.radius = radius;
	if (grouped) {
		geometry.addGroup(0, 3, 0);
		geometry.addGroup(3, 3, 1);
	}
	return geometry;
}

function makeBall(center, radius = 0.1, material) {
	const mesh = new Mesh(makeGeometry(radius), material || new BasicMaterial());
	mesh.position.copy(center);
	return mesh;
}

function installCollector(scene) {
	const collector = new PortalRenderCollector();
	scene.collector = collector;
	return collector;
}

function collect(scene, camera, collectLights = true, updateSkeletons = true) {
	scene.updateMatrix();
	scene.updateRenderStates(camera);
	return scene.updateRenderQueue(camera, collectLights, updateSkeletons);
}

function renderables(queue) {
	return queue.layerList.flatMap(layer => [...layer.opaque, ...layer.transparent]);
}

function uniqueObjects(queue) {
	return [...new Set(renderables(queue).map(renderable => renderable.object))];
}

function hasObject(queue, object) {
	return uniqueObjects(queue).includes(object);
}

// Local rectangle in the XY plane, CCW, winding normal +Z.
function rectXY(halfW, halfH) {
	return [
		new Vector3(-halfW, -halfH, 0),
		new Vector3(halfW, -halfH, 0),
		new Vector3(halfW, halfH, 0),
		new Vector3(-halfW, halfH, 0)
	];
}

function transformPoint(point, matrix) {
	return point.clone().applyMatrix4(matrix);
}

// Independent world-space polygon for a rectXY door node.
function worldRect(node, halfW, halfH) {
	return rectXY(halfW, halfH).map(point => transformPoint(point, node.worldMatrix));
}

function polygonNormal(points) {
	const normal = new Vector3()
		.crossVectors(points[1].clone().sub(points[0]), points[2].clone().sub(points[1]));
	return normal.normalize();
}

function pointInConvexPolygon(point, points, normal) {
	for (let i = 0; i < points.length; i++) {
		const edge = points[(i + 1) % points.length].clone().sub(points[i]);
		const toPoint = point.clone().sub(points[i]);
		if (new Vector3().crossVectors(edge, toPoint).dot(normal) < -EPS) {
			return false;
		}
	}
	return true;
}

// Independent ray cast: ray from eye to ball center must cross every portal
// polygon, in chain order.
function rayPassesPortals(eye, center, polygons) {
	const direction = center.clone().sub(eye);
	for (const polygon of polygons) {
		const normal = polygonNormal(polygon);
		const denom = normal.dot(direction);
		if (Math.abs(denom) < 1e-12) {
			return false;
		}
		const t = polygon[0].clone().sub(eye).dot(normal) / denom;
		if (!(t > 0 && t <= 1)) {
			return false;
		}
		const hit = eye.clone().addScaledVector(direction, t);
		if (!pointInConvexPolygon(hit, polygon, normal)) {
			return false;
		}
	}
	return true;
}

// Independent half-space construction from eye + portal boundary, matching
// the collector's side/back planes, expressed as plain { n, d } scalars.
function buildReferencePlanes(eye, polygons) {
	const planes = [];
	for (const polygon of polygons) {
		const normal = polygonNormal(polygon);
		for (let i = 0; i < polygon.length; i++) {
			const a = polygon[i];
			const b = polygon[(i + 1) % polygon.length];
			const edge = b.clone().sub(a);
			const toEye = eye.clone().sub(a);
			const sideNormal = new Vector3().crossVectors(toEye, edge).normalize();
			planes.push({ n: sideNormal, d: -sideNormal.dot(eye) });
		}
		const backNormal = normal.clone().multiplyScalar(-1);
		planes.push({ n: backNormal, d: -backNormal.dot(polygon[0]) });
	}
	return planes;
}

function spherePassesPlanes(center, radius, planes) {
	for (let i = 0; i < planes.length; i++) {
		if (planes[i].n.dot(center) + planes[i].d < -radius - EPS) {
			return false;
		}
	}
	return true;
}

function frustumScalarPlanes(camera) {
	return camera.frustum.planes.map(plane => ({ n: plane.normal.clone(), d: plane.constant }));
}

// Independent Sutherland-Hodgman clipping written with plain scalars, used to
// derive the clipped portal boundary that the collector must also use.
function clipPolygonScalar(points, planes, tolerance = EPS) {
	let current = points.map(point => point.clone());
	for (const plane of planes) {
		const next = [];
		for (let i = 0; i < current.length; i++) {
			const a = current[(i + current.length - 1) % current.length];
			const b = current[i];
			const da = plane.n.dot(a) + plane.d;
			const db = plane.n.dot(b) + plane.d;
			const aInside = da >= -tolerance;
			const bInside = db >= -tolerance;
			if (aInside !== bInside) {
				let t = da / (da - db);
				if (!Number.isFinite(t) || t < 0) {
					t = 0;
				} else if (t > 1) {
					t = 1;
				}
				next.push(a.clone().lerp(b, t));
			}
			if (bInside) {
				next.push(b.clone());
			}
		}
		current = next;
		if (current.length === 0) {
			break;
		}
	}
	return current;
}

// Programmatic forked graph builder: rooms are named Object3D roots, portals
// are rectXY door nodes placed in the world.
function buildGraph(scene, collector, spec) {
	const rooms = {};
	for (const name of spec.rooms) {
		const root = new Object3D();
		root.name = name;
		scene.add(root);
		collector.addRoom(root);
		rooms[name] = root;
	}
	const doors = [];
	for (const portalSpec of spec.portals) {
		const node = new Object3D();
		node.position.copy(portalSpec.pos);
		if (portalSpec.rotY) {
			node.euler.y = portalSpec.rotY;
		}
		scene.add(node);
		const handle = collector.addPortal(
			rooms[portalSpec.from], rooms[portalSpec.to], node,
			rectXY(portalSpec.halfW, portalSpec.halfH)
		);
		doors.push({ spec: portalSpec, node: node, handle: handle });
	}
	return { rooms: rooms, doors: doors };
}

function eyePosition(camera) {
	return new Vector3().setFromMatrixPosition(camera.worldMatrix);
}

// Reference verdict for one ball: ray through every chain OR full scalar
// half-space test through every chain, intersected with the camera frustum.
function referenceVisible(camera, chains, ball) {
	const eye = eyePosition(camera);
	const center = new Vector3().setFromMatrixPosition(ball.worldMatrix);
	const radius = ball.geometry.boundingSphere.radius;
	const frustumPlanes = frustumScalarPlanes(camera);
	if (!spherePassesPlanes(center, radius, frustumPlanes)) {
		return { scalar: false, ray: false };
	}
	let scalar = false;
	let ray = false;
	for (const chain of chains) {
		if (spherePassesPlanes(center, radius, buildReferencePlanes(eye, chain))) {
			scalar = true;
		}
		if (rayPassesPortals(eye, center, chain)) {
			ray = true;
		}
	}
	return { scalar: scalar, ray: ray };
}

// ---------------------------------------------------------------------------
// geometry group
// ---------------------------------------------------------------------------

const geometryCases = [
	{
		group: 'geometry',
		name: 'room and portal validation rejects bad input',
		run(check) {
			const collector = new PortalRenderCollector();
			const scene = new Scene();
			const room = new Object3D();
			scene.add(room);
			collector.addRoom(room);

			check(throws(() => collector.addRoom(null)));
			check(throws(() => collector.addRoom(scene)));
			check(throws(() => collector.addRoom(room)));
			const nested = new Object3D();
			room.add(nested);
			check(throws(() => collector.addRoom(nested)));
			const outer = new Object3D();
			scene.add(outer);
			const child = new Object3D();
			outer.add(child);
			collector.addRoom(outer);
			check(throws(() => collector.addRoom(child)));
			check(throws(() => collector.removeRoom(new Object3D())));

			const other = new Object3D();
			scene.add(other);
			collector.addRoom(other);
			const node = new Object3D();
			scene.add(node);
			check(throws(() => collector.addPortal(room, room, node, rectXY(1, 1))));
			check(throws(() => collector.addPortal(room, new Object3D(), node, rectXY(1, 1))));
			check(throws(() => collector.addPortal(room, other, null, rectXY(1, 1))));
			check(throws(() => collector.addPortal(room, other, node, [new Vector3(), new Vector3(1, 0, 0)])));
			check(throws(() => collector.addPortal(room, other, node,
				[new Vector3(0, 0, 0), new Vector3(1, 0, 0), new Vector3(Number.NaN, 1, 0)])));
			// zero area
			check(throws(() => collector.addPortal(room, other, node,
				[new Vector3(0, 0, 0), new Vector3(1, 0, 0), new Vector3(2, 0, 0), new Vector3(0, 0, 0)])));
			// non coplanar
			check(throws(() => collector.addPortal(room, other, node,
				[new Vector3(0, 0, 0), new Vector3(1, 0, 0), new Vector3(1, 1, 0.2), new Vector3(0, 1, 0)])));
			// non convex / wrong winding
			check(throws(() => collector.addPortal(room, other, node,
				[new Vector3(0, 0, 0), new Vector3(1, 1, 0), new Vector3(1, 0, 0), new Vector3(0, 1, 0)])));

			const handle = collector.addPortal(room, other, node, rectXY(1, 1));
			check(throws(() => collector.removePortal(handle + 1)));
			collector.removePortal(handle);
			check(throws(() => collector.removePortal(handle)));

			const camera = new Camera();
			check(throws(() => collector.setCameraRoom(null, room)));
			check(throws(() => collector.setCameraRoom(camera, new Object3D())));
			collector.setCameraRoom(camera, room);
			collector.setCameraRoom(camera, null);
			check(true);
		}
	},
	{
		group: 'geometry',
		name: 'portal vertices are copied and input array is not retained',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const roomA = new Object3D();
			const roomB = new Object3D();
			scene.add(roomA); scene.add(roomB);
			collector.addRoom(roomA); collector.addRoom(roomB);
			const door = new Object3D();
			door.position.set(0, 0, -4);
			scene.add(door);
			const vertices = rectXY(1, 1.5);
			collector.addPortal(roomA, roomB, door, vertices);

			// Mutate everything the caller owns after registration.
			vertices.length = 0;
			door.updateMatrix();
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, roomA);
			const ball = makeBall(new Vector3(0, 0, -6), 0.1);
			roomB.add(ball);
			const queue = collect(scene, camera);

			check(hasObject(queue, ball));
			check(collector.portal_clips > 0);
		}
	},
	{
		group: 'geometry',
		name: 'eye on or behind the portal plane does not traverse',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const roomA = new Object3D();
			const roomB = new Object3D();
			scene.add(roomA); scene.add(roomB);
			collector.addRoom(roomA); collector.addRoom(roomB);
			const door = new Object3D();
			door.position.set(0, 0, -4);
			scene.add(door);
			collector.addPortal(roomA, roomB, door, rectXY(1, 1.5));

			const ball = makeBall(new Vector3(0, 0, -6), 0.1);
			roomB.add(ball);

			// Eye sits exactly on the door plane: distance 0, below the 1e-7 limit.
			const camera = makeCamera(new Vector3(0, 0, -4));
			scene.add(camera);
			collector.setCameraRoom(camera, roomA);
			const queue = collect(scene, camera);
			check(!hasObject(queue, ball));

			// Eye behind the door (negative winding side) also fails.
			camera.position.set(0, 0, -4.5);
			const queueBehind = collect(scene, camera);
			check(!hasObject(queueBehind, ball));

			// Back in front of the door again, propagation resumes.
			camera.position.set(0, 0, -0.5);
			const queueFront = collect(scene, camera);
			check(hasObject(queueFront, ball));
		}
	},
	{
		group: 'geometry',
		name: 'tangent sphere is kept by the half-space test',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const roomA = new Object3D();
			const roomB = new Object3D();
			scene.add(roomA); scene.add(roomB);
			collector.addRoom(roomA); collector.addRoom(roomB);
			const door = new Object3D();
			door.position.set(0, 0, -4);
			scene.add(door);
			const halfW = 1;
			collector.addPortal(roomA, roomB, door, rectXY(halfW, 1.5));

			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, roomA);
			scene.updateMatrix();

			// Independently build the side planes and locate the right wall:
			// it is the side plane whose normal points toward -x.
			const eye = eyePosition(camera);
			const polygon = worldRect(door, halfW, 1.5);
			const planes = buildReferencePlanes(eye, [polygon]);
			const sidePlanes = planes.slice(0, polygon.length);
			const rightSide = sidePlanes.reduce((best, plane) =>
				plane.n.x < best.n.x ? plane : best);
			const radius = 0.1;
			// Scan x at fixed depth, find the boundary where margin == 0.
			let tangentX = 0;
			let previous = null;
			for (let x = 0; x <= 3; x += 0.005) {
				const center = new Vector3(x, 0, -6);
				const margin = rightSide.n.dot(center) + rightSide.d + radius;
				if (previous && previous.margin >= 0 && margin < 0) {
					tangentX = previous.x;
					break;
				}
				previous = { x: x, margin: margin };
			}
			check(tangentX > 0);
			const tangentBall = makeBall(new Vector3(tangentX, 0, -6), radius);
			roomB.add(tangentBall);
			const queue = collect(scene, camera);
			const center = new Vector3(tangentX, 0, -6);
			const margin = rightSide.n.dot(center) + rightSide.d + radius;
			check(Math.abs(margin) < 0.02);
			check(hasObject(queue, tangentBall));

			// Clearly outside the side plane is rejected.
			const outsideBall = makeBall(new Vector3(tangentX + 0.6, 0, -6), radius);
			roomB.add(outsideBall);
			const queue2 = collect(scene, camera);
			check(!hasObject(queue2, outsideBall));
		}
	},
	{
		group: 'geometry',
		name: 'clipping counts are exposed and reset on every collection',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const roomA = new Object3D();
			const roomB = new Object3D();
			scene.add(roomA); scene.add(roomB);
			collector.addRoom(roomA); collector.addRoom(roomB);
			const door = new Object3D();
			door.position.set(0, 0, -4);
			scene.add(door);
			collector.addPortal(roomA, roomB, door, rectXY(1, 1.5));
			roomB.add(makeBall(new Vector3(0, 0, -6), 0.1));

			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, roomA);
			const queue = collect(scene, camera);
			// Six frustum planes clip the one portal polygon, plus sphere tests.
			check(collector.portal_clips >= 6);
			check(collector.sphere_tests >= 1);
			check(uniqueObjects(queue).length === 1);

			// Ordinary collection resets both counters.
			collector.setCameraRoom(camera, null);
			collect(scene, camera);
			check(collector.portal_clips === 0);
			check(collector.sphere_tests === 0);
		}
	}
];

// ---------------------------------------------------------------------------
// membership group
// ---------------------------------------------------------------------------

function buildForkedScene() {
	const scene = new Scene();
	const collector = installCollector(scene);
	const graph = buildGraph(scene, collector, {
		rooms: ['A', 'B', 'C', 'D'],
		portals: [
			{ from: 'A', to: 'B', pos: new Vector3(-1.2, 0, -4), halfW: 1, halfH: 1.5 },
			{ from: 'A', to: 'C', pos: new Vector3(1.2, 0, -4), halfW: 1, halfH: 1.5 },
			{ from: 'B', to: 'D', pos: new Vector3(-2, 0, -8), halfW: 1.8, halfH: 1.8 },
			{ from: 'C', to: 'D', pos: new Vector3(2, 0, -8), halfW: 1.8, halfH: 1.8 }
		]
	});
	return { scene: scene, collector: collector, graph: graph };
}

const membershipCases = [
	{
		group: 'membership',
		name: 'forked graph: two disjoint paths merge into one room',
		run(check) {
			const { scene, collector, graph } = buildForkedScene();
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, graph.rooms.A);

			const balls = {
				bVisible: makeBall(new Vector3(-1.2, 0, -6), 0.05),
				cVisible: makeBall(new Vector3(1.2, 0, -6), 0.05),
				bGap: makeBall(new Vector3(-0.18, 0, -6), 0.05),
				cGap: makeBall(new Vector3(0.18, 0, -6), 0.05),
				dLeft: makeBall(new Vector3(-2.5, 0, -10), 0.05),
				dRight: makeBall(new Vector3(2.5, 0, -10), 0.05),
				dGap: makeBall(new Vector3(0, 0, -10), 0.05)
			};
			graph.rooms.B.add(balls.bVisible); graph.rooms.B.add(balls.bGap);
			graph.rooms.C.add(balls.cVisible); graph.rooms.C.add(balls.cGap);
			graph.rooms.D.add(balls.dLeft); graph.rooms.D.add(balls.dRight); graph.rooms.D.add(balls.dGap);

			scene.updateMatrix();
			const doorAB = worldRect(graph.doors[0].node, 1, 1.5);
			const doorAC = worldRect(graph.doors[1].node, 1, 1.5);
			const doorBD = worldRect(graph.doors[2].node, 1.8, 1.8);
			const doorCD = worldRect(graph.doors[3].node, 1.8, 1.8);
			const chains = {
				bVisible: [doorAB],
				cVisible: [doorAC],
				bGap: [doorAB],
				cGap: [doorAC],
				dLeft: [doorAB, doorBD],
				dRight: [doorAC, doorCD],
				dGap: [doorAB, doorBD, doorAC, doorCD]
			};
			const expected = {};
			for (const name of Object.keys(balls)) {
				const options = name === 'dGap'
					? [[doorAB, doorBD], [doorAC, doorCD]]
					: [chains[name]];
				expected[name] = referenceVisible(camera, options, balls[name]);
			}

			const queue = collect(scene, camera);
			for (const name of Object.keys(balls)) {
				const actual = hasObject(queue, balls[name]);
				// Collector must agree with both independent references.
				check(actual === expected[name].scalar, `${name} scalar reference`);
				check(actual === expected[name].ray, `${name} ray reference`);
			}
			check(hasObject(queue, balls.dLeft));
			check(hasObject(queue, balls.dRight));
			check(!hasObject(queue, balls.dGap));
			check(!hasObject(queue, balls.bGap));
			check(collector.portal_clips > 0);
			check(collector.sphere_tests >= 7);
		}
	},
	{
		group: 'membership',
		name: 'cycles terminate: each directed edge used at most once per path',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const graph = buildGraph(scene, collector, {
				rooms: ['A', 'B'],
				portals: [
					{ from: 'A', to: 'B', pos: new Vector3(0, 0, -4), halfW: 1.2, halfH: 1.5 },
					{ from: 'B', to: 'A', pos: new Vector3(0, 0, -4), halfW: 1.2, halfH: 1.5 }
				]
			});
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, graph.rooms.A);
			const ballB = makeBall(new Vector3(0, 0, -6), 0.1);
			const ballA = makeBall(new Vector3(0, 0, -2), 0.1);
			graph.rooms.B.add(ballB);
			graph.rooms.A.add(ballA);

			const queue = collect(scene, camera);
			check(hasObject(queue, ballB));
			check(hasObject(queue, ballA));
			// Propagation returns instead of looping forever, and work was done.
			check(collector.portal_clips > 0);
			check(collector.sphere_tests >= 2);
		}
	},
	{
		group: 'membership',
		name: 'door straddling the near plane keeps its clipped portion',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const near = 3;
			const graph = buildGraph(scene, collector, {
				rooms: ['A', 'B'],
				portals: [
					{ from: 'A', to: 'B', pos: new Vector3(0, 0, -3.5), rotY: Math.PI / 4, halfW: 1.5, halfH: 1.5 }
				]
			});
			const camera = makeCamera(new Vector3(0, 0, -0.5), new Vector3(0, 0, -5), near, 100);
			scene.add(camera);
			collector.setCameraRoom(camera, graph.rooms.A);

			scene.updateMatrix();
			const polygon = worldRect(graph.doors[0].node, 1.5, 1.5);
			// Independent confirmation: polygon really straddles the near plane
			// (in this engine frustum planes[5] is the near plane).
			const nearPlane = camera.frustum.planes[5];
			const distances = polygon.map(point => nearPlane.distanceToPoint(point));
			check(distances.some(d => d < 0) && distances.some(d => d >= 0));

			const doorCenter = polygon.reduce((acc, point) => acc.add(point), new Vector3())
				.multiplyScalar(1 / polygon.length);
			const hiddenBall = makeBall(new Vector3(3.5, 0, -2), 0.05);

			// Scalar reference clips the door by the frustum first and builds
			// the side/back half-spaces from the clipped boundary.
			const clipped = clipPolygonScalar(polygon, frustumScalarPlanes(camera));
			check(clipped.length >= 3);
			const eye = eyePosition(camera);
			const clippedPlanes = buildReferencePlanes(eye, [clipped]);

			// Pick a point straight behind the door center that passes both the
			// independent ray test and the clipped scalar test.
			let visiblePosition = null;
			for (let depth = 1; depth <= 4; depth++) {
				const candidate = doorCenter.clone();
				candidate.z -= depth;
				if (spherePassesPlanes(candidate, 0.05, clippedPlanes) &&
					rayPassesPortals(eye, candidate, [polygon])) {
					visiblePosition = candidate;
					break;
				}
			}
			check(visiblePosition !== null);
			const visibleBall = makeBall(visiblePosition, 0.05);
			graph.rooms.B.add(visibleBall);
			graph.rooms.B.add(hiddenBall);
			scene.updateMatrix();

			const visibleCenter = new Vector3().setFromMatrixPosition(visibleBall.worldMatrix);
			check(spherePassesPlanes(visibleCenter, 0.05, clippedPlanes));
			check(rayPassesPortals(eye, visibleCenter, [polygon]));
			const queue = collect(scene, camera);
			check(hasObject(queue, visibleBall));
			check(!hasObject(queue, hiddenBall));
			// Clipping happened against the near plane among the frustum planes.
			check(collector.portal_clips >= 6);
		}
	},
	{
		group: 'membership',
		name: 'slanted door with arbitrary rotation is traversed',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const graph = buildGraph(scene, collector, {
				rooms: ['A', 'B'],
				portals: [
					{ from: 'A', to: 'B', pos: new Vector3(0.4, 0, -4), rotY: 0.61, halfW: 1.3, halfH: 1.6 }
				]
			});
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, graph.rooms.A);
			scene.updateMatrix();
			const polygon = worldRect(graph.doors[0].node, 1.3, 1.6);

			const candidates = [];
			for (let i = 0; i < 9; i++) {
				const ball = makeBall(new Vector3(-2 + i * 0.5, 0, -7), 0.05);
				graph.rooms.B.add(ball);
				candidates.push(ball);
			}
			const queue = collect(scene, camera);
			let matched = 0;
			for (const ball of candidates) {
				const reference = referenceVisible(camera, [[polygon]], ball);
				check(reference.scalar === reference.ray);
				if (hasObject(queue, ball)) {
					check(reference.scalar);
					matched++;
				} else {
					check(!reference.scalar);
				}
			}
			check(matched > 0 && matched < candidates.length);
		}
	},
	{
		group: 'membership',
		name: 'object in the gap outside the door stays on the ordinary side',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const graph = buildGraph(scene, collector, {
				rooms: ['A', 'B'],
				portals: [
					{ from: 'A', to: 'B', pos: new Vector3(0, 0, -4), halfW: 0.8, halfH: 1.5 }
				]
			});
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, graph.rooms.A);
			scene.updateMatrix();
			const polygon = worldRect(graph.doors[0].node, 0.8, 1.5);

			const inside = makeBall(new Vector3(0, 0, -6), 0.05);
			const gap = makeBall(new Vector3(2.2, 0, -6), 0.05);
			graph.rooms.B.add(inside);
			graph.rooms.B.add(gap);

			const queue = collect(scene, camera);
			check(hasObject(queue, inside));
			check(!hasObject(queue, gap));
			const gapReference = referenceVisible(camera, [[polygon]], gap);
			check(!gapReference.scalar && !gapReference.ray);

			// The same ball placed outside any room uses the ordinary path and
			// is governed only by the camera frustum.
			graph.rooms.B.remove(gap);
			scene.add(gap);
			const queueOrdinary = collect(scene, camera);
			check(hasObject(queueOrdinary, gap) === camera.frustum.intersectsSphere(
				makeSphereOf(gap)
			));
		}
	},
	{
		group: 'membership',
		name: 'unreachable rooms never expose geometry bounding spheres',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			const roomA = new Object3D();
			scene.add(roomA);
			collector.addRoom(roomA);
			collector.setCameraRoom(camera, roomA);

			let reads = 0;
			const realSphere = makeGeometry(0.1).boundingSphere;
			const guardedGeometry = new Geometry();
			Object.defineProperty(guardedGeometry, 'boundingSphere', {
				configurable: true,
				get() {
					reads++;
					return realSphere;
				}
			});
			const material = new BasicMaterial();
			const unreachableRooms = [];
			for (let i = 0; i < 1000; i++) {
				const room = new Object3D();
				scene.add(room);
				collector.addRoom(room);
				const mesh = new Mesh(guardedGeometry, material);
				mesh.position.set((i % 40) * 0.5 - 10, 0, -6 - Math.floor(i / 40) * 0.5);
				room.add(mesh);
				unreachableRooms.push(room);
			}
			const reachable = new Mesh(makeGeometry(0.1), material);
			reachable.position.set(0, 0, -2);
			roomA.add(reachable);

			const queue = collect(scene, camera);
			check(reads === 0);
			check(hasObject(queue, reachable));
			check(uniqueObjects(queue).length === 1);
			check(unreachableRooms.length === 1000);
		}
	},
	{
		group: 'membership',
		name: 'frustumCulled false does not bypass room connectivity',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const graph = buildGraph(scene, collector, {
				rooms: ['A', 'B'],
				portals: [
					{ from: 'A', to: 'B', pos: new Vector3(0, 0, -4), halfW: 0.6, halfH: 1.2 }
				]
			});
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, graph.rooms.A);

			const locked = makeBall(new Vector3(3, 0, -6), 0.1);
			locked.frustumCulled = false;
			graph.rooms.B.add(locked);
			const queue = collect(scene, camera);
			check(!hasObject(queue, locked));

			// Outside rooms, frustumCulled=false keeps the original frustum bypass.
			const outside = makeBall(new Vector3(50, 50, -50), 0.1);
			outside.frustumCulled = false;
			scene.add(outside);
			const queue2 = collect(scene, camera);
			check(hasObject(queue2, outside));
		}
	}
];

function makeSphereOf(mesh) {
	const sphere = mesh.geometry.boundingSphere.clone();
	sphere.applyMatrix4(mesh.worldMatrix);
	return sphere;
}

function throws(fn) {
	try {
		fn();
		return false;
	} catch (error) {
		return error instanceof Error;
	}
}

// ---------------------------------------------------------------------------
// integration group
// ---------------------------------------------------------------------------

const integrationCases = [
	{
		group: 'integration',
		name: 'reparenting, hidden rooms and detach reattach take effect next collection',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const graph = buildGraph(scene, collector, {
				rooms: ['A', 'B'],
				portals: [
					{ from: 'A', to: 'B', pos: new Vector3(0, 0, -4), halfW: 1, halfH: 1.5 }
				]
			});
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, graph.rooms.A);
			const ball = makeBall(new Vector3(0, 0, -6), 0.1);
			graph.rooms.B.add(ball);

			let queue = collect(scene, camera);
			check(hasObject(queue, ball));

			// Reparent across rooms: membership follows the current ancestor chain.
			graph.rooms.B.remove(ball);
			graph.rooms.A.add(ball);
			queue = collect(scene, camera);
			check(hasObject(queue, ball));
			graph.rooms.A.remove(ball);
			graph.rooms.B.add(ball);
			queue = collect(scene, camera);
			check(hasObject(queue, ball));

			// Hidden room drops its objects and deactivates edges.
			graph.rooms.B.visible = false;
			queue = collect(scene, camera);
			check(!hasObject(queue, ball));
			graph.rooms.B.visible = true;
			queue = collect(scene, camera);
			check(hasObject(queue, ball));

			// Detach and reattach.
			scene.remove(graph.rooms.B);
			queue = collect(scene, camera);
			check(!hasObject(queue, ball));
			scene.add(graph.rooms.B);
			queue = collect(scene, camera);
			check(hasObject(queue, ball));

			// Hidden door node deactivates the edge.
			graph.doors[0].node.visible = false;
			queue = collect(scene, camera);
			check(!hasObject(queue, ball));
			graph.doors[0].node.visible = true;
			queue = collect(scene, camera);
			check(hasObject(queue, ball));

			// Moving the door node uses its latest worldMatrix.
			graph.doors[0].node.position.set(3, 0, -4);
			queue = collect(scene, camera);
			check(!hasObject(queue, ball));
			graph.doors[0].node.position.set(0, 0, -4);
			queue = collect(scene, camera);
			check(hasObject(queue, ball));
		}
	},
	{
		group: 'integration',
		name: 'removed room drops edges and errors until the camera is reassigned',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const roomA = new Object3D();
			const roomB = new Object3D();
			scene.add(roomA); scene.add(roomB);
			collector.addRoom(roomA); collector.addRoom(roomB);
			const door = new Object3D();
			door.position.set(0, 0, -4);
			scene.add(door);
			const handle = collector.addPortal(roomA, roomB, door, rectXY(1, 1.5));
			const camA = makeCamera(new Vector3(0, 0, -0.5));
			const camB = makeCamera(new Vector3(0, 0, -6));
			scene.add(camA); scene.add(camB);
			collector.setCameraRoom(camA, roomA);
			collector.setCameraRoom(camB, roomB);
			const ballA = makeBall(new Vector3(0, 0, -2), 0.1);
			const ballB = makeBall(new Vector3(0, 0, -6), 0.1);
			roomA.add(ballA);
			roomB.add(ballB);

			check(hasObject(collect(scene, camA), ballB));

			// With the room registered but its edge removed, the room is unreachable.
			collector.removePortal(handle);
			check(!hasObject(collect(scene, camA), ballB));
			const reopened = new Object3D();
			reopened.position.set(0, 0, -4);
			scene.add(reopened);
			collector.addPortal(roomA, roomB, reopened, rectXY(1, 1.5));
			check(hasObject(collect(scene, camA), ballB));

			collector.removeRoom(roomB);
			// The room's incident edge was removed together with the room.
			check(throws(() => collector.removePortal(handle)));

			// A camera still assigned to the removed room fails explicitly.
			check(throws(() => collect(scene, camB)));
			// The other camera keeps collecting; its own room is unaffected.
			check(hasObject(collect(scene, camA), ballA));
			// The unregistered subtree is now "outside rooms" and uses the ordinary path.
			check(hasObject(collect(scene, camA), ballB));

			// Re-registering the same root does not silently revive the assignment.
			collector.addRoom(roomB);
			check(throws(() => collect(scene, camB)));

			// Reassign and rebuild the edge, then collection continues.
			collector.setCameraRoom(camB, roomB);
			const newDoor = new Object3D();
			newDoor.position.set(0, 0, -4);
			scene.add(newDoor);
			collector.addPortal(roomA, roomB, newDoor, rectXY(1, 1.5));
			check(hasObject(collect(scene, camA), ballB));

			// Clearing an assignment falls back to ordinary collection.
			collector.setCameraRoom(camA, null);
			const ordinary = collect(scene, camA);
			check(ordinary === scene.getRenderQueue(camA));
		}
	},
	{
		group: 'integration',
		name: 'two cameras keep separate rooms, queues and windows',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const graph = buildGraph(scene, collector, {
				rooms: ['A', 'B', 'C'],
				portals: [
					{ from: 'A', to: 'B', pos: new Vector3(0, 0, -4), halfW: 1, halfH: 1.5 },
					{ from: 'B', to: 'C', pos: new Vector3(0, 0, -8), halfW: 1.4, halfH: 1.6 }
				]
			});
			const camA = makeCamera(new Vector3(0, 0, -0.5));
			const camB = makeCamera(new Vector3(0, 0, -4.5), new Vector3(0, 0, -9));
			scene.add(camA); scene.add(camB);
			collector.setCameraRoom(camA, graph.rooms.A);
			collector.setCameraRoom(camB, graph.rooms.B);

			const ballA = makeBall(new Vector3(0, 0, -2), 0.1);
			const ballB = makeBall(new Vector3(0, 0, -6), 0.1);
			const ballC = makeBall(new Vector3(0, 0, -10), 0.1);
			graph.rooms.A.add(ballA);
			graph.rooms.B.add(ballB);
			graph.rooms.C.add(ballC);

			const queueA = collect(scene, camA);
			const queueB = collect(scene, camB);
			check(queueA !== queueB);
			check(hasObject(queueA, ballA) && hasObject(queueA, ballB) && hasObject(queueA, ballC));
			check(!hasObject(queueB, ballA) && hasObject(queueB, ballB) && hasObject(queueB, ballC));

			// Interleave again: per-camera windows do not leak.
			const queueB2 = collect(scene, camB);
			const queueA2 = collect(scene, camA);
			check(!hasObject(queueB2, ballA));
			check(hasObject(queueA2, ballC));
		}
	},
	{
		group: 'integration',
		name: 'two scenes and collectors never cross rooms or queues',
		run(check) {
			function buildOne(tag) {
				const scene = new Scene();
				const collector = installCollector(scene);
				const graph = buildGraph(scene, collector, {
					rooms: ['A', 'B'],
					portals: [
						{ from: 'A', to: 'B', pos: new Vector3(0, 0, -4), halfW: 1, halfH: 1.5 }
					]
				});
				const camera = makeCamera(new Vector3(0, 0, -0.5));
				scene.add(camera);
				collector.setCameraRoom(camera, graph.rooms.A);
				const ball = makeBall(new Vector3(0, 0, -6), 0.1);
				graph.rooms.B.add(ball);
				return { scene: scene, collector: collector, camera: camera, ball: ball };
			}
			const one = buildOne('one');
			const two = buildOne('two');
			const q1 = collect(one.scene, one.camera);
			const q2 = collect(two.scene, two.camera);
			const q1b = collect(one.scene, one.camera);
			const q2b = collect(two.scene, two.camera);
			check(q1 !== q2 && q1b !== q2b);
			check(hasObject(q1, one.ball) && !hasObject(q1, two.ball));
			check(hasObject(q2, two.ball) && !hasObject(q2, one.ball));
			check(one.collector !== two.collector);
			// Stats belong to the collector that produced them.
			check(one.collector.portal_clips > 0 && two.collector.portal_clips > 0);
		}
	},
	{
		group: 'integration',
		name: 'material groups, render layers and transparent sorting survive the portal path',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const graph = buildGraph(scene, collector, {
				rooms: ['A', 'B'],
				portals: [
					{ from: 'A', to: 'B', pos: new Vector3(0, 0, -4), halfW: 1, halfH: 1.5 }
				]
			});
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, graph.rooms.A);

			const opaque = new BasicMaterial();
			const transparent = new BasicMaterial();
			transparent.transparent = true;
			const grouped = new Mesh(makeGeometry(0.2, true), [opaque, transparent]);
			grouped.position.set(0, 0, -6);
			grouped.renderLayer = 2;
			graph.rooms.B.add(grouped);

			const queue = collect(scene, camera);
			check(hasObject(queue, grouped));
			// One push only, even with two material groups.
			check(uniqueObjects(queue).length === 1);
			check(renderables(queue).length === 2);
			check(queue.getLayer(2).opaqueCount === 1);
			check(queue.getLayer(2).transparentCount === 1);
		}
	},
	{
		group: 'integration',
		name: 'shared skeleton updates once per version and catches up next frame',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const graph = buildGraph(scene, collector, {
				rooms: ['A', 'B', 'C'],
				portals: [
					{ from: 'A', to: 'B', pos: new Vector3(0, 0, -4), halfW: 1, halfH: 1.5 }
				]
			});
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, graph.rooms.A);

			const bone = new Bone();
			bone.position.x = 0.4;
			scene.add(bone);
			const skeleton = new Skeleton([bone], [new Matrix4()]);
			let updates = 0;
			const originalUpdate = skeleton.updateBones.bind(skeleton);
			skeleton.updateBones = function(data) {
				updates++;
				originalUpdate(data);
			};
			const geometry = makeGeometry(0.1);
			const material = new BasicMaterial();
			const skin1 = new SkinnedMesh(geometry, material);
			skin1.position.set(0, 0.2, -6);
			skin1.skeleton = skeleton;
			const skin2 = new SkinnedMesh(geometry, material);
			skin2.position.set(0, -0.2, -6);
			skin2.skeleton = skeleton;
			graph.rooms.B.add(skin1); graph.rooms.B.add(skin2);

			// Room C has no door yet: its skinned mesh is one frame late.
			const late = new SkinnedMesh(geometry, material);
			late.position.set(0, 0, -10);
			late.skeleton = skeleton;
			graph.rooms.C.add(late);

			collect(scene, camera);
			check(updates === 1);
			check(Math.abs(skeleton.boneMatrices[12] - 0.4) < 1e-6);

			// A second camera interleaving without skeleton version must not update.
			collect(scene, camera, false, false);
			check(updates === 1);

			// Open the path to C on the next frame: one more version, one update.
			const door = new Object3D();
			door.position.set(0, 0, -8);
			scene.add(door);
			collector.addPortal(graph.rooms.B, graph.rooms.C, door, rectXY(1.4, 1.6));
			const queue = collect(scene, camera);
			check(updates === 2);
			check(queue.skeletons.size === 1);
			check(hasObject(queue, late));
		}
	},
	{
		group: 'integration',
		name: 'lights keep whole-scene visibility and ordinary path stays available',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const graph = buildGraph(scene, collector, {
				rooms: ['A', 'B'],
				portals: [
					{ from: 'A', to: 'B', pos: new Vector3(0, 0, -4), halfW: 0.5, halfH: 1 }
				]
			});
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, graph.rooms.A);

			const light = new AmbientLight();
			light.position.set(0, 0, -6);
			graph.rooms.B.add(light);
			const behindWall = makeBall(new Vector3(2, 0, -6), 0.1);
			graph.rooms.B.add(behindWall);
			const outside = makeBall(new Vector3(0, 0, -2), 0.1);
			scene.add(outside);

			const queue = collect(scene, camera);
			check(!hasObject(queue, behindWall));
			check(hasObject(queue, outside));
			const lights = scene.getRenderStates(camera).lighting.lightsArray;
			check(lights.length === 1 && lights[0] === light);

			// Ordinary collection on the same scene sees through walls.
			collector.setCameraRoom(camera, null);
			const ordinary = collect(scene, camera);
			check(hasObject(ordinary, behindWall));
			check(hasObject(ordinary, outside));
			check(collector.portal_clips === 0 && collector.sphere_tests === 0);
		}
	},
	{
		group: 'integration',
		name: 'non perspective camera is rejected while portal collection is active',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const roomA = new Object3D();
			scene.add(roomA);
			collector.addRoom(roomA);
			const camera = new Camera();
			camera.setOrtho(-2, 2, 2, -2, 1, 100);
			camera.position.set(0, 0, -0.5);
			scene.add(camera);
			collector.setCameraRoom(camera, roomA);
			let errored = false;
			try {
				collect(scene, camera);
			} catch (error) {
				errored = error instanceof Error;
			}
			check(errored);

			// Clearing the room assignment makes the ortho camera work normally.
			collector.setCameraRoom(camera, null);
			const queue = collect(scene, camera);
			check(Array.isArray(queue.layerList));
		}
	},
	{
		group: 'integration',
		name: 'hidden or detached camera room is an explicit error',
		run(check) {
			const scene = new Scene();
			const collector = installCollector(scene);
			const roomA = new Object3D();
			scene.add(roomA);
			collector.addRoom(roomA);
			const camera = makeCamera(new Vector3(0, 0, -0.5));
			scene.add(camera);
			collector.setCameraRoom(camera, roomA);
			roomA.add(makeBall(new Vector3(0, 0, -2), 0.1));

			roomA.visible = false;
			check(throws(() => collect(scene, camera)));
			roomA.visible = true;
			check(collect(scene, camera) && true);

			scene.remove(roomA);
			check(throws(() => collect(scene, camera)));
			scene.add(roomA);
			check(collect(scene, camera) && true);
		}
	}
];

export const cases = [...geometryCases, ...membershipCases, ...integrationCases];
