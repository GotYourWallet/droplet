#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdint>
#include "math_utils.h"
#include "geometry.h"

using Microsoft::WRL::ComPtr;

struct alignas(16) FrameCBData {
    math::Mat4 viewProj;
    math::Mat4 invViewProj;
    math::Mat4 view;
    math::Mat4 proj;
    math::Mat4 lightViewProj;

    math::Vec4 cameraPosTime;
    math::Vec4 sunDirNear;
    math::Vec4 sunColorFar;
    math::Vec4 resolutionInv;
    math::Vec4 dofParams;
    math::Vec4 dripOriginPool;
    math::Vec4 heroDropCaustic;
    math::Vec4 featureFlags;
};

struct alignas(16) DropletCBData {
    math::Vec4 dropPosRadius;
    math::Vec4 dropDeform;
    math::Vec4 dropAttach;
};

struct alignas(16) WaterSimCBData {
    math::Vec4 splashParams;
    math::Vec4 simParams;
    math::Vec4 sunDirSim;
};

struct alignas(16) PostCBData {
    math::Vec4 texelSize;
};

struct DropletInstance {
    math::Vec3 pos;
    float radius = 0.0f;
    float elongation = 0.0f;
    float neckPinch = 0.0f;
    float oscillation = 0.0f;
    float mode = 0.0f; // 0=pendant, 1=falling, 2=worthington jet
    math::Vec3 attachPos;
    float phase = 0.0f;
    bool active = false;
};

struct DemoSettings {
    bool wireframe = false;
    bool dofEnabled = true;
    bool volumetricEnabled = true;
    bool bloomEnabled = true;
    bool showFps = true;
    bool paused = false;
    float tessScale = 1.0f;
    int cameraPreset = 0;
    float displayFps = 0.0f;
    float displayFrameMs = 0.0f;
};

struct GpuMesh {
    ComPtr<ID3D11Buffer> vb;
    ComPtr<ID3D11Buffer> ib;
    uint32_t indexCount = 0;
};

class Renderer {
public:
    bool init(HWND hwnd, uint32_t width, uint32_t height, const std::wstring& shaderDir);
    void resize(uint32_t width, uint32_t height);
    void updateSimulation(float dt, float totalTime, const DemoSettings& settings, bool forceDrop);
    void renderFrame(
        const math::Vec3& camPos,
        const math::Vec3& camTarget,
        float totalTime,
        const DemoSettings& settings);
    void present(bool vsync);
    bool saveBackbufferBMP(const std::string& filename);

    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    std::string adapterName() const { return m_adapterName; }
    float cyclePhase() const { return m_cycleTime / m_cycleDuration; }
    const math::Vec3& heroDropletPos() const { return m_heroDropCenter; }

private:
    bool createDeviceAndSwapchain(HWND hwnd);
    bool createRenderTargets();
    bool createStates();
    bool createShaders(const std::wstring& shaderDir);
    bool createGeometry();
    bool createWaterSimResources();

    GpuMesh uploadMesh(const geo::MeshData& data);
    void stepWaterSimulation(float dt, float totalTime, bool triggerSplash, float splashStrength);
    void drawDroplet(const DropletInstance& d, bool wireframe);

    HWND m_hwnd = nullptr;
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    std::string m_adapterName;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IDXGISwapChain> m_swapchain;
    ComPtr<ID3D11RenderTargetView> m_backbufferRTV;

    // Main HDR scene + depth + copies
    ComPtr<ID3D11Texture2D> m_hdrTex, m_sceneCopyTex, m_dropletBackTex, m_dofTex, m_ldrTex;
    ComPtr<ID3D11RenderTargetView> m_hdrRTV, m_dropletBackRTV, m_dofRTV, m_ldrRTV;
    ComPtr<ID3D11ShaderResourceView> m_hdrSRV, m_sceneCopySRV, m_dropletBackSRV, m_dofSRV, m_ldrSRV;

    ComPtr<ID3D11Texture2D> m_depthTex;
    ComPtr<ID3D11DepthStencilView> m_depthDSV, m_depthReadOnlyDSV;
    ComPtr<ID3D11ShaderResourceView> m_depthSRV;

    // Half-res volumetric god rays
    ComPtr<ID3D11Texture2D> m_volTex;
    ComPtr<ID3D11RenderTargetView> m_volRTV;
    ComPtr<ID3D11ShaderResourceView> m_volSRV;
    uint32_t m_volW = 0, m_volH = 0;

