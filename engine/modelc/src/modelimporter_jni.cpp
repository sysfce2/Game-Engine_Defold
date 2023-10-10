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

#include <jni.h>
#include <jni/jni_util.h>
#include "jni/ModelImporter_jni.h"

#include <dlib/array.h>
#include <dlib/log.h>
#include <dlib/dstrings.h>

#define CLASS_SCENE     "com/dynamo/bob/pipeline/ModelImporter$Scene"

namespace dmModelImporter
{

// ******************************************************************************************************************

static jobjectArray CreateObjectArray(JNIEnv* env, jclass cls, const dmArray<jobject>& values)
{
    uint32_t count = values.Size();
    jobjectArray arr = env->NewObjectArray(count, cls, 0);
    for (uint32_t i = 0; i < count; ++i)
    {
        env->SetObjectArrayElement(arr, i, values[i]);
    }
    return arr;
}

// Creates an array of materials, sorted on the material index
static jobjectArray CreateMaterialsArray(JNIEnv* env, jni::TypeInfos* types,
                        const dmArray<dmModelImporter::Material>& materials,
                        const dmArray<dmModelImporter::Material*>& dynamic_materials,
                        dmArray<jobject>& nodes)
{
    uint32_t count = materials.Size();
    uint32_t dynamic_count = dynamic_materials.Size();
    uint32_t total_count = count + dynamic_count;
    nodes.SetCapacity(total_count);
    nodes.SetSize(total_count);
    jobjectArray arr = env->NewObjectArray(total_count, types->m_MaterialJNI.cls, 0);
    for (uint32_t i = 0; i < count; ++i)
    {
        const dmModelImporter::Material* material = &materials[i];
        jobject obj = jni::C2J_CreateMaterial(env, types, material);
        nodes[material->m_Index] = obj;
        env->SetObjectArrayElement(arr, material->m_Index, obj);
    }
    for (uint32_t i = 0; i < dynamic_count; ++i)
    {
        const dmModelImporter::Material* material = dynamic_materials[i];
        jobject obj = jni::C2J_CreateMaterial(env, types, material);
        nodes[material->m_Index] = obj;
        env->SetObjectArrayElement(arr, material->m_Index, obj);
    }

    return arr;
}

// **************************************************
// Nodes

static jobject CreateNode(JNIEnv* env, jni::TypeInfos* types, const dmModelImporter::Node* node)
{
    jobject obj = env->AllocObject(types->m_NodeJNI.cls);
    // from Modelimporter_jni.cpp C2J_CreateNode():
    dmJNI::SetObjectDeref(env, obj, types->m_NodeJNI.local, C2J_CreateTransform(env, types, &node->m_Local));
    dmJNI::SetObjectDeref(env, obj, types->m_NodeJNI.world, C2J_CreateTransform(env, types, &node->m_World));
    dmJNI::SetString(env, obj, types->m_NodeJNI.name, node->m_Name);
    dmJNI::SetInt(env, obj, types->m_NodeJNI.index, node->m_Index);
    dmJNI::SetLong(env, obj, types->m_NodeJNI.nameHash, node->m_NameHash);
    // Since we want to do fixup ourselves, we delay setting these fields (see)
    // dmJNI::SetObjectDeref(env, obj, types->m_NodeJNI.model, C2J_CreateModel(env, types, node->m_Model));
    // dmJNI::SetObjectDeref(env, obj, types->m_NodeJNI.skin, C2J_CreateSkin(env, types, node->m_Skin));
    // dmJNI::SetObjectDeref(env, obj, types->m_NodeJNI.parent, C2J_CreateNode(env, types, node->m_Parent));
    // dmJNI::SetObjectDeref(env, obj, types->m_NodeJNI.children, C2J_CreateNodePtrArray(env, types, node->m_Children, node->m_ChildrenCount));

    return obj;
}

static void CreateNodes(JNIEnv* env, jni::TypeInfos* types, const dmModelImporter::Scene* scene, dmArray<jobject>& nodes)
{
    uint32_t count = scene->m_Nodes.Size();
    // Create an array of nodes, sorted on node index
    {
        nodes.SetCapacity(count);
        nodes.SetSize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const Node* node = &scene->m_Nodes[i];
            nodes[node->m_Index] = CreateNode(env, types, node);
        }
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        const Node* node = &scene->m_Nodes[i];

        if (node->m_Parent != 0)
        {
            dmJNI::SetObject(env, nodes[i], types->m_NodeJNI.parent, nodes[node->m_Parent->m_Index]);
        }

        // Create an array of children, sorted on child index
        jobjectArray childrenArray = env->NewObjectArray(node->m_Children.Size(), types->m_NodeJNI.cls, 0);
        for (uint32_t i = 0; i < node->m_Children.Size(); ++i)
        {
            dmModelImporter::Node* child = node->m_Children[i];
            env->SetObjectArrayElement(childrenArray, i, nodes[child->m_Index]);
        }
        dmJNI::SetObjectDeref(env, nodes[i], types->m_NodeJNI.children, childrenArray);
    }
}

