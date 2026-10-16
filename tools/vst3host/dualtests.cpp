// Dual NAM A2 Full -- automated test suite.
//
// Loads the BUILT .vst3 through the VST3 hosting API and drives it like a DAW:
// bus arrangements, process(), parameter automation, getState/setState. Models
// and IRs reach the plugin the same way a saved project does: through setState.
//
//   dualtests <DualNAM_A2_Full.vst3> <repo-root> [--quick]
//
// Prints one PASS/FAIL line per check and a summary. Exit code = failures.

#include "host.h"
#include "heaphook.h"

#include "dsp/RecursiveLinearFilter.h"

#define NOMINMAX
#include <Windows.h>
#include <avrt.h>

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <thread>

namespace fs = std::filesystem;

static int gFail = 0, gPass = 0;
static void Check(bool ok, const char* fmt, ...)
{
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", buf);
  std::fflush(stdout);
  (ok ? gPass : gFail)++;
}
static void Section(const char* name)
{
  std::printf("\n== %s\n", name);
  std::fflush(stdout);
}

static std::string gPlugPath;
static fs::path gModels, gGen;
static bool gQuick = false;
static std::string gOnly; // ",name,name," -- empty runs everything

static std::string M(const char* name) { return (gModels / name).string(); }
static std::string G(const char* name) { return (gGen / name).string(); }

// ── Signals ──────────────────────────────────────────────────────────────────

using Buf = std::vector<double>;

static Buf GuitarSignal(int n, double sr, double level = 0.25, unsigned seed = 7u)
{
  Buf x((size_t)n);
  for (int i = 0; i < n; i++)
  {
    const double t = i / sr;
    const double env = std::exp(-3.0 * std::fmod(t, 0.5));
    double v = 0.0;
    for (int h = 1; h <= 8; h++)
      v += std::sin(2.0 * 3.14159265358979 * 110.0 * h * t + 0.3 * h) / h;
    seed = seed * 1664525u + 1013904223u;
    const double noise = ((double)(seed >> 8) / 8388608.0 - 1.0) * 0.005;
    x[(size_t)i] = level * env * v + noise;
  }
  return x;
}

static Buf Sine(int n, double sr, double f, double a)
{
  Buf x((size_t)n);
  for (int i = 0; i < n; i++)
    x[(size_t)i] = a * std::sin(2.0 * 3.14159265358979 * f * i / sr);
  return x;
}

// Band-limited noise (sum of random sines below 15 kHz), for delay measurements.
static Buf BandNoise(int n, double sr, unsigned seed = 3u)
{
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> uf(60.0, 15000.0), up(0.0, 6.283185307179586);
  Buf x((size_t)n, 0.0);
  for (int k = 0; k < 64; k++)
  {
    const double f = uf(rng), p = up(rng);
    for (int i = 0; i < n; i++)
      x[(size_t)i] += 0.02 * std::sin(2.0 * 3.14159265358979 * f * i / sr + p);
  }
  return x;
}

static double Rms(const Buf& x, size_t from = 0, size_t to = SIZE_MAX)
{
  to = std::min(to, x.size());
  double s = 0.0;
  for (size_t i = from; i < to; i++)
    s += x[i] * x[i];
  return std::sqrt(s / (double)std::max<size_t>(1, to - from));
}
static double MaxDiff(const Buf& a, const Buf& b, size_t from = 0)
{
  double m = 0.0;
  for (size_t i = from; i < std::min(a.size(), b.size()); i++)
    m = std::max(m, std::fabs(a[i] - b[i]));
  return m;
}
static double NullDB(const Buf& a, const Buf& ref, size_t from = 0)
{
  double e = 0.0, r = 0.0;
  for (size_t i = from; i < std::min(a.size(), ref.size()); i++)
  {
    e += (a[i] - ref[i]) * (a[i] - ref[i]);
    r += ref[i] * ref[i];
  }
  if (r <= 0.0)
    return e <= 0.0 ? -999.0 : 999.0;
  return e <= 0.0 ? -999.0 : 10.0 * std::log10(e / r);
}
static bool AllFinite(const Buf& x)
{
  for (double v : x)
    if (!std::isfinite(v))
      return false;
  return true;
}
// Lag (in samples, -maxLag..maxLag) that maximises the cross-correlation of y against x.
static int BestLag(const Buf& x, const Buf& y, int maxLag, size_t from)
{
  int best = 0;
  double bestV = -1e300;
  for (int lag = -maxLag; lag <= maxLag; lag++)
  {
    double s = 0.0;
    for (size_t i = from; i + (size_t)std::max(lag, 0) < y.size() && i < x.size(); i++)
    {
      const long j = (long)i + lag;
      if (j < 0 || (size_t)j >= y.size())
        continue;
      s += x[i] * y[(size_t)j];
    }
    if (s > bestV)
    {
      bestV = s;
      best = lag;
    }
  }
  return best;
}

static Buf ReferenceHPF(const Buf& x, double sr)
{
  recursive_linear_filter::HighPass hp;
  hp.SetParams(recursive_linear_filter::HighPassParams(sr, 5.0));
  Buf y(x.size());
  const size_t blk = 4096;
  for (size_t pos = 0; pos < x.size(); pos += blk)
  {
    const size_t n = std::min(blk, x.size() - pos);
    Buf tmp(x.begin() + pos, x.begin() + pos + n);
    double* p = tmp.data();
    double** out = hp.Process(&p, 1, n);
    std::copy(out[0], out[0] + n, y.begin() + pos);
  }
  return y;
}

// What dsp::ImpulseResponse computes, for an IR at the host rate.
static Buf ReferenceIR(const Buf& x, const std::vector<float>& ir, double sr)
{
  const float gain = (float)(std::pow(10.0, -18.0 * 0.05) * 48000.0 / sr);
  std::vector<float> w(ir.size());
  for (size_t i = 0; i < ir.size(); i++)
    w[i] = gain * ir[i];
  Buf y(x.size(), 0.0);
  for (size_t n = 0; n < x.size(); n++)
  {
    double acc = 0.0;
    for (size_t k = 0; k < w.size() && k <= n; k++)
      acc += (double)w[k] * (double)(float)x[n - k];
    y[n] = acc;
  }
  return y;
}

// ── Plugin helpers ───────────────────────────────────────────────────────────

struct Rig
{
  std::unique_ptr<Plug> plug;
  double sr = 48000.0;
  int block = 256;
  int nIn = 1, nOut = 2;
};

static std::unique_ptr<Rig> MakeRig(double sr, int block, int nIn = 1, int nOut = 2)
{
  auto r = std::make_unique<Rig>();
  r->plug = std::make_unique<Plug>();
  r->sr = sr;
  r->block = block;
  r->nIn = nIn;
  r->nOut = nOut;
  std::string err;
  if (!r->plug->load(gPlugPath, err))
  {
    std::printf("  load failed: %s\n", err.c_str());
    return nullptr;
  }
  if (!r->plug->setup(sr, block, nIn, nOut, err))
  {
    std::printf("  setup failed: %s\n", err.c_str());
    return nullptr;
  }
  r->plug->activate(true);
  return r;
}

// Neutral test state: EQ off, gate off, everything else default.
static DualState Neutral()
{
  DualState st;
  st.params[kEQActiveA] = 0.0;
  st.params[kEQActiveB] = 0.0;
  st.params[kNoiseGateActive] = 0.0;
  return st;
}

static bool Apply(Rig& r, const DualState& st) { return r.plug->setState(BuildState(st)); }

struct Out
{
  Buf L, R;
};

// Process `in` (one buffer per input channel) in blocks. blockFn may vary the size.
static Out Render(Rig& r, const std::vector<Buf>& in, std::function<int(int)> blockFn = nullptr,
                  const std::vector<std::pair<int, double>>& firstBlockParams = {})
{
  const int n = (int)in[0].size();
  Out o;
  o.L.assign((size_t)n, 0.0);
  o.R.assign((size_t)n, 0.0);
  std::vector<Buf> ib((size_t)r.nIn, Buf((size_t)r.block * 8));
  std::vector<Buf> ob((size_t)r.nOut, Buf((size_t)r.block * 8));
  int pos = 0, k = 0;
  while (pos < n)
  {
    int b = blockFn ? blockFn(k) : r.block;
    b = std::min(b, n - pos);
    std::vector<const double*> ip((size_t)r.nIn);
    std::vector<double*> op((size_t)r.nOut);
    for (int c = 0; c < r.nIn; c++)
    {
      const Buf& src = in[(size_t)std::min(c, (int)in.size() - 1)];
      std::copy(src.begin() + pos, src.begin() + pos + b, ib[(size_t)c].begin());
      ip[(size_t)c] = ib[(size_t)c].data();
    }
    for (int c = 0; c < r.nOut; c++)
      op[(size_t)c] = ob[(size_t)c].data();
    r.plug->process(ip.data(), op.data(), b, k == 0 ? firstBlockParams : std::vector<std::pair<int, double>>{});
    std::copy(ob[0].begin(), ob[0].begin() + b, o.L.begin() + pos);
    std::copy(ob[(size_t)(r.nOut > 1 ? 1 : 0)].begin(), ob[(size_t)(r.nOut > 1 ? 1 : 0)].begin() + b,
              o.R.begin() + pos);
    pos += b;
    k++;
  }
  return o;
}

static Out RenderMono(Rig& r, const Buf& x) { return Render(r, {x}); }

// Arm the heap counter only around the plugin's process() call.
static void ArmOn() { heaphook::Arm(true); }
static void ArmOff() { heaphook::Arm(false); }

// ── Tests ────────────────────────────────────────────────────────────────────

static void TestA2FullInPlugin()
{
  Section("1  A2 container in the plugin plays A2 Full, not Lite");
  auto r = MakeRig(48000.0, 256);
  if (!r)
    return Check(false, "rig");
  const Buf x = GuitarSignal(48000, r->sr);

  DualState st = Neutral();
  st.nam[0] = M("A2.nam");
  st.nam[1] = G("A2_full_only.nam");
  st.params[kPanA] = -100.0;
  st.params[kPanB] = 100.0;
  Check(Apply(*r, st), "setState with A2.nam on A and A2_full_only.nam on B");
  Out o = RenderMono(*r, x);
  Check(Rms(o.L) > 0.01 && AllFinite(o.L), "channel A produces output (rms %.4f)", Rms(o.L));
  Check(MaxDiff(o.L, o.R) == 0.0, "A2.nam (container) == A2 Full, bit-identical (max diff %.3g)", MaxDiff(o.L, o.R));

  st.nam[1] = G("A2_lite_only.nam");
  Apply(*r, st);
  RenderMono(*r, Buf(4096, 0.0)); // let the swap happen
  o = RenderMono(*r, x);
  Check(MaxDiff(o.L, o.R) > 1e-3, "A2.nam differs from A2 Lite (max diff %.4f) -- the test discriminates",
        MaxDiff(o.L, o.R));
}

