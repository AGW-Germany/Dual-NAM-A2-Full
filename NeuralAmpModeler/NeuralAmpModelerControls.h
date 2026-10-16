#pragma once

#include <chrono> // VU ballistics timing
#include <cmath> // std::round
#include <cstdio> // FILE, fclose
#include <sstream> // std::stringstream
#include <unordered_map> // std::unordered_map
#include "IControls.h"
#include "IPlugPaths.h"

#ifdef OS_WIN
  #include <Windows.h>
  #include <Shellapi.h>
#endif

#define PLUG() static_cast<PLUG_CLASS_NAME*>(GetDelegate())
#define NAM_KNOB_HEIGHT 120.0f
#define NAM_SWTICH_HEIGHT 50.0f

using namespace iplug;
using namespace igraphics;

enum class NAMBrowserState
{
  Empty, // when no file loaded, show "Get" button
  Loaded // when file loaded, show "Clear" button
};

// Where the corner button on the plugin (settings, close settings) goes
// :param rect: Rect for the whole plugin's UI
IRECT CornerButtonArea(const IRECT& rect)
{
  const auto mainArea = rect.GetPadded(-20);
  return mainArea.GetFromTRHC(50, 50).GetCentredInside(20, 20);
};

/// Draws a bitmap stretched to fill its rect. IGraphics::AttachBackground()
/// blits at native size from (0,0), which left the window unpainted once the
/// UI grew wider than the artwork.
class NAMFittedBitmapControl : public IControl
                             , public IBitmapBase
{
public:
  NAMFittedBitmapControl(const IRECT& bounds, const IBitmap& bitmap)
  : IControl(bounds)
  , IBitmapBase(bitmap)
  {
    SetIgnoreMouse(true);
  }

  void OnRescale() override { mBitmap = GetUI()->GetScaledBitmap(mBitmap); }
  void Draw(IGraphics& g) override { g.DrawFittedBitmap(mBitmap, mRECT); }
};

/// Framed section with a title in the channel colour. Purely decorative.
class DualNAMPanelControl : public IControl
{
public:
  DualNAMPanelControl(const IRECT& bounds, const char* title, const IColor& color)
  : IControl(bounds)
  , mTitle(title)
  , mColor(color)
  {
    SetIgnoreMouse(true);
  }

  void Draw(IGraphics& g) override
  {
    const float r = 8.f;
    g.FillRoundRect(COLOR_BLACK.WithOpacity(0.38f), mRECT, r);
    g.DrawRoundRect(mColor.WithOpacity(0.65f), mRECT.GetPadded(-0.75f), r, nullptr, 1.5f);
    const IRECT titleRect = mRECT.GetFromTop(30.f).GetReducedFromLeft(14.f);
    g.DrawText(IText(13.f, mColor, "Michroma-Regular", EAlign::Near, EVAlign::Middle), mTitle.Get(), titleRect);
    g.DrawLine(mColor.WithOpacity(0.25f), mRECT.L + 10.f, mRECT.T + 30.f, mRECT.R - 10.f, mRECT.T + 30.f);
  }

private:
  WDL_String mTitle;
  IColor mColor;
};

class NAMSquareButtonControl : public ISVGButtonControl
{
public:
  NAMSquareButtonControl(const IRECT& bounds, IActionFunction af, const ISVG& svg)
  : ISVGButtonControl(bounds, af, svg, svg)
  {
  }

  void Draw(IGraphics& g) override
  {
    if (mMouseIsOver)
      g.FillRoundRect(PluginColors::MOUSEOVER, mRECT, 2.f);

    ISVGButtonControl::Draw(g);
  }
};

class NAMCircleButtonControl : public ISVGButtonControl
{
public:
  NAMCircleButtonControl(const IRECT& bounds, IActionFunction af, const ISVG& svg)
  : ISVGButtonControl(bounds, af, svg, svg)
  {
  }

  void Draw(IGraphics& g) override
  {
    if (mMouseIsOver)
      g.FillEllipse(PluginColors::MOUSEOVER, mRECT);

    ISVGButtonControl::Draw(g);
  }
};

/// Full-window dim layer; click dismisses (used for Slim overlay).
class NAMSlimOverlayBackdropControl : public IControl
{
public:
  NAMSlimOverlayBackdropControl(const IRECT& bounds, IActionFunction dismiss)
  : IControl(bounds, dismiss)
  , mDismiss(dismiss)
  {
  }

  void Draw(IGraphics& g) override { g.FillRect(COLOR_BLACK.WithOpacity(0.45f), mRECT); }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (mDismiss)
      mDismiss(this);
  }

private:
  IActionFunction mDismiss;
};

class NAMKnobControl : public IVKnobControl, public IBitmapBase
{
public:
  NAMKnobControl(const IRECT& bounds, int paramIdx, const char* label, const IVStyle& style, IBitmap bitmap)
  : IVKnobControl(bounds, paramIdx, label, style, true)
  , IBitmapBase(bitmap)
  {
    mInnerPointerFrac = 0.55;
  }

  void OnRescale() override { mBitmap = GetUI()->GetScaledBitmap(mBitmap); }

  void DrawWidget(IGraphics& g) override
  {
    auto knobRect = mWidgetBounds.GetCentredInside(mWidgetBounds.W(), mWidgetBounds.W());
    // Ring radius from the knob face, not from IVectorBase::GetRadius(): that
    // takes the widget band's *shortest* side, so in a band wider than it is
    // tall -- the bottom strip -- the ring would be drawn inside the face and
    // disappear. Identical for the main knob row, where the band is the taller
    // of the two and the shortest side is already the width.
    const float widgetRadius = 0.5f * knobRect.W() * 0.73f;
    const float cx = knobRect.MW(), cy = knobRect.MH();
    const float angle = mAngle1 + (static_cast<float>(GetValue()) * (mAngle2 - mAngle1));
    DrawIndicatorTrack(g, angle, cx + 0.5, cy, widgetRadius);
    g.DrawFittedBitmap(mBitmap, knobRect);
    float data[2][2];
    RadialPoints(angle, cx, cy, mInnerPointerFrac * widgetRadius, mInnerPointerFrac * widgetRadius, 2, data);
    g.PathCircle(data[1][0], data[1][1], 3);
    g.PathFill(IPattern::CreateRadialGradient(data[1][0], data[1][1], 4.0f,
                                              {{GetColor(mMouseIsOver ? kX3 : kX1), 0.f},
                                               {GetColor(mMouseIsOver ? kX3 : kX1), 0.8f},
                                               {COLOR_TRANSPARENT, 1.0f}}),
               {}, &mBlend);
    g.DrawCircle(COLOR_BLACK.WithOpacity(0.5f), data[1][0], data[1][1], 3, &mBlend);
  }
};

class NAMSwitchControl : public IVSlideSwitchControl, public IBitmapBase
{
  // Track width, in px, shared by every switch in the plugin.
  static constexpr float kTrackWidth = 34.0f;

  static IVStyle SwitchStyle(const IVStyle& style)
  {
    return style.WithRoundness(0.666f)
      .WithShowValue(false)
      .WithEmboss(true)
      .WithShadowOffset(1.5f)
      .WithDrawShadows(false)
      .WithColor(kFR, COLOR_BLACK)
      .WithFrameThickness(0.5f)
      .WithWidgetFrac(0.5f)
      .WithLabelOrientation(EOrientation::South);
  }

public:
  NAMSwitchControl(const IRECT& bounds, int paramIdx, const char* label, const IVStyle& style, IBitmap bitmap)
  : IVSlideSwitchControl(bounds, paramIdx, label, SwitchStyle(style))
  , IBitmapBase(bitmap)
  {
  }

  // Same switch, driven by an action function instead of a parameter. The meter
  // style is a display choice, so it must not turn up in the host's automation
  // lanes -- but it should look and feel identical to Link / Noise Gate / EQ.
  NAMSwitchControl(const IRECT& bounds, IActionFunction af, const char* label, const IVStyle& style, IBitmap bitmap,
                   bool initialOn)
  : IVSlideSwitchControl(bounds, af, label, SwitchStyle(style), false, EDirection::Horizontal, 2, initialOn ? 1 : 0)
  , IBitmapBase(bitmap)
  {
  }

  // The base class sizes the track from the control's width, so a switch with a
  // long label ends up with a visibly fatter track than one with a short label.
  // Pin it instead: the switches read as one set of identical controls, and the
  // rect is then free to be however wide the label needs.
  void OnResize() override
  {
    IVSlideSwitchControl::OnResize();
    mWidgetBounds = mWidgetBounds.GetMidHPadded(0.5f * kTrackWidth);
    UpdateRects();
  }

  void DrawWidget(IGraphics& g) override
  {
    DrawTrack(g, mWidgetBounds);
    DrawHandle(g, mHandleBounds);
  }

