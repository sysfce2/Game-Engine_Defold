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

#include <string.h>
#include <assert.h>
#include <dmsdk/dlib/vmath.h>

#include <dlib/array.h>
#include <dlib/dstrings.h>
#include <dlib/log.h>
#include <dlib/math.h>

#include "../graphics_private.h"
#include "../graphics_native.h"
#include "../graphics_adapter.h"
#include "graphics_webgpu_private.h"

#include "../null/glsl_uniform_parser.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgpu.h>

#include <webgpu/webgpu.h>
#include <webgpu/webgpu_cpp.h>

#include <dmsdk/graphics/glfw/glfw.h>
#include <graphics/glfw/glfw_native.h>

uint64_t g_DrawCount = 0;
uint64_t g_Flipped = 0;

// Used only for tests
bool g_ForceFragmentReloadFail = false;
bool g_ForceVertexReloadFail = false;

namespace dmGraphics
{
    using namespace dmVMath;

    uint16_t TYPE_SIZE[] =
    {
        sizeof(char), // TYPE_BYTE
        sizeof(unsigned char), // TYPE_UNSIGNED_BYTE
        sizeof(short), // TYPE_SHORT
        sizeof(unsigned short), // TYPE_UNSIGNED_SHORT
        sizeof(int), // TYPE_INT
        sizeof(unsigned int), // TYPE_UNSIGNED_INT
        sizeof(float) // TYPE_FLOAT
    };

    static GraphicsAdapterFunctionTable WebGPURegisterFunctionTable();
    static bool                         WebGPUIsSupported();
    static const int8_t      g_webgpu_adapter_priority = 0;
    static GraphicsAdapter   g_webgpu_adapter(ADAPTER_TYPE_WEBGPU);
    static WebGPUContext*    g_WebGPUContext = 0x0;

    DM_REGISTER_GRAPHICS_ADAPTER(GraphicsAdapterWebGPU, &g_webgpu_adapter, WebGPUIsSupported, WebGPURegisterFunctionTable, g_webgpu_adapter_priority);

    static void print_wgpu_error(WGPUErrorType error_type, const char* message, void*)
    {
        const char* error_type_lbl = "";
        switch (error_type)
        {
        case WGPUErrorType_Validation:  error_type_lbl = "Validation"; break;
        case WGPUErrorType_OutOfMemory: error_type_lbl = "Out of memory"; break;
        case WGPUErrorType_Unknown:     error_type_lbl = "Unknown"; break;
        case WGPUErrorType_DeviceLost:  error_type_lbl = "Device lost"; break;
        default:                        error_type_lbl = "Unknown";
        }
        dmLogError("%s error: %s\n", error_type_lbl, message);
    }

    static bool WebGPUInitialize()
    {
        return (glfwInit() == GL_TRUE);
    }

    static void WebGPUFinalize()
    {
        glfwTerminate();
    }

    WebGPUContext::WebGPUContext(const ContextParams& params)
    {
        memset(this, 0, sizeof(*this));
        m_DefaultTextureMinFilter = params.m_DefaultTextureMinFilter;
        m_DefaultTextureMagFilter = params.m_DefaultTextureMagFilter;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_LUMINANCE;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_LUMINANCE_ALPHA;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGB;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGBA;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGB_16BPP;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGBA_16BPP;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGB_ETC1;
    }

    static HContext WebGPUNewContext(const ContextParams& params)
    {
        if (!g_WebGPUContext)
        {
            if (glfwInit() == GL_FALSE)
            {
                dmLogError("Could not initialize glfw.");
                return 0x0;
            }

            g_WebGPUContext = new WebGPUContext(params);
            return g_WebGPUContext;
        }
        else
        {
            return 0x0;
        }
    }

    static bool WebGPUIsSupported()
    {
        return true;
    }

    static void WebGPUDeleteContext(HContext context)
    {
        assert(context);
        if (g_WebGPUContext)
        {
            delete (WebGPUContext*) context;
            g_WebGPUContext = 0x0;
        }
    }

    static void OnWindowResize(int width, int height)
    {
        assert(g_WebGPUContext);
        g_WebGPUContext->m_WindowWidth = (uint32_t)width;
        g_WebGPUContext->m_WindowHeight = (uint32_t)height;
        if (g_WebGPUContext->m_WindowResizeCallback != 0x0)
            g_WebGPUContext->m_WindowResizeCallback(g_WebGPUContext->m_WindowResizeCallbackUserData, (uint32_t)width, (uint32_t)height);
    }

    static int OnWindowClose()
    {
        assert(g_WebGPUContext);
        if (g_WebGPUContext->m_WindowCloseCallback != 0x0)
            return g_WebGPUContext->m_WindowCloseCallback(g_WebGPUContext->m_WindowCloseCallbackUserData);
        // Close by default
        return 1;
    }

    static void OnWindowFocus(int focus)
    {
        assert(g_WebGPUContext);
        if (g_WebGPUContext->m_WindowFocusCallback != 0x0)
            g_WebGPUContext->m_WindowFocusCallback(g_WebGPUContext->m_WindowFocusCallbackUserData, focus);
    }

    static void OnWindowIconify(int iconify)
    {
        assert(g_WebGPUContext);
        if (g_WebGPUContext->m_WindowIconifyCallback != 0x0)
            g_WebGPUContext->m_WindowIconifyCallback(g_WebGPUContext->m_WindowIconifyCallbackUserData, iconify);
    }

