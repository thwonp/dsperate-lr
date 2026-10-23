// SPDX-License-Identifier: GPL-3.0-or-later
// dsperate-lr - a libretro core around DSperate's emulation core.
//
// The core library (DSperate/src/core) is driven the way DSperate's own
// headless frontend drives it: load_bios, reset, load_rom, direct boot, JIT
// attach, then one run_frame() per retro_run(). Everything a frontend owns -
// pacing, presenting, audio output, thread placement - is the libretro
// frontend's here, so DSperate's thread layout stays off and its threads run
// at normal priority.
#include "libretro.h"

#include "core/nds.h"
#include "core/host_cores.h"
#include "core/state/state.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr unsigned W = ds::SCREEN_W, H = ds::SCREEN_H;   // one screen; the frame is two stacked
constexpr double FPS = 59.8261;                          // 33.513982 MHz / (6 * 355 * 263)

retro_environment_t        env_cb;
retro_video_refresh_t      video_cb;
retro_audio_sample_batch_t audio_batch_cb;
retro_input_poll_t         input_poll_cb;
retro_input_state_t        input_state_cb;
retro_log_printf_t         log_cb;

std::unique_ptr<ds::NDS> nds;
std::string rom_path;
bool jit_on = true;
uint32_t frame[W * H * 2];
ds::s16 audio[4096 * 2];
size_t state_size;   // fixed for the session: libretro states are a fixed size
// One writer for every state, cleared rather than rebuilt: its buffer keeps
// its capacity, so a capture (rewind takes one every few frames) does not
// grow 6 MB through reallocations each time.
ds::state::Writer state_w;

void log(retro_log_level lvl, const char* fmt, ...) {
  char buf[512];
  va_list a;
  va_start(a, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, a);
  va_end(a);
  if (log_cb) log_cb(lvl, "%s", buf);
  else std::fprintf(stderr, "%s", buf);
}

std::string dir(unsigned cmd) {
  const char* d = nullptr;
  return env_cb(cmd, &d) && d ? d : "";
}

void read_options() {
  retro_variable v{"dsperate_jit", nullptr};
  jit_on = !(env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value && !std::strcmp(v.value, "disabled"));
}

// Battery saves are the frontend's (RETRO_MEMORY_SAVE_RAM, a raw chip image,
// the same file format other DS cores use). A game DSperate's save list
// names has its chip sized at load; one it doesn't is sized from an existing
// .srm, read here because the frontend reads the size before it writes the
// file in. An unlisted game with no save yet is detected on its first save
// access, after the frontend asked - that one case saves nothing.
void load_unlisted_save() {
  if (!nds->cart || nds->cart->save_type_listed()) return;
  const std::string save_dir = dir(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY);
  if (save_dir.empty()) return;
  std::string base = rom_path.substr(rom_path.find_last_of('/') + 1);
  base = base.substr(0, base.find_last_of('.'));
  FILE* f = std::fopen((save_dir + "/" + base + ".srm").c_str(), "rb");
  if (!f) return;
  std::vector<ds::u8> data;
  ds::u8 buf[65536];
  for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + got);
  std::fclose(f);
  nds->cart->load_save(data.data(), data.size());
}

bool boot() {
  const std::string sys = dir(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY);
  auto in_sys = [&](const char* name) {
    const std::string p = sys + "/" + name;
    FILE* f = sys.empty() ? nullptr : std::fopen(p.c_str(), "rb");
    if (!f) return std::string();   // missing: FreeBIOS / a generated firmware
    std::fclose(f);
    return p;
  };
  std::string err;
  if (!nds->load_bios(in_sys("bios9.bin"), in_sys("bios7.bin"), in_sys("firmware.bin"), {}, &err)) {
    log(RETRO_LOG_ERROR, "dsperate: bios: %s\n", err.c_str());
    return false;
  }
  nds->reset();
  if (!nds->load_rom(rom_path)) {
    log(RETRO_LOG_ERROR, "dsperate: %s: %s\n", rom_path.c_str(), nds->rom_error.c_str());
    return false;
  }
  nds->setup_direct_boot();
  return true;
}

} // namespace

extern "C" {

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb) {
  env_cb = cb;
  static const retro_variable vars[] = {
    {"dsperate_jit", "JIT recompiler (restart); enabled|disabled"},
    {nullptr, nullptr},
  };
  cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(vars));
  retro_log_callback l{};
  if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &l)) log_cb = l.log;
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t) {}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void) { ds::set_thread_layout(false); }
RETRO_API void retro_deinit(void) {}

RETRO_API void retro_get_system_info(retro_system_info* info) {
  std::memset(info, 0, sizeof *info);
  info->library_name = "DSperate";
#ifdef DSPERATE_LR_VERSION
  info->library_version = DSPERATE_LR_VERSION;
#else
  info->library_version = "git";
#endif
  info->valid_extensions = "nds|zip";
  info->need_fullpath = true;   // the cart maps the file itself
  info->block_extract = true;   // and reads zips itself
}

