/***************************************************************************
# Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
***************************************************************************/
#include "BSDFViewer.h"

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("BSDFViewer", BSDFViewer::sDesc, BSDFViewer::create);
}

namespace
{
    const char kFileViewerPass[] = "RenderPasses/BSDFViewer/BSDFViewer.cs.slang";
    const char kOutput[] = "output";
}

const char* BSDFViewer::sDesc = "BSDF Viewer";

BSDFViewer::SharedPtr BSDFViewer::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new BSDFViewer);
    return pPass->init(dict) ? pPass : nullptr;
}

bool BSDFViewer::init(const Dictionary& dict)
{
    // Defines to disable discard and gradient operations in Falcor's material system.
    Program::DefineList defines =
    {
        {"_MS_DISABLE_ALPHA_TEST", ""},
        {"_DEFAULT_ALPHA_TEST", ""},
    };

    // Create programs.
    Program::Desc desc;
    desc.addShaderLibrary(kFileViewerPass).csEntry("main").setShaderModel("6_0");
    mpViewerPass = ComputePass::create(desc, defines, false);
    if (!mpViewerPass) return false;

    // Create a high-quality pseudorandom number generator.
    mpSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);
    if (!mpSampleGenerator) return false;
    mpSampleGenerator->prepareProgram(mpViewerPass->getProgram().get());
    mpViewerPass->setVars(nullptr); // Trigger vars creation

    // Create readback buffer.
    mPixelDataBuffer = StructuredBuffer::create(mpViewerPass->getProgram().get(), "gPixelData", 1u, ResourceBindFlags::UnorderedAccess);
    if (!mPixelDataBuffer) return false;

    return true;
}

Dictionary BSDFViewer::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection BSDFViewer::reflect(const CompileData& compileData)
{
    RenderPassReflection r;
    r.addOutput(kOutput, "Output buffer").format(ResourceFormat::RGBA32Float).bindFlags(ResourceBindFlags::UnorderedAccess);
    return r;
}

void BSDFViewer::compile(RenderContext* pContext, const CompileData& compileData)
{
    mParams.frameDim = compileData.defaultTexDims;

    // Place a square viewport centered in the frame.
    uint32_t extent = std::min(mParams.frameDim.x, mParams.frameDim.y);
    uint32_t xOffset = (mParams.frameDim.x - extent) / 2;
    uint32_t yOffset = (mParams.frameDim.y - extent) / 2;

    mParams.viewportOffset = float2(xOffset, yOffset);
    mParams.viewportScale = float2(1.f / extent);
}

void BSDFViewer::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;
    mpEnvProbe = nullptr;
    mEnvProbeFilename = "";
    mMaterialList.clear();
    mParams.materialID = 0;

    if (pScene == nullptr)
    {
        mParams.useSceneMaterial = false;
        mParams.useEnvMap = false;
    }
    else
    {
        // Bind the scene to our program.
        Shader::DefineList defines = mpScene->getSceneDefines();
        mpViewerPass->getProgram()->addDefines(defines);
        mpViewerPass->setVars(nullptr); // Trigger vars creation
        mpViewerPass["gScene"] = mpScene->getParameterBlock();

        // Load and bind environment map.
        // We're getting the file name from the scene's LightProbe because that was used in the fscene files.
        // TODO: Switch to use Scene::getEnvironmentMap() when the assets have been updated.
        auto pLightProbe = mpScene->getLightProbe();
        if (pLightProbe != nullptr)
        {
            std::string fn = pLightProbe->getOrigTexture()->getSourceFilename();
            loadEnvMap(pRenderContext, fn);
        }
        if (!mpEnvProbe) mParams.useEnvMap = false;

        // Prepare UI list of materials.
        mMaterialList.reserve(mpScene->getMaterialCount());
        for (uint32_t i = 0; i < mpScene->getMaterialCount(); i++)
        {
            auto mtl = mpScene->getMaterial(i);
            std::string name = std::to_string(i) + ": " + mtl->getName();
            mMaterialList.push_back({ i, name });
        }
        assert(mMaterialList.size() > 0);
    }
}

