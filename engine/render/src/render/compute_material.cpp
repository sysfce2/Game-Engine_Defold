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

#include <algorithm>
#include <string.h>

#include <dlib/array.h>
#include <dlib/dstrings.h>
#include <dlib/hashtable.h>
#include <dlib/log.h>
#include <dmsdk/dlib/vmath.h>
#include "render.h"
#include "render_private.h"

namespace dmRender
{
    using namespace dmVMath;

    HMaterial NewComputeMaterial(dmRender::HRenderContext render_context, dmGraphics::HComputeProgram compute_program)
    {
        ComputeMaterial* m  = new ComputeMaterial;
        m->m_ComputeProgram = compute_program;
        m->m_Program        = dmGraphics::NewProgram(dmRender::GetGraphicsContext(render_context), compute_program);

    	return new Material(m);
    }

    dmGraphics::HProgram GetComputeMaterialProgram(HMaterial material)
    {
        return material->m_ComputeMaterial->m_Program;
    }
}