  void DrawTrack(IGraphics& g, const IRECT& bounds) override
  {
    IRECT handleBounds = GetAdjustedHandleBounds(bounds);
    handleBounds = IRECT(handleBounds.L, handleBounds.T, handleBounds.R, handleBounds.T + mBitmap.H());
    IRECT centreBounds = handleBounds.GetPadded(-mStyle.shadowOffset);
    IRECT shadowBounds = handleBounds.GetTranslated(mStyle.shadowOffset, mStyle.shadowOffset);
    //    const float contrast = mDisabled ? -GRAYED_ALPHA : 0.f;
    float cR = 7.f;
    const float tlr = cR;
    const float trr = cR;
    const float blr = cR;
    const float brr = cR;

    // outer shadow
    if (mStyle.drawShadows)
      g.FillRoundRect(GetColor(kSH), shadowBounds, tlr, trr, blr, brr, &mBlend);

    // Embossed style unpressed
    if (mStyle.emboss)
    {
      // Positive light
      g.FillRoundRect(GetColor(kPR), handleBounds, tlr, trr, blr, brr /*, &blend*/);

      // Negative light
      g.FillRoundRect(GetColor(kSH), shadowBounds, tlr, trr, blr, brr /*, &blend*/);

      // Fill in foreground
      g.FillRoundRect(GetValue() > 0.5 ? GetColor(kX1) : COLOR_BLACK, centreBounds, tlr, trr, blr, brr, &mBlend);

      // Shade when hovered
      if (mMouseIsOver)
        g.FillRoundRect(GetColor(kHL), centreBounds, tlr, trr, blr, brr, &mBlend);
    }
    else
    {
      g.FillRoundRect(GetValue() > 0.5 ? GetColor(kX1) : COLOR_BLACK, handleBounds, tlr, trr, blr, brr /*, &blend*/);

      // Shade when hovered
      if (mMouseIsOver)
        g.FillRoundRect(GetColor(kHL), handleBounds, tlr, trr, blr, brr, &mBlend);
    }

    if (mStyle.drawFrame)
      g.DrawRoundRect(GetColor(kFR), handleBounds, tlr, trr, blr, brr, &mBlend, mStyle.frameThickness);
  }

  void DrawHandle(IGraphics& g, const IRECT& filledArea) override
  {
    IRECT r;
    if (GetSelectedIdx() == 0)
    {
      r = filledArea.GetFromLeft(mBitmap.W());
    }
    else
    {
      r = filledArea.GetFromRight(mBitmap.W());
    }

    g.DrawBitmap(mBitmap, r, 0, 0, nullptr);
  }
};

/// The same slide switch turned by 90 degrees, for a choice between two equal
/// options (Gate Position: Pre NAM on top, Post NAM below). The track stays lit
/// in both positions -- a dark track means "off" on every other switch. The
/// handle bitmap is not rotated, so its light falls from the top left like on
/// the other switches. The option labels sit beside the two handle positions,
/// the active one white, the other grey; the caption sits below.
class NAMVerticalSwitchControl : public NAMSwitchControl
{
  static constexpr float kTrackLength = 34.0f; // = NAMSwitchControl's track width
  static constexpr float kTrackOffsetX = -32.6f; // track's left edge from the control's centre
  static constexpr float kTrackOffsetY = 4.0f;
  static constexpr float kLabelGap = 6.0f;
  static constexpr float kCaptionOffsetY = 18.0f; // caption centre below the track

public:
  NAMVerticalSwitchControl(const IRECT& bounds, int paramIdx, const char* topLabel, const char* bottomLabel,
                           const char* caption, const IVStyle& style, IBitmap bitmap)
  : NAMSwitchControl(bounds, paramIdx, "", style.WithShowLabel(false), bitmap)
  , mTopLabel(topLabel)
  , mBottomLabel(bottomLabel)
  , mCaption(caption)
  {
    mDirection = EDirection::Vertical;
  }

  void OnResize() override
  {
    IVSlideSwitchControl::OnResize();
    const float l = mRECT.MW() + kTrackOffsetX;
    const float t = mRECT.T + kTrackOffsetY;
    mWidgetBounds = IRECT(l, t, l + static_cast<float>(mBitmap.W()), t + kTrackLength);
    UpdateRects();
  }

  void Draw(IGraphics& g) override
  {
    DrawWidget(g);
    const IColor active = mStyle.labelText.mFGColor;
    const IColor inactive = active.WithOpacity(0.45f);
    const bool bottom = GetSelectedIdx() != 0;
    const float x = mWidgetBounds.R + kLabelGap;
    const float hh = 0.5f * static_cast<float>(mBitmap.H());
    DrawOption(g, mTopLabel, bottom ? inactive : active, x, mWidgetBounds.T + hh);
    DrawOption(g, mBottomLabel, bottom ? active : inactive, x, mWidgetBounds.B - hh);
    const IText captionText = mStyle.labelText.WithFGColor(active).WithAlign(EAlign::Center).WithVAlign(EVAlign::Middle);
    const float cy = mWidgetBounds.B + kCaptionOffsetY;
    g.DrawText(captionText, mCaption.Get(), IRECT(mRECT.L, cy - 10.f, mRECT.R, cy + 10.f), &mBlend);
  }

  void DrawTrack(IGraphics& g, const IRECT& bounds) override
  {
    const IRECT track = bounds;
    const IRECT centreBounds = track.GetPadded(-mStyle.shadowOffset);
    const IRECT shadowBounds = track.GetTranslated(mStyle.shadowOffset, mStyle.shadowOffset);
    const float cR = 7.f;
    if (mStyle.drawShadows)
      g.FillRoundRect(GetColor(kSH), shadowBounds, cR, &mBlend);
    if (mStyle.emboss)
    {
      g.FillRoundRect(GetColor(kPR), track, cR, &mBlend);
      g.FillRoundRect(GetColor(kSH), shadowBounds, cR, &mBlend);
      g.FillRoundRect(GetColor(kX1), centreBounds, cR, &mBlend);
      if (mMouseIsOver)
        g.FillRoundRect(GetColor(kHL), centreBounds, cR, &mBlend);
    }
    else
    {
      g.FillRoundRect(GetColor(kX1), track, cR, &mBlend);
      if (mMouseIsOver)
        g.FillRoundRect(GetColor(kHL), track, cR, &mBlend);
    }
    if (mStyle.drawFrame)
      g.DrawRoundRect(GetColor(kFR), track, cR, &mBlend, mStyle.frameThickness);
  }

  void DrawHandle(IGraphics& g, const IRECT& filledArea) override
  {
    const float h = static_cast<float>(mBitmap.H());
    const IRECT r = GetSelectedIdx() == 0 ? filledArea.GetFromTop(h) : filledArea.GetFromBottom(h);
    g.DrawBitmap(mBitmap, r, 0, 0, &mBlend);
  }

private:
  void DrawOption(IGraphics& g, const WDL_String& text, const IColor& color, float x, float cy)
  {
    const IText t = mStyle.labelText.WithFGColor(color).WithAlign(EAlign::Near).WithVAlign(EVAlign::Middle);
    g.DrawText(t, text.Get(), IRECT(x, cy - 10.f, mRECT.R, cy + 10.f), &mBlend);
  }

  WDL_String mTopLabel, mBottomLabel, mCaption;
};

/// A NAMSwitchControl that pulses while it is engaged.
///
/// Delta is a listen mode, not a setting: left on by accident it silently
/// changes what you are hearing, and unlike Mix it has no knob position to give
/// itself away. So while it is on it keeps announcing itself. The pulse is a
/// raised cosine that never reaches black -- it should read as "on, and
/// pulsing", not as flickering between on and off.
class NAMPulsingSwitchControl : public NAMSwitchControl
{
public:
  NAMPulsingSwitchControl(const IRECT& bounds, int paramIdx, const char* label, const IVStyle& style, IBitmap bitmap)
  : NAMSwitchControl(bounds, paramIdx, label, style, bitmap)
  , mOnColor(mStyle.colorSpec.GetColor(kX1)) // base is built first, so this is the style actually in use
  {
  }

  // Repaint every frame while engaged so the pulse animates. A switch that is
  // off costs nothing extra -- it falls back to the normal dirty flag.
  bool IsDirty() override { return (GetValue() > 0.5) ? true : NAMSwitchControl::IsDirty(); }

  void Draw(IGraphics& g) override
  {
    if (GetValue() <= 0.5)
    {
      NAMSwitchControl::Draw(g);
      return;
    }

    const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mStart).count();
    const float phase = 0.5f + 0.5f * (float)std::cos(kTwoPi * ms / kPeriodMs);
    const float brightness = kMinBrightness + (1.0f - kMinBrightness) * phase;

    // The base class reads the "on" fill straight out of the style, and neither
    // GetColor nor DrawTrack is overridable in a way that lets us tint just
    // that one fill -- so swap the colour in for the duration of the draw.
    mStyle.colorSpec.mColors[kX1] = IColor::LinearInterpolateBetween(COLOR_BLACK, mOnColor, brightness);
    NAMSwitchControl::Draw(g);
    mStyle.colorSpec.mColors[kX1] = mOnColor;
  }

private:
  static constexpr double kTwoPi = 6.283185307179586;
  static constexpr double kPeriodMs = 900.0;
  static constexpr float kMinBrightness = 0.28f;

  const IColor mOnColor;
  const std::chrono::steady_clock::time_point mStart = std::chrono::steady_clock::now();
};

class NAMFileNameControl : public IVButtonControl
{
public:
  NAMFileNameControl(const IRECT& bounds, const char* label, const IVStyle& style)
  : IVButtonControl(bounds, DefaultClickActionFunc, label, style)
  {
  }

  void SetLabelAndTooltip(const char* str)
  {
    SetLabelStr(str);
    SetTooltip(str);
  }

  void SetLabelAndTooltipEllipsizing(const WDL_String& fileName)
  {
    auto EllipsizeFilePath = [](const char* filePath, size_t prefixLength, size_t suffixLength, size_t maxLength) {
      const std::string ellipses = "...";
      assert(maxLength <= (prefixLength + suffixLength + ellipses.size()));
      std::string str{filePath};

      if (str.length() <= maxLength)
      {
        return str;
      }
      else
      {
        return str.substr(0, prefixLength) + ellipses + str.substr(str.length() - suffixLength);
      }
    };

    auto ellipsizedFileName = EllipsizeFilePath(fileName.get_filepart(), 22, 22, 45);
    SetLabelStr(ellipsizedFileName.c_str());
    SetTooltip(fileName.get_filepart());
  }
};