    static WindowResult WebGPUOpenWindow(HContext _context, WindowParams* params)
    {
        assert(_context);
        assert(params);

        WebGPUContext* context = (WebGPUContext*) _context;

        if (context->m_WindowOpened)
        {
            return WINDOW_RESULT_ALREADY_OPENED;
        }

        context->m_WindowResizeCallback           = params->m_ResizeCallback;
        context->m_WindowResizeCallbackUserData   = params->m_ResizeCallbackUserData;
        context->m_WindowCloseCallback            = params->m_CloseCallback;
        context->m_WindowCloseCallbackUserData    = params->m_CloseCallbackUserData;
        context->m_WindowFocusCallback            = params->m_FocusCallback;
        context->m_WindowFocusCallbackUserData    = params->m_FocusCallbackUserData;
        context->m_WindowIconifyCallback          = params->m_IconifyCallback;
        context->m_WindowIconifyCallbackUserData  = params->m_IconifyCallbackUserData;
        context->m_WindowOpened                   = 1;
        context->m_Width                          = params->m_Width;
        context->m_Height                         = params->m_Height;

        context->m_WindowWidth = params->m_Width;
        context->m_WindowHeight = params->m_Height;
        context->m_Dpi = 0;
        context->m_WindowOpened = 1;
        uint32_t buffer_size = 4 * context->m_WindowWidth * context->m_WindowHeight;
        context->m_MainFrameBuffer.m_ColorBuffer[0] = new char[buffer_size];
        context->m_MainFrameBuffer.m_ColorBufferSize[0] = buffer_size;
        context->m_MainFrameBuffer.m_DepthBuffer = new char[buffer_size];
        context->m_MainFrameBuffer.m_StencilBuffer = new char[buffer_size];
        context->m_MainFrameBuffer.m_DepthBufferSize = buffer_size;
        context->m_MainFrameBuffer.m_StencilBufferSize = buffer_size;
        context->m_CurrentFrameBuffer = &context->m_MainFrameBuffer;
        context->m_Program = 0x0;
        context->m_PipelineState = GetDefaultPipelineState();

        int mode = GLFW_WINDOW;
        if (params->m_Fullscreen)
        {
            mode = GLFW_FULLSCREEN;
        }

        if (!glfwOpenWindow(params->m_Width, params->m_Height, 8, 8, 8, 8, 32, 8, mode))
        {
            return WINDOW_RESULT_WINDOW_OPEN_ERROR;
        }

        glfwSetWindowTitle(params->m_Title);
        glfwSetWindowBackgroundColor(params->m_BackgroundColor);

        glfwSetWindowSizeCallback(OnWindowResize);
        glfwSetWindowCloseCallback(OnWindowClose);
        glfwSetWindowFocusCallback(OnWindowFocus);
        glfwSetWindowIconifyCallback(OnWindowIconify);
        glfwSwapInterval(1);

        context->m_Device = emscripten_webgpu_get_device();

        if (!context->m_Device)
        {
            dmLogError("Unable to get WebGPU device");
            return WINDOW_RESULT_WINDOW_OPEN_ERROR;
        }

        wgpuDeviceSetUncapturedErrorCallback(context->m_Device, print_wgpu_error, NULL);

        wgpu::SurfaceDescriptorFromCanvasHTMLSelector html_surface_desc{};
        html_surface_desc.selector = "#canvas";
        wgpu::SurfaceDescriptor surface_desc{};
        surface_desc.nextInChain = &html_surface_desc;

        // Use 'null' instance
        wgpu::Instance instance{};
        context->m_Surface = instance.CreateSurface(&surface_desc).Release();

        if (params->m_PrintDeviceInfo)
        {
            dmLogInfo("Device: WebGPU");
        }
        return WINDOW_RESULT_OK;
    }

    static uint32_t WebGPUGetWindowRefreshRate(HContext context)
    {
        if (((WebGPUContext*) context)->m_WindowOpened)
            return glfwGetWindowRefreshRate();
        else
            return 0;
    }

    static void WebGPUCloseWindow(HContext _context)
    {
        assert(_context);
        WebGPUContext* context = (WebGPUContext*) _context;

        if (context->m_WindowOpened)
        {
            glfwCloseWindow();
            FrameBuffer& main = context->m_MainFrameBuffer;
            delete [] (char*)main.m_ColorBuffer[0];
            delete [] (char*)main.m_DepthBuffer;
            delete [] (char*)main.m_StencilBuffer;
            context->m_WindowOpened = 0;
            context->m_Width = 0;
            context->m_Height = 0;
            context->m_WindowWidth = 0;
            context->m_WindowHeight = 0;
        }
    }

    static void WebGPUIconifyWindow(HContext context)
    {
        assert(context);
        if (((WebGPUContext*) context)->m_WindowOpened)
        {
            glfwIconifyWindow();
        }
    }

    static void WebGPURunApplicationLoop(void* user_data, WindowStepMethod step_method, WindowIsRunning is_running)
    {
    #ifdef __EMSCRIPTEN__
        while (0 != is_running(user_data))
        {
            // N.B. Beyond the first test, the above statement is essentially formal since set_main_loop will throw an exception.
            emscripten_set_main_loop_arg(step_method, user_data, 0, 1);
        }
    #else
        while (0 != is_running(user_data))
        {
            step_method(user_data);
        }
    #endif
    }

    static uint32_t WebGPUGetWindowState(HContext context, WindowState state)
    {
        if (((WebGPUContext*) context)->m_WindowOpened)
            return glfwGetWindowParam(state);
        else
            return 0;
    }

    static uint32_t WebGPUGetDisplayDpi(HContext context)
    {
        assert(context);
        return ((WebGPUContext*) context)->m_Dpi;
    }

    static uint32_t WebGPUGetWidth(HContext context)
    {
        return ((WebGPUContext*) context)->m_Width;
    }

    static uint32_t WebGPUGetHeight(HContext context)
    {
        return ((WebGPUContext*) context)->m_Height;
    }

    static uint32_t WebGPUGetWindowWidth(HContext context)
    {
        return ((WebGPUContext*) context)->m_WindowWidth;
    }

    static float WebGPUGetDisplayScaleFactor(HContext context)
    {
        return glfwGetDisplayScaleFactor();
    }

    static uint32_t WebGPUGetWindowHeight(HContext context)
    {
        return ((WebGPUContext*) context)->m_WindowHeight;
    }

    static void WebGPUSetWindowSize(HContext _context, uint32_t width, uint32_t height)
    {
        assert(_context);
        WebGPUContext* context = (WebGPUContext*) _context;
        if (context->m_WindowOpened)
        {
            FrameBuffer& main = context->m_MainFrameBuffer;
            delete [] (char*)main.m_ColorBuffer[0];
            delete [] (char*)main.m_DepthBuffer;
            delete [] (char*)main.m_StencilBuffer;
            context->m_Width = width;
            context->m_Height = height;
            context->m_WindowWidth = width;
            context->m_WindowHeight = height;
            uint32_t buffer_size = 4 * width * height;
            main.m_ColorBuffer[0] = new char[buffer_size];
            main.m_ColorBufferSize[0] = buffer_size;
            main.m_DepthBuffer = new char[buffer_size];
            main.m_DepthBufferSize = buffer_size;
            main.m_StencilBuffer = new char[buffer_size];
            main.m_StencilBufferSize = buffer_size;

            if (context->m_WindowResizeCallback)
                context->m_WindowResizeCallback(context->m_WindowResizeCallbackUserData, width, height);
        }
    }

    static void WebGPUResizeWindow(HContext _context, uint32_t width, uint32_t height)
    {
        assert(_context);
        WebGPUContext* context = (WebGPUContext*) _context;
        if (context->m_WindowOpened)
        {
            context->m_WindowWidth = width;
            context->m_WindowHeight = height;

            if (context->m_WindowResizeCallback)
                context->m_WindowResizeCallback(context->m_WindowResizeCallbackUserData, width, height);
        }
    }

