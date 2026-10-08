#include "render.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numbers>
#include <tuple>
#include <utility>
#include <vector>

#include <safetyhook.hpp>

#include "game.hpp"
#include "log.hpp"
#include "skate.hpp"
#include "skate_ffi.h"

#pragma comment(lib, "d3d11.lib")

// How the swap chain and its device are found, and the care taken around MGSHDFix (MIT,
// https://github.com/ShizCalev/MGSHDFix, src/resources/d3d11_api.cpp and d3d11_state_cache.cpp),
// which runs in the same process: it mid-hooks the game's Present call site, inline-hooks
// IDXGIFactory::CreateSwapChain, IDXGISwapChain::ResizeBuffers and several context methods
// (IASetInputLayout, IASetPrimitiveTopology, OMSetRenderTargets, ClearState). So here:
// - Present/Present1 are hooked inside dxgi.dll, not at the call site; we run after its effects.
// - The throwaway swap chain comes from CreateSwapChainForHwnd, never CreateSwapChain, so its
//   CreateSwapChain hook never sees (and adopts) our dummy.
// - Render targets are bound with OMSetRenderTargetsAndUnorderedAccessViews, which it doesn't
//   hook: its OMSetRenderTargets hook would otherwise take our depth buffer for the scene depth
//   and keep a reference to the back buffer view.
// - The back buffer view is created and released every frame so ResizeBuffers never fails.
// - Never ClearState; IA calls go through its state cache, which stays consistent.

namespace render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kMmPerUnit = 1000.f;

// ---- settings ----------------------------------------------------------------------------

struct Settings {
  float fov_deg = 50.f;      // vertical field of view
  float near_mm = 100.f;
  float far_mm = 300000.f;
  float aspect = 0.f;        // width / height; 0 = from the back buffer
  bool flip_z = false;       // false: right-handed view (looks down -z); true: left-handed (mirror image)
  bool depth_game = false;   // test against the depth buffer bound at Present, if any (else our own)
  bool debug = false;        // marker + axes at Snake's position, skating or not
};

std::mutex g_settings_mutex;
Settings g_settings;

Settings settings() {
  std::lock_guard lock(g_settings_mutex);
  return g_settings;
}

std::string describe(const Settings& s) {
  char text[256];
  snprintf(text, sizeof(text), "fov_deg=%.2f near_mm=%.1f far_mm=%.0f aspect=%.4f flip_z=%d depth_game=%d debug=%d",
           s.fov_deg, s.near_mm, s.far_mm, s.aspect, s.flip_z ? 1 : 0, s.depth_game ? 1 : 0, s.debug ? 1 : 0);
  return text;
}

bool parse_bool(const std::string& text, bool& out) {
  if (text == "1" || text == "on" || text == "true") return out = true, true;
  if (text == "0" || text == "off" || text == "false") return out = false, true;
  return false;
}

bool parse_float(const std::string& text, float& out) {
  char* end = nullptr;
  const float v = std::strtof(text.c_str(), &end);
  if (text.empty() || *end != '\0' || !std::isfinite(v)) return false;
  out = v;
  return true;
}

// Applies key=value to s; empty on success, else the error.
std::string apply(Settings& s, const std::string& key, const std::string& value) {
  Settings n = s;
  bool ok = false;
  if (key == "fov_deg") ok = parse_float(value, n.fov_deg) && n.fov_deg > 1.f && n.fov_deg < 170.f;
  else if (key == "near_mm") ok = parse_float(value, n.near_mm) && n.near_mm > 0.f && n.near_mm < n.far_mm;
  else if (key == "far_mm") ok = parse_float(value, n.far_mm) && n.far_mm > n.near_mm;
  else if (key == "aspect") ok = parse_float(value, n.aspect) && n.aspect >= 0.f && n.aspect < 10.f;
  else if (key == "flip_z") ok = parse_bool(value, n.flip_z);
  else if (key == "depth_game") ok = parse_bool(value, n.depth_game);
  else if (key == "debug") ok = parse_bool(value, n.debug);
  else return "unknown key (fov_deg near_mm far_mm aspect flip_z depth_game debug)";
  if (!ok) return "bad value for " + key;
  s = n;
  return "";
}