// URL control for the "Get" models/irs links
class NAMGetButtonControl : public NAMSquareButtonControl
{
public:
  NAMGetButtonControl(const IRECT& bounds, const char* label, const char* url, const ISVG& globeSVG)
  : NAMSquareButtonControl(
      bounds,
      [url](IControl* pCaller) {
        WDL_String fullURL(url);
        auto* ui = pCaller->GetUI();
        ui->ReleaseMouseCapture();
        ui->OpenURL(fullURL.Get());
      },
      globeSVG)
  {
    SetTooltip(label);
  }
};

class NAMFileBrowserControl : public IDirBrowseControlBase
{
public:
  NAMFileBrowserControl(const IRECT& bounds, int clearMsgTag, const char* labelStr, const char* fileExtension,
                        IFileDialogCompletionHandlerFunc ch, const IVStyle& style, const ISVG& loadSVG,
                        const ISVG& clearSVG, const ISVG& leftSVG, const ISVG& rightSVG, const IBitmap& bitmap,
                        const ISVG& globeSVG, const char* getButtonLabel, const char* getButtonURL)
  : IDirBrowseControlBase(bounds, fileExtension, false, false)
  , mClearMsgTag(clearMsgTag)
  , mDefaultLabelStr(labelStr)
  , mCompletionHandlerFunc(ch)
  , mStyle(style.WithColor(kFG, COLOR_TRANSPARENT).WithDrawFrame(false))
  , mBitmap(bitmap)
  , mLoadSVG(loadSVG)
  , mClearSVG(clearSVG)
  , mLeftSVG(leftSVG)
  , mRightSVG(rightSVG)
  , mGlobeSVG(globeSVG)
  , mGetButtonLabel(getButtonLabel)
  , mGetButtonURL(getButtonURL)
  , mBrowserState(NAMBrowserState::Empty)
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override { g.DrawFittedBitmap(mBitmap, mRECT); }

  void OnPopupMenuSelection(IPopupMenu* pSelectedMenu, int valIdx) override
  {
    if (pSelectedMenu)
    {
      IPopupMenu::Item* pItem = pSelectedMenu->GetChosenItem();

      if (pItem)
      {
        mSelectedItemIndex = mItems.Find(pItem);
        LoadFileAtCurrentIndex();
      }
    }
  }

  void OnAttached() override
  {
    auto prevFileFunc = [&](IControl* pCaller) {
      const auto nItems = NItems();
      if (nItems == 0)
        return;
      mSelectedItemIndex--;

      if (mSelectedItemIndex < 0)
        mSelectedItemIndex = nItems - 1;

      LoadFileAtCurrentIndex();
    };

    auto nextFileFunc = [&](IControl* pCaller) {
      const auto nItems = NItems();
      if (nItems == 0)
        return;
      mSelectedItemIndex++;

      if (mSelectedItemIndex >= nItems)
        mSelectedItemIndex = 0;

      LoadFileAtCurrentIndex();
    };

    auto loadFileFunc = [&](IControl* pCaller) {
      WDL_String fileName;
      WDL_String path;
      GetSelectedFileDirectory(path);
#ifdef NAM_PICK_DIRECTORY
      pCaller->GetUI()->PromptForDirectory(path, [&](const WDL_String& fileName, const WDL_String& path) {
        if (path.GetLength())
        {
          ClearPathList();
          AddPath(path.Get(), "");
          SetupMenu();
          SelectFirstFile();
          LoadFileAtCurrentIndex();
        }
      });
#else
      pCaller->GetUI()->PromptForFile(
        fileName, path, EFileAction::Open, mExtension.Get(), [&](const WDL_String& fileName, const WDL_String& path) {
          if (fileName.GetLength())
          {
            ClearPathList();
            AddPath(path.Get(), "");
            SetupMenu();
            SetSelectedFile(fileName.Get());
            LoadFileAtCurrentIndex();
          }
        });
#endif
    };

    auto clearFileFunc = [&](IControl* pCaller) {
      pCaller->GetDelegate()->SendArbitraryMsgFromUI(mClearMsgTag);
      mFileNameControl->SetLabelAndTooltip(mDefaultLabelStr.Get());
      SetBrowserState(NAMBrowserState::Empty);
      // FIXME disabling output mode...
      //      pCaller->GetUI()->GetControlWithTag(kCtrlTagOutputMode)->SetDisabled(false);
    };

    auto chooseFileFunc = [&, loadFileFunc](IControl* pCaller) {
      if (std::string_view(pCaller->As<IVButtonControl>()->GetLabelStr()) == mDefaultLabelStr.Get())
      {
        loadFileFunc(pCaller);
      }
      else
      {
        CheckSelectedItem();

        if (!mMainMenu.HasSubMenus())
        {
          mMainMenu.SetChosenItemIdx(mSelectedItemIndex);
        }
        pCaller->GetUI()->CreatePopupMenu(*this, mMainMenu, pCaller->GetRECT());
      }
    };

    IRECT padded = mRECT.GetPadded(-6.f).GetHPadded(-2.f);
    const auto buttonWidth = padded.H();
    const auto loadFileButtonBounds = padded.ReduceFromLeft(buttonWidth);
    const auto clearAndGetButtonBounds = padded.ReduceFromRight(buttonWidth);
    const auto leftButtonBounds = padded.ReduceFromLeft(buttonWidth);
    const auto rightButtonBounds = padded.ReduceFromLeft(buttonWidth);
    const auto fileNameButtonBounds = padded;

    AddChildControl(new NAMSquareButtonControl(loadFileButtonBounds, DefaultClickActionFunc, mLoadSVG))
      ->SetAnimationEndActionFunction(loadFileFunc);
    AddChildControl(new NAMSquareButtonControl(leftButtonBounds, DefaultClickActionFunc, mLeftSVG))
      ->SetAnimationEndActionFunction(prevFileFunc);
    AddChildControl(new NAMSquareButtonControl(rightButtonBounds, DefaultClickActionFunc, mRightSVG))
      ->SetAnimationEndActionFunction(nextFileFunc);
    AddChildControl(mFileNameControl = new NAMFileNameControl(fileNameButtonBounds, mDefaultLabelStr.Get(), mStyle))
      ->SetAnimationEndActionFunction(chooseFileFunc);

    // creates both right-side controls but only show one based on state
    mClearButton = new NAMSquareButtonControl(clearAndGetButtonBounds, DefaultClickActionFunc, mClearSVG);
    mClearButton->SetAnimationEndActionFunction(clearFileFunc);
    AddChildControl(mClearButton);

    mGetButton = new NAMGetButtonControl(clearAndGetButtonBounds, mGetButtonLabel, mGetButtonURL, mGlobeSVG);
    AddChildControl(mGetButton);

    // initialize control visibility
    SetBrowserState(NAMBrowserState::Empty);
  }

  void LoadFileAtCurrentIndex()
  {
    if (mSelectedItemIndex > -1 && mSelectedItemIndex < NItems())
    {
      WDL_String fileName, path;
      GetSelectedFile(fileName);
      mFileNameControl->SetLabelAndTooltipEllipsizing(fileName);
      mCompletionHandlerFunc(fileName, path);
    }
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    switch (msgTag)
    {
      case kMsgTagLoadFailed:
        // Honestly, not sure why I made a big stink of it before. Why not just say it failed and move on? :)
        {
          std::string label(std::string("(FAILED) ") + std::string(mFileNameControl->GetLabelStr()));
          mFileNameControl->SetLabelAndTooltip(label.c_str());
          SetBrowserState(NAMBrowserState::Empty);
        }
        break;
      case kMsgTagFileMissing:
        // A restored project names a file that could not be loaded. Keep the
        // name visible (and the Clear button, so the slot can be emptied).
        {
          std::string label(std::string("(MISSING) ") + std::string(mFileNameControl->GetLabelStr()));
          mFileNameControl->SetLabelStr(label.c_str());
          mFileNameControl->SetTooltip(mMissingTooltip.Get());
          SetBrowserState(NAMBrowserState::Loaded);
        }
        break;
      case kMsgTagLoadedModel:
      case kMsgTagLoadedIR:
      {
        WDL_String fileName, directory;
        fileName.Set(reinterpret_cast<const char*>(pData));
        directory.Set(reinterpret_cast<const char*>(pData));
        directory.remove_filepart(true);

        mMissingTooltip.SetFormatted((int)fileName.GetLength() + 64, "File not found or not loadable:\n%s",
                                     fileName.Get());
        ClearPathList();
        AddPath(directory.Get(), "");
        SetupMenu();
        SetSelectedFile(fileName.Get());
        mFileNameControl->SetLabelAndTooltipEllipsizing(fileName);
        SetBrowserState(NAMBrowserState::Loaded);
      }
      break;
      default: break;
    }
  }

private:
  void SelectFirstFile() { mSelectedItemIndex = mFiles.GetSize() ? 0 : -1; }

  void GetSelectedFileDirectory(WDL_String& path)
  {
    GetSelectedFile(path);
    path.remove_filepart();
    return;
  }

  // set the state of the browser and the visibility of the "Get" vs. "Clear" buttons
  void SetBrowserState(NAMBrowserState newState)
  {
    mBrowserState = newState;

    switch (mBrowserState)
    {
      case NAMBrowserState::Empty:
        mClearButton->Hide(true);
        mGetButton->Hide(false);
        break;
      case NAMBrowserState::Loaded:
        mClearButton->Hide(false);
        mGetButton->Hide(true);
        break;
    }
  }

