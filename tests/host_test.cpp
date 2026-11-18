// Offline host test for OTTmpc 2 (tests/run.sh: ASan/UBSan, x86). The plugin, the engine and Vital's DSP are linked in; this file
// drives VSTPluginMain the way the MPC does. Must print OK.
//   identity      name, uid, effect, the frozen parameter order (vst/params.lock first, then ./params.lock), 10 + 32 programs
//   audio         a sine through it: finite, louder at DEPTH 100 than at 0 (upward compression), OUT GAIN works, meters move
//   other rates   48 kHz runs and stays finite
//   state         every setting and the picked program survive effGetChunk -> a new instance -> effSetChunk
//   re-pick       the host's program re-pick right after a state load is ignored; a later pick applies; re-picking the current
//                 program keeps edits (1.x's rule)
//   1.0.1 project an empty / non-MPCP chunk is accepted, leaves the defaults, and the program re-pick then applies the preset
//   user presets  SAVE PRESET -> keyboard (default name, typing, length cap, DELETE, CANCEL) -> SAVE: a "U01 <name>" program,
//                 the file, a new instance lists it, a pick applies it, DELETE PRESET empties it
//   parameter 0   the host's nudge is debounced, but a held change applies

#include "mpc_vst2_abi.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback host);
extern "C" void ott_dbg_tick(AEffect* e);
extern "C" void ott_dbg_no_ignore(AEffect* e);
extern "C" void ott_dbg_no_repeat_guard(AEffect* e);
extern "C" int ott_dbg_favs(AEffect* e);
extern "C" int ott_dbg_typed(AEffect* e, char* out, int n);

#include "params_gen.h"

#ifndef OTT_EXPECT_NAME
#define OTT_EXPECT_NAME "OTTmpc"
#endif
#ifndef OTT_EXPECT_UID
#define OTT_EXPECT_UID 0x764f5478   // 'vOTx'
#endif

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float)
{
    return op == audioMasterVersion ? 2400 : 0;
}

static VstIntPtr call(AEffect* e, VstInt32 op, VstInt32 idx = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0.0f)
{
    return e->dispatcher(e, op, idx, v, p, o);
}

static int find(const char* key)
{
    for (int i = 0; i < kNumParams; ++i) if (!std::strcmp(g_pspecs[i].key, key)) return i;
    std::printf("no param %s\n", key);
    std::exit(1);
}

static float norm(int i, float plain) { return (plain - g_pspecs[i].min) / (g_pspecs[i].max - g_pspecs[i].min); }
static float plainOf(AEffect* e, int i) { return g_pspecs[i].min + e->getParameter(e, i) * (g_pspecs[i].max - g_pspecs[i].min); }
static void setPlain(AEffect* e, const char* key, float v) { const int i = find(key); e->setParameter(e, i, norm(i, v)); }

static AEffect* open(double rate = 44100.0)
{
    AEffect* e = VSTPluginMain(host);
    if (!e) { std::printf("VSTPluginMain failed\n"); std::exit(1); }
    call(e, effOpen);
    call(e, effSetSampleRate, 0, 0, nullptr, (float)rate);
    call(e, effSetBlockSize, 0, 128);
    call(e, effMainsChanged, 0, 1);
    return e;
}

static void close(AEffect* e) { call(e, effMainsChanged, 0, 0); call(e, effClose); }

