This demo contains sample code for loading the IQM format. It requires GLUT or freeglut to build and run, and should compile on any platform for which they are available with the included Makefile. On Windows, you can just run the pre-built demo.exe and gpu-demo.exe. 

The GPU Skinning demo (gpu-demo) shows an example of how one can animate meshes stored on the GPU in vertex buffer objects using GLSL shaders.

The demos are run as follows:

./demo [options] [file.iqm] [anim.iqm]
./gpu-demo [options] [file.iqm] [anim.iqm]

If no "file.iqm" is specified, then "mrfixit.iqm" is loaded by default. If "file.iqm" contains animations, these animations will be used. If an extra "anim.iqm" is specified containing animations, these animations will be used instead along with the meshes in "file.iqm".

Options can be any of the following command-line switches:

-s N
  Displays the model with scale N.

-r N
  Rotates the model by N degrees.

-t Z
-t X,Y,Z
  Translates model by X,Y,Z units.


*** Background CPU animation (async_pose.h / async_pose.cpp) ***

The "asyncpose" service moves the CPU skinning of animateiqm() onto
background worker threads.  Many characters can share one read-only model
asset while the render thread always reads a complete, immutable frame
(snapshot).  It needs no GL, GLUT or display; see example/async_pose.cpp
for a runnable CPU-only example.  The classic serial animateiqm() entry
keeps working and shares the exact same sampling + skinning implementation
(asyncpose::sample_and_skin) with the service.

The pieces, and how to use them:

1. Load a read-only asset (before submitting anything):

     std::shared_ptr<const asyncpose::PoseAsset> asset =
         asyncpose::load_pose_asset("model.iqm");

   The asset owns copies of all model data; it never borrows the file
   buffer after loading.  One asset serves any number of characters.
   Returns an empty shared_ptr on failure.

2. Create the service (thread scheduling):

     asyncpose::PoseService service(0); // 0 = default, at least 2 workers

   Workers block when idle and at least two can evaluate concurrently.
   The request queue holds at most one not-yet-started request per
   character, so it stays bounded under continuous submission.

3. Create a character (its own evaluation context):

     asyncpose::CharacterId id = service.create_character(asset);

4. Submit an absolute float frame number:

     asyncpose::Ticket ticket = service.submit(id, 1.5f);

   Returns the character's strictly increasing ticket, or 0 if the request
   was rejected (bad id, unregistered character, non-finite frame, service
   shut down); a rejection changes nothing.  Frames may be negative and
   loop with a positive modulo over the whole animation frame array.  A
   new submit replaces the character's single not-yet-started request and
   settles the replaced ticket as superseded.  A request already being
   evaluated only updates the snapshot if it is still the character's
   latest accepted ticket when it finishes, so a late old request never
   overwrites a newer frame.

5. Wait for a request:

     std::shared_ptr<const asyncpose::PoseSnapshot> snap;
     asyncpose::RequestStatus st = service.wait(id, ticket, &snap);

   Returns published / superseded / failed (or invalid for unknown ids or
   tickets).  published means the result was installed as the character's
   snapshot, and snap receives that exact frame.  Every accepted ticket is
   guaranteed to reach a terminal state, so wait never blocks forever.
   service.poll(id, ticket) is the non-blocking variant.

6. Get the current snapshot:

     auto current = service.snapshot(id); // nullptr until first publish

   Snapshots carry the ticket, frame number, bone matrices, positions,
   normals, tangents and bitangents of one request.  They are immutable;
   readers may hold the shared_ptr and keep reading after further submits,
   after unregistration and after service shutdown.

7. Unregister a character:

     service.unregister_character(id);

   Settles the character's unpublished requests as superseded and rejects
   further submissions for that id.  Ids are never reused, so a stale id
   cannot address another character.

8. Shut the service down:

     service.shutdown();

   Rejects new submissions, settles queued requests as superseded, wakes
   all waiters, lets in-flight evaluations finish (they may still publish),
   then joins the workers.  Idempotent; the destructor does the same.
   Call it from outside the worker threads.

Optionally, service.set_eval_callback(fn) installs a callback
fn(CharacterId, Ticket) that runs on a worker thread after the
four-attribute computation completes and before the publish check, without
internal locks held.  It exists to deterministically pause stale results
(testing/tooling); normal operation leaves it unset.  A throwing callback
marks that request failed and does not kill the worker.  The callback may
read the current snapshot but must not shut the service down or wait on
its own ticket.


*** LICENSE INFO ***

The demo source code is licensed as specified by the LICENSE file included with this distribution.

For GL/ and freeglut file licenses, please refer to the licenses included within the files. 

The "mrfixit.iqm" model (and "Head.tga"/"Body.tga" skins) is subject to the following license:

Mr. Fixit conceptualized, modeled and animated by geartrooper a.k.a. John Siar ironantknight@gmail.com
Mr. Fixit textures, normal maps and displacement by Acord a.k.a Anthony Cord tonycord@gmail.com
mask textures by Nieb

Mr. Fixit is released under the CC-BY-NC (Creative Commons Attribution Non-Commercial) license. See http://creativecommons.org/licenses/by-nc/3.0/legalcode for more info. Please contact Geartrooper for permission for uses not within the scope of this license.