void load_settings(const std::filesystem::path& ini) {
  Settings s;
  for (const char* key : {"fov_deg", "near_mm", "far_mm", "aspect", "flip_z", "depth_game", "debug"}) {
    char value[64] = {};
    GetPrivateProfileStringA("render", key, "", value, sizeof(value), ini.string().c_str());
    if (!*value) continue;
    if (const std::string error = apply(s, key, value); !error.empty()) {
      log("render: ini [render] %s=%s ignored: %s", key, value, error.c_str());
    }
  }
  std::lock_guard lock(g_settings_mutex);
  g_settings = s;
}

// ---- math (column vectors, column-major storage like the engine's matrices) --------------

struct Vec {
  float x, y, z;
};
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec normalize(Vec a) {
  const float l = std::sqrt(dot(a, a));
  return l > 0.f ? Vec{a.x / l, a.y / l, a.z / l} : a;
}

struct Mat {
  float m[16] = {};
  float& at(int row, int col) { return m[col * 4 + row]; }
  float at(int row, int col) const { return m[col * 4 + row]; }
};

Mat mul(const Mat& a, const Mat& b) {
  Mat r;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      float sum = 0.f;
      for (int k = 0; k < 4; ++k) sum += a.at(i, k) * b.at(k, j);
      r.at(i, j) = sum;
    }
  }
  return r;
}

// Translate by t after scaling by s.
Mat place(Vec t, Vec s) {
  Mat r;
  r.at(0, 0) = s.x;
  r.at(1, 1) = s.y;
  r.at(2, 2) = s.z;
  r.at(0, 3) = t.x;
  r.at(1, 3) = t.y;
  r.at(2, 3) = t.z;
  r.at(3, 3) = 1.f;
  return r;
}

// D3DXMatrixLookAtRH / LH: right-handed looks down -z, left-handed down +z.
Mat look_at(Vec eye, Vec target, bool left_handed) {
  const Vec z = normalize(left_handed ? target - eye : eye - target);
  const Vec x = normalize(cross({0.f, 1.f, 0.f}, z));
  const Vec y = cross(z, x);
  Mat r;
  const Vec axes[3] = {x, y, z};
  for (int row = 0; row < 3; ++row) {
    r.at(row, 0) = axes[row].x;
    r.at(row, 1) = axes[row].y;
    r.at(row, 2) = axes[row].z;
    r.at(row, 3) = -dot(axes[row], eye);
  }
  r.at(3, 3) = 1.f;
  return r;
}

// D3DXMatrixPerspectiveFovRH / LH, depth 0..1.
Mat perspective(float fov_y, float aspect, float near_z, float far_z, bool left_handed) {
  const float y = 1.f / std::tan(fov_y / 2.f);
  Mat r;
  r.at(0, 0) = y / aspect;
  r.at(1, 1) = y;
  if (left_handed) {
    r.at(2, 2) = far_z / (far_z - near_z);
    r.at(2, 3) = -near_z * far_z / (far_z - near_z);
    r.at(3, 2) = 1.f;
  } else {
    r.at(2, 2) = far_z / (near_z - far_z);
    r.at(2, 3) = near_z * far_z / (near_z - far_z);
    r.at(3, 2) = -1.f;
  }
  return r;
}

// Where world point p lands, as "ndc x y z" (or "behind camera").
std::string project(const Mat& vp, Vec p) {
  float c[4];
  for (int i = 0; i < 4; ++i) c[i] = vp.at(i, 0) * p.x + vp.at(i, 1) * p.y + vp.at(i, 2) * p.z + vp.at(i, 3);
  char text[96];
  if (c[3] <= 0.f) {
    snprintf(text, sizeof(text), "behind camera (w %.1f)", c[3]);
  } else {
    snprintf(text, sizeof(text), "ndc %.3f %.3f depth %.4f", c[0] / c[3], c[1] / c[3], c[2] / c[3]);
  }
  return text;
}

// ---- meshes --------------------------------------------------------------------------------

struct Range {
  UINT start_index = 0, index_count = 0;
  INT base_vertex = 0;
};

// Filled once by start() before the hooks go in; read-only afterwards.
std::vector<MgskBoardVertex> g_vertices;
std::vector<uint16_t> g_indices;
Range g_cube;
Range g_parts[skate::kBoardBodies];
bool g_board_loaded = false;

Range append(const std::vector<MgskBoardVertex>& v, const std::vector<uint16_t>& i) {
  Range r{static_cast<UINT>(g_indices.size()), static_cast<UINT>(i.size()), static_cast<INT>(g_vertices.size())};
  g_vertices.insert(g_vertices.end(), v.begin(), v.end());
  g_indices.insert(g_indices.end(), i.begin(), i.end());
  return r;
}

