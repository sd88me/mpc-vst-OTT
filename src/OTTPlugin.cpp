// OTTmpc for MPC / Force: the native front end (framework/mpc_plugin.h) around the unchanged OTT engine (src/engine.cpp,
// Vital's multiband compressor in src/vital_dsp, compiled by src/vital_unit.cpp).
//
// Signal flow: host float audio -> ott_process_float (the engine's own code, no int16 step) -> host. The engine runs only on the
// audio thread: a host change marks the setting dirty and process() sends it before the block, so no lock is ever taken.
//
// Parameters: 1.0.1's 18 in the same order (vst/params.lock), then the appended ones (params.lock, tools/gen_params.py).
//   * 0..11 settings (kEngine): plain floats in the engine's units. Parameter 0 (DEPTH) is nudged by the host on every prepare:
//     a change to it applies once it has held for ~40 ms.
//   * 12..17 band meters (kMeter): steps 0..23 the audio thread reads from the engine; the worker tells the host when a step
//     moves. A host write (a state load) shows until the engine's step next moves, as in 1.x.
//   * programs: the MPC's PRESET menu lists 1.x's 10 factory presets (programs 0..9), then 32 user presets "U01 ..". A pick
//     applies it. The host re-picks the saved program right after a project load: ignored for 2 s after a state load, and a
//     re-pick of the current program is always ignored (1.x's rule), so edits are kept.
//   * user presets (kLocal): SAVE PRESET opens the drawn keyboard (kb_open = 1) on the current preset's name; SAVE stores the
//     settings as they were when SAVE PRESET was pressed, in the first free slot. DELETE PRESET empties the picked user preset.
//     Kept in <plugin folder>/ottmpc-presets.txt, shared by every project. A drawn key's repeat within 250 ms is ignored (one tap
//     can arrive as two presses).
// State: the framework's chunk (every parameter by id) plus "OTT2 prog=<n>" as extra state. 1.0.1 saved no state at all (its
// engine had no "state" key, so its chunk was empty): a non-"MPCP" chunk is accepted and leaves the defaults, and the program
// re-pick that follows is then applied, which brings back the factory preset that project had picked.

#include "mpc_plugin.h"

extern "C" {
#include "engine.h"   // a C header without extern "C" guards
}
#include "params_gen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" void ott_process_float(void* inst, const float* inL, const float* inR, float* outL, float* outR, int frames);
extern "C" void ott_meters(void* inst, int steps[6]);
extern "C" void ott_levels(void* inst, float db[3]);

#ifndef OTT_NAME
#define OTT_NAME "OTTmpc"
#endif
#ifndef OTT_UID
#define OTT_UID 'v', 'O', 'T', 'x'
#endif
#define OTT_FOURCC(...) MPC_FOURCC(__VA_ARGS__)   // expands OTT_UID before MPC_FOURCC counts its arguments
#ifndef OTT_VERSION
#define OTT_VERSION 2000
#endif

namespace {

static_assert(kNumMeters == 6, "the engine reports six meter steps");
static_assert(kNumEngineParams <= 32, "dirty_ is a 32-bit mask");

mpc::PluginInfo makeInfo()
{
    mpc::PluginInfo i;
    i.name = OTT_NAME;
    i.vendor = "sd88me";
    i.uniqueId = OTT_FOURCC(OTT_UID);
    i.version = OTT_VERSION;
    i.isInstrument = false;
    i.numInputs = 2;
    i.numOutputs = 2;
    return i;
}

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

thread_local int t_pushing = -1;   // the parameter the worker is telling the host about right now (its echo is ignored)

} // namespace

class OTTPlugin : public mpc::Plugin
{
public:
    OTTPlugin() : mpc::Plugin(makeInfo())
    {
        for (int i = 0; i < kNumParams; ++i)
        {
            const PSpec& s = g_pspecs[i];
            mpc::Param p = (s.flags & kChoice) ? mpc::choiceParam(s.key, s.name, std::vector<std::string>(s.choices, s.choices + s.nchoices), (int)s.def)
                           : (s.flags & (kEngine | kLevel)) ? mpc::floatParam(s.key, s.name, s.min, s.max, s.def, s.unit, s.decimals)
                                               : mpc::intParam(s.key, s.name, (int)s.min, (int)s.max, (int)s.def);
            p.automatable = !(s.flags & kNoAuto);
            addParam(p);
        }
        for (int i = 0; i < kNumFactory; ++i) addProgram(g_factory[i].name, {});
        for (int i = 0; i < kFavSlots; ++i) addProgram("", {});
        eng_ = mpc_engine();
        inst_ = eng_->create(nullptr);
        dirty_.store((1u << kNumEngineParams) - 1u);   // the table's defaults reach the engine before the first block
        // (Not mpc::contentPath(): a function-local static std::string segfaults at dlclose.)
        favFile_ = mpc::detail::computePluginFolder() + "/ottmpc-presets.txt";
        loadFavs();
        worker_ = std::thread(&OTTPlugin::workerMain, this);
    }

