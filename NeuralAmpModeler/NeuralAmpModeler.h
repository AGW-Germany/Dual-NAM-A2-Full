#pragma once

#include "../AudioDSPTools/dsp/ImpulseResponse.h"
#include "../AudioDSPTools/dsp/NoiseGate.h"
#include "../AudioDSPTools/dsp/dsp.h"
#include "../AudioDSPTools/dsp/wav.h"
#include "../AudioDSPTools/dsp/ResamplingContainer/ResamplingContainer.h"
#include "../NeuralAmpModelerCore/NAM/dsp.h"
#include "../NeuralAmpModelerCore/NAM/slimmable.h"

#include "Colors.h"
#include "ToneStack.h"

#include <array>
#include <atomic>
#include <mutex>

#include "IPlug_include_in_plug_hdr.h"
#include "ISender.h"


const int kNumPresets = 1;

// Channel A and channel B. Everything per-channel is indexed with these.
constexpr int kNumChains = 2;
constexpr int kChainA = 0;
constexpr int kChainB = 1;

// Two meter bars: the left and right of the stereo output.
constexpr int kNumMeterChannels = 2;

// Upper bound for the per-channel delay that lines the two chains up when their
// models report different latencies (different native sample rates). The
// resampler latency we have measured is 0 at 48k, 27 at 44.1k and 42 at 96k, so
// this is far above anything real; the delay is clamped to it.
constexpr int kMaxAlignDelay = 512;

class NAMSender : public iplug::IPeakAvgSender<kNumMeterChannels>
{
public:
  NAMSender()
  : iplug::IPeakAvgSender<kNumMeterChannels>(-90.0, true, 5.0f, 1.0f, 5.0f, 0.0f)
  {
  }
};

// Parameters. The state chunk stores them by NAME, so the order here only
// matters for the host's automation list, not for saved projects.
enum EParams
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
  // ID 24: the 1.1.0 input source selection. Reserved and without function
  // since 1.2.0 (fixed routing); kept so that the ID is never reused -- a
  // host automation lane recorded on it must not drive anything else.
  kInputSourceUnused,
  // New in 1.2.0. Appended: existing IDs 0-24 are unchanged.
  kGateAttack, // 25
  kGateRelease, // 26
  kGatePosition, // 27
  kNumParams
};

// Per-chain parameter lookup: kParamInput[kChainB] == kInputB, and so on.
constexpr int kParamInput[kNumChains] = {kInputA, kInputB};
constexpr int kParamOutput[kNumChains] = {kOutputA, kOutputB};
constexpr int kParamPan[kNumChains] = {kPanA, kPanB};
constexpr int kParamBass[kNumChains] = {kBassA, kBassB};
constexpr int kParamMid[kNumChains] = {kMidA, kMidB};
constexpr int kParamTreble[kNumChains] = {kTrebleA, kTrebleB};
constexpr int kParamEQActive[kNumChains] = {kEQActiveA, kEQActiveB};
constexpr int kParamNAMBypass[kNumChains] = {kNAMBypassA, kNAMBypassB};
constexpr int kParamIRBypass[kNumChains] = {kIRBypassA, kIRBypassB};
constexpr int kParamMute[kNumChains] = {kMuteA, kMuteB};

// Values of the 1.1.0 parameter "Input Source", still read from old projects
// (threshold migration). Since 1.2.0 the routing is fixed: mono bus -> its one
// channel, stereo bus -> the left input; both chains at full level.
enum ELegacyInputSource
{
  kLegacyInputMonoLeft = 0,
  kLegacyInputMonoRight,
  kLegacyInputMonoSum,
  kLegacyInputStereo,
};

enum EGatePosition
{
  kGatePre = 0, // gate the mono input before it is split into A and B
  kGatePost, // one shared envelope on both NAM outputs, before EQ and IR
};

