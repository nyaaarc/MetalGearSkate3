#include "render.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
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

#include "camera.hpp"
#include "game.hpp"
#include "log.hpp"
#include "pose.hpp"
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
  bool depth_game = true;    // hide behind the scene's depth buffer (copied at Present), else draw over it
  float depth_bias = 0.002f; // relative: the scene must be this much nearer to hide the board
  bool depth_debug = false;  // log the scene's depth under the deck and the deck's own, once a second
  bool textures = true;      // the board's Skate 3 textures (when the DLL has them), else flat colours
  bool debug = false;        // marker + axes at Snake's position, skating or not
  // skate: while skating, the whole frame is drawn from the Skate 3 engine's chase camera (the
  //        game's camera constants are rewritten), otherwise as `game`;
  // game:   the game's own view-projection (captured), untouched;
  // object: the old guess, camera object + fov_deg/near_mm/far_mm/aspect/flip_z;
  // drive:  (default) the game's own camera, moved onto the skate camera (camera.cpp), so the
  //         game draws and culls the scene from there itself; drawn like `game`. Falls back to
  //         `skate` if the camera hook isn't installed.
  enum class Camera { Skate, Game, Object, Drive } camera = Camera::Drive;
  float skate_fov_deg = 0.f;  // vertical fov for the skate camera; 0 = the engine's
};

std::mutex g_settings_mutex;
Settings g_settings;

Settings settings() {
  std::lock_guard lock(g_settings_mutex);
  return g_settings;
}

std::string describe(const Settings& s) {
  char text[352];
  snprintf(text, sizeof(text),
           "camera=%s skate_fov_deg=%.1f fov_deg=%.2f near_mm=%.1f far_mm=%.0f aspect=%.4f flip_z=%d depth_game=%d "
           "depth_bias=%.4f depth_debug=%d textures=%d debug=%d",
           s.camera == Settings::Camera::Skate  ? "skate"
           : s.camera == Settings::Camera::Game ? "game"
           : s.camera == Settings::Camera::Drive ? "drive"
                                                 : "object",
           s.skate_fov_deg, s.fov_deg, s.near_mm, s.far_mm, s.aspect, s.flip_z ? 1 : 0,
           s.depth_game ? 1 : 0, s.depth_bias, s.depth_debug ? 1 : 0, s.textures ? 1 : 0, s.debug ? 1 : 0);
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
  else if (key == "depth_bias") ok = parse_float(value, n.depth_bias) && n.depth_bias > -0.5f && n.depth_bias < 0.5f;
  else if (key == "depth_debug") ok = parse_bool(value, n.depth_debug);
  else if (key == "textures") ok = parse_bool(value, n.textures);
  else if (key == "debug") ok = parse_bool(value, n.debug);
  else if (key == "camera") {
    ok = true;
    if (value == "skate") n.camera = Settings::Camera::Skate;
    else if (value == "game") n.camera = Settings::Camera::Game;
    else if (value == "object") n.camera = Settings::Camera::Object;
    else if (value == "drive") n.camera = Settings::Camera::Drive;
    else ok = false;
    if (ok) camera::set_driving(n.camera == Settings::Camera::Drive);
  }
  else if (key == "drive_zoom") {
    float zoom = 0.f;
    ok = parse_float(value, zoom) && zoom >= 0.f && zoom < 20.f;
    if (ok) camera::set_zoom(zoom);
  }
  else if (key == "skate_fov_deg") ok = parse_float(value, n.skate_fov_deg) && n.skate_fov_deg >= 0.f && n.skate_fov_deg < 170.f;
  else return "unknown key (camera skate_fov_deg fov_deg near_mm far_mm aspect flip_z depth_game depth_bias depth_debug textures debug)";
  if (!ok) return "bad value for " + key;
  s = n;
  return "";
}

