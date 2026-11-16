/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

#include <GEL/HMesh/obj_save.h>

#include <fstream>
#include <string>
#include <vector>

#include <GEL/CGLA/Vec.h>

#include <GEL/HMesh/Manifold.h>
#include <GEL/HMesh/AttributeVector.h>
#include <GEL/HMesh/AttributedManifold.h>

namespace HMesh
{
    using namespace std;
    using namespace CGLA;

    bool obj_save(const string& filename, Manifold& m)
    {
        ofstream os(filename.data());
        os.precision(20);
        if(os.bad())
            return false;

        VertexAttributeVector<int> vmap;
        int k = 0;
        for(VertexIDIterator v = m.vertices_begin(); v != m.vertices_end(); ++v){
            Vec3d p = m.pos(*v);
            os << "v "<< p[0] << " " << p[1] << " " << p[2] << "\n";
            vmap[*v] = k++;
        }

        for(FaceIDIterator f = m.faces_begin(); f != m.faces_end(); ++f){        
            vector<int> verts;
            for(Walker w = m.walker(*f); !w.full_circle(); w = w.circulate_face_ccw()){
                int idx = vmap[w.vertex()];			
                assert(static_cast<size_t>(idx) < m.no_vertices());
                // move subscript range from 0..size-1 to 1..size according to OBJ standards
                verts.push_back(idx + 1);
            }
			os << "f ";
            for(size_t i = 0; i < verts.size() ; ++i){
                os << verts[i];
                if (i+1==verts.size())
                    break;
                os << " ";
            }
			os<<endl;
        }

        return true;
    }

    bool obj_save(const string& filename, Manifold& m, VertexAttributeVector<Vec3d>& c)
    {
	ofstream os(filename.data());
	os.precision(20);
	if(os.bad())
	    return false;

	VertexAttributeVector<int> vmap;
        int k = 0;
        for(VertexIDIterator v = m.vertices_begin(); v != m.vertices_end(); ++v){
            Vec3d p = m.pos(*v);
	    Vec3d col = c[*v];
            os << "v "<< p[0] << " " << p[1] << " " << p[2];
	    os << " " << col[0] << " " << col[1] << " " << col[2]; 
	    os << "\n";
            vmap[*v] = k++;
        }

        for(FaceIDIterator f = m.faces_begin(); f != m.faces_end(); ++f){
            vector<int> verts;
            for(Walker w = m.walker(*f); !w.full_circle(); w = w.circulate_face_ccw()){
               int idx = vmap[w.vertex()];
               assert(static_cast<size_t>(idx) < m.no_vertices());
               // move subscript range from 0..size-1 to 1..size according to OBJ standards
               verts.push_back(idx + 1);
            }
			os << "f ";
            for(size_t i = 0; i < verts.size() ; ++i){
                os << verts[i];
                if (i+1==verts.size())
                    break;
                os << " ";
            }
			os<<endl;
       }

       return true;
   }

    bool obj_save(const string& filename, AttributedManifold& am)
    {
        Manifold& m = am.mesh;

        ofstream os(filename.data());
        os.precision(20);
        if(!os)
            return false;

        VertexAttributeVector<int> vmap;
        int vcount = 0;
        for(VertexID v : m.vertices()){
            Vec3d p = m.pos(v);
            os << "v " << p[0] << " " << p[1] << " " << p[2] << "\n";
            vmap[v] = ++vcount;
        }

        bool any_t = false, any_n = false;
        for(HalfEdgeID h : m.halfedges()){
            if(m.walker(h).face() == InvalidFaceID)
                continue;
            if(am.has_texcoord[h])
                any_t = true;
            if(am.has_normal[h])
                any_n = true;
        }

        // Independent corner indices: each attributed corner gets its own
        // vt/vn entry, so geometry vertices are not split and the stored
        // attribute values are written verbatim.
        HalfEdgeAttributeVector<int> tmap(-1);
        HalfEdgeAttributeVector<int> nmap(-1);
        int tcount = 0, ncount = 0;
        if(any_t){
            for(FaceID f : m.faces())
                for(Walker w = m.walker(f); !w.full_circle(); w = w.circulate_face_ccw()){
                    HalfEdgeID h = w.halfedge();
                    if(am.has_texcoord[h]){
                        const Vec2d& t = am.texcoords[h];
                        os << "vt " << t[0] << " " << t[1] << "\n";
                        tmap[h] = ++tcount;
                    }
                }
        }
        if(any_n){
            for(FaceID f : m.faces())
                for(Walker w = m.walker(f); !w.full_circle(); w = w.circulate_face_ccw()){
                    HalfEdgeID h = w.halfedge();
                    if(am.has_normal[h]){
                        const Vec3d& n = am.normals[h];
                        os << "vn " << n[0] << " " << n[1] << " " << n[2] << "\n";
                        nmap[h] = ++ncount;
                    }
                }
        }

        for(FaceID f : m.faces()){
            os << "f";
            for(Walker w = m.walker(f); !w.full_circle(); w = w.circulate_face_ccw()){
                HalfEdgeID h = w.halfedge();
                int vi = vmap[w.vertex()];
                bool ht = any_t && am.has_texcoord[h];
                bool hn = any_n && am.has_normal[h];
                os << " " << vi;
                if(ht)
                    os << "/" << tmap[h];
                else if(hn)
                    os << "/";
                if(hn)
                    os << "/" << nmap[h];
            }
            os << "\n";
        }

        return static_cast<bool>(os);
    }

}
