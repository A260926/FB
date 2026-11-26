#include "SkeletalModelSerializer.h"

#include "SkeletalModel.h"

namespace SA
{
	// Legacy writers emitted the fixed header (1, 2, 3) followed by meshes that
	// carried no binding information. New writers emit this explicit version as
	// the first word; anything else is treated as the legacy format.
	static const int FORMAT_VERSION = 2;
	static const int LEGACY_HEADER_A = 1;
	static const int LEGACY_HEADER_B = 2;
	static const int LEGACY_HEADER_C = 3;

	// Helper function used for reading and writing
	template <typename _Ty>
	static void WriteBinary(unsigned int& a_InOutPos, char* a_pDestination, const _Ty& a_Source)
	{
		if (a_pDestination != NULL)
		{
			memcpy(a_pDestination + a_InOutPos, &a_Source, sizeof(_Ty));
		}
		a_InOutPos += sizeof(_Ty);
	}

	template <typename _Ty>
	static void ReadBinary(unsigned int& a_InOutPos, _Ty& a_Destination, const char* a_pSource)
	{
		memcpy(&a_Destination, a_pSource + a_InOutPos, sizeof(_Ty));
		a_InOutPos += sizeof(_Ty);
	}





	template <typename _Ty>
	static void WriteBinary(unsigned int& a_InOutPos, char* a_pDestination, const _Ty* a_Source, unsigned int a_NumElements)
	{
		if (a_pDestination != NULL)
		{
			memcpy(a_pDestination + a_InOutPos, a_Source, sizeof(_Ty) * a_NumElements);
		}
		a_InOutPos += sizeof(_Ty) * a_NumElements;
	}

	template <typename _Ty>
	static void ReadBinary(unsigned int& a_InOutPos, _Ty*& a_pDestination, const char* a_pSource, unsigned int a_NumElements)
	{
		a_pDestination = NULL;
		if (a_NumElements > 0)
		{
			a_pDestination = new _Ty[a_NumElements];
			memcpy(a_pDestination, a_pSource + a_InOutPos, sizeof(_Ty) * a_NumElements);
			a_InOutPos += sizeof(_Ty) * a_NumElements;
		}
	}





	static void WriteString(unsigned int& a_InOutPos, char* a_pDestination, const std::string& a_Source)
	{
		unsigned int StringLength = a_Source.size();
		WriteBinary(a_InOutPos, a_pDestination, StringLength);
		WriteBinary(a_InOutPos, a_pDestination, a_Source.c_str(), StringLength);
	}

	static void ReadString(unsigned int& a_InOutPos, std::string& a_Destination, const char* a_pSource)
	{
		unsigned int StringLength = 0;
		ReadBinary(a_InOutPos, StringLength, a_pSource);
		if (StringLength > 0)
		{
			// Build a string of exactly the stored length; leaving out the
			// terminator prevents trailing characters from leaking into names.
			a_Destination.assign(a_pSource + a_InOutPos, StringLength);
			a_InOutPos += StringLength;
		}
		else
		{
			a_Destination.clear();
		}
	}





	template <typename _Ty>
	static void WriteKeys(unsigned int& a_InOutPos, char* a_pDestination, const std::vector<sNodeAnimationKey<_Ty> >& a_Source)
	{
		unsigned int NumKeys = a_Source.size();
		WriteBinary(a_InOutPos, a_pDestination, NumKeys);
		for (unsigned int i = 0; i < NumKeys; ++i)
		{
			WriteBinary(a_InOutPos, a_pDestination, a_Source[i].Time);
			WriteBinary(a_InOutPos, a_pDestination, a_Source[i].Value);
		}
	}