static void TestIndependentChains()
{
  Section("2/3  two different models processed at the same time, independently");
  struct Pair
  {
    const char* label;
    std::string a, b;
  } pairs[] = {{"A2 Full + A1 standard", G("A2_full_only.nam"), M("wavenet_a1_standard.nam")},
               {"A2 Full + A2 Full (variant)", G("A2_full_only.nam"), G("A2_full_variant.nam")},
               {"A2 container + LSTM", M("A2.nam"), M("lstm.nam")}};
  for (const auto& p : pairs)
  {
    auto r = MakeRig(48000.0, 128);
    if (!r)
      return Check(false, "rig");
    const Buf x = GuitarSignal(48000, r->sr);
    DualState st = Neutral();
    st.nam[0] = p.a;
    st.nam[1] = p.b;
    st.ir[0] = G("ir_lowpass_48k.wav");
    st.ir[1] = G("ir_bright_48k_pcm24.wav");
    st.params[kPanA] = -100.0;
    st.params[kPanB] = 100.0;
    Apply(*r, st);
    const Out both = RenderMono(*r, x);

    // Same, but B muted: A alone. Fresh instance so model state starts equal.
    auto rA = MakeRig(48000.0, 128);
    DualState stA = st;
    stA.params[kMuteB] = 1.0;
    Apply(*rA, stA);
    const Out onlyA = RenderMono(*rA, x);

    auto rB = MakeRig(48000.0, 128);
    DualState stB = st;
    stB.params[kMuteA] = 1.0;
    Apply(*rB, stB);
    const Out onlyB = RenderMono(*rB, x);

    // And with NAM bypassed on both: proves the models are actually doing something.
    auto rD = MakeRig(48000.0, 128);
    DualState stD = st;
    stD.params[kNAMBypassA] = 1.0;
    stD.params[kNAMBypassB] = 1.0;
    Apply(*rD, stD);
    const Out di = RenderMono(*rD, x);

    std::printf("  -- %s\n", p.label);
    // (The LSTM example model is a tiny test fixture with a very low output level.)
    Check(Rms(both.L) > 1e-5 && Rms(both.R) > 1e-5 && AllFinite(both.L) && AllFinite(both.R),
          "both chains produce output (L rms %.4f, R rms %.4f)", Rms(both.L), Rms(both.R));
    Check(MaxDiff(both.L, onlyA.L) == 0.0, "A with B running == A alone, bit-identical (no crosstalk)");
    Check(MaxDiff(both.R, onlyB.R) == 0.0, "B with A running == B alone, bit-identical (no crosstalk)");
    Check(Rms(onlyA.R) == 0.0 && Rms(onlyB.L) == 0.0, "a muted chain contributes exactly nothing");
    Check(NullDB(both.L, both.R) > -20.0, "A and B differ (null %.1f dB)", NullDB(both.L, both.R));
    Check(NullDB(both.L, di.L) > -20.0 && NullDB(both.R, di.R) > -20.0,
          "both models really process (vs NAM bypass: A %.1f dB, B %.1f dB)", NullDB(both.L, di.L),
          NullDB(both.R, di.R));
  }
}

static std::vector<float> ReadFloatWavMono(const std::string& path)
{
  // Only for the generated 32-bit float test files.
  std::ifstream f(path, std::ios::binary);
  std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  size_t pos = 12;
  while (pos + 8 <= b.size())
  {
    const std::string id(b.data() + pos, 4);
    uint32_t len;
    memcpy(&len, b.data() + pos + 4, 4);
    if (id == "data")
    {
      std::vector<float> s(len / 4);
      memcpy(s.data(), b.data() + pos + 8, len);
      return s;
    }
    pos += 8 + len;
  }
  return {};
}

static void TestIRs()
{
  Section("4  two different IRs, exact against a reference convolution");
  auto r = MakeRig(48000.0, 256);
  if (!r)
    return Check(false, "rig");
  const Buf x = GuitarSignal(24000, r->sr);
  DualState st = Neutral();
  st.ir[0] = G("ir_impulse_48k.wav");
  st.ir[1] = G("ir_lowpass_48k.wav");
  st.params[kPanA] = -100.0;
  st.params[kPanB] = 100.0;
  Apply(*r, st);
  const Out o = RenderMono(*r, x);
  // Hard-panned chains come out at 0 dB on their side.
  const Buf refA = ReferenceHPF(ReferenceIR(x, ReadFloatWavMono(G("ir_impulse_48k.wav")), r->sr), r->sr);
  const Buf refB = ReferenceHPF(ReferenceIR(x, ReadFloatWavMono(G("ir_lowpass_48k.wav")), r->sr), r->sr);
  Check(NullDB(o.L, refA) < -100.0, "A: impulse IR matches reference (null %.1f dB)", NullDB(o.L, refA));
  Check(NullDB(o.R, refB) < -100.0, "B: lowpass IR matches reference (null %.1f dB)", NullDB(o.R, refB));
  Check(NullDB(o.L, o.R) > -40.0, "the two IRs give different results (null %.1f dB)", NullDB(o.L, o.R));

  // IR bypass == no IR at all.
  auto r2 = MakeRig(48000.0, 256);
  DualState st2 = st;
  st2.params[kIRBypassB] = 1.0;
  Apply(*r2, st2);
  const Out o2 = RenderMono(*r2, x);
  auto r3 = MakeRig(48000.0, 256);
  DualState st3 = st;
  st3.ir[1] = "";
  Apply(*r3, st3);
  const Out o3 = RenderMono(*r3, x);
  Check(MaxDiff(o2.R, o3.R) == 0.0, "IR Bypass B == no IR loaded on B (bit-identical)");
  Check(MaxDiff(o2.L, o.L) == 0.0, "bypassing B's IR leaves A untouched");

  // An IR at another sample rate is resampled, not rejected.
  auto r4 = MakeRig(48000.0, 256);
  DualState st4 = st;
  st4.ir[0] = G("ir_impulse_44k.wav");
  Apply(*r4, st4);
  const Out o4 = RenderMono(*r4, x);
  const double ratio = Rms(o4.L) / Rms(o.L);
  Check(AllFinite(o4.L) && ratio > 0.5 && ratio < 2.0,
        "44.1 kHz IR on a 48 kHz host loads and plays (level ratio %.3f vs native)", ratio);
}

static void TestRouting()
{
  Section("5  fixed input routing: mono bus / left input -> A and B at full level");
  const double sr = 48000.0;
  // NAM bypassed, A hard left, B hard right: the left output IS chain A's
  // input and the right output chain B's (after the 5 Hz DC blocker).
  const Buf inL = GuitarSignal(24000, sr, 0.25, 7u);
  const Buf inR = Sine(24000, sr, 700.0, 0.2);
  const Buf refL = ReferenceHPF(inL, sr);
  DualState st = Neutral();
  st.params[kNAMBypassA] = st.params[kNAMBypassB] = 1.0;
  st.params[kPanA] = -100.0;
  st.params[kPanB] = 100.0;
  {
    auto r = MakeRig(sr, 256, 1, 2);
    Apply(*r, st);
    const Out o = Render(*r, {inL});
    Check(NullDB(o.L, refL) < -120.0 && NullDB(o.R, refL) < -120.0,
          "mono bus: input -> A and B unchanged, full level (null %.0f / %.0f dB)", NullDB(o.L, refL),
          NullDB(o.R, refL));
  }
  {
    auto r = MakeRig(sr, 256, 2, 2);
    Apply(*r, st);
    const Out o = Render(*r, {inL, inR});
    Check(NullDB(o.L, refL) < -120.0 && NullDB(o.R, refL) < -120.0,
          "stereo bus, different L and R: A and B both get L only, full level (null %.0f / %.0f dB)", NullDB(o.L, refL),
          NullDB(o.R, refL));
    // Fresh instance: the DC blocker would otherwise still ring out the run above.
    auto rz = MakeRig(sr, 256, 2, 2);
    Apply(*rz, st);
    const Out z = Render(*rz, {Buf(inL.size(), 0.0), inR});
    Check(Rms(z.L, 4800) < 1e-12 && Rms(z.R, 4800) < 1e-12, "stereo bus, signal only on R: ignored (rms %.2g / %.2g)",
          Rms(z.L, 4800), Rms(z.R, 4800));
  }
  // Old values of the reserved ID 24 change nothing -- in a state or as automation.
  for (double legacy : {1.0, 2.0, 3.0})
  {
    auto r = MakeRig(sr, 256, 2, 2);
    DualState s2 = st;
    s2.extra.push_back({"Input Source", legacy}); // 1.1.0 name
    s2.params[kInputSourceUnused] = legacy; // 1.2.0 name
    Apply(*r, s2);
    const Out o = Render(*r, {inL, inR}, nullptr, {{kInputSourceUnused, legacy / 3.0}});
    Check(NullDB(o.L, refL) < -120.0 && NullDB(o.R, refL) < -120.0,
          "ID 24 = %.0f (state and automation): routing unchanged, L -> A and B", legacy);
  }
  {
    // getParameterInfo() takes an INDEX; look the parameters up by their ID.
    auto r = MakeRig(sr, 256, 1, 2);
    IEditController* ec = r->plug->controller();
    auto byId = [&](ParamID id, ParameterInfo& out) {
      for (int32 i = 0; i < ec->getParameterCount(); i++)
        if (ec->getParameterInfo(i, out) == kResultOk && out.id == id)
          return true;
      return false;
    };
    auto title = [](const ParameterInfo& pi) {
      std::string t;
      for (int i = 0; i < 128 && pi.title[i]; i++)
        t += (char)pi.title[i];
      return t;
    };
    ParameterInfo info{};
    const bool got = byId(kInputSourceUnused, info);
    Check(got && !(info.flags & ParameterInfo::kCanAutomate), "ID 24 '%s' is reserved and NOT automatable (flags 0x%x)",
          title(info).c_str(), (unsigned)info.flags);
    const int expectIds[] = {kNoiseGateActive, kNoiseGateThreshold, kGateAttack, kGateRelease, kGatePosition};
    const char* const expectNames[] = {"Gate", "Gate Threshold", "Gate Attack", "Gate Release", "Gate Position"};
    for (int k = 0; k < 5; k++)
    {
      ParameterInfo pi{};
      const bool found = byId((ParamID)expectIds[k], pi);
      Check(found && title(pi) == expectNames[k] && (pi.flags & ParameterInfo::kCanAutomate), "ID %d = '%s', automatable",
            expectIds[k], title(pi).c_str());
    }
    // IDs 0-23 unchanged since 1.0.0 (spot checks), plus iPlug2's own bypass parameter.
    ParameterInfo a{}, m{};
    byId(kInputA, a);
    byId(kMasterOutput, m);
    Check(title(a) == "Input A" && title(m) == "Master" && ec->getParameterCount() == kNumParams + 1,
          "ID 0 = '%s', ID 21 = '%s', %d parameters (28 plus the host bypass)", title(a).c_str(), title(m).c_str(),
          ec->getParameterCount());
  }
  {
    // Bus arrangements a host may ask for.
    Plug p;
    std::string err;
    p.load(gPlugPath, err);
    Check(p.setup(48000.0, 256, 1, 2, err), "accepts mono in -> stereo out (1-2)");
    Check(p.setup(48000.0, 256, 2, 2, err), "accepts stereo in -> stereo out (2-2)");
  }
}

// ── Gate ─────────────────────────────────────────────────────────────────────

struct GateRun
{
  Out out;
  // Gate gain seen on each chain, two ways:
  //  gA/gB    per sample, out / (gain * x) -- exact, but only where the input
  //           is loud (|x| > 1e-3 near a carrier peak), i.e. while a burst plays;
  //  gAd/gBd  lock-in at the 4 kHz carrier over one period (12 samples):
  //           sum(out*ref) / sum(gain*x*ref). Rejects the slow 5 Hz DC-blocker
  //           tail, so it also works after a burst, in the -110 dB "silence".
  Buf gA, gB, gAd, gBd;
};