// Unit cube centred on the origin, flat normals, for the debug marker.
void add_cube() {
  std::vector<MgskBoardVertex> v;
  std::vector<uint16_t> idx;
  for (int axis = 0; axis < 3; ++axis) {
    for (const float sign : {-1.f, 1.f}) {
      float n[3] = {};
      n[axis] = sign;
      const int u = (axis + 1) % 3, w = (axis + 2) % 3;
      const auto base = static_cast<uint16_t>(v.size());
      constexpr std::pair<float, float> kCorners[] = {{-.5f, -.5f}, {.5f, -.5f}, {.5f, .5f}, {-.5f, .5f}};
      for (const auto& [a, b] : kCorners) {
        MgskBoardVertex vert{};
        vert.position[axis] = .5f * sign;
        vert.position[u] = a;
        vert.position[w] = b;
        memcpy(vert.normal, n, sizeof(n));
        v.push_back(vert);
      }
      for (const int k : {0, 1, 2, 0, 2, 3}) idx.push_back(static_cast<uint16_t>(base + k));
    }
  }
  g_cube = append(v, idx);
}

void load_board(HMODULE ffi, const std::string& assets) {
  const auto board_mesh = reinterpret_cast<decltype(&mgsk_board_mesh)>(GetProcAddress(ffi, "mgsk_board_mesh"));
  const auto last_error = reinterpret_cast<decltype(&mgsk_last_error)>(GetProcAddress(ffi, "mgsk_last_error"));
  if (!board_mesh) {
    log("render: skate_ffi.dll has no mgsk_board_mesh; no board to draw");
    return;
  }
  if (assets.empty()) {
    log("render: [skate] skate_data not set; no board to draw");
    return;
  }
  uint32_t total = 0;
  for (uint32_t body = 0; body < skate::kBoardBodies; ++body) {
    uint32_t nv = 0, ni = 0;
    std::vector<MgskBoardVertex> v;
    std::vector<uint16_t> idx;
    if (board_mesh(assets.c_str(), body, nullptr, 0, nullptr, 0, &nv, &ni) >= 0) {
      v.resize(nv);
      idx.resize(ni);
    }
    if (nv == 0 || ni == 0 || board_mesh(assets.c_str(), body, v.data(), nv, idx.data(), ni, &nv, &ni) != 1) {
      log("render: board part %u failed: %s", body, last_error ? last_error() : "?");
      return;
    }
    g_parts[body] = append(v, idx);
    total += ni / 3;
  }
  g_board_loaded = true;
  log("render: board meshes loaded, %u triangles", total);
}

// ---- shaders ---------------------------------------------------------------------------------

constexpr char kShader[] = R"(
cbuffer Draw : register(b0) {
  float4x4 mvp;
  float4x4 world;
  float4 color;
};
struct VsIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; };
VsOut vs_main(VsIn v) {
  VsOut o;
  o.pos = mul(mvp, float4(v.pos, 1));
  o.normal = mul((float3x3)world, v.normal);
  return o;
}
float4 ps_main(VsOut v) : SV_Target {
  float3 l = normalize(float3(0.3, 1.0, 0.2));
  float shade = 0.45 + 0.55 * saturate(dot(normalize(v.normal), l));
  return float4(color.rgb * shade, 1);
}
)";

ComPtr<ID3DBlob> g_vs_blob, g_ps_blob;

bool compile_shaders() {
  const HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  const auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
  if (!compile) {
    log("render: d3dcompiler_47.dll / D3DCompile not available");
    return false;
  }
  ComPtr<ID3DBlob> errors;
  const std::tuple<const char*, const char*, ComPtr<ID3DBlob>*> stages[] = {{"vs_main", "vs_5_0", &g_vs_blob},
                                                                            {"ps_main", "ps_5_0", &g_ps_blob}};
  for (const auto& [entry, target, out] : stages) {
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "mgsk_board", nullptr, nullptr, entry, target, 0, 0,
                       out->ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf()))) {
      log("render: shader %s failed: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
      return false;
    }
  }
  return true;
}

// ---- GPU resources (render thread only) ------------------------------------------------------

struct DrawConstants {
  float mvp[16];
  float world[16];
  float color[4];
};

