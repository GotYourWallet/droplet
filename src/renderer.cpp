#include "renderer.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace math;

namespace {

ComPtr<ID3DBlob> compileShader(const std::wstring& path, const char* entry, const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ComPtr<ID3DBlob> bytecode, errors;
    HRESULT hr = D3DCompileFromFile(
        path.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry,
        target,
        flags,
        0,
        &bytecode,
        &errors);
    if (FAILED(hr)) {
        std::string msg = "Shader compile failed: ";
        if (errors) {
            msg += static_cast<const char*>(errors->GetBufferPointer());
        } else {
            msg += "File not found or HRESULT=" + std::to_string(hr);
        }
        OutputDebugStringA((msg + "\n").c_str());
        fprintf(stderr, "%s\n", msg.c_str());
        return nullptr;
    }
    return bytecode;
}

template <typename T>
ComPtr<ID3D11Buffer> createConstantBuffer(ID3D11Device* dev) {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (sizeof(T) + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> buf;
    dev->CreateBuffer(&bd, nullptr, &buf);
    return buf;
}

template <typename T>
void updateConstantBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, const T& data) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &data, sizeof(T));
        ctx->Unmap(buf, 0);
    }
}

void unbindAllSRVs(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* nullSRVs[8] = {};
    ctx->VSSetShaderResources(0, 8, nullSRVs);
    ctx->PSSetShaderResources(0, 8, nullSRVs);
    ctx->CSSetShaderResources(0, 8, nullSRVs);
}

void unbindAllUAVs(ID3D11DeviceContext* ctx) {
    ID3D11UnorderedAccessView* nullUAVs[4] = {};
    ctx->CSSetUnorderedAccessViews(0, 4, nullUAVs, nullptr);
}

} // namespace

bool Renderer::init(HWND hwnd, uint32_t width, uint32_t height, const std::wstring& shaderDir) {
    m_hwnd = hwnd;
    m_width = std::max(width, 64u);
    m_height = std::max(height, 64u);

    if (!createDeviceAndSwapchain(hwnd)) return false;
    if (!createRenderTargets()) return false;
    if (!createStates()) return false;
    if (!createShaders(shaderDir)) return false;
    if (!createGeometry()) return false;
    if (!createWaterSimResources()) return false;

    m_frameCB    = createConstantBuffer<FrameCBData>(m_device.Get());
    m_dropletCB  = createConstantBuffer<DropletCBData>(m_device.Get());
    m_waterSimCB = createConstantBuffer<WaterSimCBData>(m_device.Get());
    m_postCB     = createConstantBuffer<PostCBData>(m_device.Get());

    // Pre-seed a gentle initial ripple in the forest pool so the very first frame has shimmering caustics
    stepWaterSimulation(0.016f, 0.0f, true, 0.10f);
    for (int i = 0; i < 18; ++i) {
        stepWaterSimulation(0.016f, i * 0.016f, false, 0.0f);
    }
    return true;
}

bool Renderer::createDeviceAndSwapchain(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = m_width;
    sd.BufferDesc.Height = m_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL flOut;
    D3D_FEATURE_LEVEL flReq = D3D_FEATURE_LEVEL_11_0;
    UINT flags = 0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        &flReq, 1, D3D11_SDK_VERSION,
        &sd, &m_swapchain, &m_device, &flOut, &m_ctx);

    if (FAILED(hr)) {
        // Fallback to WARP software rasterizer if running on a headless/no-GPU VM
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            &flReq, 1, D3D11_SDK_VERSION,
            &sd, &m_swapchain, &m_device, &flOut, &m_ctx);
        if (FAILED(hr)) {
            fprintf(stderr, "Failed to create DX11 device & swapchain (hr=0x%08X)\n", hr);
            return false;
        }
    }

    // Query GPU adapter name
    ComPtr<IDXGIDevice> dxgiDev;
    if (SUCCEEDED(m_device.As(&dxgiDev))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc = {};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                char buf[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof(buf), nullptr, nullptr);
                m_adapterName = buf;
            }
        }
    }
    return true;
}

bool Renderer::createRenderTargets() {
    m_backbufferRTV.Reset();

    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return false;
    if (FAILED(m_device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_backbufferRTV))) return false;

    auto createRT = [&](uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                        ComPtr<ID3D11Texture2D>& tex,
                        ComPtr<ID3D11RenderTargetView>& rtv,
                        ComPtr<ID3D11ShaderResourceView>& srv,
                        bool needRtv = true) -> bool {
        tex.Reset(); rtv.Reset(); srv.Reset();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = std::max(w, 1u);
        td.Height = std::max(h, 1u);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (needRtv ? D3D11_BIND_RENDER_TARGET : 0);
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &tex))) return false;
        if (needRtv && FAILED(m_device->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) return false;
        if (FAILED(m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return false;
        return true;
    };

    if (!createRT(m_width, m_height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_hdrTex, m_hdrRTV, m_hdrSRV)) return false;
    if (!createRT(m_width, m_height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_sceneCopyTex, m_dropletBackRTV, m_sceneCopySRV, false)) return false;
    if (!createRT(m_width, m_height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_dropletBackTex, m_dropletBackRTV, m_dropletBackSRV)) return false;
    if (!createRT(m_width, m_height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_dofTex, m_dofRTV, m_dofSRV)) return false;
    if (!createRT(m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM,     m_ldrTex, m_ldrRTV, m_ldrSRV)) return false;

    // Half-res volumetric god rays buffer
    m_volW = std::max(m_width / 2, 1u);
    m_volH = std::max(m_height / 2, 1u);
    if (!createRT(m_volW, m_volH, DXGI_FORMAT_R16G16B16A16_FLOAT, m_volTex, m_volRTV, m_volSRV)) return false;

    // 5-level HDR Bloom pyramid
    uint32_t bw = std::max(m_width / 2, 1u);
    uint32_t bh = std::max(m_height / 2, 1u);
    for (int i = 0; i < BLOOM_MIPS; ++i) {
        m_bloomW[i] = bw;
        m_bloomH[i] = bh;
        if (!createRT(bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, m_bloomTex[i], m_bloomRTV[i], m_bloomSRV[i])) return false;
        bw = std::max(bw / 2, 1u);
        bh = std::max(bh / 2, 1u);
    }

    // Main readable Depth Buffer (R32_TYPELESS -> D32_FLOAT + R32_FLOAT)
    m_depthTex.Reset(); m_depthDSV.Reset(); m_depthReadOnlyDSV.Reset(); m_depthSRV.Reset();
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = m_width;
        td.Height = m_height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_depthTex))) return false;

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
        dsvd.Format = DXGI_FORMAT_D32_FLOAT;
        dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(m_device->CreateDepthStencilView(m_depthTex.Get(), &dsvd, &m_depthDSV))) return false;

        dsvd.Flags = D3D11_DSV_READ_ONLY_DEPTH;
        if (FAILED(m_device->CreateDepthStencilView(m_depthTex.Get(), &dsvd, &m_depthReadOnlyDSV))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format = DXGI_FORMAT_R32_FLOAT;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        if (FAILED(m_device->CreateShaderResourceView(m_depthTex.Get(), &srvd, &m_depthSRV))) return false;
    }

    // 2048x2048 Shadow Map (created once)
    if (!m_shadowTex) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = SHADOW_SIZE;
        td.Height = SHADOW_SIZE;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_shadowTex))) return false;

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
        dsvd.Format = DXGI_FORMAT_D32_FLOAT;
        dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(m_device->CreateDepthStencilView(m_shadowTex.Get(), &dsvd, &m_shadowDSV))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format = DXGI_FORMAT_R32_FLOAT;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        if (FAILED(m_device->CreateShaderResourceView(m_shadowTex.Get(), &srvd, &m_shadowSRV))) return false;
    }
    return true;
}

