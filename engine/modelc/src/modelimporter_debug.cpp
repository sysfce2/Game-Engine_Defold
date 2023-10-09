
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

#include "modelimporter.h"
#include <dmsdk/dlib/dstrings.h>
#include <stdio.h>
#include <stdlib.h>

namespace dmModelImporter
{

void* ReadFile(const char* path, uint32_t* file_size)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        printf("Failed to load %s\n", path);
        return 0;
    }

    fseek(file, 0, SEEK_END);
    size_t size = (size_t)ftell(file);
    fseek(file, 0, SEEK_SET);

    void* mem = malloc(size);

    size_t nread = fread(mem, 1, size, file);
    fclose(file);

    if (nread != size)
    {
        printf("Failed to read %u bytes from %s\n", (uint32_t)size, path);
        free(mem);
        return 0;
    }

    if (file_size)
        *file_size = size;

    return mem;
}

static void OutputIndent(int indent)
{
    for (int i = 0; i < indent; ++i) {
        printf("  ");
    }
}

// static void OutputTransform(const dmTransform::Transform& transform)
// {
//     printf("t: %f, %f, %f  ", transform.GetTranslation().getX(), transform.GetTranslation().getY(), transform.GetTranslation().getZ());
//     printf("r: %f, %f, %f, %f  ", transform.GetRotation().getX(), transform.GetRotation().getY(), transform.GetRotation().getZ(), transform.GetRotation().getW());
//     printf("s: %f, %f, %f  ", transform.GetScale().getX(), transform.GetScale().getY(), transform.GetScale().getZ());
// }


static void OutputVector3(const dmModelImporter::Vec3f& v)
{
    printf("%f, %f, %f\n", v.x, v.y, v.z);
}

static void OutputVector4(const dmModelImporter::Vec4f& v)
{
    printf("%f, %f, %f, %f\n", v.x, v.y, v.z, v.w);
}

static void OutputMatrix(const dmModelImporter::Transform& transform)
{
    printf("    t: "); OutputVector3(transform.m_Translation);
    printf("    s: "); OutputVector3(transform.m_Scale);
    printf("    r: "); OutputVector4(transform.m_Rotation);
}

static void OutputBone(int i, Bone* bone, int indent)
{
    OutputIndent(indent);
    printf("#%d: %s  idx: %u parent: %s node: %s inv_bind_pose:\n", i, bone->m_Name, bone->m_Index, bone->m_Parent?bone->m_Parent->m_Name:"null", bone->m_Node?bone->m_Node->m_Name:"null");
    OutputMatrix(bone->m_InvBindPose);
    printf("\n");
}

static void OutputSkin(Skin* skin, int indent)
{
    OutputIndent(indent);
    printf("Skin name: %s\n", skin->m_Name);

    printf("  Bones: count: %u\n", skin->m_Bones.Size());
    for (uint32_t i = 0; i < skin->m_Bones.Size(); ++i)
    {
        OutputBone(i, skin->m_Bones[i], indent+1);
    }
}

static void OutputNode(Node* node)
{
    printf("Node: %s : \n", node->m_Name);
    printf("  local\n");
    OutputMatrix(node->m_Local);
    printf("\n  world\n");
    OutputMatrix(node->m_World);
    printf("\n");
}

static void OutputNodeTree(Node* node, int indent)
{
    OutputIndent(indent);
    printf("%s: ", node->m_Name);
        if (node->m_Skin)
            printf("skin: %s", node->m_Skin->m_Name);
        printf("\n");


    for (uint32_t i = 0; i < node->m_Children.Size(); ++i)
    {
        OutputNodeTree(node->m_Children[i], indent+1);
    }
}

static void OutputMaterial(Material* material, int indent)
{
    OutputIndent(indent);
    printf("material  %s\n", material->m_Name);
}