void BSDFViewer::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Update refresh flag if options that affect the output have changed.
    if (mOptionsChanged)
    {
        Dictionary& dict = renderData.getDictionary();
        auto prevFlags = (Falcor::RenderPassRefreshFlags)(dict.keyExists(kRenderPassRefreshFlags) ? dict[Falcor::kRenderPassRefreshFlags] : 0u);
        dict[Falcor::kRenderPassRefreshFlags] = (uint32_t)(prevFlags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged);
        mOptionsChanged = false;
    }

    // Setup constants.
    mParams.cameraViewportScale = std::tan(glm::radians(mParams.cameraFovY / 2.f)) * mParams.cameraDistance;

    // Set resources.
    if (!mpSampleGenerator->setIntoProgramVars(mpViewerPass->getVars().get())) throw std::exception("Failed to bind sample generator");
    mpViewerPass["gOutput"] = renderData[kOutput]->asTexture();
    mpViewerPass["gPixelData"] = mPixelDataBuffer;
    mpViewerPass["PerFrameCB"]["gParams"].setBlob(mParams);

    // Execute pass.
    mpViewerPass->execute(pRenderContext, uvec3(mParams.frameDim, 1));

    mPixelDataValid = false;
    if (mParams.readback)
    {
        const PixelData* pData = static_cast<const PixelData*>(mPixelDataBuffer->map(Buffer::MapType::Read));
        mPixelData = *pData;
        mPixelDataBuffer->unmap();
        mPixelDataValid = true;

        // Copy values from selected pixel.
        mParams.texCoords = mPixelData.texC;
    }

    mParams.frameCount++;
}