void Renderer::resize(uint32_t width, uint32_t height) {
    if (width < 64 || height < 64 || !m_swapchain) return;
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_backbufferRTV.Reset();
    m_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTargets();
}

bool Renderer::createStates() {
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    m_device->CreateRasterizerState(&rd, &m_rsCullBack);

    rd.CullMode = D3D11_CULL_FRONT;
    m_device->CreateRasterizerState(&rd, &m_rsCullFront);

    rd.CullMode = D3D11_CULL_NONE;
    m_device->CreateRasterizerState(&rd, &m_rsCullNone);

    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_NONE;
    m_device->CreateRasterizerState(&rd, &m_rsWireframe);

    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthBias = 120;
    rd.SlopeScaledDepthBias = 2.0f;
    m_device->CreateRasterizerState(&rd, &m_rsShadow);

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    m_device->CreateDepthStencilState(&dsd, &m_dssDefault);

    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    m_device->CreateDepthStencilState(&dsd, &m_dssReadOnly);

    dsd.DepthEnable = FALSE;
    m_device->CreateDepthStencilState(&dsd, &m_dssDisabled);

    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    m_device->CreateDepthStencilState(&dsd, &m_dssSky);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&bd, &m_bsOpaque);

    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    m_device->CreateBlendState(&bd, &m_bsAlpha);

    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    m_device->CreateBlendState(&bd, &m_bsAdditive);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&sd, &m_sampLinearClamp);

    sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
    m_device->CreateSamplerState(&sd, &m_sampShadowCmp);

    return true;
}

