#include <algorithm> // std::clamp, std::min
#include <cmath> // pow
#include <filesystem>
#include <iostream>
#include <utility>

#include "Colors.h"
#include "../NeuralAmpModelerCore/NAM/activations.h"
#include "../NeuralAmpModelerCore/NAM/get_dsp.h"
// clang-format off
// These includes need to happen in this order or else the latter won't know
// a bunch of stuff.
#include "NeuralAmpModeler.h"
#include "IPlug_include_in_plug_src.h"
// clang-format on
#include "architecture.hpp"

#include "NeuralAmpModelerControls.h"

using namespace iplug;
using namespace igraphics;

const double kDCBlockerFrequency = 5.0;

// State chunk identification. The format is our own (this plugin has its own
// IDs, so it never receives Gateway state); see SerializeState().
static const char* const kStateHeader = "###DualNAMA2Full###";
// 1: versions 1.0.0 and 1.1.0. 2: since 1.2.0 (gate detector before the input
// gains; older states get their gate threshold migrated once, on load).
static const int32_t kStateFormatVersion = 2;

// Channel colours. A: the original Gateway / Neural Amp Modeler theme blue
// (NAM_THEMECOLOR in NeuralAmpModelerPlugin's Colors.h, "Azure" #5085E8; here
// PluginColors::NAM_2). B: a warm sun yellow, #F2C448.
static const IColor kChainColor[kNumChains] = {PluginColors::NAM_2, IColor(255, 242, 196, 72)};
static const IColor kNeutralColor(255, 200, 200, 205);

static IVStyle MakeStyle(const IColor& color, float labelSize = 14.f, float valueSize = 13.f)
{
  const IVColorSpec colorSpec{
    DEFAULT_BGCOLOR, // Background
    color, // Foreground
    color.WithOpacity(0.3f), // Pressed
    color.WithOpacity(0.4f), // Frame
    PluginColors::MOUSEOVER, // Highlight
    DEFAULT_SHCOLOR, // Shadow
    color, // Extra 1
    COLOR_RED, // Extra 2
    color.WithContrast(0.1f), // Extra 3
  };
  return IVStyle{true, // Show label
                 true, // Show value
                 colorSpec,
                 {labelSize, EVAlign::Middle, PluginColors::NAM_THEMEFONTCOLOR}, // Knob label text
                 {valueSize, EVAlign::Bottom, PluginColors::NAM_THEMEFONTCOLOR}, // Knob value text
                 DEFAULT_HIDE_CURSOR,
                 DEFAULT_DRAW_FRAME,
                 false,
                 DEFAULT_EMBOSS,
                 0.2f,
                 2.f,
                 DEFAULT_SHADOW_OFFSET,
                 DEFAULT_WIDGET_FRAC,
                 DEFAULT_WIDGET_ANGLE};
}

static const IVStyle titleStyle =
  DEFAULT_STYLE.WithValueText(IText(28, COLOR_WHITE, "Michroma-Regular")).WithDrawFrame(false).WithShadowOffset(2.f);

static EMsgBoxResult _ShowMessageBox(iplug::igraphics::IGraphics* pGraphics, const char* str, const char* caption,
                                     EMsgBoxType type)
{
  if (pGraphics == nullptr)
    return kNoResult;
#ifdef OS_MAC
  return pGraphics->ShowMessageBox(caption, str, type);
#else
  return pGraphics->ShowMessageBox(str, caption, type);
#endif
}

// Blend: -100 = only A, 0 = both at full level, +100 = only B.
static void BlendGains(const double blend, double& gA, double& gB)
{
  const double b = std::clamp((blend + 100.0) / 200.0, 0.0, 1.0);
  gA = std::min(1.0, 2.0 * (1.0 - b));
  gB = std::min(1.0, 2.0 * b);
}

// Constant-power (sine/cosine) pan: hard left = 0 dB left / silent right,
// centre = -3.01 dB on both sides, hard right = silent left / 0 dB right.
static void PanGains(const double pan, double& gL, double& gR)
{
  const double p = std::clamp(pan / 100.0, -1.0, 1.0);
  // Exact at the ends: cos(pi/2) is 6e-17, not 0, and a hard-panned channel
  // must not leak into the other side at all.
  if (p <= -1.0)
  {
    gL = 1.0;
    gR = 0.0;
    return;
  }
  if (p >= 1.0)
  {
    gL = 0.0;
    gR = 1.0;
    return;
  }
  const double theta = (p + 1.0) * 0.25 * 3.14159265358979323846;
  gL = std::cos(theta);
  gR = std::sin(theta);
}