// ─────────────────────────────────────────────────────────────────────────────
// Noise gate (1.2.0): one detector, one gain envelope.
//
//   detector  mean-square level of the guitar input, 2 ms time constant
//   decision  open at >= threshold; close once the level has stayed below
//             threshold - 3 dB (hysteresis) for 10 ms (hold)
//   envelope  g in [0, 1]: towards 1 with the attack time, towards 0 with the
//             release time; one-pole, 99 % of the way in the set time
//
// The level is compared in the power domain against precomputed thresholds, so
// the per-sample work is a handful of multiplies and no transcendentals.
// Coefficients are recomputed once per block. No allocation, no latency.
// ─────────────────────────────────────────────────────────────────────────────
class DualNAMGate
{
public:
  static constexpr double kDetectorTimeS = 0.002;
  static constexpr double kHoldS = 0.010;
  static constexpr double kHysteresisDB = 3.0;

  void SetSampleRate(double sampleRate)
  {
    mSampleRate = sampleRate;
    mDetCoeff = std::exp(-1.0 / (kDetectorTimeS * sampleRate));
    mHoldSamples = (int)std::lround(kHoldS * sampleRate);
  }

  void Reset()
  {
    mLevel = 0.0;
    mOpen = false;
    mHoldLeft = 0;
    mEnv = 0.0;
  }

  void SetParams(double thresholdDB, double attackMs, double releaseMs)
  {
    mOpenPow = std::pow(10.0, thresholdDB / 10.0);
    mClosePow = std::pow(10.0, (thresholdDB - kHysteresisDB) / 10.0);
    // ln(0.01): the envelope covers 99 % of the way in the set time.
    const double ln01 = -4.605170185988091;
    mAttackKeep = std::exp(ln01 / (std::max(attackMs, 0.01) * 0.001 * mSampleRate));
    mReleaseKeep = std::exp(ln01 / (std::max(releaseMs, 0.01) * 0.001 * mSampleRate));
  }

  /** Gate gain for n samples of the detector signal x. */
  void Process(const iplug::sample* x, iplug::sample* g, int n)
  {
    for (int s = 0; s < n; s++)
    {
      mLevel = mDetCoeff * mLevel + (1.0 - mDetCoeff) * (x[s] * x[s]);
      if (mLevel >= mOpenPow)
      {
        mOpen = true;
        mHoldLeft = mHoldSamples;
      }
      else if (mOpen)
      {
        if (mLevel >= mClosePow)
          mHoldLeft = mHoldSamples;
        else if (--mHoldLeft <= 0)
          mOpen = false;
      }
      if (mOpen)
        mEnv = 1.0 - mAttackKeep * (1.0 - mEnv);
      else
      {
        mEnv *= mReleaseKeep;
        if (mEnv < 1e-6) // -120 dB: settle at exactly zero
          mEnv = 0.0;
      }
      g[s] = mEnv;
    }
  }

private:
  double mSampleRate = 48000.0;
  double mDetCoeff = 0.0, mLevel = 0.0;
  double mOpenPow = 1e-8, mClosePow = 5e-9;
  double mAttackKeep = 0.0, mReleaseKeep = 0.0;
  double mEnv = 0.0;
  int mHoldSamples = 480, mHoldLeft = 0;
  bool mOpen = false;
};

enum ECtrlTags
{
  kCtrlTagModelFileBrowserA = 0,
  kCtrlTagModelFileBrowserB,
  kCtrlTagIRFileBrowserA,
  kCtrlTagIRFileBrowserB,
  kCtrlTagOutputMeter,
  kNumCtrlTags
};

constexpr int kCtrlTagModelFileBrowser[kNumChains] = {kCtrlTagModelFileBrowserA, kCtrlTagModelFileBrowserB};
constexpr int kCtrlTagIRFileBrowser[kNumChains] = {kCtrlTagIRFileBrowserA, kCtrlTagIRFileBrowserB};

enum EMsgTags
{
  // UI -> DSP
  kMsgTagClearModelA = 0,
  kMsgTagClearModelB,
  kMsgTagClearIRA,
  kMsgTagClearIRB,
  kMsgTagHighlightColor,
  // DSP -> UI
  kMsgTagLoadFailed,
  kMsgTagFileMissing,
  kMsgTagLoadedModel,
  kMsgTagLoadedIR,
  kNumMsgTags
};

constexpr int kMsgTagClearModel[kNumChains] = {kMsgTagClearModelA, kMsgTagClearModelB};
constexpr int kMsgTagClearIR[kNumChains] = {kMsgTagClearIRA, kMsgTagClearIRB};

