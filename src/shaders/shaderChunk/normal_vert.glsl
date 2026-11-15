#ifndef FLAT_SHADED
    mat4 modelMatrix = getModelMatrix();
    v_Normal = (transposeMat4(inverseMat4(modelMatrix)) * vec4(objectNormal, 0.0)).xyz;

    #ifdef FLIP_SIDED
    	v_Normal = - v_Normal;
    #endif

    #ifdef USE_TANGENT
        v_Tangent = (transposeMat4(inverseMat4(modelMatrix)) * vec4(objectTangent, 0.0)).xyz;

        #ifdef FLIP_SIDED
            v_Tangent = - v_Tangent;
        #endif

        v_Bitangent = normalize(cross(v_Normal, v_Tangent) * a_Tangent.w);
    #endif
#endif
