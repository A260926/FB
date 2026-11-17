#include "SkeletalModelSerializer.h"

#include "SkeletalModel.h"

namespace SA
{
	// "BAMS" in little-endian byte order.
	static const unsigned int SerializationMagic = 0x534D4142;
	static const unsigned int SerializationVersion = 2;
	static const unsigned int LegacyHeaderWord0 = 1;
	static const unsigned int LegacyHeaderWord1 = 2;
	static const unsigned int LegacyHeaderWord2 = 3;
	// Refuse counts that cannot be addressed or that would overflow the
	// position arithmetic while sizing a read.
	static const unsigned int MaxElementCount = 0x10000000u;

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
			// Copy exactly StringLength bytes; no borrowed pointers and no
			// trailing characters.
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
		if (NumKeys > MaxElementCount)
			NumKeys = 0;
		a_Destination.resize(NumKeys);
		for (unsigned int i = 0; i < NumKeys; ++i)
		{
			sNodeAnimationKey<_Ty> PosKey;
			ReadBinary(a_InOutPos, PosKey.Time, a_pSource);
			ReadBinary(a_InOutPos, PosKey.Value, a_pSource);
			a_Destination[i] = PosKey;
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

		// Explicit format marker so readers can distinguish this layout from
		// the legacy 1/2/3-header layout.
		WriteBinary(DataPos, a_pOutData, SerializationMagic);
		WriteBinary(DataPos, a_pOutData, SerializationVersion);

		// Write meshes
		unsigned int NumMeshes = a_Model.GetNumMeshes();
		WriteBinary(DataPos, a_pOutData, NumMeshes);
		for (unsigned int i = 0; i < NumMeshes; ++i)
		{
			const sAnimatedMesh& Mesh = a_Model.GetMesh(i);

			// Bound counts to what the format can address.
			if (Mesh.NumVertices > MaxElementCount || Mesh.NumIndices > MaxElementCount)
				return 0;

			// Write vertices and normals
 			WriteBinary(DataPos, a_pOutData, Mesh.NumVertices);
 			WriteBinary(DataPos, a_pOutData, Mesh.pVertices, Mesh.NumVertices);
 			WriteBinary(DataPos, a_pOutData, Mesh.pNormals, Mesh.NumVertices);

			// Write indices
			WriteBinary(DataPos, a_pOutData, Mesh.NumIndices);
			WriteBinary(DataPos, a_pOutData, Mesh.pIndices, Mesh.NumIndices);

			// Write mesh-local bindings. The flag and the (possibly empty)
			// binding table are always serialized so static meshes round-trip.
			unsigned int UseMeshBindings = Mesh.UseMeshBindings ? 1u : 0u;
			WriteBinary(DataPos, a_pOutData, UseMeshBindings);

			unsigned int NumBindings = Mesh.MeshBindings.size();
			WriteBinary(DataPos, a_pOutData, NumBindings);
			for (unsigned int b = 0; b < NumBindings; ++b)
			{
				const sMeshBoneBinding& Binding = Mesh.MeshBindings[b];
				if (Binding.Weights.size() > MaxElementCount)
					return 0;

				WriteBinary(DataPos, a_pOutData, Binding.BoneIndex);
				WriteBinary(DataPos, a_pOutData, Binding.OffsetMatrix);

				unsigned int NumWeights = Binding.Weights.size();
				WriteBinary(DataPos, a_pOutData, NumWeights);
				for (unsigned int w = 0; w < NumWeights; ++w)
				{
					WriteBinary(DataPos, a_pOutData, Binding.Weights[w].VertexID);
					WriteBinary(DataPos, a_pOutData, Binding.Weights[w].Weight);
				}
			}
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





	// Releases whatever a partially parsed mesh/bone already owns.
	static void DeleteParsedMesh(sAnimatedMesh& a_Mesh)
	{
		delete[] a_Mesh.pVertices;
		delete[] a_Mesh.pNormals;
		delete[] a_Mesh.pTransformedVertices;
		delete[] a_Mesh.pTransformedNormals;
		delete[] a_Mesh.pIndices;
		a_Mesh.pVertices = NULL;
		a_Mesh.pNormals = NULL;
		a_Mesh.pTransformedVertices = NULL;
		a_Mesh.pTransformedNormals = NULL;
		a_Mesh.pIndices = NULL;
	}

	static void DeleteParsedMeshes(std::vector<sAnimatedMesh>& a_Meshes)
	{
		for (unsigned int i = 0; i < a_Meshes.size(); ++i)
		{
			DeleteParsedMesh(a_Meshes[i]);
		}
		a_Meshes.clear();
	}

	static void DeleteParsedBones(std::vector<sBone>& a_Bones)
	{
		for (unsigned int i = 0; i < a_Bones.size(); ++i)
		{
			delete[] a_Bones[i].pWeights;
			delete[] a_Bones[i].pChildren;
			a_Bones[i].pWeights = NULL;
			a_Bones[i].pChildren = NULL;
		}
		a_Bones.clear();
	}





	bool DataToModel(const char* a_pData, SkeletalModel& a_OutModel)
	{
		unsigned int DataPos = 0;

		unsigned int Magic = 0;
		ReadBinary(DataPos, Magic, a_pData);

		if (Magic == SerializationMagic)
		{
			unsigned int Version = 0;
			ReadBinary(DataPos, Version, a_pData);
			if (Version != SerializationVersion)
				return false;
		}
		else if (Magic == LegacyHeaderWord0)
		{
			// Legacy layout: the first word is 1, followed by 2 and 3.
			unsigned int Word1 = 0, Word2 = 0;
			ReadBinary(DataPos, Word1, a_pData);
			ReadBinary(DataPos, Word2, a_pData);
			if (Word1 != LegacyHeaderWord1 || Word2 != LegacyHeaderWord2)
				return false;
		}
		else
		{
			return false;
		}

		// Parse into detached storage first so a failed load never leaves a
		// half-populated or borrowed model behind.
		std::vector<sAnimatedMesh> Meshes;
		std::vector<sBone> Bones;
		sAnimation AnimationData;
		glm::mat4x4 GlobalInverseTransform(1);
		AnimationData.TicksPerSecond = 0.0f;
		AnimationData.Duration = 0.0f;

		unsigned int NumMeshes = 0;
		ReadBinary(DataPos, NumMeshes, a_pData);
		for (unsigned int i = 0; i < NumMeshes; ++i)
		{
			sAnimatedMesh Mesh;
			Mesh.NumVertices = 0;
			Mesh.pVertices = NULL;
			Mesh.pNormals = NULL;
			Mesh.pTransformedVertices = NULL;
			Mesh.pTransformedNormals = NULL;
			Mesh.NumIndices = 0;
			Mesh.pIndices = NULL;
			Mesh.UseMeshBindings = false;

			// Read vertices and normals
			ReadBinary(DataPos, Mesh.NumVertices, a_pData);
			if (Mesh.NumVertices > MaxElementCount)
			{
				DeleteParsedMeshes(Meshes);
				return false;
			}
			ReadBinary(DataPos, Mesh.pVertices, a_pData, Mesh.NumVertices);
			ReadBinary(DataPos, Mesh.pNormals, a_pData, Mesh.NumVertices);
			if (Mesh.NumVertices > 0)
			{
				Mesh.pTransformedVertices = new glm::vec3[Mesh.NumVertices]();
				Mesh.pTransformedNormals = new glm::vec3[Mesh.NumVertices]();
			}
			else
			{
				Mesh.pTransformedVertices = NULL;
				Mesh.pTransformedNormals = NULL;
			}

			// Read indices
			ReadBinary(DataPos, Mesh.NumIndices, a_pData);
			if (Mesh.NumIndices > MaxElementCount)
			{
				DeleteParsedMesh(Mesh);
				DeleteParsedMeshes(Meshes);
				return false;
			}
			ReadBinary(DataPos, Mesh.pIndices, a_pData, Mesh.NumIndices);

			// Read mesh-local bindings (version 2 only)
			if (Magic == SerializationMagic)
			{
				unsigned int UseMeshBindings = 0;
				ReadBinary(DataPos, UseMeshBindings, a_pData);
				Mesh.UseMeshBindings = (UseMeshBindings != 0);

				unsigned int NumBindings = 0;
				ReadBinary(DataPos, NumBindings, a_pData);
				if (NumBindings > MaxElementCount)
				{
					DeleteParsedMesh(Mesh);
					DeleteParsedMeshes(Meshes);
					return false;
				}
				for (unsigned int b = 0; b < NumBindings; ++b)
				{
					sMeshBoneBinding Binding;
					ReadBinary(DataPos, Binding.BoneIndex, a_pData);
					ReadBinary(DataPos, Binding.OffsetMatrix, a_pData);

					unsigned int NumWeights = 0;
					ReadBinary(DataPos, NumWeights, a_pData);
					if (NumWeights > MaxElementCount)
					{
						DeleteParsedMesh(Mesh);
						DeleteParsedMeshes(Meshes);
						return false;
					}
					Binding.Weights.resize(NumWeights);
					for (unsigned int w = 0; w < NumWeights; ++w)
					{
						ReadBinary(DataPos, Binding.Weights[w].VertexID, a_pData);
						ReadBinary(DataPos, Binding.Weights[w].Weight, a_pData);
					}

					Mesh.MeshBindings.push_back(Binding);
				}
			}

			Meshes.push_back(Mesh);
		}

		unsigned int NumBones = 0;
		ReadBinary(DataPos, NumBones, a_pData);
		if (NumBones > MaxElementCount)
		{
			DeleteParsedMeshes(Meshes);
			return false;
		}
		for (unsigned int i = 0; i < NumBones; ++i)
		{
			sBone Bone;
			Bone.NumWeights = 0;
			Bone.pWeights = NULL;
			Bone.NumChildren = 0;
			Bone.pChildren = NULL;

			// Read bone name
			ReadString(DataPos, Bone.Name, a_pData);

			// Read matrices
			ReadBinary(DataPos, Bone.NodeTransform, a_pData);
			ReadBinary(DataPos, Bone.OffsetMatrix, a_pData);

			// Read weights
			ReadBinary(DataPos, Bone.NumWeights, a_pData);
			if (Bone.NumWeights > MaxElementCount)
			{
				DeleteParsedMeshes(Meshes);
				DeleteParsedBones(Bones);
				return false;
			}
			if (Bone.NumWeights > 0)
			{
				Bone.pWeights = new sWeight[Bone.NumWeights];
				for (unsigned int w = 0; w < Bone.NumWeights; ++w)
				{
					sWeight Weight;
					ReadBinary(DataPos, Weight.VertexID, a_pData);
					ReadBinary(DataPos, Weight.Weight, a_pData);
					Bone.pWeights[w] = Weight;
				}
			}
			else
			{
				Bone.pWeights = NULL;
			}

			// Read children IDs
			ReadBinary(DataPos, Bone.NumChildren, a_pData);
			if (Bone.NumChildren > MaxElementCount)
			{
				delete[] Bone.pWeights;
				DeleteParsedMeshes(Meshes);
				DeleteParsedBones(Bones);
				return false;
			}
			ReadBinary(DataPos, Bone.pChildren, a_pData, Bone.NumChildren);

			Bones.push_back(Bone);
		}

		// Read animation
		ReadBinary(DataPos, AnimationData.TicksPerSecond, a_pData);
		ReadBinary(DataPos, AnimationData.Duration, a_pData);
		unsigned int NumNodeAnimations = 0;
		ReadBinary(DataPos, NumNodeAnimations, a_pData);
		if (NumNodeAnimations > MaxElementCount)
		{
			DeleteParsedMeshes(Meshes);
			DeleteParsedBones(Bones);
			return false;
		}
		for (unsigned int i = 0; i < NumNodeAnimations; ++i)
		{
			sNodeAnimation Animation;

			// Read node name
			ReadString(DataPos, Animation.Name, a_pData);

			// Read position keys
			ReadKeys(DataPos, Animation.PositionKeys, a_pData);

			// Read rotation keys
			ReadKeys(DataPos, Animation.RotationKeys, a_pData);

			AnimationData.NodeAnimations.push_back(Animation);
		}

		ReadBinary(DataPos, GlobalInverseTransform, a_pData);

		// Legacy assets cannot tell which mesh owned each bone's weights, so
		// multi-mesh legacy data is ambiguous and is rejected rather than
		// guessed. A single-mesh legacy asset is unambiguous and is converted
		// to mesh-local bindings.
		if (Magic == LegacyHeaderWord0)
		{
			if (Meshes.size() > 1)
			{
				DeleteParsedMeshes(Meshes);
				DeleteParsedBones(Bones);
				return false;
			}

			for (unsigned int i = 0; i < Bones.size() && Meshes.size() == 1; ++i)
			{
				if (Bones[i].NumWeights == 0)
					continue;

				sMeshBoneBinding Binding;
				Binding.BoneIndex = i;
				Binding.OffsetMatrix = Bones[i].OffsetMatrix;
				Binding.Weights.assign(Bones[i].pWeights, Bones[i].pWeights + Bones[i].NumWeights);

				Meshes[0].MeshBindings.push_back(Binding);
			}
			if (Meshes.size() == 1)
			{
				Meshes[0].UseMeshBindings = true;
			}
		}

		// Commit: replace, never merge with, whatever the model held before.
		a_OutModel.Clear();
		for (unsigned int i = 0; i < Meshes.size(); ++i)
		{
			a_OutModel.AddMesh(Meshes[i]);
		}
		a_OutModel.GetSkeleton().Bones.swap(Bones);
		a_OutModel.GetAnimation() = AnimationData;
		a_OutModel.SetGlobalInverseTransform(GlobalInverseTransform);

		return true;
	}
}