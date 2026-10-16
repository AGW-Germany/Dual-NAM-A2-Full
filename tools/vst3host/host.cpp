#include "host.h"
#include <algorithm>

static HostApplication gHostApp;

Plug::~Plug()
{
  if (mProcessor)
    mProcessor->setProcessing(false);
  if (mComponent && mActive)
    mComponent->setActive(false);
  mProcessor = nullptr;
  mController = nullptr;
  if (mComponent)
    mComponent->terminate();
  mComponent = nullptr;
}

bool Plug::load(const std::string& path, std::string& err)
{
  mModule = VST3::Hosting::Module::create(path, err);
  if (!mModule)
    return false;

  auto factory = mModule->getFactory();
  for (auto& classInfo : factory.classInfos())
  {
    if (classInfo.category() != kVstAudioEffectClass)
      continue;
    mComponent = factory.createInstance<IComponent>(classInfo.ID());
    if (!mComponent)
      continue;
    if (mComponent->initialize(&gHostApp) != kResultOk)
    {
      err = "IComponent::initialize failed";
      return false;
    }
    mProcessor = FUnknownPtr<IAudioProcessor>(mComponent);
    if (!mProcessor)
    {
      err = "no IAudioProcessor";
      return false;
    }
    // Single-component effect (iPlug2): controller lives on the same object.
    mController = FUnknownPtr<IEditController>(mComponent);
    if (!mController)
    {
      TUID cid;
      if (mComponent->getControllerClassId(cid) == kResultOk)
      {
        mController = factory.createInstance<IEditController>(VST3::UID(cid));
        if (mController)
          mController->initialize(&gHostApp);
      }
    }
    return true;
  }
  err = "no audio effect class found";
  return false;
}

bool Plug::setup(double sampleRate, int maxBlock, int numIn, int numOut, std::string& err)
{
  if (mActive)
  {
    mProcessor->setProcessing(false);
    mComponent->setActive(false);
    mActive = false;
  }

  mNumIn = numIn;
  mNumOut = numOut;
  mMaxBlock = maxBlock;
  mSampleRate = sampleRate;

  SpeakerArrangement inArr = (numIn == 2) ? SpeakerArr::kStereo : SpeakerArr::kMono;
  SpeakerArrangement outArr = (numOut == 2) ? SpeakerArr::kStereo : SpeakerArr::kMono;
  if (mProcessor->setBusArrangements(&inArr, 1, &outArr, 1) != kResultTrue)
  {
    err = "setBusArrangements rejected " + std::to_string(numIn) + "-" + std::to_string(numOut);
    return false;
  }

  // Prefer 64-bit; fall back to 32-bit.
  mSampleSize = kSample64;
  if (mForce32 || mProcessor->canProcessSampleSize(kSample64) != kResultTrue)
    mSampleSize = kSample32;
  if (mSampleSize == kSample32 && mProcessor->canProcessSampleSize(kSample32) != kResultTrue)
    mSampleSize = kSample64; // plugin only does 64-bit

  ProcessSetup setup{};
  setup.processMode = kRealtime;
  setup.symbolicSampleSize = mSampleSize;
  setup.maxSamplesPerBlock = maxBlock;
  setup.sampleRate = sampleRate;
  if (mProcessor->setupProcessing(setup) != kResultOk)
  {
    err = "setupProcessing failed";
    return false;
  }

  mComponent->activateBus(kAudio, kInput, 0, true);
  mComponent->activateBus(kAudio, kOutput, 0, true);

  // Deliberately over-allocated: one test deliberately sends a block larger
  // than the declared max, and the harness must not be what breaks.
  const int cap = maxBlock * 8;
  mInBufD.assign(numIn, std::vector<double>(cap, 0.0));
  mOutBufD.assign(numOut, std::vector<double>(cap, 0.0));
  mInBufF.assign(numIn, std::vector<float>(cap, 0.f));
  mOutBufF.assign(numOut, std::vector<float>(cap, 0.f));
  return true;
}

void Plug::activate(bool on)
{
  mComponent->setActive(on);
  mProcessor->setProcessing(on);
  mActive = on;
}