    ~OTTPlugin() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        if (inst_) eng_->destroy(inst_);
    }

    void prepare(double sampleRate, int) override { rate_.store((int)std::lround(sampleRate)); }

    // ---- audio ----------------------------------------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        if (!inst_ || ctx.numOutputs < 1) return;
        char buf[32];
        const int sr = rate_.load();
        if (sr > 0 && sr != engineRate_)
        {
            engineRate_ = sr;
            std::snprintf(buf, sizeof buf, "%d", sr);
            eng_->set_param(inst_, "sample_rate", buf);
        }
        uint32_t d = dirty_.exchange(0);
        for (int i = 0; d; ++i, d >>= 1)
            if (d & 1u)
            {
                std::snprintf(buf, sizeof buf, "%.6g", engineValue(i));
                eng_->set_param(inst_, g_pspecs[i].key, buf);
            }
        const float* inL = ctx.numInputs > 0 && ctx.inputs ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.numInputs > 1 && ctx.inputs ? ctx.inputs[1] : inL;
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        if (!inL)
        {
            for (int c = 0; c < ctx.numOutputs; ++c) std::fill(ctx.outputs[c], ctx.outputs[c] + ctx.numFrames, 0.0f);
            return;
        }
        ott_process_float(inst_, inL, inR, ctx.outputs[0], outR, ctx.numFrames);
        int steps[kNumMeters];
        ott_meters(inst_, steps);
        for (int k = 0; k < kNumMeters; ++k) meter_[k].store(steps[k], std::memory_order_relaxed);
        float lv[3];
        ott_levels(inst_, lv);
        for (int k = 0; k < 3; ++k) level_[k].store(lv[k], std::memory_order_relaxed);
    }

    // ---- parameters -----------------------------------------------------------------------------------------------------------
    void parameterChanged(int index, float plain) override
    {
        if (index < 0 || index >= kNumParams) return;
        if (t_pushing == index) return;          // the host echoing what the worker just told it
        const PSpec& s = g_pspecs[index];
        if (s.flags & kLocal) { localChanged(index, plain); return; }
        if (!(s.flags & kEngine)) return;        // a meter: shown until the engine's step moves
        if (index == 0 && !loading_.load())      // debounced (see the header comment)
        {
            pend0At_.store(nowMs());
            return;
        }
        dirty_.fetch_or(1u << index);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index < 0 || index >= kNumParams) return false;
        if (index == kFavName) { std::lock_guard<std::mutex> g(favMu_); out = kbName_ + "_"; return true; }
        if (g_pspecs[index].flags & kMomentary) { out = ""; return true; }
        (void)plain;
        return false;
    }

    // ---- state ----------------------------------------------------------------------------------------------------------------
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        char b[48];
        std::snprintf(b, sizeof b, "OTT2 prog=%d", currentProgram_);
        out.assign(b, b + std::strlen(b));
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        const std::string s(reinterpret_cast<const char*>(data), size);
        const size_t at = s.find("prog=");
        if (s.compare(0, 5, "OTT2 ") == 0 && at != std::string::npos)
        {
            const int prog = std::atoi(s.c_str() + at + 5);
            if (prog >= 0 && prog < kNumFactory + kFavSlots) currentProgram_ = prog;
        }
    }

    // effSetChunk (wrapped below). An "MPCP" chunk is a 2.x state: the framework restores it, and the program re-pick that
    // follows is ignored. Anything else is a 1.x project (always empty: see the header comment): accepted, defaults kept.
    VstIntPtr setChunk(AEffect* e, VstIntPtr value, void* ptr, VstIntPtr (*orig)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float))
    {
        if (ptr && value >= 4 && std::memcmp(ptr, "MPCP", 4) == 0)
        {
            loading_.store(true);
            const VstIntPtr r = orig(e, effSetChunk, 0, value, ptr, 0.0f);
            loading_.store(false);
            if (r)
            {
                pend0At_.store(0);
                dirty_.fetch_or((1u << kNumEngineParams) - 1u);
                ignoreProgUntil_.store(nowMs() + 2000);
                stateAt_.store(nowMs());
                wantDisplay_.store(true);
            }
            return r;
        }
        return 1;
    }

    // The MPC picked a program in its PRESET menu. prev = the program before the pick (the framework has stored the new one).
    void programPicked(int prev, int prog)
    {
        if (prog < 0 || prog >= kNumFactory + kFavSlots) return;
        if (nowMs() < ignoreProgUntil_.load() || prog == prev) { currentProgram_ = prev; return; }
        std::vector<float> vals;
        if (prog < kNumFactory)
            vals.assign(g_factory[prog].values, g_factory[prog].values + kNumEngineParams);
        else
        {
            std::lock_guard<std::mutex> g(favMu_);
            const Fav& f = favs_[(size_t)(prog - kNumFactory)];
            if (!f.used) return;   // an empty slot: nothing changes, but it is the picked one (DELETE PRESET does nothing)
            vals = f.values;
        }
        applyValues(vals);
    }

    std::atomic<bool> loading_{false};

