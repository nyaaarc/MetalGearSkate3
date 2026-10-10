#include "control.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "core.hpp"
#include "game.hpp"
#include "log.hpp"
#include "audio.hpp"
#include "render.hpp"
#include "skate.hpp"

namespace control {
namespace {

std::filesystem::path g_dir;

struct ButtonName {
  const char* name;
  uint16_t bit;
};
// PS2 bits, as in game::Pad.
constexpr ButtonName kButtons[] = {
    {"cross", 0x4000}, {"circle", 0x2000}, {"square", 0x8000}, {"triangle", 0x1000}, {"l1", 0x0400},
    {"r1", 0x0800},    {"l2", 0x0100},     {"r2", 0x0200},     {"start", 0x0008},    {"select", 0x0001},
    {"l3", 0x0002},    {"r3", 0x0004},     {"up", 0x0010},     {"right", 0x0020},    {"down", 0x0040},
    {"left", 0x0080},
};

// "cross+up", "0x4010" or "none".
bool parse_buttons(const std::string& text, uint16_t& out) {
  out = 0;
  if (text == "none" || text == "0") return true;
  if (text.starts_with("0x")) {
    out = static_cast<uint16_t>(std::stoul(text, nullptr, 16));
    return true;
  }
  std::stringstream parts(text);
  for (std::string part; std::getline(parts, part, '+');) {
    bool known = false;
    for (const ButtonName& b : kButtons) {
      if (part == b.name) {
        out = static_cast<uint16_t>(out | b.bit);
        known = true;
      }
    }
    if (!known) return false;
  }
  return true;
}

std::string run(const std::string& line) {
  std::stringstream in(line);
  std::string verb;
  in >> verb;
  if (verb.empty() || verb[0] == '#') return "";
  if (verb == "pad") {
    std::string buttons;
    int lx, ly, rx, ry, frames;
    uint16_t mask;
    if (!(in >> buttons >> lx >> ly >> rx >> ry >> frames) || !parse_buttons(buttons, mask)) {
      return "error: usage pad <buttons> <lx> <ly> <rx> <ry> <frames>";
    }
    game::inject_pad(mask, static_cast<uint8_t>(lx), static_cast<uint8_t>(ly), static_cast<uint8_t>(rx),
                     static_cast<uint8_t>(ry), frames);
    return "ok";
  }
  if (verb == "seq") {
    // seq <buttons> <lx> <ly> <rx> <ry> <frames> ; <buttons> ... ; ...   (plays back to back)
    std::string rest;
    std::getline(in, rest);
    std::stringstream steps(rest);
    game::inject_pad(0, 128, 128, 128, 128, 0);
    int count = 0;
    for (std::string step; std::getline(steps, step, ';');) {
      std::stringstream fields(step);
      std::string buttons;
      int lx, ly, rx, ry, frames;
      uint16_t mask;
      if (!(fields >> buttons >> lx >> ly >> rx >> ry >> frames) || !parse_buttons(buttons, mask)) {
        return "error: bad step " + std::to_string(count + 1) + ": usage <buttons> <lx> <ly> <rx> <ry> <frames>";
      }
      game::queue_pad(mask, static_cast<uint8_t>(lx), static_cast<uint8_t>(ly), static_cast<uint8_t>(rx),
                      static_cast<uint8_t>(ry), frames);
      ++count;
    }
    return "ok " + std::to_string(count) + " steps";
  }
  if (verb == "teleport") {
    float x, y, z;
    if (!(in >> x >> y >> z)) return "error: usage teleport <x> <y> <z> (mm, Snake's waist position)";
    game::Vec3* snake = game::snake_position();
    if (!snake) return "error: no player";
    *snake = {x, y, z};
    return "ok";
  }
  if (verb == "block") {
    std::string arg;
    in >> arg;
    if (arg != "on" && arg != "off") return "error: usage block on|off";
    game::block_input(arg == "on");
    return "ok";
  }
  if (verb == "peek" || verb == "poke") {
    // peek <offset> <i8|i16|i32|f32> / poke <offset> <type> <value>: the link-variable (state) buffer.
    size_t offset;
    std::string type;
    if (!(in >> offset >> type)) return "error: usage " + verb + " <linkvar offset> <i8|i16|i32|f32> [value]";
    uint8_t* link = game::linkvar();
    if (!link) return "error: no linkvar buffer";
    uint8_t* at = link + offset;
    if (verb == "poke") {
      double value;
      if (!(in >> value)) return "error: poke needs a value";
      if (type == "i8") *at = static_cast<uint8_t>(value);
      else if (type == "i16") { const int16_t v = static_cast<int16_t>(value); memcpy(at, &v, 2); }
      else if (type == "i32") { const int32_t v = static_cast<int32_t>(value); memcpy(at, &v, 4); }
      else if (type == "f32") { const float v = static_cast<float>(value); memcpy(at, &v, 4); }
      else return "error: type must be i8|i16|i32|f32";
      return "ok";
    }
    char text[64];
    if (type == "i8") snprintf(text, sizeof(text), "%d", *at);
    else if (type == "i16") { int16_t v; memcpy(&v, at, 2); snprintf(text, sizeof(text), "%d", v); }
    else if (type == "i32") { int32_t v; memcpy(&v, at, 4); snprintf(text, sizeof(text), "%d", v); }
    else if (type == "f32") { float v; memcpy(&v, at, 4); snprintf(text, sizeof(text), "%g", v); }
    else return "error: type must be i8|i16|i32|f32";
    return text;
  }
  if (verb == "actordump") {
    // actordump <label>: Snake's actor work (from his actor header, 0x120 before the control block)
    // to MetalGearSkate.actor-<label>.bin, for diffing his state between moments.
    std::string label;
    if (!(in >> label)) return "error: usage actordump <label>";
    const game::Vec3* snake = game::snake_position();
    if (!snake) return "error: no player";
    const auto* actor = reinterpret_cast<const uint8_t*>(snake) - 0x10 - 0x120;
    std::string bytes(0x1000, '\0');
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), actor, bytes.data(), bytes.size(), &got);
    bytes.resize(got);
    std::ofstream(g_dir / ("MetalGearSkate.actor-" + label + ".bin"), std::ios::binary) << bytes;
    char text[96];
    snprintf(text, sizeof(text), "ok %zu bytes from %p", static_cast<size_t>(got), static_cast<const void*>(actor));
    return text;
  }
  if (verb == "release") {
    game::inject_pad(0, 128, 128, 128, 128, 0);
    return "ok";
  }
  if (verb == "skate") {
    std::string arg;
    in >> arg;
    if (arg != "on" && arg != "off") return "error: usage skate on|off [x y z heading_deg]";
    float x, y, z, heading;
    if (arg == "on" && (in >> x >> y >> z >> heading)) {
      skate::request_at(x, y, z, heading);
      return "ok (at position)";
    }
    skate::request(arg == "on");
    return "ok";
  }
  if (verb == "reload") {  // load the current build of the core (the loader swaps it within ~250 ms)
    core::request_reload();
    return "ok reloading";
  }
  if (verb == "trace") {  // trace <ticks>: the next engine ticks to MetalGearSkate.trace.csv
    int ticks = 0;
    if (!(in >> ticks) || ticks <= 0 || ticks > 36000) return "error: usage trace <ticks 1..36000>";
    skate::trace(ticks);
    return "ok";
  }
  if (verb == "recent") {  // recent reset: start collecting states/peak height afresh
    skate::reset_recent();
    return "ok";
  }
  if (verb == "audio") {  // audio volume <0..4>
    std::string key;
    float value = 0;
    if (!(in >> key >> value) || key != "volume") return "error: usage audio volume <0..4>";
    audio::set_volume(value);
    return "ok volume " + std::to_string(audio::volume());
  }
  if (verb == "stealth") {
    std::string arg;
    in >> arg;
    if (arg != "on" && arg != "off") return "error: usage stealth on|off";
    skate::set_stealth(arg == "on");
    return "ok";
  }
  if (verb == "render") {
    // The value is the rest of the line (e.g. `render camera orbit 90 2500 1200`).
    std::string key, value;
    in >> key;
    std::getline(in >> std::ws, value);
    if (!key.empty() && value.empty()) return "error: usage render [<key> <value>]";
    return render::set(key, value);
  }
  if (verb == "stage") {  // stage <code> [<x> <y> <z>]: with a spot, Snake is held there on arrival
    std::string name;
    in >> name;
    if (name.empty()) return "error: usage stage <code> [<x> <y> <z>]";
    game::Vec3 at{};
    const bool arrive = static_cast<bool>(in >> at.x >> at.y >> at.z);
    return game::request_stage(name.c_str(), arrive ? &at : nullptr) ? "ok" : "error: stage loading unavailable";
  }
  if (verb == "mark") {
    std::string rest;
    std::getline(in, rest);
    log("mark:%s", rest.c_str());
    return "ok";
  }
  return "error: unknown command";
}