// Get the sample rate of a NAM model.
// Sometimes, the model doesn't know its own sample rate; this wrapper guesses 48k based on the way that most
// people have used NAM in the past.
inline double GetNAMSampleRate(const std::unique_ptr<nam::DSP>& model)
{
  const double assumedSampleRate = 48000.0;
  const double reportedEncapsulatedSampleRate = model->GetExpectedSampleRate();
  return reportedEncapsulatedSampleRate <= 0.0 ? assumedSampleRate : reportedEncapsulatedSampleRate;
}

using NAMResampler = dsp::ResamplingContainer<NAM_SAMPLE, 1, 12>;

// The real delay of a ResamplingContainer, measured with an impulse through an
// identity "model". NAMResampler::GetLatency() reports the number of samples it
// pre-fills, which is 2 samples short of where the impulse actually comes out
// (measured at 44.1<->48k, 96k and 88.2k; see tools/coretest/resamplerprobe.cpp).
// Lining the two chains up with the reported figure would leave them 2 samples
// apart -- a comb filter with its first notch around 11 kHz.
inline int MeasureResamplerDelay(const double hostRate, const double modelRate, const int maxBlockSize)
{
  NAMResampler probe(modelRate);
  const int block = std::max(1, std::min(maxBlockSize, 256));
  probe.Reset(hostRate, block);
  auto identity = [](NAM_SAMPLE** in, NAM_SAMPLE** out, int n) { std::copy(in[0], in[0] + n, out[0]); };
  std::vector<NAM_SAMPLE> in((size_t)block, 0.0), out((size_t)block, 0.0);
  // Energy centroid of the impulse response = its group delay. (The single
  // largest sample is not a reliable measure: at 2:1 ratios two neighbouring
  // samples are nearly equal.)
  double sumW = 0.0, sumWT = 0.0;
  const int total = 2048;
  for (int pos = 0; pos < total; pos += block)
  {
    std::fill(in.begin(), in.end(), 0.0);
    if (pos == 0)
      in[0] = 1.0;
    NAM_SAMPLE* pIn = in.data();
    NAM_SAMPLE* pOut = out.data();
    probe.ProcessBlock(&pIn, &pOut, block, identity);
    for (int i = 0; i < block; i++)
    {
      const double e = (double)out[(size_t)i] * (double)out[(size_t)i];
      sumW += e;
      sumWT += e * (double)(pos + i);
    }
  }
  return sumW > 0.0 ? (int)std::lround(sumWT / sumW) : 0;
}

class ResamplingNAM : public nam::DSP
{
public:
  // Resampling wrapper around the NAM models
  ResamplingNAM(std::unique_ptr<nam::DSP> encapsulated, const double expected_sample_rate)
  : nam::DSP(encapsulated->NumInputChannels(), encapsulated->NumOutputChannels(), expected_sample_rate)
  , mEncapsulated(std::move(encapsulated))
  {
    // Assign the encapsulated object's processing function  to this object's member so that the resampler can use it:
    auto ProcessBlockFunc = [&](NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) {
      mEncapsulated->process(input, output, numFrames);
    };
    mBlockProcessFunc = ProcessBlockFunc;

    if (mEncapsulated->HasLoudness())
      SetLoudness(mEncapsulated->GetLoudness());
    if (mEncapsulated->HasInputLevel())
      SetInputLevel(mEncapsulated->GetInputLevel());
    if (mEncapsulated->HasOutputLevel())
      SetOutputLevel(mEncapsulated->GetOutputLevel());

    int maxBlockSize = 2048; // Conservative
    Reset(expected_sample_rate, maxBlockSize);
  };

  ~ResamplingNAM() = default;

  void prewarm() override { mEncapsulated->prewarm(); };