private:
    float engineValue(int i) const { return std::min(std::max(get(i), g_pspecs[i].min), g_pspecs[i].max); }

    // Set the settings without calling parameterChanged; the engine gets them before the next block and the host from the worker.
    void applyValues(const std::vector<float>& v)
    {
        for (int i = 0; i < kNumEngineParams && i < (int)v.size(); ++i)
            values_[(size_t)i]->store(param(i).toNormalised(v[(size_t)i]), std::memory_order_relaxed);
        pend0At_.store(0);
        dirty_.fetch_or((1u << kNumEngineParams) - 1u);
        tellAll_.store(true);
    }

    void pushValue(int i, float plain)
    {
        values_[(size_t)i]->store(param(i).toNormalised(plain), std::memory_order_relaxed);
        t_pushing = i;
        notifyHostParameterChanged(i);
        t_pushing = -1;
    }

    void workerMain()
    {
        while (!quit_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            tickOnce(false);
        }
    }

    // One worker pass. force = the offline test: apply a pending parameter 0 now.
    void tickOnce(bool force)
    {
        const long long now = nowMs();
        long long p0 = pend0At_.load();
        if (p0 && (force || now - p0 >= 40) && pend0At_.compare_exchange_strong(p0, 0))   // a newer nudge restarts the wait
            dirty_.fetch_or(1u);
        for (int i = kFavAdd; i < kNumParams; ++i)
            if (release_[i].exchange(false)) { held_[i].store(false); pushValue(i, 0.0f); }
        if (tellAll_.exchange(false))   // a program pick: every setting moved
        {
            for (int i = 0; i < kNumEngineParams; ++i) { t_pushing = i; notifyHostParameterChanged(i); t_pushing = -1; }
            wantDisplay_.store(true);
        }
        for (int k = 0; k < kNumMeters; ++k)   // a meter step the engine moved
        {
            const int v = meter_[k].load(std::memory_order_relaxed);
            if (v == lastMeter_[k]) continue;
            lastMeter_[k] = v;
            pushValue(kFirstMeter + k, (float)v);
        }
        // band levels for the native Meter: ~30 updates a second, only when they moved by a quarter dB
        if (force || now - lastLevel_ >= 33)
        {
            lastLevel_ = now;
            for (int k = 0; k < 3; ++k)
            {
                const float v = std::min(std::max(level_[k].load(std::memory_order_relaxed), -60.0f), 0.0f);
                // peak hold: the highest level, held 1.5 s, then falling 20 dB/s (never below the level)
                if (v >= hold_[k]) { hold_[k] = v; holdAt_[k] = now; }
                else if (now - holdAt_[k] > 1500) hold_[k] = std::max(v, hold_[k] - 20.0f * (float)(now - std::max(lastHoldStep_, holdAt_[k] + 1500)) / 1000.0f);
                if (std::fabs(hold_[k] - toldHold_[k]) >= 0.25f) { toldHold_[k] = hold_[k]; pushValue(kFirstHold + k, hold_[k]); }
                if (std::fabs(v - toldLevel_[k]) < 0.25f) continue;
                toldLevel_[k] = v;
                pushValue(kFirstLevel + k, v);
            }
            lastHoldStep_ = now;
        }
        // a 2.x project saved with the keyboard open: it comes back closed (after host_sim's round-trip check has looked)
        if (!kbOpen_.load() && get(kKbOpen) >= 0.5f && now - stateAt_.load() > 500) kbDirty_.store(true);
        if (favDirty_.exchange(false)) { saveFavs(); wantDisplay_.store(true); }   // new names in the PRESET menu
        if (kbDirty_.exchange(false))   // the keyboard opened or closed, or the typed name changed
        {
            pushValue(kKbOpen, kbOpen_.load() ? 1.0f : 0.0f);
            counter_ = (counter_ + 1) % 1024;
            pushValue(kFavName, (float)counter_);
            wantDisplay_.store(true);
        }
        // at most every 100 ms, never periodically: it re-lists the programs and closes the PRESET menu
        if (wantDisplay_.load() && (force || now - lastDisplay_ >= 100))
        {
            wantDisplay_.store(false);
            lastDisplay_ = now;
            requestDisplayUpdate();
        }
    }

    // ---- user presets (kLocal) --------------------------------------------------------------------------------------------------
    struct Fav { bool used = false; std::string name; std::vector<float> values; };

    // ottmpc-presets.txt: "OTTP1", then one line per slot: "<slot>\t<name>\t<key>=<value> ..." (used slots only). Plain text so a
    // user can read or back it up; names only hold keyboard characters, so no tab or newline.
    void loadFavs()   // stdio, not iostreams: libc++'s locale statics would outlive the library at dlclose
    {
        std::lock_guard<std::mutex> g(favMu_);
        favs_.assign(kFavSlots, Fav());
        FILE* f = std::fopen(favFile_.c_str(), "r");
        char line[1024];
        if (f && std::fgets(line, sizeof line, f) && std::strncmp(line, "OTTP1", 5) == 0)
            while (std::fgets(line, sizeof line, f))
            {
                line[std::strcspn(line, "\r\n")] = 0;
                char* t1 = std::strchr(line, '\t');
                char* t2 = t1 ? std::strchr(t1 + 1, '\t') : nullptr;
                const int slot = std::atoi(line) - 1;
                if (!t2 || slot < 0 || slot >= kFavSlots) continue;
                *t2 = 0;
                Fav v;
                v.name.assign(t1 + 1, std::min(std::strlen(t1 + 1), (size_t)kFavNameMax));
                for (int i = 0; i < kNumEngineParams; ++i) v.values.push_back(g_pspecs[i].def);
                char* save = nullptr;
                for (char* tok = strtok_r(t2 + 1, " ", &save); tok; tok = strtok_r(nullptr, " ", &save))
                {
                    char* eq = std::strchr(tok, '=');
                    if (!eq) continue;
                    *eq = 0;
                    for (int i = 0; i < kNumEngineParams; ++i)
                        if (!std::strcmp(tok, g_pspecs[i].key))
                            v.values[(size_t)i] = std::min(std::max((float)std::atof(eq + 1), g_pspecs[i].min), g_pspecs[i].max);
                }
                v.used = true;
                favs_[(size_t)slot] = v;
            }
        if (f) std::fclose(f);
        nameAll();
    }

    void saveFavs()   // worker thread: a snapshot under the lock, then the file (.tmp + rename)
    {
        std::vector<Fav> snap;
        { std::lock_guard<std::mutex> g(favMu_); snap = favs_; }
        const std::string tmp = favFile_ + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        bool ok = std::fputs("OTTP1\n", f) >= 0;
        for (int s = 0; s < (int)snap.size(); ++s)
        {
            if (!snap[(size_t)s].used) continue;
            ok = ok && std::fprintf(f, "%d\t%s\t", s + 1, snap[(size_t)s].name.c_str()) > 0;
            for (int i = 0; i < kNumEngineParams; ++i)
                ok = ok && std::fprintf(f, "%s%s=%.6g", i ? " " : "", g_pspecs[i].key, (double)snap[(size_t)s].values[(size_t)i]) > 0;
            ok = ok && std::fputc('\n', f) != EOF;
        }
        ok = (std::fclose(f) == 0) && ok;
        if (ok) std::rename(tmp.c_str(), favFile_.c_str());
        else std::remove(tmp.c_str());
    }

    void nameAll() { for (int i = 0; i < kFavSlots; ++i) nameProgram(i); }

    void nameProgram(int i)   // favMu_ held
    {
        char b[8];
        std::snprintf(b, sizeof b, "U%02d ", i + 1);
        programs_[(size_t)(kNumFactory + i)].name = b + (favs_[(size_t)i].used ? favs_[(size_t)i].name : std::string("--"));
    }

    std::string currentName()   // the picked preset's name, without the "U01 " prefix
    {
        const int p = currentProgram_;
        if (p >= 0 && p < kNumFactory) return g_factory[p].name;
        std::lock_guard<std::mutex> g(favMu_);
        if (p >= kNumFactory && p < kNumFactory + kFavSlots && favs_[(size_t)(p - kNumFactory)].used) return favs_[(size_t)(p - kNumFactory)].name;
        return "";
    }

    void closeKeyboard()
    {
        if (kbOpen_.exchange(false)) kbDirty_.store(true);
    }

    void typeKey(int k)   // favMu_ held; k = 0 .. kKbCount - 1
    {
        const int nchars = (int)sizeof kKbChars - 1;
        if (k <= nchars)   // a character or space
        {
            if ((int)kbName_.size() >= kFavNameMax) return;
            char c = k < nchars ? kKbChars[k] : ' ';
            const bool cap = kbName_.empty() || kbName_.back() == ' ';
            if (c >= 'A' && c <= 'Z')
            {
                if (cap == kbShift_) c = (char)(c - 'A' + 'a');   // capital at the start and after a space; SHIFT flips the next letter
                kbShift_ = false;
            }
            kbName_ += c;
        }
        else if (k == nchars + 1) { if (!kbName_.empty()) kbName_.pop_back(); }
        else if (k == nchars + 2) kbShift_ = !kbShift_;
    }

    void localChanged(int index, float plain)
    {
        if (!(g_pspecs[index].flags & kMomentary)) return;   // kb_open / fav_name: set by the plugin only
        if (plain < 0.5f) { held_[index].store(false); return; }
        if (held_[index].exchange(true)) return;
        release_[index].store(true);   // a momentary: back to 0 on the next worker pass
        const long long now = nowMs();
        if (now - lastFire_[index] < 250) return;   // the second report of one tap
        lastFire_[index] = now;
        if (index >= kKbFirst && index < kKbFirst + kKbCount)
        {
            if (!kbOpen_.load()) return;
            const int k = index - kKbFirst, nchars = (int)sizeof kKbChars - 1;
            if (k == nchars + 3) { closeKeyboard(); return; }   // CANCEL
            if (k == nchars + 4) { saveTyped(); return; }       // SAVE
            { std::lock_guard<std::mutex> g(favMu_); typeKey(k); }
            kbDirty_.store(true);
            return;
        }
        if (index == kFavAdd)   // open the keyboard on the preset's name; the settings are taken now
        {
            std::vector<float> vals;
            for (int i = 0; i < kNumEngineParams; ++i) vals.push_back(engineValue(i));
            std::string name = currentName();
            if (name.size() > (size_t)kFavNameMax) name.resize((size_t)kFavNameMax);
            {
                std::lock_guard<std::mutex> g(favMu_);
                kbValues_ = vals;
                kbName_ = name;
                kbShift_ = false;
            }
            kbOpen_.store(true);
            kbDirty_.store(true);
            return;
        }
        if (index == kFavDel)   // the user preset that is picked now
        {
            const int slot = currentProgram_ - kNumFactory;
            if (slot < 0 || slot >= kFavSlots) return;
            bool had;
            {
                std::lock_guard<std::mutex> g(favMu_);
                had = favs_[(size_t)slot].used;
                if (had) { favs_[(size_t)slot] = Fav(); nameProgram(slot); }
            }
            if (had) favDirty_.store(true);
        }
    }

    void saveTyped()
    {
        Fav v;
        {
            std::lock_guard<std::mutex> g(favMu_);
            v.values = kbValues_;
            v.name = kbName_;
        }
        while (!v.name.empty() && v.name.back() == ' ') v.name.pop_back();
        if (v.name.empty()) v.name = "User";
        v.used = true;
        closeKeyboard();
        int slot = -1;
        {
            std::lock_guard<std::mutex> g(favMu_);
            for (int i = 0; i < kFavSlots && slot < 0; ++i) if (!favs_[(size_t)i].used) slot = i;
            if (slot >= 0) { favs_[(size_t)slot] = v; nameProgram(slot); }
        }
        if (slot < 0) return;   // all 32 used: nothing stored (DELETE PRESET frees one)
        currentProgram_ = kNumFactory + slot;   // the new preset is the picked one
        favDirty_.store(true);
    }

