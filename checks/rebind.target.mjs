// Checks for AnimationMixer.rebindRoot().
//
// Windowless usage example (plain Node.js, no DOM, no renderer):
//
//   import { AnimationMixer } from '../src/animation/AnimationMixer.js';
//   import { AnimationClip } from '../src/animation/AnimationClip.js';
//   import { VectorKeyframeTrack } from '../src/animation/tracks/VectorKeyframeTrack.js';
//   import { Bone } from '../src/objects/Bone.js';
//   import { Skeleton } from '../src/objects/Skeleton.js';
//   import { SkinnedMesh } from '../src/objects/SkinnedMesh.js';
//
//   const mixer = new AnimationMixer( mesh );
//   const action = mixer.clipAction( new AnimationClip( 'reach', 1, [
//   	new VectorKeyframeTrack( '.bones[Hand].position', [ 0, 1 ], [ 1, 0, 0, 3, 0, 0 ] )
//   ] ) );
//   action.play();
//   mixer.update( 0.5 ); // hand bone is animated
//
//   // the running role swaps its same-named skeleton subtree at runtime
//   mesh.remove( oldHip );
//   mesh.add( newHip ); // newHip -> newHand, same names and rest pose
//   mesh.updateMatrixWorld( true );
//   mesh.bind( new Skeleton( [ newHip, newHand ] ) );
//
//   mixer.rebindRoot();  // retarget every cached action to the new subtree
//   mixer.update( 0.1 ); // playback continues on the new bones
//
// The skinning group below executes exactly this flow and verifies the
// skinned vertices via SkinnedMesh.getVertexPosition().

import { AnimationMixer } from '../src/animation/AnimationMixer.js';
import { AnimationClip } from '../src/animation/AnimationClip.js';
import { VectorKeyframeTrack } from '../src/animation/tracks/VectorKeyframeTrack.js';
import { NumberKeyframeTrack } from '../src/animation/tracks/NumberKeyframeTrack.js';
import { QuaternionKeyframeTrack } from '../src/animation/tracks/QuaternionKeyframeTrack.js';
import { Object3D } from '../src/core/Object3D.js';
import { Bone } from '../src/objects/Bone.js';
import { Skeleton } from '../src/objects/Skeleton.js';
import { SkinnedMesh } from '../src/objects/SkinnedMesh.js';
import { Mesh } from '../src/objects/Mesh.js';
import { BufferGeometry } from '../src/core/BufferGeometry.js';
import { Float32BufferAttribute, Uint16BufferAttribute } from '../src/core/BufferAttribute.js';
import { Vector3 } from '../src/math/Vector3.js';
import { Quaternion } from '../src/math/Quaternion.js';

const EPS = 1e-6;

function approx( a, b ) {

	return Math.abs( a - b ) <= EPS;

}

function vecApprox( v, x, y, z ) {

	return approx( v.x, x ) && approx( v.y, y ) && approx( v.z, z );

}

function quatApprox( q, x, y, z, w ) {

	return approx( q.x, x ) && approx( q.y, y ) && approx( q.z, z ) && approx( q.w, w );

}

function makeMorphGeometry() {

	const geometry = new BufferGeometry();
	geometry.setAttribute( 'position', new Float32BufferAttribute( [ 0, 0, 0, 1, 0, 0, 0, 1, 0 ], 3 ) );
	const morph = new Float32BufferAttribute( [ 0, 0, 1, 1, 0, 1, 0, 1, 1 ], 3 );
	morph.name = 'smile';
	geometry.morphAttributes.position = [ morph ];
	return geometry;

}