void (*Plug::sOnProcessEnter)() = nullptr;
void (*Plug::sOnProcessLeave)() = nullptr;

void Plug::process(const double* const* in, double** out, int nFrames,
                   const std::vector<std::pair<int, double>>& paramChanges)
{
  ParameterChanges changes((int32)kNumParams + 8);
  for (auto& pc : paramChanges)
  {
    int32 idx = 0;
    IParamValueQueue* q = changes.addParameterData((ParamID)pc.first, idx);
    if (q)
    {
      int32 pt = 0;
      q->addPoint(0, pc.second, pt);
    }
  }

  if (nFrames > (int)mInBufD[0].size())
    return; // beyond even our over-allocation; nothing sensible to do
  AudioBusBuffers inBus{}, outBus{};
  inBus.numChannels = mNumIn;
  outBus.numChannels = mNumOut;

  std::vector<double*> inPtrD(mNumIn), outPtrD(mNumOut);
  std::vector<float*> inPtrF(mNumIn), outPtrF(mNumOut);

  for (int c = 0; c < mNumIn; c++)
  {
    std::copy(in[c], in[c] + nFrames, mInBufD[c].begin());
    inPtrD[c] = mInBufD[c].data();
    for (int s = 0; s < nFrames; s++)
      mInBufF[c][s] = (float)in[c][s];
    inPtrF[c] = mInBufF[c].data();
  }
  for (int c = 0; c < mNumOut; c++)
  {
    std::fill(mOutBufD[c].begin(), mOutBufD[c].end(), 0.0);
    std::fill(mOutBufF[c].begin(), mOutBufF[c].end(), 0.f);
    outPtrD[c] = mOutBufD[c].data();
    outPtrF[c] = mOutBufF[c].data();
  }

  if (mSampleSize == kSample64)
  {
    inBus.channelBuffers64 = inPtrD.data();
    outBus.channelBuffers64 = outPtrD.data();
  }
  else
  {
    inBus.channelBuffers32 = inPtrF.data();
    outBus.channelBuffers32 = outPtrF.data();
  }

  ProcessContext ctx{};
  ctx.state = ProcessContext::kPlaying;
  ctx.sampleRate = mSampleRate;
  ctx.tempo = 120.0;

  ProcessData data{};
  data.processMode = kRealtime;
  data.symbolicSampleSize = mSampleSize;
  data.numSamples = nFrames;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &inBus;
  data.outputs = &outBus;
  data.inputParameterChanges = &changes;
  data.processContext = &ctx;

  if (Plug::sOnProcessEnter)
    Plug::sOnProcessEnter();
  mProcessor->process(data);
  if (Plug::sOnProcessLeave)
    Plug::sOnProcessLeave();

  for (int c = 0; c < mNumOut; c++)
  {
    if (mSampleSize == kSample64)
      std::copy(mOutBufD[c].begin(), mOutBufD[c].begin() + nFrames, out[c]);
    else
      for (int s = 0; s < nFrames; s++)
        out[c][s] = (double)mOutBufF[c][s];
  }
}

bool Plug::controllerIsComponent() const
{
  return mController && (FUnknown*)mController.get() == (FUnknown*)mComponent.get();
}

int32 Plug::latency()
{
  return mProcessor ? mProcessor->getLatencySamples() : 0;
}

bool Plug::getState(std::vector<char>& outBytes)
{
  MemoryStream s;
  if (mComponent->getState(&s) != kResultOk)
    return false;
  outBytes.assign(s.getData(), s.getData() + s.getSize());
  return true;
}

bool Plug::setState(const std::vector<char>& bytes)
{
  MemoryStream s(const_cast<char*>(bytes.data()), (TSize)bytes.size());
  s.seek(0, IBStream::kIBSeekSet, nullptr);
  bool okC = (mComponent->setState(&s) == kResultOk);
  if (mController)
  {
    s.seek(0, IBStream::kIBSeekSet, nullptr);
    mController->setComponentState(&s);
  }
  return okC;
}

