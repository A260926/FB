/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

#include <GEL/HMesh/AttributedManifold.h>

namespace HMesh
{
    void AttributedManifold::cleanup()
    {
        IDRemap map;
        mesh.cleanup(map);
        texcoords.cleanup(map.hmap);
        normals.cleanup(map.hmap);
        has_texcoord.cleanup(map.hmap);
        has_normal.cleanup(map.hmap);
    }

    void AttributedManifold::clear()
    {
        mesh.clear();
        texcoords.clear();
        normals.clear();
        has_texcoord.clear();
        has_normal.clear();
    }
}
