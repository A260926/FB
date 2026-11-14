/*
  Copyright (c) 2009 Erin Catto http://www.box2d.org
  Copyright (c) 2016-2018 Lester Hedges <lester.hedges+aabbcc@gmail.com>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.

  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.

  3. This notice may not be removed or altered from any source distribution.

  This code was adapted from parts of the Box2D Physics Engine,
  http://www.box2d.org
*/

#include "LightScene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace aabb
{
    namespace
    {
        /// The dimensionality of the scene (fixed to 3D).
        const unsigned int DIMENSION = 3;

        //! Compute the tight bounding box of a sphere.
        void sphereBounds(const std::vector<double>& centre, double radius,
                          std::vector<double>& lower, std::vector<double>& upper)
        {
            lower.resize(DIMENSION);
            upper.resize(DIMENSION);
            for (unsigned int i=0;i<DIMENSION;i++)
            {
                lower[i] = centre[i] - radius;
                upper[i] = centre[i] + radius;
            }
        }
    }

    LightScene::LightScene(double skinThickness) :
        skinThickness_(skinThickness),
        objectTree_(DIMENSION, skinThickness, 16, true),
        lightTree_(DIMENSION, skinThickness, 16, true)
    {
        // Validate the skin thickness.
        if (!std::isfinite(skinThickness) || skinThickness < 0.0)
        {
            throw std::invalid_argument("[ERROR]: Invalid skin thickness!");
        }
    }

    void LightScene::validateId(uint32_t id)
    {
        // UINT32_MAX is reserved (it doubles as the tree's null flag).
        if (id == std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("[ERROR]: ID out of range!");
        }
    }

    void LightScene::validateBox(const std::vector<double>& lowerBound,
                                 const std::vector<double>& upperBound)
    {
        // Validate the dimensionality of the bounds vectors.
        if ((lowerBound.size() != DIMENSION) || (upperBound.size() != DIMENSION))
        {
            throw std::invalid_argument("[ERROR]: Dimensionality mismatch!");
        }

        for (unsigned int i=0;i<DIMENSION;i++)
        {
            // Validate that the bounds are finite.
            if (!std::isfinite(lowerBound[i]) || !std::isfinite(upperBound[i]))
            {
                throw std::invalid_argument("[ERROR]: Non-finite AABB bound!");
            }

            // Validate that the box is not inverted.
            if (lowerBound[i] > upperBound[i])
            {
                throw std::invalid_argument("[ERROR]: AABB lower bound is greater than the upper bound!");
            }
        }
    }

    void LightScene::validateSphere(const std::vector<double>& centre, double radius)
    {
        // Validate the dimensionality of the centre vector.
        if (centre.size() != DIMENSION)
        {
            throw std::invalid_argument("[ERROR]: Dimensionality mismatch!");
        }

        // Validate that the centre is finite.
        for (unsigned int i=0;i<DIMENSION;i++)
        {
            if (!std::isfinite(centre[i]))
            {
                throw std::invalid_argument("[ERROR]: Non-finite sphere centre!");
            }
        }

        // Validate that the radius is finite and strictly positive.
        if (!std::isfinite(radius) || (radius <= 0.0))
        {
            throw std::invalid_argument("[ERROR]: Non-positive sphere radius!");
        }
    }

    void LightScene::addObject(uint32_t id, const std::vector<double>& lowerBound,
                               const std::vector<double>& upperBound, uint32_t mask)
    {
        // Validate everything before touching the staged state.
        validateId(id);
        validateBox(lowerBound, upperBound);

        ObjectRec& rec = objects_[id];

        // Make sure the object isn't already alive.
        if (rec.alive)
        {
            throw std::invalid_argument("[ERROR]: Object ID already exists in scene!");
        }

        rec.lower = lowerBound;
        rec.upper = upperBound;
        rec.mask = mask;
        rec.alive = true;

        if (!rec.dirty)
        {
            rec.dirty = true;
            dirtyObjects_.push_back(id);
        }
    }

    void LightScene::addLight(uint32_t id, const std::vector<double>& centre,
                              double radius, uint32_t mask)
    {
        // Validate everything before touching the staged state.
        validateId(id);
        validateSphere(centre, radius);

        LightRec& rec = lights_[id];

        // Make sure the light isn't already alive.
        if (rec.alive)
        {
            throw std::invalid_argument("[ERROR]: Light ID already exists in scene!");
        }

        rec.centre = centre;
        rec.radius = radius;
        rec.mask = mask;
        rec.alive = true;

        if (!rec.dirty)
        {
            rec.dirty = true;
            dirtyLights_.push_back(id);
        }
    }

    void LightScene::updateObject(uint32_t id, const std::vector<double>& lowerBound,
                                  const std::vector<double>& upperBound, uint32_t mask)
    {
        // Validate everything before touching the staged state.
        validateId(id);
        validateBox(lowerBound, upperBound);

        std::unordered_map<uint32_t, ObjectRec>::iterator it = objects_.find(id);

        // The object must be alive.
        if ((it == objects_.end()) || !it->second.alive)
        {
            throw std::invalid_argument("[ERROR]: Invalid object ID!");
        }

        it->second.lower = lowerBound;
        it->second.upper = upperBound;
        it->second.mask = mask;

        if (!it->second.dirty)
        {
            it->second.dirty = true;
            dirtyObjects_.push_back(id);
        }
    }

    void LightScene::updateLight(uint32_t id, const std::vector<double>& centre,
                                 double radius, uint32_t mask)
    {
        // Validate everything before touching the staged state.
        validateId(id);
        validateSphere(centre, radius);

        std::unordered_map<uint32_t, LightRec>::iterator it = lights_.find(id);

        // The light must be alive.
        if ((it == lights_.end()) || !it->second.alive)
        {
            throw std::invalid_argument("[ERROR]: Invalid light ID!");
        }

        it->second.centre = centre;
        it->second.radius = radius;
        it->second.mask = mask;

        if (!it->second.dirty)
        {
            it->second.dirty = true;
            dirtyLights_.push_back(id);
        }
    }

    void LightScene::removeObject(uint32_t id)
    {
        validateId(id);

        std::unordered_map<uint32_t, ObjectRec>::iterator it = objects_.find(id);

        // The object must be alive.
        if ((it == objects_.end()) || !it->second.alive)
        {
            throw std::invalid_argument("[ERROR]: Invalid object ID!");
        }

        it->second.alive = false;

        if (!it->second.dirty)
        {
            it->second.dirty = true;
            dirtyObjects_.push_back(id);
        }
    }

    void LightScene::removeLight(uint32_t id)
    {
        validateId(id);

        std::unordered_map<uint32_t, LightRec>::iterator it = lights_.find(id);

        // The light must be alive.
        if ((it == lights_.end()) || !it->second.alive)
        {
            throw std::invalid_argument("[ERROR]: Invalid light ID!");
        }

        it->second.alive = false;

        if (!it->second.dirty)
        {
            it->second.dirty = true;
            dirtyLights_.push_back(id);
        }
    }

    bool LightScene::testPair(uint32_t lightId, uint32_t objectId) const
    {
        const LightRec& light = lights_.at(lightId);
        const ObjectRec& object = objects_.at(objectId);

        // The masks must share at least one bit.
        if ((light.mask & object.mask) == 0) return false;

        // Distance squared from the sphere centre to the tight box.
        double distance2 = 0.0;
        for (unsigned int i=0;i<DIMENSION;i++)
        {
            if (light.centre[i] < object.lower[i])
            {
                double d = object.lower[i] - light.centre[i];
                distance2 += d * d;
            }
            else if (light.centre[i] > object.upper[i])
            {
                double d = light.centre[i] - object.upper[i];
                distance2 += d * d;
            }
        }

        // Touching counts as intersecting.
        return distance2 <= light.radius * light.radius;
    }

    void LightScene::insertPair(const Pair& pair)
    {
        pairs_.insert(pair);
        objectLights_[pair.objectId].push_back(pair.lightId);
        lightObjects_[pair.lightId].push_back(pair.objectId);
    }

    void LightScene::erasePair(const Pair& pair)
    {
        pairs_.erase(pair);

        std::vector<uint32_t>& lights = objectLights_[pair.objectId];
        lights.erase(std::remove(lights.begin(), lights.end(), pair.lightId), lights.end());

        std::vector<uint32_t>& objects = lightObjects_[pair.lightId];
        objects.erase(std::remove(objects.begin(), objects.end(), pair.objectId), objects.end());
    }

    LightScene::CommitResult LightScene::commit()
    {
        CommitResult result;
        CommitStats stats;

        // Phase 1: bring both broad-phase trees up to date with the final
        // staged geometry of this frame before any candidate is computed.
        // Every staged change marks its endpoint dirty, even when the tree
        // keeps the node inside its old fat AABB (updateParticle == false).
        for (uint32_t id : dirtyObjects_)
        {
            ObjectRec& rec = objects_.at(id);

            if (rec.committed && rec.alive)
                objectTree_.updateParticle(id, rec.lower, rec.upper);
            else if (rec.committed)
                objectTree_.removeParticle(id);
            else if (rec.alive)
                objectTree_.insertParticle(id, rec.lower, rec.upper);
        }
        for (uint32_t id : dirtyLights_)
        {
            LightRec& rec = lights_.at(id);

            std::vector<double> lower, upper;
            if (rec.alive) sphereBounds(rec.centre, rec.radius, lower, upper);

            if (rec.committed && rec.alive)
                lightTree_.updateParticle(id, lower, upper);
            else if (rec.committed)
                lightTree_.removeParticle(id);
            else if (rec.alive)
                lightTree_.insertParticle(id, lower, upper);
        }

        // Phase 2: enumerate the old relations of the dirty endpoints and
        // gather fresh candidates from the opposite trees. Both sides use
        // sets so that pairs touched from both ends appear exactly once.
        std::set<Pair> oldPairs;
        std::set<Pair> candidates;

        for (uint32_t id : dirtyObjects_)
        {
            const ObjectRec& rec = objects_.at(id);

            std::unordered_map<uint32_t, std::vector<uint32_t>>::const_iterator adj =
                objectLights_.find(id);
            if (adj != objectLights_.end())
            {
                for (uint32_t lightId : adj->second)
                {
                    oldPairs.insert(Pair{lightId, id});
                    stats.adjacencyVisits++;
                }
            }

            if (rec.alive)
            {
                AABB queryBox(rec.lower, rec.upper);
                unsigned long long nodeVisits = 0;
                std::vector<unsigned int> hits = lightTree_.query(queryBox, nodeVisits);
                stats.treeNodeVisits += nodeVisits;
                for (uint32_t lightId : hits) candidates.insert(Pair{lightId, id});
            }
        }
        for (uint32_t id : dirtyLights_)
        {
            const LightRec& rec = lights_.at(id);

            std::unordered_map<uint32_t, std::vector<uint32_t>>::const_iterator adj =
                lightObjects_.find(id);
            if (adj != lightObjects_.end())
            {
                for (uint32_t objectId : adj->second)
                {
                    oldPairs.insert(Pair{id, objectId});
                    stats.adjacencyVisits++;
                }
            }

            if (rec.alive)
            {
                std::vector<double> lower, upper;
                sphereBounds(rec.centre, rec.radius, lower, upper);
                AABB queryBox(lower, upper);
                unsigned long long nodeVisits = 0;
                std::vector<unsigned int> hits = objectTree_.query(queryBox, nodeVisits);
                stats.treeNodeVisits += nodeVisits;
                for (uint32_t objectId : hits) candidates.insert(Pair{id, objectId});
            }
        }

        // Phase 3: exact sphere vs. tight box tests on the unique candidates.
        std::set<Pair> passing;
        for (const Pair& pair : candidates)
        {
            stats.pairTests++;
            if (testPair(pair.lightId, pair.objectId)) passing.insert(pair);
        }

        // Phase 4: diff against the committed pair set. Any old relation of
        // a dirty endpoint that is still valid is necessarily rediscovered
        // as a candidate, since fat AABBs always contain the tight geometry.
        for (const Pair& pair : passing)
        {
            if (pairs_.find(pair) == pairs_.end()) result.added.push_back(pair);
        }
        for (const Pair& pair : oldPairs)
        {
            if (passing.find(pair) == passing.end()) result.removed.push_back(pair);
        }

        // Phase 5: publish the new pair set and adjacency lists.
        for (const Pair& pair : result.removed) erasePair(pair);
        for (const Pair& pair : result.added)   insertPair(pair);

        // Phase 6: finalize the endpoint records and clear the dirty lists.
        for (uint32_t id : dirtyObjects_)
        {
            std::unordered_map<uint32_t, ObjectRec>::iterator it = objects_.find(id);
            it->second.committed = it->second.alive;
            it->second.dirty = false;
            if (!it->second.alive)
            {
                // Fully gone: drop the record so a reused ID starts fresh.
                objects_.erase(it);
                objectLights_.erase(id);
            }
        }
        for (uint32_t id : dirtyLights_)
        {
            std::unordered_map<uint32_t, LightRec>::iterator it = lights_.find(id);
            it->second.committed = it->second.alive;
            it->second.dirty = false;
            if (!it->second.alive)
            {
                lights_.erase(it);
                lightObjects_.erase(id);
            }
        }
        dirtyObjects_.clear();
        dirtyLights_.clear();

        lastStats_ = stats;
        result.stats = stats;

        return result;
    }
}