struct Gpu {
  ID3D11Device* device = nullptr;  // identity only; the swap chain owns it
  bool ok = false;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11InputLayout> layout;
  ComPtr<ID3D11Buffer> vb, ib, cb;
  ComPtr<ID3D11RasterizerState> rs;
  ComPtr<ID3D11DepthStencilState> depth_own, depth_game;
  ComPtr<ID3D11Texture2D> depth;
  ComPtr<ID3D11DepthStencilView> dsv;
  UINT depth_w = 0, depth_h = 0, depth_samples = 0, depth_quality = 0;
};
Gpu g_gpu;

bool create_gpu(ID3D11Device* d) {
  Gpu& g = g_gpu;
  if (FAILED(d->CreateVertexShader(g_vs_blob->GetBufferPointer(), g_vs_blob->GetBufferSize(), nullptr, &g.vs)) ||
      FAILED(d->CreatePixelShader(g_ps_blob->GetBufferPointer(), g_ps_blob->GetBufferSize(), nullptr, &g.ps))) {
    return false;
  }
  const D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  if (FAILED(d->CreateInputLayout(elements, 3, g_vs_blob->GetBufferPointer(), g_vs_blob->GetBufferSize(), &g.layout))) {
    return false;
  }
  D3D11_BUFFER_DESC bd{};
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.ByteWidth = static_cast<UINT>(g_vertices.size() * sizeof(MgskBoardVertex));
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init{g_vertices.data(), 0, 0};
  if (FAILED(d->CreateBuffer(&bd, &init, &g.vb))) return false;
  bd.ByteWidth = static_cast<UINT>(((g_indices.size() * sizeof(uint16_t)) + 3) & ~size_t{3});
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  std::vector<uint16_t> padded(bd.ByteWidth / sizeof(uint16_t), 0);
  memcpy(padded.data(), g_indices.data(), g_indices.size() * sizeof(uint16_t));
  init.pSysMem = padded.data();
  if (FAILED(d->CreateBuffer(&bd, &init, &g.ib))) return false;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.ByteWidth = sizeof(DrawConstants);
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(d->CreateBuffer(&bd, nullptr, &g.cb))) return false;

  // Both windings drawn: the mirror flag flips them, and depth sorts the parts out.
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  rd.MultisampleEnable = TRUE;
  if (FAILED(d->CreateRasterizerState(&rd, &g.rs))) return false;
  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_LESS;
  if (FAILED(d->CreateDepthStencilState(&dd, &g.depth_own))) return false;
  // The game's buffer is bound read-only (it may also be bound as a shader resource).
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  return SUCCEEDED(d->CreateDepthStencilState(&dd, &g.depth_game));
}

bool ensure_gpu(ID3D11Device* d) {
  if (g_gpu.device == d) return g_gpu.ok;
  g_gpu = Gpu{};
  g_gpu.device = d;
  g_gpu.ok = create_gpu(d);
  log("render: device %p resources %s", static_cast<void*>(d), g_gpu.ok ? "ready" : "FAILED");
  return g_gpu.ok;
}

// Our own depth buffer, matching the back buffer, for the board's parts to hide each other.
ID3D11DepthStencilView* own_depth(ID3D11Device* d, const D3D11_TEXTURE2D_DESC& bb) {
  Gpu& g = g_gpu;
  if (!g.dsv || g.depth_w != bb.Width || g.depth_h != bb.Height || g.depth_samples != bb.SampleDesc.Count ||
      g.depth_quality != bb.SampleDesc.Quality) {
    g.dsv.Reset();
    g.depth.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = bb.Width;
    td.Height = bb.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc = bb.SampleDesc;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    vd.Format = td.Format;
    vd.ViewDimension = bb.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(d->CreateTexture2D(&td, nullptr, &g.depth)) || FAILED(d->CreateDepthStencilView(g.depth.Get(), &vd, &g.dsv))) {
      g.dsv.Reset();
      g.depth.Reset();
      return nullptr;
    }
    g.depth_w = bb.Width;
    g.depth_h = bb.Height;
    g.depth_samples = bb.SampleDesc.Count;
    g.depth_quality = bb.SampleDesc.Quality;
    log("render: own depth buffer %ux%u x%u", bb.Width, bb.Height, bb.SampleDesc.Count);
  }
  return g.dsv.Get();
}

// ---- state save/restore --------------------------------------------------------------------

