// Minimal VST3 host harness for testing Dual NAM A2 Full.
#pragma once

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// ── Parameter indices, mirroring NeuralAmpModeler.h EParams ──────────────────
enum EP
{
  kInputA = 0,
  kInputB,
  kOutputA,
  kOutputB,
  kPanA,
  kPanB,
  kBassA,
  kMidA,
  kTrebleA,
  kBassB,
  kMidB,
  kTrebleB,
  kEQActiveA,
  kEQActiveB,
  kNAMBypassA,
  kNAMBypassB,
  kIRBypassA,
  kIRBypassB,
  kMuteA,
  kMuteB,
  kBlend,
  kMasterOutput,
  kNoiseGateActive,
  kNoiseGateThreshold,
  kInputSourceUnused, // ID 24, reserved since 1.2.0
  kGateAttack, // 25
  kGateRelease, // 26
  kGatePosition, // 27
  kNumParams
};

// Values of "Gate Position" (ID 27).
enum EGatePos
{
  kGatePre = 0,
  kGatePost = 1,
};

// Param ranges as Init'd in the plugin, so the harness can convert real <-> normalized.
// NOTE: Gate Attack / Gate Release use a power-curve shape in the plugin, so
// ToNorm/FromNorm are only correct for the linear parameters. Set those two
// through the state (real values) instead.
struct ParamRange
{
  double lo, hi;
  const char* name;
};
static const ParamRange kRanges[kNumParams] = {
  {-20.0, 20.0, "Input A"},      {-20.0, 20.0, "Input B"},       {-40.0, 40.0, "Output A"},
  {-40.0, 40.0, "Output B"},     {-100.0, 100.0, "Pan A"},       {-100.0, 100.0, "Pan B"},
  {0.0, 10.0, "Bass A"},         {0.0, 10.0, "Middle A"},        {0.0, 10.0, "Treble A"},
  {0.0, 10.0, "Bass B"},         {0.0, 10.0, "Middle B"},        {0.0, 10.0, "Treble B"},
  {0.0, 1.0, "EQ A"},            {0.0, 1.0, "EQ B"},             {0.0, 1.0, "NAM Bypass A"},
  {0.0, 1.0, "NAM Bypass B"},    {0.0, 1.0, "IR Bypass A"},      {0.0, 1.0, "IR Bypass B"},
  {0.0, 1.0, "Mute A"},          {0.0, 1.0, "Mute B"},           {-100.0, 100.0, "Blend"},
  {-40.0, 12.0, "Master"},       {0.0, 1.0, "Gate"},             {-100.0, 0.0, "Gate Threshold"},
  {0.0, 3.0, "Input Source (unused)"}, {0.1, 50.0, "Gate Attack"},
  {10.0, 1000.0, "Gate Release"}, {0.0, 1.0, "Gate Position"},
};

inline double ToNorm(int p, double real)
{
  const auto& r = kRanges[p];
  double v = (real - r.lo) / (r.hi - r.lo);
  return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}
inline double FromNorm(int p, double norm)
{
  const auto& r = kRanges[p];
  return r.lo + norm * (r.hi - r.lo);
}

// ── Plugin wrapper ───────────────────────────────────────────────────────────
class Plug
{
public:
  bool load(const std::string& path, std::string& err);
  bool setup(double sampleRate, int maxBlock, int numIn, int numOut, std::string& err);
  void activate(bool on);

  // Process one block. in/out are numIn/numOut arrays of length nFrames.
  // paramChanges: list of (paramIdx, normalizedValue) applied at sample 0.
  void process(const double* const* in, double** out, int nFrames,
               const std::vector<std::pair<int, double>>& paramChanges = {});

  // Called immediately around the plugin's own IAudioProcessor::process() and
  // nothing else. Null by default, so there is no cost for suites that do not
  // use them. rtalloc needs this: Plug::process() allocates on every call
  // (ParameterChanges plus four vector temporaries), so arming an allocation
  // counter around the whole wrapper measures the harness, not the plugin.
  static void (*sOnProcessEnter)();
  static void (*sOnProcessLeave)();

  bool getState(std::vector<char>& outBytes);
  bool setState(const std::vector<char>& bytes);

  // Read a param's normalized value back from the edit controller.
  double getParamNorm(int idx);
  // Set via the controller (the "UI" path), not automation.
  void setParamNormViaController(int idx, double norm);

  int numIn() const { return mNumIn; }
  int numOut() const { return mNumOut; }
  int32 latency();
  bool controllerIsComponent() const;
  bool usingDouble() const { return mSampleSize == kSample64; }
  // Force the 32-bit path on the next setup() (hosts vary; both must work).
  void forceSampleSize32() { mForce32 = true; }
  bool ok() const { return mProcessor != nullptr; }

  // Raw interface access, for suites that must work against a plugin whose
  // parameter layout is not known ahead of time (rival.cpp). Everything else
  // should go through the typed helpers above.
  IEditController* controller() const { return mController; }
  IAudioProcessor* processor() const { return mProcessor; }
  IComponent* component() const { return mComponent; }

  ~Plug();

private:
  std::shared_ptr<VST3::Hosting::Module> mModule;
  IPtr<IComponent> mComponent;
  IPtr<IAudioProcessor> mProcessor;
  IPtr<IEditController> mController;
  int mNumIn = 0, mNumOut = 0, mMaxBlock = 0;
  double mSampleRate = 48000.0;
  int32 mSampleSize = kSample64;
  bool mForce32 = false;
  bool mActive = false;

  std::vector<std::vector<double>> mInBufD, mOutBufD;
  std::vector<std::vector<float>> mInBufF, mOutBufF;
};

// ── State chunk builder (mirrors NeuralAmpModeler::SerializeState) ───────────
class ChunkBuilder
{
public:
  void putStr(const std::string& s)
  {
    int32_t len = (int32_t)s.size();
    putRaw(&len, sizeof(len));
    putRaw(s.data(), len);
  }
  void putDouble(double d) { putRaw(&d, sizeof(d)); }
  void putInt32(int32_t i) { putRaw(&i, sizeof(i)); }
  const std::vector<char>& bytes() const { return mBytes; }

private:
  void putRaw(const void* p, size_t n)
  {
    const char* c = (const char*)p;
    mBytes.insert(mBytes.end(), c, c + n);
  }
  std::vector<char> mBytes;
};

struct DualState
{
  std::string nam[2], ir[2];
  // REAL parameter values by enum index; NaN = leave out of the chunk.
  double params[kNumParams];
  // Extra (name, value) pairs written after the known ones, e.g. the 1.0.0
  // parameter "Input Mode", to build old-format projects.
  std::vector<std::pair<std::string, double>> extra;
  // 1 = written by 1.0.0 / 1.1.0 (gets the gate threshold migrated on load),
  // 2 = written by 1.2.0.
  int formatVersion = 2;
  DualState();
};

// header, format version, 4 paths, param count, (name, value) pairs, int32 bypass.
std::vector<char> BuildState(const DualState& st);
// Parse the processor's own state: paths and real param values by name.
bool ParseState(const std::vector<char>& bytes, DualState& out);

// ── Signal helpers ───────────────────────────────────────────────────────────
double RMS(const double* x, int n);
double PeakAbs(const double* x, int n);
double MaxAbsDiff(const double* a, const double* b, int n);
bool HasNonFinite(const double* x, int n);
double NullDepthDB(const double* a, const double* b, int n);