NeuralAmpModeler::NeuralAmpModeler(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  nam::activations::Activation::enable_fast_tanh();

  GetParam(kInputA)->InitGain("Input A", 0.0, -20.0, 20.0, 0.1);
  GetParam(kInputB)->InitGain("Input B", 0.0, -20.0, 20.0, 0.1);
  GetParam(kOutputA)->InitGain("Output A", 0.0, -40.0, 40.0, 0.1);
  GetParam(kOutputB)->InitGain("Output B", 0.0, -40.0, 40.0, 0.1);
  auto panDisplay = [](double v, WDL_String& str) {
    if (std::fabs(v) < 0.5)
      str.Set("C");
    else
      str.SetFormatted(16, "%s%.0f", v < 0.0 ? "L" : "R", std::fabs(v));
  };
  GetParam(kPanA)->InitDouble("Pan A", 0.0, -100.0, 100.0, 1.0, "", 0, "", IParam::ShapeLinear(),
                              IParam::kUnitCustom, panDisplay);
  GetParam(kPanB)->InitDouble("Pan B", 0.0, -100.0, 100.0, 1.0, "", 0, "", IParam::ShapeLinear(),
                              IParam::kUnitCustom, panDisplay);
  GetParam(kBassA)->InitDouble("Bass A", 5.0, 0.0, 10.0, 0.1);
  GetParam(kMidA)->InitDouble("Middle A", 5.0, 0.0, 10.0, 0.1);
  GetParam(kTrebleA)->InitDouble("Treble A", 5.0, 0.0, 10.0, 0.1);
  GetParam(kBassB)->InitDouble("Bass B", 5.0, 0.0, 10.0, 0.1);
  GetParam(kMidB)->InitDouble("Middle B", 5.0, 0.0, 10.0, 0.1);
  GetParam(kTrebleB)->InitDouble("Treble B", 5.0, 0.0, 10.0, 0.1);
  GetParam(kEQActiveA)->InitBool("EQ A", true);
  GetParam(kEQActiveB)->InitBool("EQ B", true);
  GetParam(kNAMBypassA)->InitBool("NAM Bypass A", false);
  GetParam(kNAMBypassB)->InitBool("NAM Bypass B", false);
  GetParam(kIRBypassA)->InitBool("IR Bypass A", false);
  GetParam(kIRBypassB)->InitBool("IR Bypass B", false);
  GetParam(kMuteA)->InitBool("Mute A", false);
  GetParam(kMuteB)->InitBool("Mute B", false);
  GetParam(kBlend)->InitDouble("Blend", 0.0, -100.0, 100.0, 1.0, "", 0, "", IParam::ShapeLinear(),
                               IParam::kUnitCustom, [](double v, WDL_String& str) {
                                 double gA, gB;
                                 BlendGains(v, gA, gB);
                                 str.SetFormatted(32, "A%.0f B%.0f", 100.0 * gA, 100.0 * gB);
                               });
  GetParam(kMasterOutput)->InitGain("Master", 0.0, -40.0, 12.0, 0.1);
  GetParam(kNoiseGateActive)->InitBool("Gate", false);
  GetParam(kNoiseGateThreshold)->InitGain("Gate Threshold", -80.0, -100.0, 0.0, 0.1);
  // ID 24 is reserved: it was "Input Source" in 1.1.0. No function, not
  // automatable (iPlug2 then omits kCanAutomate in the VST3 parameter info).
  GetParam(kInputSourceUnused)
    ->InitEnum("Input Source (unused)", 0, {"Mono Left", "Mono Right", "Mono Sum", "Stereo Independent"},
               IParam::kFlagCannotAutomate);
  GetParam(kGateAttack)->InitDouble("Gate Attack", 3.0, 0.1, 50.0, 0.1, "ms", 0, "", IParam::ShapePowCurve(3.0));
  GetParam(kGateRelease)->InitDouble("Gate Release", 150.0, 10.0, 1000.0, 1.0, "ms", 0, "", IParam::ShapePowCurve(3.0));
  GetParam(kGatePosition)->InitEnum("Gate Position", kGatePost, {"Pre NAM", "Post NAM"});

  for (auto& ch : mChains)
    ch.toneStack = std::make_unique<dsp::tone_stack::BasicNamToneStack>();

  _UpdateGains();

  mMakeGraphicsFunc = [&]() { return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, 1.0f); };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachTextEntryControl();
    pGraphics->EnableMouseOver(true);
    pGraphics->EnableTooltips(true);

    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    pGraphics->LoadFont("Michroma-Regular", MICHROMA_FN);

    const auto fileSVG = pGraphics->LoadSVG(FILE_FN);
    const auto globeSVG = pGraphics->LoadSVG(GLOBE_ICON_FN);
    const auto crossSVG = pGraphics->LoadSVG(CLOSE_BUTTON_FN);
    const auto rightArrowSVG = pGraphics->LoadSVG(RIGHT_ARROW_FN);
    const auto leftArrowSVG = pGraphics->LoadSVG(LEFT_ARROW_FN);
    const auto modelIconSVG = pGraphics->LoadSVG(MODEL_ICON_FN);
    const auto irIconSVG = pGraphics->LoadSVG(IR_ICON_ON_FN);

    const auto fileBackgroundBitmap = pGraphics->LoadBitmap(FILEBACKGROUND_FN);
    const auto knobBackgroundBitmap = pGraphics->LoadBitmap(KNOBBACKGROUND_FN);
    const auto switchHandleBitmap = pGraphics->LoadBitmap(SLIDESWITCHHANDLE_FN);

    const auto b = pGraphics->GetBounds();
    // Plain dark gradient. (The NAM background artwork is drawn for 600x400 and
    // shows a seam across the bottom panels when stretched to this size.)
    pGraphics->AttachControl(new IPanelControl(
      b, IPattern::CreateLinearGradient(b, EDirection::Vertical,
                                        {{IColor(255, 40, 40, 44), 0.f}, {IColor(255, 16, 16, 18), 1.f}})));
    pGraphics->AttachControl(new IVLabelControl(IRECT(0.f, 12.f, b.R, 58.f), "DUAL NAM A2 FULL", titleStyle));

    const char* const getUrl = "https://www.neuralampmodeler.com/users#comp-marb84o5";
    const char* const chainName[kNumChains] = {"CHANNEL A", "CHANNEL B"};
    const char* const eqGroup[kNumChains] = {"EQ_KNOBS_A", "EQ_KNOBS_B"};

    // ── The two channel panels ────────────────────────────────────────────────
    const float panelT = 66.f, panelH = 340.f, panelGap = 10.f, margin = 20.f;
    const float panelW = 0.5f * (b.W() - 2.f * margin - panelGap);
    for (int c = 0; c < kNumChains; c++)
    {
      const IVStyle style = MakeStyle(kChainColor[c]);
      const float left = margin + (float)c * (panelW + panelGap);
      const IRECT panel(left, panelT, left + panelW, panelT + panelH);
      pGraphics->AttachControl(new DualNAMPanelControl(panel, chainName[c], kChainColor[c]));

      const IRECT modelArea(panel.L + 46.f, panel.T + 40.f, panel.R - 12.f, panel.T + 72.f);
      const IRECT irArea = modelArea.GetVShifted(40.f);
      pGraphics->AttachControl(new ISVGControl(IRECT(panel.L + 12.f, modelArea.T + 2.f, panel.L + 40.f, modelArea.B - 2.f), modelIconSVG));
      pGraphics->AttachControl(new ISVGControl(IRECT(panel.L + 12.f, irArea.T + 2.f, panel.L + 40.f, irArea.B - 2.f), irIconSVG));

      auto loadModel = [this, c](const WDL_String& fileName, const WDL_String& path) {
        if (fileName.GetLength() == 0)
          return;
        const std::string msg = LoadModel(c, fileName);
        if (!msg.empty())
        {
          const std::string text = std::string("Could not load the NAM model for channel ") + (c == kChainA ? "A" : "B")
                                   + ":\n\n" + fileName.Get() + "\n\n" + msg;
          _ShowMessageBox(GetUI(), text.c_str(), "Failed to load model", kMB_OK);
        }
      };
      auto loadIR = [this, c](const WDL_String& fileName, const WDL_String& path) {
        if (fileName.GetLength() == 0)
          return;
        const std::string msg = LoadIR(c, fileName);
        if (!msg.empty())
        {
          const std::string text = std::string("Could not load the IR for channel ") + (c == kChainA ? "A" : "B")
                                   + ":\n\n" + fileName.Get() + "\n\n" + msg;
          _ShowMessageBox(GetUI(), text.c_str(), "Failed to load IR", kMB_OK);
        }
      };

      pGraphics->AttachControl(new NAMFileBrowserControl(modelArea, kMsgTagClearModel[c], "Select NAM model...", "nam",
                                                         loadModel, style, fileSVG, crossSVG, leftArrowSVG,
                                                         rightArrowSVG, fileBackgroundBitmap, globeSVG,
                                                         "Get NAM Models", getUrl),
                               kCtrlTagModelFileBrowser[c]);
      pGraphics->AttachControl(new NAMFileBrowserControl(irArea, kMsgTagClearIR[c], "Select IR...", "wav", loadIR,
                                                         style, fileSVG, crossSVG, leftArrowSVG, rightArrowSVG,
                                                         fileBackgroundBitmap, globeSVG, "Get IRs", getUrl),
                               kCtrlTagIRFileBrowser[c]);

      // Knob row: Input, Bass, Middle, Treble, Output, Pan.
      const IRECT knobRow(panel.L + 6.f, panel.T + 122.f, panel.R - 6.f, panel.T + 238.f);
      const int knobParams[6] = {kParamInput[c], kParamBass[c], kParamMid[c], kParamTreble[c], kParamOutput[c],
                                 kParamPan[c]};
      const char* const knobLabels[6] = {"Input", "Bass", "Middle", "Treble", "Output", "Pan"};
      for (int k = 0; k < 6; k++)
      {
        const IRECT cell = knobRow.GetGridCell(0, k, 1, 6);
        const bool isEQ = (k >= 1 && k <= 3);
        pGraphics->AttachControl(
          new NAMKnobControl(cell, knobParams[k], knobLabels[k], style, knobBackgroundBitmap), -1,
          isEQ ? eqGroup[c] : "");
      }

      // Switch row: EQ, NAM Bypass, IR Bypass, Mute.
      const IRECT switchRow(panel.L + 6.f, panel.T + 262.f, panel.R - 6.f, panel.T + 316.f);
      const int switchParams[4] = {kParamEQActive[c], kParamNAMBypass[c], kParamIRBypass[c], kParamMute[c]};
      const char* const switchLabels[4] = {"EQ", "NAM Bypass", "IR Bypass", "Mute"};
      for (int k = 0; k < 4; k++)
        pGraphics->AttachControl(new NAMSwitchControl(switchRow.GetGridCell(0, k, 1, 4), switchParams[k],
                                                      switchLabels[k], style, switchHandleBitmap));
    }

    // ── Bottom: input / gate on the left, blend / master / meter on the right ──
    const IVStyle neutralStyle = MakeStyle(kNeutralColor);
    const float bottomT = panelT + panelH + 10.f;
    const float bottomB = b.B - 14.f;
    const IRECT inPanel(margin, bottomT, margin + panelW, bottomB);
    const IRECT outPanel(margin + panelW + panelGap, bottomT, b.R - margin, bottomB);
    pGraphics->AttachControl(new DualNAMPanelControl(inPanel, "NOISE GATE", kNeutralColor));
    pGraphics->AttachControl(new DualNAMPanelControl(outPanel, "OUTPUT", kNeutralColor));

    // Knobs in the bottom row get the same proportions as the channel knobs
    // (about 90 x 126), centred in their cell; a full cell would make them huge.
    auto KnobIn = [](const IRECT& cell) { return cell.GetCentredInside(92.f, 126.f); };
    const IRECT inRow = inPanel.GetReducedFromTop(32.f).GetPadded(-6.f);
    // Gate: on/off and Pre/Post NAM stacked in the first cell, then Threshold,
    // Attack and Release. The input routing is fixed since 1.2.0.
    const IRECT switchCell = inRow.GetGridCell(0, 0, 1, 4);
    pGraphics->AttachControl(new NAMSwitchControl(switchCell.GetFromTop(62.f).GetVShifted(4.f), kNoiseGateActive,
                                                  "Gate", neutralStyle, switchHandleBitmap));
    pGraphics->AttachControl(new NAMVerticalSwitchControl(switchCell.GetFromBottom(74.f), kGatePosition, "Pre NAM",
                                                          "Post NAM", "Position", neutralStyle, switchHandleBitmap),
                             -1, "GATE_CONTROLS");
    const int gateKnobs[3] = {kNoiseGateThreshold, kGateAttack, kGateRelease};
    const char* const gateLabels[3] = {"Threshold", "Attack", "Release"};
    for (int k = 0; k < 3; k++)
      pGraphics->AttachControl(new NAMKnobControl(KnobIn(inRow.GetGridCell(0, k + 1, 1, 4)), gateKnobs[k],
                                                  gateLabels[k], neutralStyle, knobBackgroundBitmap),
                               -1, "GATE_CONTROLS");

    const IRECT outRow = outPanel.GetReducedFromTop(32.f).GetPadded(-6.f);
    const IRECT meterArea = outRow.GetFromRight(176.f).GetMidVPadded(std::min(outRow.H(), 168.f) * 0.5f);
    const IRECT knobsArea = outRow.GetReducedFromRight(176.f);
    pGraphics->AttachControl(new NAMKnobControl(KnobIn(knobsArea.GetGridCell(0, 0, 1, 2)), kBlend, "Blend A | B",
                                                neutralStyle, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(KnobIn(knobsArea.GetGridCell(0, 1, 1, 2)), kMasterOutput, "Master",
                                                neutralStyle, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMLevelMeterControl(meterArea, "OUTPUT L / R"), kCtrlTagOutputMeter);

    pGraphics->ForAllControlsFunc([](IControl* pControl) {
      pControl->SetMouseEventsWhenDisabled(true);
      pControl->SetMouseOverWhenDisabled(true);
    });
  };
}

