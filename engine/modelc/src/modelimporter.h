
// Copyright 2020-2023 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#ifndef DM_MODELIMPORTER_H
#define DM_MODELIMPORTER_H

#include <dmsdk/dlib/align.h>
#include <dmsdk/dlib/array.h>
#include <dmsdk/dlib/transform.h>
#include <dmsdk/dlib/shared_library.h>
#include <dmsdk/dlib/vmath.h>
#include <stdint.h>

namespace dmModelImporter
{
    static const uint32_t INVALID_INDEX = 0x7FFFFFFF; // needs to fit into an int

    struct Vec3f
    {
        float x, y, z;

        Vec3f() {}
        Vec3f(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
        Vec3f(const Vec3f& v) : x(v.x), y(v.y), z(v.z) {}
        Vec3f& operator= (const Vec3f& rhs) {
            x = rhs.x;
            y = rhs.y;
            z = rhs.z;
            return *this;
        }
    };

    struct Vec4f
    {
        float x, y, z, w;

        Vec4f() {}
        Vec4f(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z) {}
        Vec4f(const Vec4f& v) : x(v.x), y(v.y), z(v.z), w(v.w) {}
        Vec4f& operator= (const Vec4f& rhs) {
            x = rhs.x;
            y = rhs.y;
            z = rhs.z;
            w = rhs.w;
            return *this;
        }
    };

    struct Aabb
    {
        Vec3f m_Min;
        Vec3f m_Max;

        Aabb();
        void Union(const Vec3f& p);
    };

    struct Transform
    {
        Vec3f m_Translation;
        Vec3f m_Scale;
        Vec4f m_Rotation;

        Transform() {};
        Transform(const Vec3f& t, const Vec4f& r, const Vec3f& s)
            : m_Translation(t)
            , m_Scale(s)
            , m_Rotation(r) {};
        Transform(const Transform& t)
            : m_Translation(t.m_Translation)
            , m_Scale(t.m_Scale)
            , m_Rotation(t.m_Rotation) {};

        Transform& operator= (const Transform& rhs) {
            m_Translation = rhs.m_Translation;
            m_Rotation = rhs.m_Rotation;
            m_Scale = rhs.m_Scale;
            return *this;
        }
        void SetIdentity() {
            m_Translation = Vec3f(0,0,0);
            m_Scale = Vec3f(1,1,1);
            m_Rotation = Vec4f(0,0,0,1);
        }
    };

    Transform ToTransform(const float* m);
    Transform ToTransform(const dmTransform::Transform& t);
    Transform Mul(const Transform& a, const Transform& b);

    struct Material
    {
        const char* m_Name;
        uint32_t    m_Index;        // The index into the scene.materials array
        uint8_t     m_IsSkinned;    // If a skinned mesh is using this
    };

    struct Mesh
    {
        const char*         m_Name;
        Material*           m_Material;
        Aabb                m_Aabb;         // The min/max of the positions data
        uint32_t            m_VertexCount;

        // loop using m_VertexCount * stride
        dmArray<float>      m_Positions;    // 3 floats per vertex
        dmArray<float>      m_Normals;      // 3 floats per vertex
        dmArray<float>      m_Tangents;     // 3 floats per vertex
        dmArray<float>      m_Colors;       // 4 floats per vertex
        dmArray<float>      m_Weights;      // 4 weights per vertex
        dmArray<float>      m_TexCoord0;    // m_TexCoord0NumComponents floats per vertex
        dmArray<float>      m_TexCoord1;    // m_TexCoord1NumComponents floats per vertex
        uint32_t            m_TexCoord0NumComponents; // e.g 2 or 3
        uint32_t            m_TexCoord1NumComponents; // e.g 2 or 3
        dmArray<int32_t>    m_Bones;        // 4 bones per vertex

        dmArray<int32_t>    m_Indices;
    };

    struct Bone;

    struct Model
    {
        const char*     m_Name;
        dmArray<Mesh>   m_Meshes;
        uint32_t        m_Index;        // The index into the scene.models array
        Bone*           m_ParentBone;   // If the model is not skinned, but a child of a bone
    };

    struct Node;