    static void WebGPUGetDefaultTextureFilters(HContext _context, TextureFilter& out_min_filter, TextureFilter& out_mag_filter)
    {
        WebGPUContext* context = (WebGPUContext*) _context;
        out_min_filter = context->m_DefaultTextureMinFilter;
        out_mag_filter = context->m_DefaultTextureMagFilter;
    }

    static void WebGPUClear(HContext _context, uint32_t flags, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha, float depth, uint32_t stencil)
    {
        assert(_context);

        WebGPUContext* context = (WebGPUContext*) _context;
    }

    static void WebGPUBeginFrame(HContext context)
    {
    }

    static void WebGPUFlip(HContext _context)
    {
        glfwSwapBuffers();
    }

    static void WebGPUSetSwapInterval(HContext /*context*/, uint32_t swap_interval)
    {
        glfwSwapInterval(swap_interval);
    }

    static HVertexBuffer WebGPUNewVertexBuffer(HContext context, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        VertexBuffer* vb = new VertexBuffer();
        vb->m_Buffer = new char[size];
        vb->m_Copy = 0x0;
        vb->m_Size = size;
        if (size > 0 && data != 0x0)
            memcpy(vb->m_Buffer, data, size);
        return (uintptr_t)vb;
    }

    static void WebGPUDeleteVertexBuffer(HVertexBuffer buffer)
    {
        if (!buffer)
            return;
        VertexBuffer* vb = (VertexBuffer*)buffer;
        assert(vb->m_Copy == 0x0);
        delete [] vb->m_Buffer;
        delete vb;
    }

    static void WebGPUSetVertexBufferData(HVertexBuffer buffer, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        VertexBuffer* vb = (VertexBuffer*)buffer;
        assert(vb->m_Copy == 0x0);
        delete [] vb->m_Buffer;
        vb->m_Buffer = new char[size];
        vb->m_Size = size;
        if (data != 0x0)
            memcpy(vb->m_Buffer, data, size);
    }

    static void WebGPUSetVertexBufferSubData(HVertexBuffer buffer, uint32_t offset, uint32_t size, const void* data)
    {
        VertexBuffer* vb = (VertexBuffer*)buffer;
        if (offset + size <= vb->m_Size && data != 0x0)
            memcpy(&(vb->m_Buffer)[offset], data, size);
    }

    static uint32_t WebGPUGetMaxElementsVertices(HContext context)
    {
        return 65536;
    }

    static HIndexBuffer WebGPUNewIndexBuffer(HContext context, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        IndexBuffer* ib = new IndexBuffer();
        ib->m_Buffer = new char[size];
        ib->m_Copy = 0x0;
        ib->m_Size = size;
        memcpy(ib->m_Buffer, data, size);
        return (uintptr_t)ib;
    }

    static void WebGPUDeleteIndexBuffer(HIndexBuffer buffer)
    {
        if (!buffer)
            return;
        IndexBuffer* ib = (IndexBuffer*)buffer;
        assert(ib->m_Copy == 0x0);
        delete [] ib->m_Buffer;
        delete ib;
    }

    static void WebGPUSetIndexBufferData(HIndexBuffer buffer, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        IndexBuffer* ib = (IndexBuffer*)buffer;
        assert(ib->m_Copy == 0x0);
        delete [] ib->m_Buffer;
        ib->m_Buffer = new char[size];
        ib->m_Size = size;
        if (data != 0x0)
            memcpy(ib->m_Buffer, data, size);
    }

    static void WebGPUSetIndexBufferSubData(HIndexBuffer buffer, uint32_t offset, uint32_t size, const void* data)
    {
        IndexBuffer* ib = (IndexBuffer*)buffer;
        if (offset + size <= ib->m_Size && data != 0x0)
            memcpy(&(ib->m_Buffer)[offset], data, size);
    }

    static bool WebGPUIsIndexBufferFormatSupported(HContext context, IndexBufferFormat format)
    {
        return true;
    }

    static uint32_t WebGPUGetMaxElementsIndices(HContext context)
    {
        return 65536;
    }

    static HVertexDeclaration WebGPUNewVertexDeclarationStride(HContext context, HVertexStreamDeclaration stream_declaration, uint32_t stride)
    {
        return NewVertexDeclaration(context, stream_declaration);
    }

    static HVertexDeclaration WebGPUNewVertexDeclaration(HContext context, HVertexStreamDeclaration stream_declaration)
    {
        VertexDeclaration* vd = new VertexDeclaration();
        if (stream_declaration == 0)
        {
            memset(vd, 0, sizeof(*vd));
            return vd;
        }
        memcpy(&vd->m_StreamDeclaration, stream_declaration, sizeof(VertexStreamDeclaration));
        return vd;
    }

    bool WebGPUSetStreamOffset(HVertexDeclaration vertex_declaration, uint32_t stream_index, uint16_t offset)
    {
        if (stream_index > vertex_declaration->m_StreamDeclaration.m_StreamCount) {
            return false;
        }

        return true;
    }

    static void WebGPUDeleteVertexDeclaration(HVertexDeclaration vertex_declaration)
    {
        delete vertex_declaration;
    }

    static void EnableVertexStream(HContext context, uint16_t stream, uint16_t size, Type type, uint16_t stride, const void* vertex_buffer)
    {
        assert(context);
        assert(vertex_buffer);
        VertexStreamBuffer& s = ((WebGPUContext*) context)->m_VertexStreams[stream];
        assert(s.m_Source == 0x0);
        assert(s.m_Buffer == 0x0);
        s.m_Source = vertex_buffer;
        s.m_Size   = size * TYPE_SIZE[type - dmGraphics::TYPE_BYTE];
        s.m_Stride = stride;
    }

    static void DisableVertexStream(HContext context, uint16_t stream)
    {
        assert(context);
        VertexStreamBuffer& s = ((WebGPUContext*) context)->m_VertexStreams[stream];
        s.m_Size = 0;
        if (s.m_Buffer != 0x0)
        {
            delete [] (char*)s.m_Buffer;
            s.m_Buffer = 0x0;
        }
        s.m_Source = 0x0;
    }

