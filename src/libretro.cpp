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
#include "core/input/input_log.h"
#include "core/slot2/slot2.h"
#include "core/slot2/compat.h"
#include "core/cheat/database.h"
#include "core/cheat/ar_engine.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <algorithm>
#include <dirent.h>
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

// Slot 2 (DSperate v3.2.0): a GBA game, the Rumble Pak or the Guitar Grip,
// chosen the way DSperate's own frontend does it (main.cpp fill_slot2).
retro_rumble_interface rumble_if;
bool have_rumble;
std::string gba_path;          // the GBA game in slot 2, "" for none
std::string gba_save_path;
unsigned gba_writes_seen, rumble_seen, rumble_off_in;
bool analog_on;                // an enabled CPAD patch is being fed the sticks
std::string gba_dir_guess;
// The frontend's microphone, when it has one (plorpOS plorpos-3yv.75). Opened
// the first time the game reads the mic (Io::mic_used), so a game that never
// listens never holds the device open. Io::set_mic takes any rate.
constexpr unsigned MIC_RATE = 48000;
retro_microphone_interface mic_if;
retro_microphone_t* mic;
ds::s16 mic_buf[2048];   // a frame is ~802 samples at 48 kHz
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


// ---- Slot 2, rumble, grip, analog (DSperate v3.2.0 / v3.1.0 parity) --------

std::string opt(const char* key) {
  retro_variable v{key, nullptr};
  return env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value ? v.value : "";
}

std::string stem_of(const std::string& p) {
  std::string b = p.substr(p.find_last_of('/') + 1);
  return b.substr(0, b.find_last_of('.'));
}

ds::cheat::GameCheats analog_cheats;      // the game's analog (CPAD) codes, merged at load
std::vector<std::string> analog_names;    // their names, the dsperate_analog choices

struct GbaChoice { std::string stem, path; };
std::vector<GbaChoice> gba_choices;   // the GBA games the loaded DS game reads

// Where GBA games live: DSPERATE_GBA_DIR, else the GBA folder beside the DS
// one (plorpOS's Roms/<system>/ layout).
std::string gba_dir() {
  if (const char* e = std::getenv("DSPERATE_GBA_DIR")) return e;
  std::string d = rom_path.substr(0, rom_path.find_last_of('/'));
  return d.substr(0, d.find_last_of('/')) + "/Game Boy Advance";
}

// Only the games the DS game accepts (DSperate's own dual-slot table), as its
// frontend's picker offers them.
void scan_gba() {
  gba_choices.clear();
  if (!nds || !nds->cart || nds->dsi) return;
  const ds::slot2::Linkage* link = ds::slot2::linkage_for(nds->cart->header().game_code);
  if (!link) return;
  const std::string dir = gba_dir();
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (const dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() < 5 || strcasecmp(n.c_str() + n.size() - 4, ".gba") != 0) continue;
    const std::string stem = n.substr(0, n.size() - 4);
    if (stem.find_first_of("|;") != std::string::npos) continue;   // would break the option list
    char code[5], title[13];
    if (ds::slot2::peek_game(dir + "/" + n, code, title) && ds::slot2::accepts(*link, code))
      gba_choices.push_back({stem, dir + "/" + n});
  }
  closedir(d);
  std::sort(gba_choices.begin(), gba_choices.end(), [](const GbaChoice& a, const GbaChoice& b) { return a.stem < b.stem; });
}

// The option list. The GBA game's values are only known once the DS game is
// (after load), and a frontend accepts a stored choice only if it is offered,
// so this is declared again before the choice is read.
void declare_options() {
  static std::string gba_decl;
  gba_decl = "GBA game in slot 2 (restart); none";
  for (const GbaChoice& c : gba_choices) gba_decl += "|" + c.stem;
  static std::string analog_decl;
  analog_decl = "Analog (CPAD) patch for the sticks (restart); off";
  for (const std::string& n : analog_names) analog_decl += "|" + n;
  const retro_variable vars[] = {
    {"dsperate_jit", "JIT recompiler (restart); enabled|disabled"},
    {"dsperate_frameskip", "Frameskip, when the frontend asks for it (restart); auto|disabled"},
    {"dsperate_slot2", "Slot 2 without a GBA game (restart); auto|rumble pak|empty"},
    {"dsperate_gba_game", gba_decl.c_str()},
    {"dsperate_guitar_grip", "Guitar Grip for Guitar Hero (restart); auto|off"},
    {"dsperate_solar", "GBA solar sensor light (Boktai); 5|0|1|2|3|4|6|7|8|9|10"},
    {"dsperate_analog", analog_decl.c_str()},
    {nullptr, nullptr},
  };
  env_cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(vars));
}