NeuralAmpModeler::~NeuralAmpModeler()
{
  // Handoff destructors free live, pending and retired objects. The audio
  // thread is gone by now.
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio thread
// ─────────────────────────────────────────────────────────────────────────────

void NeuralAmpModeler::ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames)
{
  const int nIn = NInChansConnected();
  const int nOut = NOutChansConnected();

  if (mMaxBlock <= 0 || nFrames <= 0)
  {
    for (int c = 0; c < nOut; c++)
      std::fill(outputs[c], outputs[c] + std::max(nFrames, 0), 0.0);
    return;
  }

  std::fenv_t fe_state;
  std::feholdexcept(&fe_state);
  disable_denormals();

  _ApplyStaging();

  // A host may hand us more frames than it announced. Split rather than
  // resize anything here.
  for (int offset = 0; offset < nFrames; offset += mMaxBlock)
    _ProcessSubBlock(inputs, outputs, offset, std::min(mMaxBlock, nFrames - offset), nIn, nOut);

  std::feupdateenv(&fe_state);

  if (nOut >= 1)
  {
    sample* meterPtrs[kNumMeterChannels] = {outputs[0], outputs[nOut >= 2 ? 1 : 0]};
    mOutputSender.ProcessBlock(meterPtrs, nFrames, kCtrlTagOutputMeter, kNumMeterChannels);
  }
}