// n frames of a stereo sine at amplitude a; returns the output RMS of the last half
static double run(AEffect* e, int frames, float a, double rate = 44100.0, float freq = 1000.0f)
{
    std::vector<float> l(128), r(128), ol(128), orr(128);
    float* in[2] = {l.data(), r.data()};
    float* out[2] = {ol.data(), orr.data()};
    double sum = 0.0;
    long cnt = 0;
    static double ph = 0.0;
    for (int done = 0; done < frames; done += 128)
    {
        for (int i = 0; i < 128; ++i) { l[(size_t)i] = r[(size_t)i] = a * (float)std::sin(ph); ph += 2.0 * M_PI * freq / rate; }
        e->processReplacing(e, in, out, 128);
        for (int i = 0; i < 128; ++i)
        {
            if (!std::isfinite(ol[(size_t)i]) || !std::isfinite(orr[(size_t)i])) { std::printf("FAIL: non-finite output\n"); std::exit(1); }
            if (done >= frames / 2) { sum += (double)ol[(size_t)i] * ol[(size_t)i]; ++cnt; }
        }
    }
    return cnt ? std::sqrt(sum / (double)cnt) : 0.0;
}

static double db(double x) { return 20.0 * std::log10(x > 1e-12 ? x : 1e-12); }

static std::string progName(AEffect* e, int i)
{
    char b[kVstMaxProgNameLen + 8] = {};
    call(e, effGetProgramNameIndexed, i, 0, b);
    return b;
}

static std::vector<uint8_t> chunk(AEffect* e)
{
    void* p = nullptr;
    const VstIntPtr n = call(e, effGetChunk, 0, 0, &p);
    return n > 0 && p ? std::vector<uint8_t>((uint8_t*)p, (uint8_t*)p + n) : std::vector<uint8_t>();
}

static std::vector<std::string> lines(const std::string& path)
{
    std::vector<std::string> v;
    std::ifstream f(path);
    for (std::string s; std::getline(f, s);) if (!s.empty() && s[0] != '#') v.push_back(s);
    return v;
}

static void tap(AEffect* e, int i)   // a momentary key as the skin sends it
{
    e->setParameter(e, i, 1.0f);
    ott_dbg_tick(e);
    e->setParameter(e, i, 0.0f);
    ott_dbg_no_repeat_guard(e);
}

static void type(AEffect* e, const char* s)
{
    for (; *s; ++s)
    {
        const char* at = std::strchr(kKbChars, *s >= 'a' && *s <= 'z' ? *s - 'a' + 'A' : *s);
        tap(e, *s == ' ' ? kKbFirst + (int)sizeof kKbChars - 1 : kKbFirst + (int)(at - kKbChars));
    }
}

static const int kDel = kKbFirst + (int)sizeof kKbChars, kCancel = kKbFirst + (int)sizeof kKbChars + 2, kSave = kKbFirst + (int)sizeof kKbChars + 3;