static void FixupNodeReferences(JNIEnv* env, jni::TypeInfos* types, const dmModelImporter::Scene* scene, const dmArray<jobject>& skins, const dmArray<jobject>& models, const dmArray<jobject>& nodes)
{
    uint32_t count = scene->m_Nodes.Size();
    for (uint32_t i = 0; i < count; ++i)
    {
        const dmModelImporter::Node* node = &scene->m_Nodes[i];
        if (node->m_Skin)
        {
            jobject node_obj = nodes[node->m_Index];
            jobject skin_obj = skins[node->m_Skin->m_Index];
            dmJNI::SetObject(env, node_obj, types->m_NodeJNI.skin, skin_obj);
        }

        if (node->m_Model)
        {
            jobject node_obj = nodes[node->m_Index];
            jobject model_obj = models[node->m_Model->m_Index];
            dmJNI::SetObject(env, node_obj, types->m_NodeJNI.model, model_obj);
        }
    }
}

// **************************************************
// Meshes

static jobjectArray CreateMeshesArray(JNIEnv* env, jni::TypeInfos* types, const dmArray<jobject>& materials, const dmArray<dmModelImporter::Mesh>& meshes)
{
    uint32_t count = meshes.Size();
    jobjectArray arr = env->NewObjectArray(count, types->m_MeshJNI.cls, 0);
    for (uint32_t i = 0; i < count; ++i)
    {
        Mesh* mesh = (Mesh*) &meshes[i];
        Material* material = mesh->m_Material;
        mesh->m_Material = 0; // don't create a new java object...
        jobject o = jni::C2J_CreateMesh(env, types, mesh);
        /// ... but let's reuse the material we already created
        if (material)
        {
            dmJNI::SetObject(env, o, types->m_MeshJNI.material, materials[material->m_Index]);
        }
        env->SetObjectArrayElement(arr, i, o);
        env->DeleteLocalRef(o);
    }
    return arr;
}

static jobject CreateModel(JNIEnv* env, jni::TypeInfos* types, const dmArray<jobject>& materials, const dmModelImporter::Model* model)
{
    jobject obj = env->AllocObject(types->m_ModelJNI.cls);
    dmJNI::SetInt(env, obj, types->m_ModelJNI.index, model->m_Index);
    dmJNI::SetString(env, obj, types->m_ModelJNI.name, model->m_Name);
    dmJNI::SetObjectDeref(env, obj, types->m_ModelJNI.parentBone, C2J_CreateBone(env, types, model->m_ParentBone));
    dmJNI::SetObjectDeref(env, obj, types->m_ModelJNI.meshes, CreateMeshesArray(env, types, materials, model->m_Meshes));
    return obj;
}

// Create array or models, sorted on model index
static void CreateModels(JNIEnv* env, jni::TypeInfos* types, const dmModelImporter::Scene* scene, const dmArray<jobject>& materials, dmArray<jobject>& models)
{
    uint32_t count = scene->m_Models.Size();
    models.SetCapacity(count);
    models.SetSize(count);

    for (uint32_t i = 0; i < count; ++i)
    {
        const Model* model = &scene->m_Models[i];
        models[model->m_Index] = CreateModel(env, types, materials, model);
    }
}

// **************************************************
// Animations