void BSDFViewer::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    dirty |= widget.checkbox("Enable BSDF slice viewer", mParams.sliceViewer);
    widget.tooltip("Run BSDF slice viewer.\nOtherise the default mode shows a shaded sphere of the specified material.", true);

    if (mParams.sliceViewer)
    {
        widget.text("The current mode shows a slice of the BSDF.\n"
                    "The x-axis is theta_h (angle between H and normal)\n"
                    "and y-axis is theta_d (angle between H and wi/wo),\n"
                    "both in [0,pi/2] with origin in the lower/left.");
    }
    else
    {
        widget.text("The current mode shows a shaded unit sphere.\n"
                    "The coordinate frame is right-handed with xy\n"
                    "pointing right/up and +z towards the viewer.\n"
                    " ");
    }

    auto mtlGroup = Gui::Group(widget, "Material", true);
    if (mtlGroup.open())
    {
        bool prevMode = mParams.useSceneMaterial;
        mtlGroup.checkbox("Use scene material", mParams.useSceneMaterial);
        mtlGroup.tooltip("Choose material in the dropdown below.\n\n"
            "Left/right arrow keys step to the previous/next material in the list.", true);

        if (!mpScene) mParams.useSceneMaterial = false;
        dirty |= ((bool)mParams.useSceneMaterial != prevMode);

        if (mParams.useSceneMaterial)
        {
            assert(mMaterialList.size() > 0);
            dirty |= mtlGroup.dropdown("Materials", mMaterialList, mParams.materialID);

            dirty |= mtlGroup.checkbox("Normal mapping", mParams.useNormalMapping);
            dirty |= mtlGroup.checkbox("Fixed tex coords", mParams.useFixedTexCoords);
            dirty |= mtlGroup.var("Tex coords", mParams.texCoords, -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0.01f);
        }
        else
        {
            dirty |= mtlGroup.rgbColor("Base color", mParams.baseColor);
            dirty |= mtlGroup.var("Roughness", mParams.linearRoughness, 0.f, 1.f, 1e-2f);
            dirty |= mtlGroup.var("Metallic", mParams.metallic, 0.f, 1.f, 1e-2f);
        }

        mtlGroup.release();
    }

    auto bsdfGroup = Gui::Group(widget, "BSDF", true);
    if (bsdfGroup.open())
    {
        dirty |= bsdfGroup.checkbox("Original Disney BRDF", mParams.originalDisney);
        bsdfGroup.tooltip("When enabled uses the original Disney BRDF, otherwise the modified version by Frostbite.", true);
        dirty |= bsdfGroup.checkbox("Enable diffuse", mParams.enableDiffuse);
        dirty |= bsdfGroup.checkbox("Enable specular", mParams.enableSpecular, true);

        dirty |= bsdfGroup.checkbox("Use BRDF sampling", mParams.useBrdfSampling);
        bsdfGroup.tooltip("When enabled uses BSDF importance sampling, otherwise hemispherical cosine-weighted sampling for verification purposes.", true);
        dirty |= bsdfGroup.checkbox("Use pdf", mParams.usePdf);
        bsdfGroup.tooltip("When enabled evaluates BRDF * NdotL / pdf explicitly for verification purposes.\nOtherwise the weight computed by the importance sampling is used.", true);

        dirty |= bsdfGroup.checkbox("Multiply BSDF slice by NdotL", mParams.applyNdotL);
        bsdfGroup.tooltip("Note: This setting Only affects the BSDF slice viewer. NdotL is always enabled in lighting mode.", true);

        bsdfGroup.release();
    }

    auto lightGroup = Gui::Group(widget, "Light", true);
    if (lightGroup.open())
    {
        dirty |= lightGroup.var("Light intensity", mParams.lightIntensity, 0.f, std::numeric_limits<float>::max(), 0.01f, false, "%.4f");
        dirty |= lightGroup.rgbColor("Light color", mParams.lightColor);
        lightGroup.tooltip("Not used when environment map is enabled.", true);

        dirty |= lightGroup.checkbox("Show ground plane", mParams.useGroundPlane);
        lightGroup.tooltip("When the ground plane is enabled, incident illumination from the lower hemisphere is zero.", true);

        // Directional lighting
        dirty |= lightGroup.checkbox("Directional light", mParams.useDirectionalLight);
        lightGroup.tooltip("When enabled a single directional light source is used, otherwise the light is omnidirectional.", true);

        if (mParams.useDirectionalLight)
        {
            mParams.useEnvMap = false;
            dirty |= lightGroup.var("Light direction", mParams.lightDir, -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0.01f, false, "%.4f");
        }

        // Envmap lighting
        if (mpEnvProbe)
        {
            dirty |= lightGroup.checkbox(("Envmap: " + mEnvProbeFilename).c_str(), mParams.useEnvMap);
            lightGroup.tooltip("When enabled the specified environment map is used as light source. Enabling this option turns off directional lighting.", true);

            if (mParams.useEnvMap)
            {
                mParams.useDirectionalLight = false;
            }
        }
        else
        {
            lightGroup.text("Envmap: N/A");
        }

        if (lightGroup.button("Load envmap"))
        {
            // Get file dialog filters.
            auto filters = Bitmap::getFileDialogFilters();
            filters.push_back({ "hdr", "High Dynamic Range" });
            filters.push_back({ "dds", "DDS textures" });

            std::string fn;
            if (openFileDialog(filters, fn))
            {
                // TODO: RenderContext* should maybe be a parameter to renderUI()?
                auto pRenderContext = gpFramework->getRenderContext();
                if (loadEnvMap(pRenderContext, fn))
                {
                    mParams.useDirectionalLight = false;
                    mParams.useEnvMap = true;
                    dirty = true;
                }
            }
        }

        lightGroup.release();
    }

    auto cameraGroup = Gui::Group(widget, "Camera", true);
    if (cameraGroup.open())
    {
        dirty |= cameraGroup.checkbox("Orthographic camera", mParams.orthographicCamera);

        if (!mParams.orthographicCamera)
        {
            dirty |= cameraGroup.var("Viewing distance", mParams.cameraDistance, 1.01f, std::numeric_limits<float>::max(), 0.01f, false, "%.2f");
            cameraGroup.tooltip("This is the camera's distance to origin in projective mode. The scene has radius 1.0 so the minimum camera distance has to be > 1.0", true);

            dirty |= cameraGroup.var("Vertical FOV (degrees)", mParams.cameraFovY, 1.f, 179.f, 1.f, false, "%.2f");
            cameraGroup.tooltip("The allowed range is [1,179] degrees to avoid numerical issues.", true);
        }

        cameraGroup.release();
    }

    auto pixelGroup = Gui::Group(widget, "Pixel data", true);
    bool readTexCoords = mParams.useSceneMaterial && !mParams.useFixedTexCoords;
    mParams.readback = readTexCoords || pixelGroup.open(); // Configure if readback is necessary

    if (pixelGroup.open())
    {
        pixelGroup.var("Pixel", mParams.selectedPixel);

        if (mPixelDataValid)
        {
            pixelGroup.var("texC", mPixelData.texC, -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0.f, false, "%.4f");
            pixelGroup.var("baseColor", mPixelData.baseColor, 0.f, 1.f, 0.f, false, "%.4f");
            pixelGroup.var("diffuse", mPixelData.diffuse, 0.f, 1.f, 0.f, false, "%.4f");
            pixelGroup.var("specular", mPixelData.specular, 0.f, 1.f, 0.f, false, "%.4f");
            pixelGroup.var("roughness", mPixelData.linearRoughness, 0.f, 1.f, 0.f, false, "%.4f");
            pixelGroup.tooltip("This is the unmapped roughness parameters as specified in the content creation tool.", true);
            pixelGroup.var("metallic", mPixelData.metallic, 0.f, 1.f, 0.f, false, "%.4f");
            pixelGroup.var("T", mPixelData.T, -1.f, 1.f, 0.f, false, "%.4f");
            pixelGroup.var("B", mPixelData.B, -1.f, 1.f, 0.f, false, "%.4f");
            pixelGroup.var("N", mPixelData.N, -1.f, 1.f, 0.f, false, "%.4f");
            pixelGroup.var("wo", mPixelData.wo, -1.f, 1.f, 0.f, false, "%.4f");
            pixelGroup.var("wi", mPixelData.wi, -1.f, 1.f, 0.f, false, "%.4f");
            pixelGroup.var("output", mPixelData.output, 0.f, std::numeric_limits<float>::max(), 0.f, false, "%.4f");
        }
        else
        {
            pixelGroup.text("No data available");
        }

        pixelGroup.release();
    }

    //widget.dummy("#space3", vec2(1, 16));
    //dirty |= widget.checkbox("Debug switch", mParams.debugSwitch0);

    if (dirty)
    {
        mOptionsChanged = true;
    }
}