void load_settings(const std::filesystem::path& ini) {
  Settings s;
  for (const char* key : {"camera", "drive_zoom", "skate_fov_deg", "fov_deg", "near_mm", "far_mm", "aspect", "flip_z", "depth_game",
                          "depth_bias", "depth_debug", "textures", "debug"}) {
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

// A row-major float4x4 as HLSL stores it (`row_major`, rows contiguous, used as mul(M, v)).
Mat from_rows(const float* f) {
  Mat r;
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) r.at(row, col) = f[row * 4 + col];
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
// The board's textures (mgsk_board_texture, RGBA8, with a mip chain built here) and, per part,
// the runs of its indices each texture draws (mgsk_board_ranges). Empty with an older DLL or if
// loading fails: then the parts are drawn in flat colours.
struct BoardTexture {
  uint32_t width = 0, height = 0;
  std::vector<std::vector<uint8_t>> mips;  // level 0 first, RGBA8 rows from the top
};
std::vector<BoardTexture> g_board_textures;
struct TexturedRange {
  Range range;
  uint32_t texture = 0;
};
std::vector<TexturedRange> g_part_runs[skate::kBoardBodies];

// Box-filtered mip chain down to 1x1 (keeps the deck's grip and graphic from shimmering far off).
std::vector<std::vector<uint8_t>> build_mips(uint32_t w, uint32_t h, std::vector<uint8_t> base) {
  std::vector<std::vector<uint8_t>> mips;
  mips.push_back(std::move(base));
  while (w > 1 || h > 1) {
    const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    const std::vector<uint8_t>& src = mips.back();
    std::vector<uint8_t> dst(static_cast<size_t>(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y) {
      for (uint32_t x = 0; x < nw; ++x) {
        const uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
        const uint32_t y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
        for (uint32_t c = 0; c < 4; ++c) {
          const auto at = [&](uint32_t px, uint32_t py) { return src[(static_cast<size_t>(py) * w + px) * 4 + c]; };
          dst[(static_cast<size_t>(y) * nw + x) * 4 + c] =
              static_cast<uint8_t>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
        }
      }
    }
    mips.push_back(std::move(dst));
    w = nw;
    h = nh;
  }
  return mips;
}

// Texture runs and textures for the loaded parts; on any failure, none (flat colours).
void load_board_textures(HMODULE ffi, const std::string& assets, const char* (*last_error)()) {
  const auto ranges_fn = reinterpret_cast<decltype(&mgsk_board_ranges)>(GetProcAddress(ffi, "mgsk_board_ranges"));
  const auto texture_fn = reinterpret_cast<decltype(&mgsk_board_texture)>(GetProcAddress(ffi, "mgsk_board_texture"));
  if (!ranges_fn || !texture_fn) {
    log("render: skate_ffi.dll has no board textures (mgsk_board_ranges/mgsk_board_texture); flat colours");
    return;
  }
  auto give_up = [&](const char* what) {
    log("render: board textures: %s failed (%s); flat colours", what, last_error ? last_error() : "?");
    for (auto& runs : g_part_runs) runs.clear();
    g_board_textures.clear();
  };
  uint32_t textures = 0;
  for (uint32_t body = 0; body < skate::kBoardBodies; ++body) {
    MgskBoardRange ranges[8];
    const int n = ranges_fn(assets.c_str(), body, ranges, 8);
    if (n <= 0 || n > 8) return give_up("mgsk_board_ranges");
    const Range& part = g_parts[body];
    for (int i = 0; i < n; ++i) {
      if (ranges[i].first_index + ranges[i].index_count > part.index_count) return give_up("a texture run");
      g_part_runs[body].push_back(
          {{part.start_index + ranges[i].first_index, ranges[i].index_count, part.base_vertex}, ranges[i].texture});
      textures = std::max(textures, ranges[i].texture + 1);
    }
  }
  for (uint32_t t = 0; t < textures; ++t) {
    uint32_t w = 0, h = 0;
    if (texture_fn(assets.c_str(), t, nullptr, 0, &w, &h) != 0 || !w || !h || w > 4096 || h > 4096) {
      return give_up("mgsk_board_texture size");
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    if (texture_fn(assets.c_str(), t, rgba.data(), static_cast<uint32_t>(rgba.size()), &w, &h) != 1) {
      return give_up("mgsk_board_texture");
    }
    // Opaque materials: the converter's alpha channel isn't coverage.
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    g_board_textures.push_back({w, h, build_mips(w, h, std::move(rgba))});
  }
  log("render: board textures loaded: %u", textures);
}

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
  load_board_textures(ffi, assets, last_error);
}

// ---- shaders ---------------------------------------------------------------------------------

constexpr char kShader[] = R"(
cbuffer Draw : register(b0) {
  float4x4 mvp;
  float4x4 world;
  float4 color;
  // Scene depth test: xy scene pixels per target pixel, z relative bias, w mode (0 off,
  // 1 single-sampled scene depth in t0, 2 multisampled in t1).
  float4 depth_params;
  float4 depth_extra;  // x: scene sample count, yz: probe pixel (scene pixels)
  float4 material;     // x: 1 = colour is board_tex (t2) times color.rgb, 0 = color.rgb alone
};
Texture2D<float> scene_depth : register(t0);
Texture2DMS<float> scene_depth_ms : register(t1);
Texture2D board_tex : register(t2);
SamplerState board_sampler : register(s0);
struct VsIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
VsOut vs_main(VsIn v) {
  VsOut o;
  o.pos = mul(mvp, float4(v.pos, 1));
  o.normal = mul((float3x3)world, v.normal);
  o.uv = v.uv;
  return o;
}
// The scene's depth at a pixel. MGS3's depth is reversed (k / distance, nearer = larger); of a
// multisampled pixel's samples take the farthest, so the board only hides where all are nearer.
float scene_at(int2 p) {
  float d = 1;
  if (depth_params.w > 1.5) {
    uint n = (uint)depth_extra.x;
    for (uint i = 0; i < n && i < 16; ++i) d = min(d, scene_depth_ms.Load(p, i));
  } else {
    d = scene_depth.Load(int3(p, 0));
  }
  return d;
}
float4 ps_main(VsOut v) : SV_Target {
  if (depth_params.w > 0.5 && scene_at(int2(v.pos.xy * depth_params.xy)) > v.pos.z * (1 + depth_params.z)) discard;
  float3 l = normalize(float3(0.3, 1.0, 0.2));
  float shade = 0.45 + 0.55 * saturate(dot(normalize(v.normal), l));
  float3 base = color.rgb;
  if (material.x > 0.5) base *= board_tex.Sample(board_sampler, v.uv).rgb;
  return float4(base * shade, 1);
}
// `render depth_debug`: one triangle over a 1x1 target that reads the scene depth at a pixel.
float4 vs_probe(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? 3 : -1, 0, 1);
}
float4 ps_probe() : SV_Target {
  return float4(scene_at(int2(depth_extra.yz)), 0, 0, 1);
}
)";

ComPtr<ID3DBlob> g_vs_blob, g_ps_blob, g_vs_probe_blob, g_ps_probe_blob;

bool compile_shaders() {
  const HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  const auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
  if (!compile) {
    log("render: d3dcompiler_47.dll / D3DCompile not available");
    return false;
  }
  ComPtr<ID3DBlob> errors;
  const std::tuple<const char*, const char*, ComPtr<ID3DBlob>*> stages[] = {
      {"vs_main", "vs_5_0", &g_vs_blob},
      {"ps_main", "ps_5_0", &g_ps_blob},
      {"vs_probe", "vs_5_0", &g_vs_probe_blob},
      {"ps_probe", "ps_5_0", &g_ps_probe_blob}};
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
  float depth_params[4];  // see the shader's cbuffer
  float depth_extra[4];
  float material[4];      // x: textured
};

struct Gpu {
  ID3D11Device* device = nullptr;  // identity only; the swap chain owns it
  bool ok = false;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11InputLayout> layout;
  ComPtr<ID3D11Buffer> vb, ib, cb;
  ComPtr<ID3D11RasterizerState> rs;
  ComPtr<ID3D11DepthStencilState> depth_own;      // depth grows with distance
  ComPtr<ID3D11DepthStencilState> depth_own_rev;  // depth shrinks with distance (the game's)
  ComPtr<ID3D11Texture2D> depth;
  ComPtr<ID3D11DepthStencilView> dsv;
  UINT depth_w = 0, depth_h = 0, depth_samples = 0, depth_quality = 0;
  // Copy of the scene's depth buffer (its typeless format, same size and samples), which the
  // board's pixel shader reads to hide behind the scene.
  ComPtr<ID3D11Texture2D> scene_copy;
  ComPtr<ID3D11ShaderResourceView> scene_copy_srv;
  D3D11_TEXTURE2D_DESC scene_copy_desc{};
  // `render depth_debug`: reads one scene depth value back.
  ComPtr<ID3D11VertexShader> vs_probe;
  ComPtr<ID3D11PixelShader> ps_probe;
  ComPtr<ID3D11Texture2D> probe, probe_staging;
  ComPtr<ID3D11RenderTargetView> probe_rtv;
  // The board's textures (g_board_textures order); empty = flat colours.
  std::vector<ComPtr<ID3D11ShaderResourceView>> board_srvs;
  ComPtr<ID3D11SamplerState> sampler;
};
Gpu g_gpu;

// Board textures and their sampler. False leaves the board flat-coloured (not an error).
bool create_board_textures(ID3D11Device* d) {
  Gpu& g = g_gpu;
  g.board_srvs.clear();
  if (g_board_textures.empty()) return false;
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(d->CreateSamplerState(&sd, &g.sampler))) return false;
  for (const BoardTexture& t : g_board_textures) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = t.width;
    td.Height = t.height;
    td.MipLevels = static_cast<UINT>(t.mips.size());
    td.ArraySize = 1;
    // The game draws in gamma space (UNORM back buffer), so the sRGB texels go through as they are.
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> init(t.mips.size());
    for (size_t level = 0; level < t.mips.size(); ++level) {
      init[level] = {t.mips[level].data(), std::max(1u, t.width >> level) * 4, 0};
    }
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(d->CreateTexture2D(&td, init.data(), &texture)) ||
        FAILED(d->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
      log("render: board texture %ux%u could not be created; flat colours", t.width, t.height);
      g.board_srvs.clear();
      return false;
    }
    g.board_srvs.push_back(srv);
  }
  return true;
}

bool create_gpu(ID3D11Device* d) {
  Gpu& g = g_gpu;
  if (FAILED(d->CreateVertexShader(g_vs_blob->GetBufferPointer(), g_vs_blob->GetBufferSize(), nullptr, &g.vs)) ||
      FAILED(d->CreatePixelShader(g_ps_blob->GetBufferPointer(), g_ps_blob->GetBufferSize(), nullptr, &g.ps)) ||
      FAILED(d->CreateVertexShader(g_vs_probe_blob->GetBufferPointer(), g_vs_probe_blob->GetBufferSize(), nullptr,
                                   &g.vs_probe)) ||
      FAILED(d->CreatePixelShader(g_ps_probe_blob->GetBufferPointer(), g_ps_probe_blob->GetBufferSize(), nullptr,
                                  &g.ps_probe))) {
    return false;
  }
  D3D11_TEXTURE2D_DESC pd{};
  pd.Width = pd.Height = 1;
  pd.MipLevels = pd.ArraySize = 1;
  pd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  pd.SampleDesc.Count = 1;
  pd.Usage = D3D11_USAGE_DEFAULT;
  pd.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(d->CreateTexture2D(&pd, nullptr, &g.probe)) ||
      FAILED(d->CreateRenderTargetView(g.probe.Get(), nullptr, &g.probe_rtv))) {
    return false;
  }
  pd.Usage = D3D11_USAGE_STAGING;
  pd.BindFlags = 0;
  pd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(d->CreateTexture2D(&pd, nullptr, &g.probe_staging))) return false;
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
  dd.DepthFunc = D3D11_COMPARISON_GREATER;
  if (FAILED(d->CreateDepthStencilState(&dd, &g.depth_own_rev))) return false;
  create_board_textures(d);
  return true;
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
  ComPtr<ID3D11ShaderResourceView> ps_srvs[3];
  ComPtr<ID3D11SamplerState> ps_sampler;
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
  ID3D11ShaderResourceView* srvs[3] = {};
  c->PSGetShaderResources(0, 3, srvs);
  for (int i = 0; i < 3; ++i) s.ps_srvs[i].Attach(srvs[i]);
  c->PSGetSamplers(0, 1, &s.ps_sampler);
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
  ID3D11ShaderResourceView* srvs[3] = {s.ps_srvs[0].Get(), s.ps_srvs[1].Get(), s.ps_srvs[2].Get()};
  c->PSSetShaderResources(0, 3, srvs);
  ID3D11SamplerState* sampler = s.ps_sampler.Get();
  c->PSSetSamplers(0, 1, &sampler);
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

// ---- the game's own camera, from its vertex-shader constants ---------------------------------
//
// Every stock 3D vertex shader (dumped from the running game, disassembled with D3DDisassemble)
// declares the same `$Globals` constant buffer at b0, compiled row_major, and computes
//   view_pos = mul(gVS_Mat0, pos)   (rigid models; skinned ones use the gVS_Mat[] array instead)
//   clip     = mul(gVS_Pers, view_pos)
// gVS_Pers is float4x4 at byte 480, gVS_Mat0 float4x4 at byte 608 (model -> view). So the view
// projection is gVS_Pers x (the view matrix), and the view matrix is gVS_Mat0 of any model whose
// world matrix is identity (the stage's own geometry: most draws of a frame share it).
// The context's Map/Unmap and UpdateSubresource are hooked to read those two matrices from every
// $Globals-sized constant buffer the game fills, tallied per frame; Present takes the result.