  WDL_String mDefaultLabelStr;
  WDL_String mMissingTooltip;
  IFileDialogCompletionHandlerFunc mCompletionHandlerFunc;
  NAMFileNameControl* mFileNameControl = nullptr;
  IVStyle mStyle;
  IBitmap mBitmap;
  ISVG mLoadSVG, mClearSVG, mLeftSVG, mRightSVG, mGlobeSVG;
  int mClearMsgTag;

  // new members for the "Get" button
  const char* mGetButtonLabel;
  const char* mGetButtonURL;
  NAMBrowserState mBrowserState;
  NAMSquareButtonControl* mClearButton = nullptr;
  NAMGetButtonControl* mGetButton = nullptr;
};

class NAMMeterControl : public IVPeakAvgMeterControl<kNumMeterChannels>, public IBitmapBase
{
  static constexpr float KMeterMin = -70.0f;
  static constexpr float KMeterMax = -0.01f;

public:
  NAMMeterControl(const IRECT& bounds, const IBitmap& bitmap, const IVStyle& style)
  : IVPeakAvgMeterControl<kNumMeterChannels>(bounds, "",
                                             style.WithShowValue(false).WithDrawFrame(false).WithWidgetFrac(0.8),
                                             EDirection::Vertical, {}, 0, KMeterMin, KMeterMax, {})
  , IBitmapBase(bitmap)
  {
    SetPeakSize(1.0f);
  }

  void OnRescale() override { mBitmap = GetUI()->GetScaledBitmap(mBitmap); }

  virtual void OnResize() override
  {
    SetTargetRECT(MakeRects(mRECT));
    // GetMidHPadded(n) yields a rect 2n wide, so this is the real width of the
    // drawn meter regardless of how wide the housing is. It was 10px for a
    // single bar; with two bars (L and R) that left ~5px each, so widen it.
    mWidgetBounds = mWidgetBounds.GetMidHPadded(11).GetVPadded(10);
    MakeTrackRects(mWidgetBounds);
    MakeStepRects(mWidgetBounds, mNSteps);
    SetDirty(false);
  }

  void DrawBackground(IGraphics& g, const IRECT& r) override { g.DrawFittedBitmap(mBitmap, r); }

  void DrawTrackHandle(IGraphics& g, const IRECT& r, int chIdx, bool aboveBaseValue) override
  {
    if (r.H() > 2)
      g.FillRect(GetColor(kX1), r, &mBlend);
  }