// NAM bypassed (unless models are given), no IR, EQ off, A hard left, B hard
// right. With bypassed NAM the left output is  inputGainA * g * x  (after the
// DC blocker, negligible at the 4 kHz carrier used), so g can be read back per
// sample as out / (gain * x) wherever |x| is not near a zero crossing.
static GateRun RunGate(const Buf& x, double thr, double attack, double release, int position, double inA = 0.0,
                       double inB = 0.0, bool gateOn = true, int block = 128,
                       std::function<std::vector<std::pair<int, double>>(int)> automation = nullptr,
                       const char* namA = nullptr, const char* namB = nullptr, std::function<void(int, Plug&)> perBlock = nullptr)
{
  const double sr = 48000.0;
  auto r = MakeRig(sr, block, 1, 2);
  DualState st = Neutral();
  st.params[kNAMBypassA] = namA ? 0.0 : 1.0;
  st.params[kNAMBypassB] = namB ? 0.0 : 1.0;
  if (namA)
    st.nam[0] = namA;
  if (namB)
    st.nam[1] = namB;
  st.params[kPanA] = -100.0;
  st.params[kPanB] = 100.0;
  st.params[kNoiseGateActive] = gateOn ? 1.0 : 0.0;
  st.params[kNoiseGateThreshold] = thr;
  st.params[kGateAttack] = attack;
  st.params[kGateRelease] = release;
  st.params[kGatePosition] = position;
  st.params[kInputA] = inA;
  st.params[kInputB] = inB;
  Apply(*r, st);
  // Restart processing with the state in place, as a DAW does when it opens a
  // project: the gate then starts closed at full depth, instead of fading in
  // from "gate off" over the first 10 ms.
  {
    std::string err;
    r->plug->setup(sr, block, 1, 2, err);
    r->plug->activate(true);
  }
  GateRun g;
  g.out.L.assign(x.size(), 0.0);
  g.out.R.assign(x.size(), 0.0);
  Buf l((size_t)block), rr((size_t)block);
  std::vector<double*> op = {l.data(), rr.data()};
  for (size_t pos = 0, k = 0; pos < x.size(); pos += (size_t)block, k++)
  {
    const int n = (int)std::min((size_t)block, x.size() - pos);
    std::vector<const double*> ip = {x.data() + pos};
    if (perBlock)
      perBlock((int)k, *r->plug);
    r->plug->process(ip.data(), op.data(), n, automation ? automation((int)k) : std::vector<std::pair<int, double>>{});
    std::copy(l.begin(), l.begin() + n, g.out.L.begin() + (long)pos);
    std::copy(rr.begin(), rr.begin() + n, g.out.R.begin() + (long)pos);
  }
  const double gainA = std::pow(10.0, inA / 20.0), gainB = std::pow(10.0, inB / 20.0);
  g.gA.assign(x.size(), std::nan(""));
  g.gB.assign(x.size(), std::nan(""));
  g.gAd.assign(x.size(), std::nan(""));
  g.gBd.assign(x.size(), std::nan(""));
  for (size_t i = 0; i < x.size(); i++)
  {
    // Per sample: only near a carrier peak (a 4 kHz period is 12 samples) and
    // only while the input is loud enough that the DC-blocker tail is negligible.
    double localPeak = 0.0;
    for (size_t j = (i >= 6 ? i - 6 : 0); j < std::min(x.size(), i + 6); j++)
      localPeak = std::max(localPeak, std::fabs(x[j]));
    if (localPeak > 1e-3 && std::fabs(x[i]) > 0.6 * localPeak)
    {
      g.gA[i] = g.out.L[i] / (gainA * x[i]);
      g.gB[i] = g.out.R[i] / (gainB * x[i]);
    }
    // Lock-in over one carrier period centred on i.
    if (i >= 6 && i + 6 <= x.size())
    {
      // Complex demodulation (sine and cosine), so the carrier phase does not matter.
      double aS = 0, aC = 0, bS = 0, bC = 0, xS = 0, xC = 0;
      for (size_t j = i - 6; j < i + 6; j++)
      {
        const double ph = 2.0 * 3.14159265358979 * 4000.0 * j / sr;
        const double rs = std::sin(ph), rc = std::cos(ph);
        aS += g.out.L[j] * rs;
        aC += g.out.L[j] * rc;
        bS += g.out.R[j] * rs;
        bC += g.out.R[j] * rc;
        xS += x[j] * rs;
        xC += x[j] * rc;
      }
      const double d = std::hypot(xS, xC);
      if (d > 0.0)
      {
        g.gAd[i] = std::hypot(aS, aC) / (gainA * d);
        g.gBd[i] = std::hypot(bS, bC) / (gainB * d);
      }
    }
  }
  return g;
}

// 4 kHz carrier bursts: silence (-110 dBFS carrier) / burst at levelDb / silence.
static Buf Bursts(double sr, double onS, double offS, double totalS, double levelDb)
{
  Buf x((size_t)(totalS * sr));
  for (size_t i = 0; i < x.size(); i++)
  {
    const double t = i / sr;
    const double a = (t >= onS && t < offS) ? std::pow(10.0, levelDb / 20.0) : std::pow(10.0, -110.0 / 20.0);
    x[i] = a * std::sin(2.0 * 3.14159265358979 * 4000.0 * t);
  }
  return x;
}

// First time (ms, from `from`) at which g (ignoring NaN) reaches / falls to `level`.
static double TimeTo(const Buf& g, size_t from, double level, bool rising, double sr = 48000.0)
{
  for (size_t i = from; i < g.size(); i++)
    if (!std::isnan(g[i]) && (rising ? g[i] >= level : g[i] <= level))
      return (double)(i - from) / sr * 1000.0;
  return -1.0;
}

// A click detector: the largest second difference of the signal.
static double MaxD2(const Buf& y, size_t from, size_t to)
{
  double m = 0.0;
  for (size_t i = std::max<size_t>(from, 2); i < std::min(to, y.size()); i++)
    m = std::max(m, std::fabs(y[i] - 2.0 * y[i - 1] + y[i - 2]));
  return m;
}

// Undo the plug-in's 5 Hz DC blocker (first order, y = b0 x + b1 x[-1] - a1 y[-1]),
// coefficients taken from the impulse response of the very same filter class.
// Gives the signal right after the gate, free of the blocker's slow tails.
static Buf UndoDCBlocker(const Buf& y, double sr, bool* firstOrder = nullptr)
{
  Buf imp(8, 0.0);
  imp[0] = 1.0;
  const Buf h = ReferenceHPF(imp, sr);
  const double b0 = h[0], a1 = -h[2] / h[1], b1 = h[1] + a1 * b0;
  if (firstOrder)
    *firstOrder = std::fabs(h[3] / h[2] - h[2] / h[1]) < 1e-12;
  Buf u(y.size());
  double up = 0.0, yp = 0.0;
  for (size_t i = 0; i < y.size(); i++)
  {
    u[i] = (y[i] + a1 * yp - b1 * up) / b0;
    up = u[i];
    yp = y[i];
  }
  return u;
}