// A failing rebind must be atomic: bindings, buffers, action state and scene
// properties stay untouched, and playback simply continues afterwards.
function transaction( check ) {

	const root = new Object3D();
	root.name = 'Rig';
	const alpha = new Object3D();
	alpha.name = 'Alpha';
	root.add( alpha );
	const beta = new Object3D();
	beta.name = 'Beta';
	root.add( beta );

	const clip = new AnimationClip( 'move', 1, [
		new VectorKeyframeTrack( 'Alpha.position', [ 0, 1 ], [ 1, 0, 0, 2, 0, 0 ] ),
		new VectorKeyframeTrack( 'Beta.position', [ 0, 1 ], [ 5, 0, 0, 7, 0, 0 ] )
	] );
	const mixer = new AnimationMixer( root );
	const action = mixer.clipAction( clip ).play();
	mixer.update( 0.5 );

	check( vecApprox( alpha.position, 1.5, 0, 0 ), 'alpha animated before swap' );
	check( vecApprox( beta.position, 6, 0, 0 ), 'beta animated before swap' );
	check( approx( mixer.time, 0.5 ), 'mixer time advanced' );

	// caller replaces the subtree with same-named nodes
	const newAlpha = new Object3D();
	newAlpha.name = 'Alpha';
	const newBeta = new Object3D();
	newBeta.name = 'Beta';
	newBeta.position.set( 0, 9, 0 );
	root.remove( alpha );
	root.remove( beta );
	root.add( newAlpha );
	root.add( newBeta );

	// sabotage: one track can no longer be resolved
	newBeta.name = 'BetaMissing';

	let thrown = null;

	try {

		mixer.rebindRoot();

	} catch ( error ) {

		thrown = error;

	}

	check( thrown instanceof Error, 'rebindRoot throws an Error when a target node is missing' );

	// nothing may have changed partially
	check( vecApprox( alpha.position, 1.5, 0, 0 ), 'old alpha keeps its animated value after failed rebind' );
	check( vecApprox( beta.position, 6, 0, 0 ), 'old beta keeps its animated value after failed rebind' );
	check( vecApprox( newAlpha.position, 0, 0, 0 ), 'new alpha untouched after failed rebind' );
	check( vecApprox( newBeta.position, 0, 9, 0 ), 'new beta untouched after failed rebind' );
	check( mixer.stats.bindings.inUse === 2, 'active binding count unchanged after failed rebind' );
	check( mixer.stats.bindings.total === 2, 'binding total unchanged after failed rebind' );
	check( approx( mixer.time, 0.5 ), 'mixer time unchanged after failed rebind' );
	check( action.isRunning(), 'action still running after failed rebind' );

	// playback continues on the untouched bindings
	mixer.update( 0.25 );
	check( vecApprox( alpha.position, 1.75, 0, 0 ), 'old alpha keeps animating after failed rebind' );
	check( vecApprox( beta.position, 6.5, 0, 0 ), 'old beta keeps animating after failed rebind' );
	check( vecApprox( newAlpha.position, 0, 0, 0 ), 'new alpha still untouched' );
	check( vecApprox( newBeta.position, 0, 9, 0 ), 'new beta still untouched' );

	// repair the hierarchy, rebind for real
	newBeta.name = 'Beta';
	check( mixer.rebindRoot( mixer.getRoot() ) === mixer, 'rebindRoot returns the mixer on success' );

	check( vecApprox( alpha.position, 0, 0, 0 ), 'old alpha restored to first-activation original' );
	check( vecApprox( beta.position, 0, 0, 0 ), 'old beta restored to first-activation original' );
	check( vecApprox( newAlpha.position, 0, 0, 0 ), 'new alpha keeps its captured value until the next update' );
	check( vecApprox( newBeta.position, 0, 9, 0 ), 'new beta keeps its captured value until the next update' );
	check( mixer.stats.bindings.inUse === 2, 'active binding count preserved by rebind' );
	check( mixer.stats.bindings.total === 2, 'rebind allocates no new bindings' );
	check( mixer.rebindRoot() === mixer, 'rebinding twice is a successful no-op' );

	// the same sample values as before the rebind must still be written to the new targets
	mixer.update( 0 );
	check( vecApprox( newAlpha.position, 1.75, 0, 0 ), 'new alpha written by update(0) even though the sample did not change' );
	check( vecApprox( newBeta.position, 6.5, 0, 0 ), 'new beta written by update(0) even though the sample did not change' );
	check( vecApprox( alpha.position, 0, 0, 0 ), 'old alpha no longer written' );
	check( vecApprox( beta.position, 0, 0, 0 ), 'old beta no longer written' );

	mixer.update( 0.125 );
	check( vecApprox( newAlpha.position, 1.875, 0, 0 ), 'new alpha keeps animating' );
	check( vecApprox( newBeta.position, 6.75, 0, 0 ), 'new beta keeps animating' );
	check( vecApprox( alpha.position, 0, 0, 0 ), 'old alpha stays restored' );
	check( vecApprox( beta.position, 0, 0, 0 ), 'old beta stays restored' );

	action.stop();
	check( vecApprox( newAlpha.position, 0, 0, 0 ), 'new alpha restored to its own original value' );
	check( vecApprox( newBeta.position, 0, 9, 0 ), 'new beta restored to its own original value' );
	check( vecApprox( alpha.position, 0, 0, 0 ), 'old alpha not touched by stop' );
	check( vecApprox( beta.position, 0, 0, 0 ), 'old beta not touched by stop' );
	check( mixer.stats.bindings.inUse === 0, 'no active bindings after stop' );

	// existing uncache and re-play behavior is preserved after a rebind
	mixer.uncacheClip( clip );
	check( mixer.stats.bindings.total === 0, 'uncacheClip releases the rebound bindings' );
	const replayed = mixer.clipAction( clip ).play();
	mixer.update( 0.5 );
	check( vecApprox( newAlpha.position, 1.5, 0, 0 ), 're-created action plays on the current subtree' );
	check( vecApprox( newBeta.position, 6, 0, 0 ), 're-created action animates the current beta' );
	replayed.stop();

}