void fill_slot2() {
  nds->slot2.reset();
  gba_path.clear();
  if (nds->dsi || !nds->cart) return;
  const std::string sel = opt("dsperate_gba_game");
  if (!sel.empty() && sel != "none") {
    for (const GbaChoice& c : gba_choices) {
      if (c.stem != sel) continue;
      const std::string save_dir = dir(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY);
      const std::string sav = (save_dir.empty() ? c.path.substr(0, c.path.find_last_of('.')) : save_dir + "/" + c.stem) + ".sav";
      std::string err;
      if (auto g = ds::slot2::load_game(c.path, sav, err)) {
        g->set_light_level(std::atoi(opt("dsperate_solar").c_str()));
        log(RETRO_LOG_INFO, "dsperate: slot 2: %s [%s], save %s\n", c.stem.c_str(), g->game_code(), ds::slot2::save_type_name(g->save_type()));
        gba_path = sav;
        nds->slot2 = std::move(g);
        return;
      }
      log(RETRO_LOG_ERROR, "dsperate: slot 2: %s\n", err.c_str());
    }
  }
  if (opt("dsperate_guitar_grip") != "off" && ds::slot2::uses_guitar_grip(nds->cart->header().game_code)) {
    nds->slot2 = std::make_unique<ds::slot2::GuitarGrip>();
    log(RETRO_LOG_INFO, "dsperate: slot 2: Guitar Grip\n");
    return;
  }
  const std::string m = opt("dsperate_slot2");
  if (m == "rumble pak" || (m != "empty" && have_rumble)) {
    nds->slot2 = std::make_unique<ds::slot2::RumblePak>();
    log(RETRO_LOG_INFO, "dsperate: slot 2: Rumble Pak\n");
  }
}

// Analog patches (shocoman's CPAD codes, built in). A game's catalogue entry
// is merged into the cheat list at load (scan_analog) so its codes can be
// offered by name as dsperate_analog; the chosen one is enabled, which
// installs its hook, and the frame's sticks go to guest RAM while it is on
// (input::apply). Off unless a code is chosen: DSperate's own frontend makes
// the player enable one in its Analog Cheats menu, and on a handheld whose
// stick is also the pen cursor the patch and the cursor would both move.
void scan_analog() {
  analog_cheats = {};
  analog_names.clear();
  if (!nds || !nds->cart || nds->dsi) return;
  ds::u8 header[512] = {};
  nds->cart->rom_read(0, header, sizeof header);
  ds::cheat::GameCheats analog;
  std::string err;
  if (!ds::cheat::load_analog_for_header("", header, analog, err)) return;
  if (!ds::cheat::merge_analog(analog_cheats, std::move(analog), ds::cheat::header_checksum(header))) return;
  for (const ds::cheat::Code& c : analog_cheats.codes)
    if (c.analog && !c.is_note() && c.name.find_first_of("|;") == std::string::npos) analog_names.push_back(c.name);
}

void setup_analog() {
  analog_on = false;
  const std::string sel = opt("dsperate_analog");
  if (analog_names.empty() || sel.empty() || sel == "off") return;
  for (ds::cheat::Code& c : analog_cheats.codes) c.enabled = c.analog && c.name == sel;
  nds->cheats.codes = analog_cheats.codes;
  const ds::cheat::AnalogRequest req = ds::cheat::analog_request(nds->cheats.codes);
  if (req.schemes != 1) return;
  nds->analog_patch = true;
  nds->analog_full = req.nub;
  nds->analog_layout = req.extended ? ds::NDS::AnalogLayout::Extended : ds::NDS::AnalogLayout::Packed;
  analog_on = true;
  log(RETRO_LOG_INFO, "dsperate: analog patch \"%s\"%s\n", req.name ? req.name : sel.c_str(), req.nub ? " (nub + ZL/ZR)" : "");
}

void flush_gba_save() {
  const ds::slot2::GameCart* g = nds ? nds->gba_cart() : nullptr;
  if (!g || !g->save_dirty() || gba_path.empty()) return;
  std::string err;
  if (ds::slot2::write_save(*g, gba_path, err)) const_cast<ds::slot2::GameCart*>(g)->clear_save_dirty();
  else log(RETRO_LOG_ERROR, "dsperate: GBA save: %s\n", err.c_str());
}

} // namespace

extern "C" {

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb) {
  env_cb = cb;
  declare_options();
  retro_log_callback l{};
  if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &l)) log_cb = l.log;
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t) {}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