struct SavedState {
  ComPtr<ID3D11InputLayout> layout;
  D3D11_PRIMITIVE_TOPOLOGY topology{};
  ComPtr<ID3D11Buffer> vb;
  UINT vb_stride = 0, vb_offset = 0;
  ComPtr<ID3D11Buffer> ib;
  DXGI_FORMAT ib_format{};
  UINT ib_offset = 0;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11GeometryShader> gs;
  ComPtr<ID3D11HullShader> hs;
  ComPtr<ID3D11DomainShader> ds;
  ComPtr<ID3D11Buffer> vs_cb, ps_cb;
  ComPtr<ID3D11RasterizerState> rs;
  UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
  ComPtr<ID3D11BlendState> blend;
  float blend_factor[4]{};
  UINT sample_mask = 0;
  ComPtr<ID3D11DepthStencilState> depth;
  UINT stencil_ref = 0;
  ComPtr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
  ComPtr<ID3D11DepthStencilView> dsv;
};

void save(ID3D11DeviceContext* c, SavedState& s) {
  c->IAGetInputLayout(&s.layout);
  c->IAGetPrimitiveTopology(&s.topology);
  c->IAGetVertexBuffers(0, 1, &s.vb, &s.vb_stride, &s.vb_offset);
  c->IAGetIndexBuffer(&s.ib, &s.ib_format, &s.ib_offset);
  c->VSGetShader(&s.vs, nullptr, nullptr);
  c->PSGetShader(&s.ps, nullptr, nullptr);
  c->GSGetShader(&s.gs, nullptr, nullptr);
  c->HSGetShader(&s.hs, nullptr, nullptr);
  c->DSGetShader(&s.ds, nullptr, nullptr);
  c->VSGetConstantBuffers(0, 1, &s.vs_cb);
  c->PSGetConstantBuffers(0, 1, &s.ps_cb);
  c->RSGetState(&s.rs);
  c->RSGetViewports(&s.viewport_count, s.viewports);
  c->OMGetBlendState(&s.blend, s.blend_factor, &s.sample_mask);
  c->OMGetDepthStencilState(&s.depth, &s.stencil_ref);
  ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &s.dsv);
  for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) s.rtvs[i].Attach(rtvs[i]);
}

void restore(ID3D11DeviceContext* c, SavedState& s) {
  ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
  for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) rtvs[i] = s.rtvs[i].Get();
  c->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, s.dsv.Get(),
                                               0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
  c->OMSetDepthStencilState(s.depth.Get(), s.stencil_ref);
  c->OMSetBlendState(s.blend.Get(), s.blend_factor, s.sample_mask);
  if (s.viewport_count) c->RSSetViewports(s.viewport_count, s.viewports);
  c->RSSetState(s.rs.Get());
  ID3D11Buffer* vs_cb = s.vs_cb.Get();
  ID3D11Buffer* ps_cb = s.ps_cb.Get();
  c->VSSetConstantBuffers(0, 1, &vs_cb);
  c->PSSetConstantBuffers(0, 1, &ps_cb);
  c->DSSetShader(s.ds.Get(), nullptr, 0);
  c->HSSetShader(s.hs.Get(), nullptr, 0);
  c->GSSetShader(s.gs.Get(), nullptr, 0);
  c->PSSetShader(s.ps.Get(), nullptr, 0);
  c->VSSetShader(s.vs.Get(), nullptr, 0);
  c->IASetIndexBuffer(s.ib.Get(), s.ib_format, s.ib_offset);
  ID3D11Buffer* vb = s.vb.Get();
  c->IASetVertexBuffers(0, 1, &vb, &s.vb_stride, &s.vb_offset);
  c->IASetPrimitiveTopology(s.topology);
  c->IASetInputLayout(s.layout.Get());
}

// ---- drawing ---------------------------------------------------------------------------------

// Flat colours (0..255): deck, truck, wheel.
constexpr float kDeck[3] = {60, 62, 70}, kTruck[3] = {139, 140, 146}, kWheel[3] = {177, 174, 169};
const float* part_color(int body) {
  if (body == MGSK_BODY_DECK) return kDeck;
  if (body == MGSK_BODY_FRONT_TRUCK || body == MGSK_BODY_BACK_TRUCK) return kTruck;
  return kWheel;
}