  void DrawPeak(IGraphics& g, const IRECT& r, int chIdx, bool aboveBaseValue) override
  {
    g.DrawGrid(COLOR_BLACK, mTrackBounds.Get()[chIdx], 10, 2);
    g.FillRect(GetColor(kX3), r, &mBlend);
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Backlit analogue VU meter with twin needles (L/R) and a dB readout pair.
//
// Standards-accurate rather than decorative:
//
//   * Deflection is proportional to VOLTAGE, not to dB. That is the defining
//     property of a VU movement and the reason the printed percentage scale
//     (0-100%) is evenly spaced while the dB scale bunches up at the top.
//     Full-scale (the right stop) is +3 VU, so frac = 10^((VU-3)/20) and 0 VU
//     lands at 1/10^(3/20) = 70.8% of the arc -- exactly where it sits on a
//     real faceplate.
//   * 0 VU is referenced to -18 dBFS RMS (EBU R68).
//   * The ballistics are a critically damped second order integrated in the
//     LINEAR domain, reaching 99% of deflection in 300 ms.
//
// The ballistics live here in the UI, driven by the RMS value the existing
// 2-channel sender already provides. No DSP is involved, so the audio path is
// untouched.
// ─────────────────────────────────────────────────────────────────────────────

inline IColor VUHex(unsigned int rgb, int a = 255)
{
  return IColor(a, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// NanoVG hands whatever coordinate it is given straight to the rasteriser --
// IGraphicsNanoVG::DoDrawText passes r.MW()/r.MH() with no rounding -- so a text
// box with a fractional centre puts the glyphs on a half pixel and they come out
// soft. Meter scales are computed from trig and log curves, so their centres are
// fractional almost every time. These build a box whose centre is a whole pixel.
// (Font sizes want to be whole numbers for the same reason.)
inline IRECT TextBoxC(float cx, float cy, float hw, float hh)
{
  const float x = std::round(cx), y = std::round(cy);
  return IRECT(x - hw, y - hh, x + hw, y + hh);
}

struct VUFaceTheme
{
  unsigned int hot, face, mid, dark, ink; // lamp hotspot -> face -> shadowed edge, and the printed ink
  unsigned int needleL, needleR; // the two pointers
  unsigned int readout; // colour of the dB readout text
};

// The lamp colours. Switch the whole meter by changing kVUTheme below --
// everything else derives from it.
static const VUFaceTheme kVUThemeGrey{0xFFFFFF, 0xE3E3E3, 0xBFBFBF, 0x8E8E8E, 0x1A1A1A, 0x232323, 0xB3261E, 0xE9E9E9};
static const VUFaceTheme kVUThemeGreen{0xE8FFF0, 0x7FD79A, 0x4AA96F, 0x2C7049, 0x06210F, 0x24301F, 0xB3261E, 0x56E03E};
static const VUFaceTheme kVUThemeCream{0xFFFDF5, 0xE4DAC2, 0xB6AB90, 0x847A63, 0x1C1810, 0x2A2118, 0xB3261E, 0xF0E7D4};
static const VUFaceTheme kVUThemeAmber{0xFFF4D6, 0xEFB85E, 0xC98C33, 0x8A5A17, 0x2A1A04, 0x2A2118, 0xB3261E, 0xEFB85E};
static const VUFaceTheme kVUThemeIce{0xF2FDFF, 0xA2D3E6, 0x5D9CB4, 0x3A6C80, 0x05191F, 0x1B2430, 0xB3261E, 0xA2D3E6};

// >>> Change this one line to re-skin both meters (kVUThemeCream, ...Amber, ...Ice). <<<
static const VUFaceTheme& kVUTheme = kVUThemeGrey;

// How long the two meter styles take to cross-fade into one another.
static constexpr double kMeterFadeMs = 170.0;

// ─────────────────────────────────────────────────────────────────────────────
// Backlit analogue VU meter with twin needles (L/R) and a dB readout pair.
//
// Standards-accurate rather than decorative:
//
//   * Deflection is proportional to VOLTAGE, not to dB. That is the defining
//     property of a VU movement and the reason the printed percentage scale
//     (0-100%) is evenly spaced while the dB scale bunches up at the top.
//     Full-scale (the right stop) is +3 VU, so frac = 10^((VU-3)/20) and 0 VU
//     lands at 1/10^(3/20) = 70.8% of the arc -- exactly where it sits on a
//     real faceplate.
//   * 0 VU is referenced to -18 dBFS RMS (EBU R68).
//   * The ballistics are a critically damped second order integrated in the
//     LINEAR domain, reaching 99% of deflection in 300 ms.
//
// The ballistics live here in the UI, driven by the RMS value the existing
// 2-channel sender already provides. No DSP is involved, so the audio path is
// untouched.
// ─────────────────────────────────────────────────────────────────────────────
class NAMVUMeterControl : public IControl
{
  static constexpr int kNChans = 2;
  // +/- from vertical. Shallow, like a real VU, but as wide as the face allows:
  // the corrected (linear-in-voltage) scale bunches the marks between -10 and 0,
  // so every degree of sweep buys readability for the numerals.
  static constexpr float kSweepDeg = 42.0f;

  // ── Calibration ───────────────────────────────────────────────────────────
  // A VU meter is RMS-responding and referenced to a fixed level. The digital
  // convention (EBU R68) puts 0 VU at -18 dBFS RMS; SMPTE uses -20. Change this
  // one constant to re-reference the whole meter.
  static constexpr double kZeroVUdBFS = -18.0;

  // The right-hand stop. A VU face runs to +3 VU (= 141.3% of reference volts).
  static constexpr double kFullScaleVU = 3.0;

  // ── Ballistics ────────────────────────────────────────────────────────────
  // Spec: the needle reaches 99% of DEFLECTION in 300 ms with the character of
  // a critically damped second-order (mechanical) movement. Two cascaded
  // one-poles give exactly that; for a critically damped pair the step response
  // hits 99% at t = 6.638*tau, so tau = 300/6.638 ms. Because deflection is
  // proportional to voltage, the filter must run on the LINEAR value -- running
  // it on dB gives a quite different (and much lazier) rise.
  static constexpr double kBallisticTauMs = 45.2;

  // If nothing arrives for this long the source has gone digitally silent. The
  // sender stops pushing below its threshold, so without this the needle would
  // simply freeze wherever it was when the audio stopped.
  static constexpr double kStaleMs = 120.0;

  static constexpr double kFloorVU = -40.0; // below this the readout shows -inf

public:
  NAMVUMeterControl(const IRECT& bounds, const char* label)
  : IControl(bounds)
  , mLabel(label)
  {
    for (int c = 0; c < kNChans; c++)
    {
      mTarget[c] = mS1[c] = mS2[c] = mMaxLin[c] = 0.0;
      mClip[c] = false;
    }
    mLast = mLastMsg = std::chrono::steady_clock::now();
  }

  // Reference volts (1.0 == 0 VU) -> position along the arc. Linear in voltage,
  // which is what makes the printed percentage scale evenly spaced.
  static double LinToFrac(double lin) { return Clip(lin / DBToAmp(kFullScaleVU), 0.0, 1.0); }
  static double VUToFrac(double vu) { return LinToFrac(DBToAmp(vu)); }
  static double LinToVU(double lin) { return lin > 1.0e-7 ? 20.0 * std::log10(lin) : -144.0; }

  static float FracToDeg(double f) { return (float)(-kSweepDeg + f * 2.0 * kSweepDeg); }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    if (IsDisabled() || msgTag != ISender<>::kUpdateMessage)
      return;

    IByteStream stream(pData, dataSize);
    int pos = 0;
    ISenderData<kNChans, std::pair<float, float>> d;
    pos = stream.Get(&d, pos);

    for (auto c = d.chanOffset; c < (d.chanOffset + d.nChans) && c < kNChans; c++)
    {
      mTarget[c] = static_cast<double>(std::get<1>(d.vals[c])) / DBToAmp(kZeroVUdBFS);
      // A VU is RMS-responding and cannot show an over, so clipping is taken
      // from the peak the sender already carries -- same source and same latch
      // behaviour as the ladder's over LEDs.
      if (static_cast<double>(std::get<0>(d.vals[c])) >= DBToAmp(-0.1))
        mClip[c] = true;
    }

    mLastMsg = std::chrono::steady_clock::now();
    SetDirty(false);
  }

  // The movement is advanced here rather than on message arrival: IsDirty() is
  // called exactly once per frame by IGraphics, whereas Draw() can run several
  // times in a frame (once per dirty region) and messages stop entirely on
  // silence. Wall-clock dt keeps the 300 ms spec true at any frame rate.
  bool IsDirty() override
  {
    const auto now = std::chrono::steady_clock::now();
    const double dtMs = Clip(std::chrono::duration<double, std::milli>(now - mLast).count(), 0.0, 200.0);
    mLast = now;
    // Cross-fade with the other meter style. Both are drawn while it runs, so
    // the swap reads as a dissolve rather than a jump cut.
    bool fading = false;
    if (mFade != mFadeTarget)
    {
      const float step = (float)(dtMs / kMeterFadeMs);
      mFade = mFadeTarget > mFade ? std::min(mFadeTarget, mFade + step) : std::max(mFadeTarget, mFade - step);
      mBlend.mWeight = mFade;
      fading = true;
      if (mFade <= 0.0f)
        Hide(true); // sets mDirty, which is what repaints the area underneath
    }

    // The other meter style: keep the clock running but skip the work. mDirty
    // must still be reported -- Hide() sets it, and that is what gets the area
    // underneath repainted; swallowing it leaves the hidden meter's pixels on
    // screen around the edges of the one that replaced it.
    if (IsHidden())
      return mDirty;

    if (std::chrono::duration<double, std::milli>(now - mLastMsg).count() > kStaleMs)
      for (int c = 0; c < kNChans; c++)
        mTarget[c] = 0.0;

    const double a = 1.0 - std::exp(-dtMs / kBallisticTauMs);
    bool moving = false;
    for (int c = 0; c < kNChans; c++)
    {
      mS1[c] += (mTarget[c] - mS1[c]) * a; // two cascaded one-poles ==
      mS2[c] += (mS1[c] - mS2[c]) * a; //   critically damped second order
      if (mS2[c] > mMaxLin[c])
        mMaxLin[c] = mS2[c];
      const double rest = DBToAmp(kFloorVU);
      if (mS1[c] > rest || mS2[c] > rest || mTarget[c] > rest)
        moving = true;
    }
    return mDirty || moving || fading; // parked at the left stop == no repaint
  }

  // Cross-fade this style in or out. Called by the meter-style switch.
  void SetShown(bool shown)
  {
    mFadeTarget = shown ? 1.0f : 0.0f;
    if (shown)
      Hide(false);
    SetDirty(false);
  }

  // Same, without the dissolve -- for laying out the editor, where fading in
  // from nothing on open would just look like a glitch.
  void SetShownImmediate(bool shown)
  {
    mFade = mFadeTarget = shown ? 1.0f : 0.0f;
    mBlend.mWeight = mFade;
    Hide(!shown);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    for (int c = 0; c < kNChans; c++) // click anywhere to reset the hold and the overs
    {
      mMaxLin[c] = mS2[c];
      mClip[c] = false;
    }
    SetDirty(false);
  }

  void Draw(IGraphics& g) override
  {
    const VUFaceTheme& T = kVUTheme;

    const float readoutH = 30.f;
    const float captionH = 15.f;
    const IRECT bezel = mRECT.GetFromTop(mRECT.H() - readoutH - captionH);
    const IRECT face = bezel.GetPadded(-4.f);
    const IRECT readR = mRECT.GetFromBottom(readoutH);
    const IRECT captionR = IRECT(mRECT.L, bezel.B, mRECT.R, readR.T);

    const float fw = face.W(), fh = face.H();
    const float px = face.MW();
    const float py = face.B + fh * 0.34f; // pivot sits below the glass: shallow arc
    const float arcR = py - (face.T + fh * 0.16f);
    const float needleLen = arcR - fh * 0.04f;
    const float labelRad = arcR - fh * 0.215f; // clear of the tick ends
    const float pctRad = arcR - fh * 0.345f; // between the dB numerals and "VU"

    // ── bezel + lamp-lit face ─────────────────────────────────────────────
    g.FillRoundRect(VUHex(0x0D1011), bezel, 5.f, &mBlend);
    const float faceR = 3.f;
    g.PathClipRegion(face);
    g.FillRoundRect(VUHex(T.dark), face, faceR, &mBlend);
    // Even ivory card, a soft vignette towards the corners, then a gentle lamp
    // wash from the three emitters along the bottom edge. Keeping the base flat
    // is what makes it read as printed card rather than a glowing gradient.
    g.FillRoundRect(VUHex(T.face), face, faceR, &mBlend);
    g.PathRoundRect(face, faceR);
    g.PathFill(IPattern::CreateRadialGradient(px, face.B, fw * 0.78f,
                                              {IColorStop(VUHex(T.face, 0), 0.f), IColorStop(VUHex(T.face, 0), 0.55f),
                                               IColorStop(VUHex(T.mid, 110), 1.f)}),
               IFillOptions(), &mBlend);
    for (float fx : {0.21f, 0.50f, 0.79f})
    {
      g.PathRoundRect(face, faceR);
      g.PathFill(IPattern::CreateRadialGradient(face.L + fw * fx, face.B + 3.f, fh * 0.42f,
                                                {IColorStop(VUHex(T.hot, 70), 0.f), IColorStop(VUHex(T.hot, 0), 1.f)}),
                 IFillOptions(), &mBlend);
    }

    // ── ticks ─────────────────────────────────────────────────────────────
    const double redFrac = VUToFrac(0.0); // the red zone begins at 0 VU (70.8%)
    auto tick = [&](double frac, float rOuter, float len, float w, const IColor& col) {
      const float rad = DegToRad(FracToDeg(frac) - 90.f);
      const float c = std::cos(rad), sn = std::sin(rad);
      g.DrawLine(col, px + c * rOuter, py + sn * rOuter, px + c * (rOuter - len), py + sn * (rOuter - len), &mBlend, w);
    };
    g.DrawArc(VUHex(T.ink), px, py, arcR, -kSweepDeg, kSweepDeg, &mBlend, 1.2f);
    // the red band above 0 VU, drawn over the black arc as on a real faceplate
    g.DrawArc(VUHex(0xA8180F), px, py, arcR, FracToDeg(redFrac), kSweepDeg, &mBlend, 2.4f);

    // dB ticks. Real VU faces carry marks only at the scale points -- no
    // arbitrary subdivision -- so these are exactly the printed values.
    for (double vu : {-20.0, -10.0, -7.0, -5.0, -3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0})
    {
      const bool hot = vu > 0.0;
      const bool lbl = (vu != -2.0);
      tick(VUToFrac(vu), arcR, fh * (lbl ? 0.085f : 0.055f), lbl ? 1.5f : 1.1f,
           hot ? VUHex(0xA8180F) : VUHex(T.ink, lbl ? 255 : 150));
    }
    // ── dB numerals ───────────────────────────────────────────────────────
    // Single glyphs throughout: "+3" was a two-glyph string centred on the same
    // point as the one-glyph labels, so its digit sat visibly left of its tick.
    // The red band and red ink already say "above zero".
    struct L
    {
      double vu;
      const char* t;
    };
    for (const L& l : {L{-20, "20"}, L{-10, "10"}, L{-7, "7"}, L{-5, "5"}, L{-3, "3"}, L{-1, "1"}, L{0, "0"}, L{1, "1"},
                       L{2, "2"}, L{3, "3"}})
    {
      const float rad = DegToRad(FracToDeg(VUToFrac(l.vu)) - 90.f);
      const float lx = px + std::cos(rad) * labelRad;
      const float ly = py + std::sin(rad) * labelRad;
      const IColor col = l.vu > 0 ? VUHex(0xA8180F) : VUHex(T.ink);
      // The two-digit marks are set a little narrower: -10 and -7 are the
      // tightest pair on a correctly scaled face and would otherwise read "107".
      IText txt(l.t[1] ? 12.f : 13.f, col, "Roboto-Regular", EAlign::Center, EVAlign::Middle);
      g.DrawText(txt, l.t, TextBoxC(lx, ly, 12.f, 8.f), &mBlend);
    }
    // ── percentage scale (100% == 0 VU, linear in voltage) ────────────────
    // 20% is omitted: it sits almost on top of the -20 dB numeral on this face.
    for (int pc : {40, 60, 80, 100})
    {
      const float rad = DegToRad(FracToDeg(LinToFrac(pc / 100.0)) - 90.f);
      const float lx = px + std::cos(rad) * pctRad;
      const float ly = py + std::sin(rad) * pctRad;
      WDL_String t;
      t.SetFormatted(8, "%d", pc);
      IText txt(10.f, VUHex(T.ink, 165), "Roboto-Regular", EAlign::Center, EVAlign::Middle);
      g.DrawText(txt, t.Get(), TextBoxC(lx, ly, 11.f, 7.f), &mBlend);
    }

    // "VU" legend
    {
      IText txt(13.f, VUHex(T.ink), "Roboto-Regular", EAlign::Center, EVAlign::Middle);
      const float vy = face.B - fh * 0.30f;
      g.DrawText(txt, "VU", TextBoxC(px, vy, 20.f, 9.f), &mBlend);
    }

    // ── needles ───────────────────────────────────────────────────────────
    for (int c = 0; c < kNChans; c++)
    {
      const float rad = DegToRad(FracToDeg(LinToFrac(mS2[c])) - 90.f);
      const float tipX = px + std::cos(rad) * needleLen;
      const float tipY = py + std::sin(rad) * needleLen;
      // The needle rotates about the FIXED pivot below the glass -- drawing the
      // base out along the rotating radius made the whole needle swing sideways
      // and read as a stray diagonal at full left deflection. The clip to the
      // face hides everything below the bottom edge, as on real hardware.
      const float bx = px;
      const float by = py;
      const IColor col = VUHex(c == 0 ? T.needleL : T.needleR);
      const float perp = rad + (float)PI * 0.5f;
      const float hw = 2.2f;
      g.PathClear();
      g.PathMoveTo(bx + std::cos(perp) * hw, by + std::sin(perp) * hw);
      g.PathLineTo(bx - std::cos(perp) * hw, by - std::sin(perp) * hw);
      g.PathLineTo(tipX, tipY);
      g.PathClose();
      g.PathFill(IPattern(col), IFillOptions(), &mBlend);
    }

    // ── shroud ────────────────────────────────────────────────────────────
    // A disc centred on the pivot, so both needles emerge from one point the
    // way they do on real hardware instead of crossing the bottom edge as two
    // separate diagonals.
    {
      const float hubR = (py - face.B) + fh * 0.16f;
      g.FillCircle(VUHex(0x2B2B2B), px, py, hubR, &mBlend);
      g.DrawArc(VUHex(0x6E6E6E, 120), px, py, hubR, -46.f, 46.f, &mBlend, 1.f);
    }

    // ── glass ─────────────────────────────────────────────────────────────
    g.PathRoundRect(face, faceR);
    g.PathFill(IPattern::CreateLinearGradient(face.L, face.T, face.R, face.B,
                                              {IColorStop(VUHex(0xFFFFFF, 30), 0.f), IColorStop(VUHex(0xFFFFFF, 8), 0.22f),
                                               IColorStop(VUHex(0xFFFFFF, 0), 0.45f)}),
               IFillOptions(), &mBlend);
    g.DrawRoundRect(VUHex(T.ink, 120), face.GetPadded(-1.f), faceR, &mBlend, 1.f);

    // Clip lamps, top right: L then R, click the meter to clear them.
    {
      const float lw = 9.f, lh = 5.f, gap = 4.f;
      const float ly = face.T + 7.f;
      const float lx = face.R - 9.f - (lw * 2.f + gap);
      for (int c = 0; c < kNChans; c++)
      {
        const IRECT lamp = IRECT(lx + (lw + gap) * c, ly, lx + (lw + gap) * c + lw, ly + lh);
        if (mClip[c])
        {
          g.FillRoundRect(VUHex(0xFF3B26, 70), lamp.GetPadded(2.f), 2.f, &mBlend);
          g.FillRoundRect(VUHex(0xFF3B26), lamp, 1.f, &mBlend);
        }
        else
        {
          // A dark lens rather than a pale one: on a light faceplate an unlit
          // lamp should read as off, not as another printed mark.
          g.FillRoundRect(VUHex(0x6E4A46, 170), lamp, 1.f, &mBlend);
          g.DrawRoundRect(VUHex(T.ink, 110), lamp, 1.f, &mBlend, 1.f);
        }
      }
    }
    g.PathClipRegion(IRECT());

    // ── CURRENT / MAX readouts ────────────────────────────────────────────
    // Both readouts are in VU (dB relative to 0 VU), the same units the needle
    // reads, so the number always agrees with where the pointer is sitting.
    const double curVU = LinToVU(std::max(mS2[0], mS2[1]));
    const double maxVU = LinToVU(std::max(mMaxLin[0], mMaxLin[1]));
    auto readout = [&](const IRECT& box, const char* cap, double vu) {
      g.FillRoundRect(VUHex(0x080B0B), box, 3.f, &mBlend);
      g.DrawRoundRect(VUHex(0xFFFFFF, 20), box, 3.f, &mBlend, 1.f);
      IText capT(9.f, VUHex(0x99A5A0), "Roboto-Regular", EAlign::Near, EVAlign::Middle);
      g.DrawText(capT, cap, box.GetPadded(-4.f, 0.f, 0.f, 0.f), &mBlend);
      WDL_String v;
      if (vu <= kFloorVU)
        v.Set("-inf");
      else
        v.SetFormatted(16, "%+.1f", vu);
      const IColor led = vu > 0.0 ? VUHex(0xFF6152) : VUHex(T.readout);
      IText vT(16.f, led, "Roboto-Regular", EAlign::Far, EVAlign::Middle);
      g.DrawText(vT, v.Get(), box.GetPadded(0.f, 0.f, -4.f, 0.f), &mBlend);
    };
    const float gap = 5.f;
    const float bw = (readR.W() - gap) * 0.5f;
    readout(IRECT(readR.L, readR.T, readR.L + bw, readR.B), "CURRENT", curVU);
    readout(IRECT(readR.R - bw, readR.T, readR.R, readR.B), "MAX", maxVU);

    // caption
    {
      IText txt(13.f, VUHex(0x9AA6A1), "Roboto-Regular", EAlign::Center, EVAlign::Middle);
      g.DrawText(txt, mLabel.Get(), TextBoxC(captionR.MW(), captionR.MH(), captionR.W() * 0.5f, 9.f), &mBlend);
    }
  }

private:
  WDL_String mLabel;
  double mTarget[kNChans]; // reference volts, 1.0 == 0 VU
  double mS1[kNChans], mS2[kNChans]; // cascaded ballistic stages
  double mMaxLin[kNChans];
  bool mClip[kNChans] = {false, false};
  std::chrono::steady_clock::time_point mLast, mLastMsg;
  float mFade = 1.0f, mFadeTarget = 1.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// Difference readout, in the same lamp-on-black style as the meters' own dB
// boxes so it reads as part of the same instrument.
// ─────────────────────────────────────────────────────────────────────────────
/// The unlit-segment ghost drawn behind a readout, built to the shape of the
/// value in front of it. A fixed "-88.8" only lines up behind a reading of the
/// same width: the font is proportional and the text right-aligned, so behind
/// anything shorter -- "-inf", "n/a", a positive value -- its left end hangs out
/// and reads as stray characters rather than as dark segments.
inline std::string GhostFor(const char* value)
{
  std::string g(value);
  for (char& c : g)
    if (c >= '0' && c <= '9')
      c = '8';
  return g;
}

class NAMDiffReadoutControl : public IControl
{
public:
  NAMDiffReadoutControl(const IRECT& bounds)
  : IControl(bounds)
  {
    SetIgnoreMouse(true); // a readout, and it sits close enough to Delta to matter
  }

  /** dB of difference relative to the output. Any value is legal: the figure is
   * measured against the level-matched dry, so two decorrelated signals of equal
   * power give +3 dB and a model that reshapes the spectrum goes higher still.
   * An amp model sits around +5 dB. "Not applicable" is therefore a separate
   * flag, not a sentinel value -- it used to be +1.0 dB, which every amp model
   * exceeded, so the readout showed "n/a" permanently. */
  void SetValueDB(float db)
  {
    if (db == mDB && !mNotApplicable)
      return;
    mDB = db;
    mNotApplicable = false;
    SetDirty(false);
  }

  /** No honest reading available -- an IR is loaded, so the dry is not aligned. */
  void SetNotApplicable()
  {
    if (mNotApplicable)
      return;
    mNotApplicable = true;
    SetDirty(false);
  }

  void Draw(IGraphics& g) override
  {
    const IColor led = VUHex(0xF0E7D4);
    g.PathRoundRect(mRECT, 3.f);
    g.PathFill(IPattern::CreateLinearGradient(mRECT.L, mRECT.T, mRECT.L, mRECT.B,
                                              {IColorStop(VUHex(0x060808), 0.f), IColorStop(VUHex(0x101414), 1.f)}),
               IFillOptions(), &mBlend);
    g.DrawRoundRect(VUHex(0xFFFFFF, 18), mRECT, 3.f, &mBlend, 1.f);

    WDL_String v;
    const bool na = mNotApplicable;
    if (na)
      v.Set("n/a");
    else if (mDB <= -100.0f)
      v.Set("-inf");
    else
      v.SetFormatted(32, "%.1f", mDB);

    // Caption, number and unit as one tightly-set group around the box's
    // centre, rather than pushed out to its two ends. The pivots are fixed, not
    // measured, so the digits do not shuffle sideways as the value changes --
    // a full-width reading sets the spacing.
    const float cx = mRECT.MW();
    const IRECT capR = IRECT(mRECT.L, mRECT.T, cx - 20.f, mRECT.B);
    const IRECT valR = IRECT(cx - 20.f, mRECT.T, cx + 30.f, mRECT.B);
    const IRECT unitR = IRECT(cx + 33.f, mRECT.T, mRECT.R, mRECT.B);

    g.DrawText(IText(11.f, led.WithOpacity(0.62f), "Roboto-Regular", EAlign::Far, EVAlign::Middle), "DIFF", capR,
               &mBlend);
    const IText lit{17.f, led, "Roboto-Regular", EAlign::Far, EVAlign::Middle};
    // Unlit-segment ghost behind the reading -- see GhostFor().
    g.DrawText(lit.WithFGColor(led.WithOpacity(0.15f)), GhostFor(v.Get()).c_str(), valR, &mBlend);
    g.DrawText(lit, v.Get(), valR, &mBlend);
    if (!na)
      g.DrawText(IText(10.f, led.WithOpacity(0.6f), "Roboto-Regular", EAlign::Near, EVAlign::Middle), "dB", unitR,
                 &mBlend);
  }

private:
  float mDB = -144.0f;
  bool mNotApplicable = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Segmented digital level meter, in dBFS.
//
// Ported from the Claude Design mock ("Gateway Stereo.dc.html"): a lit LED
// ladder per channel either side of a centre dB scale, peak-hold markers, over
// LEDs above each ladder, and a per-channel dB readout beneath.
//
// Scale: 32 segments spanning -39 dBFS to +3 dBFS, with the design's gamma of
// 0.78 expanding the top of the range. The two segments above 0 dBFS are the
// over-range headroom, which is why the ladder tops out at +3 rather than 0.
//
// Like the VU meter, all ballistics live here rather than in the sender, so the
// audio path is untouched.
// ─────────────────────────────────────────────────────────────────────────────
class NAMLevelMeterControl : public IControl
{
  static constexpr int kNChans = 2;
  static constexpr int kNSegs = 32;
  static constexpr double kTopDB = 3.0; // top of the ladder, dBFS
  static constexpr double kBotDB = -39.0; // bottom of the ladder, dBFS
  static constexpr double kGamma = 0.78; // scale expansion toward the top
  static constexpr double kOverDB = -0.1; // the over LED latches at/above this

  // Ballistics: instant attack, then a constant dB/s fall -- the standard
  // digital peak-meter behaviour, and frame-rate independent because the fall
  // is integrated against wall-clock time.
  static constexpr double kBarFallDBPerSec = 20.0;
  static constexpr double kPeakFallDBPerSec = 10.0;
  static constexpr double kRestDB = -60.0; // parked; stop repainting below this
  static constexpr double kStaleMs = 120.0; // no data for this long == silence

public:
  NAMLevelMeterControl(const IRECT& bounds, const char* label)
  : IControl(bounds)
  , mLabel(label)
  {
    for (int c = 0; c < kNChans; c++)
    {
      mTargetDB[c] = mBarDB[c] = mPeakDB[c] = kRestDB;
      mOver[c] = false;
    }
    mLast = mLastMsg = std::chrono::steady_clock::now();
  }

  // Position along the ladder, 0 at the bottom stop, 1 at the top.
  static double Frac(double db)
  {
    const double t = Clip((db - kBotDB) / (kTopDB - kBotDB), 0.0, 1.0);
    return std::pow(t, kGamma);
  }
  // Level at which segment i lights (i = 0 at the bottom).
  static double SegDB(int i) { return kBotDB + (kTopDB - kBotDB) * std::pow((i + 1.0) / kNSegs, 1.0 / kGamma); }

  static IColor SegColor(double db)
  {
    if (db >= -0.5)
      return VUHex(0xFF3B26); // over
    if (db >= -4.0)
      return VUHex(0xFF8A1F);
    if (db >= -9.0)
      return VUHex(0xF2D024);
    return VUHex(0x4CE06A);
  }

  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    if (IsDisabled() || msgTag != ISender<>::kUpdateMessage)
      return;

    IByteStream stream(pData, dataSize);
    int pos = 0;
    ISenderData<kNChans, std::pair<float, float>> d;
    pos = stream.Get(&d, pos);

    for (auto c = d.chanOffset; c < (d.chanOffset + d.nChans) && c < kNChans; c++)
    {
      // std::get<0> is the peak. The sender's own hold is switched off (see
      // NAMSender), so this is the raw per-window peak and the holding below
      // is entirely ours.
      const double amp = static_cast<double>(std::get<0>(d.vals[c]));
      mTargetDB[c] = amp > 1.0e-7 ? 20.0 * std::log10(amp) : kRestDB;
    }
    mLastMsg = std::chrono::steady_clock::now();
    SetDirty(false);
  }

  // Advanced once per frame -- see the note on NAMVUMeterControl::IsDirty.
  bool IsDirty() override
  {
    const auto now = std::chrono::steady_clock::now();
    const double dtMs = Clip(std::chrono::duration<double, std::milli>(now - mLast).count(), 0.0, 200.0);
    mLast = now;
    // Cross-fade with the other meter style. Both are drawn while it runs, so
    // the swap reads as a dissolve rather than a jump cut.
    bool fading = false;
    if (mFade != mFadeTarget)
    {
      const float step = (float)(dtMs / kMeterFadeMs);
      mFade = mFadeTarget > mFade ? std::min(mFadeTarget, mFade + step) : std::max(mFadeTarget, mFade - step);
      mBlend.mWeight = mFade;
      fading = true;
      if (mFade <= 0.0f)
        Hide(true); // sets mDirty, which is what repaints the area underneath
    }

    // The other meter style: keep the clock running but skip the work. mDirty
    // must still be reported -- Hide() sets it, and that is what gets the area
    // underneath repainted; swallowing it leaves the hidden meter's pixels on
    // screen around the edges of the one that replaced it.
    if (IsHidden())
      return mDirty;

    if (std::chrono::duration<double, std::milli>(now - mLastMsg).count() > kStaleMs)
      for (int c = 0; c < kNChans; c++)
        mTargetDB[c] = kRestDB;


    const double barFall = kBarFallDBPerSec * dtMs * 0.001;
    const double peakFall = kPeakFallDBPerSec * dtMs * 0.001;
    bool moving = false;
    for (int c = 0; c < kNChans; c++)
    {
      mBarDB[c] = std::max(mTargetDB[c], std::max(mBarDB[c] - barFall, kRestDB));
      mPeakDB[c] = std::max(mBarDB[c], std::max(mPeakDB[c] - peakFall, kRestDB));
      if (mPeakDB[c] >= kOverDB)
        mOver[c] = true;
      if (mBarDB[c] > kBotDB || mPeakDB[c] > kBotDB)
        moving = true;
    }
    return mDirty || moving || fading;
  }

  // Cross-fade this style in or out. Called by the meter-style switch.
  void SetShown(bool shown)
  {
    mFadeTarget = shown ? 1.0f : 0.0f;
    if (shown)
      Hide(false);
    SetDirty(false);
  }

  // Same, without the dissolve -- for laying out the editor, where fading in
  // from nothing on open would just look like a glitch.
  void SetShownImmediate(bool shown)
  {
    mFade = mFadeTarget = shown ? 1.0f : 0.0f;
    mBlend.mWeight = mFade;
    Hide(!shown);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    for (int c = 0; c < kNChans; c++) // click anywhere to clear holds and overs
    {
      mPeakDB[c] = mBarDB[c];
      mOver[c] = false;
    }
    SetDirty(false);
  }

  void Draw(IGraphics& g) override
  {
    const float readoutH = 20.f;
    const float captionH = 12.f;
    const float gapHousingCaption = 6.f;
    const float gapCaptionReadout = 5.f;

    const IRECT housing = mRECT.GetFromTop(mRECT.H() - readoutH - captionH - gapHousingCaption - gapCaptionReadout);
    const IRECT captionR = IRECT(mRECT.L, housing.B + gapHousingCaption, mRECT.R, housing.B + gapHousingCaption + captionH);
    const IRECT readR = mRECT.GetFromBottom(readoutH);
    const IRECT glass = housing.GetPadded(-7.f);

    // ── housing ───────────────────────────────────────────────────────────
    g.PathRoundRect(housing, 8.f);
    g.PathFill(IPattern::CreateLinearGradient(housing.L, housing.T, housing.L, housing.B,
                                              {IColorStop(VUHex(0x1E2325), 0.f), IColorStop(VUHex(0x0D1011), 1.f)}),
               IFillOptions(), &mBlend);
    g.DrawRoundRect(VUHex(0x000000, 190), housing, 8.f, &mBlend, 1.f);
    g.DrawRoundRect(VUHex(0xFFFFFF, 18), housing.GetPadded(-1.f), 8.f, &mBlend, 1.f);

    // ── glass ─────────────────────────────────────────────────────────────
    g.PathRoundRect(glass, 5.f);
    g.PathFill(IPattern::CreateLinearGradient(glass.L, glass.T, glass.L, glass.B,
                                              {IColorStop(VUHex(0x050706), 0.f), IColorStop(VUHex(0x0A0D0C), 0.6f),
                                               IColorStop(VUHex(0x070909), 1.f)}),
               IFillOptions(), &mBlend);
    g.PathClipRegion(glass);

    // ── geometry ──────────────────────────────────────────────────────────
    const IRECT content = glass.GetPadded(-9.f, -5.f, -9.f, -3.f);
    // Wider bars than the design's 18px: the column here is squarer than the
    // mock's, so the ladders need the extra width to keep their weight.
    const float barW = 24.f, scaleW = 44.f, colGap = 10.f;
    // Tight gaps top and bottom: the ladder is the point of this meter, so it
    // takes every pixel the glass can spare.
    const float ovrH = 5.f, chLabelH = 10.f, stackGap = 3.f;
    const float groupW = barW * 2.f + scaleW + colGap * 2.f;
    const float gx = content.MW() - groupW * 0.5f;

    const float barT = content.T + ovrH + stackGap;
    const float barB = content.B - chLabelH - stackGap;
    const IRECT barL = IRECT(gx, barT, gx + barW, barB);
    const IRECT barRr = IRECT(gx + groupW - barW, barT, gx + groupW, barB);
    const IRECT scaleR = IRECT(barL.R + colGap, barT, barRr.L - colGap, barB);

    // ── the two ladders ───────────────────────────────────────────────────
    for (int c = 0; c < kNChans; c++)
    {
      const IRECT bar = c == 0 ? barL : barRr;

      // trough
      g.PathRoundRect(bar, 2.f);
      g.PathFill(IPattern::CreateLinearGradient(bar.L, bar.T, bar.L, bar.B,
                                                {IColorStop(VUHex(0x0A0D0C), 0.f), IColorStop(VUHex(0x050706), 1.f)}),
                 IFillOptions(), &mBlend);
      g.DrawRoundRect(VUHex(0xFFFFFF, 13), bar, 2.f, &mBlend, 1.f);

      const IRECT segs = bar.GetPadded(-2.f);
      const float segH = segs.H() / (float)kNSegs;

      // A single soft bloom behind the lit column: 32 per-segment glows would
      // be 128 gradient fills a frame, and this reads the same at this size.
      if (mBarDB[c] > kBotDB)
      {
        const float litTop = segs.B - (float)Frac(mBarDB[c]) * segs.H();
        g.FillRoundRect(SegColor(mBarDB[c]).WithOpacity(0.16f), IRECT(bar.L - 3.f, litTop - 2.f, bar.R + 3.f, segs.B),
                        4.f, &mBlend);
      }

      for (int i = 0; i < kNSegs; i++)
      {
        const double segDb = SegDB(i);
        const IColor col = SegColor(segDb);
        const bool lit = mBarDB[c] >= segDb;
        // segment i counts up from the bottom
        const float sB = segs.B - segH * i - 0.5f;
        const float sT = segs.B - segH * (i + 1) + 0.5f;
        const IRECT sr(segs.L, sT, segs.R, sB);
        if (lit)
        {
          g.FillRoundRect(col, sr, 1.f, &mBlend);
          g.FillRoundRect(VUHex(0xFFFFFF, 70), sr.GetFromTop(1.f), 1.f, &mBlend);
        }
        else
        {
          // the design's unlit segment: 13% of the colour over near-black
          g.FillRoundRect(IColor::LinearInterpolateBetween(VUHex(0x060807), col, 0.13f), sr, 1.f, &mBlend);
        }
      }

      // peak-hold marker
      if (mPeakDB[c] > kBotDB)
      {
        const float py = segs.B - (float)Frac(mPeakDB[c]) * segs.H();
        const IColor pc = SegColor(mPeakDB[c]);
        g.FillRoundRect(pc, IRECT(bar.L + 1.f, py - 1.f, bar.R - 1.f, py + 1.f), 1.f, &mBlend);
      }

      // over LED, above the ladder
      const IRECT ovr = IRECT(bar.MW() - 4.5f, content.T, bar.MW() + 4.5f, content.T + ovrH);
      if (mOver[c])
      {
        g.FillRoundRect(VUHex(0xFF3B26, 60), ovr.GetPadded(2.f), 2.f, &mBlend);
        g.FillRoundRect(VUHex(0xFF3B26), ovr, 1.f, &mBlend);
      }
      else
      {
        g.FillRoundRect(VUHex(0x2A1512), ovr, 1.f, &mBlend);
      }

      // channel letter under the ladder
      IText chT(11.f, VUHex(0x94A09B), "Roboto-Regular", EAlign::Center, EVAlign::Middle);
      g.DrawText(chT, c == 0 ? "L" : "R", TextBoxC(bar.MW(), (barB + stackGap + content.B) * 0.5f, 10.f, 6.f),
                 &mBlend);
    }

    // ── centre dB scale ───────────────────────────────────────────────────
    {
      // Numeral flanked by a rule either side, as in the design, but lightened
      // to match the numerals -- at the original 10% white they read as dirt on
      // the glass rather than as scale rules.
      // -5 is dropped from the design's set: it sits 11px from 0 on this ladder,
      // which was forcing every numeral down to a size that would not resolve at
      // 1x. The remaining marks are >=23px apart and can be set at 13px, the same
      // order as the rest of the interface. The colour bands cover the top end.
      const IRECT segs = barL.GetPadded(-2.f); // marks line up with the segments
      for (const double db : {0.0, -10.0, -20.0, -30.0, -39.0})
      {
        const float y = segs.B - (float)Frac(db) * segs.H();
        WDL_String t;
        t.SetFormatted(8, "%d", (int)db);
        IText txt(13.f, VUHex(0xCDD6D2), "Roboto-Regular", EAlign::Center, EVAlign::Middle);
        const IRECT tr = TextBoxC(scaleR.MW(), y, 15.f, 8.f);
        g.DrawText(txt, t.Get(), tr, &mBlend);
        const float ry = std::round(y);
        g.FillRect(VUHex(0xB4BFBA, 120), IRECT(scaleR.L, ry - 0.5f, tr.L - 1.f, ry + 0.5f), &mBlend);
        g.FillRect(VUHex(0xB4BFBA, 120), IRECT(tr.R + 1.f, ry - 0.5f, scaleR.R, ry + 0.5f), &mBlend);
      }
    }

    // ── glass sheen + vignette ────────────────────────────────────────────
    g.PathRoundRect(glass, 5.f);
    g.PathFill(IPattern::CreateLinearGradient(glass.L, glass.T, glass.R, glass.B,
                                              {IColorStop(VUHex(0xFFFFFF, 15), 0.f), IColorStop(VUHex(0xFFFFFF, 4), 0.22f),
                                               IColorStop(VUHex(0xFFFFFF, 0), 0.40f)}),
               IFillOptions(), &mBlend);
    g.PathClipRegion(IRECT());

    // ── caption ───────────────────────────────────────────────────────────
    {
      IText txt(13.f, VUHex(0x9AA6A1), "Roboto-Regular", EAlign::Center, EVAlign::Middle);
      g.DrawText(txt, mLabel.Get(), TextBoxC(captionR.MW(), captionR.MH(), captionR.W() * 0.5f, 9.f), &mBlend);
    }

    // ── per-channel dB readouts ───────────────────────────────────────────
    // These read dBFS -- the peak-hold value, so the number agrees with the
    // marker on the ladder rather than chasing the falling bar.
    const float rGap = 10.f;
    const float rw = (readR.W() - rGap) * 0.5f;
    for (int c = 0; c < kNChans; c++)
    {
      const IRECT box = c == 0 ? IRECT(readR.L, readR.T, readR.L + rw, readR.B)
                               : IRECT(readR.R - rw, readR.T, readR.R, readR.B);
      g.PathRoundRect(box, 3.f);
      g.PathFill(IPattern::CreateLinearGradient(box.L, box.T, box.L, box.B,
                                                {IColorStop(VUHex(0x060808), 0.f), IColorStop(VUHex(0x101414), 1.f)}),
                 IFillOptions(), &mBlend);
      g.DrawRoundRect(VUHex(0xFFFFFF, 18), box, 3.f, &mBlend, 1.f);

      const IColor led = c == 0 ? VUHex(0xF0E7D4) : VUHex(0xFF6152);
      const IRECT inner = box.GetPadded(-5.f, 0.f, -5.f, 0.f);

      // Channel letter, with the unlit-segment ghost behind it.
      const IRECT chR = inner.GetFromLeft(9.f);
      g.DrawText(IText(10.f, led.WithOpacity(0.13f), "Roboto-Regular", EAlign::Near, EVAlign::Middle), "8", chR,
                 &mBlend);
      g.DrawText(IText(10.f, led, "Roboto-Regular", EAlign::Near, EVAlign::Middle), c == 0 ? "L" : "R", chR, &mBlend);

      const IRECT unitR = inner.GetFromRight(15.f);
      g.DrawText(IText(9.f, led.WithOpacity(0.6f), "Roboto-Regular", EAlign::Far, EVAlign::Middle), "dB", unitR,
                 &mBlend);

      WDL_String v;
      if (mPeakDB[c] <= kRestDB + 0.5)
        v.Set("-inf");
      else
        v.SetFormatted(16, "%.1f", mPeakDB[c]);
      const IRECT valR = IRECT(chR.R + 2.f, inner.T, unitR.L - 2.f, inner.B);
      g.DrawText(IText(13.f, led.WithOpacity(0.16f), "Roboto-Regular", EAlign::Far, EVAlign::Middle),
                 GhostFor(v.Get()).c_str(), valR, &mBlend);
      g.DrawText(IText(13.f, led, "Roboto-Regular", EAlign::Far, EVAlign::Middle), v.Get(), valR, &mBlend);
    }
  }

private:
  WDL_String mLabel;
  double mTargetDB[kNChans], mBarDB[kNChans], mPeakDB[kNChans];
  bool mOver[kNChans];
  std::chrono::steady_clock::time_point mLast, mLastMsg;
  float mFade = 1.0f, mFadeTarget = 1.0f;
};