static jobject CreateNodeAnimation(JNIEnv* env, jni::TypeInfos* types, const dmModelImporter::NodeAnimation* node_anim, const dmArray<jobject>& nodes)
{
    jobject obj = env->AllocObject(types->m_NodeAnimationJNI.cls);
    dmJNI::SetObject(env, obj, types->m_NodeAnimationJNI.node, nodes[node_anim->m_Node->m_Index]);

    dmJNI::SetFloat(env, obj, types->m_NodeAnimationJNI.startTime, node_anim->m_StartTime);
    dmJNI::SetFloat(env, obj, types->m_NodeAnimationJNI.endTime, node_anim->m_EndTime);
    dmJNI::SetObjectDeref(env, obj, types->m_NodeAnimationJNI.translationKeys, jni::C2J_CreateKeyFrameArray(env, types, node_anim->m_TranslationKeys.Begin(), node_anim->m_TranslationKeys.Size()));
    dmJNI::SetObjectDeref(env, obj, types->m_NodeAnimationJNI.rotationKeys, jni::C2J_CreateKeyFrameArray(env, types, node_anim->m_RotationKeys.Begin(), node_anim->m_RotationKeys.Size()));
    dmJNI::SetObjectDeref(env, obj, types->m_NodeAnimationJNI.scaleKeys, jni::C2J_CreateKeyFrameArray(env, types, node_anim->m_ScaleKeys.Begin(), node_anim->m_ScaleKeys.Size()));
    return obj;
}

static jobjectArray CreateNodeAnimationsArray(JNIEnv* env, jni::TypeInfos* types, const dmArray<dmModelImporter::NodeAnimation>& node_animations, const dmArray<jobject>& nodes)
{
    uint32_t count = node_animations.Size();
    jobjectArray arr = env->NewObjectArray(count, types->m_NodeAnimationJNI.cls, 0);
    for (uint32_t i = 0; i < count; ++i)
    {
        jobject o = CreateNodeAnimation(env, types, &node_animations[i], nodes);
        env->SetObjectArrayElement(arr, i, o);
        env->DeleteLocalRef(o);
    }
    return arr;
}

static jobject CreateAnimation(JNIEnv* env, jni::TypeInfos* types, const dmModelImporter::Animation* animation, const dmArray<jobject>& nodes)
{
    jobject obj = env->AllocObject(types->m_AnimationJNI.cls);
    dmJNI::SetString(env, obj, types->m_AnimationJNI.name, animation->m_Name);
    dmJNI::SetFloat(env, obj, types->m_AnimationJNI.duration, animation->m_Duration);

    jobjectArray arr = CreateNodeAnimationsArray(env, types, animation->m_NodeAnimations, nodes);
    dmJNI::SetObjectDeref(env, obj, types->m_AnimationJNI.nodeAnimations, arr);
    return obj;
}

static jobjectArray CreateAnimationsArray(JNIEnv* env, jni::TypeInfos* types, const dmArray<dmModelImporter::Animation>& animations, const dmArray<jobject>& nodes)
{
    uint32_t count = animations.Size();
    jobjectArray arr = env->NewObjectArray(count, types->m_AnimationJNI.cls, 0);
    for (uint32_t i = 0; i < count; ++i)
    {
        env->SetObjectArrayElement(arr, i, CreateAnimation(env, types, &animations[i], nodes));
    }
    return arr;
}

// **************************************************
// Bones

static jobject CreateBone(JNIEnv* env, jni::TypeInfos* types, const dmModelImporter::Bone* bone)
{
    jobject obj = env->AllocObject(types->m_BoneJNI.cls);
    // Subset of jni::C2J_CreateBone()
    dmJNI::SetObjectDeref(env, obj, types->m_BoneJNI.invBindPose, jni::C2J_CreateTransform(env, types, &bone->m_InvBindPose));
    dmJNI::SetString(env, obj, types->m_BoneJNI.name, bone->m_Name);
    dmJNI::SetInt(env, obj, types->m_BoneJNI.index, bone->m_Index);
    //dmJNI::SetObjectDeref(env, obj, types->m_BoneJNI.parent, C2J_CreateBone(env, types, bone->m_Parent));
    //dmJNI::SetObjectDeref(env, obj, types->m_BoneJNI.node, C2J_CreateNode(env, types, bone->m_Node));
    //dmJNI::SetObjectDeref(env, obj, types->m_BoneJNI.children, C2J_CreateBonePtrArray(env, types, bone->m_Children.Begin(), bone->m_Children.Size()));
    return obj;
}