RETRO_API void retro_get_system_av_info(retro_system_av_info* info) {
  std::memset(info, 0, sizeof *info);
  info->geometry.base_width = info->geometry.max_width = W;
  info->geometry.base_height = info->geometry.max_height = H * 2;
  info->geometry.aspect_ratio = float(W) / float(H * 2);
  info->timing.fps = FPS;
  info->timing.sample_rate = nds ? nds->spu.output_rate_hz() : 32728.5;
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API void retro_reset(void) {
  if (!nds) return;
#if DSPERATE_JIT
  if (jit_on) ds::jit::flush_all();
#endif
  // The cart resets in place: the frontend holds a pointer into its save RAM.
  nds->reset();
  nds->setup_direct_boot();
}

RETRO_API void retro_run(void) {
  static const struct { unsigned id; ds::u32 btn; } map[] = {
    {RETRO_DEVICE_ID_JOYPAD_A, ds::io::Io::BTN_A},           {RETRO_DEVICE_ID_JOYPAD_B, ds::io::Io::BTN_B},
    {RETRO_DEVICE_ID_JOYPAD_X, ds::io::Io::BTN_X},           {RETRO_DEVICE_ID_JOYPAD_Y, ds::io::Io::BTN_Y},
    {RETRO_DEVICE_ID_JOYPAD_L, ds::io::Io::BTN_L},           {RETRO_DEVICE_ID_JOYPAD_R, ds::io::Io::BTN_R},
    {RETRO_DEVICE_ID_JOYPAD_START, ds::io::Io::BTN_START},   {RETRO_DEVICE_ID_JOYPAD_SELECT, ds::io::Io::BTN_SELECT},
    {RETRO_DEVICE_ID_JOYPAD_UP, ds::io::Io::BTN_UP},         {RETRO_DEVICE_ID_JOYPAD_DOWN, ds::io::Io::BTN_DOWN},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, ds::io::Io::BTN_LEFT},     {RETRO_DEVICE_ID_JOYPAD_RIGHT, ds::io::Io::BTN_RIGHT},
  };
  input_poll_cb();
  ds::u32 pressed = 0;
  for (const auto& m : map)
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, m.id)) pressed |= 1u << m.btn;
  nds->io.set_buttons(pressed);

  // The pointer spans the whole frame, the two screens stacked: the bottom
  // (touch) screen is y 0..0x7fff of -0x7fff..0x7fff.
  const bool down = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
  const int px = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
  const int py = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
  if (down && py >= 0) nds->io.set_touch((px + 0x7fff) * int(W) / 0xffff, py * int(H) / 0x8000, true);
  else nds->io.set_touch(0, 0, false);

  nds->run_frame();

  std::memcpy(frame, nds->gpu.framebuffer(0), W * H * 4);
  std::memcpy(frame + W * H, nds->gpu.framebuffer(1), W * H * 4);
  video_cb(frame, W, H * 2, W * 4);
  for (size_t n; (n = nds->spu.take(audio, 4096)) != 0;) audio_batch_cb(audio, n);
}

// States carry a length in front: DSperate's are a little different in size
// from one moment to the next, a libretro state is one fixed size.
RETRO_API size_t retro_serialize_size(void) {
  if (!state_size && nds) {
    std::string err;
    state_w.data().clear();
    if (nds->save_state(state_w, err)) state_size = 4 + state_w.data().size() + 256 * 1024;
  }
  return state_size;
}

RETRO_API bool retro_serialize(void* data, size_t size) {
  ds::state::Writer& w = state_w;
  std::string err;
  w.data().clear();
  if (!nds->save_state(w, err)) { log(RETRO_LOG_ERROR, "dsperate: state: %s\n", err.c_str()); return false; }
  const ds::u32 n = static_cast<ds::u32>(w.data().size());
  if (4 + size_t(n) > size) { log(RETRO_LOG_ERROR, "dsperate: state of %u bytes past its %zu\n", n, size); return false; }
  std::memcpy(data, &n, 4);
  std::memcpy(static_cast<ds::u8*>(data) + 4, w.data().data(), n);
  std::memset(static_cast<ds::u8*>(data) + 4 + n, 0, size - 4 - n);
  return true;
}

RETRO_API bool retro_unserialize(const void* data, size_t size) {
  ds::u32 n;
  if (size < 4) return false;
  std::memcpy(&n, data, 4);
  if (4 + size_t(n) > size) return false;
  ds::state::Reader r(static_cast<const ds::u8*>(data) + 4, n);
  std::string err;
  if (nds->load_state(r, err)) return true;
  // A failed load leaves the machine unusable: start the game over.
  log(RETRO_LOG_ERROR, "dsperate: state refused: %s\n", err.c_str());
  retro_reset();
  return false;
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

RETRO_API bool retro_load_game(const retro_game_info* game) {
  if (!game || !game->path) return false;
  retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;   // DSperate's 0xAARRGGBB, alpha ignored
  if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;
  read_options();
  rom_path = game->path;
  nds = std::make_unique<ds::NDS>();
  if (!boot()) { nds.reset(); return false; }
  load_unlisted_save();
#if DSPERATE_JIT
  if (jit_on && !ds::jit::attach(*nds, true, true)) {
    log(RETRO_LOG_WARN, "dsperate: no JIT, interpreting\n");
    jit_on = false;
  }
#endif
  state_size = 0;
  return true;
}

RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }

RETRO_API void retro_unload_game(void) {
  if (!nds) return;
#if DSPERATE_JIT
  if (jit_on) ds::jit::detach(*nds);
#endif
  nds.reset();
  state_size = 0;
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void* retro_get_memory_data(unsigned id) {
  if (!nds) return nullptr;
  if (id == RETRO_MEMORY_SAVE_RAM && nds->cart && !nds->cart->sram().empty()) return nds->cart->sram().data();
  if (id == RETRO_MEMORY_SYSTEM_RAM) return nds->bus.main_ram.get();
  return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
  if (!nds) return 0;
  if (id == RETRO_MEMORY_SAVE_RAM && nds->cart) return nds->cart->sram().size();
  if (id == RETRO_MEMORY_SYSTEM_RAM) return ds::mem::Bus::MAIN_RAM_SIZE;
  return 0;
}

} // extern "C"