  void process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames) override
  {
    if (num_frames > mMaxExternalBlockSize)
      throw std::runtime_error("More frames were provided than the max expected!");

    if (!NeedToResample())
      mEncapsulated->process(input, output, num_frames);
    else
      mResampler->ProcessBlock(input, output, num_frames, mBlockProcessFunc);
  };

  int GetLatency() const { return NeedToResample() ? mLatency : 0; };

  void Reset(const double sampleRate, const int maxBlockSize) override
  {
    mExpectedSampleRate = sampleRate;
    mMaxExternalBlockSize = maxBlockSize;
    // A fresh resampler every time. NAMResampler::Reset() with unchanged
    // settings only clears its buffers and skips the pre-fill it does on a real
    // change, so a second Reset() at the same rate (every transport restart in
    // a DAW) would leave it in a different state -- and with a different delay
    // -- than the first one.
    mResampler = std::make_unique<NAMResampler>(GetEncapsulatedSampleRate());
    mResampler->Reset(sampleRate, maxBlockSize);
    mLatency = NeedToResample() ? MeasureResamplerDelay(sampleRate, GetEncapsulatedSampleRate(), maxBlockSize) : 0;

    // Allocations in the encapsulated model (HACK)
    const double mUpRatio = sampleRate / GetEncapsulatedSampleRate();
    const auto maxEncapsulatedBlockSize = static_cast<int>(std::ceil(static_cast<double>(maxBlockSize) / mUpRatio));
    mEncapsulated->ResetAndPrewarm(sampleRate, maxEncapsulatedBlockSize);
  };

  double GetEncapsulatedSampleRate() const { return GetNAMSampleRate(mEncapsulated); };

  nam::SlimmableModel* GetSlimmableModel() { return dynamic_cast<nam::SlimmableModel*>(mEncapsulated.get()); }

private:
  bool NeedToResample() const { return GetExpectedSampleRate() != GetEncapsulatedSampleRate(); };
  std::unique_ptr<nam::DSP> mEncapsulated;
  std::unique_ptr<NAMResampler> mResampler;
  int mLatency = 0;
  int mMaxExternalBlockSize = 0;
  std::function<void(NAM_SAMPLE**, NAM_SAMPLE**, int)> mBlockProcessFunc;
};

// ─────────────────────────────────────────────────────────────────────────────
// Hand-over of a DSP object (model or IR) from a loader thread to the audio
// thread, without the audio thread ever allocating or freeing.
//
//   loader thread  -- Stage(p)  --> mPending (atomic, one slot, latest wins)
//   audio thread   -- Apply()   --> moves mPending into mLive, pushes the old
//                                   live object into the retire ring
//   non-RT thread  -- Drain()   --> deletes everything in the retire ring
//
// * Several Stage() calls before the audio thread runs: each exchange returns the
//   previous pending object, which the audio thread never saw, so the loader
//   deletes it right there. Only the newest one survives. No leak.
// * Several Apply() swaps before anybody drains: every retired object sits in
//   the ring. If the ring is full the audio thread simply does not swap this
//   block and tries again next block -- it never frees and never drops a pointer.
// * Stage(nullptr) is "clear the slot"; it is carried by a sentinel pointer so a
//   clear and a load can never be reordered.
//
// Single producer (audio thread) / single consumer (whoever holds the owner's
// load mutex) on the ring; any number of loader threads, serialised by that
// same mutex, on the pending slot.
// ─────────────────────────────────────────────────────────────────────────────
template <class T>
class RTHandoff
{
public:
  static constexpr size_t kRetireCapacity = 32;

  RTHandoff() = default;
  RTHandoff(const RTHandoff&) = delete;
  RTHandoff& operator=(const RTHandoff&) = delete;
  ~RTHandoff()
  {
    T* pending = mPending.exchange(nullptr);
    if (pending != nullptr && pending != ClearToken())
      delete pending;
    delete mLive;
    Drain();
  }

  /** Non-RT. Takes ownership; nullptr requests "clear". */
  void Stage(std::unique_ptr<T> p)
  {
    T* raw = p ? p.release() : ClearToken();
    T* superseded = mPending.exchange(raw, std::memory_order_acq_rel);
    if (superseded != nullptr && superseded != ClearToken())
      delete superseded; // never reached the audio thread
    Drain();
  }

  /** RT. Returns true if mLive changed this call. */
  bool Apply()
  {
    // Need room for the object we are about to retire. Space only grows while
    // we are between this check and the push (we are the only producer).
    if (_RetireCount() >= kRetireCapacity)
      return false;
    T* p = mPending.exchange(nullptr, std::memory_order_acq_rel);
    if (p == nullptr)
      return false;
    T* old = mLive;
    mLive = (p == ClearToken()) ? nullptr : p;
    if (old != nullptr)
      _Retire(old);
    return true;
  }