	template <typename _Ty>
	static void ReadKeys(unsigned int& a_InOutPos, std::vector<sNodeAnimationKey<_Ty> >& a_Destination, const char* a_pSource)
	{
		unsigned int NumKeys = 0;
		ReadBinary(a_InOutPos, NumKeys, a_pSource);
		for (unsigned int i = 0; i < NumKeys; ++i)
		{
			sNodeAnimationKey<_Ty> PosKey;
			ReadBinary(a_InOutPos, PosKey.Time, a_pSource);
			ReadBinary(a_InOutPos, PosKey.Value, a_pSource);
			a_Destination.push_back(PosKey);
		}
	}






	static void WriteMeshBindings(unsigned int& a_InOutPos, char* a_pDestination, const sAnimatedMesh& a_Mesh)
	{
		// Write the flag and the table even when the table is empty so the
		// static-mesh state survives a round trip. The flag is stored as an
		// unsigned int so the byte layout does not depend on sizeof(bool).
		unsigned int UseMeshBindings = a_Mesh.UseMeshBindings ? 1u : 0u;
		WriteBinary(a_InOutPos, a_pDestination, UseMeshBindings);

		unsigned int NumBindings = a_Mesh.MeshBindings.size();
		WriteBinary(a_InOutPos, a_pDestination, NumBindings);
		for (unsigned int i = 0; i < NumBindings; ++i)
		{
			const sMeshBoneBinding& Binding = a_Mesh.MeshBindings[i];
			WriteBinary(a_InOutPos, a_pDestination, Binding.BoneIndex);
			WriteBinary(a_InOutPos, a_pDestination, Binding.OffsetMatrix);

			unsigned int NumWeights = Binding.Weights.size();
			WriteBinary(a_InOutPos, a_pDestination, NumWeights);
			for (unsigned int w = 0; w < NumWeights; ++w)
			{
				WriteBinary(a_InOutPos, a_pDestination, Binding.Weights[w].VertexID);
				WriteBinary(a_InOutPos, a_pDestination, Binding.Weights[w].Weight);
			}
		}
	}






	static void ReadMeshBindings(unsigned int& a_InOutPos, sAnimatedMesh& a_Mesh, const char* a_pSource)
	{
		unsigned int UseMeshBindings = 0;
		ReadBinary(a_InOutPos, UseMeshBindings, a_pSource);
		a_Mesh.UseMeshBindings = (UseMeshBindings != 0);

		unsigned int NumBindings = 0;
		ReadBinary(a_InOutPos, NumBindings, a_pSource);
		a_Mesh.MeshBindings.clear();
		a_Mesh.MeshBindings.resize(NumBindings);
		for (unsigned int i = 0; i < NumBindings; ++i)
		{
			sMeshBoneBinding& Binding = a_Mesh.MeshBindings[i];
			ReadBinary(a_InOutPos, Binding.BoneIndex, a_pSource);
			ReadBinary(a_InOutPos, Binding.OffsetMatrix, a_pSource);

			unsigned int NumWeights = 0;
			ReadBinary(a_InOutPos, NumWeights, a_pSource);
			// Copy every weight out of the input bytes rather than aliasing them.
			Binding.Weights.resize(NumWeights);
			for (unsigned int w = 0; w < NumWeights; ++w)
			{
				ReadBinary(a_InOutPos, Binding.Weights[w].VertexID, a_pSource);
				ReadBinary(a_InOutPos, Binding.Weights[w].Weight, a_pSource);
			}
		}
	}





	extern std::string ModelToData(const SkeletalModel& a_Model)
	{
		std::string OutData;
		ModelToData(a_Model, OutData);
		return OutData;
	}





	extern bool ModelToData(const SkeletalModel& a_Model, std::string& a_OutData)
	{
		unsigned int DataSize = ModelToData(a_Model, NULL);
		a_OutData.resize(DataSize);
		return ModelToData(a_Model, (char*)a_OutData.data()) > 0;
	}