static void TestGate()
{
  Section("G  noise gate: detector, attack, release, Pre/Post, switching");
  const double sr = 48000.0;
  heaphook::ResetArmed();
  Plug::sOnProcessEnter = ArmOn;
  Plug::sOnProcessLeave = ArmOff;

  // ── G1 detector independent of the per-chain input gains
  {
    const Buf x = Bursts(sr, 0.10, 0.40, 0.8, -40.0);
    const GateRun ref = RunGate(x, -60.0, 3.0, 150.0, kGatePost, 0.0, 0.0);
    const GateRun hot = RunGate(x, -60.0, 3.0, 150.0, kGatePost, 12.0, -12.0);
    double dAB = 0.0, dRef = 0.0;
    for (size_t i = 0; i < x.size(); i++)
      if (!std::isnan(hot.gAd[i]) && !std::isnan(ref.gAd[i]))
      {
        dAB = std::max(dAB, std::fabs(hot.gAd[i] - hot.gBd[i]));
        dRef = std::max(dRef, std::fabs(hot.gAd[i] - ref.gAd[i]));
      }
    Check(dAB < 1e-6 && dRef < 1e-6,
          "Input A +12 dB / Input B -12 dB: same envelope on A and B (max diff %.2g) and as with 0 dB (max diff %.2g)", dAB,
          dRef);
  }

  // ── G2 attack: time from the onset until the gain reaches 99 %
  {
    const Buf x = Bursts(sr, 0.10, 0.40, 0.8, -30.0);
    const size_t on = (size_t)(0.10 * sr);
    for (double a : {0.1, 3.0, 50.0})
    {
      const GateRun g = RunGate(x, -60.0, a, 150.0, kGatePost);
      const double t99 = TimeTo(g.gA, on, 0.99, true), t50 = TimeTo(g.gA, on, 0.5, true);
      // One-pole: 50 % after ln(0.5)/ln(0.01) = 15 % of the attack time, 99 % after 100 %.
      const bool ok = std::fabs(t99 - a) <= 0.1 * a + 0.1 && std::fabs(t50 - 0.1505 * a) <= 0.1 * a + 0.1;
      Check(ok, "attack %5.1f ms: gain 50 %% after %.2f ms (exp %.2f), 99 %% after %.2f ms (exp %.1f)", a, t50, 0.1505 * a,
            t99, a);
    }
  }

  // ── G3 release: time from the end of the burst until the gain is down to 1 %
  {
    const Buf x = Bursts(sr, 0.10, 0.40, 2.0, -30.0);
    const size_t off = (size_t)(0.40 * sr);
    double t[3];
    const double rel[3] = {10.0, 150.0, 1000.0};
    for (int k = 0; k < 3; k++)
      t[k] = TimeTo(RunGate(x, -60.0, 3.0, rel[k], kGatePost).gAd, off, 0.01, false);
    // Before the release starts: the detector must fall below threshold - 3 dB
    // (2 ms time constant, ~2.2 dB/ms) and the 10 ms hold must expire.
    const double startDelay = t[0] - rel[0];
    Check(std::fabs((t[1] - t[0]) - 140.0) < 2.0 && std::fabs((t[2] - t[1]) - 850.0) < 5.0,
          "release 10 / 150 / 1000 ms: gain at 1 %% after %.1f / %.1f / %.1f ms (differences %.1f and %.1f ms, exp 140 and 850)",
          t[0], t[1], t[2], t[1] - t[0], t[2] - t[1]);
    Check(startDelay > 10.0 && startDelay < 30.0,
          "  release starts %.1f ms after the signal stops (detector fall from -30 to -63 dB + 10 ms hold)", startDelay);
  }

  // ── G4 no hard switching: largest gain step per sample
  {
    // Carrier starts on a peak (cosine), plays at -30 dBFS, then continues at
    // -75 dBFS (below threshold - 3 dB): the gate opens and closes on a carrier
    // that never stops, so the gain is measurable throughout.
    Buf x((size_t)(0.8 * sr));
    // The level drop lands on a carrier zero crossing (a quarter period after a
    // peak), so the DC blocker gets no step to ring out over the quiet carrier.
    const size_t on = (size_t)(0.10 * sr), off = (size_t)(0.40 * sr) + 3;
    for (size_t i = 0; i < x.size(); i++)
    {
      const double lvl = i < on ? -110.0 : (i < off ? -30.0 : -75.0);
      x[i] = std::pow(10.0, lvl / 20.0) * std::cos(2.0 * 3.14159265358979 * 4000.0 * (double)(i - on) / sr);
    }
    for (double a : {0.1, 3.0})
    {
      const GateRun g = RunGate(x, -60.0, a, 10.0, kGatePost);
      // Exact gate gain per sample: signal right after the gate / input,
      // wherever the carrier is away from a zero crossing.
      const Buf u = UndoDCBlocker(g.out.L, sr);
      Buf gg(x.size(), std::nan(""));
      for (size_t i = 0; i < x.size(); i++)
      {
        double pk = 0.0;
        for (size_t j = (i >= 6 ? i - 6 : 0); j < std::min(x.size(), i + 6); j++)
          pk = std::max(pk, std::fabs(x[j]));
        if (pk > 0.0 && std::fabs(x[i]) > 0.5 * pk)
          gg[i] = u[i] / x[i];
      }
      double stepOpen = 0.0, stepClose = 0.0;
      for (size_t i = on + 1; i < on + (size_t)(0.005 * sr); i++)
        if (!std::isnan(gg[i]) && !std::isnan(gg[i - 1]))
          stepOpen = std::max(stepOpen, std::fabs(gg[i] - gg[i - 1]));
      for (size_t i = off + 1; i < off + (size_t)(0.1 * sr); i++)
        if (!std::isnan(gg[i]) && !std::isnan(gg[i - 1]))
          stepClose = std::max(stepClose, std::fabs(gg[i] - gg[i - 1]));
      const double theoryOpen = 1.0 - std::exp(-4.605170185988091 / (a * 0.001 * sr));
      const double theoryClose = 1.0 - std::exp(-4.605170185988091 / (10.0 * 0.001 * sr));
      Check(stepOpen <= theoryOpen + 1e-9 && stepClose <= theoryClose + 1e-9 && stepOpen < 0.99,
            "attack %.1f ms: largest gain step per sample opening %.4f (one-pole limit %.4f), closing with release 10 ms %.4f "
            "(limit %.4f)",
            a, stepOpen, theoryOpen, stepClose, theoryClose);
    }
  }

  // ── G5 a short pluck with 0.1 ms attack keeps its transient
  {
    Buf x((size_t)(0.5 * sr), 0.0);
    const size_t on = (size_t)(0.1 * sr);
    for (size_t i = on; i < x.size(); i++)
    {
      const double t = (i - on) / sr;
      x[i] = 0.3 * std::exp(-t / 0.03) * std::sin(2.0 * 3.14159265358979 * 196.0 * t + 0.4);
    }
    const GateRun gated = RunGate(x, -50.0, 0.1, 150.0, kGatePost);
    const GateRun open = RunGate(x, -50.0, 0.1, 150.0, kGatePost, 0.0, 0.0, false);
    auto energy = [&](const Buf& y, size_t a, size_t b) {
      double e = 0.0;
      for (size_t i = a; i < b; i++)
        e += y[i] * y[i];
      return e;
    };
    const size_t w1 = on + (size_t)(0.001 * sr), w5 = on + (size_t)(0.005 * sr);
    const double r1 = energy(gated.out.L, on, w1) / energy(open.out.L, on, w1);
    const double r5 = energy(gated.out.L, on, w5) / energy(open.out.L, on, w5);
    Check(r5 > 0.98, "0.1 ms attack, 30 ms pluck: first 1 ms keeps %.1f %%, first 5 ms keeps %.1f %% of the energy", 100.0 * r1,
          100.0 * r5);
  }

  // ── G6 Pre NAM gates before the models, Post NAM after them
  {
    // Signal below the threshold all the time: the gate stays closed.
    Buf quiet = Sine((int)(0.5 * sr), sr, 220.0, 0.001); // -60 dBFS
    const GateRun pre = RunGate(quiet, -30.0, 3.0, 150.0, kGatePre, 0, 0, true, 128, nullptr,
                                G("A2_full_only.nam").c_str(), G("A2_full_variant.nam").c_str());
    const GateRun silent = RunGate(Buf(quiet.size(), 0.0), -30.0, 3.0, 150.0, kGatePre, 0, 0, true, 128, nullptr,
                                   G("A2_full_only.nam").c_str(), G("A2_full_variant.nam").c_str());
    const GateRun post = RunGate(quiet, -30.0, 3.0, 150.0, kGatePost, 0, 0, true, 128, nullptr,
                                 G("A2_full_only.nam").c_str(), G("A2_full_variant.nam").c_str());
    const GateRun ungated = RunGate(quiet, -30.0, 3.0, 150.0, kGatePost, 0, 0, false, 128, nullptr,
                                    G("A2_full_only.nam").c_str(), G("A2_full_variant.nam").c_str());
    Check(MaxDiff(pre.out.L, silent.out.L) == 0.0 && MaxDiff(pre.out.R, silent.out.R) == 0.0 && Rms(ungated.out.L) > 1e-4,
          "Pre NAM, gate closed: both models get exactly silence (output == model fed with zeros; ungated rms %.4f)",
          Rms(ungated.out.L));
    Check(Rms(post.out.L) == 0.0 && Rms(post.out.R) == 0.0,
          "Post NAM, gate closed: both NAM outputs exactly 0 before EQ / IR");
  }
  {
    // Post: one envelope on two different models. Compare per 1 ms window the
    // gated against the ungated output of each chain.
    const Buf x = Bursts(sr, 0.10, 0.30, 0.8, -25.0);
    const GateRun gated = RunGate(x, -60.0, 3.0, 50.0, kGatePost, 0, 0, true, 128, nullptr,
                                  G("A2_full_only.nam").c_str(), M("wavenet_a1_standard.nam").c_str());
    const GateRun open = RunGate(x, -60.0, 3.0, 50.0, kGatePost, 0, 0, false, 128, nullptr,
                                 G("A2_full_only.nam").c_str(), M("wavenet_a1_standard.nam").c_str());
    // The 5 Hz DC blocker sits AFTER the gate, so a model's DC offset (-45 to
    // -72 dBFS for these models) turns into a slow tail whenever the gate
    // moves, different per model: compare the signal right after the gate.
    bool firstOrder = false;
    UndoDCBlocker(Buf(4, 0.0), sr, &firstOrder);
    auto undo = [&](const Buf& y) { return UndoDCBlocker(y, sr); };
    const Buf uGA = undo(gated.out.L), uGB = undo(gated.out.R), uOA = undo(open.out.L), uOB = undo(open.out.R);
    // Per sample: (gated / ungated) right after the gate, wherever both
    // ungated signals are well away from a zero crossing; A against B.
    double worst = 0.0;
    int windows = 0;
    auto valid = [&](const Buf& y, size_t i) {
      double pk = 0.0;
      for (size_t j = (i >= 6 ? i - 6 : 0); j < std::min(y.size(), i + 6); j++)
        pk = std::max(pk, std::fabs(y[j]));
      return pk > 1e-3 && std::fabs(y[i]) > 0.5 * pk;
    };
    for (size_t i = (size_t)(0.1 * sr); i < (size_t)(0.45 * sr) && firstOrder; i++)
    {
      if (!valid(uOA, i) || !valid(uOB, i))
        continue;
      const double gA = uGA[i] / uOA[i], gB = uGB[i] / uOB[i];
      if (std::fabs(gA - gB) > worst)
      {
        worst = std::fabs(gA - gB);
        if (std::getenv("DUALNAM_DEBUG"))
          std::printf("    worst at %zu (%.2f ms): gA %.4f gB %.4f  openL %.5f openR %.5f gatedL %.5f gatedR %.5f\n", i,
                      i / sr * 1000.0, gA, gB, open.out.L[i], open.out.R[i], gated.out.L[i], gated.out.R[i]);
      }
      windows++;
    }
    Check(firstOrder && windows > 1000 && worst < 1e-6,
          "Post NAM, A2 Full on A and A1 on B: identical gate gain on both chains right after the gate (max diff %.2g over %d "
          "samples)",
          worst, windows);
  }

  // ── G7/G8 switching while audio runs: Pre <-> Post, gate on/off, fast parameter moves
  {
    // Guitar-like signal: notes and pauses, through two real models.
    const Buf x = GuitarSignal((int)(3.0 * sr), sr, 0.25, 21u);
    const std::string mA = G("A2_full_only.nam"), mB = G("A2_full_variant.nam");
    const int block = 128;
    const GateRun steadyPost = RunGate(x, -50.0, 3.0, 100.0, kGatePost, 0, 0, true, block, nullptr, mA.c_str(), mB.c_str());
    const GateRun steadyPre = RunGate(x, -50.0, 3.0, 100.0, kGatePre, 0, 0, true, block, nullptr, mA.c_str(), mB.c_str());
    const double refD2 = std::max({MaxD2(steadyPost.out.L, 0, x.size()), MaxD2(steadyPre.out.L, 0, x.size()),
                                   MaxD2(steadyPost.out.R, 0, x.size()), MaxD2(steadyPre.out.R, 0, x.size())});
    // Switch the position every 61 blocks (~160 ms): lands while notes ring,
    // while the gate opens and while it closes.
    auto posSwitch = [&](int k) -> std::vector<std::pair<int, double>> {
      if (k % 61 == 30)
        return {{kGatePosition, ((k / 61) % 2) ? 1.0 : 0.0}};
      return {};
    };
    const GateRun sw = RunGate(x, -50.0, 3.0, 100.0, kGatePost, 0, 0, true, block, posSwitch, mA.c_str(), mB.c_str());
    const double swD2 = std::max(MaxD2(sw.out.L, 0, x.size()), MaxD2(sw.out.R, 0, x.size()));
    Check(swD2 <= 1.05 * refD2 && AllFinite(sw.out.L),
          "Pre <-> Post every ~160 ms during playing: largest 2nd difference %.4g vs %.4g without switching (ratio %.2f)", swD2,
          refD2, swD2 / refD2);

    auto onOff = [&](int k) -> std::vector<std::pair<int, double>> {
      if (k % 47 == 20)
        return {{kNoiseGateActive, ((k / 47) % 2) ? 1.0 : 0.0}};
      return {};
    };
    const GateRun oo = RunGate(x, -50.0, 3.0, 100.0, kGatePre, 0, 0, true, block, onOff, mA.c_str(), mB.c_str());
    const GateRun ungated = RunGate(x, -50.0, 3.0, 100.0, kGatePre, 0, 0, false, block, nullptr, mA.c_str(), mB.c_str());
    const double ref2 = std::max(refD2, std::max(MaxD2(ungated.out.L, 0, x.size()), MaxD2(ungated.out.R, 0, x.size())));
    const double ooD2 = std::max(MaxD2(oo.out.L, 0, x.size()), MaxD2(oo.out.R, 0, x.size()));
    Check(ooD2 <= 1.05 * ref2, "gate on/off every ~125 ms during playing (Pre): largest 2nd difference %.4g vs %.4g (ratio %.2f)",
          ooD2, ref2, ooD2 / ref2);

    // Threshold swept every block between -90 and -20 dB (normalised), plus
    // the position flipped: the worst a host automation could do.
    auto sweep = [&](int k) -> std::vector<std::pair<int, double>> {
      const double thr = -55.0 + 35.0 * std::sin(k * 0.3);
      std::vector<std::pair<int, double>> v = {{kNoiseGateThreshold, ToNorm(kNoiseGateThreshold, thr)}};
      if (k % 37 == 0)
        v.push_back({kGatePosition, (k / 37) % 2 ? 1.0 : 0.0});
      return v;
    };
    const GateRun sweepRun = RunGate(x, -50.0, 3.0, 100.0, kGatePost, 0, 0, true, block, sweep, mA.c_str(), mB.c_str());
    const double swpD2 = std::max(MaxD2(sweepRun.out.L, 0, x.size()), MaxD2(sweepRun.out.R, 0, x.size()));
    Check(swpD2 <= 1.05 * ref2 && AllFinite(sweepRun.out.L),
          "threshold automated every block + position flips: largest 2nd difference %.4g vs %.4g (ratio %.2f)", swpD2, ref2,
          swpD2 / ref2);
  }

  Plug::sOnProcessEnter = nullptr;
  Plug::sOnProcessLeave = nullptr;
  Check(heaphook::ArmedOps() == 0, "all gate runs above: %lld heap operations inside process()",
        (long long)heaphook::ArmedOps());
}
static void TestPanLaw()
{
  Section("6a  pan law: sine/cosine, hard = 0 dB, centre = -3.01 dB");
  // One chain (A), NAM bypassed, B blended away: output = input x pan gain.
  for (double pan : {-100.0, -75.0, -50.0, -25.0, 0.0, 25.0, 50.0, 75.0, 100.0})
  {
    auto r = MakeRig(48000.0, 256);
    const Buf x = Sine(24000, r->sr, 1000.0, 0.25);
    DualState st = Neutral();
    st.params[kNAMBypassA] = st.params[kNAMBypassB] = 1.0;
    st.params[kBlend] = -100.0;
    st.params[kPanA] = pan;
    Apply(*r, st);
    const Out o = RenderMono(*r, x);
    const double theta = (pan / 100.0 + 1.0) * 0.25 * 3.14159265358979323846;
    const double eL = pan <= -100.0 ? 1.0 : (pan >= 100.0 ? 0.0 : std::cos(theta));
    const double eR = pan <= -100.0 ? 0.0 : (pan >= 100.0 ? 1.0 : std::sin(theta));
    // Measured against the input run through the same 5 Hz DC blocker, so only
    // the pan factor is left (the blocker alone accounts for ~0.03 % at 1 kHz).
    const Buf ref = ReferenceHPF(x, r->sr);
    const double gl = Rms(o.L, 4800) / Rms(ref, 4800), gr = Rms(o.R, 4800) / Rms(ref, 4800);
    auto dB = [](double g) { return g > 0.0 ? 20.0 * std::log10(g) : -999.0; };
    const bool exactEnds = (pan > -100.0 || Rms(o.R) == 0.0) && (pan < 100.0 || Rms(o.L) == 0.0);
    Check(std::fabs(gl - eL) < 1e-9 && std::fabs(gr - eR) < 1e-9 && exactEnds && std::fabs(gl * gl + gr * gr - 1.0) < 1e-9,
          "pan %+5.0f: L %.4f (%+6.2f dB, exp %.4f)  R %.4f (%+6.2f dB, exp %.4f)  L^2+R^2 %.4f", pan, gl, dB(gl), eL, gr,
          dB(gr), eR, gl * gl + gr * gr);
  }
}