bool Renderer::createShaders(const std::wstring& dir) {
    auto loadVS = [&](const std::wstring& file, const char* entry, ComPtr<ID3D11VertexShader>& out, ComPtr<ID3DBlob>* outBlob = nullptr) {
        auto blob = compileShader(dir + L"\\" + file, entry, "vs_5_0");
        if (!blob) return false;
        if (FAILED(m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out))) return false;
        if (outBlob) *outBlob = blob;
        return true;
    };
    auto loadHS = [&](const std::wstring& file, const char* entry, ComPtr<ID3D11HullShader>& out) {
        auto blob = compileShader(dir + L"\\" + file, entry, "hs_5_0");
        if (!blob) return false;
        return SUCCEEDED(m_device->CreateHullShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
    };
    auto loadDS = [&](const std::wstring& file, const char* entry, ComPtr<ID3D11DomainShader>& out) {
        auto blob = compileShader(dir + L"\\" + file, entry, "ds_5_0");
        if (!blob) return false;
        return SUCCEEDED(m_device->CreateDomainShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
    };
    auto loadPS = [&](const std::wstring& file, const char* entry, ComPtr<ID3D11PixelShader>& out) {
        auto blob = compileShader(dir + L"\\" + file, entry, "ps_5_0");
        if (!blob) return false;
        return SUCCEEDED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
    };
    auto loadCS = [&](const std::wstring& file, const char* entry, ComPtr<ID3D11ComputeShader>& out) {
        auto blob = compileShader(dir + L"\\" + file, entry, "cs_5_0");
        if (!blob) return false;
        return SUCCEEDED(m_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
    };

    ComPtr<ID3DBlob> branchVSBlob;
    if (!loadVS(L"branch_tess.hlsl", "VSMain",   m_vsBranch, &branchVSBlob)) return false;
    if (!loadVS(L"branch_tess.hlsl", "VSShadow", m_vsBranchShadow)) return false;
    if (!loadHS(L"branch_tess.hlsl", "HSMain",   m_hsBranch)) return false;
    if (!loadDS(L"branch_tess.hlsl", "DSMain",   m_dsBranch)) return false;
    if (!loadPS(L"branch_tess.hlsl", "PSMain",   m_psBranch)) return false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(geo::Vertex, pos),     D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(geo::Vertex, normal),  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(geo::Vertex, tangent), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(geo::Vertex, uv),      D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(geo::Vertex, extra),   D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(m_device->CreateInputLayout(layout, 5, branchVSBlob->GetBufferPointer(), branchVSBlob->GetBufferSize(), &m_vertexLayout))) return false;

    if (!loadVS(L"leaf.hlsl", "VSMain",   m_vsLeaf)) return false;
    if (!loadVS(L"leaf.hlsl", "VSShadow", m_vsLeafShadow)) return false;
    if (!loadPS(L"leaf.hlsl", "PSMain",   m_psLeaf)) return false;

    if (!loadVS(L"droplet_tess.hlsl", "VSMain",      m_vsDroplet)) return false;
    if (!loadHS(L"droplet_tess.hlsl", "HSMain",      m_hsDroplet)) return false;
    if (!loadDS(L"droplet_tess.hlsl", "DSMain",      m_dsDroplet)) return false;
    if (!loadPS(L"droplet_tess.hlsl", "PSBackface",  m_psDropletBack)) return false;
    if (!loadPS(L"droplet_tess.hlsl", "PSFrontface", m_psDropletFront)) return false;

    if (!loadCS(L"water_sim.hlsl", "CSStepWave",        m_csWaveStep)) return false;
    if (!loadCS(L"water_sim.hlsl", "CSCausticsNormals", m_csCaustics)) return false;

    if (!loadVS(L"pool.hlsl", "VSPoolBed",   m_vsPoolBed)) return false;
    if (!loadPS(L"pool.hlsl", "PSPoolBed",   m_psPoolBed)) return false;
    if (!loadVS(L"pool.hlsl", "VSPoolWater", m_vsPoolWater)) return false;
    if (!loadPS(L"pool.hlsl", "PSPoolWater", m_psPoolWater)) return false;

    if (!loadVS(L"sky_volumetric.hlsl", "VSFullscreen", m_vsSkyFullscreen)) return false;
    if (!loadPS(L"sky_volumetric.hlsl", "PSSky",        m_psSky)) return false;
    if (!loadPS(L"sky_volumetric.hlsl", "PSVolumetric", m_psVolumetric)) return false;

    if (!loadVS(L"particles.hlsl", "VSMain", m_vsParticles)) return false;
    if (!loadPS(L"particles.hlsl", "PSMain", m_psParticles)) return false;

    if (!loadVS(L"postprocess.hlsl", "VSFullscreen",     m_vsFullscreen)) return false;
    if (!loadPS(L"postprocess.hlsl", "PSBokehDoF",       m_psDof)) return false;
    if (!loadPS(L"postprocess.hlsl", "PSBloomPrefilter", m_psBloomPrefilter)) return false;
    if (!loadPS(L"postprocess.hlsl", "PSBloomDownsample",m_psBloomDown)) return false;
    if (!loadPS(L"postprocess.hlsl", "PSBloomUpsample",  m_psBloomUp)) return false;
    if (!loadPS(L"postprocess.hlsl", "PSFinalComposite", m_psComposite)) return false;
    if (!loadPS(L"postprocess.hlsl", "PSFXAA",           m_psFxaa)) return false;

    return true;
}

GpuMesh Renderer::uploadMesh(const geo::MeshData& data) {
    GpuMesh gm;
    gm.indexCount = static_cast<uint32_t>(data.indices.size());

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = static_cast<UINT>(data.vertices.size() * sizeof(geo::Vertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = {data.vertices.data(), 0, 0};
    m_device->CreateBuffer(&vbd, &vsd, &gm.vb);

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = static_cast<UINT>(data.indices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = {data.indices.data(), 0, 0};
    m_device->CreateBuffer(&ibd, &isd, &gm.ib);

    return gm;
}

bool Renderer::createGeometry() {
    m_branchMesh    = uploadMesh(geo::buildBranchMesh());
    m_bgForestMesh  = uploadMesh(geo::buildBackgroundForestMesh());
    m_leavesMesh    = uploadMesh(geo::buildLeavesMesh());
    m_dropletMesh   = uploadMesh(geo::buildIcosphereMesh(2));
    m_poolBedMesh   = uploadMesh(geo::buildPoolBedMesh());
    m_poolWaterMesh = uploadMesh(geo::buildPoolWaterMesh());

    auto particles = geo::buildDustParticles(140);
    m_particleCount = static_cast<uint32_t>(particles.size());

    D3D11_BUFFER_DESC pbd = {};
    pbd.ByteWidth = static_cast<UINT>(particles.size() * sizeof(geo::DustParticle));
    pbd.Usage = D3D11_USAGE_IMMUTABLE;
    pbd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    pbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    pbd.StructureByteStride = sizeof(geo::DustParticle);
    D3D11_SUBRESOURCE_DATA psd = {particles.data(), 0, 0};
    if (FAILED(m_device->CreateBuffer(&pbd, &psd, &m_particleBuf))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC psrv = {};
    psrv.Format = DXGI_FORMAT_UNKNOWN;
    psrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    psrv.Buffer.NumElements = m_particleCount;
    if (FAILED(m_device->CreateShaderResourceView(m_particleBuf.Get(), &psrv, &m_particleSRV))) return false;

    return true;
}

bool Renderer::createWaterSimResources() {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = WATER_SIM_SIZE;
    td.Height = WATER_SIM_SIZE;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    std::vector<float> zeros(WATER_SIM_SIZE * WATER_SIM_SIZE * 2, 0.0f);
    D3D11_SUBRESOURCE_DATA initData = {zeros.data(), WATER_SIM_SIZE * 8, 0};

    for (int i = 0; i < 2; ++i) {
        if (FAILED(m_device->CreateTexture2D(&td, &initData, &m_waterStateTex[i]))) return false;
        if (FAILED(m_device->CreateShaderResourceView(m_waterStateTex[i].Get(), nullptr, &m_waterStateSRV[i]))) return false;
        if (FAILED(m_device->CreateUnorderedAccessView(m_waterStateTex[i].Get(), nullptr, &m_waterStateUAV[i]))) return false;
    }

    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_waterNormalTex))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_waterNormalTex.Get(), nullptr, &m_waterNormalSRV))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_waterNormalTex.Get(), nullptr, &m_waterNormalUAV))) return false;

    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_causticTex))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_causticTex.Get(), nullptr, &m_causticSRV))) return false;
    if (FAILED(m_device->CreateUnorderedAccessView(m_causticTex.Get(), nullptr, &m_causticUAV))) return false;

    return true;
}