constexpr UINT kPersOffset = 480, kMat0Offset = 608, kGlobalsMin = kMat0Offset + 64;

struct CamSample {
  float pers[16];
  float mat0[16];
  uint64_t hash;
  uint32_t count;
};

std::mutex g_cap_mutex;
std::vector<CamSample> g_cap_frame;    // this frame's distinct (pers, mat0) pairs; guarded
std::vector<CamSample> g_cap_last;     // last complete frame; guarded
uint64_t g_cap_writes = 0, g_cap_maps = 0, g_cap_updates = 0, g_cap_partial = 0;  // guarded, per frame
uint64_t g_cap_last_writes = 0, g_cap_last_maps = 0, g_cap_last_updates = 0, g_cap_last_partial = 0;
ID3D11DeviceContext* g_cap_context = nullptr;  // the immediate context we hooked
struct Pending {
  ID3D11Resource* res;
  uint8_t* data;
};
Pending g_pending_maps[8] = {};  // guarded

// The scene's depth buffer, found during the frame (nothing is bound at Present) among the
// depth-stencil views bound while main-camera draws fill $Globals: the one Snake's skinned draws
// use, else the one with the most samples (the main pass is 4x MSAA; a 1x one also shows up on
// some frames), else the most used. A new pick replaces the current one only after holding for
// kDepthSwitchFrames frames running, so the frame-to-frame alternation doesn't flip the test.
// Guarded by g_cap_mutex.
struct DepthCount {
  ComPtr<ID3D11DepthStencilView> view;
  uint32_t count = 0, snake = 0, samples = 0;
};
DepthCount g_depth_frame[4];  // this frame's candidates
ComPtr<ID3D11DepthStencilView> g_scene_depth;  // the current pick
ComPtr<ID3D11DepthStencilView> g_depth_next;   // a different pick, and for how many frames running
int g_depth_next_frames = 0;
constexpr int kDepthSwitchFrames = 30;

SafetyHookInline g_map_hook, g_unmap_hook, g_update_hook;

bool is_globals(ID3D11Resource* res) {
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  res->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
  D3D11_BUFFER_DESC d;
  static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
  return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth >= kGlobalsMin;
}

// Records the matrices at `globals` (a $Globals image). Caller holds g_cap_mutex.
void tally(const uint8_t* globals) {
  CamSample s;
  memcpy(s.pers, globals + kPersOffset, sizeof(s.pers));
  memcpy(s.mat0, globals + kMat0Offset, sizeof(s.mat0));
  s.hash = 1469598103934665603ull;  // FNV-1a over both matrices
  for (const float* m : {s.pers, s.mat0}) {
    const auto* b = reinterpret_cast<const uint8_t*>(m);
    for (size_t i = 0; i < 64; ++i) s.hash = (s.hash ^ b[i]) * 1099511628211ull;
  }
  ++g_cap_writes;
  for (CamSample& e : g_cap_frame) {
    if (e.hash == s.hash && memcmp(e.mat0, s.mat0, sizeof(s.mat0)) == 0 && memcmp(e.pers, s.pers, sizeof(s.pers)) == 0) {
      ++e.count;
      return;
    }
  }
  if (g_cap_frame.size() < 4096) {
    s.count = 1;
    g_cap_frame.push_back(s);
  }
}

// ---- drawing the frame from another camera (`render camera skate`) ---------------------------
//
// Every main-camera draw's gVS_Pers is `S * T(d)`: S = projection x view rotation (rows 0, 1 and 3
// carry no constant; row 2 is (~0, ~0, ~0, k), k ~ 50, a little more for depth-biased layers)
// and d an offset (0 for the stage and rigid models, whose gVS_Mat0 = T(-L) * world with L the
// frame's look-at point; non-zero for skinned models). So the world position of whatever the
// draw transforms is T(L + d) * gVS_Mat0 * v (or * the bones), and the same draw seen from
// another camera VP is gVS_Pers' = VP_k * T(L + d), VP_k being VP with row 2 = (0, 0, 0, k).
// L is read from this frame's first stage draw (gVS_Mat0 a pure translation, d = 0); draws
// before it use last frame's L extrapolated. Shadow, light and screen-space passes don't have
// the shape and are left alone.

bool g_ovr_active = false;  // guarded by g_cap_mutex: rewrite this frame's draws
Mat g_ovr_vp;               // the new view-projection (row 2 replaced per draw)
bool g_ovr_used = false;    // what the frame that just ended was drawn with (the board uses it)
Mat g_ovr_used_vp;
Vec g_L_frame{}, g_L_prev{}, g_L_prev2{};
bool g_L_known = false;
int g_L_history = 0;  // frames in a row with L seen (0, 1, 2+)
uint64_t g_ovr_replaced = 0, g_ovr_early = 0, g_ovr_last_replaced = 0, g_ovr_last_early = 0;

bool stage_translation(const Mat& m);

bool solve3(const Vec rows[3], Vec b, Vec& x) {
  const Vec c0 = cross(rows[1], rows[2]);
  const float det = dot(rows[0], c0);
  if (std::fabs(det) < 1e-9f) return false;
  const Vec c1 = cross(rows[2], rows[0]), c2 = cross(rows[0], rows[1]);
  // inverse(A) has columns c0 c1 c2 over det.
  x = {(c0.x * b.x + c1.x * b.y + c2.x * b.z) / det, (c0.y * b.x + c1.y * b.y + c2.y * b.z) / det,
       (c0.z * b.x + c1.z * b.y + c2.z * b.z) / det};
  return true;
}

Vec row_of(const Mat& m, int r) { return {m.at(r, 0), m.at(r, 1), m.at(r, 2)}; }

// A main-camera gVS_Pers (perspective, k depth row), not a light/shadow/screen one.
bool main_camera_pers(const Mat& p) {
  const Vec r0 = row_of(p, 0), r1 = row_of(p, 1), r2 = row_of(p, 2), r3 = row_of(p, 3);
  const float l0 = std::sqrt(dot(r0, r0)), l1 = std::sqrt(dot(r1, r1)), l3 = std::sqrt(dot(r3, r3));
  return std::sqrt(dot(r2, r2)) < 1e-3f && p.at(2, 3) > 1.f && p.at(2, 3) < 1000.f && std::fabs(l3 - 1.f) < 1e-3f &&
         l0 > 0.1f && l1 > 0.1f && std::fabs(dot(r0, r3)) < 1e-3f * l0 && std::fabs(dot(r1, r3)) < 1e-3f * l1;
}

// A main-camera $Globals image: its gVS_Pers and the offset d (gVS_Pers = S * T(d)).
bool main_camera_draw(const uint8_t* globals, Mat& pers, Vec& d) {
  float f[16];
  memcpy(f, globals + kPersOffset, sizeof(f));
  pers = from_rows(f);
  if (!main_camera_pers(pers)) return false;
  const Vec rows[3] = {row_of(pers, 0), row_of(pers, 1), row_of(pers, 3)};
  return solve3(rows, {pers.at(0, 3), pers.at(1, 3), pers.at(3, 3)}, d);
}

// Counts the depth-stencil view bound for a main-camera draw (`snake`: one of Snake's). Caller
// holds g_cap_mutex.
void note_depth(ID3D11DeviceContext* c, const uint8_t* globals, bool snake) {
  Mat pers;
  Vec d;
  if (!main_camera_draw(globals, pers, d)) return;
  ComPtr<ID3D11DepthStencilView> view;
  c->OMGetRenderTargets(0, nullptr, &view);
  if (!view) return;
  for (DepthCount& e : g_depth_frame) {
    if (e.view == view) {
      ++e.count;
      e.snake += snake ? 1 : 0;
      return;
    }
  }
  for (DepthCount& e : g_depth_frame) {
    if (!e.view) {
      ComPtr<ID3D11Resource> res;
      ComPtr<ID3D11Texture2D> tex;
      view->GetResource(&res);
      D3D11_TEXTURE2D_DESC td{};
      if (res && SUCCEEDED(res.As(&tex))) tex->GetDesc(&td);
      e = {view, 1, snake ? 1u : 0u, td.SampleDesc.Count};
      return;
    }
  }
}

// At the end of a frame: picks its scene depth and applies the switching rule above. Caller
// holds g_cap_mutex.
void pick_scene_depth() {
  DepthCount* best = nullptr;
  auto better = [](const DepthCount& a, const DepthCount& b) {
    if ((a.snake > 0) != (b.snake > 0)) return a.snake > 0;
    if (a.samples != b.samples) return a.samples > b.samples;
    return a.count > b.count;
  };
  for (DepthCount& e : g_depth_frame) {
    if (e.view && (!best || better(e, *best))) best = &e;
  }
  ComPtr<ID3D11DepthStencilView> pick = best ? best->view : nullptr;
  for (DepthCount& e : g_depth_frame) e = {};
  if (!pick || pick == g_scene_depth) {  // no main-camera draws (menus, loads): keep the current
    g_depth_next.Reset();
    g_depth_next_frames = 0;
    return;
  }
  if (!g_scene_depth) {
    g_scene_depth = pick;
    return;
  }
  if (pick != g_depth_next) {
    g_depth_next = pick;
    g_depth_next_frames = 0;
  }
  if (++g_depth_next_frames >= kDepthSwitchFrames) {
    g_scene_depth = g_depth_next;
    g_depth_next.Reset();
    g_depth_next_frames = 0;
  }
}