static void TestBlendPan()
{
  Section("6  blend and pan gains");
  // NAM bypassed, no IR, EQ off: each chain is a known gain. A at 0 dB, B at -6.02 dB.
  // Pan law: hard = 0 dB, centre = -3.01 dB (cos/sin). Blend unchanged.
  const double gB = std::pow(10.0, -6.0206 / 20.0);
  const double c = std::sqrt(0.5); // centre pan, -3.01 dB
  const double c50 = std::cos(0.125 * 3.14159265358979), s50 = std::sin(0.125 * 3.14159265358979);
  struct Case
  {
    double blend, panA, panB, expL, expR;
    const char* what;
  } cases[] = {
    {-100, 0, 0, c, c, "blend full left = A only (centred)"},
    {100, 0, 0, c * gB, c * gB, "blend full right = B only (centred)"},
    {0, 0, 0, c * (1.0 + gB), c * (1.0 + gB), "blend centre = A and B both at full level"},
    {-50, 0, 0, c * (1.0 + 0.5 * gB), c * (1.0 + 0.5 * gB), "blend -50 = A full, B half"},
    {50, 0, 0, c * (0.5 + gB), c * (0.5 + gB), "blend +50 = A half, B full"},
    {0, -100, 100, 1.0, gB, "pan A hard left, B hard right"},
    {0, 100, -100, gB, 1.0, "pan swapped"},
    {0, -50, 0, c50 + c * gB, s50 + c * gB, "pan A 50% left (constant power)"},
    {-100, -100, 100, 1.0, 0.0, "blend A only + A hard left: right is silent"},
  };
  for (const auto& c : cases)
  {
    auto r = MakeRig(48000.0, 256);
    const Buf x = Sine(24000, r->sr, 1000.0, 0.25);
    DualState st = Neutral();
    st.params[kNAMBypassA] = st.params[kNAMBypassB] = 1.0;
    st.params[kOutputB] = -6.0206;
    st.params[kBlend] = c.blend;
    st.params[kPanA] = c.panA;
    st.params[kPanB] = c.panB;
    Apply(*r, st);
    const Out o = RenderMono(*r, x);
    const double gl = Rms(o.L, 4800) / Rms(x, 4800), gr = Rms(o.R, 4800) / Rms(x, 4800);
    const bool ok = std::fabs(gl - c.expL) < 2e-3 * std::max(1.0, c.expL) && std::fabs(gr - c.expR) < 2e-3 * std::max(1.0, c.expR);
    Check(ok, "%-42s L %.4f (exp %.4f)  R %.4f (exp %.4f)", c.what, gl, c.expL, gr, c.expR);
  }
}

static void TestGains()
{
  Section("7  input gain drives the model, output gain scales after it");
  const double sr = 48000.0;
  const Buf x = GuitarSignal(48000, sr);
  auto run = [&](double inA, double outA, bool bypass) {
    auto r = MakeRig(sr, 256);
    DualState st = Neutral();
    st.nam[0] = G("A2_full_only.nam");
    st.params[kMuteB] = 1.0;
    st.params[kInputA] = inA;
    st.params[kOutputA] = outA;
    st.params[kNAMBypassA] = bypass ? 1.0 : 0.0;
    Apply(*r, st);
    return RenderMono(*r, x).L;
  };
  const Buf base = run(0.0, 0.0, false);
  const Buf hot = run(12.0, -12.0, false);
  Check(NullDB(hot, base) > -25.0,
        "Input +12 dB / Output -12 dB changes the sound (null %.1f dB): the model is driven harder",
        NullDB(hot, base));
  const Buf louder = run(0.0, 6.0, false);
  Buf scaled = base;
  for (auto& v : scaled)
    v *= std::pow(10.0, 6.0 / 20.0);
  Check(NullDB(louder, scaled) < -120.0, "Output +6 dB is a pure gain after the model (null %.1f dB)",
        NullDB(louder, scaled));
  const Buf diIn = run(6.0, 0.0, true);
  const Buf di0 = run(0.0, 0.0, true);
  Check(std::fabs(Rms(diIn) / Rms(di0) - std::pow(10.0, 0.3)) < 1e-3,
        "with NAM bypassed, Input +6 dB is exactly +6 dB (ratio %.5f)", Rms(diIn) / Rms(di0));
  Buf master = base;
  {
    auto r = MakeRig(sr, 256);
    DualState st = Neutral();
    st.nam[0] = G("A2_full_only.nam");
    st.params[kMuteB] = 1.0;
    st.params[kMasterOutput] = -6.0;
    Apply(*r, st);
    master = RenderMono(*r, x).L;
  }
  Buf exp = base;
  for (auto& v : exp)
    v *= std::pow(10.0, -6.0 / 20.0);
  Check(NullDB(master, exp) < -120.0, "Master -6 dB scales the sum (null %.1f dB)", NullDB(master, exp));
}

static void TestBypassMute()
{
  Section("8  NAM bypass, IR bypass, mute");
  const double sr = 48000.0;
  const Buf x = GuitarSignal(24000, sr);
  auto r = MakeRig(sr, 256);
  DualState st = Neutral();
  st.nam[0] = G("A2_full_only.nam");
  st.nam[1] = G("A2_full_variant.nam");
  st.params[kNAMBypassA] = 1.0;
  st.params[kPanA] = -100.0;
  st.params[kPanB] = 100.0;
  Apply(*r, st);
  const Out o = RenderMono(*r, x);
  const Buf ref = ReferenceHPF(x, sr); // A hard left: 0 dB on the left
  Check(NullDB(o.L, ref) < -120.0, "NAM Bypass A: A is the clean (DC-blocked) input (null %.1f dB)", NullDB(o.L, ref));
  Check(Rms(o.R) > 0.01, "B keeps processing while A's model is bypassed");

  st.params[kNAMBypassA] = 0.0;
  st.params[kMuteA] = st.params[kMuteB] = 1.0;
  Apply(*r, st);
  RenderMono(*r, Buf(4096, 0.0));
  const Out m = RenderMono(*r, x);
  Check(Rms(m.L) == 0.0 && Rms(m.R) == 0.0, "both muted: output is exactly zero");
}

static void TestState()
{
  Section("9  save and restore the complete state");
  const double sr = 48000.0;
  auto r1 = MakeRig(sr, 256);
  DualState st;
  st.nam[0] = M("A2.nam");
  st.nam[1] = M("wavenet_a1_standard.nam");
  st.ir[0] = G("ir_lowpass_48k.wav");
  st.ir[1] = G("ir_bright_48k_pcm24.wav");
  const double vals[][2] = {{kInputA, 3.5},   {kInputB, -4.0},  {kOutputA, -2.5}, {kOutputB, 1.5},
                            {kPanA, -60.0},   {kPanB, 35.0},    {kBassA, 7.0},    {kMidB, 3.0},
                            {kTrebleA, 6.5},  {kEQActiveB, 0.0}, {kNAMBypassB, 0.0}, {kIRBypassA, 0.0},
                            {kMuteB, 0.0},    {kBlend, -25.0},  {kMasterOutput, -3.0}, {kNoiseGateActive, 1.0},
                            {kNoiseGateThreshold, -70.0}, {kGateAttack, 7.5}, {kGateRelease, 420.0}, {kGatePosition, 0.0}};
  for (const auto& v : vals)
    st.params[(int)v[0]] = v[1];
  Check(Apply(*r1, st), "setState accepted");
  // Change two more via host automation, so state is not just an echo.
  Render(*r1, {Buf(1024, 0.0)}, nullptr, {{kTrebleB, ToNorm(kTrebleB, 8.0)}, {kNAMBypassA, 1.0}});
  std::vector<char> saved;
  Check(r1->plug->getState(saved), "getState");
  DualState back;
  Check(ParseState(saved, back), "saved state parses");
  bool pathsOk = back.nam[0] == st.nam[0] && back.nam[1] == st.nam[1] && back.ir[0] == st.ir[0] && back.ir[1] == st.ir[1];
  Check(pathsOk, "all four file paths are saved");
  bool paramsOk = true;
  for (const auto& v : vals)
    paramsOk &= std::fabs(back.params[(int)v[0]] - v[1]) < 1e-9;
  paramsOk &= std::fabs(back.params[kTrebleB] - 8.0) < 0.051 && back.params[kNAMBypassA] == 1.0;
  Check(paramsOk, "all parameters are saved, including automated ones");

  auto r2 = MakeRig(sr, 256);
  Check(r2->plug->setState(saved), "restore into a fresh instance");
  std::vector<char> saved2;
  r2->plug->getState(saved2);
  Check(saved == saved2, "restored instance saves byte-identical state");
  // Same audio from both: fresh instances, same state, same input.
  auto r3 = MakeRig(sr, 256);
  r3->plug->setState(saved);
  const Buf x = GuitarSignal(48000, sr);
  const Out a = RenderMono(*r2, x), b = RenderMono(*r3, x);
  Check(MaxDiff(a.L, b.L) == 0.0 && MaxDiff(a.R, b.R) == 0.0 && Rms(a.L) > 1e-3,
        "two restores of the same project sound identical (reproducible)");

  {
    // The state now carries format version 2.
    int32_t len = 0, version = 0;
    memcpy(&len, saved.data(), 4);
    memcpy(&version, saved.data() + 4 + len, 4);
    Check(version == 2, "state format version written by 1.2.0: %d", version);
  }

  // ── Projects saved by 1.0.0 and 1.1.0 ─────────────────────────────────────
  // tests/legacy/*.bin were written by the RELEASED 1.0.0 / 1.1.0 plug-ins
  // themselves (dualtests --dump-legacy-state / --dump-legacy-110 against those
  // builds). Expected gate threshold: T_old - max(Input gain of the audible
  // chains), only chain A for the old stereo routing, clamped to -100..0.
  struct Legacy
  {
    const char* file;
    double inA, inB, panB, blend, thrOld, expectThr;
    bool muteA;
    const char* what;
  } legacy[] = {
    {"state_1.0.0_mono_split.bin", 4.5, 0.0, 40.0, -30.0, -80.0, -84.5, false, "1.0.0 Mono Split"},
    {"state_1.0.0_stereo.bin", 4.5, 0.0, 40.0, -30.0, -80.0, -84.5, false, "1.0.0 Stereo (A only)"},
    {"state_1.1.0_mono_left.bin", 6.0, -3.0, 0.0, 0.0, -70.0, -76.0, false, "1.1.0 Mono Left"},
    {"state_1.1.0_mono_left_muteA.bin", 6.0, -3.0, 0.0, 0.0, -70.0, -67.0, true, "1.1.0 Mono Left, A muted"},
    {"state_1.1.0_mono_sum_blendB.bin", 6.0, -3.0, 0.0, 100.0, -70.0, -67.0, false, "1.1.0 Mono Sum, Blend only B"},
    {"state_1.1.0_stereo.bin", 6.0, 9.0, 0.0, 0.0, -70.0, -76.0, false, "1.1.0 Stereo Ind. (A only)"},
    {"state_1.1.0_stereo_muteA.bin", 6.0, 9.0, 0.0, 0.0, -70.0, -70.0, true, "1.1.0 Stereo Ind., A muted"},
    {"state_1.1.0_clamp.bin", 6.0, 0.0, 0.0, 0.0, -98.0, -100.0, false, "1.1.0 T -98, Input +6 (clamped)"},
  };
  for (const auto& l : legacy)
  {
    const fs::path f = gModels.parent_path() / "legacy" / l.file;
    std::ifstream in(f, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
      Check(false, "legacy state file missing: %s", f.string().c_str());
      continue;
    }
    auto rl = MakeRig(sr, 256, 2, 2);
    const bool ok = rl->plug->setState(bytes);
    std::vector<char> now;
    rl->plug->getState(now);
    DualState s;
    ParseState(now, s);
    const bool others = std::fabs(s.params[kInputA] - l.inA) < 1e-9 && std::fabs(s.params[kInputB] - l.inB) < 1e-9 &&
                        std::fabs(s.params[kPanB] - l.panB) < 1e-9 && std::fabs(s.params[kBlend] - l.blend) < 1e-9 &&
                        (s.params[kMuteA] == (l.muteA ? 1.0 : 0.0)) && s.params[kGatePosition] == 1.0 &&
                        std::fabs(s.params[kGateAttack] - 3.0) < 1e-9 && std::fabs(s.params[kGateRelease] - 150.0) < 1e-9;
    Check(ok && std::fabs(s.params[kNoiseGateThreshold] - l.expectThr) < 1e-9 && others,
          "%-32s threshold %5.1f -> %6.1f dB (exp %6.1f); other parameters kept, new gate params at defaults: %s", l.what,
          l.thrOld, s.params[kNoiseGateThreshold], l.expectThr, others ? "yes" : "NO");

    // Saved again (now version 2) and loaded again: no second migration.
    auto r2b = MakeRig(sr, 256, 2, 2);
    r2b->plug->setState(now);
    std::vector<char> again;
    r2b->plug->getState(again);
    DualState s2;
    ParseState(again, s2);
    Check(std::fabs(s2.params[kNoiseGateThreshold] - l.expectThr) < 1e-9 && again == now,
          "  ... saved as 1.2.0 and reloaded: threshold stays %.1f dB, state byte-identical", s2.params[kNoiseGateThreshold]);
  }
  {
    // A new instance is never migrated: defaults straight away.
    auto rl = MakeRig(sr, 256, 2, 2);
    std::vector<char> now;
    rl->plug->getState(now);
    DualState s;
    ParseState(now, s);
    Check(s.params[kNoiseGateThreshold] == -80.0 && s.params[kGateAttack] == 3.0 && s.params[kGateRelease] == 150.0 &&
            s.params[kGatePosition] == 1.0,
          "new instance: threshold -80 dB, attack 3 ms, release 150 ms, Post NAM (no migration)");
    // A 1.2.0 state with a high input gain keeps its threshold as saved.
    DualState st2 = Neutral();
    st2.params[kInputA] = 12.0;
    st2.params[kNoiseGateThreshold] = -60.0;
    auto r4 = MakeRig(sr, 256, 2, 2);
    r4->plug->setState(BuildState(st2));
    r4->plug->getState(now);
    ParseState(now, s);
    Check(s.params[kNoiseGateThreshold] == -60.0, "1.2.0 state with Input A +12 dB: threshold -60 dB not touched");
  }
}