  /** Non-RT. Frees objects the audio thread has finished with. */
  void Drain()
  {
    size_t tail = mTail.load(std::memory_order_relaxed);
    const size_t head = mHead.load(std::memory_order_acquire);
    while (tail != head)
    {
      delete mRing[tail % kRetireCapacity];
      mRing[tail % kRetireCapacity] = nullptr;
      tail++;
    }
    mTail.store(tail, std::memory_order_release);
  }

  /** RT (or non-RT while the audio thread is stopped). */
  T* Live() const { return mLive; }

  /** Non-RT, only while the audio thread is stopped (OnReset). The object that
   * will be live after the next Apply(): the pending one if there is one. */
  T* Newest() const
  {
    T* p = mPending.load(std::memory_order_acquire);
    if (p == ClearToken())
      return nullptr;
    return p != nullptr ? p : mLive;
  }
  /** Same caveat as Newest(): the pending object, if any (not a clear). */
  T* Pending() const
  {
    T* p = mPending.load(std::memory_order_acquire);
    return p == ClearToken() ? nullptr : p;
  }

  /** For tests: objects waiting in the retire ring. */
  size_t RetiredCount() const { return _RetireCount(); }

private:
  static T* ClearToken()
  {
    static char token;
    return reinterpret_cast<T*>(&token);
  }
  size_t _RetireCount() const
  {
    return mHead.load(std::memory_order_acquire) - mTail.load(std::memory_order_acquire);
  }
  void _Retire(T* p)
  {
    const size_t head = mHead.load(std::memory_order_relaxed);
    mRing[head % kRetireCapacity] = p;
    mHead.store(head + 1, std::memory_order_release);
  }

  std::atomic<T*> mPending{nullptr};
  T* mLive = nullptr;
  std::array<T*, kRetireCapacity> mRing{};
  std::atomic<size_t> mHead{0}; // written by the audio thread
  std::atomic<size_t> mTail{0}; // written by the drainer
};

// Fixed-size delay line used to line the two chains up. Sized off the audio
// thread; Process() never allocates.
class AlignDelay
{
public:
  void Prepare()
  {
    mRing.assign(kMaxAlignDelay + 1, 0.0);
    mWrite = 0;
  }
  void Reset()
  {
    std::fill(mRing.begin(), mRing.end(), 0.0);
    mWrite = 0;
  }
  void Process(iplug::sample* x, const int nFrames, const int delay)
  {
    const int size = (int)mRing.size();
    if (size == 0)
      return;
    const int d = std::max(0, std::min(delay, size - 1));
    if (d == 0 && mLastDelay == 0)
    {
      // Keep the history current so a later delay change reads real signal.
      for (int s = 0; s < nFrames; s++)
      {
        mRing[mWrite] = x[s];
        if (++mWrite >= size)
          mWrite = 0;
      }
      return;
    }
    mLastDelay = d;
    for (int s = 0; s < nFrames; s++)
    {
      mRing[mWrite] = x[s];
      int r = mWrite - d;
      if (r < 0)
        r += size;
      x[s] = mRing[r];
      if (++mWrite >= size)
        mWrite = 0;
    }
  }

private:
  std::vector<iplug::sample> mRing;
  int mWrite = 0;
  int mLastDelay = 0;
};

// One processing chain: Input Gain -> NAM -> EQ -> IR -> DC blocker -> align ->
// Output Gain / Blend / Mute -> Pan.
struct Chain
{
  RTHandoff<ResamplingNAM> model;
  RTHandoff<dsp::ImpulseResponse> ir;
  std::unique_ptr<dsp::tone_stack::AbstractToneStack> toneStack;
  recursive_linear_filter::HighPass highPass;
  AlignDelay align;

  // Buffers, sized in OnReset to the host's maximum block.
  std::vector<iplug::sample> in, namOut;

  // Gain targets (written from OnParamChange) and the values the audio thread
  // has reached. Glided across one block; negative == snap on the next block.
  double inputGain = 1.0, inputGainCurrent = -1.0;
  double outL = 1.0, outR = 1.0; // output * blend * mute * pan, per side
  double outLCurrent = -1.0, outRCurrent = -1.0;

