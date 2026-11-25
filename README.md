libanim
=======

About
-----
Libanim is a C animation library, which can be used as a generic framework to
add keyframe interpolation tracks for any single-valued or 3-vector parameters,
or at a slightly higher level as a hierarchical PRS (position/rotation/scaling)
animation node framework for 3D graphics programs.

Version 2 of libanim dropped the dependency to libvmath, and instead carries a
copy of gph-cmath (https://github.com/jtsiomb/gph-cmath) internally. The API
has been reworked to avoid forcing a dependency to any math library to the user
program, relying on floats and float pointers instead, which can be aliased to
any kind contiguous `x,y,z` vector and `x,y,z,w` quaternion, or simple arrays
of floats. Matrix arguments are expected to be arrays of 16 contiguous floats,
in OpenGL-compatible order.

Programs written for earlier versions of libanim, and using the high-level PRS
interface in `anim.h` are not source-compatible, nor binary-compatible with
libanim 2. Though in practice the API changes are minor, and porting should be
straightforward. Programs using only the low-level keyframe tracks in `track.h`
are unaffected by these changes.

License
-------
Copyright (C) 2012-2024 John Tsiombikas <nuclear@member.fsf.org>

This program is free software. You may use, modify, and redistribute it under
the terms of the GNU Lesser General Public License v3 or (at your option), any
later version published by the Free Software Foundation. See COPYING and
COPYING.LESSER for details.

Usage
-----
libanim has two levels you can use: the low level keyframe tracks API in
`track.h`, and the high level animation node system in `anim.h`.

### Track interface
`track.h` is all about the basic "set keyframe", "evaluate at time t",
"interpolation/extrapolation" operations, and it works on single valued
keyframe tracks, just time/value pairs. The idea is that to
animate a position, you'd have a higher level abstraction which sets keyframes
to three tracks (xyz), and evaluate those three tracks to figure out the
position at time t. 

#### Quaternions
The above works great for position and scale, but rotation quaternions need
special interpolation (slerp). This used to be handled by the higher level
interface initially, but since the node animation interface is not always
appropriate for all projects, and it's useful to be able to use the low level
interface only, version 2.0 adds `anm_get_quat` which takes 4 tracks, treats
them as representing a rotation quaternion over time, and interpolates
accordingly using slerp.

### Animation node interface
Initially this was supposed to be "the" API for this library, but over time it
became apparent that it contains abstractions which the application *might*
want to handle by itself, and have them integrated with the rest of its scene
management. So it doesn't necessarilly fit into every application.

This interface is centered around the `anm_node` structure. `anm_node`
represents a hierarchical transformation node which can be part of a 3D object.
Calling `anm_set_position`, `anm_set_rotation` and so on, automatically sets
keyframes to the "default animation" used by that node (you can have multiple
animations and switch/blend between them). And then you can just call
`anm_get_matrix` for a specific time, which will evaluate all 10 tracks for
that time, combine position/rotation/scaling, and give you back a matrix. The
matrix will be cached, so if you call multiple times for the same time, it
won't have to be re-evaluated. If it's a child node, it will recurse up,
evaluate its parent, and combine that matrix too. There is also a
`anm_get_node_matrix` function, if you don't want to take hierarchy into
account.

The animation node interface is pretty useful for a wide range of applications,
but if it doesn't fit your design, just ignore it altogether and use the low
level track API directly (`track.h`).

### Shared curves and character snapshots
`anm_share_track(dest, src)` lets two initialized tracks reference the same
keyframe buffer instead of copying every key. It returns 0 on success, or -1
leaving `dest` untouched (for instance when one side is uninitialized); sharing
a track with itself succeeds and does nothing. After sharing, the `keys` pointer
of a non-empty track is the same in both tracks, while the name, default value,
interpolator and extrapolator stay independent per track. The buffer is
reference counted and freed when the last holder destroys its track.

Writes through `anm_set_keyframe` / `anm_set_value` (and the higher level
`anm_set_position`, `anm_set_scaling`, ... setters) are copy-on-write: the first
write to a shared track gives that track a private copy of the keys, so the
other sharers keep their key order and sampled values. `anm_copy_track` remains
a full deep copy: for a non-empty source the destination always gets a distinct
key buffer.

`anm_clone_node_tree(src)` takes a snapshot of a whole character subtree (the
root and all descendants, but not the root's siblings). Node and animation
names, every animation slot, track settings, pivots, the two active animations
with their offsets/mix, and any in-progress transition are copied; the `data`
pointer is copied by value (borrowed, never owned). All keyframe buffers are
shared, not copied, so a snapshot is cheap even for characters with dense
animation. Mutating, re-snapshotting or destroying either tree never affects the
other, and the snapshot keeps sampling correctly even after the source tree has
been destroyed. The clone starts with a cold matrix cache, and it must be
created, edited and released from a single thread (concurrent editing of shared
curves is not supported). On allocation failure the partially built tree is
freed and the source is left untouched.

**Important limitation:** copy-on-write only triggers through the track editing
functions above. Writing directly through the public `struct anm_track` /
`struct anm_node` fields, or through a `struct anm_keyframe *` returned by
`anm_get_keyframe`, bypasses the detach step and will corrupt the other
sharers' curves. Those pointers are for reading only.

`anm_share_track`, `anm_copy_track` and `anm_clone_node_tree` can be mixed
freely; a key buffer is released by whichever holder is destroyed last.

#### ABI note
Sharing required adding a `keys_refcount` member to `struct anm_track` (and the
node clone relies on the cache fields already present in `struct anm_node`).
Public function signatures are unchanged, but the layout of `struct anm_track`
changed, so code built against an older libanim header must be recompiled
against the new one. Programs that only call the API need no source changes.

Build
-----
To build and install libanim on UNIX, run the usual:

    ./configure
    make
    make install

See `./configure --help` for a complete list of build-time options.

`example/clone-cow-cpu.c` is a headless demo of the shared-curve snapshot path
(no GLUT/OpenGL needed): it builds a two-bone character, snapshots it, plays the
two copies at different times, edits one copy, destroys the source, and skins a
vertex through the surviving copy. It prints `CLONE_COW_DEMO_OK` on success.

`tests/clone_cow.c` exercises the same functionality in the three groups
`sharing`, `playback` and `lifetime`; it is built against the normal and the
`ANIM_THREAD_SAFE` configurations.

To cross-compile for windows with mingw-w64, try the following incantation:

    ./configure --prefix=/usr/i686-w64-mingw32
    make CC=i686-w64-mingw32-gcc AR=i686-w64-mingw32-ar sys=mingw
    make install sys=mingw