void NeuralAmpModeler::_ApplyStaging()
{
  for (auto& ch : mChains)
  {
    ch.model.Apply();
    ch.ir.Apply();
  }
}

void NeuralAmpModeler::_ProcessSubBlock(iplug::sample** inputs, iplug::sample** outputs, const int offset,
                                        const int n, const int nIn, const int nOut)
{
  const double sampleRate = GetSampleRate();

  // ── Input routing (fixed since 1.2.0): the guitar is the mono input bus or,
  // on a stereo bus, the LEFT input. It goes to both chains at full level.
  // Everything is read from the host buffers before anything is written,
  // because hosts may pass the same memory for input and output.
  const sample* guitar;
  if (nIn >= 1)
    guitar = inputs[0] + offset;
  else
  {
    std::fill(mMonoIn.begin(), mMonoIn.begin() + n, 0.0);
    guitar = mMonoIn.data();
  }

  // ── Noise gate. One detector on the guitar signal (before the per-chain
  // input gains), one envelope. mGateDepth fades the whole gate in and out;
  // the position only ever changes while the depth is 0, where Pre and Post
  // are the same thing (gain 1 everywhere), so the switch itself cannot step.
  mGate.SetParams(GetParam(kNoiseGateThreshold)->Value(), GetParam(kGateAttack)->Value(),
                  GetParam(kGateRelease)->Value());
  mGate.Process(guitar, mGateEnv.data(), n);
  {
    const bool enabled = GetParam(kNoiseGateActive)->Bool();
    const int wantedPosition = GetParam(kGatePosition)->Int() == kGatePre ? kGatePre : kGatePost;
    const double depthStep = 1.0 / (kGateFadeS * sampleRate);
    bool anyPre = false, anyPost = false;
    for (int s = 0; s < n; s++)
    {
      if (mGateDepth <= 0.0 && mGatePositionActive != wantedPosition)
        mGatePositionActive = wantedPosition;
      const double target = (enabled && mGatePositionActive == wantedPosition) ? 1.0 : 0.0;
      mGateDepth = target > mGateDepth ? std::min(target, mGateDepth + depthStep)
                                       : std::max(target, mGateDepth - depthStep);
      const double g = 1.0 - mGateDepth * (1.0 - mGateEnv[s]);
      const bool pre = mGatePositionActive == kGatePre;
      mGatePre[s] = pre ? g : 1.0;
      mGatePost[s] = pre ? 1.0 : g;
      anyPre |= pre && g != 1.0;
      anyPost |= !pre && g != 1.0;
    }
    if (anyPre)
    {
      // Pre NAM: gate the mono signal before it is split.
      for (int s = 0; s < n; s++)
        mMonoIn[s] = guitar[s] * mGatePre[s];
      guitar = mMonoIn.data();
    }
    mGatePostActive = anyPost;
  }

  // A chain that cannot be heard (muted, or blended fully away) is not
  // processed at all. It still finishes its fade-out first.
  bool active[kNumChains];
  for (int c = 0; c < kNumChains; c++)
  {
    Chain& ch = mChains[c];
    active[c] = ch.outL > 0.0 || ch.outR > 0.0 || ch.outLCurrent > 0.0 || ch.outRCurrent > 0.0;
    if (ch.inputGainCurrent < 0.0 || !active[c])
      ch.inputGainCurrent = ch.inputGain;
    if (!active[c])
    {
      ch.outLCurrent = ch.outL;
      ch.outRCurrent = ch.outR;
      continue;
    }
    // Input gain is applied to the signal that drives the model.
    const double g0 = ch.inputGainCurrent;
    const double step = (ch.inputGain - g0) / (double)n;
    sample* in = ch.in.data();
    for (int s = 0; s < n; s++)
      in[s] = (g0 + step * (double)(s + 1)) * guitar[s];
    ch.inputGainCurrent = ch.inputGain;
  }

  // ── NAM, one independent model per chain.
  for (int c = 0; c < kNumChains; c++)
  {
    Chain& ch = mChains[c];
    sample* in = ch.in.data();
    sample* out = ch.namOut.data();
    if (!active[c])
    {
      std::fill(out, out + n, 0.0);
      continue;
    }
    ResamplingNAM* model = ch.model.Live();
    if (model != nullptr && !GetParam(kParamNAMBypass[c])->Bool())
    {
      try
      {
        model->process(&in, &out, n);
      }
      catch (...)
      {
        // An exception must never leave the audio callback.
        std::copy(in, in + n, out);
      }
    }
    else
    {
      std::copy(in, in + n, out);
    }
    // Post NAM: the same envelope on both NAM outputs, before EQ and IR.
    if (mGatePostActive)
      for (int s = 0; s < n; s++)
        out[s] *= mGatePost[s];
  }

  sample* chainPtr[kNumChains] = {mChains[kChainA].namOut.data(), mChains[kChainB].namOut.data()};

  const recursive_linear_filter::HighPassParams hpParams(sampleRate, kDCBlockerFrequency);
  const int reportedLatency = mReportedLatency.load(std::memory_order_acquire);

  std::fill(mMixL.begin(), mMixL.begin() + n, 0.0);
  std::fill(mMixR.begin(), mMixR.begin() + n, 0.0);

  for (int c = 0; c < kNumChains; c++)
  {
    if (!active[c])
      continue;
    Chain& ch = mChains[c];
    sample** p = &chainPtr[c];

    if (GetParam(kParamEQActive[c])->Bool())
      p = ch.toneStack->Process(p, 1, n);

    dsp::ImpulseResponse* ir = ch.ir.Live();
    if (ir != nullptr && !GetParam(kParamIRBypass[c])->Bool())
      p = ir->Process(p, 1, n);

    ch.highPass.SetParams(hpParams);
    p = ch.highPass.Process(p, 1, n);

    // Copy into our own buffer: the delay below works in place.
    sample* x = ch.namOut.data();
    if (p[0] != x)
      std::copy(p[0], p[0] + n, x);

    // Line the chains up: pad this one by however much less latency its model
    // has than the one reported to the host.
    ResamplingNAM* model = ch.model.Live();
    const int ownLatency =
      (model != nullptr && !GetParam(kParamNAMBypass[c])->Bool()) ? model->GetLatency() : 0;
    ch.align.Process(x, n, reportedLatency - ownLatency);

    // Output gain, blend, mute and pan, glided across the block.
    if (ch.outLCurrent < 0.0)
      ch.outLCurrent = ch.outL;
    if (ch.outRCurrent < 0.0)
      ch.outRCurrent = ch.outR;
    const double l0 = ch.outLCurrent, r0 = ch.outRCurrent;
    const double lStep = (ch.outL - l0) / (double)n;
    const double rStep = (ch.outR - r0) / (double)n;
    for (int s = 0; s < n; s++)
    {
      const double t = (double)(s + 1);
      mMixL[s] += (l0 + lStep * t) * x[s];
      mMixR[s] += (r0 + rStep * t) * x[s];
    }
    ch.outLCurrent = ch.outL;
    ch.outRCurrent = ch.outR;
  }

  // ── Master and write-out.
  if (mMasterGainCurrent < 0.0)
    mMasterGainCurrent = mMasterGain;
  const double m0 = mMasterGainCurrent;
  const double mStep = (mMasterGain - m0) / (double)n;
  for (int s = 0; s < n; s++)
  {
    const double g = m0 + mStep * (double)(s + 1);
    if (nOut >= 2)
    {
      outputs[0][offset + s] = g * mMixL[s];
      outputs[1][offset + s] = g * mMixR[s];
    }
    else if (nOut == 1)
    {
      outputs[0][offset + s] = g * 0.5 * (mMixL[s] + mMixR[s]);
    }
  }
  for (int c = 2; c < nOut; c++)
    std::fill(outputs[c] + offset, outputs[c] + offset + n, 0.0);
  mMasterGainCurrent = mMasterGain;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameters
// ─────────────────────────────────────────────────────────────────────────────

void NeuralAmpModeler::_UpdateGains()
{
  double blend[kNumChains];
  BlendGains(GetParam(kBlend)->Value(), blend[kChainA], blend[kChainB]);
  for (int c = 0; c < kNumChains; c++)
  {
    Chain& ch = mChains[c];
    ch.inputGain = DBToAmp(GetParam(kParamInput[c])->Value());
    double panL, panR;
    PanGains(GetParam(kParamPan[c])->Value(), panL, panR);
    const double level =
      DBToAmp(GetParam(kParamOutput[c])->Value()) * blend[c] * (GetParam(kParamMute[c])->Bool() ? 0.0 : 1.0);
    ch.outL = level * panL;
    ch.outR = level * panR;
  }
  mMasterGain = DBToAmp(GetParam(kMasterOutput)->Value());
}

void NeuralAmpModeler::_SetToneStackParam(const int chain, const int paramIdx)
{
  auto& ts = mChains[chain].toneStack;
  if (ts == nullptr)
    return;
  const double v = GetParam(paramIdx)->Value();
  if (paramIdx == kParamBass[chain])
    ts->SetParam("bass", v);
  else if (paramIdx == kParamMid[chain])
    ts->SetParam("middle", v);
  else if (paramIdx == kParamTreble[chain])
    ts->SetParam("treble", v);
}

void NeuralAmpModeler::OnParamChange(int paramIdx)
{
  switch (paramIdx)
  {
    case kInputA:
    case kInputB:
    case kOutputA:
    case kOutputB:
    case kPanA:
    case kPanB:
    case kMuteA:
    case kMuteB:
    case kBlend:
    case kMasterOutput: _UpdateGains(); break;
    case kBassA:
    case kMidA:
    case kTrebleA: _SetToneStackParam(kChainA, paramIdx); break;
    case kBassB:
    case kMidB:
    case kTrebleB: _SetToneStackParam(kChainB, paramIdx); break;
    default: break;
  }
}

void NeuralAmpModeler::OnParamChangeUI(int paramIdx, EParamSource source)
{
  auto* pGraphics = GetUI();
  if (pGraphics == nullptr)
    return;
  const bool on = GetParam(paramIdx)->Bool();
  switch (paramIdx)
  {
    case kNoiseGateActive:
      pGraphics->ForControlInGroup("GATE_CONTROLS", [on](IControl* pControl) { pControl->SetDisabled(!on); });
      break;
    case kEQActiveA:
      pGraphics->ForControlInGroup("EQ_KNOBS_A", [on](IControl* pControl) { pControl->SetDisabled(!on); });
      break;
    case kEQActiveB:
      pGraphics->ForControlInGroup("EQ_KNOBS_B", [on](IControl* pControl) { pControl->SetDisabled(!on); });
      break;
    default: break;
  }
}

bool NeuralAmpModeler::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
{
  switch (msgTag)
  {
    case kMsgTagClearModelA: ClearModel(kChainA); return true;
    case kMsgTagClearModelB: ClearModel(kChainB); return true;
    case kMsgTagClearIRA: ClearIR(kChainA); return true;
    case kMsgTagClearIRB: ClearIR(kChainB); return true;
    default: return false;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle (non-RT; the audio thread is stopped during OnReset)
// ─────────────────────────────────────────────────────────────────────────────

void NeuralAmpModeler::_PrepareBuffers(const int maxBlock)
{
  for (auto& ch : mChains)
  {
    ch.in.assign((size_t)maxBlock, 0.0);
    ch.namOut.assign((size_t)maxBlock, 0.0);
    ch.align.Prepare();
  }
  mMonoIn.assign((size_t)maxBlock, 0.0);
  mGateEnv.assign((size_t)maxBlock, 0.0);
  mGatePre.assign((size_t)maxBlock, 1.0);
  mGatePost.assign((size_t)maxBlock, 1.0);
  mMixL.assign((size_t)maxBlock, 0.0);
  mMixR.assign((size_t)maxBlock, 0.0);
}

void NeuralAmpModeler::_PrepareModel(ResamplingNAM& model, const double sampleRate, const int maxBlock) const
{
  model.Reset(sampleRate, maxBlock);
  // One silent block at the full size, so every lazily sized buffer inside the
  // model exists before the audio thread ever calls it...
  std::vector<NAM_SAMPLE> in((size_t)maxBlock, 0.0), out((size_t)maxBlock, 0.0);
  NAM_SAMPLE* pIn = in.data();
  NAM_SAMPLE* pOut = out.data();
  model.process(&pIn, &pOut, maxBlock);
  // ...then reset again. The priming block moved the resampler's phase by an
  // amount that depends on the block size; starting from a fresh resampler
  // keeps the output independent of the host's buffer size.
  model.Reset(sampleRate, maxBlock);
}

std::unique_ptr<dsp::ImpulseResponse> NeuralAmpModeler::_MakeIR(const dsp::ImpulseResponse::IRData& data) const
{
  auto ir = std::make_unique<dsp::ImpulseResponse>(data, GetSampleRate());
  // The IR sizes its history on first use. Do that here, at the largest block,
  // so the audio thread never resizes it.
  std::vector<sample> zeros((size_t)mMaxBlock, 0.0);
  sample* p = zeros.data();
  ir->Process(&p, 1, (size_t)mMaxBlock);
  return ir;
}

void NeuralAmpModeler::OnReset()
{
  std::lock_guard<std::mutex> lock(mLoadMutex);

  const double sampleRate = GetSampleRate();
  const int maxBlock = std::max(GetBlockSize(), 1);
  const bool rateChanged = sampleRate != mPreparedSampleRate;
  mMaxBlock = maxBlock;
  _PrepareBuffers(maxBlock);

  // Silent priming blocks for the persistent per-chain DSP, at the largest size.
  std::vector<sample> zeros((size_t)maxBlock, 0.0);
  std::vector<sample> zeros2((size_t)maxBlock, 0.0);
  sample* z[2] = {zeros.data(), zeros2.data()};

  for (int c = 0; c < kNumChains; c++)
  {
    Chain& ch = mChains[c];

    // Models: both the live one and one still waiting to go live.
    int newestLatency = 0;
    for (ResamplingNAM* m : {ch.model.Live(), ch.model.Pending()})
      if (m != nullptr)
        _PrepareModel(*m, sampleRate, maxBlock);
    if (ResamplingNAM* m = ch.model.Newest())
      newestLatency = m->GetLatency();
    ch.modelLatency.store(newestLatency, std::memory_order_release);

    // IRs are resampled when they are built, so a new rate needs a new object.
    if (ch.haveIRData && rateChanged)
      ch.ir.Stage(_MakeIR(ch.irData));
    for (dsp::ImpulseResponse* ir : {ch.ir.Live(), ch.ir.Pending()})
    {
      if (ir == nullptr)
        continue;
      std::fill(zeros.begin(), zeros.end(), 0.0);
      ir->Process(z, 1, (size_t)maxBlock);
    }

    ch.toneStack->Reset(sampleRate, maxBlock);
    for (int p : {kParamBass[c], kParamMid[c], kParamTreble[c]})
      _SetToneStackParam(c, p);
    std::fill(zeros.begin(), zeros.end(), 0.0);
    ch.toneStack->Process(z, 1, maxBlock);

    ch.highPass.SetParams(recursive_linear_filter::HighPassParams(sampleRate, kDCBlockerFrequency));
    std::fill(zeros.begin(), zeros.end(), 0.0);
    ch.highPass.Process(z, 1, (size_t)maxBlock);

    ch.inputGainCurrent = ch.outLCurrent = ch.outRCurrent = -1.0;
  }

  // Gate: start closed (as a gate does), at the wanted position and depth --
  // playback is starting from scratch, so there is nothing to fade from.
  mGate.SetSampleRate(sampleRate);
  mGate.Reset();
  mGatePositionActive = GetParam(kGatePosition)->Int() == kGatePre ? kGatePre : kGatePost;
  mGateDepth = GetParam(kNoiseGateActive)->Bool() ? 1.0 : 0.0;
  mGatePostActive = false;
  mMasterGainCurrent = -1.0;

  // The HPF DC blocker has a decay; 10 cycles is enough for the VST3 tail tests.
  SetTailSize(10 * (int)(sampleRate / kDCBlockerFrequency));
  mOutputSender.Reset(sampleRate);
  mPreparedSampleRate = sampleRate;
  _UpdateReportedLatency();
}

void NeuralAmpModeler::_UpdateReportedLatency()
{
  int latency = 0;
  for (const auto& ch : mChains)
    latency = std::max(latency, ch.modelLatency.load(std::memory_order_acquire));
  mReportedLatency.store(latency, std::memory_order_release);
  if (GetLatency() != latency)
    SetLatency(latency);
}

void NeuralAmpModeler::OnIdle()
{
  // Free whatever the audio thread has retired. try_lock: never stall the UI
  // behind a model that is still loading -- the next idle tick will do it.
  if (mLoadMutex.try_lock())
  {
    for (auto& ch : mChains)
    {
      ch.model.Drain();
      ch.ir.Drain();
    }
    mLoadMutex.unlock();
  }

  mOutputSender.TransmitData(*this);
}

void NeuralAmpModeler::OnUIOpen()
{
  Plugin::OnUIOpen();

  std::lock_guard<std::mutex> lock(mLoadMutex);
  for (int c = 0; c < kNumChains; c++)
  {
    const Chain& ch = mChains[c];
    if (ch.namPath.GetLength())
    {
      SendControlMsgFromDelegate(kCtrlTagModelFileBrowser[c], kMsgTagLoadedModel, ch.namPath.GetLength() + 1,
                                 ch.namPath.Get());
      if (ch.namFailed)
        SendControlMsgFromDelegate(kCtrlTagModelFileBrowser[c], kMsgTagFileMissing);
    }
    if (ch.irPath.GetLength())
    {
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser[c], kMsgTagLoadedIR, ch.irPath.GetLength() + 1,
                                 ch.irPath.Get());
      if (ch.irFailed)
        SendControlMsgFromDelegate(kCtrlTagIRFileBrowser[c], kMsgTagFileMissing);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Loading (non-RT)
// ─────────────────────────────────────────────────────────────────────────────

std::string NeuralAmpModeler::LoadModel(const int chain, const WDL_String& path)
{
  std::lock_guard<std::mutex> lock(mLoadMutex);
  return _LoadModel(chain, path, false);
}

std::string NeuralAmpModeler::LoadIR(const int chain, const WDL_String& path)
{
  std::lock_guard<std::mutex> lock(mLoadMutex);
  return _LoadIR(chain, path, false);
}

void NeuralAmpModeler::ClearModel(const int chain)
{
  std::lock_guard<std::mutex> lock(mLoadMutex);
  Chain& ch = mChains[chain];
  ch.model.Stage(nullptr);
  ch.namPath.Set("");
  ch.namFailed = false;
  ch.modelLatency.store(0, std::memory_order_release);
  _UpdateReportedLatency();
}

void NeuralAmpModeler::ClearIR(const int chain)
{
  std::lock_guard<std::mutex> lock(mLoadMutex);
  Chain& ch = mChains[chain];
  ch.ir.Stage(nullptr);
  ch.irPath.Set("");
  ch.irFailed = false;
  ch.haveIRData = false;
  ch.irData = dsp::ImpulseResponse::IRData();
}

std::string NeuralAmpModeler::_LoadModel(const int chain, const WDL_String& path, const bool fromState)
{
  Chain& ch = mChains[chain];
  const int ctrlTag = kCtrlTagModelFileBrowser[chain];
  const double sampleRate = GetSampleRate();
  const int maxBlock = mMaxBlock > 0 ? mMaxBlock : std::max(GetBlockSize(), 1);
  std::string error;
  try
  {
    const auto fsPath = std::filesystem::u8path(path.Get());
    if (!std::filesystem::exists(fsPath))
      throw std::runtime_error("File not found.");

    std::unique_ptr<nam::DSP> dsp = nam::get_dsp(fsPath);
    if (dsp->NumInputChannels() != 1 || dsp->NumOutputChannels() != 1)
      throw std::runtime_error("Only mono (1 in / 1 out) models are supported; this one has "
                               + std::to_string(dsp->NumInputChannels()) + " in / "
                               + std::to_string(dsp->NumOutputChannels()) + " out.");

    // A2 files carry several sizes (A2 Lite and A2 Full) in one container. Pick
    // the full-size model explicitly: the container's selection is driven by a
    // "slim" value, and 0.0 -- the value an unset control would give -- selects
    // the SMALLEST submodel.
    if (auto* slimmable = dynamic_cast<nam::SlimmableModel*>(dsp.get()))
      slimmable->SetSlimmableSize(1.0);

    auto model = std::make_unique<ResamplingNAM>(std::move(dsp), sampleRate);
    _PrepareModel(*model, sampleRate, maxBlock);
    const int latency = model->GetLatency();

    ch.model.Stage(std::move(model));
    ch.modelLatency.store(latency, std::memory_order_release);
    ch.namPath = path;
    ch.namFailed = false;
    _UpdateReportedLatency();
    SendControlMsgFromDelegate(ctrlTag, kMsgTagLoadedModel, ch.namPath.GetLength() + 1, ch.namPath.Get());
    return "";
  }
  catch (const std::exception& e)
  {
    error = e.what();
  }
  catch (...)
  {
    error = "Unknown error while loading the model.";
  }
  if (error.empty())
    error = "Unknown error while loading the model.";

  std::cerr << "DualNAM: failed to load model " << path.Get() << ": " << error << std::endl;
  if (fromState)
  {
    // Restoring a project: the slot is empty, but the path is remembered so the
    // project still names the file, and the browser says it is missing.
    ch.model.Stage(nullptr);
    ch.modelLatency.store(0, std::memory_order_release);
    ch.namPath = path;
    ch.namFailed = true;
    _UpdateReportedLatency();
    SendControlMsgFromDelegate(ctrlTag, kMsgTagLoadedModel, ch.namPath.GetLength() + 1, ch.namPath.Get());
    SendControlMsgFromDelegate(ctrlTag, kMsgTagFileMissing);
  }
  else if (ch.namPath.GetLength() && !ch.namFailed)
  {
    // Interactive load failed: keep playing the previous model and show its name.
    SendControlMsgFromDelegate(ctrlTag, kMsgTagLoadedModel, ch.namPath.GetLength() + 1, ch.namPath.Get());
  }
  else
  {
    SendControlMsgFromDelegate(ctrlTag, kMsgTagLoadFailed);
  }
  return error;
}

std::string NeuralAmpModeler::_LoadIR(const int chain, const WDL_String& path, const bool fromState)
{
  Chain& ch = mChains[chain];
  const int ctrlTag = kCtrlTagIRFileBrowser[chain];
  std::string error;
  try
  {
    const auto fsPath = std::filesystem::u8path(path.Get());
    if (!std::filesystem::exists(fsPath))
      throw std::runtime_error("File not found.");

    dsp::ImpulseResponse probe(path.Get(), GetSampleRate());
    const dsp::wav::LoadReturnCode state = probe.GetWavState();
    if (state != dsp::wav::LoadReturnCode::SUCCESS)
      throw std::runtime_error(dsp::wav::GetMsgForLoadReturnCode(state));

    dsp::ImpulseResponse::IRData data = probe.GetData();
    if (mMaxBlock > 0)
      ch.ir.Stage(_MakeIR(data));
    else
      ch.ir.Stage(std::make_unique<dsp::ImpulseResponse>(data, GetSampleRate()));
    ch.irData = std::move(data);
    ch.haveIRData = true;
    ch.irPath = path;
    ch.irFailed = false;
    SendControlMsgFromDelegate(ctrlTag, kMsgTagLoadedIR, ch.irPath.GetLength() + 1, ch.irPath.Get());
    return "";
  }
  catch (const std::exception& e)
  {
    error = e.what();
  }
  catch (...)
  {
    error = "Unknown error while loading the IR.";
  }
  if (error.empty())
    error = "Unknown error while loading the IR.";

  std::cerr << "DualNAM: failed to load IR " << path.Get() << ": " << error << std::endl;
  if (fromState)
  {
    ch.ir.Stage(nullptr);
    ch.haveIRData = false;
    ch.irPath = path;
    ch.irFailed = true;
    SendControlMsgFromDelegate(ctrlTag, kMsgTagLoadedIR, ch.irPath.GetLength() + 1, ch.irPath.Get());
    SendControlMsgFromDelegate(ctrlTag, kMsgTagFileMissing);
  }
  else if (ch.irPath.GetLength() && !ch.irFailed)
  {
    SendControlMsgFromDelegate(ctrlTag, kMsgTagLoadedIR, ch.irPath.GetLength() + 1, ch.irPath.Get());
  }
  else
  {
    SendControlMsgFromDelegate(ctrlTag, kMsgTagLoadFailed);
  }
  return error;
}

// ─────────────────────────────────────────────────────────────────────────────
// State
//
//   str   header "###DualNAMA2Full###"
//   int32 format version
//   str   NAM path A, NAM path B, IR path A, IR path B
//   int32 parameter count
//   n x  (str name, double value)
//
// Parameters are stored by name: a newer build that adds parameters reads an
// older state (missing names keep their defaults), and an older build reading a
// newer state skips names it does not know.
// ─────────────────────────────────────────────────────────────────────────────

// Gate threshold of a state written before 1.2.0.
//
// 1.0.0/1.1.0 fed the gate detector  max |g_c * src_c|  over the audible chains
// (after the input gains). With a mono routing (src_A == src_B == guitar) that
// is  G * |guitar|,  G = max(g_A, g_B), so the old detector level is the new
// one plus 20*log10(G) dB, and  T_new = T_old - G_dB  keeps the level at which
// the gate opens and closes exactly where it was (tools/coretest/thresholdproof:
// identical decisions). The old gate's whole gain curve cannot be kept -- its
// depth and slew depended on the threshold itself -- and the new gate replaces
// it anyway.
//
// 1.1.0 "Stereo Independent" / 1.0.0 "Stereo" detected on max(g_A|L|, g_B|R|);
// since 1.2.0 the detector hears only L, which used to feed chain A, so only
// chain A's gain is taken there (an approximation, documented).
//
// Audible = not muted and not blended fully away (output gain and pan can
// never silence a chain), matching the 1.1.0 "active" test.
void NeuralAmpModeler::_MigrateGateThreshold(const std::vector<std::pair<std::string, double>>& saved,
                                             std::vector<std::pair<int, double>>& values) const
{
  auto find = [&](const char* name, double fallback, bool* found = nullptr) {
    for (const auto& s : saved)
      if (s.first == name)
      {
        if (found)
          *found = true;
        return s.second;
      }
    return fallback;
  };
  bool haveThreshold = false;
  const double oldThreshold = find("Gate Threshold", 0.0, &haveThreshold);
  if (!haveThreshold)
    return; // nothing to migrate; the parameter keeps its (new) default

  double blendA, blendB;
  BlendGains(find("Blend", 0.0), blendA, blendB);
  const bool audible[kNumChains] = {find("Mute A", 0.0) < 0.5 && blendA > 0.0, find("Mute B", 0.0) < 0.5 && blendB > 0.0};
  const double inputDB[kNumChains] = {find("Input A", 0.0), find("Input B", 0.0)};

  bool haveSource = false;
  const double source = find("Input Source", 0.0, &haveSource); // 1.1.0
  const bool stereoIndependent =
    haveSource ? std::lround(source) == kLegacyInputStereo : find("Input Mode", 0.0) >= 0.5; // 1.0.0

  double offsetDB = -1e9;
  for (int c = 0; c < kNumChains; c++)
    if (audible[c] && (!stereoIndependent || c == kChainA))
      offsetDB = std::max(offsetDB, inputDB[c]);
  if (offsetDB <= -1e9)
    return; // no audible chain (or only B in stereo mode): no defined relation

  const IParam* p = GetParam(kNoiseGateThreshold);
  const double newThreshold = std::clamp(oldThreshold - offsetDB, p->GetMin(), p->GetMax());
  for (auto& v : values)
    if (v.first == kNoiseGateThreshold)
      v.second = newThreshold;
}

bool NeuralAmpModeler::SerializeState(IByteChunk& chunk) const
{
  std::lock_guard<std::mutex> lock(mLoadMutex);
  chunk.PutStr(kStateHeader);
  chunk.Put(&kStateFormatVersion);
  for (int c = 0; c < kNumChains; c++)
    chunk.PutStr(mChains[c].namPath.Get());
  for (int c = 0; c < kNumChains; c++)
    chunk.PutStr(mChains[c].irPath.Get());
  const int32_t nParams = kNumParams;
  chunk.Put(&nParams);
  for (int i = 0; i < kNumParams; i++)
  {
    const IParam* p = GetParam(i);
    chunk.PutStr(p->GetName());
    const double v = p->Value();
    chunk.Put(&v);
  }
  return true;
}

int NeuralAmpModeler::UnserializeState(const IByteChunk& chunk, int startPos)
{
  try
  {
    int pos = startPos;
    WDL_String header;
    pos = chunk.GetStr(header, pos);
    if (pos < 0 || strcmp(header.Get(), kStateHeader) != 0)
      throw std::runtime_error("not a Dual NAM A2 Full state");

    int32_t version = 0;
    pos = chunk.Get(&version, pos);
    if (pos < 0 || version < 1)
      throw std::runtime_error("bad state version");

    WDL_String namPath[kNumChains], irPath[kNumChains];
    for (int c = 0; c < kNumChains; c++)
      if ((pos = chunk.GetStr(namPath[c], pos)) < 0)
        throw std::runtime_error("truncated state (model paths)");
    for (int c = 0; c < kNumChains; c++)
      if ((pos = chunk.GetStr(irPath[c], pos)) < 0)
        throw std::runtime_error("truncated state (IR paths)");

    int32_t nParams = 0;
    pos = chunk.Get(&nParams, pos);
    if (pos < 0 || nParams < 0 || nParams > 10000)
      throw std::runtime_error("bad parameter count");

    std::vector<std::pair<int, double>> values;
    // Every (finite) value by its saved name, for the migration below.
    std::vector<std::pair<std::string, double>> saved;
    for (int32_t i = 0; i < nParams; i++)
    {
      WDL_String name;
      double v = 0.0;
      if ((pos = chunk.GetStr(name, pos)) < 0 || (pos = chunk.Get(&v, pos)) < 0)
        throw std::runtime_error("truncated state (parameters)");
      if (!std::isfinite(v))
        continue;
      saved.emplace_back(name.Get(), v);
      for (int p = 0; p < kNumParams; p++)
      {
        if (strcmp(GetParam(p)->GetName(), name.Get()) == 0)
        {
          values.emplace_back(p, v);
          break;
        }
      }
    }
    // States written before 1.2.0 (format version 1): the old gate detector sat
    // AFTER the per-chain input gains, the new one sits before them. Version 2
    // states are already in the new meaning and are never converted again.
    if (version < 2)
      _MigrateGateThreshold(saved, values);

    // Everything parsed: apply. Parameters not in the state go to defaults, so
    // loading a project is reproducible no matter what the instance held before.
    ENTER_PARAMS_MUTEX
    for (int p = 0; p < kNumParams; p++)
      GetParam(p)->SetToDefault();
    for (const auto& pv : values)
      GetParam(pv.first)->Set(pv.second);
    LEAVE_PARAMS_MUTEX
    OnParamReset(kPresetRecall);

    {
      std::lock_guard<std::mutex> lock(mLoadMutex);
      for (int c = 0; c < kNumChains; c++)
      {
        Chain& ch = mChains[c];
        if (namPath[c].GetLength())
          _LoadModel(c, namPath[c], true);
        else
        {
          ch.model.Stage(nullptr);
          ch.namPath.Set("");
          ch.namFailed = false;
          ch.modelLatency.store(0, std::memory_order_release);
        }
        if (irPath[c].GetLength())
          _LoadIR(c, irPath[c], true);
        else
        {
          ch.ir.Stage(nullptr);
          ch.irPath.Set("");
          ch.irFailed = false;
          ch.haveIRData = false;
        }
      }
      _UpdateReportedLatency();
    }
    return pos;
  }
  catch (const std::exception& e)
  {
    std::cerr << "DualNAM: failed to restore state: " << e.what() << std::endl;
  }
  catch (...)
  {
    std::cerr << "DualNAM: failed to restore state: unknown error" << std::endl;
  }
  // Report the whole chunk as consumed, so the wrapper's following read fails
  // cleanly instead of interpreting our bytes as its own.
  return chunk.Size();
}