public:
#ifdef OTT_TEST
    void dbgTick() { tickOnce(true); }
    void dbgNoIgnore() { ignoreProgUntil_.store(0); }
    void dbgNoRepeatGuard() { std::fill(std::begin(lastFire_), std::end(lastFire_), 0LL); }
    int dbgFavCount()
    {
        std::lock_guard<std::mutex> g(favMu_);
        int n = 0;
        for (const Fav& v : favs_) n += v.used ? 1 : 0;
        return n;
    }
    std::string dbgTyped() { std::lock_guard<std::mutex> g(favMu_); return kbName_; }
#endif

private:
    const mpc_engine_t* eng_ = nullptr;
    void* inst_ = nullptr;
    std::atomic<int> rate_{0};
    int engineRate_ = 0;                        // audio thread
    std::atomic<uint32_t> dirty_{0};            // settings the engine has not had yet (bit = index)
    std::atomic<int> meter_[kNumMeters] = {};   // the engine's steps, from the audio thread
    int lastMeter_[kNumMeters] = {0, 0, 0, 0, 0, 0};
    std::atomic<float> level_[3] = {};          // band output levels (dB), from the audio thread
    float toldLevel_[3] = {-60.0f, -60.0f, -60.0f};
    float hold_[3] = {-60.0f, -60.0f, -60.0f}, toldHold_[3] = {-60.0f, -60.0f, -60.0f};
    long long holdAt_[3] = {0, 0, 0}, lastHoldStep_ = 0;
    long long lastLevel_ = 0;
    std::string favFile_;
    std::mutex favMu_;
    std::vector<Fav> favs_;
    std::atomic<bool> favDirty_{false}, tellAll_{false}, wantDisplay_{false};
    std::atomic<bool> kbOpen_{false}, kbDirty_{false};
    std::string kbName_;               // favMu_: the name being typed
    std::vector<float> kbValues_;      // favMu_: the settings SAVE PRESET took
    bool kbShift_ = false;
    std::atomic<bool> held_[kNumParams] = {};
    std::atomic<bool> release_[kNumParams] = {};
    long long lastFire_[kNumParams] = {0};
    std::thread worker_;
    std::atomic<bool> quit_{false};
    std::atomic<long long> pend0At_{0}, ignoreProgUntil_{0}, stateAt_{0};
    int counter_ = 0;
    long long lastDisplay_ = 0;
};