    static void WebGPUEnableVertexDeclaration(HContext context, HVertexDeclaration vertex_declaration, HVertexBuffer vertex_buffer)
    {
        assert(context);
        assert(vertex_declaration);
        assert(vertex_buffer);
        VertexBuffer* vb = (VertexBuffer*)vertex_buffer;
        uint16_t stride = 0;

        for (uint32_t i = 0; i < vertex_declaration->m_StreamDeclaration.m_StreamCount; ++i)
        {
            stride += vertex_declaration->m_StreamDeclaration.m_Streams[i].m_Size * TYPE_SIZE[vertex_declaration->m_StreamDeclaration.m_Streams[i].m_Type - dmGraphics::TYPE_BYTE];
        }

        uint32_t offset = 0;
        for (uint16_t i = 0; i < vertex_declaration->m_StreamDeclaration.m_StreamCount; ++i)
        {
            VertexStream& stream = vertex_declaration->m_StreamDeclaration.m_Streams[i];
            if (stream.m_Size > 0)
            {
                EnableVertexStream(context, i, stream.m_Size, stream.m_Type, stride, &vb->m_Buffer[offset]);
                offset += stream.m_Size * TYPE_SIZE[stream.m_Type - dmGraphics::TYPE_BYTE];
            }
        }
    }

    static void WebGPUEnableVertexDeclarationProgram(HContext context, HVertexDeclaration vertex_declaration, HVertexBuffer vertex_buffer, HProgram program)
    {
        WebGPUEnableVertexDeclaration(context, vertex_declaration, vertex_buffer);
    }

    static void WebGPUDisableVertexDeclaration(HContext context, HVertexDeclaration vertex_declaration)
    {
        assert(context);
        assert(vertex_declaration);
        for (uint32_t i = 0; i < vertex_declaration->m_StreamDeclaration.m_StreamCount; ++i)
            if (vertex_declaration->m_StreamDeclaration.m_Streams[i].m_Size > 0)
                DisableVertexStream(context, i);
    }

    void WebGPUHashVertexDeclaration(HashState32 *state, HVertexDeclaration vertex_declaration)
    {
        for (int i = 0; i < vertex_declaration->m_StreamDeclaration.m_StreamCount; ++i)
        {
            VertexStream& stream = vertex_declaration->m_StreamDeclaration.m_Streams[i];
            dmHashUpdateBuffer32(state, &stream.m_NameHash,  sizeof(dmhash_t));
            dmHashUpdateBuffer32(state, &stream.m_Stream,    sizeof(stream.m_Stream));
            dmHashUpdateBuffer32(state, &stream.m_Size,      sizeof(stream.m_Size));
            dmHashUpdateBuffer32(state, &stream.m_Type,      sizeof(stream.m_Type));
            dmHashUpdateBuffer32(state, &stream.m_Normalize, sizeof(stream.m_Normalize));
        }
    }

    static void WebGPUDrawElements(HContext _context, PrimitiveType prim_type, uint32_t first, uint32_t count, Type type, HIndexBuffer index_buffer)
    {
        assert(_context);
        assert(index_buffer);
        WebGPUContext* context = (WebGPUContext*) _context;
    }

    static void WebGPUDraw(HContext context, PrimitiveType prim_type, uint32_t first, uint32_t count)
    {
        assert(context);
    }

    struct VertexProgram
    {
        char*                m_Data;
        ShaderDesc::Language m_Language;
    };

    struct FragmentProgram
    {
        char*                m_Data;
        ShaderDesc::Language m_Language;
    };

    static void WebGPUShaderResourceCallback(dmGraphics::GLSLUniformParserBindingType binding_type, const char* name, uint32_t name_length, dmGraphics::Type type, uint32_t size, uintptr_t userdata);

    struct ShaderBinding
    {
        ShaderBinding() : m_Name(0) {};
        char*    m_Name;
        uint32_t m_Index;
        uint32_t m_Size;
        uint32_t m_Stride;
        Type     m_Type;
    };

    struct Program
    {
        Program(VertexProgram* vp, FragmentProgram* fp)
        {
            m_Uniforms.SetCapacity(16);
            m_VP = vp;
            m_FP = fp;
            if (m_VP != 0x0)
            {
                GLSLAttributeParse(m_VP->m_Language, m_VP->m_Data, WebGPUShaderResourceCallback, (uintptr_t)this);
                GLSLUniformParse(m_VP->m_Language, m_VP->m_Data, WebGPUShaderResourceCallback, (uintptr_t)this);
            }
            if (m_FP != 0x0)
            {
                GLSLUniformParse(m_FP->m_Language, m_FP->m_Data, WebGPUShaderResourceCallback, (uintptr_t)this);
            }
        }

        ~Program()
        {
            for(uint32_t i = 0; i < m_Uniforms.Size(); ++i)
                delete[] m_Uniforms[i].m_Name;
        }

        VertexProgram*         m_VP;
        FragmentProgram*       m_FP;
        dmArray<ShaderBinding> m_Uniforms;
        dmArray<ShaderBinding> m_Attributes;
    };

    static void WebGPUShaderResourceCallback(dmGraphics::GLSLUniformParserBindingType binding_type, const char* name, uint32_t name_length, dmGraphics::Type type, uint32_t size, uintptr_t userdata)
    {
        Program* program = (Program*) userdata;

        dmArray<ShaderBinding>* binding_array = binding_type == GLSLUniformParserBindingType::UNIFORM ?
            &program->m_Uniforms : &program->m_Attributes;

        if(binding_array->Full())
        {
            binding_array->OffsetCapacity(16);
        }

        ShaderBinding binding;
        name_length++;
        binding.m_Name   = new char[name_length];
        binding.m_Index  = binding_array->Size();
        binding.m_Type   = type;
        binding.m_Size   = size;
        binding.m_Stride = GetTypeSize(type);

        dmStrlCpy(binding.m_Name, name, name_length);
        binding_array->Push(binding);
    }

    static HProgram WebGPUNewProgram(HContext context, HVertexProgram vertex_program, HFragmentProgram fragment_program)
    {
        VertexProgram* vertex     = 0x0;
        FragmentProgram* fragment = 0x0;
        if (vertex_program != INVALID_VERTEX_PROGRAM_HANDLE)
        {
            vertex = (VertexProgram*) vertex_program;
        }
        if (fragment_program != INVALID_FRAGMENT_PROGRAM_HANDLE)
        {
            fragment = (FragmentProgram*) fragment_program;
        }
        return (HProgram) new Program(vertex, fragment);
    }

    static void WebGPUDeleteProgram(HContext context, HProgram program)
    {
        delete (Program*) program;
    }

    static HVertexProgram WebGPUNewVertexProgram(HContext context, ShaderDesc::Shader* ddf)
    {
        assert(ddf);
        VertexProgram* p = new VertexProgram();
        p->m_Data = new char[ddf->m_Source.m_Count+1];
        memcpy(p->m_Data, ddf->m_Source.m_Data, ddf->m_Source.m_Count);
        p->m_Data[ddf->m_Source.m_Count] = '\0';
        p->m_Language = ddf->m_Language;
        return (uintptr_t)p;
    }