// Notes the frame's look-at point L from its first stage draw. Caller holds g_cap_mutex.
void note_look_at(const uint8_t* globals) {
  Mat pers;
  Vec d;
  if (g_L_known || !main_camera_draw(globals, pers, d) || dot(d, d) >= 1.f) return;
  float f[16];
  memcpy(f, globals + kMat0Offset, sizeof(f));
  const Mat mat0 = from_rows(f);
  if (stage_translation(mat0)) {
    g_L_frame = {-mat0.at(0, 3), -mat0.at(1, 3), -mat0.at(2, 3)};
    g_L_known = true;
  }
}

// Rewrites gVS_Pers in a $Globals image to draw from g_ovr_vp. Caller holds g_cap_mutex.
void rewrite(uint8_t* globals) {
  Mat pers;
  Vec d;
  if (!main_camera_draw(globals, pers, d)) return;
  float f[16];
  Vec L;
  if (g_L_known) {
    L = g_L_frame;
  } else if (g_L_history >= 2) {
    L = {2.f * g_L_prev.x - g_L_prev2.x, 2.f * g_L_prev.y - g_L_prev2.y, 2.f * g_L_prev.z - g_L_prev2.z};
    ++g_ovr_early;
  } else if (g_L_history == 1) {
    L = g_L_prev;
    ++g_ovr_early;
  } else {
    return;  // no look-at point yet: leave the draw as the game made it
  }
  Mat vp = g_ovr_vp;
  vp.at(2, 0) = vp.at(2, 1) = vp.at(2, 2) = 0.f;
  vp.at(2, 3) = pers.at(2, 3) - dot(row_of(pers, 2), d);
  const Mat out = mul(vp, place({L.x + d.x, L.y + d.y, L.z + d.z}, {1.f, 1.f, 1.f}));
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) f[r * 4 + c] = out.at(r, c);
  }
  memcpy(globals + kPersOffset, f, sizeof(f));
  ++g_ovr_replaced;
}

// View-projection from the engine camera (13 floats: position mm, basis columns, vertical fov),
// in the game's convention: depth = k / distance (row 2 = (0, 0, 0, k)), w = distance along
// forward. The engine's first basis column is up x forward, which is screen left here.
Mat skate_view_projection(const float cam[13], float aspect, float fov_override_deg, float k) {
  const Vec C{cam[0], cam[1], cam[2]};
  const Vec right{-cam[3], -cam[4], -cam[5]}, up{cam[6], cam[7], cam[8]}, fwd{cam[9], cam[10], cam[11]};
  const float fov = (fov_override_deg > 0.f ? fov_override_deg : cam[12]) * std::numbers::pi_v<float> / 180.f;
  const float sy = 1.f / std::tan(fov / 2.f), sx = sy / aspect;
  const Vec rows[3] = {Vec{right.x * sx, right.y * sx, right.z * sx}, Vec{up.x * sy, up.y * sy, up.z * sy}, fwd};
  constexpr int kAtRow[3] = {0, 1, 3};
  Mat m;
  for (int i = 0; i < 3; ++i) {
    m.at(kAtRow[i], 0) = rows[i].x;
    m.at(kAtRow[i], 1) = rows[i].y;
    m.at(kAtRow[i], 2) = rows[i].z;
    m.at(kAtRow[i], 3) = -dot(rows[i], C);
  }
  m.at(2, 3) = k;
  return m;
}

// ---- palette dump (`render palette now`): one frame's skinned main-camera draws -------------
//
// For working out how Snake's bone palette (gVS_Mat[64], row_major float3x4 at byte 624) relates
// to his bone matrices. Writes MetalGearSkate.palette.txt beside the plugin.
constexpr UINT kPaletteOffset = 624, kPaletteSlots = 64;
int g_dump_state = 0;  // guarded by g_cap_mutex: 0 idle, 1 requested, 2 recording this frame
std::vector<std::vector<float>> g_dump;  // per draw: L(3) d(3) pers(16) mat0(16) palette(64*12)
std::filesystem::path g_dump_path;
// Snake's bones as the game left them, taken at the frame's first skinned draw (same frame).
float g_dump_bones[pose::kSnakeBones * 16], g_dump_origin[3], g_dump_object[16];
bool g_dump_relative = false, g_dump_have_bones = false;

void dump_draw(const uint8_t* globals) {
  Mat pers;
  Vec d;
  if (!main_camera_draw(globals, pers, d) || dot(d, d) < 1.f || g_dump.size() >= 1024) return;
  if (g_dump.empty()) {
    g_dump_have_bones = pose::game_bones(g_dump_bones, g_dump_origin, g_dump_object, g_dump_relative);
  }
  std::vector<float> e(3 + 3 + 16 + 16 + kPaletteSlots * 12);
  e[0] = g_L_frame.x, e[1] = g_L_frame.y, e[2] = g_L_frame.z;
  if (!g_L_known) e[0] = e[1] = e[2] = NAN;
  e[3] = d.x, e[4] = d.y, e[5] = d.z;
  memcpy(&e[6], globals + kPersOffset, 64);
  memcpy(&e[22], globals + kMat0Offset, 64);
  memcpy(&e[38], globals + kPaletteOffset, kPaletteSlots * 48);
  g_dump.push_back(std::move(e));
}

// At the end of a recorded frame. Caller holds g_cap_mutex.
void write_dump() {
  FILE* file = nullptr;
  if (_wfopen_s(&file, g_dump_path.c_str(), L"w") != 0 || !file) return;
  const game::Vec3* snake = game::snake_position();
  if (snake) fprintf(file, "snake %.3f %.3f %.3f\n", snake->x, snake->y, snake->z);
  const float* bones = g_dump_bones;
  const float* origin = g_dump_origin;
  const bool relative = g_dump_relative;
  if (g_dump_have_bones) {
    fprintf(file, "origin %.3f %.3f %.3f relative %d\nobject", origin[0], origin[1], origin[2], relative ? 1 : 0);
    for (const float v : g_dump_object) fprintf(file, " %.6g", v);
    fprintf(file, "\n");
    for (int b = 0; b < pose::kSnakeBones; ++b) {
      fprintf(file, "bone %d", b);
      for (int i = 0; i < 16; ++i) fprintf(file, " %.6g", bones[b * 16 + i]);
      fprintf(file, "\n");
    }
  }
  {
    float ours[pose::kSnakeBones * 16];
    if (pose::current(ours)) {
      for (int b = 0; b < pose::kSnakeBones; ++b) {
        fprintf(file, "ours %d", b);
        for (int i = 0; i < 16; ++i) fprintf(file, " %.6g", ours[b * 16 + i]);
        fprintf(file, "\n");
      }
    }
    float board[skate::kBoardBodies * 16];
    if (skate::board_pose(board)) {
      fprintf(file, "deck");
      for (int i = 0; i < 16; ++i) fprintf(file, " %.6g", board[MGSK_BODY_DECK * 16 + i]);
      fprintf(file, "\n");
    }
  }
  for (const auto& e : g_dump) {
    fprintf(file, "draw");
    for (const float v : e) fprintf(file, " %.6g", v);
    fprintf(file, "\n");
  }
  fclose(file);
  log("render: palette dump: %zu skinned draws -> %ls", g_dump.size(), g_dump_path.c_str());
  g_dump.clear();
}

// ---- posing Snake through his bone palette ---------------------------------------------------
//
// Skinned vertex shaders take a bone palette gVS_Mat[64] (row_major float3x4 at byte 624; each
// draw (mesh) indexes a few slots through BLENDINDICES) and transform by gVS_Pers after it. For
// Snake every used slot is, exactly (checked to 0.0 mm on a palette dump):
//   slot = [ R^T | T - O - R^T * rest ]
// with R, T one of his bones as the game computed it (game layout: rows = axes, translation row),
// rest that bone's rest position in sna_def.mdl (no rest rotations) and O = L + d, the render
// object's translation (obj+0x20 row 3). So the slot is bone x inverse bind, relative to O.
// Which bone a slot holds is found by matching it against the game's own bones for the frame
// (pose::game_bones), cached per frame by content; the slot is then rebuilt from the published
// pose (pose::current). Draws of other objects (his gear) don't sit on O and are left alone.

constexpr int kSnakeBones = pose::kSnakeBones;
float g_rest[kSnakeBones][3];
bool g_rest_loaded = false;