// Actions sharing a path keep sharing one PropertyMixer across a rebind;
// fades, pauses, not-yet-played actions and unchanged targets behave.
function shared_lifecycle( check ) {

	const root = new Object3D();
	root.name = 'Rig';
	const limb = new Object3D();
	limb.name = 'Limb';
	root.add( limb );

	const clipA = new AnimationClip( 'swing', 1, [
		new VectorKeyframeTrack( 'Limb.position', [ 0, 1 ], [ 0, 0, 0, 4, 0, 0 ] ),
		new VectorKeyframeTrack( 'Rig.position', [ 0, 1 ], [ 0, 0, 0, 0, 0, 4 ] )
	] );
	const clipB = new AnimationClip( 'lift', 1, [
		new VectorKeyframeTrack( 'Limb.position', [ 0, 1 ], [ 0, 4, 0, 0, 4, 0 ] )
	] );

	const mixer = new AnimationMixer( root );
	const a1 = mixer.clipAction( clipA ).play();
	const a2 = mixer.clipAction( clipB );

	check( a1._propertyBindings[ 0 ] === a2._propertyBindings[ 0 ], 'actions on the same path share one PropertyMixer' );
	check( a1._propertyBindings[ 0 ].referenceCount === 2, 'shared binding reference count is two' );
	check( a1._propertyBindings[ 0 ].useCount === 1, 'only the played action uses the binding' );

	mixer.update( 0.2 );
	a2.play().fadeIn( 1 ); // fade scheduled over mixer time [0.2, 1.2]
	mixer.update( 0.3 ); // t = 0.5, fade factor 0.3

	check( vecApprox( limb.position, 2 / 1.3, 4 * 0.3 / 1.3, 0 ), 'mid-fade blend on the old target' );
	check( approx( root.position.z, 2 ), 'root track animated' );
	check( a1._propertyBindings[ 0 ].useCount === 2, 'both actions use the shared binding' );

	// replace the node in the middle of the fade-in
	const newLimb = new Object3D();
	newLimb.name = 'Limb';
	newLimb.position.set( 10, 10, 10 );
	root.remove( limb );
	root.add( newLimb );

	check( mixer.rebindRoot() === mixer, 'rebind succeeds mid-fade' );
	check( a1._propertyBindings[ 0 ] === a2._propertyBindings[ 0 ], 'shared PropertyMixer survives the rebind' );
	check( a1._propertyBindings[ 0 ].referenceCount === 2, 'reference count preserved' );
	check( a1._propertyBindings[ 0 ].useCount === 2, 'use count preserved' );
	check( mixer.stats.bindings.total === 2, 'no PropertyMixer recreated' );
	check( mixer.stats.bindings.inUse === 2, 'active binding set preserved' );
	check( vecApprox( limb.position, 0, 0, 0 ), 'old target restored to its first-activation original' );
	check( vecApprox( newLimb.position, 10, 10, 10 ), 'new target keeps its captured value until the next update' );
	check( approx( root.position.z, 2 ), 'unchanged root target is not restored mid-play' );

	mixer.update( 0.2 ); // t = 0.7, fade factor 0.5 if the fade was not restarted
	check( vecApprox( newLimb.position, 2.8 / 1.5, 4 * 0.5 / 1.5, 0 ), 'fade continues on the new target without restarting' );
	check( vecApprox( limb.position, 0, 0, 0 ), 'old target not written anymore' );
	check( approx( root.position.z, 2.8 ), 'root track keeps animating' );

	mixer.update( 0.6 ); // t = 1.3, fade complete, a1 wrapped to clip time 0.3
	check( vecApprox( newLimb.position, 0.6, 2, 0 ), 'full blend after the fade completed' );

	// a paused action is rebound as well
	a1.paused = true;
	const pausedLimb = new Object3D();
	pausedLimb.name = 'Limb';
	pausedLimb.position.set( - 3, - 3, - 3 );
	root.remove( newLimb );
	root.add( pausedLimb );
	mixer.rebindRoot();

	check( vecApprox( newLimb.position, 10, 10, 10 ), 'previous target restored to its own captured original' );
	check( vecApprox( pausedLimb.position, - 3, - 3, - 3 ), 'paused action target captured but not yet written' );
	check( approx( root.position.z, 1.2 ), 'paused root track left at its current value' );

	mixer.update( 0.1 ); // a1 stays at clip time 0.3 while paused
	check( vecApprox( pausedLimb.position, 0.6, 2, 0 ), 'paused action still applies to the new target' );
	a1.paused = false;

	// a not-yet-played action is rebound without touching the scene
	const clipC = new AnimationClip( 'bulk', 1, [
		new VectorKeyframeTrack( 'Limb.scale', [ 0, 1 ], [ 1, 1, 1, 3, 3, 3 ] )
	] );
	const a3 = mixer.clipAction( clipC ); // never played
	check( mixer.stats.bindings.total === 3, 'inactive binding of the unplayed action is cached' );

	const freshLimb = new Object3D();
	freshLimb.name = 'Limb';
	freshLimb.position.set( 1, 2, 3 );
	root.remove( pausedLimb );
	root.add( freshLimb );
	mixer.rebindRoot();

	check( vecApprox( pausedLimb.position, - 3, - 3, - 3 ), 'previous target restored to its own captured original again' );
	check( vecApprox( freshLimb.position, 1, 2, 3 ), 'new target captured but not yet written' );
	check( vecApprox( freshLimb.scale, 1, 1, 1 ), 'inactive binding does not touch the new target' );

	freshLimb.scale.set( 2, 2, 2 ); // scene change before the first play
	a3.play(); // the original state is captured now, not at rebind time
	mixer.update( 0.25 );
	check( vecApprox( freshLimb.scale, 1.5, 1.5, 1.5 ), 'unplayed action animates the new target after its first play' );
	check( vecApprox( freshLimb.position, 1.1, 2, 0 ), 'shared position blend follows on the new target' );

	a3.stop();
	check( vecApprox( freshLimb.scale, 2, 2, 2 ), 'first-play capture is the original restored on stop' );

	a1.stop();
	a2.stop();
	check( vecApprox( freshLimb.position, 1, 2, 3 ), 'stopping the last action restores the new target original' );
	check( vecApprox( root.position, 0, 0, 0 ), 'unchanged target restored to its first-activation original, not a played value' );
	check( vecApprox( limb.position, 0, 0, 0 ), 'first target left at its restored value' );
	check( mixer.stats.bindings.inUse === 0, 'no active bindings left' );
	check( mixer.stats.actions.inUse === 0, 'no active actions left' );

}