void draw(ID3D11DeviceContext* c, const Mat& vp, const Mat& world, const float rgb255[3], const Range& r) {
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(c->Map(g_gpu.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
  DrawConstants k;
  memcpy(k.mvp, mul(vp, world).m, sizeof(k.mvp));
  memcpy(k.world, world.m, sizeof(k.world));
  for (int i = 0; i < 3; ++i) k.color[i] = rgb255[i] / 255.f;
  k.color[3] = 1.f;
  memcpy(mapped.pData, &k, sizeof(k));
  c->Unmap(g_gpu.cb.Get(), 0);
  c->DrawIndexed(r.index_count, r.start_index, r.base_vertex);
}

std::atomic<bool> g_hooked{false};
std::atomic<uint64_t> g_presents{0}, g_drawn{0};
std::atomic<uint32_t> g_width{0}, g_height{0};
bool g_was_drawing = false;  // render thread only
int g_game_depth_seen = -1;  // last logged game-depth outcome

// Picks the depth buffer: the game's (read-only view) when asked and one is bound at the back
// buffer's size, else ours (cleared). Logs whenever the outcome changes.
ID3D11DepthStencilView* pick_depth(ID3D11Device* d, ID3D11DeviceContext* c, const SavedState& saved,
                                   const D3D11_TEXTURE2D_DESC& bb, bool want_game, ComPtr<ID3D11DepthStencilView>& game_view,
                                   bool& is_game) {
  is_game = false;
  if (want_game) {
    int outcome = 0;  // 0 none bound, 1 size/format mismatch, 2 used
    D3D11_TEXTURE2D_DESC td{};
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    if (saved.dsv) {
      ComPtr<ID3D11Resource> res;
      ComPtr<ID3D11Texture2D> tex;
      saved.dsv->GetResource(&res);
      saved.dsv->GetDesc(&vd);
      if (res && SUCCEEDED(res.As(&tex))) tex->GetDesc(&td);
      outcome = 1;
      if (tex && td.Width == bb.Width && td.Height == bb.Height && td.SampleDesc.Count == bb.SampleDesc.Count) {
        vd.Flags |= D3D11_DSV_READ_ONLY_DEPTH;
        if (SUCCEEDED(d->CreateDepthStencilView(tex.Get(), &vd, &game_view))) outcome = 2;
      }
    }
    if (outcome != g_game_depth_seen) {
      g_game_depth_seen = outcome;
      if (outcome == 0) {
        log("render: depth_game: no depth buffer bound at Present; using our own");
      } else {
        log("render: depth_game: bound depth %ux%u format %d x%u (back buffer %ux%u x%u): %s", td.Width, td.Height,
            static_cast<int>(td.Format), td.SampleDesc.Count, bb.Width, bb.Height, bb.SampleDesc.Count,
            outcome == 2 ? "using it" : "unusable, using our own");
      }
    }
    if (outcome == 2) {
      is_game = true;
      return game_view.Get();
    }
  } else {
    g_game_depth_seen = -1;
  }
  ID3D11DepthStencilView* own = own_depth(d, bb);
  if (own) c->ClearDepthStencilView(own, D3D11_CLEAR_DEPTH, 1.f, 0);
  return own;
}

void frame(IDXGISwapChain* swap) {
  g_presents.fetch_add(1, std::memory_order_relaxed);
  const Settings s = settings();
  float pose[skate::kBoardBodies * 16];
  const bool have_board = g_board_loaded && skate::board_pose(pose);
  game::Vec3 snake{};
  bool have_snake = false;
  if (s.debug) {
    if (const game::Vec3* p = game::snake_position()) {
      snake = *p;
      have_snake = true;
    }
  }
  game::Vec3 eye{}, target{};
  if ((!have_board && !have_snake) || !game::camera(eye, target)) {
    g_was_drawing = false;
    return;
  }

  ComPtr<ID3D11Device> device;
  if (FAILED(swap->GetDevice(IID_PPV_ARGS(&device))) || !ensure_gpu(device.Get())) return;
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
  D3D11_TEXTURE2D_DESC bb{};
  back->GetDesc(&bb);
  ComPtr<ID3D11RenderTargetView> rtv;
  if (bb.Width == 0 || bb.Height == 0 || FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &rtv))) return;
  ComPtr<ID3D11DeviceContext> c;
  device->GetImmediateContext(&c);

  SavedState saved;
  save(c.Get(), saved);
  ComPtr<ID3D11DepthStencilView> game_view;
  bool game_depth = false;
  ID3D11DepthStencilView* dsv = pick_depth(device.Get(), c.Get(), saved, bb, s.depth_game, game_view, game_depth);

  const float aspect = s.aspect > 0.f ? s.aspect : static_cast<float>(bb.Width) / static_cast<float>(bb.Height);
  const Vec eye_v{eye.x, eye.y, eye.z}, target_v{target.x, target.y, target.z};
  const Mat vp = mul(perspective(s.fov_deg * std::numbers::pi_v<float> / 180.f, aspect, s.near_mm, s.far_mm, s.flip_z),
                     look_at(eye_v, target_v, s.flip_z));

  ID3D11RenderTargetView* target_rtv = rtv.Get();
  c->OMSetRenderTargetsAndUnorderedAccessViews(1, &target_rtv, dsv, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
  c->OMSetDepthStencilState(game_depth ? g_gpu.depth_game.Get() : g_gpu.depth_own.Get(), 0);
  c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  const D3D11_VIEWPORT viewport{0.f, 0.f, static_cast<float>(bb.Width), static_cast<float>(bb.Height), 0.f, 1.f};
  c->RSSetViewports(1, &viewport);
  c->RSSetState(g_gpu.rs.Get());
  c->IASetInputLayout(g_gpu.layout.Get());
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11Buffer* vb = g_gpu.vb.Get();
  const UINT stride = sizeof(MgskBoardVertex), offset = 0;
  c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  c->IASetIndexBuffer(g_gpu.ib.Get(), DXGI_FORMAT_R16_UINT, 0);
  c->VSSetShader(g_gpu.vs.Get(), nullptr, 0);
  c->PSSetShader(g_gpu.ps.Get(), nullptr, 0);
  c->GSSetShader(nullptr, nullptr, 0);
  c->HSSetShader(nullptr, nullptr, 0);
  c->DSSetShader(nullptr, nullptr, 0);
  ID3D11Buffer* cb = g_gpu.cb.Get();
  c->VSSetConstantBuffers(0, 1, &cb);
  c->PSSetConstantBuffers(0, 1, &cb);

  // Engine metres -> MGS3 millimetres, same axes.
  const Mat to_mm = place({0.f, 0.f, 0.f}, {kMmPerUnit, kMmPerUnit, kMmPerUnit});
  Vec deck{};
  if (have_board) {
    for (int body = 0; body < skate::kBoardBodies; ++body) {
      Mat local;
      memcpy(local.m, pose + body * 16, sizeof(local.m));
      const Mat world = mul(to_mm, local);
      if (body == MGSK_BODY_DECK) deck = {world.at(0, 3), world.at(1, 3), world.at(2, 3)};
      draw(c.Get(), vp, world, part_color(body), g_parts[body]);
    }
  }
  const Vec at_snake{snake.x, snake.y, snake.z};
  if (have_snake) {
    // 15 cm yellow cube on Snake's position, 1 m bars along +X (red), +Y (green), +Z (blue).
    constexpr float kYellow[3] = {255, 220, 40}, kRed[3] = {230, 40, 40}, kGreen[3] = {40, 200, 60}, kBlue[3] = {50, 90, 255};
    draw(c.Get(), vp, place(at_snake, {150.f, 150.f, 150.f}), kYellow, g_cube);
    draw(c.Get(), vp, place({snake.x + 500.f, snake.y, snake.z}, {1000.f, 40.f, 40.f}), kRed, g_cube);
    draw(c.Get(), vp, place({snake.x, snake.y + 500.f, snake.z}, {40.f, 1000.f, 40.f}), kGreen, g_cube);
    draw(c.Get(), vp, place({snake.x, snake.y, snake.z + 500.f}, {40.f, 40.f, 1000.f}), kBlue, g_cube);
  }
  restore(c.Get(), saved);

  const uint64_t drawn = g_drawn.fetch_add(1, std::memory_order_relaxed) + 1;
  g_width = bb.Width;
  g_height = bb.Height;
  if (!g_was_drawing || drawn % 300 == 0) {
    log("render: frame %llu %ux%u fmt %d aspect %.3f; eye %.0f %.0f %.0f look-at %.0f %.0f %.0f (mm); %s",
        static_cast<unsigned long long>(drawn), bb.Width, bb.Height, static_cast<int>(bb.Format), aspect, eye.x, eye.y,
        eye.z, target.x, target.y, target.z, describe(s).c_str());
    if (have_board) log("render:   deck at %.0f %.0f %.0f mm -> %s", deck.x, deck.y, deck.z, project(vp, deck).c_str());
    if (have_snake) {
      log("render:   snake at %.0f %.0f %.0f mm -> %s", snake.x, snake.y, snake.z, project(vp, at_snake).c_str());
    }
  }
  g_was_drawing = true;
}

// ---- hooks -----------------------------------------------------------------------------------

SafetyHookInline g_present_hook, g_present1_hook;
// Set while inside either hook, so a Present that calls Present1 (or an overlay that chains
// them) draws only once.
thread_local bool t_in_present = false;

void before_present(IDXGISwapChain* swap, UINT flags) {
  if (!(flags & DXGI_PRESENT_TEST)) frame(swap);
}

HRESULT STDMETHODCALLTYPE on_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
  const bool outer = !t_in_present;
  if (outer) {
    t_in_present = true;
    before_present(swap, flags);
  }
  const HRESULT hr = g_present_hook.stdcall<HRESULT>(swap, sync, flags);
  if (outer) t_in_present = false;
  return hr;
}