static jobjectArray CreateBonesArray(JNIEnv* env, jni::TypeInfos* types, const dmArray<dmModelImporter::Bone*>& bones, const dmArray<jobject>& nodes)
{
    dmArray<jobject> tmp;

    uint32_t count = bones.Size();
    tmp.SetCapacity(count);
    tmp.SetSize(count);

    jobjectArray arr = env->NewObjectArray(count, types->m_BoneJNI.cls, 0);
    for (uint32_t i = 0; i < count; ++i)
    {
        const dmModelImporter::Bone* bone = bones[i];
        tmp[bone->m_Index] = CreateBone(env, types, bone);
        env->SetObjectArrayElement(arr, i, tmp[bone->m_Index]);
    }

    // Set the bone parents now that each bone is actually created
    for (uint32_t i = 0; i < count; ++i)
    {
        const dmModelImporter::Bone* bone = bones[i];
        jobject o = tmp[bone->m_Index];
        if (bone->m_Node != 0) // A generated root bone doesn't have a corresponding Node
            dmJNI::SetObject(env, o, types->m_BoneJNI.node, nodes[bone->m_Node->m_Index]);

        if (bone->m_Parent != 0)
            dmJNI::SetObject(env, o, types->m_BoneJNI.parent, tmp[bone->m_Parent->m_Index]);
        // else
        //     dmJNI::SetObject(env, tmp[bone->m_Index], types->m_BoneJNI.parent, 0);


        jobjectArray children = env->NewObjectArray(bone->m_Children.Size(), types->m_BoneJNI.cls, 0);
        for (uint32_t i = 0; i < bone->m_Children.Size(); ++i)
        {
            const dmModelImporter::Bone* child_bone = bone->m_Children[i];
            env->SetObjectArrayElement(children, i, tmp[child_bone->m_Index]);
        }
        dmJNI::SetObjectDeref(env, o, types->m_BoneJNI.children, children);
    }

    return arr;
}

static void CreateBones(JNIEnv* env, jni::TypeInfos* types, const dmModelImporter::Scene* scene, const dmArray<jobject>& skins, const dmArray<jobject>& nodes)
{
    uint32_t count = scene->m_Skins.Size();
    for (uint32_t i = 0; i < count; ++i)
    {
        const Skin* skin = &scene->m_Skins[i];
        jobject skin_obj = skins[skin->m_Index];

        jobjectArray arr = CreateBonesArray(env, types, skin->m_Bones, nodes);
        dmJNI::SetObjectDeref(env, skin_obj, types->m_SkinJNI.bones, arr);
    }
}

// **************************************************
// Skins

// static jobject CreateSkin(JNIEnv* env, jni::TypeInfos* types, dmModelImporter::Skin* skin)
// {
//     jobject obj = env->AllocObject(types->m_SkinJNI.cls);
//     SetInt(env, obj, types->m_SkinJNI.index, skin->m_Index);
//     SetString(env, obj, types->m_SkinJNI.name, skin->m_Name);
//     return obj;
// }

// Creates a list of skins, sorted on index
static void CreateSkins(JNIEnv* env, jni::TypeInfos* types, const dmModelImporter::Scene* scene, dmArray<jobject>& skins)
{
    uint32_t count = scene->m_Skins.Size();
    skins.SetCapacity(count);
    skins.SetSize(count);

    for (uint32_t i = 0; i < count; ++i)
    {
        const Skin* skin = &scene->m_Skins[i];
        skins[skin->m_Index] = jni::C2J_CreateSkin(env, types, skin);
    }
}

static void DeleteLocalRefs(JNIEnv* env, dmArray<jobject>& objects)
{
    for (uint32_t i = 0; i < objects.Size(); ++i)
    {
        env->DeleteLocalRef(objects[i]);
    }
}