// sna_def.mdl bone table (tools/mdl_bones.py): count at 0x04, bones at [0x18], 0x50 bytes each,
// rest position (model space, mm) at +0x20.
void load_rest(const std::filesystem::path& mdl) {
  FILE* file = nullptr;
  if (_wfopen_s(&file, mdl.c_str(), L"rb") != 0 || !file) {
    log("render: %ls not readable; Snake can't be posed", mdl.c_str());
    return;
  }
  std::vector<uint8_t> data;
  uint8_t chunk[65536];
  for (size_t n; (n = fread(chunk, 1, sizeof(chunk), file)) > 0;) data.insert(data.end(), chunk, chunk + n);
  fclose(file);
  uint32_t count = 0;
  uint64_t offset = 0;
  if (data.size() >= 0x50) {
    memcpy(&count, data.data() + 4, 4);
    memcpy(&offset, data.data() + 0x18, 8);
  }
  if (count != kSnakeBones || offset + count * 0x50ull > data.size()) {
    log("render: %ls: unexpected bone table (%u bones); Snake can't be posed", mdl.c_str(), count);
    return;
  }
  for (int b = 0; b < kSnakeBones; ++b) memcpy(g_rest[b], data.data() + offset + b * 0x50 + 0x20, 12);
  g_rest_loaded = true;
  log("render: Snake rest skeleton from %ls (%u bones)", mdl.c_str(), count);
}

// Per frame (guarded by g_cap_mutex): the game's and our bones, and the slot -> bone cache.
struct SkinFrame {
  int state = 0;  // 0 not fetched, 1 ready, -1 nothing to do this frame
  float game[pose::kBoneHistory][kSnakeBones * 16];  // the last few passes, newest first
  Vec origin[pose::kBoneHistory];                     // each pass's render object translation, O
  int passes = 0;
  float ours[kSnakeBones * 16];
  // slot content hash -> bone + kSnakeBones * pass (-1 = not Snake's)
  std::vector<std::pair<uint64_t, int>> slots;
  uint64_t posed = 0, draws = 0;
};
SkinFrame g_skin;

uint64_t hash_slot(const float* f) {
  uint64_t h = 1469598103934665603ull;
  const auto* b = reinterpret_cast<const uint8_t*>(f);
  for (int i = 0; i < 48; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

// The bone whose inverse-bind matrix relative to O is closest to `slot` (3 rows of 4), or -1.
// Not exact: the game draws at 60 fps from 30 fps logic and blends bones between logic frames,
// so while Snake animates his palette lies between two passes' bones. Rotation picks the
// candidates; the rest-adjusted translation (bones sit centimetres apart) picks among them.
// `err` returns the match's error (rotation sum + translation mm / 100) for choosing between passes.
int match_bone(const float* slot, const float* bones, Vec origin, float& err) {
  constexpr float kMaxRot = 0.6f;     // sum of |differences| over the 9 rotation entries
  constexpr float kMaxMoveMm = 60.f;  // largest translation difference per axis
  int best = -1;
  err = 1e30f;
  const float o[3] = {origin.x, origin.y, origin.z};
  for (int b = 0; b < kSnakeBones; ++b) {
    const float* m = bones + b * 16;  // rows: X, Y, Z axis, translation
    float rot = 0;
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) rot += std::fabs(slot[r * 4 + c] - m[c * 4 + r]);
    }
    if (rot > kMaxRot) continue;
    float move = 0;
    for (int r = 0; r < 3; ++r) {
      const float t = m[12 + r] - o[r] - (m[0 * 4 + r] * g_rest[b][0] + m[1 * 4 + r] * g_rest[b][1] + m[2 * 4 + r] * g_rest[b][2]);
      move = std::max(move, std::fabs(slot[r * 4 + 3] - t));
    }
    if (move > kMaxMoveMm) continue;
    const float e = rot + move / 100.f;
    if (e < err) {
      err = e;
      best = b;
    }
  }
  return best;
}

// Rewrites Snake's palette in a $Globals image with the published pose; true if this draw was
// one of Snake's. Caller holds g_cap_mutex.
bool pose_snake(uint8_t* globals) {
  if (!g_rest_loaded || g_skin.state < 0 || !g_L_known) return false;
  Mat pers;
  Vec d;
  if (!main_camera_draw(globals, pers, d) || dot(d, d) < 1.f) return false;  // skinned draws only
  if (g_skin.state == 0) {
    float objects[pose::kBoneHistory][16];
    g_skin.passes = pose::current(g_skin.ours) ? pose::game_bone_history(g_skin.game, objects, pose::kBoneHistory) : 0;
    for (int k = 0; k < g_skin.passes; ++k) g_skin.origin[k] = {objects[k][12], objects[k][13], objects[k][14]};
    g_skin.state = g_skin.passes > 0 ? 1 : -1;
    if (g_skin.state < 0) return false;
  }
  // No check of the draw's own origin: some of Snake's draws (about 22 a frame, ~640 mm above his
  // object) sit on another origin yet use palettes relative to his object's, and only his slots
  // match his bones exactly, so matching alone picks them out.
  const uint64_t posed_before = g_skin.posed;
  auto* palette = reinterpret_cast<float*>(globals + kPaletteOffset);
  for (UINT s = 0; s < kPaletteSlots; ++s) {
    float* slot = palette + s * 12;
    const uint64_t h = hash_slot(slot);
    int bone = -2;
    for (const auto& [key, b] : g_skin.slots) {
      if (key == h) {
        bone = b;
        break;
      }
    }
    if (bone == -2) {
      // While he animates, the palette may come from a pass a frame or two before the latest.
      bone = -1;
      float best = 1e30f;
      for (int k = 0; k < g_skin.passes; ++k) {
        float e;
        const int b = match_bone(slot, g_skin.game[k], g_skin.origin[k], e);
        if (b >= 0 && e < best) {
          best = e;
          bone = b + kSnakeBones * k;
        }
      }
      g_skin.slots.emplace_back(h, bone);
    }
    if (bone < 0) continue;
    const int pass = bone / kSnakeBones;
    bone %= kSnakeBones;
    const float* m = g_skin.ours + bone * 16;
    // Write relative to the origin the GPU adds back for this draw (L + d) when it is his body's:
    // where the game moves him between our writes (no floor of its own under him) the snapshot's
    // origin lags it and he'd jitter. Draws on another origin (his head, ~640 mm up) keep the
    // snapshot's.
    Vec o_draw{g_L_frame.x + d.x, g_L_frame.y + d.y, g_L_frame.z + d.z};
    const Vec gap = o_draw - g_skin.origin[pass];
    if (dot(gap, gap) > 300.f * 300.f) o_draw = g_skin.origin[pass];
    const float o[3] = {o_draw.x, o_draw.y, o_draw.z};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) slot[r * 4 + c] = m[c * 4 + r];
      slot[r * 4 + 3] = m[12 + r] - o[r] -
                        (m[0 * 4 + r] * g_rest[bone][0] + m[1 * 4 + r] * g_rest[bone][1] + m[2 * 4 + r] * g_rest[bone][2]);
    }
    ++g_skin.posed;
  }
  if (g_skin.posed == posed_before) return false;
  ++g_skin.draws;
  return true;
}

void end_skin_frame() {
  if (g_skin.posed || g_skin.draws) pose::count_applied(g_skin.posed, g_skin.draws);
  g_skin.state = 0;
  g_skin.slots.clear();
  g_skin.posed = g_skin.draws = 0;
}

HRESULT STDMETHODCALLTYPE on_map(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags,
                                 D3D11_MAPPED_SUBRESOURCE* out) {
  const HRESULT hr = g_map_hook.stdcall<HRESULT>(c, res, sub, type, flags, out);
  if (SUCCEEDED(hr) && c == g_cap_context && sub == 0 && out && out->pData && type != D3D11_MAP_READ &&
      is_globals(res)) {
    std::lock_guard lock(g_cap_mutex);
    ++g_cap_maps;
    for (Pending& p : g_pending_maps) {
      if (!p.res || p.res == res) {
        p = {res, static_cast<uint8_t*>(out->pData)};
        break;
      }
    }
  }
  return hr;
}

void STDMETHODCALLTYPE on_unmap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub) {
  if (c == g_cap_context && sub == 0) {
    std::lock_guard lock(g_cap_mutex);
    for (Pending& p : g_pending_maps) {
      if (p.res == res) {
        tally(p.data);  // the game has written the whole buffer by now
        note_look_at(p.data);
        if (g_dump_state == 2) dump_draw(p.data);
        note_depth(c, p.data, pose_snake(p.data));
        if (g_ovr_active) rewrite(p.data);
        p = {};
        break;
      }
    }
  }
  g_unmap_hook.stdcall<void>(c, res, sub);
}