// Writes the processor state of the given (old) plug-in build with fixed,
// non-default parameters. Used once to create tests/legacy/*.bin from the
// released builds; the parameters are set through host automation, exactly as
// a user would have.
static int DumpLegacyStates(const fs::path& outDir)
{
  fs::create_directories(outDir);
  for (int mode = 0; mode < 2; mode++)
  {
    auto r = MakeRig(48000.0, 256, 2, 2);
    if (!r)
      return 1;
    const int idxInputMode = 24; // 1.0.0: "Input Mode", 2 states
    Render(*r, {Buf(256, 0.0), Buf(256, 0.0)}, nullptr,
           {{kInputA, ToNorm(kInputA, 4.5)}, {kPanB, ToNorm(kPanB, 40.0)}, {kBlend, ToNorm(kBlend, -30.0)},
            {idxInputMode, (double)mode}});
    std::vector<char> bytes;
    if (!r->plug->getState(bytes))
      return 1;
    const fs::path f = outDir / (mode == 0 ? "state_1.0.0_mono_split.bin" : "state_1.0.0_stereo.bin");
    std::ofstream(f, std::ios::binary).write(bytes.data(), (std::streamsize)bytes.size());
    std::printf("wrote %s (%zu bytes)\n", f.string().c_str(), bytes.size());
  }
  return 0;
}

static int DumpLegacyStates110(const fs::path& outDir)
{
  fs::create_directories(outDir);
  const int idxSource = 24; // 1.1.0: "Input Source", 4 states (0..3)
  struct D
  {
    const char* file;
    double inA, inB, blend, thr, source;
    bool muteA;
  } dumps[] = {
    {"state_1.1.0_mono_left.bin", 6.0, -3.0, 0.0, -70.0, 0, false},
    {"state_1.1.0_mono_left_muteA.bin", 6.0, -3.0, 0.0, -70.0, 0, true},
    {"state_1.1.0_mono_sum_blendB.bin", 6.0, -3.0, 100.0, -70.0, 2, false},
    {"state_1.1.0_stereo.bin", 6.0, 9.0, 0.0, -70.0, 3, false},
    {"state_1.1.0_stereo_muteA.bin", 6.0, 9.0, 0.0, -70.0, 3, true},
    {"state_1.1.0_clamp.bin", 6.0, 0.0, 0.0, -98.0, 0, false},
  };
  for (const auto& d : dumps)
  {
    auto r = MakeRig(48000.0, 256, 2, 2);
    if (!r)
      return 1;
    Render(*r, {Buf(256, 0.0), Buf(256, 0.0)}, nullptr,
           {{kInputA, ToNorm(kInputA, d.inA)},
            {kInputB, ToNorm(kInputB, d.inB)},
            {kBlend, ToNorm(kBlend, d.blend)},
            {kMuteA, d.muteA ? 1.0 : 0.0},
            {kNoiseGateActive, 1.0},
            {kNoiseGateThreshold, ToNorm(kNoiseGateThreshold, d.thr)},
            {idxSource, d.source / 3.0}});
    std::vector<char> bytes;
    if (!r->plug->getState(bytes))
      return 1;
    const fs::path f = outDir / d.file;
    std::ofstream(f, std::ios::binary).write(bytes.data(), (std::streamsize)bytes.size());
    std::printf("wrote %s (%zu bytes)\n", f.string().c_str(), bytes.size());
  }
  return 0;
}

static void TestBlockSizesAndRates()
{
  Section("10/11  block sizes 64/128/256/512, 44.1 and 48 kHz, no RT heap use");
  for (double sr : {44100.0, 48000.0})
  {
    const Buf x = GuitarSignal((int)sr * 2, sr);
    Out ref;
    for (int block : {512, 256, 128, 64})
    {
      auto r = MakeRig(sr, block);
      DualState st = Neutral();
      st.nam[0] = M("A2.nam");
      st.nam[1] = G("A2_full_variant.nam");
      st.ir[0] = G("ir_lowpass_48k.wav");
      st.ir[1] = G("ir_bright_48k_pcm24.wav");
      st.params[kNoiseGateActive] = 1.0;
      st.params[kEQActiveA] = 1.0;
      Apply(*r, st);
      heaphook::ResetArmed();
      Plug::sOnProcessEnter = ArmOn;
      Plug::sOnProcessLeave = ArmOff;
      const Out o = RenderMono(*r, x);
      Plug::sOnProcessEnter = nullptr;
      Plug::sOnProcessLeave = nullptr;
      const int64_t heapOps = heaphook::ArmedOps();
      if (block == 512)
        ref = o;
      const double d = std::max(MaxDiff(o.L, ref.L), MaxDiff(o.R, ref.R));
      Check(AllFinite(o.L) && AllFinite(o.R) && Rms(o.L) > 1e-3 && heapOps == 0 && d < 1e-5,
            "%5.1f kHz, block %3d: finite, heap ops in process() = %lld, max diff vs block 512 = %.2g", sr / 1000.0,
            block, (long long)heapOps, d);
    }
  }
  // Varying block sizes within one run, and a block larger than announced.
  auto r = MakeRig(48000.0, 256);
  DualState st = Neutral();
  st.nam[0] = M("A2.nam");
  st.nam[1] = G("A2_full_variant.nam");
  Apply(*r, st);
  const Buf x = GuitarSignal(48000, 48000.0);
  std::mt19937 rng(5);
  heaphook::ResetArmed();
  Plug::sOnProcessEnter = ArmOn;
  Plug::sOnProcessLeave = ArmOff;
  const Out v = Render(*r, {x}, [&](int k) { return k == 7 ? 1024 : 1 + (int)(rng() % 256); });
  Plug::sOnProcessEnter = nullptr;
  Plug::sOnProcessLeave = nullptr;
  auto rr = MakeRig(48000.0, 256);
  Apply(*rr, st);
  const Out ref = RenderMono(*rr, x);
  Check(AllFinite(v.L) && heaphook::ArmedOps() == 0 && MaxDiff(v.L, ref.L) < 1e-5,
        "random block sizes 1..256 plus one 1024 block (> max): same output (diff %.2g), heap ops %lld",
        MaxDiff(v.L, ref.L), (long long)heaphook::ArmedOps());
}