    // 5-mip Bloom chain
    static constexpr int BLOOM_MIPS = 5;
    ComPtr<ID3D11Texture2D> m_bloomTex[BLOOM_MIPS];
    ComPtr<ID3D11RenderTargetView> m_bloomRTV[BLOOM_MIPS];
    ComPtr<ID3D11ShaderResourceView> m_bloomSRV[BLOOM_MIPS];
    uint32_t m_bloomW[BLOOM_MIPS] = {};
    uint32_t m_bloomH[BLOOM_MIPS] = {};

    // 2048x2048 Shadow Map
    static constexpr uint32_t SHADOW_SIZE = 2048;
    ComPtr<ID3D11Texture2D> m_shadowTex;
    ComPtr<ID3D11DepthStencilView> m_shadowDSV;
    ComPtr<ID3D11ShaderResourceView> m_shadowSRV;

    // 256x256 Compute Water Simulation & Caustics
    static constexpr uint32_t WATER_SIM_SIZE = 256;
    ComPtr<ID3D11Texture2D> m_waterStateTex[2];
    ComPtr<ID3D11ShaderResourceView> m_waterStateSRV[2];
    ComPtr<ID3D11UnorderedAccessView> m_waterStateUAV[2];
    int m_waterPingPong = 0;

    ComPtr<ID3D11Texture2D> m_waterNormalTex, m_causticTex;
    ComPtr<ID3D11ShaderResourceView> m_waterNormalSRV, m_causticSRV;
    ComPtr<ID3D11UnorderedAccessView> m_waterNormalUAV, m_causticUAV;

    // Shaders & Input Layout
    ComPtr<ID3D11InputLayout> m_vertexLayout;
    ComPtr<ID3D11VertexShader> m_vsBranch, m_vsBranchShadow, m_vsLeaf, m_vsLeafShadow;
    ComPtr<ID3D11HullShader> m_hsBranch, m_hsDroplet;
    ComPtr<ID3D11DomainShader> m_dsBranch, m_dsDroplet;
    ComPtr<ID3D11PixelShader> m_psBranch, m_psLeaf;
    ComPtr<ID3D11VertexShader> m_vsDroplet;
    ComPtr<ID3D11PixelShader> m_psDropletBack, m_psDropletFront;
    ComPtr<ID3D11VertexShader> m_vsPoolBed, m_vsPoolWater;
    ComPtr<ID3D11PixelShader> m_psPoolBed, m_psPoolWater;
    ComPtr<ID3D11VertexShader> m_vsFullscreen, m_vsSkyFullscreen, m_vsParticles;
    ComPtr<ID3D11PixelShader> m_psSky, m_psVolumetric, m_psParticles;
    ComPtr<ID3D11PixelShader> m_psDof, m_psBloomPrefilter, m_psBloomDown, m_psBloomUp, m_psComposite, m_psFxaa;
    ComPtr<ID3D11ComputeShader> m_csWaveStep, m_csCaustics;

    // Constant buffers
    ComPtr<ID3D11Buffer> m_frameCB, m_dropletCB, m_waterSimCB, m_postCB;

    // States
    ComPtr<ID3D11RasterizerState> m_rsCullBack, m_rsCullFront, m_rsCullNone, m_rsWireframe, m_rsShadow;
    ComPtr<ID3D11DepthStencilState> m_dssDefault, m_dssReadOnly, m_dssDisabled, m_dssSky;
    ComPtr<ID3D11BlendState> m_bsOpaque, m_bsAlpha, m_bsAdditive;
    ComPtr<ID3D11SamplerState> m_sampLinearClamp, m_sampShadowCmp;

    // Geometry
    GpuMesh m_branchMesh, m_bgForestMesh, m_leavesMesh, m_dropletMesh, m_poolBedMesh, m_poolWaterMesh;
    ComPtr<ID3D11Buffer> m_particleBuf;
    ComPtr<ID3D11ShaderResourceView> m_particleSRV;
    uint32_t m_particleCount = 0;

    // Droplet physics state
    float m_cycleTime = 1.6f; // Start mid-pooling so the first frame already shows a plump droplet!
    float m_cycleDuration = 6.2f;
    bool m_hasImpactedThisCycle = false;
    bool m_pendingSplash = false;
    float m_pendingSplashStrength = 0.0f;
    float m_simAccum = 0.0f;

    DropletInstance m_pendantDrop;
    DropletInstance m_fallingDrop;
    DropletInstance m_splashJet;
    math::Vec3 m_heroDropCenter = {geo::DRIP_TIP_X, geo::DRIP_TIP_Y - 0.08f, geo::DRIP_TIP_Z};
    float m_heroDropRadius = 0.08f;
};