	unsigned int ModelToData(const SkeletalModel& a_Model, char* a_pOutData)
	{
		unsigned int DataPos = 0;

		WriteBinary(DataPos, a_pOutData, FORMAT_VERSION);

		// Write meshes
		unsigned int NumMeshes = a_Model.GetNumMeshes();
		WriteBinary(DataPos, a_pOutData, NumMeshes);
		for (unsigned int i = 0; i < NumMeshes; ++i)
		{
			const sAnimatedMesh& Mesh = a_Model.GetMesh(i);

			// Write vertices and normals
 			WriteBinary(DataPos, a_pOutData, Mesh.NumVertices);
 			WriteBinary(DataPos, a_pOutData, Mesh.pVertices, Mesh.NumVertices);
 			WriteBinary(DataPos, a_pOutData, Mesh.pNormals, Mesh.NumVertices);

			// Write indices
			WriteBinary(DataPos, a_pOutData, Mesh.NumIndices);
			WriteBinary(DataPos, a_pOutData, Mesh.pIndices, Mesh.NumIndices);

			// Write mesh-local bone bindings
			WriteMeshBindings(DataPos, a_pOutData, Mesh);
		}

		// Write skeleton
		unsigned int NumBones = a_Model.GetSkeleton().Bones.size();
		WriteBinary(DataPos, a_pOutData, NumBones);
		for (unsigned int i = 0; i < NumBones; ++i)
		{
			const sBone& Bone = a_Model.GetSkeleton().Bones[i];

			// Write bone name
			WriteString(DataPos, a_pOutData, Bone.Name);

			// Write matrices
			WriteBinary(DataPos, a_pOutData, Bone.NodeTransform);
			WriteBinary(DataPos, a_pOutData, Bone.OffsetMatrix);

			// Write weights
			WriteBinary(DataPos, a_pOutData, Bone.NumWeights);
			for (unsigned int i = 0; i < Bone.NumWeights; ++i)
			{
				const sWeight& Weight = Bone.pWeights[i];
				WriteBinary(DataPos, a_pOutData, Weight.VertexID);
				WriteBinary(DataPos, a_pOutData, Weight.Weight);
			}

			// Write children IDs
			WriteBinary(DataPos, a_pOutData, Bone.NumChildren);
			WriteBinary(DataPos, a_pOutData, Bone.pChildren, Bone.NumChildren);
		}

		// Write animation
		WriteBinary(DataPos, a_pOutData, a_Model.GetAnimation().TicksPerSecond);
		WriteBinary(DataPos, a_pOutData, a_Model.GetAnimation().Duration);
		unsigned int NumNodeAnimations = a_Model.GetAnimation().NodeAnimations.size();
		WriteBinary(DataPos, a_pOutData, NumNodeAnimations);
		for (unsigned int i = 0; i < NumNodeAnimations; ++i)
		{
			const sNodeAnimation Animation = a_Model.GetAnimation().NodeAnimations[i];

			// Write node name
			WriteString(DataPos, a_pOutData, Animation.Name);

			// Write keyframes
			WriteKeys(DataPos, a_pOutData, Animation.PositionKeys);
			WriteKeys(DataPos, a_pOutData, Animation.RotationKeys);
		}

		WriteBinary(DataPos, a_pOutData, a_Model.GetGlobalInverseTransform());

		return DataPos;
	}





