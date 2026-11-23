/*
 * Poly2Tri Copyright (c) 2009-2018, Poly2Tri Contributors
 * https://github.com/jhasse/poly2tri
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * * Neither the name of Poly2Tri nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without specific
 *   prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "advancing_front.h"
#include "constraint_editor.h"
#include "sweep_context.h"
#include "sweep.h"

#include "../common/dll_symbol.h"

/**
 *
 * @author Mason Green <mason.green@gmail.com>
 *
 */

namespace p2t {

class P2T_DLL_SYMBOL CDT
{
public:

  /**
   * Handle of a user-inserted dynamic constraint.  Zero never identifies a
   * live constraint and successful ids are never reused while the CDT lives.
   */
  using ConstraintId = std::uint64_t;

  /**
   * Constructor - add polyline with non repeating points
   *
   * @param polyline
   */
  CDT(const std::vector<Point*>& polyline);

   /**
   * Destructor - clean up memory
   */
  ~CDT();

  /**
   * Add a hole
   *
   * @param polyline
   */
  void AddHole(const std::vector<Point*>& polyline);

  /**
   * Add a steiner point
   *
   * @param point
   */
  void AddPoint(Point* point);

  /**
   * Triangulate - do this AFTER you've added the polyline, holes, and Steiner points
   */
  void Triangulate();

  /**
   * Insert a dynamic crease line after Triangulate().
   *
   * Both endpoints must be distinct Point objects already used by this
   * triangulation; null pointers, foreign Point objects and calls before
   * Triangulate() return 0.  A line that leaves the outer contour, enters a
   * hole or properly crosses a protected edge where no vertex exists is
   * rejected as a whole, again returning 0 and leaving the mesh unchanged.
   * On success the line is split at mesh vertices lying on it, every piece
   * becomes a constrained edge and a non-zero id is returned.
   */
  ConstraintId InsertConstraint(Point* a, Point* b);

  /**
   * Remove the constraint previously returned by InsertConstraint().
   *
   * Returns false for 0, unknown ids and ids that were already removed.
   * Sub-edges still shared with another live constraint stay protected;
   * freed edges rejoin the local Delaunay legalization.
   */
  bool RemoveConstraint(ConstraintId id);

  /**
   * Get CDT triangles
   */
  std::vector<Triangle*> GetTriangles();

  /**
   * Get triangle map
   */
  std::list<Triangle*> GetMap();

  private:

  /**
   * Internals
   */

  SweepContext* sweep_context_;
  Sweep* sweep_;
  ConstraintEditor* constraint_editor_;

};

}