    struct Bone
    {
        Transform           m_InvBindPose; // inverse(world_transform)
        const char*         m_Name;
        Node*               m_Node;
        Bone*               m_Parent;       // 0 if root bone
        uint32_t            m_Index;        // Index into skin.bones

        dmArray<Bone*>      m_Children;
    };

    struct Skin
    {
        const char*         m_Name;
        uint32_t            m_Index;        // The index into the scene.skins array
        dmArray<Bone*>      m_Bones;

        // internal
        dmArray<int32_t>    m_BoneRemap;    // old index -> new index: for sorting the bones
    };

    struct Node
    {
        Transform           m_Local;        // The local transform
        Transform           m_World;        // The world transform
        const char*         m_Name;
        Model*              m_Model;        // not all nodes have a mesh
        Skin*               m_Skin;         // not all nodes have a skin
        Node*               m_Parent;
        dmArray<Node*>      m_Children;
        uint32_t            m_Index;        // The index into the scene.nodes array

        // internal
        uint64_t            m_NameHash;
    };

    struct KeyFrame
    {
        float m_Value[4];   // 3 for translation/scale, 4 for rotation
        float m_Time;
    };

    struct NodeAnimation
    {
        Node*               m_Node;
        float               m_StartTime;
        float               m_EndTime;
        dmArray<KeyFrame>   m_TranslationKeys;
        dmArray<KeyFrame>   m_RotationKeys;
        dmArray<KeyFrame>   m_ScaleKeys;
    };

    struct Animation
    {
        const char*             m_Name;
        float                   m_Duration;
        dmArray<NodeAnimation>  m_NodeAnimations;
    };

    struct Buffer // GLTF format
    {
        const char*     m_Uri;
        void*           m_Buffer;
        uint32_t        m_BufferSize;
    };

    struct Scene
    {
        void*       m_OpaqueSceneData;
        bool        (*m_LoadFinalizeFn)(Scene*);
        bool        (*m_ValidateFn)(Scene*);
        void        (*m_DestroyFn)(Scene*);

        // There may be more than one root node
        dmArray<Node>       m_Nodes;
        dmArray<Model>      m_Models;
        dmArray<Skin>       m_Skins;
        dmArray<Node*>      m_RootNodes;
        dmArray<Animation>  m_Animations;
        dmArray<Material>   m_Materials;
        dmArray<Material*>  m_DynamicMaterials;
        dmArray<Buffer>     m_Buffers;
    };

    struct Options
    {
        Options();

        int dummy; // for the java binding to not be zero size
    };

    extern "C" DM_DLLEXPORT Scene* LoadGltfFromBuffer(Options* options, void* data, uint32_t data_size);

    // GLTF: Returns true if there are unresolved data buffers
    extern "C" DM_DLLEXPORT bool NeedsResolve(Scene* scene);

    // GLTF: Loop over the buffers, and for each missing one, supply the data here
    extern "C" DM_DLLEXPORT void ResolveBuffer(Scene* scene, const char* uri, void* data, uint32_t data_size);

    extern "C" DM_DLLEXPORT Scene* LoadFromBuffer(Options* options, const char* suffix, void* data, uint32_t file_size);

    // GLTF: Finalize the load and create the actual scene structure
    extern "C" DM_DLLEXPORT bool LoadFinalize(Scene* scene);

    // GLTF: Validate after all buffers are resolved
    extern "C" DM_DLLEXPORT bool Validate(Scene* scene);

    extern "C" DM_DLLEXPORT Scene* LoadFromPath(Options* options, const char* path);

    extern "C" DM_DLLEXPORT void DestroyScene(Scene* scene);

    // Used by the editor to create a standalone data blob suitable for reading
    // Caller owns the memory
    extern "C" DM_DLLEXPORT void* ConvertToProtobufMessage(Scene* scene, size_t* length);

    // Switches between warning and debug level
    extern "C" DM_DLLEXPORT void EnableDebugLogging(bool enable);

    void DebugScene(Scene* scene);
    void DebugStructScene(Scene* scene);

    // For tests. User needs to call free() on the returned memory
    void* ReadFile(const char* path, uint32_t* file_size);
    void* ReadFileToBuffer(const char* path, uint32_t buffer_size, void* buffer);
}


#endif // DM_MODELIMPORTER_H
