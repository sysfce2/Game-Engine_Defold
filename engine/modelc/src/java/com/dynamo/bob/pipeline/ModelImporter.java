// generated, do not edit

package com.dynamo.bob.pipeline;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.IOException;
import java.lang.reflect.Method;

public class ModelImporter {
    public static final int INVALID_INDEX = 2147483647;
    public static class Vec3f {
        public float x = 0.0f;
        public float y = 0.0f;
        public float z = 0.0f;
    };
    public static class Vec4f {
        public float x = 0.0f;
        public float y = 0.0f;
        public float z = 0.0f;
        public float w = 0.0f;
    };
    public static class Aabb {
        public Vec3f min;
        public Vec3f max;
    };
    public static class Transform {
        public Vec3f translation;
        public Vec3f scale;
        public Vec4f rotation;
    };
    public static class Material {
        public String name;
        public int index = 0;
        public byte isSkinned = 0;
    };
    public static class Mesh {
        public String name;
        public Material material;
        public Aabb aabb;
        public int vertexCount = 0;
        public float[] positions;
        public float[] normals;
        public float[] tangents;
        public float[] colors;
        public float[] weights;
        public float[] texCoord0;
        public float[] texCoord1;
        public int texCoord0NumComponents = 0;
        public int texCoord1NumComponents = 0;
        public int[] bones;
        public int[] indices;
    };
    public static class Model {
        public String name;
        public Mesh[] meshes;
        public int index = 0;
        public Bone parentBone;
    };
    public static class Bone {
        public Transform invBindPose;
        public String name;
        public Node node;
        public Bone parent;
        public int index = 0;
        public Bone[] children;
    };
    public static class Skin {
        public String name;
        public int index = 0;
        public Bone[] bones;
        public int[] boneRemap;
    };
    public static class Node {
        public Transform local;
        public Transform world;
        public String name;
        public Model model;
        public Skin skin;
        public Node parent;
        public Node[] children;
        public int index = 0;
        public long nameHash = 0;
    };
    public static class KeyFrame {
        public float[] value;
        public float time = 0.0f;
    };
    public static class NodeAnimation {
        public Node node;
        public float startTime = 0.0f;
        public float endTime = 0.0f;
        public KeyFrame[] translationKeys;
        public KeyFrame[] rotationKeys;
        public KeyFrame[] scaleKeys;
    };
    public static class Animation {
        public String name;
        public float duration = 0.0f;
        public NodeAnimation[] nodeAnimations;
    };
    public static class Buffer {
        public String uri;
        public long buffer;
        public int bufferSize = 0;
    };
    public static class Scene {
        public long opaqueSceneData;
        public Node[] nodes;
        public Model[] models;
        public Skin[] skins;
        public Node[] rootNodes;
        public Animation[] animations;
        public Material[] materials;
        public Material[] dynamicMaterials;
        public Buffer[] buffers;
    };
    public static class Options {
        public int dummy = 0;
    };
}