static jobject CreateJavaScene(JNIEnv* env, jni::TypeInfos* types, const dmModelImporter::Scene* scene)
{
    jobject obj = env->AllocObject(types->m_SceneJNI.cls);

    dmArray<jobject> models;
    dmArray<jobject> skins;
    dmArray<jobject> nodes;
    dmArray<jobject> materials;

    dmJNI::SetObjectDeref(env, obj, types->m_SceneJNI.buffers, jni::C2J_CreateBufferArray(env, types, scene->m_Buffers.Begin(), scene->m_Buffers.Size()));

    jobjectArray materialArray = CreateMaterialsArray(env, types, scene->m_Materials, scene->m_DynamicMaterials, materials);
    dmJNI::SetObjectDeref(env, obj, types->m_SceneJNI.materials, materialArray);

    // {
    //     jobjectArray arr = CreateBuffersArray(env, &types, scene->m_BuffersCount, scene->m_Buffers);
    //     env->SetObjectField(obj, types.m_SceneJNI.buffers, arr);
    //     env->DeleteLocalRef(arr);
    // }

    // {
    //     jobjectArray arr = CreateMaterialsArray(env, &types, scene->m_MaterialsCount, scene->m_Materials, materials);
    //     env->SetObjectField(obj, types.m_SceneJNI.materials, arr);
    //     env->DeleteLocalRef(arr);
    // }

    // Creates all nodes, and leaves out setting skins/models
    CreateNodes(env, types, scene, nodes);

    CreateSkins(env, types, scene, skins);
    CreateModels(env, types, scene, materials, models);
    CreateBones(env, types, scene, skins, nodes);

    // Set the skin+model to the nodes
    FixupNodeReferences(env, types, scene, skins, models, nodes);

    ///
    {
        //jobjectArray arr = CreateObjectArray(env, types.m_NodeJNI.cls, nodes);
        // env->SetObjectField(obj, types.m_SceneJNI.nodes, arr);
        // env->DeleteLocalRef(arr);
        dmJNI::SetObjectDeref(env, obj, types->m_SceneJNI.nodes, CreateObjectArray(env, types->m_NodeJNI.cls, nodes));
    }

    {
        uint32_t count = scene->m_RootNodes.Size();
        jobjectArray arr = env->NewObjectArray(count, types->m_NodeJNI.cls, 0);
        for (uint32_t i = 0; i < count; ++i)
        {
            dmModelImporter::Node* root = scene->m_RootNodes[i];
            env->SetObjectArrayElement(arr, i, nodes[root->m_Index]);
        }
        // env->SetObjectField(obj, types->m_SceneJNI.rootNodes, arr);
        // env->DeleteLocalRef(arr);
        dmJNI::SetObjectDeref(env, obj, types->m_SceneJNI.rootNodes, arr);
    }

    dmJNI::SetObjectDeref(env, obj, types->m_SceneJNI.skins, CreateObjectArray(env, types->m_SkinJNI.cls, skins));
    // {
    //     jobjectArray arr = CreateObjectArray(env, types->m_SkinJNI.cls, skins);
    //     env->SetObjectField(obj, types->m_SceneJNI.skins, arr);
    //     env->DeleteLocalRef(arr);
    // }

    dmJNI::SetObjectDeref(env, obj, types->m_SceneJNI.models, CreateObjectArray(env, types->m_ModelJNI.cls, models));
    // {
    //     jobjectArray arr = CreateObjectArray(env, types->m_ModelJNI.cls, models);
    //     env->SetObjectField(obj, types->m_SceneJNI.models, arr);
    //     env->DeleteLocalRef(arr);
    // }

    {
        //jobjectArray arr = CreateAnimationsArray(env, &types, scene->m_Animations, nodes);
        dmJNI::SetObjectDeref(env, obj, types->m_SceneJNI.animations, CreateAnimationsArray(env, types, scene->m_Animations, nodes));
        // env->SetObjectField(obj, types->m_SceneJNI.animations, arr);
        // env->DeleteLocalRef(arr);
    }

    DeleteLocalRefs(env, nodes);
    DeleteLocalRefs(env, skins);
    DeleteLocalRefs(env, models);
    DeleteLocalRefs(env, materials);

    return obj;
}

} // namespace

static jobject LoadFromBufferInternal(JNIEnv* env, dmModelImporter::jni::TypeInfos* types, jclass cls, jstring _path, jbyteArray array, jobject data_resolver)
{
    dmJNI::ScopedString j_path(env, _path);
    const char* path = j_path.m_String;

    const char* suffix = strrchr(path, '.');
    if (!suffix) {
        dmLogError("No suffix found in path: %s", path);
        return 0;
    } else {
        suffix++; // skip the '.'
    }

    jsize file_size = env->GetArrayLength(array);
    jbyte* file_data = env->GetByteArrayElements(array, 0);

    dmModelImporter::Options options;
    dmModelImporter::Scene* scene = dmModelImporter::LoadFromBuffer(&options, suffix, (uint8_t*)file_data, file_size);

    if (!scene)
    {
        dmLogError("Failed to load %s", path);
        return 0;
    }

    bool resolved = false;
    if (data_resolver != 0 && dmModelImporter::NeedsResolve(scene))
    {
        jclass cls_resolver = env->GetObjectClass(data_resolver);
        jmethodID get_data = env->GetMethodID(cls_resolver, "getData", "(Ljava/lang/String;Ljava/lang/String;)[B");

        for (uint32_t i = 0; i < scene->m_Buffers.Size(); ++i)
        {
            if (scene->m_Buffers[i].m_Buffer)
                continue;

            const char* uri = scene->m_Buffers[i].m_Uri;
            jstring j_uri = env->NewStringUTF(uri);

            jbyteArray bytes = (jbyteArray)env->CallObjectMethod(data_resolver, get_data, _path, j_uri);
            if (bytes)
            {
                dmLogDebug("Found buffer for %s!\n", uri);

                jsize buffer_size = env->GetArrayLength(bytes);
                jbyte* buffer_data = env->GetByteArrayElements(bytes, 0);
                dmModelImporter::ResolveBuffer(scene, scene->m_Buffers[i].m_Uri, buffer_data, buffer_size);
                resolved = true;

                env->ReleaseByteArrayElements(bytes, buffer_data, JNI_ABORT);
            }
            else {
                dmLogDebug("Found no buffer for uri '%s'\n", uri);
            }
            env->DeleteLocalRef(j_uri);
        }

        if(dmModelImporter::NeedsResolve(scene))
        {
            dmLogWarning("The model is still missing buffers!");
        }
    }

    if (resolved && !dmModelImporter::NeedsResolve(scene))
    {
        dmModelImporter::LoadFinalize(scene);
        dmModelImporter::Validate(scene);
    }

    if (dmLogGetLevel() == LOG_SEVERITY_DEBUG) // verbose mode
    {
        dmModelImporter::DebugScene(scene);
    }

    jobject jscene = dmModelImporter::CreateJavaScene(env, types, scene);

    dmModelImporter::DestroyScene(scene);

    env->ReleaseByteArrayElements(array, file_data, JNI_ABORT);

    return jscene;
}