bool BSDFViewer::onMouseEvent(const MouseEvent& mouseEvent)
{
    if (mouseEvent.type == MouseEvent::Type::LeftButtonDown)
    {
        mParams.selectedPixel = glm::clamp((glm::ivec2)(mouseEvent.pos * (glm::vec2)mParams.frameDim), { 0,0 }, (glm::ivec2)mParams.frameDim - 1);
    }
    return false;
}

bool BSDFViewer::onKeyEvent(const KeyboardEvent& keyEvent)
{
    if (keyEvent.type == KeyboardEvent::Type::KeyPressed)
    {
        if (keyEvent.key == KeyboardEvent::Key::Left || keyEvent.key == KeyboardEvent::Key::Right)
        {
            uint32_t id = mParams.materialID;
            uint32_t lastId = mMaterialList.size() > 0 ? (uint32_t)mMaterialList.size() - 1 : 0;
            if (keyEvent.key == KeyboardEvent::Key::Left) id = id > 0 ? id - 1 : lastId;
            else if (keyEvent.key == KeyboardEvent::Key::Right) id = id < lastId ? id + 1 : 0;

            if (id != mParams.materialID) mOptionsChanged = true; // Triggers reset of accumulation
            mParams.materialID = id;
            return true;
        }
    }
    return false;
}

bool BSDFViewer::loadEnvMap(RenderContext* pRenderContext, const std::string& filename)
{
    auto pEnvProbe = EnvProbe::create(pRenderContext, filename);
    if (!pEnvProbe)
    {
        logWarning("Failed to load environment map from " + filename);
        return false;
    }

    mpEnvProbe = pEnvProbe;
    mEnvProbeFilename = getFilenameFromPath(mpEnvProbe->getEnvMap()->getSourceFilename());

    auto pVars = mpViewerPass->getVars();
    if (!mpEnvProbe->setIntoConstantBuffer(pVars.get(), pVars->getConstantBuffer("PerFrameCB").get(), "gEnvProbe"))
    {
        throw std::exception("Failed to bind EnvProbe to program");
    }

    return true;
}