static void OutputMesh(Mesh* mesh, int indent)
{
    OutputIndent(indent);

    const char* material_name = (mesh->m_Material && mesh->m_Material->m_Name) ? mesh->m_Material->m_Name : "null";

    printf("mesh  %s  vertices: %u  indices: %u  mat: %s  weights: %s  colors: %s aabb: (%f, %f, %f) (%f, %f, %f)\n",
            mesh->m_Name?mesh->m_Name:"null", mesh->m_VertexCount, mesh->m_Indices.Size(), material_name, mesh->m_Weights.Empty()?"no":"yes", mesh->m_Colors.Empty()?"no":"yes",
            mesh->m_Aabb.m_Min.x, mesh->m_Aabb.m_Min.y, mesh->m_Aabb.m_Min.z,
            mesh->m_Aabb.m_Max.x, mesh->m_Aabb.m_Max.y, mesh->m_Aabb.m_Max.z);

    // if (mesh->m_Weights)
    // {
    //     for (uint32_t i = 0; i < mesh->m_VertexCount; ++i)
    //     {
    //         printf("  %4u  B: %2u, %2u, %2u, %2u\t", i, mesh->m_Bones[i*4+0], mesh->m_Bones[i*4+1], mesh->m_Bones[i*4+2], mesh->m_Bones[i*4+3]);
    //         printf("  W: %f, %f, %f, %f\n", mesh->m_Weights[i*4+0], mesh->m_Weights[i*4+1], mesh->m_Weights[i*4+2], mesh->m_Weights[i*4+3]);
    //     }
    // }
    //printf("tris: %u  material: %s", mesh->)

    // for (uint32_t i = 0; i < mesh->m_IndexCount; ++i)
    // {
    //     printf("%3d\t", mesh->m_Indices[i]);
    //     if (((i+1) % 16) == 0)
    //         printf("\n");
    // }
    // printf("\n");
}

static void OutputModel(Model* model, int indent)
{
    OutputIndent(indent);
    printf("%s   meshes count: %u\n", model->m_Name, model->m_Meshes.Size());
        if (model->m_ParentBone)
        {
            printf("  bone: %s", model->m_ParentBone->m_Name);
        }
    printf("\n");
    for (uint32_t i = 0; i < model->m_Meshes.Size(); ++i)
    {
        Mesh* mesh = &model->m_Meshes[i];
        OutputMesh(mesh, indent+1);
    }
}

static void OutputNodeAnimation(NodeAnimation* node_animation, int indent)
{
    OutputIndent(indent);
    printf("node: %s\n", node_animation->m_Node->m_Name);

    indent++;
    OutputIndent(indent);
    printf("# translation keys: %u\n", node_animation->m_TranslationKeys.Size());
    for (uint32_t i = 0; i < node_animation->m_TranslationKeys.Size(); ++i)
    {
        OutputIndent(indent+1);
        KeyFrame* key = &node_animation->m_TranslationKeys[i];
        printf("%.3f:  %.3f, %.3f, %.3f\n", key->m_Time, key->m_Value[0], key->m_Value[1], key->m_Value[2]);
    }

    OutputIndent(indent);
    printf("# rotation keys: %u\n", node_animation->m_RotationKeys.Size());
    for (uint32_t i = 0; i < node_animation->m_RotationKeys.Size(); ++i)
    {
        OutputIndent(indent+1);
        KeyFrame* key = &node_animation->m_RotationKeys[i];
        printf("%.3f:  %.3f, %.3f, %.3f, %.3f\n", key->m_Time, key->m_Value[0], key->m_Value[1], key->m_Value[2], key->m_Value[3]);
    }

    OutputIndent(indent);
    printf("# scale keys: %u\n", node_animation->m_ScaleKeys.Size());
    for (uint32_t i = 0; i < node_animation->m_ScaleKeys.Size(); ++i)
    {
        OutputIndent(indent+1);
        KeyFrame* key = &node_animation->m_ScaleKeys[i];
        printf("%.3f:  %.3f, %.3f, %.3f\n", key->m_Time, key->m_Value[0], key->m_Value[1], key->m_Value[2]);
    }
}

