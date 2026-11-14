//
//   Copyright 2014 DreamWorks Animation LLC.
//
//   Licensed under the Apache License, Version 2.0 (the "Apache License")
//   with the following modification; you may not use this file except in
//   compliance with the Apache License and the following modification to it:
//   Section 6. Trademarks. is deleted and replaced with:
//
//   6. Trademarks. This License does not grant permission to use the trade
//      names, trademarks, service marks, or product names of the Licensor
//      and its affiliates, except as required to comply with Section 4(c) of
//      the License and to reproduce the content of the NOTICE file.
//
//   You may obtain a copy of the Apache License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the Apache License with the above modification is
//   distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
//   KIND, either express or implied. See the Apache License for the specific
//   language governing permissions and limitations under the Apache License.
//
#ifndef OPENSUBDIV3_FAR_TOPOLOGY_REFINER_H
#define OPENSUBDIV3_FAR_TOPOLOGY_REFINER_H

#include "../version.h"

#include "../sdc/types.h"
#include "../sdc/options.h"
#include "../far/types.h"
#include "../far/topologyLevel.h"

#include <vector>


namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Vtr { namespace internal { class SparseSelector; } }
namespace Far { namespace internal { class FeatureMask; } }

namespace Far {

template <typename REAL> class PrimvarRefinerReal;
template <class MESH> class TopologyRefinerFactory;

///
///  \brief Stores topology data for a specified set of refinement options.
///
class TopologyRefiner {

public:

    /// \brief Constructor
    TopologyRefiner(Sdc::SchemeType type, Sdc::Options options = Sdc::Options());

    /// \brief Destructor
    ~TopologyRefiner();

    /// \brief Returns the subdivision scheme
    Sdc::SchemeType GetSchemeType() const    { return _subdivType; }

    /// \brief Returns the subdivision options
    Sdc::Options GetSchemeOptions() const { return _subdivOptions; }

    /// \brief Returns true if uniform refinement has been applied
    bool IsUniform() const   { return _isUniform; }

    /// \brief Returns the number of refinement levels
    int  GetNumLevels() const { return (int)_farLevels.size(); }

    /// \brief Returns the highest level of refinement
    int  GetMaxLevel() const { return _maxLevel; }

    /// \brief Returns the maximum vertex valence in all levels
    int  GetMaxValence() const { return _maxValence; }

    /// \brief Returns true if faces have been tagged as holes
    bool HasHoles() const { return _hasHoles; }

    /// \brief Returns the total number of vertices in all levels
    int GetNumVerticesTotal() const { return _totalVertices; }

    /// \brief Returns the total number of edges in all levels
    int GetNumEdgesTotal() const { return _totalEdges; }

    /// \brief Returns the total number of edges in all levels
    int GetNumFacesTotal() const { return _totalFaces; }

    /// \brief Returns the total number of face vertices in all levels
    int GetNumFaceVerticesTotal() const { return _totalFaceVertices; }

    /// \brief Returns a handle to access data specific to a particular level
    TopologyLevel const & GetLevel(int level) const { return _farLevels[level]; }

    //@{
    ///  @name High-level refinement and related methods
    ///

    //
    // Uniform refinement
    //

    /// \brief Uniform refinement options
    ///
    /// Options for uniform refinement, including the number of levels, vertex
    /// ordering and generation of topology information.
    ///
    /// Note the impact of the option to generate fullTopologyInLastLevel.  Given
    /// subsequent levels of uniform refinement typically reguire 4x the data
    /// of the previous level, only the minimum amount of data is generated in the
    /// last level by default, i.e. a vertex and face-vertex list.  If requiring
    /// topology traversal of the last level, e.g. inspecting edges or incident
    /// faces of vertices, the option to generate full topology in the last
    /// level should be enabled.
    ///
    struct UniformOptions {

        UniformOptions(int level) :
            refinementLevel(level & 0xf),
            orderVerticesFromFacesFirst(false),
            fullTopologyInLastLevel(false) { }

        /// \brief Set uniform refinement level
        void SetRefinementLevel(int level) { refinementLevel = level & 0xf; }

        unsigned int refinementLevel:4,             ///< Number of refinement iterations
                     orderVerticesFromFacesFirst:1, ///< Order child vertices from faces first
                                                    ///< instead of child vertices of vertices
                     fullTopologyInLastLevel:1;     ///< Skip topological relationships in the last
                                                    ///< level of refinement that are not needed for
                                                    ///< interpolation (keep false if using limit).
    };