    static HFragmentProgram WebGPUNewFragmentProgram(HContext context, ShaderDesc::Shader* ddf)
    {
        assert(ddf);
        FragmentProgram* p = new FragmentProgram();
        p->m_Data = new char[ddf->m_Source.m_Count+1];
        memcpy(p->m_Data, ddf->m_Source.m_Data, ddf->m_Source.m_Count);
        p->m_Data[ddf->m_Source.m_Count] = '\0';
        p->m_Language = ddf->m_Language;
        return (uintptr_t)p;
    }

    static bool WebGPUReloadVertexProgram(HVertexProgram prog, ShaderDesc::Shader* ddf)
    {
        return true;
    }

    static bool WebGPUReloadFragmentProgram(HFragmentProgram prog, ShaderDesc::Shader* ddf)
    {
        return true;
    }

    static void WebGPUDeleteVertexProgram(HVertexProgram program)
    {
    }

    static void WebGPUDeleteFragmentProgram(HFragmentProgram program)
    {
    }

    static ShaderDesc::Language WebGPUGetShaderProgramLanguage(HContext context)
    {
        return ShaderDesc::LANGUAGE_WGSL;
    }

    static void WebGPUEnableProgram(HContext context, HProgram program)
    {
        assert(context);
    }

    static void WebGPUDisableProgram(HContext context)
    {
        assert(context);
    }

    static bool WebGPUReloadProgram(HContext context, HProgram program, HVertexProgram vert_program, HFragmentProgram frag_program)
    {
        (void) context;
        (void) program;

        return true;
    }

    static uint32_t WebGPUGetAttributeCount(HProgram prog)
    {
        return 0;
    }

    static void WebGPUGetAttribute(HProgram prog, uint32_t index, dmhash_t* name_hash, Type* type, uint32_t* element_count, uint32_t* num_values, int32_t* location)
    {
    }

    static uint32_t WebGPUGetUniformCount(HProgram prog)
    {
        return 0;
    }

    static uint32_t WebGPUGetVertexDeclarationStride(HVertexDeclaration vertex_declaration)
    {
        // TODO: We don't take alignment into account here. It is assumed to be tightly packed
        //       as opposed to other graphic adapters which requires a 4 byte minumum alignment per stream.
        //       Might need some investigation on impact, or adjustment in the future..
        uint32_t stride = 0;
        for (int i = 0; i < vertex_declaration->m_StreamDeclaration.m_StreamCount; ++i)
        {
            VertexStream& stream = vertex_declaration->m_StreamDeclaration.m_Streams[i];
            stride += GetTypeSize(stream.m_Type) * stream.m_Size;
        }
        return stride;
    }

    static uint32_t WebGPUGetUniformName(HProgram prog, uint32_t index, char* buffer, uint32_t buffer_size, Type* type, int32_t* size)
    {
        return 0;
    }

    static int32_t WebGPUGetUniformLocation(HProgram prog, const char* name)
    {
        return -1;
    }

    static void WebGPUSetViewport(HContext context, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        assert(context);
    }

    static void WebGPUSetConstantV4(HContext _context, const Vector4* data, int count, int base_register)
    {
    }

    static void WebGPUSetConstantM4(HContext _context, const Vector4* data, int count, int base_register)
    {
    }

    static void WebGPUSetSampler(HContext context, int32_t location, int32_t unit)
    {
    }

    static HRenderTarget WebGPUNewRenderTarget(HContext _context, uint32_t buffer_type_flags, const TextureCreationParams creation_params[MAX_BUFFER_TYPE_COUNT], const TextureParams params[MAX_BUFFER_TYPE_COUNT])
    {
        RenderTarget* rt          = new RenderTarget();
        WebGPUContext* context = (WebGPUContext*) _context;

        void** buffers[MAX_BUFFER_TYPE_COUNT] = {
            &rt->m_FrameBuffer.m_ColorBuffer[0],
            &rt->m_FrameBuffer.m_ColorBuffer[1],
            &rt->m_FrameBuffer.m_ColorBuffer[2],
            &rt->m_FrameBuffer.m_ColorBuffer[3],
            &rt->m_FrameBuffer.m_DepthBuffer,
            &rt->m_FrameBuffer.m_StencilBuffer,
        };
        uint32_t* buffer_sizes[MAX_BUFFER_TYPE_COUNT] = {
            &rt->m_FrameBuffer.m_ColorBufferSize[0],
            &rt->m_FrameBuffer.m_ColorBufferSize[1],
            &rt->m_FrameBuffer.m_ColorBufferSize[2],
            &rt->m_FrameBuffer.m_ColorBufferSize[3],
            &rt->m_FrameBuffer.m_DepthBufferSize,
            &rt->m_FrameBuffer.m_StencilBufferSize,
        };

        BufferType buffer_types[MAX_BUFFER_TYPE_COUNT] = {
            BUFFER_TYPE_COLOR0_BIT,
            BUFFER_TYPE_COLOR1_BIT,
            BUFFER_TYPE_COLOR2_BIT,
            BUFFER_TYPE_COLOR3_BIT,
            BUFFER_TYPE_DEPTH_BIT,
            BUFFER_TYPE_STENCIL_BIT,
        };

        for (uint32_t i = 0; i < MAX_BUFFER_TYPE_COUNT; ++i)
        {
            assert(GetBufferTypeIndex(buffer_types[i]) == i);

            if (buffer_type_flags & buffer_types[i])
            {
                uint32_t bytes_per_pixel                = dmGraphics::GetTextureFormatBitsPerPixel(params[i].m_Format) / 3;
                uint32_t buffer_size                    = sizeof(uint32_t) * params[i].m_Width * params[i].m_Height * bytes_per_pixel;
                *(buffer_sizes[i])                      = buffer_size;
                rt->m_BufferTextureParams[i]            = params[i];
                rt->m_BufferTextureParams[i].m_Data     = 0x0;
                rt->m_BufferTextureParams[i].m_DataSize = 0;

                if(i == dmGraphics::GetBufferTypeIndex(dmGraphics::BUFFER_TYPE_COLOR0_BIT)  ||
                   i == dmGraphics::GetBufferTypeIndex(dmGraphics::BUFFER_TYPE_COLOR1_BIT) ||
                   i == dmGraphics::GetBufferTypeIndex(dmGraphics::BUFFER_TYPE_COLOR2_BIT) ||
                   i == dmGraphics::GetBufferTypeIndex(dmGraphics::BUFFER_TYPE_COLOR3_BIT))
                {
                    rt->m_BufferTextureParams[i].m_DataSize = buffer_size;
                    rt->m_ColorBufferTexture[i]             = NewTexture(context, creation_params[i]);
                    Texture* attachment_tex                 = GetAssetFromContainer<Texture>(context->m_AssetHandleContainer, rt->m_ColorBufferTexture[i]);

                    SetTexture(rt->m_ColorBufferTexture[i], rt->m_BufferTextureParams[i]);
                    *(buffers[i]) = attachment_tex->m_Data;
                } else {
                    *(buffers[i]) = new char[buffer_size];
                }
            }
        }

        return StoreAssetInContainer(context->m_AssetHandleContainer, rt, ASSET_TYPE_RENDER_TARGET);
    }