// Frame skip, driven by the frontend (plorpOS TortOS plorpos-reo.21.7; the
// design is nelynes's slotPlus, https://github.com/nelynes/slotPlus,
// cores/dsperate/wrapper-0001-frameskip.patch, suggested by crobledo).
// With dsperate_frameskip = auto the frontend says, through the audio buffer
// status callback, which frames it would rather not have drawn (its underrun
// flag), and DSperate's adaptive frame skip skips whole display periods while
// it does - at most FS_LIMIT in a row, then one is drawn. Only the 3D raster
// and line output are left out: the game, both CPUs and the SOUND run
// unchanged, so audio is never touched by a skip. DSperate decides one frame
// ahead, so a skip asked for now lands on the next frame, and the frame the
// frontend presents then is the last one drawn (video_cb(nullptr)).
// A frontend that never calls the callback (plorpOS: every diatom port except
// the Brick and H700) never skips.
constexpr int FS_LIMIT = 2;
bool fs_auto = true;
bool fs_behind, fs_in_skip;
int fs_left, fs_blocks;
void RETRO_CALLCONV fs_status(bool active, unsigned, bool underrun) { fs_behind = active && underrun; }

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
  ds::input::Frame in;
  ds::u32 pressed = 0;
  for (const auto& m : map)
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, m.id)) pressed |= 1u << m.btn;
  // The Guitar Grip's frets are the shoulders and triggers (DSperate's
  // defaults: L, L2, R2, R = green, red, yellow, blue); they are not the DS's
  // own L and R while the grip is in.
  if (nds->slot2 && nds->slot2->kind() == ds::slot2::Kind::GuitarGrip) {
    static const unsigned fret_id[4] = {RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_L2,
                                        RETRO_DEVICE_ID_JOYPAD_R2, RETRO_DEVICE_ID_JOYPAD_R};
    for (int i = 0; i < 4; ++i)
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, fret_id[i])) in.frets |= 1u << i;
    pressed &= ~((1u << ds::io::Io::BTN_L) | (1u << ds::io::Io::BTN_R));
  }
  in.buttons = static_cast<ds::u16>(pressed);

  // The pointer spans the whole frame, the two screens stacked: the bottom
  // (touch) screen is y 0..0x7fff of -0x7fff..0x7fff.
  const bool down = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
  const int px = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
  const int py = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
  if (down && py >= 0) {
    in.down = true;
    in.x = static_cast<ds::u8>((px + 0x7fff) * int(W) / 0xffff);
    in.y = static_cast<ds::u8>(py * int(H) / 0x8000);
  }

  // Both sticks and the triggers, for an analog (CPAD) patch; the core only
  // forwards them while one is on. Libretro's axes are the SDL range.
  if (analog_on) {
    in.cpad_x = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    in.cpad_y = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    in.cstick_x = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
    in.cstick_y = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
    in.zl = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
    in.zr = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);
  }

  if (!mic && mic_if.open_mic && nds->io.mic_used()) {
    retro_microphone_params_t p{MIC_RATE};
    mic = mic_if.open_mic(&p);
    if (mic && !mic_if.set_mic_state(mic, true)) { mic_if.close_mic(mic); mic = nullptr; }
    if (!mic) mic_if.open_mic = nullptr;   // once: not again every frame
    else log(RETRO_LOG_INFO, "dsperate: microphone on\n");
  }
  if (mic) {
    const int n = mic_if.read_mic(mic, mic_buf, sizeof mic_buf / sizeof mic_buf[0]);
    ds::input::apply(*nds, in, n > 0 ? mic_buf : nullptr, n > 0 ? size_t(n) : 0);
  } else {
    ds::input::apply(*nds, in);
  }

  if (fs_auto) {
    if (fs_left == 0) {   // a block ended: pick the next one
      const bool skip = fs_behind && fs_blocks < FS_LIMIT;
      fs_blocks = skip ? fs_blocks + 1 : 0;
      fs_in_skip = skip;
      fs_left = nds->gpu.display_phase_period();
    }
    --fs_left;
    nds->gpu.set_frame_skip(fs_in_skip);
  } else {
    nds->gpu.set_frame_skip(false);
  }
  nds->run_frame();

  // The Rumble Pak's motor line: any change is a short pulse (40 ms = 3
  // frames), DSperate's own frontend does the same.
  if (nds->slot2 && have_rumble) {
    if (nds->slot2->rumble_events() != rumble_seen) {
      rumble_seen = nds->slot2->rumble_events();
      rumble_if.set_rumble_state(0, RETRO_RUMBLE_STRONG, 0xffff);
      rumble_off_in = 3;
    } else if (rumble_off_in && --rumble_off_in == 0) {
      rumble_if.set_rumble_state(0, RETRO_RUMBLE_STRONG, 0);
    }
  }
  // A GBA game's save goes to its file once it has been quiet a second.
  if (const ds::slot2::GameCart* g = nds->gba_cart(); g && g->save_dirty()) {
    static unsigned quiet;
    if (g->save_writes() != gba_writes_seen) { gba_writes_seen = g->save_writes(); quiet = 0; }
    else if (++quiet >= 60) { flush_gba_save(); quiet = 0; }
  }

  if (nds->gpu.will_skip_frame()) {
    video_cb(nullptr, W, H * 2, W * 4);   // nothing new drawn: show the last frame again
  } else {
    std::memcpy(frame, nds->gpu.framebuffer(0), W * H * 4);
    std::memcpy(frame + W * H, nds->gpu.framebuffer(1), W * H * 4);
    video_cb(frame, W, H * 2, W * 4);
  }
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