	bool DataToModel(const char* a_pData, SkeletalModel& a_OutModel)
	{
		unsigned int DataPos = 0;

		int Version = 0;
		ReadBinary(DataPos, Version, a_pData);

		bool IsLegacy = (Version == LEGACY_HEADER_A);
		if (IsLegacy)
		{
			// Old writers stored the constant header (1, 2, 3). Consume the
			// rest of it so the stream stays aligned.
			int LegacyB = 0, LegacyC = 0;
			ReadBinary(DataPos, LegacyB, a_pData);
			ReadBinary(DataPos, LegacyC, a_pData);
			if (LegacyB != LEGACY_HEADER_B || LegacyC != LEGACY_HEADER_C)
				return false;
		}
		else if (Version != FORMAT_VERSION)
		{
			return false;
		}

		unsigned int NumMeshes = 0;
		ReadBinary(DataPos, NumMeshes, a_pData);

		// The legacy format kept weights on bones, with vertex IDs global to the
		// model. With several meshes there is no way to tell which mesh a
		// weight belongs to, so refuse instead of guessing.
		if (IsLegacy && NumMeshes > 1)
			return false;

		// Replace whatever the model held before; never merge into stale state.
		a_OutModel.Clear();

		for (unsigned int i = 0; i < NumMeshes; ++i)
		{
			sAnimatedMesh Mesh;

			// Read vertices and normals
			ReadBinary(DataPos, Mesh.NumVertices, a_pData);
			ReadBinary(DataPos, Mesh.pVertices, a_pData, Mesh.NumVertices);
			ReadBinary(DataPos, Mesh.pNormals, a_pData, Mesh.NumVertices);
			if (Mesh.NumVertices > 0)
			{

				Mesh.pTransformedVertices = new glm::vec3[Mesh.NumVertices];
				Mesh.pTransformedNormals = new glm::vec3[Mesh.NumVertices];
			}
			else
			{
				Mesh.pTransformedVertices = NULL;
				Mesh.pTransformedNormals = NULL;
			}

			// Read indices
			ReadBinary(DataPos, Mesh.NumIndices, a_pData);
			ReadBinary(DataPos, Mesh.pIndices, a_pData, Mesh.NumIndices);

			// Read mesh-local bone bindings (new format only). Legacy meshes
			// keep the defaults: no bindings, UseMeshBindings == false.
			if (!IsLegacy)
				ReadMeshBindings(DataPos, Mesh, a_pData);

			a_OutModel.AddMesh(Mesh);
		}

		unsigned int NumBones = 0;
		ReadBinary(DataPos, NumBones, a_pData);
		for (unsigned int i = 0; i < NumBones; ++i)
		{
			sBone Bone;

			// Read bone name
			ReadString(DataPos, Bone.Name, a_pData);

			// Read matrices
			ReadBinary(DataPos, Bone.NodeTransform, a_pData);
			ReadBinary(DataPos, Bone.OffsetMatrix, a_pData);

			// Read weights
			ReadBinary(DataPos, Bone.NumWeights, a_pData);
			if (Bone.NumWeights > 0)
			{
				Bone.pWeights = new sWeight[Bone.NumWeights];
				for (unsigned int i = 0; i < Bone.NumWeights; ++i)
				{
					sWeight Weight;
					ReadBinary(DataPos, Weight.VertexID, a_pData);
					ReadBinary(DataPos, Weight.Weight, a_pData);
					Bone.pWeights[i] = Weight;
				}
			}
			else
			{
				Bone.pWeights = NULL;
			}

			// Read children IDs
			ReadBinary(DataPos, Bone.NumChildren, a_pData);
			ReadBinary(DataPos, Bone.pChildren, a_pData, Bone.NumChildren);

			a_OutModel.GetSkeleton().Bones.push_back(Bone);
		}

		// Read animation
		ReadBinary(DataPos, a_OutModel.GetAnimation().TicksPerSecond, a_pData);
		ReadBinary(DataPos, a_OutModel.GetAnimation().Duration, a_pData);
		unsigned int NumNodeAnimations = 0;
		ReadBinary(DataPos, NumNodeAnimations, a_pData);
		for (unsigned int i = 0; i < NumNodeAnimations; ++i)
		{
			sNodeAnimation Animation;

			// Read node name
			ReadString(DataPos, Animation.Name, a_pData);

			// Read position keys
			ReadKeys(DataPos, Animation.PositionKeys, a_pData);

			// Read rotation keys
			ReadKeys(DataPos, Animation.RotationKeys, a_pData);

			a_OutModel.GetAnimation().NodeAnimations.push_back(Animation);
		}

		glm::mat4x4 GlobalInverseTransform;
		ReadBinary(DataPos, GlobalInverseTransform, a_pData);
		a_OutModel.SetGlobalInverseTransform(GlobalInverseTransform);

		return true;
	}
}