    static void WebGPUDeleteRenderTarget(HRenderTarget render_target)
    {
        RenderTarget* rt = GetAssetFromContainer<RenderTarget>(g_WebGPUContext->m_AssetHandleContainer, render_target);

        for (int i = 0; i < MAX_BUFFER_COLOR_ATTACHMENTS; ++i)
        {
            if (rt->m_ColorBufferTexture[i])
            {
                DeleteTexture(rt->m_ColorBufferTexture[i]);
            }
        }
        delete [] (char*)rt->m_FrameBuffer.m_DepthBuffer;
        delete [] (char*)rt->m_FrameBuffer.m_StencilBuffer;
        delete rt;

        g_WebGPUContext->m_AssetHandleContainer.Release(render_target);
    }

    static void WebGPUSetRenderTarget(HContext _context, HRenderTarget render_target, uint32_t transient_buffer_types)
    {
        (void) transient_buffer_types;
        assert(_context);
        WebGPUContext* context = (WebGPUContext*) _context;

        if (render_target == 0)
        {
            context->m_CurrentFrameBuffer = &context->m_MainFrameBuffer;
        }
        else
        {
            assert(GetAssetType(render_target) == dmGraphics::ASSET_TYPE_RENDER_TARGET);
            RenderTarget* rt = GetAssetFromContainer<RenderTarget>(context->m_AssetHandleContainer, render_target);
            context->m_CurrentFrameBuffer = &rt->m_FrameBuffer;
        }
    }

    static HTexture WebGPUGetRenderTargetTexture(HRenderTarget render_target, BufferType buffer_type)
    {
        if(!(buffer_type == BUFFER_TYPE_COLOR0_BIT ||
             buffer_type == BUFFER_TYPE_COLOR1_BIT ||
             buffer_type == BUFFER_TYPE_COLOR2_BIT ||
             buffer_type == BUFFER_TYPE_COLOR3_BIT))
        {
            return 0;
        }

        RenderTarget* rt = GetAssetFromContainer<RenderTarget>(g_WebGPUContext->m_AssetHandleContainer, render_target);
        return rt->m_ColorBufferTexture[GetBufferTypeIndex(buffer_type)];
    }

    static void WebGPUGetRenderTargetSize(HRenderTarget render_target, BufferType buffer_type, uint32_t& width, uint32_t& height)
    {
        assert(render_target);
        uint32_t i = GetBufferTypeIndex(buffer_type);
        assert(i < MAX_BUFFER_TYPE_COUNT);
        RenderTarget* rt = GetAssetFromContainer<RenderTarget>(g_WebGPUContext->m_AssetHandleContainer, render_target);
        width = rt->m_BufferTextureParams[i].m_Width;
        height = rt->m_BufferTextureParams[i].m_Height;
    }