void STDMETHODCALLTYPE on_update(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, const D3D11_BOX* box,
                                 const void* src, UINT row_pitch, UINT depth_pitch) {
  if (c == g_cap_context && sub == 0 && src && is_globals(res)) {
    std::lock_guard lock(g_cap_mutex);
    ++g_cap_updates;
    if (!box) {
      tally(static_cast<const uint8_t*>(src));
    } else if (box->left <= kPersOffset && box->right >= kGlobalsMin) {
      tally(static_cast<const uint8_t*>(src) - box->left);
    } else {
      ++g_cap_partial;
    }
  }
  g_update_hook.stdcall<void>(c, res, sub, box, src, row_pitch, depth_pitch);
}

void install_capture(ID3D11DeviceContext* c) {
  if (g_cap_context) return;
  g_cap_context = c;
  void** vtable = *reinterpret_cast<void***>(c);
  constexpr int kMap = 14, kUnmap = 15, kUpdateSubresource = 48;
  g_map_hook = safetyhook::create_inline(vtable[kMap], reinterpret_cast<void*>(&on_map));
  g_unmap_hook = safetyhook::create_inline(vtable[kUnmap], reinterpret_cast<void*>(&on_unmap));
  g_update_hook = safetyhook::create_inline(vtable[kUpdateSubresource], reinterpret_cast<void*>(&on_update));
  log("render: camera capture hooks Map %s Unmap %s UpdateSubresource %s", g_map_hook ? "ok" : "FAILED",
      g_unmap_hook ? "ok" : "FAILED", g_update_hook ? "ok" : "FAILED");
}

// Called at Present: closes the frame's tally.
void end_capture_frame() {
  std::lock_guard lock(g_cap_mutex);
  g_cap_last.swap(g_cap_frame);
  g_cap_frame.clear();
  g_cap_last_writes = g_cap_writes;
  g_cap_last_maps = g_cap_maps;
  g_cap_last_updates = g_cap_updates;
  g_cap_last_partial = g_cap_partial;
  g_cap_writes = g_cap_maps = g_cap_updates = g_cap_partial = 0;
  pick_scene_depth();
  if (g_L_known) {
    g_L_prev2 = g_L_prev;
    g_L_prev = g_L_frame;
    g_L_history = std::min(g_L_history + 1, 2);
  } else {
    g_L_history = 0;
  }
  g_L_known = false;
  end_skin_frame();
  if (g_dump_state == 2) {
    write_dump();
    g_dump_state = 0;
  } else if (g_dump_state == 1) {
    g_dump_state = 2;
  }
  g_ovr_used = g_ovr_active;
  g_ovr_used_vp = g_ovr_vp;
  g_ovr_active = false;  // frame() turns it back on for the next frame
  g_ovr_last_replaced = g_ovr_replaced;
  g_ovr_last_early = g_ovr_early;
  g_ovr_replaced = g_ovr_early = 0;
}

// Sets (or clears) the camera the next frame is drawn from.
void set_override(const Mat* vp) {
  std::lock_guard lock(g_cap_mutex);
  g_ovr_active = vp != nullptr;
  if (vp) g_ovr_vp = *vp;
}

// The override the frame that just ended was drawn with, if any.
bool used_override(Mat& vp, uint64_t& replaced, uint64_t& early) {
  std::lock_guard lock(g_cap_mutex);
  vp = g_ovr_used_vp;
  replaced = g_ovr_last_replaced;
  early = g_ovr_last_early;
  return g_ovr_used;
}

// What the game uploads for its stage geometry (seen on s091a):
// - gVS_Mat0 = a pure translation by minus the camera's look-at point (rotation identity), and
// - gVS_Pers = projection x view rotation, with no translation: its x/y/w rows carry no
//   constant term (the z row's constant is the depth scale, ~50).
// Skinned draws use a gVS_Pers with the translation folded in and leave stale data in gVS_Mat0,
// so they don't match. A pair that matches both is the view-projection, split in two.
bool stage_translation(const Mat& m) {
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 3; ++col) {
      if (std::fabs(m.at(row, col) - (row == col ? 1.f : 0.f)) > 1e-5f) return false;
    }
  }
  return m.at(3, 3) == 1.f;
}

bool rotation_projection(const Mat& p) {
  const float w = std::sqrt(p.at(3, 0) * p.at(3, 0) + p.at(3, 1) * p.at(3, 1) + p.at(3, 2) * p.at(3, 2));
  return std::fabs(p.at(0, 3)) < 1e-3f && std::fabs(p.at(1, 3)) < 1e-3f && std::fabs(p.at(3, 3)) < 1e-3f &&
         std::fabs(w - 1.f) < 1e-3f && std::fabs(p.at(0, 0) * p.at(0, 0) + p.at(0, 1) * p.at(0, 1) + p.at(0, 2) * p.at(0, 2)) > 1e-6f;
}

// The main camera of the last frame: the most-used pair of that shape.
bool captured_camera(Mat& pers, Mat& view, uint32_t& uses) {
  std::lock_guard lock(g_cap_mutex);
  const CamSample* best = nullptr;
  for (const CamSample& e : g_cap_last) {
    if (!stage_translation(from_rows(e.mat0)) || !rotation_projection(from_rows(e.pers))) continue;
    if (!best || e.count > best->count) best = &e;
  }
  if (!best) return false;
  pers = from_rows(best->pers);
  view = from_rows(best->mat0);
  uses = best->count;
  return true;
}