static void TestRatesAndLatency()
{
  Section("Ergaenzung 3  sample-rate conversion and latency compensation");
  struct Case
  {
    double hostSR;
    const char *a, *b;
    bool bypassB;
    const char* what;
  } cases[] = {
    {48000.0, "identity_48k.nam", "identity_48k.nam", false, "48k host, both models 48k"},
    {44100.0, "identity_48k.nam", "identity_48k.nam", false, "44.1k host, both models 48k (both resampled)"},
    {48000.0, "identity_48k.nam", "identity_44k.nam", false, "48k host, A 48k native, B 44.1k resampled"},
    {44100.0, "identity_48k.nam", "identity_44k.nam", false, "44.1k host, A 48k resampled, B 44.1k native"},
    {44100.0, "identity_48k.nam", "identity_48k.nam", true, "44.1k host, A resampled, B NAM-bypassed"},
    {96000.0, "identity_48k.nam", "identity_44k.nam", false, "96k host, A 48k, B 44.1k (both resampled)"},
    {88200.0, "identity_48k.nam", "identity_44k.nam", false, "88.2k host, A 48k, B 44.1k (both resampled)"},
  };
  for (const auto& c : cases)
  {
    auto r = MakeRig(c.hostSR, 256);
    DualState st = Neutral();
    st.nam[0] = G(c.a);
    st.nam[1] = G(c.b);
    st.params[kNAMBypassB] = c.bypassB ? 1.0 : 0.0;
    st.params[kPanA] = -100.0;
    st.params[kPanB] = 100.0;
    Apply(*r, st);
    for (int pass = 0; pass < 2; pass++)
    {
    if (pass == 1)
    {
      // Play a bit, then stop and restart processing with the same settings,
      // as a DAW does on every transport start. Delay must not change.
      RenderMono(*r, BandNoise(8192, c.hostSR, 99u));
      std::string err;
      r->plug->setup(c.hostSR, 256, 1, 2, err);
      r->plug->activate(true);
    }
    const int lat = r->plug->latency();
    const Buf x = BandNoise((int)c.hostSR, c.hostSR);
    const Out o = RenderMono(*r, x);
    const size_t skip = 4096;
    const int lagA = BestLag(x, o.L, 128, skip), lagB = BestLag(x, o.R, 128, skip);
    const int lagAB = BestLag(o.L, o.R, 64, skip);
    // Undo the reported latency and compare against the (DC-blocked, panned) input.
    Buf inRef = ReferenceHPF(x, c.hostSR);
    Buf shiftedL(o.L.size(), 0.0), shiftedR(o.R.size(), 0.0);
    for (size_t i = 0; i + (size_t)lat < o.L.size(); i++)
    {
      shiftedL[i] = o.L[i + (size_t)lat];
      shiftedR[i] = o.R[i + (size_t)lat];
    }
    const double nA = NullDB(Buf(shiftedL.begin(), shiftedL.end() - 512), Buf(inRef.begin(), inRef.end() - 512), skip);
    const double nB = NullDB(Buf(shiftedR.begin(), shiftedR.end() - 512), Buf(inRef.begin(), inRef.end() - 512), skip);
    // Delay check with a 1 kHz tone: well inside every resampler's passband, so
    // what is left after undoing the reported latency is misalignment, not the
    // resampler's high-frequency roll-off (which the band noise also contains).
    double toneNull[2];
    {
      auto rt = MakeRig(c.hostSR, 256);
      Apply(*rt, st);
      const Buf tone = Sine((int)c.hostSR / 2, c.hostSR, 1000.0, 0.25);
      const Out ot = RenderMono(*rt, tone);
      const int lt = rt->plug->latency();
      Buf ref = ReferenceHPF(tone, c.hostSR);
      for (int side = 0; side < 2; side++)
      {
        const Buf& y = side ? ot.R : ot.L;
        Buf sh(y.size() - (size_t)lt);
        for (size_t i = 0; i < sh.size(); i++)
          sh[i] = y[i + (size_t)lt];
        toneNull[side] = NullDB(sh, Buf(ref.begin(), ref.begin() + sh.size()), skip);
      }
    }
    // A chain that is not resampled must be an exact delay. A resampled one is
    // limited by the AudioDSPTools Lanczos resampler itself: -38.7 dB at 1 kHz
    // for 44.1<->48k round trips, measured in isolation by
    // tools/coretest/resamplerprobe.cpp (level error 0.0006 dB).
    auto limit = [&](const char* model, bool bypassed) {
      const double modelSR = std::string(model).find("48k") != std::string::npos ? 48000.0 : 44100.0;
      return (bypassed || modelSR == c.hostSR) ? -90.0 : -35.0;
    };
    Check(lagA == lat && lagB == lat && lagAB == 0 && toneNull[0] < limit(c.a, false) && toneNull[1] < limit(c.b, c.bypassB),
          "%-46s%s reported %2d | lag A %2d B %2d A-B %d | 1 kHz null A %.0f B %.0f dB | noise null A %.0f B %.0f dB",
          c.what, pass ? " [re-act]" : "", lat, lagA, lagB, lagAB, toneNull[0], toneNull[1], nA, nB);
    }
  }
  // Real A2 network at a different native rate on each chain.
  for (double sr : {44100.0, 48000.0})
  {
    auto r = MakeRig(sr, 128);
    DualState st = Neutral();
    st.nam[0] = G("A2_full_only.nam");
    st.nam[1] = G("A2_full_44k.nam");
    st.params[kPanA] = -100.0;
    st.params[kPanB] = 100.0;
    Apply(*r, st);
    const Out o = RenderMono(*r, GuitarSignal((int)sr, sr));
    Check(AllFinite(o.L) && AllFinite(o.R) && Rms(o.L) > 1e-3 && Rms(o.R) > 1e-3 && r->plug->latency() > 0,
          "%.1f kHz host, A2 Full at 48k on A and at 44.1k on B: plays, reported latency %d", sr / 1000.0,
          r->plug->latency());
  }
}