void Renderer::stepWaterSimulation(float dt, float totalTime, bool triggerSplash, float splashStrength) {
    unbindAllSRVs(m_ctx.Get());

    WaterSimCBData cb = {};
    cb.splashParams = Vec4(0.5f, 0.5f, 0.048f, triggerSplash ? splashStrength : 0.0f);
    cb.simParams    = Vec4(totalTime, dt, static_cast<float>(WATER_SIM_SIZE), 1.0f / WATER_SIM_SIZE);
    cb.sunDirSim    = Vec4(normalize(Vec3(0.52f, 0.44f, 0.73f)), 0.34f);
    updateConstantBuffer(m_ctx.Get(), m_waterSimCB.Get(), cb);

    ID3D11Buffer* cbs[] = {m_waterSimCB.Get()};
    m_ctx->CSSetConstantBuffers(0, 1, cbs);

    int src = m_waterPingPong;
    int dst = 1 - m_waterPingPong;

    // 1) Step 2D Wave PDE
    m_ctx->CSSetShader(m_csWaveStep.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs1[] = {m_waterStateSRV[src].Get()};
    ID3D11UnorderedAccessView* uavs1[] = {m_waterStateUAV[dst].Get()};
    m_ctx->CSSetShaderResources(0, 1, srvs1);
    m_ctx->CSSetUnorderedAccessViews(0, 1, uavs1, nullptr);
    m_ctx->Dispatch(WATER_SIM_SIZE / 16, WATER_SIM_SIZE / 16, 1);

    unbindAllUAVs(m_ctx.Get());
    unbindAllSRVs(m_ctx.Get());
    m_waterPingPong = dst;

    // 2) Synthesize Normals + Underwater Chromatic Caustics
    m_ctx->CSSetShader(m_csCaustics.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs2[] = {m_waterStateSRV[m_waterPingPong].Get()};
    ID3D11UnorderedAccessView* uavs2[] = {m_waterNormalUAV.Get(), m_causticUAV.Get()};
    m_ctx->CSSetShaderResources(0, 1, srvs2);
    m_ctx->CSSetUnorderedAccessViews(0, 2, uavs2, nullptr);
    m_ctx->Dispatch(WATER_SIM_SIZE / 16, WATER_SIM_SIZE / 16, 1);

    unbindAllUAVs(m_ctx.Get());
    unbindAllSRVs(m_ctx.Get());
    m_ctx->CSSetShader(nullptr, nullptr, 0);
}

void Renderer::updateSimulation(float dt, float totalTime, const DemoSettings& settings, bool forceDrop) {
    if (forceDrop) {
        // Jump straight to necking / imminent pinch-off!
        m_cycleTime = m_cycleDuration * 0.56f;
        m_hasImpactedThisCycle = false;
    }

    if (!settings.paused) {
        m_cycleTime += dt;
        if (m_cycleTime >= m_cycleDuration) {
            m_cycleTime -= m_cycleDuration;
            m_hasImpactedThisCycle = false;
        }
    }

    const Vec3 attach(geo::DRIP_TIP_X, geo::DRIP_TIP_Y, geo::DRIP_TIP_Z);
    const float u = m_cycleTime / m_cycleDuration;

    // Reset instances
    m_pendantDrop = {};
    m_fallingDrop = {};
    m_splashJet   = {};

    const float pinchU = 0.65f;
    const float maxRadius = 0.088f;

    if (u < pinchU) {
        // --- Phase 1 & 2: Pooling, Swelling & Capillary Necking on Branch Underside ---
        float growT = smoothstep(0.0f, 0.50f, u);
        float neckT = smoothstep(0.46f, pinchU, u);

        float radius = lerp(0.024f, maxRadius, std::pow(growT, 0.72f));
        float elong  = 0.12f * growT + 0.88f * std::pow(neckT, 1.45f);
        float pinch  = std::pow(neckT, 1.6f);
        float tremor = neckT * 0.65f * std::sin(totalTime * 24.0f);

        float hangDropY = attach.y - radius * (0.72f + elong * 1.15f);

        m_pendantDrop.active = true;
        m_pendantDrop.pos = Vec3(attach.x, hangDropY, attach.z);
        m_pendantDrop.radius = radius;
        m_pendantDrop.elongation = elong;
        m_pendantDrop.neckPinch = pinch;
        m_pendantDrop.oscillation = tremor;
        m_pendantDrop.mode = 0.0f;
        m_pendantDrop.attachPos = attach;

        m_heroDropCenter = m_pendantDrop.pos;
        m_heroDropRadius = radius;
    } else {
        // --- Phase 3 & 4: Post Pinch-Off (Recoiling Meniscus + Falling Drop + Splash Jet) ---
        float fallElapsed = (u - pinchU) * m_cycleDuration;

        // 1) Residual recoiling meniscus bead clinging to branch tip
        float recoilOsc = std::exp(-fallElapsed * 5.2f) * std::cos(fallElapsed * 28.0f);
        float resRadius = lerp(0.022f, 0.032f, smoothstep(0.4f, 2.0f, fallElapsed));
        m_pendantDrop.active = true;
        m_pendantDrop.pos = Vec3(attach.x, attach.y - resRadius * (0.68f - recoilOsc * 0.35f), attach.z);
        m_pendantDrop.radius = resRadius;
        m_pendantDrop.elongation = std::max(0.0f, 0.15f - recoilOsc * 0.25f);
        m_pendantDrop.neckPinch = 0.0f;
        m_pendantDrop.oscillation = recoilOsc;
        m_pendantDrop.mode = 0.0f;
        m_pendantDrop.attachPos = attach;

        // 2) Detached falling droplet accelerating under gravity with Rayleigh-Plateau oscillation
        const float startY = attach.y - maxRadius * 2.15f;
        const float gravity = 7.4f; // Slightly stylized cinema gravity for graceful visibility
        const float initVy = -0.35f;
        float fallY = startY + initVy * fallElapsed - 0.5f * gravity * fallElapsed * fallElapsed;
        float fallR = maxRadius * 0.90f;

        if (fallY > geo::POOL_WATER_Y - fallR * 0.35f) {
            m_fallingDrop.active = true;
            m_fallingDrop.pos = Vec3(attach.x, fallY, attach.z);
            m_fallingDrop.radius = fallR;
            // Capillary quadrupole oscillation (prolate <-> oblate)
            m_fallingDrop.oscillation = std::exp(-fallElapsed * 2.2f) * std::cos(fallElapsed * 34.0f);
            m_fallingDrop.elongation = std::exp(-fallElapsed * 8.5f); // Recoiling pinched tail
            m_fallingDrop.mode = 1.0f;
            m_fallingDrop.attachPos = attach;

            m_heroDropCenter = m_fallingDrop.pos;
            m_heroDropRadius = fallR;
        } else {
            // Trigger GPU Compute Wave Impulse once on impact!
            if (!m_hasImpactedThisCycle) {
                m_hasImpactedThisCycle = true;
                m_pendingSplash = true;
                m_pendingSplashStrength = 0.34f;
            }

            // Compute time since impact for brief rising Worthington splash jet bead
            // Solve startY + initVy*t - 0.5*g*t^2 = POOL_WATER_Y
            float disc = initVy * initVy - 2.0f * gravity * (geo::POOL_WATER_Y - startY);
            float tImpact = (initVy + std::sqrt(std::max(disc, 0.0f))) / gravity;
            float sinceImpact = fallElapsed - tImpact;

            if (sinceImpact > 0.02f && sinceImpact < 0.42f) {
                float jetT = (sinceImpact - 0.02f) / 0.40f;
                float jetH = std::sin(jetT * PI) * 0.24f;
                float jetR = maxRadius * 0.46f * std::sin(jetT * PI);
                m_splashJet.active = true;
                m_splashJet.pos = Vec3(attach.x, geo::POOL_WATER_Y + jetH, attach.z);
                m_splashJet.radius = std::max(jetR, 0.008f);
                m_splashJet.elongation = 0.65f * (1.0f - jetT);
                m_splashJet.mode = 2.0f;
            }

            m_heroDropCenter = m_pendantDrop.pos;
            m_heroDropRadius = resRadius;
        }
    }

    // Fixed-timestep 60 Hz wave simulation stepping so ripple speed is framerate-independent
    if (!settings.paused) {
        m_simAccum += std::min(dt, 0.05f);
        const float fixedStep = 1.0f / 60.0f;
        int substeps = 0;
        while (m_simAccum >= fixedStep && substeps < 3) {
            stepWaterSimulation(fixedStep, totalTime, m_pendingSplash, m_pendingSplashStrength);
            m_pendingSplash = false;
            m_pendingSplashStrength = 0.0f;
            m_simAccum -= fixedStep;
            ++substeps;
        }
    }
}

void Renderer::drawDroplet(const DropletInstance& d, bool wireframe) {
    if (!d.active) return;

    DropletCBData dcb = {};
    dcb.dropPosRadius = Vec4(d.pos, d.radius);
    dcb.dropDeform    = Vec4(d.elongation, d.neckPinch, d.oscillation, d.mode);
    dcb.dropAttach    = Vec4(d.attachPos, d.phase);
    updateConstantBuffer(m_ctx.Get(), m_dropletCB.Get(), dcb);

    ID3D11Buffer* cbs[] = {m_frameCB.Get(), m_dropletCB.Get()};
    m_ctx->VSSetConstantBuffers(0, 2, cbs);
    m_ctx->HSSetConstantBuffers(0, 2, cbs);
    m_ctx->DSSetConstantBuffers(0, 2, cbs);
    m_ctx->PSSetConstantBuffers(0, 2, cbs);

    UINT stride = sizeof(geo::Vertex), offset = 0;
    m_ctx->IASetInputLayout(m_vertexLayout.Get());
    m_ctx->IASetVertexBuffers(0, 1, m_dropletMesh.vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_dropletMesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

    m_ctx->VSSetShader(m_vsDroplet.Get(), nullptr, 0);
    m_ctx->HSSetShader(m_hsDroplet.Get(), nullptr, 0);
    m_ctx->DSSetShader(m_dsDroplet.Get(), nullptr, 0);

    // --- Pass A: Back-face thickness & exit normal capture ---
    unbindAllSRVs(m_ctx.Get());
    float clearBack[4] = {0.0f, 0.0f, 0.0f, 100.0f};
    m_ctx->ClearRenderTargetView(m_dropletBackRTV.Get(), clearBack);
    m_ctx->OMSetRenderTargets(1, m_dropletBackRTV.GetAddressOf(), m_depthReadOnlyDSV.Get());
    m_ctx->OMSetDepthStencilState(m_dssReadOnly.Get(), 0);
    m_ctx->OMSetBlendState(m_bsOpaque.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->RSSetState(m_rsCullFront.Get());
    m_ctx->PSSetShader(m_psDropletBack.Get(), nullptr, 0);
    m_ctx->DrawIndexed(m_dropletMesh.indexCount, 0, 0);

    // --- Pass B: Front-face chromatic dispersion refraction, internal caustics & Fresnel ---
    m_ctx->OMSetRenderTargets(1, m_hdrRTV.GetAddressOf(), m_depthDSV.Get());
    m_ctx->OMSetDepthStencilState(m_dssDefault.Get(), 0);
    m_ctx->RSSetState(wireframe ? m_rsWireframe.Get() : m_rsCullBack.Get());
    m_ctx->PSSetShader(m_psDropletFront.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {m_sceneCopySRV.Get(), m_dropletBackSRV.Get()};
    m_ctx->PSSetShaderResources(0, 2, srvs);
    m_ctx->DrawIndexed(m_dropletMesh.indexCount, 0, 0);

    // Unbind tessellation stages cleanly
    m_ctx->HSSetShader(nullptr, nullptr, 0);
    m_ctx->DSSetShader(nullptr, nullptr, 0);
}

void Renderer::renderFrame(
    const Vec3& camPos,
    const Vec3& camTarget,
    float totalTime,
    const DemoSettings& settings)
{
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    const float nearZ = 0.08f;
    const float farZ = 45.0f;

    Mat4 view = Mat4::lookAtLH(camPos, camTarget, Vec3(0.0f, 1.0f, 0.0f));
    Mat4 proj = Mat4::perspectiveFovLH(42.0f * DEG2RAD, aspect, nearZ, farZ);
    Mat4 viewProj = view * proj;
    Mat4 invViewProj = viewProj.inverse();

    // Warm golden-hour backlit sun
    Vec3 sunDir = normalize(Vec3(0.52f, 0.44f, 0.73f));
    Vec3 sunColor(3.6f, 2.35f, 1.38f);

    Vec3 lightPos = sunDir * 8.5f;
    Mat4 lightView = Mat4::lookAtLH(lightPos, Vec3(-0.3f, 0.9f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
    Mat4 lightProj = Mat4::orthoOffCenterLH(-4.2f, 4.2f, -3.5f, 3.5f, 1.0f, 18.0f);
    Mat4 lightViewProj = lightView * lightProj;

    // Auto-focus cinematic Bokeh DoF right on the hero droplet / branch tip!
    float focusDist = length(camPos - Vec3(geo::DRIP_TIP_X, geo::DRIP_TIP_Y - 0.12f, geo::DRIP_TIP_Z));
    if (settings.cameraPreset == 2) {
        // Pool impact & caustics view focuses on the pool center
        focusDist = length(camPos - Vec3(0.0f, geo::POOL_WATER_Y, 0.0f));
    }

    FrameCBData fcb = {};
    fcb.viewProj        = viewProj.transpose();
    fcb.invViewProj     = invViewProj.transpose();
    fcb.view            = view.transpose();
    fcb.proj            = proj.transpose();
    fcb.lightViewProj   = lightViewProj.transpose();
    fcb.cameraPosTime   = Vec4(camPos, totalTime);
    fcb.sunDirNear      = Vec4(sunDir, nearZ);
    fcb.sunColorFar     = Vec4(sunColor, farZ);
    fcb.resolutionInv   = Vec4(static_cast<float>(m_width), static_cast<float>(m_height),
                               1.0f / m_width, 1.0f / m_height);
    fcb.dofParams       = Vec4(focusDist, 1.15f, 11.0f * (m_height / 1440.0f), settings.dofEnabled ? 1.0f : 0.0f);
    fcb.dripOriginPool  = Vec4(geo::DRIP_TIP_X, geo::DRIP_TIP_Y, geo::DRIP_TIP_Z, geo::POOL_WATER_Y);
    fcb.heroDropCaustic = Vec4(m_heroDropCenter, m_heroDropRadius);
    fcb.featureFlags    = Vec4(settings.tessScale,
                               settings.volumetricEnabled ? 1.0f : 0.0f,
                               settings.bloomEnabled ? 0.24f : 0.0f,
                               settings.wireframe ? 1.0f : 0.0f);
    updateConstantBuffer(m_ctx.Get(), m_frameCB.Get(), fcb);

    ID3D11Buffer* frameCBs[] = {m_frameCB.Get()};
    m_ctx->VSSetConstantBuffers(0, 1, frameCBs);
    m_ctx->HSSetConstantBuffers(0, 1, frameCBs);
    m_ctx->DSSetConstantBuffers(0, 1, frameCBs);
    m_ctx->PSSetConstantBuffers(0, 1, frameCBs);

    ID3D11SamplerState* samps[] = {m_sampLinearClamp.Get(), m_sampShadowCmp.Get()};
    m_ctx->VSSetSamplers(0, 2, samps);
    m_ctx->PSSetSamplers(0, 2, samps);

    // =========================================================================
    // PASS 1: 2048x2048 Directional Sun Shadow Map
    // =========================================================================
    unbindAllSRVs(m_ctx.Get());
    D3D11_VIEWPORT shadowVP = {0, 0, static_cast<float>(SHADOW_SIZE), static_cast<float>(SHADOW_SIZE), 0.0f, 1.0f};
    m_ctx->RSSetViewports(1, &shadowVP);
    m_ctx->ClearDepthStencilView(m_shadowDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    m_ctx->OMSetRenderTargets(0, nullptr, m_shadowDSV.Get());
    m_ctx->OMSetDepthStencilState(m_dssDefault.Get(), 0);
    m_ctx->RSSetState(m_rsShadow.Get());
    m_ctx->HSSetShader(nullptr, nullptr, 0);
    m_ctx->DSSetShader(nullptr, nullptr, 0);
    m_ctx->PSSetShader(nullptr, nullptr, 0);

    UINT stride = sizeof(geo::Vertex), offset = 0;
    m_ctx->IASetInputLayout(m_vertexLayout.Get());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Branch shadow
    m_ctx->VSSetShader(m_vsBranchShadow.Get(), nullptr, 0);
    m_ctx->IASetVertexBuffers(0, 1, m_branchMesh.vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_branchMesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_ctx->DrawIndexed(m_branchMesh.indexCount, 0, 0);

    // Leaves shadow
    m_ctx->VSSetShader(m_vsLeafShadow.Get(), nullptr, 0);
    m_ctx->IASetVertexBuffers(0, 1, m_leavesMesh.vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_leavesMesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_ctx->DrawIndexed(m_leavesMesh.indexCount, 0, 0);

    // =========================================================================
    // PASS 2: Main HDR Scene (Sky + Background Forest + Tessellated Branch + Leaves + Pool Bed)
    // =========================================================================
    D3D11_VIEWPORT fullVP = {0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    m_ctx->RSSetViewports(1, &fullVP);
    float clearHdr[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_ctx->ClearRenderTargetView(m_hdrRTV.Get(), clearHdr);
    m_ctx->ClearDepthStencilView(m_depthDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    m_ctx->OMSetRenderTargets(1, m_hdrRTV.GetAddressOf(), m_depthDSV.Get());
    m_ctx->OMSetBlendState(m_bsOpaque.Get(), nullptr, 0xFFFFFFFF);

    // 2a) Golden-hour Woodland Sky
    m_ctx->OMSetDepthStencilState(m_dssSky.Get(), 0);
    m_ctx->RSSetState(m_rsCullNone.Get());
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vsSkyFullscreen.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psSky.Get(), nullptr, 0);
    m_ctx->Draw(3, 0);

    // 2b) Hardware-Tessellated Gnarled Mossy Tree Branch + Background Forest Trunks
    m_ctx->OMSetDepthStencilState(m_dssDefault.Get(), 0);
    m_ctx->RSSetState(settings.wireframe ? m_rsWireframe.Get() : m_rsCullBack.Get());
    m_ctx->IASetInputLayout(m_vertexLayout.Get());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    m_ctx->VSSetShader(m_vsBranch.Get(), nullptr, 0);
    m_ctx->HSSetShader(m_hsBranch.Get(), nullptr, 0);
    m_ctx->DSSetShader(m_dsBranch.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psBranch.Get(), nullptr, 0);
    m_ctx->PSSetShaderResources(0, 1, m_shadowSRV.GetAddressOf());

    m_ctx->IASetVertexBuffers(0, 1, m_branchMesh.vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_branchMesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_ctx->DrawIndexed(m_branchMesh.indexCount, 0, 0);

    m_ctx->IASetVertexBuffers(0, 1, m_bgForestMesh.vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_bgForestMesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_ctx->DrawIndexed(m_bgForestMesh.indexCount, 0, 0);

    m_ctx->HSSetShader(nullptr, nullptr, 0);
    m_ctx->DSSetShader(nullptr, nullptr, 0);

    // 2c) Backlit Translucent Forest Leaves (Subsurface Scattering)
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->RSSetState(settings.wireframe ? m_rsWireframe.Get() : m_rsCullNone.Get());
    m_ctx->VSSetShader(m_vsLeaf.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psLeaf.Get(), nullptr, 0);
    m_ctx->PSSetShaderResources(0, 1, m_shadowSRV.GetAddressOf());
    m_ctx->IASetVertexBuffers(0, 1, m_leavesMesh.vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_leavesMesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_ctx->DrawIndexed(m_leavesMesh.indexCount, 0, 0);

    // 2d) Mossy Forest Floor & Submerged Pool Bed (with Compute Underwater Caustics!)
    m_ctx->RSSetState(settings.wireframe ? m_rsWireframe.Get() : m_rsCullBack.Get());
    m_ctx->VSSetShader(m_vsPoolBed.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psPoolBed.Get(), nullptr, 0);
    ID3D11ShaderResourceView* bedSRVs[] = {m_shadowSRV.Get(), m_waterNormalSRV.Get(), m_causticSRV.Get()};
    m_ctx->PSSetShaderResources(0, 3, bedSRVs);
    m_ctx->IASetVertexBuffers(0, 1, m_poolBedMesh.vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_poolBedMesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_ctx->DrawIndexed(m_poolBedMesh.indexCount, 0, 0);

    // =========================================================================
    // PASS 3: Rippling Pool Water Surface (Refracting the Caustic-Lit Pool Bed)
    // =========================================================================
    m_ctx->CopyResource(m_sceneCopyTex.Get(), m_hdrTex.Get());

    m_ctx->OMSetBlendState(m_bsAlpha.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->OMSetDepthStencilState(m_dssReadOnly.Get(), 0);
    m_ctx->RSSetState(settings.wireframe ? m_rsWireframe.Get() : m_rsCullNone.Get());
    m_ctx->VSSetShader(m_vsPoolWater.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psPoolWater.Get(), nullptr, 0);
    m_ctx->VSSetShaderResources(1, 1, m_waterNormalSRV.GetAddressOf());
    ID3D11ShaderResourceView* waterSRVs[] = {m_shadowSRV.Get(), m_waterNormalSRV.Get(), m_causticSRV.Get(), m_sceneCopySRV.Get()};
    m_ctx->PSSetShaderResources(0, 4, waterSRVs);
    m_ctx->IASetVertexBuffers(0, 1, m_poolWaterMesh.vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_poolWaterMesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    m_ctx->DrawIndexed(m_poolWaterMesh.indexCount, 0, 0);

    // =========================================================================
    // PASS 4: Hardware-Tessellated Water Droplets (Refracting Full Scene + Pool)
    // =========================================================================
    m_ctx->CopyResource(m_sceneCopyTex.Get(), m_hdrTex.Get());
    drawDroplet(m_pendantDrop, settings.wireframe);
    drawDroplet(m_fallingDrop, settings.wireframe);
    drawDroplet(m_splashJet,   settings.wireframe);

    // =========================================================================
    // PASS 5: Sunlit Floating Forest Dust Motes (Additive Bokeh Particles)
    // =========================================================================
    unbindAllSRVs(m_ctx.Get());
    m_ctx->OMSetRenderTargets(1, m_hdrRTV.GetAddressOf(), m_depthReadOnlyDSV.Get());
    m_ctx->OMSetDepthStencilState(m_dssReadOnly.Get(), 0);
    m_ctx->OMSetBlendState(m_bsAdditive.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->RSSetState(m_rsCullNone.Get());
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vsParticles.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psParticles.Get(), nullptr, 0);
    m_ctx->VSSetShaderResources(0, 1, m_particleSRV.GetAddressOf());
    m_ctx->Draw(m_particleCount * 6, 0);

    // =========================================================================
    // PASS 6: Half-Resolution Volumetric Crepuscular God Rays
    // =========================================================================
    unbindAllSRVs(m_ctx.Get());
    D3D11_VIEWPORT volVP = {0, 0, static_cast<float>(m_volW), static_cast<float>(m_volH), 0.0f, 1.0f};
    m_ctx->RSSetViewports(1, &volVP);
    m_ctx->OMSetRenderTargets(1, m_volRTV.GetAddressOf(), nullptr);
    m_ctx->OMSetDepthStencilState(m_dssDisabled.Get(), 0);
    m_ctx->OMSetBlendState(m_bsOpaque.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->VSSetShader(m_vsSkyFullscreen.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psVolumetric.Get(), nullptr, 0);
    ID3D11ShaderResourceView* volSRVs[] = {m_depthSRV.Get(), m_shadowSRV.Get()};
    m_ctx->PSSetShaderResources(0, 2, volSRVs);
    m_ctx->Draw(3, 0);

    // =========================================================================
    // PASS 7: Cinematic Golden-Angle Spiral Bokeh Depth of Field
    // =========================================================================
    unbindAllSRVs(m_ctx.Get());
    m_ctx->RSSetViewports(1, &fullVP);
    m_ctx->OMSetRenderTargets(1, m_dofRTV.GetAddressOf(), nullptr);
    m_ctx->VSSetShader(m_vsFullscreen.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_psDof.Get(), nullptr, 0);
    m_ctx->PSSetShaderResources(0, 1, m_hdrSRV.GetAddressOf());
    m_ctx->PSSetShaderResources(3, 1, m_depthSRV.GetAddressOf());
    m_ctx->Draw(3, 0);

    // =========================================================================
    // PASS 8: 5-Mip Progressive HDR Bloom Pyramid
    // =========================================================================
    ID3D11Buffer* postCBs[] = {m_postCB.Get()};
    m_ctx->PSSetConstantBuffers(2, 1, postCBs);

    // 8a) Prefilter -> mip 0
    unbindAllSRVs(m_ctx.Get());
    D3D11_VIEWPORT bvp = {0, 0, static_cast<float>(m_bloomW[0]), static_cast<float>(m_bloomH[0]), 0.0f, 1.0f};
    m_ctx->RSSetViewports(1, &bvp);
    m_ctx->OMSetRenderTargets(1, m_bloomRTV[0].GetAddressOf(), nullptr);
    m_ctx->PSSetShader(m_psBloomPrefilter.Get(), nullptr, 0);
    m_ctx->PSSetShaderResources(0, 1, m_dofSRV.GetAddressOf());
    m_ctx->Draw(3, 0);

    // 8b) Downsample chain
    m_ctx->PSSetShader(m_psBloomDown.Get(), nullptr, 0);
    for (int i = 1; i < BLOOM_MIPS; ++i) {
        unbindAllSRVs(m_ctx.Get());
        PostCBData pcb = {Vec4(1.0f / m_bloomW[i - 1], 1.0f / m_bloomH[i - 1], 1.0f, 1.0f)};
        updateConstantBuffer(m_ctx.Get(), m_postCB.Get(), pcb);
        bvp = {0, 0, static_cast<float>(m_bloomW[i]), static_cast<float>(m_bloomH[i]), 0.0f, 1.0f};
        m_ctx->RSSetViewports(1, &bvp);
        m_ctx->OMSetRenderTargets(1, m_bloomRTV[i].GetAddressOf(), nullptr);
        m_ctx->PSSetShaderResources(0, 1, m_bloomSRV[i - 1].GetAddressOf());
        m_ctx->Draw(3, 0);
    }

    // 8c) Additive tent-filter upsample chain
    m_ctx->OMSetBlendState(m_bsAdditive.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->PSSetShader(m_psBloomUp.Get(), nullptr, 0);
    for (int i = BLOOM_MIPS - 1; i >= 1; --i) {
        unbindAllSRVs(m_ctx.Get());
        PostCBData pcb = {Vec4(1.0f / m_bloomW[i], 1.0f / m_bloomH[i], 1.35f, 1.0f)};
        updateConstantBuffer(m_ctx.Get(), m_postCB.Get(), pcb);
        bvp = {0, 0, static_cast<float>(m_bloomW[i - 1]), static_cast<float>(m_bloomH[i - 1]), 0.0f, 1.0f};
        m_ctx->RSSetViewports(1, &bvp);
        m_ctx->OMSetRenderTargets(1, m_bloomRTV[i - 1].GetAddressOf(), nullptr);
        m_ctx->PSSetShaderResources(0, 1, m_bloomSRV[i].GetAddressOf());
        m_ctx->Draw(3, 0);
    }

    // =========================================================================
    // PASS 9: Final HDR Composite + ACES Filmic Tone Mapping + Vignette -> LDR
    // =========================================================================
    unbindAllSRVs(m_ctx.Get());
    m_ctx->RSSetViewports(1, &fullVP);
    m_ctx->OMSetBlendState(m_bsOpaque.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->OMSetRenderTargets(1, m_ldrRTV.GetAddressOf(), nullptr);
    m_ctx->PSSetShader(m_psComposite.Get(), nullptr, 0);
    ID3D11ShaderResourceView* compSRVs[] = {m_dofSRV.Get(), m_volSRV.Get(), m_bloomSRV[0].Get()};
    m_ctx->PSSetShaderResources(0, 3, compSRVs);
    m_ctx->Draw(3, 0);

    // =========================================================================
    // PASS 10: FXAA Anti-Aliasing + Top-Right FPS Counter Overlay -> Backbuffer
    // =========================================================================
    unbindAllSRVs(m_ctx.Get());
    PostCBData hudCB = {Vec4(settings.displayFps, settings.displayFrameMs, settings.showFps ? 1.0f : 0.0f, 0.0f)};
    updateConstantBuffer(m_ctx.Get(), m_postCB.Get(), hudCB);
    m_ctx->OMSetRenderTargets(1, m_backbufferRTV.GetAddressOf(), nullptr);
    m_ctx->PSSetShader(m_psFxaa.Get(), nullptr, 0);
    m_ctx->PSSetShaderResources(0, 1, m_ldrSRV.GetAddressOf());
    m_ctx->Draw(3, 0);

    unbindAllSRVs(m_ctx.Get());
}

void Renderer::present(bool vsync) {
    m_swapchain->Present(vsync ? 1 : 0, 0);
}

bool Renderer::saveBackbufferBMP(const std::string& filename) {
    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return false;

    D3D11_TEXTURE2D_DESC td = {};
    backbuffer->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &staging))) return false;
    m_ctx->CopyResource(staging.Get(), backbuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    const uint32_t w = td.Width, h = td.Height;
    const uint32_t rowStride = ((w * 3 + 3) & ~3u);
    const uint32_t pixelBytes = rowStride * h;

    std::ofstream f(filename, std::ios::binary);
    if (!f) {
        m_ctx->Unmap(staging.Get(), 0);
        return false;
    }

    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    uint32_t fileSize = 54 + pixelBytes;
    memcpy(&header[2], &fileSize, 4);
    uint32_t dataOffset = 54;
    memcpy(&header[10], &dataOffset, 4);
    uint32_t infoSize = 40;
    memcpy(&header[14], &infoSize, 4);
    memcpy(&header[18], &w, 4);
    memcpy(&header[22], &h, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(&header[26], &planes, 2);
    memcpy(&header[28], &bpp, 2);
    memcpy(&header[34], &pixelBytes, 4);
    f.write(reinterpret_cast<const char*>(header), 54);

    std::vector<uint8_t> row(rowStride, 0);
    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2]; // B
            row[x * 3 + 1] = src[x * 4 + 1]; // G
            row[x * 3 + 2] = src[x * 4 + 0]; // R
        }
        f.write(reinterpret_cast<const char*>(row.data()), rowStride);
    }

    m_ctx->Unmap(staging.Get(), 0);
    return true;
}