// Bone names, plain node TRS, morphTargetInfluences named indices, direct
// properties, whole arrays and array elements rebind correctly; every
// resolution failure kind throws.
function binding_shapes( check ) {

	// --- bones, addressed via .bones[name] and via plain node name ---
	const skinGeometry = new BufferGeometry();
	skinGeometry.setAttribute( 'position', new Float32BufferAttribute( [ 0, 0, 0 ], 3 ) );
	skinGeometry.setAttribute( 'skinIndex', new Uint16BufferAttribute( [ 0, 0, 0, 0 ], 4 ) );
	skinGeometry.setAttribute( 'skinWeight', new Float32BufferAttribute( [ 1, 0, 0, 0 ], 4 ) );
	const body = new SkinnedMesh( skinGeometry );
	body.name = 'Body';
	const hip = new Bone();
	hip.name = 'Hip';
	const hand = new Bone();
	hand.name = 'Hand';
	hand.position.x = 1;
	hip.add( hand );
	body.add( hip );
	body.bind( new Skeleton( [ hip, hand ] ) );

	const boneMixer = new AnimationMixer( body );
	const boneClip = new AnimationClip( 'bones', 1, [
		new VectorKeyframeTrack( '.bones[Hip].position', [ 0, 1 ], [ 0, 0, 0, 0, 2, 0 ] ),
		new VectorKeyframeTrack( 'Hand.position', [ 0, 1 ], [ 1, 0, 0, 3, 0, 0 ] )
	] );
	const boneAction = boneMixer.clipAction( boneClip ).play();
	boneMixer.update( 0.5 );
	check( vecApprox( hip.position, 0, 1, 0 ), 'hip animated via .bones[name]' );
	check( vecApprox( hand.position, 2, 0, 0 ), 'hand animated via node name' );

	const hip2 = new Bone();
	hip2.name = 'Hip';
	const hand2 = new Bone();
	hand2.name = 'Hand';
	hand2.position.x = 1;
	hip2.add( hand2 );
	body.remove( hip );
	body.add( hip2 );
	body.updateMatrixWorld( true );
	body.bind( new Skeleton( [ hip2, hand2 ] ) );

	boneMixer.rebindRoot();
	check( vecApprox( hip.position, 0, 0, 0 ), 'old hip restored' );
	check( vecApprox( hand.position, 1, 0, 0 ), 'old hand restored' );
	check( vecApprox( hip2.position, 0, 0, 0 ), 'new hip captured but not yet written' );
	check( vecApprox( hand2.position, 1, 0, 0 ), 'new hand captured but not yet written' );

	boneMixer.update( 0.25 );
	check( vecApprox( hip2.position, 0, 1.5, 0 ), 'new hip animated' );
	check( vecApprox( hand2.position, 2.5, 0, 0 ), 'new hand animated' );
	check( vecApprox( hip.position, 0, 0, 0 ), 'old hip no longer written' );
	check( vecApprox( hand.position, 1, 0, 0 ), 'old hand no longer written' );

	boneAction.stop();
	check( vecApprox( hip2.position, 0, 0, 0 ), 'new hip restored to its own original' );
	check( vecApprox( hand2.position, 1, 0, 0 ), 'new hand restored to its own original' );

	// --- plain node TRS, including quaternion ---
	const rig = new Object3D();
	rig.name = 'Rig';
	const joint = new Object3D();
	joint.name = 'Joint';
	rig.add( joint );

	const q0 = new Quaternion();
	const q1 = new Quaternion().setFromAxisAngle( new Vector3( 0, 0, 1 ), Math.PI / 2 );
	const trsMixer = new AnimationMixer( rig );
	const trsClip = new AnimationClip( 'trs', 1, [
		new VectorKeyframeTrack( 'Joint.position', [ 0, 1 ], [ 0, 0, 0, 2, 0, 0 ] ),
		new QuaternionKeyframeTrack( 'Joint.quaternion', [ 0, 1 ], [ q0.x, q0.y, q0.z, q0.w, q1.x, q1.y, q1.z, q1.w ] ),
		new VectorKeyframeTrack( 'Joint.scale', [ 0, 1 ], [ 1, 1, 1, 2, 2, 2 ] )
	] );
	const trsAction = trsMixer.clipAction( trsClip ).play();
	trsMixer.update( 0.5 );
	check( vecApprox( joint.position, 1, 0, 0 ), 'position track animated' );
	check( quatApprox( joint.quaternion, 0, 0, Math.sin( Math.PI / 8 ), Math.cos( Math.PI / 8 ) ), 'quaternion track animated' );
	check( vecApprox( joint.scale, 1.5, 1.5, 1.5 ), 'scale track animated' );

	const joint2 = new Object3D();
	joint2.name = 'Joint';
	joint2.position.set( 5, 5, 5 );
	joint2.scale.set( 0.5, 0.5, 0.5 );
	rig.remove( joint );
	rig.add( joint2 );
	trsMixer.rebindRoot();

	check( vecApprox( joint.position, 0, 0, 0 ), 'old joint position restored' );
	check( quatApprox( joint.quaternion, 0, 0, 0, 1 ), 'old joint quaternion restored' );
	check( vecApprox( joint.scale, 1, 1, 1 ), 'old joint scale restored' );
	check( vecApprox( joint2.position, 5, 5, 5 ), 'new joint position captured but not yet written' );
	check( vecApprox( joint2.scale, 0.5, 0.5, 0.5 ), 'new joint scale captured but not yet written' );

	trsMixer.update( 0 );
	check( vecApprox( joint2.position, 1, 0, 0 ), 'new joint position animated' );
	check( quatApprox( joint2.quaternion, 0, 0, Math.sin( Math.PI / 8 ), Math.cos( Math.PI / 8 ) ), 'new joint quaternion animated' );
	check( vecApprox( joint2.scale, 1.5, 1.5, 1.5 ), 'new joint scale animated' );

	trsAction.stop();
	check( vecApprox( joint2.position, 5, 5, 5 ), 'new joint position restored to its own original' );
	check( quatApprox( joint2.quaternion, 0, 0, 0, 1 ), 'new joint quaternion restored to its own original' );
	check( vecApprox( joint2.scale, 0.5, 0.5, 0.5 ), 'new joint scale restored to its own original' );

	// --- morphTargetInfluences via named index ---
	const morphRig = new Object3D();
	const face = new Mesh( makeMorphGeometry() );
	face.name = 'Face';
	morphRig.add( face );
	const morphMixer = new AnimationMixer( morphRig );
	const morphClip = new AnimationClip( 'morph', 1, [
		new NumberKeyframeTrack( 'Face.morphTargetInfluences[smile]', [ 0, 1 ], [ 0, 1 ] )
	] );
	const morphAction = morphMixer.clipAction( morphClip ).play();
	morphMixer.update( 0.5 );
	check( approx( face.morphTargetInfluences[ 0 ], 0.5 ), 'named morph index animated on the old mesh' );

	const face2 = new Mesh( makeMorphGeometry() );
	face2.name = 'Face';
	morphRig.remove( face );
	morphRig.add( face2 );
	morphMixer.rebindRoot();

	check( approx( face.morphTargetInfluences[ 0 ], 0 ), 'old mesh influence restored' );
	check( approx( face2.morphTargetInfluences[ 0 ], 0 ), 'new mesh influence captured but not yet written' );

	morphMixer.update( 0.25 );
	check( approx( face2.morphTargetInfluences[ 0 ], 0.75 ), 'new mesh influence animated' );
	check( approx( face.morphTargetInfluences[ 0 ], 0 ), 'old mesh no longer written' );

	morphAction.stop();
	check( approx( face2.morphTargetInfluences[ 0 ], 0 ), 'new mesh influence restored to its own original' );

	// --- direct property, whole array, array element ---
	const propRig = new Object3D();
	const node = new Object3D();
	node.name = 'Node';
	node.hp = 0;
	node.slots = [ 0, 0, 0 ];
	node.gains = [ 0, 0, 0 ];
	propRig.add( node );
	const propMixer = new AnimationMixer( propRig );
	const propClip = new AnimationClip( 'shapes', 1, [
		new NumberKeyframeTrack( 'Node.hp', [ 0, 1 ], [ 0, 10 ] ),
		new VectorKeyframeTrack( 'Node.slots', [ 0, 1 ], [ 0, 0, 0, 3, 6, 9 ] ),
		new NumberKeyframeTrack( 'Node.gains[1]', [ 0, 1 ], [ 0, 100 ] )
	] );
	const propAction = propMixer.clipAction( propClip ).play();
	propMixer.update( 0.5 );
	check( approx( node.hp, 5 ), 'direct property animated' );
	check( approx( node.slots[ 0 ], 1.5 ) && approx( node.slots[ 1 ], 3 ) && approx( node.slots[ 2 ], 4.5 ), 'whole array animated' );
	check( approx( node.gains[ 1 ], 50 ), 'array element animated' );

	const node2 = new Object3D();
	node2.name = 'Node';
	node2.hp = 1;
	node2.slots = [ 7, 7, 7 ];
	node2.gains = [ 8, 8, 8 ];
	propRig.remove( node );
	propRig.add( node2 );
	propMixer.rebindRoot();

	check( node.hp === 0 && node.slots[ 0 ] === 0 && node.gains[ 1 ] === 0, 'old node properties restored' );
	check( node2.hp === 1 && node2.slots[ 0 ] === 7 && node2.gains[ 1 ] === 8, 'new node properties captured but not yet written' );

	propMixer.update( 0 );
	check( approx( node2.hp, 5 ), 'new direct property animated' );
	check( approx( node2.slots[ 0 ], 1.5 ) && approx( node2.slots[ 2 ], 4.5 ), 'new whole array animated' );
	check( approx( node2.gains[ 1 ], 50 ), 'new array element animated' );

	propAction.stop();
	check( node2.hp === 1 && node2.slots[ 1 ] === 7 && node2.gains[ 1 ] === 8, 'new node restored to its own originals' );

	// --- actions bound to another root are not affected ---
	const mainRoot = new Object3D();
	mainRoot.name = 'Main';
	const mainChild = new Object3D();
	mainChild.name = 'Sub';
	mainRoot.add( mainChild );
	const otherRoot = new Object3D();
	otherRoot.name = 'Other';
	const otherChild = new Object3D();
	otherChild.name = 'Sub';
	otherRoot.add( otherChild );
	const multiMixer = new AnimationMixer( mainRoot );
	const sharedClip = new AnimationClip( 'shared', 1, [
		new VectorKeyframeTrack( 'Sub.position', [ 0, 1 ], [ 0, 0, 0, 2, 0, 0 ] )
	] );
	const mainAction = multiMixer.clipAction( sharedClip ).play();
	const otherAction = multiMixer.clipAction( sharedClip, otherRoot ).play();
	multiMixer.update( 0.5 );
	check( vecApprox( mainChild.position, 1, 0, 0 ) && vecApprox( otherChild.position, 1, 0, 0 ), 'both roots animated' );

	const mainChild2 = new Object3D();
	mainChild2.name = 'Sub';
	mainRoot.remove( mainChild );
	mainRoot.add( mainChild2 );
	multiMixer.rebindRoot(); // defaults to the mixer root
	check( vecApprox( mainChild.position, 0, 0, 0 ), 'mixer root target restored' );
	check( vecApprox( otherChild.position, 1, 0, 0 ), 'action of the other root untouched by the rebind' );

	multiMixer.update( 0.25 );
	check( vecApprox( mainChild2.position, 1.5, 0, 0 ), 'mixer root rebound to the new subtree' );
	check( vecApprox( otherChild.position, 1.5, 0, 0 ), 'other root keeps animating on its original target' );

	mainAction.stop();
	otherAction.stop();
	check( vecApprox( mainChild2.position, 0, 0, 0 ), 'new subtree target restored on stop' );
	check( vecApprox( otherChild.position, 0, 0, 0 ), 'other root restored on stop' );

	// --- every resolution failure kind throws and leaves state intact ---
	const good = new Object3D();
	good.name = 'Good';
	const failRig = new Object3D();
	failRig.add( good );
	const failMixer = new AnimationMixer( failRig );
	const goodAction = failMixer.clipAction( new AnimationClip( 'good', 1, [
		new VectorKeyframeTrack( 'Good.position', [ 0, 1 ], [ 0, 0, 0, 2, 0, 0 ] )
	] ) ).play();
	failMixer.update( 0.5 );
	check( vecApprox( good.position, 1, 0, 0 ), 'good action animated before failure checks' );

	function expectThrow( mixer, message ) {

		let error = null;

		try {

			mixer.rebindRoot();

		} catch ( e ) {

			error = e;

		}

		check( error instanceof Error, message );

	}

	// missing property
	failMixer.clipAction( new AnimationClip( 'bad', 1, [
		new NumberKeyframeTrack( 'Good.missing', [ 0, 1 ], [ 0, 1 ] )
	] ) );
	expectThrow( failMixer, 'missing property throws' );
	check( vecApprox( good.position, 1, 0, 0 ), 'active binding untouched by failed rebind' );
	failMixer.update( 0.25 );
	check( vecApprox( good.position, 1.5, 0, 0 ), 'good action keeps playing after failed rebind' );
	goodAction.stop();

	// array index out of bounds
	const oobRig = new Object3D();
	const oobNode = new Object3D();
	oobNode.name = 'M';
	oobNode.arr = [ 1, 2, 3 ];
	oobRig.add( oobNode );
	const oobMixer = new AnimationMixer( oobRig );
	oobMixer.clipAction( new AnimationClip( 'oob', 1, [
		new NumberKeyframeTrack( 'M.arr[5]', [ 0, 1 ], [ 0, 1 ] )
	] ) );
	expectThrow( oobMixer, 'array index out of bounds throws' );
	check( oobNode.arr[ 0 ] === 1 && oobNode.arr[ 2 ] === 3, 'array untouched by failed rebind' );

	// morph target index out of bounds
	const morphOobRig = new Object3D();
	const morphFace = new Mesh( makeMorphGeometry() );
	morphFace.name = 'Face';
	morphOobRig.add( morphFace );
	const morphOobMixer = new AnimationMixer( morphOobRig );
	morphOobMixer.clipAction( new AnimationClip( 'oob', 1, [
		new NumberKeyframeTrack( 'Face.morphTargetInfluences[9]', [ 0, 1 ], [ 0, 1 ] )
	] ) );
	expectThrow( morphOobMixer, 'morph target index out of bounds throws' );
	check( approx( morphFace.morphTargetInfluences[ 0 ], 0 ), 'morph influences untouched by failed rebind' );

	// property dimension does not match the track
	const dimRig = new Object3D();
	const dimNode = new Object3D();
	dimNode.name = 'D';
	dimNode.scalar = 0;
	dimRig.add( dimNode );
	const dimMixer = new AnimationMixer( dimRig );
	dimMixer.clipAction( new AnimationClip( 'dim', 1, [
		new VectorKeyframeTrack( 'D.scalar', [ 0, 1 ], [ 0, 0, 0, 1, 1, 1 ] )
	] ) );
	expectThrow( dimMixer, 'dimension mismatch throws' );
	check( dimNode.scalar === 0, 'scalar untouched by failed rebind' );

	// a mixer without any cached action rebinds successfully
	const emptyMixer = new AnimationMixer( new Object3D() );
	check( emptyMixer.rebindRoot() === emptyMixer, 'rebind with no cached actions returns the mixer' );

}