static void TestSwitchingStorm()
{
  Section("12 + Ergaenzung 2  rapid model/IR switching while audio runs");
  auto r = MakeRig(48000.0, 128);
  const std::string namList[] = {G("A2_full_only.nam"), G("A2_full_variant.nam"), M("wavenet_a1_standard.nam"),
                                 G("A2_lite_only.nam"), M("A2.nam"), G("identity_44k.nam"), ""};
  const std::string irList[] = {G("ir_lowpass_48k.wav"), G("ir_bright_48k_pcm24.wav"), G("ir_impulse_44k.wav"), ""};

  // Warm-up: touch every code path once so one-time runtime allocations
  // (iostreams, locale, etc.) are not mistaken for leaks.
  {
    DualState st = Neutral();
    st.nam[0] = namList[0];
    st.nam[1] = namList[2];
    st.ir[0] = irList[0];
    st.ir[1] = irList[1];
    Apply(*r, st);
    RenderMono(*r, Buf(4096, 0.0));
    DualState empty = Neutral();
    Apply(*r, empty);
    RenderMono(*r, Buf(1024, 0.0));
    Apply(*r, empty);
    RenderMono(*r, Buf(1024, 0.0));
    Apply(*r, empty);
  }
  const int64_t baseline = heaphook::Outstanding();

  std::atomic<bool> stop{false};
  std::atomic<long long> blocks{0};
  std::atomic<bool> nonFinite{false};
  heaphook::ResetArmed();
  std::thread audio([&]() {
    Plug::sOnProcessEnter = ArmOn;
    Plug::sOnProcessLeave = ArmOff;
    const Buf x = GuitarSignal(128, 48000.0);
    std::vector<const double*> ip = {x.data()};
    Buf l(128), rr(128);
    std::vector<double*> op = {l.data(), rr.data()};
    while (!stop.load())
    {
      r->plug->process(ip.data(), op.data(), 128);
      for (int i = 0; i < 128; i++)
        if (!std::isfinite(l[i]) || !std::isfinite(rr[i]))
          nonFinite = true;
      blocks++;
    }
    Plug::sOnProcessEnter = nullptr;
    Plug::sOnProcessLeave = nullptr;
  });

  const int iterations = gQuick ? 60 : 300;
  std::mt19937 rng(11);
  int loads = 0;
  for (int i = 0; i < iterations; i++)
  {
    DualState st = Neutral();
    st.nam[0] = namList[rng() % 7];
    st.nam[1] = namList[rng() % 7];
    st.ir[0] = irList[rng() % 4];
    st.ir[1] = irList[rng() % 4];
    Apply(*r, st);
    loads++;
    // Mostly back-to-back (several stagings before the audio thread swaps),
    // sometimes a pause long enough for a few blocks.
    if (i % 10 == 9)
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  // Final, known state.
  DualState fin = Neutral();
  fin.nam[0] = G("A2_full_only.nam");
  fin.nam[1] = G("A2_full_variant.nam");
  fin.params[kPanA] = -100.0;
  fin.params[kPanB] = 100.0;
  Apply(*r, fin);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop = true;
  audio.join();

  Check(blocks.load() > 100, "audio thread ran %lld blocks during %d state loads", blocks.load(), loads);
  Check(!nonFinite.load(), "no NaN/Inf in any output block");
  Check(heaphook::gCounts.armedAlloc == 0 && heaphook::gCounts.armedFree == 0 && heaphook::gCounts.armedRealloc == 0,
        "audio thread: %lld allocations, %lld frees, %lld reallocs inside process()",
        (long long)heaphook::gCounts.armedAlloc.load(), (long long)heaphook::gCounts.armedFree.load(),
        (long long)heaphook::gCounts.armedRealloc.load());

  // The final state is the one that plays. (Scoped: the reference instance and
  // its models must be gone before the leak check below counts heap blocks.)
  {
    auto ref = MakeRig(48000.0, 128);
    Apply(*ref, fin);
    const Buf x = GuitarSignal(24000, 48000.0);
    const Out a = RenderMono(*r, x);
    const Out b = RenderMono(*ref, x);
    Check(NullDB(a.L, b.L, 8192) < -60.0 && NullDB(a.R, b.R, 8192) < -60.0,
          "after the storm the last loaded models play (null vs fresh instance A %.1f dB, B %.1f dB)",
          NullDB(a.L, b.L, 8192), NullDB(a.R, b.R, 8192));
  }

  // Leak check: clear everything, let the audio thread retire, let a loader drain.
  DualState empty = Neutral();
  Apply(*r, empty);
  RenderMono(*r, Buf(1024, 0.0));
  Apply(*r, empty);
  RenderMono(*r, Buf(1024, 0.0));
  Apply(*r, empty);
  const int64_t after = heaphook::Outstanding();
  // A block or two may remain from the C runtime's per-thread bookkeeping for the
  // audio thread (seen with 0 loads too); a leak would grow with the number of loads.
  Check(after - baseline <= 2, "no leak: outstanding heap blocks before %lld, after %d switches %lld (diff %lld)",
        (long long)baseline, loads, (long long)after, (long long)(after - baseline));
}

static void TestStormDiag()
{
  Section("diag  where does the storm leave heap blocks?");
  const std::string namList[] = {G("A2_full_only.nam"), G("A2_full_variant.nam"), M("wavenet_a1_standard.nam"),
                                 G("A2_lite_only.nam"), M("A2.nam"), G("identity_44k.nam"), ""};
  const std::string irList[] = {G("ir_lowpass_48k.wav"), G("ir_bright_48k_pcm24.wav"), G("ir_impulse_44k.wav"), ""};
  for (int variant = 0; variant < 4; variant++)
  {
    auto r = MakeRig(48000.0, 128);
    DualState empty = Neutral();
    auto settle = [&]() {
      Apply(*r, empty);
      RenderMono(*r, Buf(1024, 0.0));
      Apply(*r, empty);
      RenderMono(*r, Buf(1024, 0.0));
      Apply(*r, empty);
    };
    DualState w = Neutral();
    w.nam[0] = namList[0]; w.nam[1] = namList[2]; w.ir[0] = irList[0]; w.ir[1] = irList[1];
    Apply(*r, w);
    RenderMono(*r, Buf(1024, 0.0));
    settle();
    const int64_t base = heaphook::Outstanding();
    std::mt19937 rng(11);
    for (int i = 0; i < 40; i++)
    {
      DualState st = Neutral();
      st.nam[0] = namList[rng() % 7];
      st.nam[1] = variant >= 2 ? "" : namList[rng() % 7];
      st.ir[0] = variant == 1 || variant == 3 ? "" : irList[rng() % 4];
      st.ir[1] = variant == 1 || variant == 3 ? "" : irList[rng() % 4];
      Apply(*r, st);
      if (i % 3 == 0)
        RenderMono(*r, Buf(256, 0.0));
    }
    settle();
    std::printf("  variant %d (%s): outstanding diff %+lld\n", variant,
                variant == 0 ? "models+IRs" : variant == 1 ? "models only" : variant == 2 ? "A model + IRs" : "A model only",
                (long long)(heaphook::Outstanding() - base));
  }
  // Same, but with a concurrent audio thread; first without any loads.
  for (int loads : {0, 40})
  {
    auto r = MakeRig(48000.0, 128);
    DualState empty = Neutral();
    Apply(*r, empty);
    RenderMono(*r, Buf(1024, 0.0));
    Apply(*r, empty);
    const int64_t base = heaphook::Outstanding();
    std::atomic<bool> stop{false};
    std::thread audio([&]() {
      const Buf x = GuitarSignal(128, 48000.0);
      std::vector<const double*> ip = {x.data()};
      Buf l(128), rr(128);
      std::vector<double*> op = {l.data(), rr.data()};
      while (!stop.load())
        r->plug->process(ip.data(), op.data(), 128);
    });
    const int64_t threadStarted = heaphook::Outstanding();
    std::mt19937 rng(11);
    for (int i = 0; i < loads; i++)
    {
      DualState st = Neutral();
      st.nam[0] = namList[rng() % 7];
      st.ir[0] = irList[rng() % 4];
      Apply(*r, st);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int64_t beforeJoin = heaphook::Outstanding();
    stop = true;
    audio.join();
    const int64_t afterJoin = heaphook::Outstanding();
    Apply(*r, empty);
    RenderMono(*r, Buf(1024, 0.0));
    Apply(*r, empty);
    std::printf("  concurrent audio, %d loads: after start %+lld, before join %+lld, after join %+lld, settled %+lld\n",
                loads, (long long)(threadStarted - base), (long long)(beforeJoin - base), (long long)(afterJoin - base),
                (long long)(heaphook::Outstanding() - base));
  }
}

static void TestLeakPerFile()
{
  Section("12b  load / unload each file type repeatedly: heap must return to its baseline");
  auto r = MakeRig(48000.0, 128);
  const std::string files[] = {M("A2.nam"), G("A2_full_only.nam"), G("A2_lite_only.nam"), M("wavenet_a1_standard.nam"),
                               M("lstm.nam"), G("identity_44k.nam"), G("ir_lowpass_48k.wav"),
                               G("ir_bright_48k_pcm24.wav"), G("ir_impulse_44k.wav"), G("broken_truncated.nam"),
                               "C:/does/not/exist.nam"};
  auto cycle = [&](const std::string& f) {
    DualState st = Neutral();
    const bool isIR = f.size() > 4 && f.substr(f.size() - 4) == ".wav";
    (isIR ? st.ir[0] : st.nam[0]) = f;
    Apply(*r, st);
    RenderMono(*r, Buf(512, 0.0));
    DualState empty = Neutral();
    Apply(*r, empty);
    RenderMono(*r, Buf(512, 0.0));
    Apply(*r, empty);
  };
  for (const auto& f : files)
  {
    cycle(f); // first time may set up one-time statics
    cycle(f);
    const int64_t base = heaphook::Outstanding();
    const int n = gQuick ? 5 : 20;
    for (int i = 0; i < n; i++)
      cycle(f);
    const int64_t diff = heaphook::Outstanding() - base;
    Check(diff <= 0, "%-28s %2d load/unload cycles: outstanding heap blocks %+lld", fs::path(f).filename().string().c_str(), n,
          (long long)diff);
  }
}

static void TestBrokenFiles()
{
  Section("13  missing and broken files, corrupt state");
  auto r = MakeRig(48000.0, 256);
  const Buf x = GuitarSignal(24000, 48000.0);
  struct Case
  {
    std::string nam, ir;
    const char* what;
  } cases[] = {
    {"C:\\does\\not\\exist\\amp.nam", "C:\\does\\not\\exist\\cab.wav", "missing model and IR"},
    {G("broken_truncated.nam"), G("broken.wav"), "truncated .nam, garbage .wav"},
    {G("broken_garbage.nam"), G("ir_stereo_48k.wav"), "binary garbage .nam, stereo IR"},
    {G("broken_wrong_weights.nam"), G("A2_full_only.nam"), "wrong weight count, .nam given as IR"},
  };
  for (const auto& c : cases)
  {
    DualState st = Neutral();
    st.nam[0] = c.nam;
    st.nam[1] = G("A2_full_only.nam");
    st.ir[0] = c.ir;
    st.params[kPanA] = -100.0;
    st.params[kPanB] = 100.0;
    const bool ok = Apply(*r, st);
    const Out o = RenderMono(*r, x);
    std::vector<char> saved;
    r->plug->getState(saved);
    DualState back;
    ParseState(saved, back);
    Check(ok && AllFinite(o.L) && AllFinite(o.R) && Rms(o.R) > 1e-3 && back.nam[0] == c.nam && back.ir[0] == c.ir,
          "%-40s no crash, other chain plays, paths kept in the project", c.what);
  }
  // Corrupt state chunks: every truncation length and random byte flips.
  DualState good = Neutral();
  good.nam[0] = G("identity_48k.nam");
  const std::vector<char> valid = BuildState(good);
  int survived = 0, total = 0;
  for (size_t len = 0; len < valid.size(); len += (gQuick ? 7 : 1))
  {
    std::vector<char> t(valid.begin(), valid.begin() + len);
    r->plug->setState(t);
    total++;
    survived++;
  }
  std::mt19937 rng(9);
  for (int i = 0; i < (gQuick ? 50 : 300); i++)
  {
    std::vector<char> t = valid;
    for (int k = 0; k < 4; k++)
      t[rng() % t.size()] ^= (char)(1 << (rng() % 8));
    r->plug->setState(t);
    total++;
    survived++;
  }
  const Out o = RenderMono(*r, x);
  Check(survived == total && AllFinite(o.L), "%d truncated / corrupted state chunks: no crash, still processing",
        total);
}

static void TestCPU()
{
  Section("14  CPU load: 2 x A2 Full + 2 IRs");
  // A 2048-tap IR (~43 ms at 48k) is closer to a typical cabinet IR than the tiny test files.
  {
    std::vector<float> ir(2048);
    for (size_t i = 0; i < ir.size(); i++)
      ir[i] = (float)(std::exp(-(double)i / 300.0) * std::sin(0.3 * i));
    const fs::path p = gGen / "ir_2048_48k.wav";
    std::ofstream f(p, std::ios::binary);
    const uint32_t dataLen = (uint32_t)(ir.size() * 4), riffLen = 4 + 8 + 16 + 8 + dataLen;
    const uint16_t fmt = 3, ch = 1, bits = 32, align = 4;
    const uint32_t sr = 48000, br = sr * 4, fmtLen = 16;
    f.write("RIFF", 4); f.write((const char*)&riffLen, 4); f.write("WAVE", 4);
    f.write("fmt ", 4); f.write((const char*)&fmtLen, 4); f.write((const char*)&fmt, 2); f.write((const char*)&ch, 2);
    f.write((const char*)&sr, 4); f.write((const char*)&br, 4); f.write((const char*)&align, 2); f.write((const char*)&bits, 2);
    f.write("data", 4); f.write((const char*)&dataLen, 4); f.write((const char*)ir.data(), dataLen);
  }
  const double seconds = gQuick ? 5.0 : 20.0;

  // Measured on a thread set up the way a DAW sets up its audio thread: MMCSS
  // "Pro Audio", time-critical priority, and opted out of Windows' power
  // throttling (EcoQoS). Without that, on hybrid CPUs (P- and E-cores) Windows
  // moves a plain console thread onto an efficiency core part-way through and
  // the figures say more about the scheduler than about the plugin.
  struct Config
  {
    const char* what;
    bool namA, namB, irs, eqGate;
  };
  auto measure = [&](double sr, int block, const Config& cfg) {
    auto r = MakeRig(sr, block);
    DualState st = Neutral();
    st.nam[0] = M("A2.nam");
    st.nam[1] = G("A2_full_variant.nam");
    st.params[kNAMBypassA] = cfg.namA ? 0.0 : 1.0;
    st.params[kNAMBypassB] = cfg.namB ? 0.0 : 1.0;
    if (cfg.irs)
    {
      st.ir[0] = G("ir_2048_48k.wav");
      st.ir[1] = G("ir_2048_48k.wav");
    }
    st.params[kEQActiveA] = st.params[kEQActiveB] = cfg.eqGate ? 1.0 : 0.0;
    st.params[kNoiseGateActive] = cfg.eqGate ? 1.0 : 0.0;
    Apply(*r, st);
    double load = 0.0, worst = 0.0;
    int misses = 0, nBlocks = 0;
    std::thread t([&]() {
      DWORD taskIndex = 0;
      HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
      THREAD_POWER_THROTTLING_STATE pt{};
      pt.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
      pt.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
      pt.StateMask = 0;
      SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &pt, sizeof(pt));
      const Buf x = GuitarSignal(block, sr);
      std::vector<const double*> ip = {x.data()};
      Buf l((size_t)block), rr((size_t)block);
      std::vector<double*> op = {l.data(), rr.data()};
      for (int i = 0; i < 200; i++)
        r->plug->process(ip.data(), op.data(), block);
      nBlocks = (int)(seconds * sr / block);
      const double blockDur = block / sr;
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < nBlocks; i++)
      {
        const auto b0 = std::chrono::steady_clock::now();
        r->plug->process(ip.data(), op.data(), block);
        const double d = std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count();
        worst = std::max(worst, d);
        if (d > blockDur)
          misses++;
      }
      const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      load = wall / (nBlocks * blockDur);
      worst /= blockDur;
      if (mmcss)
        AvRevertMmThreadCharacteristics(mmcss);
    });
    t.join();
    Check(load < 1.0, "%4.1f kHz block %3d  %-34s %5.1f %% of one core, worst block %4.0f %% of deadline, %d/%d over",
          sr / 1000.0, block, cfg.what, 100.0 * load, 100.0 * worst, misses, nBlocks);
  };

  const Config full{"2x A2 Full + 2x IR(2048) + EQ + gate", true, true, true, true};
  for (double sr : {48000.0, 44100.0})
    for (int block : {64, 128, 256, 512})
      measure(sr, block, full);
  std::printf("  -- breakdown at 48 kHz, block 128\n");
  measure(48000.0, 128, Config{"1x A2 Full only", true, false, false, false});
  measure(48000.0, 128, Config{"2x A2 Full only", true, true, false, false});
  measure(48000.0, 128, Config{"2x IR(2048) only (NAM bypassed)", false, false, true, false});
  measure(48000.0, 128, Config{"EQ + gate only (NAM bypassed)", false, false, false, true});
}

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    std::printf("usage: dualtests <DualNAM_A2_Full.vst3> <repo-root> [--quick]\n");
    return 2;
  }
  gPlugPath = argv[1];
  gModels = fs::path(argv[2]) / "tests" / "models";
  gGen = fs::path(argv[2]) / "tests" / "generated";
  for (int i = 3; i < argc; i++)
  {
    if (std::string(argv[i]) == "--quick")
      gQuick = true;
    else if (std::string(argv[i]).rfind("--only=", 0) == 0)
      gOnly = "," + std::string(argv[i]).substr(7) + ",";
    else if (std::string(argv[i]).rfind("--dump-legacy-state=", 0) == 0)
      return DumpLegacyStates(std::string(argv[i]).substr(20));
    else if (std::string(argv[i]).rfind("--dump-legacy-110=", 0) == 0)
      return DumpLegacyStates110(std::string(argv[i]).substr(18));
  }

  // Keep one instance alive for the whole run: the DLL must stay loaded, or the
  // import-table hook would be lost on the next load.
  auto keeper = MakeRig(48000.0, 256);
  if (!keeper)
    return 1;
  const bool hooked = heaphook::Install(L"DualNAM_A2_Full.vst3");
  std::printf("heap hook installed: %s\n", hooked ? "yes" : "NO");
  if (!hooked)
    return 1;

  struct T
  {
    const char* name;
    void (*fn)();
  } tests[] = {{"a2full", TestA2FullInPlugin}, {"independent", TestIndependentChains}, {"ir", TestIRs},
               {"routing", TestRouting}, {"gate", TestGate}, {"panlaw", TestPanLaw}, {"blendpan", TestBlendPan}, {"gains", TestGains},
               {"bypass", TestBypassMute}, {"state", TestState}, {"blocks", TestBlockSizesAndRates},
               {"latency", TestRatesAndLatency}, {"storm", TestSwitchingStorm}, {"leak", TestLeakPerFile}, {"stormdiag", TestStormDiag},
               {"broken", TestBrokenFiles}, {"cpu", TestCPU}};
  for (const auto& t : tests)
    if (gOnly.empty() || gOnly.find(std::string(",") + t.name + ",") != std::string::npos)
      t.fn();

  std::printf("\n%d passed, %d failed\n", gPass, gFail);
  return gFail;
}