// The same entry as MPC_PLUGIN_ENTRY, plus a wrapped effSetChunk (1.x projects) and effSetProgram (presets, the re-pick rule).
namespace {
VstIntPtr (*g_origDispatcher)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float) = nullptr;

VstIntPtr ottDispatcher(AEffect* e, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
    OTTPlugin* p = static_cast<OTTPlugin*>(e->object);
    if (opcode == effSetChunk && p) return p->setChunk(e, value, ptr, g_origDispatcher);
    if (opcode == effSetProgram && p)
    {
        const int prev = p->currentProgram();
        const VstIntPtr r = g_origDispatcher(e, opcode, index, value, ptr, opt);
        p->programPicked(prev, (int)value);
        return r;
    }
    return g_origDispatcher(e, opcode, index, value, ptr, opt);
}
} // namespace

#ifdef OTT_TEST   // offline test hooks (tests/host_test.cpp); absent from the release build
extern "C" MPC_EXPORT void ott_dbg_tick(AEffect* e) { static_cast<OTTPlugin*>(e->object)->dbgTick(); }
extern "C" MPC_EXPORT void ott_dbg_no_ignore(AEffect* e) { static_cast<OTTPlugin*>(e->object)->dbgNoIgnore(); }
extern "C" MPC_EXPORT void ott_dbg_no_repeat_guard(AEffect* e) { static_cast<OTTPlugin*>(e->object)->dbgNoRepeatGuard(); }
extern "C" MPC_EXPORT int ott_dbg_favs(AEffect* e) { return static_cast<OTTPlugin*>(e->object)->dbgFavCount(); }
extern "C" MPC_EXPORT int ott_dbg_typed(AEffect* e, char* out, int n)
{
    const std::string s = static_cast<OTTPlugin*>(e->object)->dbgTyped();
    std::snprintf(out, (size_t)n, "%s", s.c_str());
    return (int)s.size();
}
#endif

extern "C" MPC_EXPORT AEffect* VSTPluginMain(audioMasterCallback host)
{
    if (host != nullptr && host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0) return nullptr;   // host is VST1 only
    AEffect* e = mpc::detail::createEffect(new OTTPlugin(), host);
    if (e && !g_origDispatcher) g_origDispatcher = e->dispatcher;
    if (e) e->dispatcher = ottDispatcher;
    return e;
}