static void OutputAnimation(Animation* animation, int indent)
{
    OutputIndent(indent);
    printf("%s duration: %f\n", animation->m_Name, animation->m_Duration);

    for (uint32_t i = 0; i < animation->m_NodeAnimations.Size(); ++i)
    {
        NodeAnimation* node_animation = &animation->m_NodeAnimations[i];
        OutputNodeAnimation(node_animation, indent+1);
    }
}

void DebugScene(Scene* scene)
{
    if (!scene)
    {
        printf("Output model importer scene: Scene is null!\n");
        return;
    }

    printf("Output model importer scene:\n");

    printf("------------------------------\n");
    printf("Buffers\n");
    for (uint32_t i = 0; i < scene->m_Buffers.Size(); ++i)
    {
        OutputIndent(1);
        printf("Buffer '%.48s' sz: %u  %p\n", scene->m_Buffers[i].m_Uri, scene->m_Buffers[i].m_BufferSize, scene->m_Buffers[i].m_Buffer);
    }

    printf("------------------------------\n");

    for (uint32_t i = 0; i < scene->m_Materials.Size(); ++i)
    {
        OutputMaterial(&scene->m_Materials[i], 0);
    }

    for (uint32_t i = 0; i < scene->m_DynamicMaterials.Size(); ++i)
    {
        OutputMaterial(scene->m_DynamicMaterials[i], 0);
    }

    printf("------------------------------\n");
    for (uint32_t i = 0; i < scene->m_Nodes.Size(); ++i)
    {
        OutputNode(&scene->m_Nodes[i]);
    }
    printf("------------------------------\n");

    printf("Subscenes: count: %u\n", scene->m_RootNodes.Size());
    for (uint32_t i = 0; i < scene->m_RootNodes.Size(); ++i)
    {
        printf("------------------------------\n");
        OutputNodeTree(scene->m_RootNodes[i], 1);
        printf("------------------------------\n");
    }

    printf("Skins: count: %u\n", scene->m_Skins.Size());
    for (uint32_t i = 0; i < scene->m_Skins.Size(); ++i)
    {
        printf("------------------------------\n");
        OutputSkin(&scene->m_Skins[i], 1);
        printf("------------------------------\n");
    }

    printf("Models: count: %u\n", scene->m_Models.Size());
    for (uint32_t i = 0; i < scene->m_Models.Size(); ++i)
    {
        printf("------------------------------\n");
        OutputModel(&scene->m_Models[i], 1);
        printf("------------------------------\n");
    }

    printf("Animations: count: %u\n", scene->m_Animations.Size());
    for (uint32_t i = 0; i < scene->m_Animations.Size(); ++i)
    {
        printf("------------------------------\n");
        OutputAnimation(&scene->m_Animations[i], 1);
        printf("------------------------------\n");
    }
    printf("Output model importer scene done\n");
}

static void DebugStructNode(Node* node, int indent)
{
    OutputIndent(indent); printf("Node: %p\n", node);
    assert(node->m_Name);
    OutputIndent(indent); printf("  m_Local: .\n");
    OutputIndent(indent); printf("  m_World: .\n");
    OutputIndent(indent); printf("  m_Name: %p (%s)\n", node->m_Name, node->m_Name);
    OutputIndent(indent); printf("  m_Model: %p\n", node->m_Model);
    OutputIndent(indent); printf("  m_Skin: %p\n", node->m_Skin);
    OutputIndent(indent); printf("  m_Parent: %p\n", node->m_Parent);
    OutputIndent(indent); printf("  m_Children#: %u\n", node->m_Children.Size());
}

static void DebugStructNodeTree(Node* node, int indent)
{
    DebugStructNode(node, indent);

    for (uint32_t i = 0; i < node->m_Children.Size(); ++i)
    {
        DebugStructNode(node->m_Children[i], indent+1);
    }
}