int main(int argc, char** argv)
{
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string folder = std::getenv("MPC_PLUGIN_FOLDER") ? std::getenv("MPC_PLUGIN_FOLDER") : ".";
    std::remove((folder + "/ottmpc-presets.txt").c_str());

    // ---- identity ----
    {
        AEffect* e = open();
        char b[256] = {};
        call(e, effGetEffectName, 0, 0, b);
        CHECK(!std::strcmp(b, OTT_EXPECT_NAME), "name %s", b);
        CHECK(e->uniqueID == OTT_EXPECT_UID, "uid %08x", (unsigned)e->uniqueID);
        CHECK(!(e->flags & effFlagsIsSynth) && e->numInputs == 2 && e->numOutputs == 2, "not a stereo effect");
        CHECK(call(e, effGetPlugCategory) == kPlugCategEffect, "category");
        const std::vector<std::string> released = lines(root + "/vst/params.lock"), all = lines(root + "/params.lock");
        CHECK(released.size() == 18, "vst/params.lock has %zu keys", released.size());
        CHECK(all.size() == (size_t)kNumParams && e->numParams == kNumParams, "params.lock %zu, plugin %d", all.size(), e->numParams);
        for (size_t i = 0; i < all.size() && i < (size_t)kNumParams; ++i)
        {
            CHECK(all[i] == g_pspecs[i].key, "index %zu: lock %s, table %s", i, all[i].c_str(), g_pspecs[i].key);
            if (i < released.size()) CHECK(released[i] == all[i], "index %zu moved: %s", i, all[i].c_str());
        }
        CHECK(e->numPrograms == kNumFactory + kFavSlots, "programs %d", e->numPrograms);
        CHECK(progName(e, 0) == "OTT Default" && progName(e, 9) == "Lo-Fi Squash", "factory names %s / %s", progName(e, 0).c_str(), progName(e, 9).c_str());
        CHECK(progName(e, 10) == "U01 --" && progName(e, 41) == "U32 --", "user slots %s", progName(e, 10).c_str());
        for (int i = 0; i < kNumEngineParams; ++i)
            CHECK(std::fabs(plainOf(e, i) - g_pspecs[i].def) < 0.01f, "%s default %g", g_pspecs[i].key, plainOf(e, i));
        close(e);
    }

    // ---- audio ----
    {
        AEffect* e = open();
        const double on = run(e, 44100, 0.01f);
        setPlain(e, "depth", 0.0f);
        const double off = run(e, 44100, 0.01f);
        std::printf("  -40 dBFS sine: depth 100 -> %.1f dB, depth 0 -> %.1f dB\n", db(on), db(off));
        CHECK(db(on) > db(off) + 3.0, "upward compression did not lift a quiet signal");
        setPlain(e, "depth", 100.0f);
        setPlain(e, "out_gain", -30.0f);
        ott_dbg_tick(e);
        const double low = run(e, 44100, 0.01f);
        CHECK(std::fabs(db(on) - 30.0 - db(low)) < 2.0, "out gain -30: %.1f vs %.1f", db(low), db(on));
        setPlain(e, "out_gain", 0.0f);
        run(e, 22050, 0.5f, 44100.0, 5000.0f);   // loud, in the high band
        ott_dbg_tick(e);
        const float hout = plainOf(e, find("h_out")), lout = plainOf(e, find("l_out"));
        std::printf("  meters after a loud 5 kHz sine: high out %g, low out %g\n", hout, lout);
        CHECK(hout >= 8 && hout > lout + 4, "meters did not follow");
        const float hl = plainOf(e, find("h_level")), ll = plainOf(e, find("l_level"));
        std::printf("  levels: high %.1f dB, low %.1f dB\n", hl, ll);
        CHECK(hl > -40.0f && hl > ll + 10.0f, "levels did not follow");
        const float hh = plainOf(e, find("h_hold"));
        CHECK(hh >= hl - 0.5f, "peak hold %.1f below the level %.1f", hh, hl);
        run(e, 2205, 0.0f);   // silence: the hold stays for 1.5 s, then falls
        ott_dbg_tick(e);
        CHECK(std::fabs(plainOf(e, find("h_hold")) - hh) < 0.5f, "peak hold fell at once");
        std::this_thread::sleep_for(std::chrono::milliseconds(2600));
        run(e, 2205, 0.0f);
        ott_dbg_tick(e);
        std::printf("  peak hold %.1f dB, after 2.6 s of silence %.1f dB\n", hh, plainOf(e, find("h_hold")));
        CHECK(plainOf(e, find("h_hold")) < hh - 5.0f && plainOf(e, find("h_hold")) >= plainOf(e, find("h_level")) - 0.5f,
              "peak hold did not fall, or fell below the level (%.1f, level %.1f)", plainOf(e, find("h_hold")), plainOf(e, find("h_level")));
        close(e);
    }
    {
        AEffect* e = open(48000.0);
        const double r = run(e, 48000, 0.05f, 48000.0);
        CHECK(r > 0.001 && r < 10.0, "48 kHz level %g", r);
        close(e);
    }

    // ---- state ----
    std::vector<uint8_t> saved;
    {
        AEffect* e = open();
        call(e, effSetProgram, 0, 3);   // Parallel Smash, then edits
        ott_dbg_tick(e);
        CHECK(std::fabs(plainOf(e, find("mix")) - g_factory[3].values[find("mix")]) < 0.01f, "program 3 not applied");
        setPlain(e, "time", 37.0f);
        setPlain(e, "low_xover", 333.0f);
        setPlain(e, "in_gain", -7.5f);
        saved = chunk(e);
        CHECK(saved.size() > 16 && !std::memcmp(saved.data(), "MPCP", 4), "chunk %zu bytes", saved.size());
        close(e);

        AEffect* f = open();
        CHECK(call(f, effSetChunk, 0, (VstIntPtr)saved.size(), saved.data()) == 1, "effSetChunk refused");
        call(f, effSetProgram, 0, 3);   // the host's re-pick right after the load
        ott_dbg_tick(f);
        CHECK(std::fabs(plainOf(f, find("time")) - 37.0f) < 0.1f, "time %g after reload + re-pick", plainOf(f, find("time")));
        CHECK(std::fabs(plainOf(f, find("low_xover")) - 333.0f) < 1.0f, "low_xover %g", plainOf(f, find("low_xover")));
        CHECK(std::fabs(plainOf(f, find("in_gain")) + 7.5f) < 0.1f, "in_gain %g", plainOf(f, find("in_gain")));
        CHECK(call(f, effGetProgram) == 3, "program %ld after reload", (long)call(f, effGetProgram));
        call(f, effSetProgram, 0, 0);   // inside the 2 s window: still ignored
        ott_dbg_tick(f);
        CHECK(std::fabs(plainOf(f, find("time")) - 37.0f) < 0.1f, "a pick inside the window applied");
        ott_dbg_no_ignore(f);
        call(f, effSetProgram, 0, 3);   // the current program again: edits kept (1.x's rule)
        CHECK(std::fabs(plainOf(f, find("time")) - 37.0f) < 0.1f, "re-picking the current program dropped edits");
        call(f, effSetProgram, 0, 1);   // a real pick
        ott_dbg_tick(f);
        CHECK(std::fabs(plainOf(f, find("depth")) - g_factory[1].values[0]) < 0.1f && std::fabs(plainOf(f, find("time")) - g_factory[1].values[1]) < 0.1f,
              "program 1 not applied after the window");
        run(f, 4410, 0.05f);
        close(f);
    }

    // ---- a 1.0.1 project ----
    {
        AEffect* e = open();
        char empty = 0;
        CHECK(call(e, effSetChunk, 0, 0, &empty) == 1, "an empty chunk was refused");
        const char junk[] = "not an OTT2 state";
        CHECK(call(e, effSetChunk, 0, sizeof junk, (void*)junk) == 1, "a non-MPCP chunk was refused");
        for (int i = 0; i < kNumEngineParams; ++i) CHECK(std::fabs(plainOf(e, i) - g_pspecs[i].def) < 0.01f, "%s changed", g_pspecs[i].key);
        call(e, effSetProgram, 0, 6);   // the re-pick of the project's preset (Drum Bus): applied
        ott_dbg_tick(e);
        for (int i = 0; i < kNumEngineParams; ++i)
            CHECK(std::fabs(plainOf(e, i) - g_factory[6].values[i]) < 0.05f, "%s = %g, Drum Bus has %g", g_pspecs[i].key, plainOf(e, i), g_factory[6].values[i]);
        close(e);
    }

    // ---- user presets ----
    {
        AEffect* e = open();
        call(e, effSetProgram, 0, 2);   // Gentle Glue, edited
        ott_dbg_tick(e);
        setPlain(e, "mix", 42.0f);
        char b[64];
        tap(e, kFavAdd);
        ott_dbg_tick(e);
        CHECK(e->getParameter(e, kKbOpen) > 0.5f, "keyboard did not open");
        ott_dbg_typed(e, b, sizeof b);
        CHECK(!std::strcmp(b, "Gentle Glue"), "default name %s", b);
        setPlain(e, "mix", 99.0f);   // edits while typing are not saved
        for (int i = 0; i < 11; ++i) tap(e, kDel);
        type(e, "my drums");
        ott_dbg_typed(e, b, sizeof b);
        CHECK(!std::strcmp(b, "My Drums"), "typed %s", b);
        type(e, " 1234567890123");   // over the 20-character cap
        ott_dbg_typed(e, b, sizeof b);
        CHECK(std::strlen(b) == (size_t)kFavNameMax, "cap: %s", b);
        for (int i = 0; i < 12; ++i) tap(e, kDel);
        tap(e, kSave);
        ott_dbg_tick(e);
        CHECK(e->getParameter(e, kKbOpen) < 0.5f, "keyboard still open");
        CHECK(ott_dbg_favs(e) == 1, "%d user presets", ott_dbg_favs(e));
        CHECK(progName(e, kNumFactory) == "U01 My Drums", "program name %s", progName(e, kNumFactory).c_str());
        CHECK(call(e, effGetProgram) == kNumFactory, "picked %ld", (long)call(e, effGetProgram));
        std::ifstream f(folder + "/ottmpc-presets.txt");
        std::string first, second;
        std::getline(f, first);
        std::getline(f, second);
        CHECK(first == "OTTP1" && second.rfind("1\tMy Drums\tdepth=", 0) == 0 && second.find(" mix=42 ") != std::string::npos, "file: %s / %s", first.c_str(), second.c_str());
        tap(e, kFavAdd);   // CANCEL stores nothing
        tap(e, kCancel);
        ott_dbg_tick(e);
        CHECK(ott_dbg_favs(e) == 1 && e->getParameter(e, kKbOpen) < 0.5f, "cancel");
        tap(e, kSave);     // keys do nothing with the keyboard closed
        CHECK(ott_dbg_favs(e) == 1, "SAVE with the keyboard closed stored a preset");
        close(e);

        AEffect* g = open();   // another instance, as another project
        CHECK(progName(g, kNumFactory) == "U01 My Drums", "new instance: %s", progName(g, kNumFactory).c_str());
        call(g, effSetProgram, 0, kNumFactory);
        ott_dbg_tick(g);
        CHECK(std::fabs(plainOf(g, find("mix")) - 42.0f) < 0.1f, "user preset mix %g", plainOf(g, find("mix")));
        CHECK(std::fabs(plainOf(g, find("depth")) - g_factory[2].values[0]) < 0.1f, "user preset depth %g", plainOf(g, find("depth")));
        call(g, effSetProgram, 0, kNumFactory + 5);   // an empty slot: nothing changes
        ott_dbg_tick(g);
        CHECK(std::fabs(plainOf(g, find("mix")) - 42.0f) < 0.1f, "an empty slot changed settings");
        tap(g, kFavDel);   // nothing to delete there
        CHECK(ott_dbg_favs(g) == 1, "DELETE PRESET on an empty slot");
        call(g, effSetProgram, 0, kNumFactory);
        tap(g, kFavDel);
        ott_dbg_tick(g);
        CHECK(ott_dbg_favs(g) == 0 && progName(g, kNumFactory) == "U01 --", "delete: %d, %s", ott_dbg_favs(g), progName(g, kNumFactory).c_str());
        call(g, effSetProgram, 0, 4);
        tap(g, kFavDel);   // a factory preset can't be deleted
        CHECK(progName(g, 4) == "Upward Only", "factory preset renamed");
        close(g);
        AEffect* h = open();
        CHECK(progName(h, kNumFactory) == "U01 --", "deleted preset came back");
        close(h);
    }

    // ---- parameter 0 ----
    {
        AEffect* e = open();
        const double before = run(e, 22050, 0.01f);
        e->setParameter(e, 0, 0.0f);   // DEPTH 0, held: applies after the debounce
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const double after = run(e, 22050, 0.01f);
        CHECK(db(before) > db(after) + 3.0, "a held DEPTH change did not apply (%.1f -> %.1f dB)", db(before), db(after));
        close(e);
    }

    std::remove((folder + "/ottmpc-presets.txt").c_str());
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("OK\n");
    return 0;
}