HRESULT STDMETHODCALLTYPE on_present1(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
  const bool outer = !t_in_present;
  if (outer) {
    t_in_present = true;
    before_present(swap, flags);
  }
  const HRESULT hr = g_present1_hook.stdcall<HRESULT>(swap, sync, flags, params);
  if (outer) t_in_present = false;
  return hr;
}

// Present's address from a throwaway device + swap chain on a hidden window. The swap chain
// comes from CreateSwapChainForHwnd, which MGSHDFix doesn't hook (see top of file).
bool install_hooks() {
  const HWND window = CreateWindowExW(0, L"STATIC", L"MetalGearSkate dummy", WS_OVERLAPPED, 0, 0, 16, 16, nullptr,
                                      nullptr, nullptr, nullptr);
  if (!window) {
    log("render: dummy window failed (%lu)", GetLastError());
    return false;
  }
  bool ok = false;
  {
    ComPtr<ID3D11Device> device;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                   nullptr, nullptr);
    if (FAILED(hr)) {
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                             nullptr);
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> swap;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 16;
    desc.Height = 16;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (SUCCEEDED(hr) && SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(hr = factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &swap))) {
      void** vtable = *reinterpret_cast<void***>(swap.Get());
      constexpr int kPresent = 8, kPresent1 = 22;
      g_present_hook = safetyhook::create_inline(vtable[kPresent], reinterpret_cast<void*>(&on_present));
      g_present1_hook = safetyhook::create_inline(vtable[kPresent1], reinterpret_cast<void*>(&on_present1));
      const auto* dxgi_base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(L"dxgi.dll"));
      log("render: Present hook at dxgi+%llX %s, Present1 at dxgi+%llX %s",
          static_cast<unsigned long long>(static_cast<const uint8_t*>(vtable[kPresent]) - dxgi_base),
          g_present_hook ? "installed" : "FAILED",
          static_cast<unsigned long long>(static_cast<const uint8_t*>(vtable[kPresent1]) - dxgi_base),
          g_present1_hook ? "installed" : "FAILED");
      ok = static_cast<bool>(g_present_hook);
    } else {
      log("render: dummy swap chain failed (0x%08lX)", static_cast<unsigned long>(hr));
    }
  }
  DestroyWindow(window);
  return ok;
}

}  // namespace

void start(const std::filesystem::path& plugin_dir, HMODULE ffi) {
  const auto ini = plugin_dir / L"MetalGearSkate.ini";
  load_settings(ini);
  log("render: settings %s", describe(settings()).c_str());
  char assets[MAX_PATH] = {};
  GetPrivateProfileStringA("skate", "skate_data", "", assets, MAX_PATH, ini.string().c_str());
  add_cube();
  load_board(ffi, assets);
  if (!compile_shaders()) return;
  g_hooked = install_hooks();
}

std::string set(const std::string& key, const std::string& value) {
  Settings s;
  {
    std::lock_guard lock(g_settings_mutex);
    if (!key.empty()) {
      if (const std::string error = apply(g_settings, key, value); !error.empty()) return "error: " + error;
    }
    s = g_settings;
  }
  if (!key.empty()) log("render: set %s %s -> %s", key.c_str(), value.c_str(), describe(s).c_str());
  return "ok " + describe(s);
}

Status status() {
  Status s;
  s.hooked = g_hooked;
  s.presents = g_presents;
  s.drawn = g_drawn;
  s.width = g_width;
  s.height = g_height;
  return s;
}

}  // namespace render