  // Non-RT bookkeeping, guarded by the plugin's load mutex.
  WDL_String namPath, irPath;
  // The path is kept even when its file could not be loaded (e.g. a project
  // opened on a machine where the file is missing), so saving the project again
  // does not silently drop it. These say why the slot is empty.
  bool namFailed = false, irFailed = false;
  dsp::ImpulseResponse::IRData irData;
  bool haveIRData = false;
  // Latency of the newest staged model (0 if none). Read by the audio thread.
  std::atomic<int> modelLatency{0};
};

class NeuralAmpModeler final : public iplug::Plugin
{
public:
  NeuralAmpModeler(const iplug::InstanceInfo& info);
  ~NeuralAmpModeler();

  void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;
  void OnReset() override;
  void OnIdle() override;

  bool SerializeState(iplug::IByteChunk& chunk) const override;
  int UnserializeState(const iplug::IByteChunk& chunk, int startPos) override;
  void OnUIOpen() override;
  bool OnHostRequestingSupportedViewConfiguration(int width, int height) override { return true; }

  void OnParamChange(int paramIdx) override;
  void OnParamChangeUI(int paramIdx, iplug::EParamSource source) override;
  bool OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData) override;

  // Loading. Callable from any non-audio thread; serialised internally.
  // Returns an empty string on success, or an error message.
  std::string LoadModel(int chain, const WDL_String& path);
  std::string LoadIR(int chain, const WDL_String& path);
  void ClearModel(int chain);
  void ClearIR(int chain);

private:
  // Unlocked versions; the caller holds mLoadMutex.
  std::string _LoadModel(int chain, const WDL_String& path, bool fromState);
  std::string _LoadIR(int chain, const WDL_String& path, bool fromState);
  // Build an IR object at the current host rate and run one silent block
  // through it so its buffers exist before the audio thread sees it.
  std::unique_ptr<dsp::ImpulseResponse> _MakeIR(const dsp::ImpulseResponse::IRData& data) const;
  // Reset, prime one silent block (so lazily sized buffers exist), reset again.
  void _PrepareModel(ResamplingNAM& model, double sampleRate, int maxBlock) const;

  void _ApplyStaging();
  void _ProcessSubBlock(iplug::sample** inputs, iplug::sample** outputs, int offset, int nFrames, int nIn, int nOut);
  void _PrepareBuffers(int maxBlock);
  void _UpdateGains();
  void _UpdateReportedLatency();
  void _SetToneStackParam(int chain, int paramIdx);
  // Pre-1.2.0 states: move the gate threshold to the new detector position.
  void _MigrateGateThreshold(const std::vector<std::pair<std::string, double>>& saved,
                             std::vector<std::pair<int, double>>& values) const;

  // Max block the buffers are sized for. Larger host blocks are split.
  int mMaxBlock = 0;
  double mPreparedSampleRate = 0.0;

  std::array<Chain, kNumChains> mChains;

  // Shared input stage and the noise gate.
  std::vector<iplug::sample> mMonoIn, mGateEnv, mGatePre, mGatePost;
  std::vector<iplug::sample> mMixL, mMixR;
  DualNAMGate mGate;
  // How much of the gate is applied (0 = none, 1 = all), ramped over
  // kGateFadeS. Turning the gate on/off and moving it between Pre and Post
  // both go through 0, so neither ever steps the signal.
  static constexpr double kGateFadeS = 0.010;
  double mGateDepth = 0.0;
  int mGatePositionActive = kGatePost;
  bool mGatePostActive = false; // this sub-block applies a Post envelope

  double mMasterGain = 1.0, mMasterGainCurrent = -1.0;

  // Latency reported to the host: the larger of the two chains' model
  // latencies. The audio thread pads the other chain up to it.
  std::atomic<int> mReportedLatency{0};
  std::atomic<bool> mLatencyDirty{false};

  // Serialises every load / clear / state restore and the retire drain.
  mutable std::mutex mLoadMutex;

  WDL_String mHighLightColor{PluginColors::NAM_THEMECOLOR.ToColorCode()};

  NAMSender mOutputSender;
};