std::string mat_text(const float* f) {
  char text[400];
  snprintf(text, sizeof(text), "[%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g]",
           f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
  return text;
}

// Diagnostics: the most-used pairs of the last frame, and where Snake lands through each.
void log_capture(const Vec* snake) {
  std::vector<CamSample> last;
  uint64_t writes, maps, updates, partial;
  {
    std::lock_guard lock(g_cap_mutex);
    last = g_cap_last;
    writes = g_cap_last_writes;
    maps = g_cap_last_maps;
    updates = g_cap_last_updates;
    partial = g_cap_last_partial;
  }
  std::sort(last.begin(), last.end(), [](const CamSample& a, const CamSample& b) { return a.count > b.count; });
  log("render: capture: %llu $Globals writes (%llu maps, %llu updates, %llu partial), %zu distinct",
      static_cast<unsigned long long>(writes), static_cast<unsigned long long>(maps),
      static_cast<unsigned long long>(updates), static_cast<unsigned long long>(partial), last.size());
  for (size_t i = 0; i < last.size() && i < 6; ++i) {
    const CamSample& e = last[i];
    const Mat vp = mul(from_rows(e.pers), from_rows(e.mat0));
    log("render:   #%zu x%u pers %s", i, e.count, mat_text(e.pers).c_str());
    log("render:       mat0 %s%s%s", mat_text(e.mat0).c_str(), snake ? " snake -> " : "",
        snake ? project(vp, *snake).c_str() : "");
  }
}

// ---- drawing ---------------------------------------------------------------------------------

// Flat colours (0..255): deck, truck, wheel.
constexpr float kDeck[3] = {60, 62, 70}, kTruck[3] = {139, 140, 146}, kWheel[3] = {177, 174, 169};
const float* part_color(int body) {
  if (body == MGSK_BODY_DECK) return kDeck;
  if (body == MGSK_BODY_FRONT_TRUCK || body == MGSK_BODY_BACK_TRUCK) return kTruck;
  return kWheel;
}

// This frame's scene depth test for the board's pixel shader (render thread only): mode 0 off,
// 1 single-sampled, 2 multisampled; see the shader's cbuffer.
struct DepthTest {
  float params[4] = {1.f, 1.f, 0.f, 0.f};  // scene pixels per target pixel (x, y), bias, mode
  float extra[4] = {};                      // sample count, probe pixel
};
DepthTest g_depth_test;

// With `texture`, the colour is that board texture times rgb255 (white = the texture as is).
void draw(ID3D11DeviceContext* c, const Mat& vp, const Mat& world, const float rgb255[3], const Range& r,
          ID3D11ShaderResourceView* texture = nullptr) {
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(c->Map(g_gpu.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
  DrawConstants k;
  memcpy(k.mvp, mul(vp, world).m, sizeof(k.mvp));
  memcpy(k.world, world.m, sizeof(k.world));
  for (int i = 0; i < 3; ++i) k.color[i] = rgb255[i] / 255.f;
  k.color[3] = 1.f;
  memcpy(k.depth_params, g_depth_test.params, sizeof(k.depth_params));
  memcpy(k.depth_extra, g_depth_test.extra, sizeof(k.depth_extra));
  k.material[0] = texture ? 1.f : 0.f;
  k.material[1] = k.material[2] = k.material[3] = 0.f;
  memcpy(mapped.pData, &k, sizeof(k));
  c->Unmap(g_gpu.cb.Get(), 0);
  if (texture) c->PSSetShaderResources(2, 1, &texture);
  c->DrawIndexed(r.index_count, r.start_index, r.base_vertex);
}

std::atomic<bool> g_hooked{false};
std::atomic<uint64_t> g_presents{0}, g_drawn{0};
std::atomic<uint32_t> g_width{0}, g_height{0};
bool g_was_drawing = false;  // render thread only

// The typeless format of a depth buffer's family (for a copy that can also be read) and the
// format to read its depth through, or false for a format we don't read.
bool depth_formats(DXGI_FORMAT f, DXGI_FORMAT& typeless, DXGI_FORMAT& read) {
  switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      typeless = DXGI_FORMAT_R24G8_TYPELESS, read = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      return true;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
      typeless = DXGI_FORMAT_R32_TYPELESS, read = DXGI_FORMAT_R32_FLOAT;
      return true;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      typeless = DXGI_FORMAT_R32G8X24_TYPELESS, read = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
      return true;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
      typeless = DXGI_FORMAT_R16_TYPELESS, read = DXGI_FORMAT_R16_UNORM;
      return true;
    default:
      return false;
  }
}

// Copies the scene's depth buffer (`scene`, found during the frame) into a texture of the same
// size, samples and format family that the board's pixel shader can read, and returns a view of
// it, or nullptr with `outcome` saying why (0 none found, 1 unusable, 2 copied). The game's own
// buffer is only read (not bound at Present).
ID3D11ShaderResourceView* scene_depth_copy(ID3D11Device* d, ID3D11DeviceContext* c, ID3D11DepthStencilView* scene,
                                           D3D11_TEXTURE2D_DESC& td, int& outcome) {
  outcome = 0;
  td = {};
  if (!scene) return nullptr;
  outcome = 1;
  ComPtr<ID3D11Resource> res;
  ComPtr<ID3D11Texture2D> tex;
  scene->GetResource(&res);
  if (!res || FAILED(res.As(&tex))) return nullptr;
  tex->GetDesc(&td);
  D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
  scene->GetDesc(&vd);
  const bool plain = (vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && vd.Texture2D.MipSlice == 0) ||
                     vd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMS;
  DXGI_FORMAT typeless{}, read{};
  if (!plain || td.ArraySize != 1 || !depth_formats(td.Format, typeless, read)) return nullptr;
  Gpu& g = g_gpu;
  D3D11_TEXTURE2D_DESC want = td;
  want.Format = typeless;
  want.Usage = D3D11_USAGE_DEFAULT;
  want.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  want.CPUAccessFlags = 0;
  want.MiscFlags = 0;
  if (!g.scene_copy || memcmp(&want, &g.scene_copy_desc, sizeof(want)) != 0) {
    g.scene_copy_srv.Reset();
    g.scene_copy.Reset();
    g.scene_copy_desc = {};
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = read;
    if (td.SampleDesc.Count > 1) {
      sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = 1;
    }
    if (FAILED(d->CreateTexture2D(&want, nullptr, &g.scene_copy)) ||
        FAILED(d->CreateShaderResourceView(g.scene_copy.Get(), &sd, &g.scene_copy_srv))) {
      g.scene_copy_srv.Reset();
      g.scene_copy.Reset();
      return nullptr;
    }
    g.scene_copy_desc = want;
  }
  c->CopyResource(g.scene_copy.Get(), tex.Get());
  outcome = 2;
  return g.scene_copy_srv.Get();
}

// The game-depth outcome last logged, and when (log lines are rate-limited).
struct DepthLog {
  int outcome = -1;
  UINT width = 0, height = 0, samples = 0;
  DXGI_FORMAT format{};
  ULONGLONG at = 0;
  int suppressed = 0;
} g_depth_log;

// Sets up the board's depth for this frame: our own buffer (cleared) for its parts to sort
// among themselves, and, when asked and usable, the scene's depth for the pixel shader to hide
// behind (only with the game's reversed depth, which the board's projection shares: k / w,
// k ~ 50, the same as gVS_Pers). Returns our depth view; binds the scene depth as t0 or t1.
ID3D11DepthStencilView* pick_depth(ID3D11Device* d, ID3D11DeviceContext* c, const D3D11_TEXTURE2D_DESC& bb,
                                   const Settings& s, bool reversed) {
  g_depth_test = {};
  ID3D11ShaderResourceView* srvs[2] = {};
  if (s.depth_game && reversed) {
    ComPtr<ID3D11DepthStencilView> scene;
    {
      std::lock_guard lock(g_cap_mutex);
      scene = g_scene_depth;
    }
    D3D11_TEXTURE2D_DESC td{};
    int outcome = 0;
    ID3D11ShaderResourceView* srv = scene_depth_copy(d, c, scene.Get(), td, outcome);
    if (srv) {
      const bool ms = td.SampleDesc.Count > 1;
      g_depth_test.params[0] = static_cast<float>(td.Width) / static_cast<float>(bb.Width);
      g_depth_test.params[1] = static_cast<float>(td.Height) / static_cast<float>(bb.Height);
      g_depth_test.params[2] = s.depth_bias;
      g_depth_test.params[3] = ms ? 2.f : 1.f;
      g_depth_test.extra[0] = static_cast<float>(td.SampleDesc.Count);
      srvs[ms ? 1 : 0] = srv;
    }
    DepthLog& l = g_depth_log;
    const bool changed = outcome != l.outcome || td.Width != l.width || td.Height != l.height ||
                         td.SampleDesc.Count != l.samples || td.Format != l.format;
    if (changed) {
      const ULONGLONG now = GetTickCount64();
      if (now - l.at >= 2000) {
        if (outcome == 0) {
          log("render: depth_game: no scene depth buffer seen yet; the board draws over everything");
        } else {
          log("render: depth_game: scene depth %ux%u format %d x%u (back buffer %ux%u): %s%s", td.Width, td.Height,
              static_cast<int>(td.Format), td.SampleDesc.Count, bb.Width, bb.Height,
              outcome == 2 ? "read by the board's shader, the board hides behind the scene"
                           : "unusable, the board draws over everything",
              l.suppressed ? " (other changes since the last line not logged)" : "");
        }
        l = {outcome, td.Width, td.Height, td.SampleDesc.Count, td.Format, now, 0};
      } else {
        ++l.suppressed;
      }
    }
  } else {
    g_depth_log.outcome = -1;
  }
  c->PSSetShaderResources(0, 2, srvs);
  ID3D11DepthStencilView* own = own_depth(d, bb);
  if (own) c->ClearDepthStencilView(own, D3D11_CLEAR_DEPTH, reversed ? 0.f : 1.f, 0);
  return own;
}

// `render depth_debug`: the scene's depth at a scene pixel, read back (stalls; once a second).
bool probe_scene_depth(ID3D11DeviceContext* c, float px, float py, float& out) {
  Gpu& g = g_gpu;
  if (g_depth_test.params[3] < 0.5f) return false;
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(c->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  DrawConstants k{};
  memcpy(k.depth_params, g_depth_test.params, sizeof(k.depth_params));
  memcpy(k.depth_extra, g_depth_test.extra, sizeof(k.depth_extra));
  k.depth_extra[1] = px;
  k.depth_extra[2] = py;
  memcpy(mapped.pData, &k, sizeof(k));
  c->Unmap(g.cb.Get(), 0);
  ID3D11RenderTargetView* rtv = g.probe_rtv.Get();
  c->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
  const D3D11_VIEWPORT one{0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
  c->RSSetViewports(1, &one);
  c->OMSetDepthStencilState(nullptr, 0);
  c->IASetInputLayout(nullptr);
  c->VSSetShader(g.vs_probe.Get(), nullptr, 0);
  c->PSSetShader(g.ps_probe.Get(), nullptr, 0);
  c->Draw(3, 0);
  c->CopyResource(g.probe_staging.Get(), g.probe.Get());
  if (FAILED(c->Map(g.probe_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
  memcpy(&out, mapped.pData, sizeof(out));
  c->Unmap(g.probe_staging.Get(), 0);
  return true;
}

ULONGLONG g_depth_debug_at = 0;

void frame(IDXGISwapChain* swap) {
  g_presents.fetch_add(1, std::memory_order_relaxed);
  ComPtr<ID3D11Device> device;
  if (FAILED(swap->GetDevice(IID_PPV_ARGS(&device)))) return;
  ComPtr<ID3D11DeviceContext> c;
  device->GetImmediateContext(&c);
  install_capture(c.Get());
  end_capture_frame();
  const Settings s = settings();
  // The camera the frame that just ended was drawn from, and the one for the next frame.
  Mat skate_vp;
  uint64_t replaced = 0, early = 0;
  const bool have_skate_camera = used_override(skate_vp, replaced, early);
  {
    float cam[skate::kCameraFloats];
    DXGI_SWAP_CHAIN_DESC sd{};
    const bool skate_mode =
        s.camera == Settings::Camera::Skate || (s.camera == Settings::Camera::Drive && !camera::driving());
    if (skate_mode && skate::camera(cam) && SUCCEEDED(swap->GetDesc(&sd)) &&
        sd.BufferDesc.Width > 0 && sd.BufferDesc.Height > 0) {
      const float bb_aspect = static_cast<float>(sd.BufferDesc.Width) / static_cast<float>(sd.BufferDesc.Height);
      const Mat next = skate_view_projection(cam, s.aspect > 0.f ? s.aspect : bb_aspect, s.skate_fov_deg, 50.f);
      set_override(&next);
    }
  }
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
  Mat game_pers, game_cam_view;
  uint32_t game_uses = 0;
  const bool have_game_camera = s.camera != Settings::Camera::Object && captured_camera(game_pers, game_cam_view, game_uses);
  if ((!have_board && !have_snake) || (!have_skate_camera && !have_game_camera && !game::camera(eye, target))) {
    g_was_drawing = false;
    return;
  }
  if (!ensure_gpu(device.Get())) return;
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
  D3D11_TEXTURE2D_DESC bb{};
  back->GetDesc(&bb);
  ComPtr<ID3D11RenderTargetView> rtv;
  if (bb.Width == 0 || bb.Height == 0 || FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &rtv))) return;

  SavedState saved;
  save(c.Get(), saved);
  const float aspect = s.aspect > 0.f ? s.aspect : static_cast<float>(bb.Width) / static_cast<float>(bb.Height);
  const Vec eye_v{eye.x, eye.y, eye.z}, target_v{target.x, target.y, target.z};
  const Mat vp = have_skate_camera  ? skate_vp
                 : have_game_camera ? mul(game_pers, game_cam_view)
                     : mul(perspective(s.fov_deg * std::numbers::pi_v<float> / 180.f, aspect, s.near_mm, s.far_mm, s.flip_z),
                           look_at(eye_v, target_v, s.flip_z));
  // The game's projection gives depth = k / distance (nearer = larger), so compare the other way.
  const bool reversed = have_skate_camera || (have_game_camera && game_pers.at(2, 3) > 0.f);

  ID3D11DepthStencilView* dsv = pick_depth(device.Get(), c.Get(), bb, s, reversed);

  ID3D11RenderTargetView* target_rtv = rtv.Get();
  c->OMSetRenderTargetsAndUnorderedAccessViews(1, &target_rtv, dsv, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
  c->OMSetDepthStencilState((reversed ? g_gpu.depth_own_rev : g_gpu.depth_own).Get(), 0);
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
  ID3D11SamplerState* sampler = g_gpu.sampler.Get();
  c->PSSetSamplers(0, 1, &sampler);
  const bool textured = s.textures && !g_gpu.board_srvs.empty();

  // Engine metres -> MGS3 millimetres, same axes.
  const Mat to_mm = place({0.f, 0.f, 0.f}, {kMmPerUnit, kMmPerUnit, kMmPerUnit});
  Vec deck{};
  if (have_board) {
    for (int body = 0; body < skate::kBoardBodies; ++body) {
      Mat local;
      memcpy(local.m, pose + body * 16, sizeof(local.m));
      const Mat world = mul(to_mm, local);
      if (body == MGSK_BODY_DECK) deck = {world.at(0, 3), world.at(1, 3), world.at(2, 3)};
      if (textured && !g_part_runs[body].empty()) {
        constexpr float kWhite[3] = {255, 255, 255};
        for (const TexturedRange& run : g_part_runs[body]) {
          draw(c.Get(), vp, world, kWhite, run.range, g_gpu.board_srvs[run.texture].Get());
        }
      } else {
        draw(c.Get(), vp, world, part_color(body), g_parts[body]);
      }
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
  if (s.depth_debug && have_board && GetTickCount64() - g_depth_debug_at >= 1000) {
    g_depth_debug_at = GetTickCount64();
    float clip[4];
    for (int i = 0; i < 4; ++i) clip[i] = vp.at(i, 0) * deck.x + vp.at(i, 1) * deck.y + vp.at(i, 2) * deck.z + vp.at(i, 3);
    if (clip[3] <= 0.f) {
      log("render: depth_debug: deck behind the camera");
    } else {
      const float x = (clip[0] / clip[3] + 1.f) * 0.5f * static_cast<float>(bb.Width);
      const float y = (1.f - clip[1] / clip[3]) * 0.5f * static_cast<float>(bb.Height);
      const float own = clip[2] / clip[3];
      float scene_d = 0.f;
      const bool on_screen = x >= 0.f && y >= 0.f && x < static_cast<float>(bb.Width) && y < static_cast<float>(bb.Height);
      if (on_screen && probe_scene_depth(c.Get(), x * g_depth_test.params[0], y * g_depth_test.params[1], scene_d)) {
        log("render: depth_debug: deck centre at pixel %.0f %.0f, its depth %.6f (k/w: distance %.0f mm); scene depth "
            "there %.6f (x%.0f samples, farthest) -> board %s (bias %.4f)",
            x, y, own, clip[3], scene_d, g_depth_test.extra[0], scene_d > own * (1.f + s.depth_bias) ? "hidden" : "shown",
            s.depth_bias);
      } else {
        log("render: depth_debug: deck centre at pixel %.0f %.0f, its depth %.6f; no scene depth to compare (%s)", x, y,
            own, on_screen ? "depth_game off or unusable" : "off screen");
      }
    }
  }
  restore(c.Get(), saved);

  const uint64_t drawn = g_drawn.fetch_add(1, std::memory_order_relaxed) + 1;
  g_width = bb.Width;
  g_height = bb.Height;
  if (!g_was_drawing || drawn % 300 == 0) {
    if (have_skate_camera) {
      float cam[skate::kCameraFloats] = {};
      skate::camera(cam);
      log("render: frame %llu %ux%u; skate camera: %llu draws redrawn (%llu before the frame's look-at was known); "
          "eye %.0f %.0f %.0f mm; %s",
          static_cast<unsigned long long>(drawn), bb.Width, bb.Height, static_cast<unsigned long long>(replaced),
          static_cast<unsigned long long>(early), cam[0], cam[1], cam[2], describe(s).c_str());
    } else if (have_game_camera) {
      log("render: frame %llu %ux%u fmt %d; game camera (pair used %u times): look-at %.0f %.0f %.0f mm, forward %.3f %.3f "
          "%.3f, x scale %.4f, y scale %.4f, depth %.3f/w; %s",
          static_cast<unsigned long long>(drawn), bb.Width, bb.Height, static_cast<int>(bb.Format), game_uses,
          -game_cam_view.at(0, 3), -game_cam_view.at(1, 3), -game_cam_view.at(2, 3), game_pers.at(3, 0),
          game_pers.at(3, 1), game_pers.at(3, 2),
          std::sqrt(game_pers.at(0, 0) * game_pers.at(0, 0) + game_pers.at(0, 1) * game_pers.at(0, 1) +
                    game_pers.at(0, 2) * game_pers.at(0, 2)),
          std::sqrt(game_pers.at(1, 0) * game_pers.at(1, 0) + game_pers.at(1, 1) * game_pers.at(1, 1) +
                    game_pers.at(1, 2) * game_pers.at(1, 2)),
          game_pers.at(2, 3), describe(s).c_str());
    } else {
      log("render: frame %llu %ux%u fmt %d aspect %.3f; eye %.0f %.0f %.0f look-at %.0f %.0f %.0f (mm); %s",
          static_cast<unsigned long long>(drawn), bb.Width, bb.Height, static_cast<int>(bb.Format), aspect, eye.x, eye.y,
          eye.z, target.x, target.y, target.z, describe(s).c_str());
    }
    log_capture(have_snake ? &at_snake : nullptr);
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
  g_dump_path = plugin_dir / L"MetalGearSkate.palette.txt";
  {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    load_rest(std::filesystem::path(exe).parent_path() / L"assets" / L"mdl" / L"us" / L"sna_def.mdl");
  }
  log("render: settings %s", describe(settings()).c_str());
  char assets[MAX_PATH] = {};
  GetPrivateProfileStringA("skate", "skate_data", "", assets, MAX_PATH, ini.string().c_str());
  add_cube();
  load_board(ffi, assets);
  if (!compile_shaders()) return;
  g_hooked = install_hooks();
}

std::string set(const std::string& key, const std::string& value) {
  if (key == "palette") {  // `render palette now`: dump the next frame's skinned draws
    std::lock_guard lock(g_cap_mutex);
    g_dump_state = 1;
    return "ok dumping next frame";
  }
  if (key == "capture") {  // `render capture now`: log the last frame's captured camera matrices
    const game::Vec3* p = game::snake_position();
    const Vec snake = p ? Vec{p->x, p->y, p->z} : Vec{};
    log_capture(p ? &snake : nullptr);
    return "ok logged";
  }
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
