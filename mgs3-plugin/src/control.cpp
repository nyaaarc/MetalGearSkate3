#include "control.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "game.hpp"
#include "log.hpp"
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
  if (verb == "release") {
    game::inject_pad(0, 128, 128, 128, 128, 0);
    return "ok";
  }
  if (verb == "skate") {
    std::string arg;
    in >> arg;
    if (arg != "on" && arg != "off") return "error: usage skate on|off";
    skate::request(arg == "on");
    return "ok";
  }
  if (verb == "render") {
    std::string key, value;
    in >> key >> value;
    if (!key.empty() && value.empty()) return "error: usage render [<key> <value>]";
    return render::set(key, value);
  }
  if (verb == "stage") {
    std::string name;
    in >> name;
    if (name.empty()) return "error: usage stage <code>";
    return game::request_stage(name.c_str()) ? "ok" : "error: stage loading unavailable";
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

void write_state() {
  const skate::Status sk = skate::status();
  const render::Status rd = render::status();
  game::Pad pad{};
  const bool have_pad = game::pad(pad);
  const char* stage = game::current_stage();
  const game::Vec3* snake = game::snake_position();
  const int16_t* yaw = game::snake_yaw();
  char json[1536];
  snprintf(json, sizeof(json),
           "{\"tick_ms\": %lu, \"stage\": \"%s\", \"snake\": %s, \"snake_yaw\": %d,\n"
           " \"engine_ready\": %s, \"skating\": %s, \"skater_m\": [%.3f, %.3f, %.3f], \"skater_heading_deg\": %.1f,"
           " \"speed_mps\": %.2f, \"engine_state\": \"%s\", \"collision_triangles\": %d, \"rails\": %d,\n"
           " \"pad\": {\"seen\": %s, \"buttons\": \"0x%04X\", \"lx\": %u, \"ly\": %u, \"rx\": %u, \"ry\": %u},"
           " \"injected_frames_left\": %d,\n"
           " \"render\": {\"hooked\": %s, \"presents\": %llu, \"drawn\": %llu, \"width\": %u, \"height\": %u}}\n",
           GetTickCount(), stage ? stage : "",
           snake ? (std::string("[") + std::to_string(snake->x) + ", " + std::to_string(snake->y) + ", " +
                    std::to_string(snake->z) + "]").c_str()
                 : "null",
           yaw ? *yaw : 0, sk.ready ? "true" : "false", sk.skating ? "true" : "false", sk.skater[0], sk.skater[1],
           sk.skater[2], sk.heading_deg, sk.speed_mps, sk.engine_state, sk.collision_triangles, sk.rails, have_pad ? "true" : "false", pad.buttons,
           pad.left_x, pad.left_y, pad.right_x, pad.right_y, game::injected_frames_left(),
           rd.hooked ? "true" : "false", static_cast<unsigned long long>(rd.presents),
           static_cast<unsigned long long>(rd.drawn), rd.width, rd.height);
  // Write then rename so readers never see a half-written file.
  const auto tmp = g_dir / L"MetalGearSkate.state.json.tmp";
  std::ofstream(tmp, std::ios::trunc) << json;
  MoveFileExW(tmp.c_str(), (g_dir / L"MetalGearSkate.state.json").c_str(), MOVEFILE_REPLACE_EXISTING);
}

DWORD WINAPI poll(LPVOID) {
  for (int i = 0;; ++i) {
    run_command_file();
    if (i % 4 == 0) write_state();
    Sleep(50);
  }
}

}  // namespace

void start(const std::filesystem::path& plugin_dir) {
  g_dir = plugin_dir;
  if (HANDLE thread = CreateThread(nullptr, 0, poll, nullptr, 0, nullptr)) CloseHandle(thread);
}

}  // namespace control