// RetroAchievements' DS map (rcheevos consoleinfo.c): main RAM, then the
// ARM9's DTCM at the pseudo-address 0x0E000000, as melonDS DS lays it out.
// Without it the frontend reaches main RAM alone and DTCM sets are disabled.
// The buffers live as long as nds, so once per game is enough.
static void register_memory_map() {
  const size_t ram = nds->bus.main_ram_size();
  retro_memory_descriptor d[2] = {};
  d[0].flags = RETRO_MEMDESC_SYSTEM_RAM;
  d[0].ptr = nds->bus.main_ram.get();
  d[0].start = 0x02000000;
  d[0].select = 0xFF000000;
  d[0].disconnect = 0x00FFFFFF & ~(ram - 1);   // a DS's 4 MB mirrors across the 16 MB window
  d[0].len = ram;
  d[1].flags = RETRO_MEMDESC_SYSTEM_RAM;
  d[1].ptr = nds->bus.dtcm.get();
  d[1].start = 0x0E000000;
  d[1].len = ds::mem::Bus::DTCM_SIZE;
  retro_memory_map map = {d, 2};
  if (!env_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map))
    log(RETRO_LOG_WARN, "dsperate: frontend refused the memory map\n");
}

RETRO_API bool retro_load_game(const retro_game_info* game) {
  if (!game || !game->path) return false;
  retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;   // DSperate's 0xAARRGGBB, alpha ignored
  if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;
  rom_path = game->path;
  nds = std::make_unique<ds::NDS>();
  // The options are read after the GBA games this DS game reads are known and
  // listed (a stored choice is only accepted if it is offered).
  rumble_if = {};
  have_rumble = env_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble_if) && rumble_if.set_rumble_state;
  if (!boot()) { nds.reset(); return false; }
  scan_gba();
  scan_analog();
  declare_options();
  read_options();
  fs_auto = opt("dsperate_frameskip") != "disabled";
  fill_slot2();   // before anything runs: a game looks for slot 2 in its first frames
  setup_analog();
  rumble_seen = nds->slot2 ? nds->slot2->rumble_events() : 0;
  rumble_off_in = 0;
  gba_writes_seen = 0;
  load_unlisted_save();
  register_memory_map();
#if DSPERATE_JIT
  if (jit_on && !ds::jit::attach(*nds, true, true)) {
    log(RETRO_LOG_WARN, "dsperate: no JIT, interpreting\n");
    jit_on = false;
  }
#endif
  state_size = 0;
  fs_behind = fs_in_skip = false;
  fs_left = fs_blocks = 0;
  if (fs_auto) {
    nds->gpu.set_frameskip_capture(true);
    retro_audio_buffer_status_callback cb{fs_status};
    if (!env_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, &cb)) fs_auto = false;
  }
  mic_if = {};
  mic_if.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;
  if (!env_cb(RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE, &mic_if)) mic_if = {};
  return true;
}

RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }

RETRO_API void retro_unload_game(void) {
  if (mic) { mic_if.close_mic(mic); mic = nullptr; }
  if (!nds) return;
  flush_gba_save();
  if (have_rumble) rumble_if.set_rumble_state(0, RETRO_RUMBLE_STRONG, 0);
  gba_choices.clear();
  analog_names.clear();
  analog_cheats = {};
  gba_path.clear();
  analog_on = false;
#if DSPERATE_JIT
  if (jit_on) ds::jit::detach(*nds);
#endif
  nds.reset();
  state_size = 0;
  // The frontend may stay up between games: don't keep a state's 6 MB.
  std::vector<ds::u8>().swap(state_w.data());
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