void run_command_file() {
  const auto file = g_dir / L"MetalGearSkate.cmd";
  std::ifstream in(file);
  if (!in) return;
  std::string results;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string result = run(line);
    if (!result.empty()) results += line + " -> " + result + "\n";
  }
  in.close();
  std::ofstream(g_dir / L"MetalGearSkate.cmdlog", std::ios::app) << results;
  std::error_code ignored;
  std::filesystem::remove(file, ignored);
}

// Virtual-key codes currently held (diagnostics: a stuck key can drive Snake).
std::string keys_down() {
  std::string out;
  for (int vk = 0x08; vk <= 0xFE; ++vk) {
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;  // reported as L/R variants
    if (GetAsyncKeyState(vk) & 0x8000) {
      char code[8];
      snprintf(code, sizeof(code), "%s%d", out.empty() ? "" : ",", vk);
      out += code;
    }
  }
  return out;
}

void write_state() {
  const skate::Status sk = skate::status();
  const skate::Recent recent = skate::recent();
  const render::Status rd = render::status();
  const audio::Status au = audio::status();
  game::Pad pad{};
  const bool have_pad = game::pad(pad);
  const char* stage = game::current_stage();
  const game::Vec3* snake = game::snake_position();
  const int16_t* yaw = game::snake_yaw();
  char json[2048];
  snprintf(json, sizeof(json),
           "{\"tick_ms\": %lu, \"core_generation\": %u, \"stage\": \"%s\", \"snake\": %s, \"snake_yaw\": %d,\n"
           " \"life\": [%d, %d], \"alert_count\": %d,\n"
           " \"engine_ready\": %s, \"skating\": %s, \"paused\": %s, \"skater_m\": [%.3f, %.3f, %.3f], \"skater_heading_deg\": %.1f,"
           " \"speed_mps\": %.2f, \"engine_state\": \"%s\", \"collision_triangles\": %d, \"rails\": %d,\n"
           " \"recent\": {\"states\": \"%s\", \"max_y\": %.3f, \"min_y\": %.3f, \"max_speed_mps\": %.2f},\n"
           " \"pad\": {\"seen\": %s, \"buttons\": \"0x%04X\", \"lx\": %u, \"ly\": %u, \"rx\": %u, \"ry\": %u},"
           " \"injected_frames_left\": %d, \"keys_down\": [%s],\n"
           " \"render\": {\"hooked\": %s, \"presents\": %llu, \"drawn\": %llu, \"in_scene\": %llu, \"snake_frames\": %llu, \"width\": %u,"
           " \"height\": %u},\n"
           " \"audio\": {\"running\": %s, \"frames\": %llu, \"underruns\": %llu}}\n",
           GetTickCount(), core::shared() ? core::shared()->generation : 0u, stage ? stage : "",
           snake ? (std::string("[") + std::to_string(snake->x) + ", " + std::to_string(snake->y) + ", " +
                    std::to_string(snake->z) + "]").c_str()
                 : "null",
           yaw ? *yaw : 0, game::link_short(game::kLinkCurrentHealth), game::link_short(game::kLinkMaxHealth),
           game::link_short(game::kLinkAlertCount), sk.ready ? "true" : "false", sk.skating ? "true" : "false", sk.paused ? "true" : "false", sk.skater[0], sk.skater[1],
           sk.skater[2], sk.heading_deg, sk.speed_mps, sk.engine_state, sk.collision_triangles, sk.rails, recent.states,
           recent.max_y, recent.min_y, recent.max_speed_mps, have_pad ? "true" : "false", pad.buttons,
           pad.left_x, pad.left_y, pad.right_x, pad.right_y, game::injected_frames_left(), keys_down().c_str(),
           rd.hooked ? "true" : "false", static_cast<unsigned long long>(rd.presents),
           static_cast<unsigned long long>(rd.drawn), static_cast<unsigned long long>(rd.in_scene),
           static_cast<unsigned long long>(rd.snake_frames), rd.width,
           rd.height, au.running ? "true" : "false",
           static_cast<unsigned long long>(au.frames), static_cast<unsigned long long>(au.underruns));
  // Write then rename so readers never see a half-written file.
  const auto tmp = g_dir / L"MetalGearSkate.state.json.tmp";
  std::ofstream(tmp, std::ios::trunc) << json;
  MoveFileExW(tmp.c_str(), (g_dir / L"MetalGearSkate.state.json").c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::atomic<bool> g_stop{false};
HANDLE g_thread = nullptr;

DWORD WINAPI poll(LPVOID) {
  for (int i = 0; !g_stop; ++i) {
    run_command_file();
    if (i % 4 == 0) write_state();
    Sleep(50);
  }
  return 0;
}

}  // namespace

void start(const std::filesystem::path& plugin_dir) {
  g_dir = plugin_dir;
  g_stop = false;
  g_thread = CreateThread(nullptr, 0, poll, nullptr, 0, nullptr);
}

void stop() {
  g_stop = true;
  if (g_thread) {
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
}

}  // namespace control