JNIEXPORT jobject JNICALL Java_Modelc_LoadFromBufferInternal(JNIEnv* env, jclass cls, jstring _path, jbyteArray array, jobject data_resolver)
{
    dmLogDebug("Java_Modelc_LoadFromBufferInternal: env = %p\n", env);
    DM_SCOPED_SIGNAL_CONTEXT(env, return 0;); // Gather any C callstacks
    dmModelImporter::jni::ScopedContext jni_scope(env); // Jni types

    jobject jscene;
    DM_JNI_GUARD_SCOPE_BEGIN(); // C callstack, needed for MSVC
        jscene = LoadFromBufferInternal(env, &jni_scope.m_TypeInfos, cls, _path, array, data_resolver);
    DM_JNI_GUARD_SCOPE_END(return 0;);
    return jscene;
}

// JNIEXPORT jint JNICALL Java_Modelc_AddressOf(JNIEnv* env, jclass cls, jobject object)
// {
//     return dmModelImporter::AddressOf(object);
// }

JNIEXPORT void JNICALL Java_Modelc_TestException(JNIEnv* env, jclass cls, jstring j_message)
{
    DM_SCOPED_SIGNAL_CONTEXT(env, return;); // Gather any C callstacks
    dmModelImporter::jni::ScopedContext jni_scope(env); // Jni types

    dmJNI::ScopedString s_message(env, j_message);
    const char* message = s_message.m_String;
    printf("Received message: %s\n", message);
    dmJNI::TestSignalFromString(message);
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {

    dmLogDebug("JNI_OnLoad ->\n");
    dmJNI::EnableDefaultSignalHandlers(vm);

    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8) != JNI_OK) {
        printf("JNI_OnLoad GetEnv error\n");
        return JNI_ERR;
    }

    // Find your class. JNI_OnLoad is called from the correct class loader context for this to work.
    jclass c = dmJNI::GetClass(env, "com/dynamo/bob/pipeline/Modelc", 0);
    dmLogDebug("JNI_OnLoad: c = %p\n", c);
    if (c == 0)
      return JNI_ERR;

    // Register your class' native methods.
    // Don't forget to add them to the corresponding java file (e.g. ModelImporter.java)
    static const JNINativeMethod methods[] = {
        {(char*)"LoadFromBufferInternal", (char*)"(Ljava/lang/String;[BLjava/lang/Object;)L" CLASS_SCENE ";", reinterpret_cast<void*>(Java_Modelc_LoadFromBufferInternal)},
        //{(char*)"AddressOf", (char*)"(Ljava/lang/Object;)I", reinterpret_cast<void*>(Java_Modelc_AddressOf)},
        {(char*)"TestException", (char*)"(Ljava/lang/String;)V", reinterpret_cast<void*>(Java_Modelc_TestException)},
    };
    int rc = env->RegisterNatives(c, methods, sizeof(methods)/sizeof(JNINativeMethod));
    env->DeleteLocalRef(c);

    if (rc != JNI_OK) return rc;

    dmLogDebug("JNI_OnLoad return.\n");
    return JNI_VERSION_1_8;
}

JNIEXPORT void JNI_OnUnload(JavaVM *vm, void *reserved)
{
    dmLogDebug("JNI_OnUnload ->\n");
}