static void DebugStructMesh(Mesh* mesh, int indent)
{
    OutputIndent(indent); printf("Mesh: %p\n", mesh);
    assert(mesh->m_Name);
    assert(mesh->m_Material);
    OutputIndent(indent); printf("  m_Name: %p (%s)\n", mesh->m_Name, mesh->m_Name);
    OutputIndent(indent); printf("  m_Material: %p (%s)\n", mesh->m_Material->m_Name, mesh->m_Material->m_Name);

    OutputIndent(indent); printf("  m_Positions: %u\n", mesh->m_Positions.Size());
    OutputIndent(indent); printf("  m_Normals: %u\n", mesh->m_Normals.Size());
    OutputIndent(indent); printf("  m_Tangents: %u\n", mesh->m_Tangents.Size());
    OutputIndent(indent); printf("  m_Color: %u\n", mesh->m_Colors.Size());
    OutputIndent(indent); printf("  m_Weights: %u\n", mesh->m_Weights.Size());
    OutputIndent(indent); printf("  m_Bones: %u\n", mesh->m_Bones.Size());

    OutputIndent(indent); printf("  m_TexCoord0: %u\n", mesh->m_TexCoord0.Size());
    OutputIndent(indent); printf("  m_TexCoord0NumComponents: %u\n", mesh->m_TexCoord0NumComponents);
    OutputIndent(indent); printf("  m_TexCoord1: %u\n", mesh->m_TexCoord1.Size());
    OutputIndent(indent); printf("  m_TexCoord1NumComponents: %u\n", mesh->m_TexCoord1NumComponents);

    OutputIndent(indent); printf("  m_Indices: %u\n", mesh->m_Indices.Size());
    OutputIndent(indent); printf("  m_VertexCount: %u\n", mesh->m_VertexCount);
}

static void DebugStructModel(Model* model, int indent)
{
    OutputIndent(indent); printf("Model: %p\n", model);
    assert(model->m_Name);
    OutputIndent(indent); printf("  m_Name: %p (%s)\n", model->m_Name, model->m_Name);
    OutputIndent(indent); printf("  m_Meshes: %u\n", model->m_Meshes.Size());

    for (uint32_t i = 0; i < model->m_Meshes.Size(); ++i) {
        DebugStructMesh(&model->m_Meshes[i], indent+1);
        OutputIndent(indent+1); printf("-------------------------------\n");
    }
}

static void DebugStructSkin(Skin* skin, int indent)
{
    OutputIndent(indent); printf("Skin: %p\n", skin);
    assert(skin->m_Name);
    OutputIndent(indent); printf("  m_Name: %p (%s)\n", skin->m_Name, skin->m_Name);
    // OutputIndent(indent); printf("  m_Model: %p\n", node->m_Model);
    // OutputIndent(indent); printf("  m_Skin: %p\n", node->m_Skin);
    // OutputIndent(indent); printf("  m_Parent: %p\n", node->m_Parent);
    // OutputIndent(indent); printf("  m_Children: %u\n", node->m_Children.Size());
}

void DebugStructScene(Scene* scene)
{
    printf("Scene: %p\n", scene);
    printf("  m_OpaqueSceneData: %p\n", scene->m_OpaqueSceneData);
    printf("  m_DestroyFn: %p\n", scene->m_DestroyFn);
    printf("  m_Nodes: %u\n", scene->m_Nodes.Size());
    printf("  m_Models: %u\n", scene->m_Models.Size());
    printf("  m_Skins: %u\n", scene->m_Skins.Size());
    printf("  m_RootNodes: %u\n", scene->m_RootNodes.Size());
    printf("  m_Animations: %u\n", scene->m_Animations.Size());

    printf("-------------------------------\n");
    for (uint32_t i = 0; i < scene->m_Nodes.Size(); ++i) {
        DebugStructNode(&scene->m_Nodes[i], 1);
        printf("-------------------------------\n");
    }
    for (uint32_t i = 0; i < scene->m_Models.Size(); ++i) {
        DebugStructModel(&scene->m_Models[i], 1);
        printf("-------------------------------\n");
    }
    for (uint32_t i = 0; i < scene->m_Skins.Size(); ++i) {
        DebugStructSkin(&scene->m_Skins[i], 1);
        printf("-------------------------------\n");
    }
    for (uint32_t i = 0; i < scene->m_RootNodes.Size(); ++i) {
        DebugStructNodeTree(scene->m_RootNodes[i], 1);
        printf("-------------------------------\n");
    }
}

}