// A skeleton swap under a non-unit ancestor transform with non-unit
// inverse-bind matrices keeps the skinned vertices identical, verified
// through SkinnedMesh.getVertexPosition() against a reference rig.
function skinning( check ) {

	function buildRig() {

		const geometry = new BufferGeometry();
		geometry.setAttribute( 'position', new Float32BufferAttribute( [ 2, 0, 0, 0.5, 0, 0 ], 3 ) );
		geometry.setAttribute( 'skinIndex', new Uint16BufferAttribute( [ 1, 0, 0, 0, 0, 0, 0, 0 ], 4 ) );
		geometry.setAttribute( 'skinWeight', new Float32BufferAttribute( [ 1, 0, 0, 0, 1, 0, 0, 0 ], 4 ) );
		const mesh = new SkinnedMesh( geometry );
		mesh.name = 'Skin';
		const hip = new Bone();
		hip.name = 'Hip';
		const hand = new Bone();
		hand.name = 'Hand';
		hand.position.x = 1; // non-unit inverse-bind matrix
		hip.add( hand );
		mesh.add( hip );
		return { mesh, hip, hand };

	}

	// non-unit ancestor transform
	const world = new Object3D();
	world.name = 'World';
	world.position.set( 3, - 2, 5 );
	world.scale.set( 2, 2, 2 );
	world.rotation.y = 0.4;

	const rigA = buildRig();
	world.add( rigA.mesh );
	world.updateMatrixWorld( true );
	rigA.mesh.bind( new Skeleton( [ rigA.hip, rigA.hand ] ) );

	// reference rig that is never swapped, animated in lockstep
	const rigB = buildRig();
	world.add( rigB.mesh );
	world.updateMatrixWorld( true );
	rigB.mesh.bind( new Skeleton( [ rigB.hip, rigB.hand ] ) );

	const clip = new AnimationClip( 'reach', 1, [
		new VectorKeyframeTrack( '.bones[Hand].position', [ 0, 1 ], [ 1, 0, 0, 3, 0, 0 ] )
	] );
	const mixer = new AnimationMixer( rigA.mesh );
	const action = mixer.clipAction( clip ).play();
	const refMixer = new AnimationMixer( rigB.mesh );
	const refAction = refMixer.clipAction( clip ).play();

	const vA = new Vector3();
	const vB = new Vector3();

	function vertex( mesh, index, target ) {

		mesh.updateMatrixWorld( true );
		return mesh.getVertexPosition( index, target );

	}

	const restV = vertex( rigA.mesh, 0, vA ).clone();
	const restV1 = vertex( rigA.mesh, 1, new Vector3() ).clone();

	mixer.update( 0.5 );
	refMixer.update( 0.5 );
	check( approx( rigA.hand.position.x, 2 ), 'hand animated before the swap' );

	const animV = vertex( rigA.mesh, 0, vA ).clone();
	check( animV.distanceTo( vertex( rigB.mesh, 0, vB ) ) < EPS, 'vertex matches the reference rig before the swap' );
	check( animV.distanceTo( restV ) > 0.1, 'animation actually moves the vertex' );

	// the caller swaps the skeleton subtree for an identical one
	const hip2 = new Bone();
	hip2.name = 'Hip';
	const hand2 = new Bone();
	hand2.name = 'Hand';
	hand2.position.x = 1;
	hip2.add( hand2 );
	rigA.mesh.remove( rigA.hip );
	rigA.mesh.add( hip2 );
	rigA.mesh.updateMatrixWorld( true );
	rigA.mesh.bind( new Skeleton( [ hip2, hand2 ] ) );

	check( mixer.rebindRoot() === mixer, 'rebind succeeds after the skeleton swap' );
	check( approx( rigA.hand.position.x, 1 ), 'old hand restored to its original pose' );
	check( approx( hand2.position.x, 1 ), 'new hand keeps its captured pose until the next update' );
	check( vertex( rigA.mesh, 0, vA ).distanceTo( restV ) < EPS, 'vertex back at rest right after the swap' );

	mixer.update( 0 );
	refMixer.update( 0 );
	check( approx( hand2.position.x, 2 ), 'new hand animated by update(0)' );
	check( approx( rigA.hand.position.x, 1 ), 'old skeleton no longer written' );
	check( vertex( rigA.mesh, 0, vA ).distanceTo( animV ) < EPS, 'same pose produces the same vertex on the new skeleton' );
	check( vertex( rigA.mesh, 0, vA ).distanceTo( vertex( rigB.mesh, 0, vB ) ) < EPS, 'vertex matches the reference rig after the swap' );

	mixer.update( 0.25 );
	refMixer.update( 0.25 );
	check( approx( hand2.position.x, 2.5 ), 'animation continues on the new skeleton' );
	check( vertex( rigA.mesh, 0, vA ).distanceTo( vertex( rigB.mesh, 0, vB ) ) < EPS, 'vertex keeps matching the reference rig' );
	check( approx( rigA.hand.position.x, 1 ), 'old skeleton still untouched' );

	action.stop();
	refAction.stop();
	check( approx( hand2.position.x, 1 ), 'new hand restored to its own rest pose' );
	check( vertex( rigA.mesh, 0, vA ).distanceTo( restV ) < EPS, 'vertex restored to the rest pose' );
	check( vertex( rigA.mesh, 0, vA ).distanceTo( vertex( rigB.mesh, 0, vB ) ) < EPS, 'both rigs rest identically' );
	check( vertex( rigA.mesh, 1, vA ).distanceTo( restV1 ) < EPS, 'hip-driven vertex never moved' );

}

export const cases = { transaction, shared_lifecycle, binding_shapes, skinning };