    static void WebGPUSetRenderTargetSize(HRenderTarget render_target, uint32_t width, uint32_t height)
    {
        RenderTarget* rt = GetAssetFromContainer<RenderTarget>(g_WebGPUContext->m_AssetHandleContainer, render_target);

        uint32_t buffer_size = sizeof(uint32_t) * width * height;
        void** buffers[MAX_BUFFER_TYPE_COUNT] = {
            &rt->m_FrameBuffer.m_ColorBuffer[0],
            &rt->m_FrameBuffer.m_ColorBuffer[1],
            &rt->m_FrameBuffer.m_ColorBuffer[2],
            &rt->m_FrameBuffer.m_ColorBuffer[3],
            &rt->m_FrameBuffer.m_DepthBuffer,
            &rt->m_FrameBuffer.m_StencilBuffer,
        };
        uint32_t* buffer_sizes[MAX_BUFFER_TYPE_COUNT] = {
            &rt->m_FrameBuffer.m_ColorBufferSize[0],
            &rt->m_FrameBuffer.m_ColorBufferSize[1],
            &rt->m_FrameBuffer.m_ColorBufferSize[2],
            &rt->m_FrameBuffer.m_ColorBufferSize[3],
            &rt->m_FrameBuffer.m_DepthBufferSize,
            &rt->m_FrameBuffer.m_StencilBufferSize,
        };

        for (uint32_t i = 0; i < MAX_BUFFER_TYPE_COUNT; ++i)
        {
            if (buffers[i])
            {
                *(buffer_sizes[i]) = buffer_size;
                rt->m_BufferTextureParams[i].m_Width = width;
                rt->m_BufferTextureParams[i].m_Height = height;
                if(i == dmGraphics::GetBufferTypeIndex(dmGraphics::BUFFER_TYPE_COLOR0_BIT) ||
                   i == dmGraphics::GetBufferTypeIndex(dmGraphics::BUFFER_TYPE_COLOR1_BIT) ||
                   i == dmGraphics::GetBufferTypeIndex(dmGraphics::BUFFER_TYPE_COLOR2_BIT) ||
                   i == dmGraphics::GetBufferTypeIndex(dmGraphics::BUFFER_TYPE_COLOR3_BIT))
                {
                    if (rt->m_ColorBufferTexture[i])
                    {
                        rt->m_BufferTextureParams[i].m_DataSize = buffer_size;
                        SetTexture(rt->m_ColorBufferTexture[i], rt->m_BufferTextureParams[i]);
                        Texture* tex = GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, rt->m_ColorBufferTexture[i]);
                        *(buffers[i]) = tex->m_Data;
                    }
                } else {
                    delete [] (char*)*(buffers[i]);
                    *(buffers[i]) = new char[buffer_size];
                }
            }
        }
    }

    static bool WebGPUIsTextureFormatSupported(HContext context, TextureFormat format)
    {
        return (((WebGPUContext*) context)->m_TextureFormatSupport & (1 << format)) != 0;
    }

    static uint32_t WebGPUGetMaxTextureSize(HContext context)
    {
        return 1024;
    }

    static HTexture WebGPUNewTexture(HContext _context, const TextureCreationParams& params)
    {
        WebGPUContext* context  = (WebGPUContext*) _context;
        Texture* tex          = new Texture();

        tex->m_Type        = params.m_Type;
        tex->m_Width       = params.m_Width;
        tex->m_Height      = params.m_Height;
        tex->m_Depth       = params.m_Depth;
        tex->m_MipMapCount = 0;
        tex->m_Data        = 0;

        if (params.m_OriginalWidth == 0)
        {
            tex->m_OriginalWidth  = params.m_Width;
            tex->m_OriginalHeight = params.m_Height;
        }
        else
        {
            tex->m_OriginalWidth  = params.m_OriginalWidth;
            tex->m_OriginalHeight = params.m_OriginalHeight;
        }

        return StoreAssetInContainer(context->m_AssetHandleContainer, tex, ASSET_TYPE_TEXTURE);
    }

    static void WebGPUDeleteTexture(HTexture texture)
    {
        Texture* tex = GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture);
        assert(tex);
        if (tex->m_Data != 0x0)
        {
            delete [] (char*)tex->m_Data;
        }
        delete tex;

        g_WebGPUContext->m_AssetHandleContainer.Release(texture);
    }

    static HandleResult WebGPUGetTextureHandle(HTexture texture, void** out_handle)
    {
        *out_handle = 0x0;
        Texture* tex = GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture);

        if (!tex)
        {
            return HANDLE_RESULT_ERROR;
        }

        *out_handle = tex->m_Data;

        return HANDLE_RESULT_OK;
    }

    static void WebGPUSetTextureParams(HTexture texture, TextureFilter minfilter, TextureFilter magfilter, TextureWrap uwrap, TextureWrap vwrap, float max_anisotropy)
    {
        assert(texture);
    }

    static void WebGPUSetTexture(HTexture texture, const TextureParams& params)
    {
        Texture* tex = GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture);
        assert(tex);
        assert(!params.m_SubUpdate || (params.m_X + params.m_Width <= tex->m_Width));
        assert(!params.m_SubUpdate || (params.m_Y + params.m_Height <= tex->m_Height));

        if (tex->m_Data != 0x0)
        {
            delete [] (char*)tex->m_Data;
        }

        tex->m_Format = params.m_Format;
        // Allocate even for 0x0 size so that the rendertarget dummies will work.
        tex->m_Data = new char[params.m_DataSize];
        if (params.m_Data != 0x0)
        {
            memcpy(tex->m_Data, params.m_Data, params.m_DataSize);
        }

        // The width/height of the texture can change from this function as well
        if (!params.m_SubUpdate && params.m_MipMap == 0)
        {
            tex->m_Width  = params.m_Width;
            tex->m_Height = params.m_Height;
        }

        tex->m_Depth       = dmMath::Max((uint16_t) 1, params.m_Depth);
        tex->m_MipMapCount = dmMath::Max(tex->m_MipMapCount, (uint8_t) (params.m_MipMap+1));
        tex->m_MipMapCount = dmMath::Clamp(tex->m_MipMapCount, (uint8_t) 0, GetMipmapCount(dmMath::Max(tex->m_Width, tex->m_Height)));
    }

    static uint32_t WebGPUGetTextureResourceSize(HTexture texture)
    {
        Texture* tex = GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture);

        uint32_t size_total = 0;
        uint32_t size = tex->m_Width * tex->m_Height * dmMath::Max(1U, GetTextureFormatBitsPerPixel(tex->m_Format)/8);
        for(uint32_t i = 0; i < tex->m_MipMapCount; ++i)
        {
            size_total += size;
            size >>= 2;
        }
        if (tex->m_Type == TEXTURE_TYPE_CUBE_MAP)
        {
            size_total *= 6;
        }
        return size_total + sizeof(Texture);
    }

    static uint16_t WebGPUGetTextureWidth(HTexture texture)
    {
        return GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture)->m_Width;
    }

    static uint16_t WebGPUGetTextureHeight(HTexture texture)
    {
        return GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture)->m_Height;
    }

    static uint16_t WebGPUGetOriginalTextureWidth(HTexture texture)
    {
        return GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture)->m_OriginalWidth;
    }

    static uint16_t WebGPUGetOriginalTextureHeight(HTexture texture)
    {
        return GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture)->m_OriginalHeight;
    }

    static void WebGPUEnableTexture(HContext _context, uint32_t unit, uint8_t value_index, HTexture texture)
    {
        assert(_context);
        assert(unit < MAX_TEXTURE_COUNT);
        assert(texture);
        WebGPUContext* context = (WebGPUContext*) _context;
        assert(GetAssetFromContainer<Texture>(context->m_AssetHandleContainer, texture)->m_Data);
        context->m_Textures[unit] = texture;
    }

    static void WebGPUDisableTexture(HContext context, uint32_t unit, HTexture texture)
    {
        assert(context);
        assert(unit < MAX_TEXTURE_COUNT);
        ((WebGPUContext*) context)->m_Textures[unit] = 0;
    }

    static void WebGPUReadPixels(HContext context, void* buffer, uint32_t buffer_size)
    {
    }

    static void WebGPUEnableState(HContext context, State state)
    {
        assert(context);
        SetPipelineStateValue(((WebGPUContext*) context)->m_PipelineState, state, 1);
    }

    static void WebGPUDisableState(HContext context, State state)
    {
        assert(context);
        SetPipelineStateValue(((WebGPUContext*) context)->m_PipelineState, state, 0);
    }

    static void WebGPUSetBlendFunc(HContext _context, BlendFactor source_factor, BlendFactor destinaton_factor)
    {
        assert(_context);
        WebGPUContext* context = (WebGPUContext*) _context;
        context->m_PipelineState.m_BlendSrcFactor = source_factor;
        context->m_PipelineState.m_BlendDstFactor = destinaton_factor;
    }

    static void WebGPUSetColorMask(HContext context, bool red, bool green, bool blue, bool alpha)
    {
        // Replace above
        uint8_t write_mask = red   ? DM_GRAPHICS_STATE_WRITE_R : 0;
        write_mask        |= green ? DM_GRAPHICS_STATE_WRITE_G : 0;
        write_mask        |= blue  ? DM_GRAPHICS_STATE_WRITE_B : 0;
        write_mask        |= alpha ? DM_GRAPHICS_STATE_WRITE_A : 0;
        ((WebGPUContext*) context)->m_PipelineState.m_WriteColorMask = write_mask;
    }

    static void WebGPUSetDepthMask(HContext context, bool mask)
    {
        assert(context);
        ((WebGPUContext*) context)->m_PipelineState.m_WriteDepth = mask;
    }

    static void WebGPUSetDepthFunc(HContext context, CompareFunc func)
    {
        assert(context);
        ((WebGPUContext*) context)->m_PipelineState.m_DepthTestFunc = func;
    }

    static void WebGPUSetScissor(HContext _context, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        assert(_context);
        WebGPUContext* context = (WebGPUContext*) _context;
        context->m_ScissorRect[0] = (int32_t) x;
        context->m_ScissorRect[1] = (int32_t) y;
        context->m_ScissorRect[2] = (int32_t) x+width;
        context->m_ScissorRect[3] = (int32_t) y+height;
    }

    static void WebGPUSetStencilMask(HContext context, uint32_t mask)
    {
        assert(context);
        ((WebGPUContext*) context)->m_PipelineState.m_StencilWriteMask = mask;
    }

    static void WebGPUSetStencilFunc(HContext _context, CompareFunc func, uint32_t ref, uint32_t mask)
    {
        assert(_context);
        WebGPUContext* context = (WebGPUContext*) _context;
        context->m_PipelineState.m_StencilFrontTestFunc = (uint8_t) func;
        context->m_PipelineState.m_StencilBackTestFunc  = (uint8_t) func;
        context->m_PipelineState.m_StencilReference     = (uint8_t) ref;
        context->m_PipelineState.m_StencilCompareMask   = (uint8_t) mask;
    }

    static void WebGPUSetStencilOp(HContext _context, StencilOp sfail, StencilOp dpfail, StencilOp dppass)
    {
        assert(_context);
        WebGPUContext* context = (WebGPUContext*) _context;
        context->m_PipelineState.m_StencilFrontOpFail      = sfail;
        context->m_PipelineState.m_StencilFrontOpDepthFail = dpfail;
        context->m_PipelineState.m_StencilFrontOpPass      = dppass;
        context->m_PipelineState.m_StencilBackOpFail       = sfail;
        context->m_PipelineState.m_StencilBackOpDepthFail  = dpfail;
        context->m_PipelineState.m_StencilBackOpPass       = dppass;
    }

    static void WebGPUSetStencilFuncSeparate(HContext _context, FaceType face_type, CompareFunc func, uint32_t ref, uint32_t mask)
    {
        WebGPUContext* context = (WebGPUContext*) _context;

        if (face_type == FACE_TYPE_BACK)
        {
            context->m_PipelineState.m_StencilBackTestFunc = (uint8_t) func;
        }
        else
        {
            context->m_PipelineState.m_StencilFrontTestFunc = (uint8_t) func;
        }
        context->m_PipelineState.m_StencilReference   = (uint8_t) ref;
        context->m_PipelineState.m_StencilCompareMask = (uint8_t) mask;
    }

    static void WebGPUSetStencilOpSeparate(HContext _context, FaceType face_type, StencilOp sfail, StencilOp dpfail, StencilOp dppass)
    {
        WebGPUContext* context = (WebGPUContext*) _context;

        if (face_type == FACE_TYPE_BACK)
        {
            context->m_PipelineState.m_StencilBackOpFail       = sfail;
            context->m_PipelineState.m_StencilBackOpDepthFail  = dpfail;
            context->m_PipelineState.m_StencilBackOpPass       = dppass;
        }
        else
        {
            context->m_PipelineState.m_StencilFrontOpFail      = sfail;
            context->m_PipelineState.m_StencilFrontOpDepthFail = dpfail;
            context->m_PipelineState.m_StencilFrontOpPass      = dppass;
        }
    }

    static void WebGPUSetFaceWinding(HContext context, FaceWinding face_winding)
    {
        ((WebGPUContext*) context)->m_PipelineState.m_FaceWinding = face_winding;
    }

    static void WebGPUSetCullFace(HContext context, FaceType face_type)
    {
        assert(context);
    }

    static void WebGPUSetPolygonOffset(HContext context, float factor, float units)
    {
        assert(context);
    }

    static PipelineState WebGPUGetPipelineState(HContext context)
    {
        return ((WebGPUContext*) context)->m_PipelineState;
    }

    static void WebGPUSetTextureAsync(HTexture texture, const TextureParams& params)
    {
        SetTexture(texture, params);
    }

    static uint32_t WebGPUGetTextureStatusFlags(HTexture texture)
    {
        return TEXTURE_STATUS_OK;
    }

    // Tests only
    void SetForceFragmentReloadFail(bool should_fail)
    {
        g_ForceFragmentReloadFail = should_fail;
    }

    // Tests only
    void SetForceVertexReloadFail(bool should_fail)
    {
        g_ForceVertexReloadFail = should_fail;
    }

    static bool WebGPUIsExtensionSupported(HContext context, const char* extension)
    {
        return true;
    }

    static TextureType WebGPUGetTextureType(HTexture texture)
    {
        return GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture)->m_Type;
    }

    static uint32_t WebGPUGetNumSupportedExtensions(HContext context)
    {
        return 0;
    }

    static const char* WebGPUGetSupportedExtension(HContext context, uint32_t index)
    {
        return "";
    }

    static uint8_t WebGPUGetNumTextureHandles(HTexture texture)
    {
        return 1;
    }

    static bool WebGPUIsContextFeatureSupported(HContext context, ContextFeature feature)
    {
        return true;
    }

    static uint16_t WebGPUGetTextureDepth(HTexture texture)
    {
        return GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture)->m_Depth;
    }

    static uint8_t WebGPUGetTextureMipmapCount(HTexture texture)
    {
        return GetAssetFromContainer<Texture>(g_WebGPUContext->m_AssetHandleContainer, texture)->m_MipMapCount;
    }

    static bool WebGPUIsAssetHandleValid(HContext _context, HAssetHandle asset_handle)
    {
        assert(_context);
        if (asset_handle == 0)
        {
            return false;
        }
        WebGPUContext* context = (WebGPUContext*) _context;
        AssetType type       = GetAssetType(asset_handle);
        if (type == ASSET_TYPE_TEXTURE)
        {
            return GetAssetFromContainer<Texture>(context->m_AssetHandleContainer, asset_handle) != 0;
        }
        else if (type == ASSET_TYPE_RENDER_TARGET)
        {
            return GetAssetFromContainer<RenderTarget>(context->m_AssetHandleContainer, asset_handle) != 0;
        }
        return false;
    }

    static GraphicsAdapterFunctionTable WebGPURegisterFunctionTable()
    {
        GraphicsAdapterFunctionTable fn_table = {};
        DM_REGISTER_GRAPHICS_FUNCTION_TABLE(fn_table, WebGPU);
        return fn_table;
    }
}