double Plug::getParamNorm(int idx)
{
  return mController ? mController->getParamNormalized((ParamID)idx) : -1.0;
}

void Plug::setParamNormViaController(int idx, double norm)
{
  if (mController)
    mController->setParamNormalized((ParamID)idx, norm);
}

// ── State builder / parser ───────────────────────────────────────────────────
DualState::DualState()
{
  for (int i = 0; i < kNumParams; i++)
    params[i] = std::nan("");
}

std::vector<char> BuildState(const DualState& st)
{
  ChunkBuilder b;
  b.putStr("###DualNAMA2Full###");
  b.putInt32(st.formatVersion);
  b.putStr(st.nam[0]);
  b.putStr(st.nam[1]);
  b.putStr(st.ir[0]);
  b.putStr(st.ir[1]);
  int32_t n = 0;
  for (int i = 0; i < kNumParams; i++)
    if (!std::isnan(st.params[i]))
      n++;
  n += (int32_t)st.extra.size();
  b.putInt32(n);
  for (int i = 0; i < kNumParams; i++)
  {
    if (std::isnan(st.params[i]))
      continue;
    b.putStr(kRanges[i].name);
    b.putDouble(st.params[i]);
  }
  for (const auto& e : st.extra)
  {
    b.putStr(e.first);
    b.putDouble(e.second);
  }
  b.putInt32(0); // bypass, appended by the iPlug2 VST3 wrapper
  return b.bytes();
}

bool ParseState(const std::vector<char>& st, DualState& out)
{
  size_t pos = 0;
  auto getStr = [&](std::string& s) -> bool {
    if (pos + 4 > st.size()) return false;
    int32_t len; memcpy(&len, st.data() + pos, 4); pos += 4;
    if (len < 0 || pos + (size_t)len > st.size()) return false;
    s.assign(st.data() + pos, (size_t)len); pos += (size_t)len; return true;
  };
  auto getI32 = [&](int32_t& v) -> bool {
    if (pos + 4 > st.size()) return false;
    memcpy(&v, st.data() + pos, 4); pos += 4; return true;
  };
  std::string header;
  int32_t version = 0, n = 0;
  if (!getStr(header) || header != "###DualNAMA2Full###" || !getI32(version))
    return false;
  if (!getStr(out.nam[0]) || !getStr(out.nam[1]) || !getStr(out.ir[0]) || !getStr(out.ir[1]) || !getI32(n))
    return false;
  for (int32_t i = 0; i < n; i++)
  {
    std::string name;
    double v;
    if (!getStr(name) || pos + 8 > st.size()) return false;
    memcpy(&v, st.data() + pos, 8); pos += 8;
    for (int p = 0; p < kNumParams; p++)
      if (name == kRanges[p].name)
        out.params[p] = v;
  }
  return true;
}

// ── Signal helpers ───────────────────────────────────────────────────────────
double RMS(const double* x, int n)
{
  double s = 0.0;
  for (int i = 0; i < n; i++)
    s += x[i] * x[i];
  return std::sqrt(s / (double)std::max(1, n));
}
double PeakAbs(const double* x, int n)
{
  double m = 0.0;
  for (int i = 0; i < n; i++)
    m = std::max(m, std::fabs(x[i]));
  return m;
}
double MaxAbsDiff(const double* a, const double* b, int n)
{
  double m = 0.0;
  for (int i = 0; i < n; i++)
    m = std::max(m, std::fabs(a[i] - b[i]));
  return m;
}
bool HasNonFinite(const double* x, int n)
{
  for (int i = 0; i < n; i++)
    if (!std::isfinite(x[i]))
      return true;
  return false;
}
double NullDepthDB(const double* a, const double* b, int n)
{
  std::vector<double> d(n);
  for (int i = 0; i < n; i++)
    d[i] = a[i] - b[i];
  const double rd = RMS(d.data(), n);
  const double ra = RMS(a, n);
  if (ra <= 0.0)
    return rd <= 0.0 ? -999.0 : 0.0;
  if (rd <= 0.0)
    return -999.0;
  return 20.0 * std::log10(rd / ra);
}