    /// \brief Refine the topology uniformly
    ///
    /// This method applies uniform refinement to the level specified in the
    /// given UniformOptions.
    ///
    /// Note the impact of the UniformOption to generate fullTopologyInLastLevel
    /// and be sure it is assigned to satisfy the needs of the resulting refinement.
    ///
    /// @param options   Options controlling uniform refinement
    ///
    void RefineUniform(UniformOptions options);

    /// \brief Returns the options specified on refinement
    UniformOptions GetUniformOptions() const { return _uniformOptions; }

    //
    // Adaptive refinement
    //

    /// \brief Adaptive refinement options
    struct AdaptiveOptions {

        AdaptiveOptions(int level) :
            isolationLevel(level & 0xf),
            secondaryLevel(0xf),
            useSingleCreasePatch(false),
            useInfSharpPatch(false),
            considerFVarChannels(false),
            orderVerticesFromFacesFirst(false) { }

        /// \brief Set isolation level
        void SetIsolationLevel(int level) { isolationLevel = level & 0xf; }

        /// \brief Set secondary isolation level
        void SetSecondaryLevel(int level) { secondaryLevel = level & 0xf; }

        unsigned int isolationLevel:4;              ///< Number of iterations applied to isolate
                                                    ///< extraordinary vertices and creases
        unsigned int secondaryLevel:4;              ///< Shallower level to stop isolation of
                                                    ///< smooth irregular features
        unsigned int useSingleCreasePatch:1;        ///< Use 'single-crease' patch and stop
                                                    ///< isolation where applicable
        unsigned int useInfSharpPatch:1;            ///< Use infinitely sharp patches and stop
                                                    ///< isolation where applicable
        unsigned int considerFVarChannels:1;        ///< Inspect face-varying channels and
                                                    ///< isolate when irregular features present
        unsigned int orderVerticesFromFacesFirst:1; ///< Order child vertices from faces first
                                                    ///< instead of child vertices of vertices
    };

    /// \brief Feature Adaptive topology refinement
    ///
    /// @param options         Options controlling adaptive refinement
    ///
    /// @param selectedFaces   Limit adaptive refinement to the specified faces
    ///
    void RefineAdaptive(AdaptiveOptions options,
                        ConstIndexArray selectedFaces = ConstIndexArray());

    /// \brief Returns the options specified on refinement
    AdaptiveOptions GetAdaptiveOptions() const { return _adaptiveOptions; }

    /// \brief Unrefine the topology, keeping only the base level.
    void Unrefine();

    //
    // In-place editing of coarse-level sharpness
    //

    /// \brief Descriptor for a single sharpness value to assign to an edge or a vertex
    ///        of the base level.
    ///
    /// Values are absolute: 0 is smooth and Sdc::Crease::SHARPNESS_INFINITE (10)
    /// is infinitely sharp.  Indices are the indices of the corresponding edge or
    /// vertex in the base level (level 0) of the refined topology.
    ///
    struct SharpnessEdit {
        SharpnessEdit() : index(0), sharpness(0.0f) { }
        SharpnessEdit(Index indexArg, float sharpnessArg)
            : index(indexArg), sharpness(sharpnessArg) { }

        Index index;        ///< Index of the edge or vertex in the base level
        float sharpness;    ///< Absolute sharpness value in the range [0, 10]
    };

    /// \brief Updates the sharpness values assigned to edges and vertices of the
    ///        base level and propagates them through the existing refinement
    ///        hierarchy in place.
    ///
    /// The refined topology is not modified:  component counts, indices, parent/child
    /// mappings and all topological relations in every level are left intact, so
    /// previously obtained TopologyLevel views remain valid.  Only the sharpness
    /// values and the derived crease classifications (component tags and vertex
    /// rules) are recomputed in each existing level -- from the base level down
    /// through the final level.  Interpolation performed afterwards by
    /// PrimvarRefiner and StencilTables created afterwards will reflect the new
    /// shape.
    ///
    /// Edits are absolute and cumulative:  a subsequent call modifies the values
    /// left by the most recent successful call, so sharpness may be lowered as well
    /// as raised.  Indices in each list must be unique and the lists themselves must
    /// contain valid base-level edges/vertices with finite sharpness in [0, 10]; if
    /// any entry is invalid, false is returned and nothing is modified.  Boundary
    /// edges are sharpened by the existing boundary interpolation rules regardless
    /// of the values assigned here, as the topology does not change.
    ///
    /// This is only supported for uniformly refined refiners (RefineUniform()) with
    /// at least one refinement level, complete topology in all levels, no holes, no
    /// face-varying channels, and no shared base level.  False is returned otherwise.
    ///
    /// @param edges      Edges of the base level whose sharpness is to be updated
    /// @param vertices   Vertices of the base level whose sharpness is to be updated
    ///
    /// @return           True if the edit was applied, false if it was rejected
    ///
    bool UpdateUniformSharpness(std::vector<SharpnessEdit> const & edges,
                                std::vector<SharpnessEdit> const & vertices);


