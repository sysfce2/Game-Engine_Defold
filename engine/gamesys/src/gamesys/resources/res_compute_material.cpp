#include "res_compute_material.h"

#include <render/render.h>
#include <render/material_ddf.h>

namespace dmGameSystem
{
	dmResource::Result AcquireResources(dmResource::HFactory factory, dmRenderDDF::ComputeMaterialDesc* ddf, void** program)
    {
        dmResource::Result factory_e;
        factory_e = dmResource::Get(factory, ddf->m_Program, program);
        if ( factory_e != dmResource::RESULT_OK)
        {
            return factory_e;
        }

        return dmResource::RESULT_OK;
    }

    dmResource::Result ResComputeMaterialCreate(const dmResource::ResourceCreateParams& params)
    {
    	dmRender::HRenderContext render_context = (dmRender::HRenderContext) params.m_Context;
        dmRenderDDF::ComputeMaterialDesc* ddf   = (dmRenderDDF::ComputeMaterialDesc*)params.m_PreloadData;

        //MaterialResources resources;
        void* pgm;

        dmResource::Result r = AcquireResources(params.m_Factory, ddf, &pgm);
        if (r == dmResource::RESULT_OK)
        {
        	dmRender::HMaterial material = dmRender::NewComputeMaterial(render_context, (dmGraphics::HComputeProgram) pgm);
            //dmRender::HMaterial material = dmRender::NewMaterial(render_context, resources.m_VertexProgram, resources.m_FragmentProgram);

            dmResource::SResourceDescriptor desc;
            dmResource::Result factory_e;

            /*
            factory_e = dmResource::GetDescriptor(params.m_Factory, ddf->m_VertexProgram, &desc);
            assert(factory_e == dmResource::RESULT_OK); // Should not fail at this point
            dmRender::SetMaterialUserData1(material, desc.m_NameHash);

            factory_e = dmResource::GetDescriptor(params.m_Factory, ddf->m_FragmentProgram, &desc);
            assert(factory_e == dmResource::RESULT_OK); // Should not fail at this point
            dmRender::SetMaterialUserData2(material, desc.m_NameHash);

            dmResource::RegisterResourceReloadedCallback(params.m_Factory, ResourceReloadedCallback, material);

            SetMaterial(params.m_Filename, material, ddf, &resources);
			*/

            params.m_Resource->m_Resource = (void*) material;
        }
        dmDDF::FreeMessage(ddf);
        return r;
    }

    dmResource::Result ResComputeMaterialDestroy(const dmResource::ResourceDestroyParams& params)
    {
    	return dmResource::RESULT_OK;
    }

    dmResource::Result ResComputeMaterialRecreate(const dmResource::ResourceRecreateParams& params)
    {
    	return dmResource::RESULT_OK;
    }

    dmResource::Result ResComputeMaterialPreload(const dmResource::ResourcePreloadParams& params)
    {
    	dmRenderDDF::ComputeMaterialDesc* ddf;
        dmDDF::Result e = dmDDF::LoadMessage<dmRenderDDF::ComputeMaterialDesc>(params.m_Buffer, params.m_BufferSize, &ddf);
        if (e != dmDDF::RESULT_OK)
        {
            return dmResource::RESULT_DDF_ERROR;
        }

        /*
        if (!ValidateFormat(ddf))
        {
            dmDDF::FreeMessage(ddf);
            return dmResource::RESULT_FORMAT_ERROR;
        }
        */

        dmResource::PreloadHint(params.m_HintInfo, ddf->m_Program);
        *params.m_PreloadData = ddf;
        return dmResource::RESULT_OK;
    }
}