    //@{
    /// @name Number and properties of face-varying channels:
    ///

    /// \brief Returns the number of face-varying channels in the tables
    int GetNumFVarChannels() const;

    /// \brief Returns the face-varying interpolation rule set for a given channel
    Sdc::Options::FVarLinearInterpolation GetFVarLinearInterpolation(int channel = 0) const;

    /// \brief Returns the total number of face-varying values in all levels
    int GetNumFVarValuesTotal(int channel = 0) const;

    //@}

protected:

    //
    //  Lower level protected methods intended strictly for internal use:
    //
    template <class MESH>
    friend class TopologyRefinerFactory;
    friend class TopologyRefinerFactoryBase;
    friend class PatchTableBuilder;
    friend class PatchBuilder;
    friend class PtexIndices;
    template <typename REAL>
    friend class PrimvarRefinerReal;

    //  Copy constructor exposed via the factory class:
    TopologyRefiner(TopologyRefiner const & source);

public:
    //  Levels and Refinements available internally (avoids need for more friends)
    Vtr::internal::Level & getLevel(int l) { return *_levels[l]; }
    Vtr::internal::Level const & getLevel(int l) const { return *_levels[l]; }

    Vtr::internal::Refinement & getRefinement(int l) { return *_refinements[l]; }
    Vtr::internal::Refinement const & getRefinement(int l) const { return *_refinements[l]; }

private:
    //  Not default constructible or copyable:
    TopologyRefiner() : _uniformOptions(0), _adaptiveOptions(0) { }
    TopologyRefiner & operator=(TopologyRefiner const &) { return *this; }

    void selectFeatureAdaptiveComponents(Vtr::internal::SparseSelector& selector,
                                         internal::FeatureMask const & mask,
                                         ConstIndexArray selectedFaces);
    void selectLinearIrregularFaces(Vtr::internal::SparseSelector& selector,
                                    ConstIndexArray selectedFaces);

    void initializeInventory();
    void updateInventory(Vtr::internal::Level const & newLevel);

    void updateBaseLevelSharpnessAndTags();

    void appendLevel(Vtr::internal::Level & newLevel);
    void appendRefinement(Vtr::internal::Refinement & newRefinement);
    void assembleFarLevels();

private:

    Sdc::SchemeType _subdivType;
    Sdc::Options    _subdivOptions;

    unsigned int _isUniform     : 1;
    unsigned int _hasHoles      : 1;
    unsigned int _hasIrregFaces : 1;
    unsigned int _regFaceSize   : 3;
    unsigned int _maxLevel      : 4;

    //  Options assigned on refinement:
    UniformOptions  _uniformOptions;
    AdaptiveOptions _adaptiveOptions;

    //  Cumulative properties of all levels:
    int _totalVertices;
    int _totalEdges;
    int _totalFaces;
    int _totalFaceVertices;
    int _maxValence;

    //  Note the base level may be shared with another instance
    bool _baseLevelOwned;

    std::vector<Vtr::internal::Level *>      _levels;
    std::vector<Vtr::internal::Refinement *> _refinements;

    std::vector<TopologyLevel> _farLevels;
};


inline int
TopologyRefiner::GetNumFVarChannels() const {

    return _levels[0]->getNumFVarChannels();
}
inline Sdc::Options::FVarLinearInterpolation
TopologyRefiner::GetFVarLinearInterpolation(int channel) const {

    return _levels[0]->getFVarOptions(channel).GetFVarLinearInterpolation();
}

} // end namespace Far

} // end namespace OPENSUBDIV_VERSION
using namespace OPENSUBDIV_VERSION;
} // end namespace OpenSubdiv

#endif /* OPENSUBDIV3_FAR_TOPOLOGY_REFINER_H */
