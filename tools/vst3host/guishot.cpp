// Opens the plugin's real VST3 editor (IPlugView) in a window, the way a DAW
// does, takes screenshots and drives it with mouse clicks.
//
//   guishot <DualNAM_A2_Full.vst3> <repo-root> <out-dir>
//
// Checks:
//  * the editor opens and has the expected size
//  * file names from a restored project appear (and a missing file is marked)
//  * clicking a switch changes its parameter
//  * clicking the browser's "next file" arrow loads the next model in the folder
// Writes gui_*.bmp into out-dir. Exit code = failures.

#include "host.h"
#include "crashtrace.h"

#define NOMINMAX
#include <Windows.h>

#include "pluginterfaces/gui/iplugview.h"

namespace Steinberg
{
DEF_CLASS_IID(IPlugFrame)
}

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

static int gFail = 0;
static void Check(bool ok, const char* what, const char* detail = "")
{
  std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what, detail);
  if (!ok)
    gFail++;
}

static void Pump(int ms)
{
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end)
  {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

static bool SaveBMP(HWND hwnd, const std::string& path)
{
  // Render ONLY this window's own content (PrintWindow), never copy pixels off
  // the screen: a screen copy picks up whatever window happens to be on top at
  // that moment -- which once put a frame of a video call into a test screenshot.
  RECT rc;
  GetClientRect(hwnd, &rc);
  const int w = rc.right, h = rc.bottom;
  HDC wdc = GetDC(hwnd);
  HDC mem = CreateCompatibleDC(wdc);
  HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  const BOOL printed = PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
  SelectObject(mem, old);
  ReleaseDC(hwnd, wdc);
  if (!printed)
  {
    DeleteObject(bmp);
    DeleteDC(mem);
    return false;
  }
  HDC screen = GetDC(nullptr);

  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(bi);
  bi.biWidth = w;
  bi.biHeight = h;
  bi.biPlanes = 1;
  bi.biBitCount = 24;
  bi.biCompression = BI_RGB;
  const int stride = ((w * 3 + 3) / 4) * 4;
  std::vector<char> pixels((size_t)stride * h);
  GetDIBits(mem, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
  BITMAPFILEHEADER bf{};
  bf.bfType = 0x4D42;
  bf.bfOffBits = sizeof(bf) + sizeof(bi);
  bf.bfSize = bf.bfOffBits + (DWORD)pixels.size();
  FILE* f = fopen(path.c_str(), "wb");
  if (!f)
    return false;
  fwrite(&bf, sizeof(bf), 1, f);
  fwrite(&bi, sizeof(bi), 1, f);
  fwrite(pixels.data(), 1, pixels.size(), f);
  fclose(f);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return true;
}

// What a DAW hands the editor before attached(): the editor calls resizeView()
// on it (iPlug2 does so straight away when the screen is DPI-scaled).
class Frame : public IPlugFrame
{
public:
  HWND hwnd = nullptr;
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
  {
    QUERY_INTERFACE(iid, obj, FUnknown::iid, IPlugFrame)
    QUERY_INTERFACE(iid, obj, IPlugFrame::iid, IPlugFrame)
    *obj = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override
  {
    if (hwnd && r)
    {
      RECT wr{0, 0, r->getWidth(), r->getHeight()};
      AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
      SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
      resizes++;
    }
    if (view && r)
      view->onSize(r);
    return kResultTrue;
  }
  int resizes = 0;
};

// Click at plugin coordinates (the editor design space, 900 wide).
static void Click(HWND top, double scale, float x, float y)
{
  HWND child = GetWindow(top, GW_CHILD);
  HWND target = child ? child : top;
  // iPlug2 may nest one more level.
  if (HWND grand = GetWindow(target, GW_CHILD))
    target = grand;
  const LPARAM pos = MAKELPARAM((int)(x * scale), (int)(y * scale));
  SendMessageW(target, WM_MOUSEMOVE, 0, pos);
  Pump(50);
  SendMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, pos);
  Pump(30);
  SendMessageW(target, WM_LBUTTONUP, 0, pos);
  Pump(400);
}

int main(int argc, char** argv)
{
  if (argc < 4)
  {
    std::printf("usage: guishot <plugin.vst3> <repo-root> <out-dir>\n");
    return 2;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  InstallCrashTrace();
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const fs::path repo = argv[2], out = argv[3];
  fs::create_directories(out);
  const fs::path gen = repo / "tests" / "generated";

  Plug plug;
  std::string err;
  // --mono-bus: run on a mono input bus (1-2) instead of stereo (2-2).
  const bool monoBus = argc > 4 && std::string(argv[4]) == "--mono-bus";
  const int nIn = monoBus ? 1 : 2;
  if (!plug.load(argv[1], err) || !plug.setup(48000.0, 256, nIn, 2, err))
  {
    std::printf("load/setup failed: %s\n", err.c_str());
    return 1;
  }
  plug.activate(true);

  DualState st;
  st.nam[0] = (gen / "A2_full_only.nam").string();
  st.nam[1] = "C:\\missing\\folder\\My Missing Amp.nam";
  st.ir[0] = (gen / "ir_lowpass_48k.wav").string();
  st.ir[1] = (gen / "ir_bright_48k_pcm24.wav").string();
  st.params[kPanA] = -60.0;
  st.params[kPanB] = 60.0;
  st.params[kInputA] = 4.0;
  Check(plug.setState(BuildState(st)), "restore a project with three files and one missing file");

  IPtr<IPlugView> view = owned(plug.controller()->createView(ViewType::kEditor));
  Check(view != nullptr, "IEditController::createView(\"editor\")");
  if (!view)
    return 1;
  Check(view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue, "editor supports HWND");
  ViewRect vr;
  view->getSize(&vr);
  char buf[128];
  snprintf(buf, sizeof(buf), "(%d x %d)", vr.getWidth(), vr.getHeight());
  Check(vr.getWidth() > 0 && vr.getHeight() > 0, "editor reports a size", buf);

  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"DualNAMGuiShot";
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  RegisterClassW(&wc);
  RECT wr{0, 0, vr.getWidth(), vr.getHeight()};
  AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"Dual NAM A2 Full - editor test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40,
                            40, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, wc.hInstance, nullptr);
  SetForegroundWindow(hwnd);
  Frame frame;
  frame.hwnd = hwnd;
  view->setFrame(&frame);
  Check(view->attached(hwnd, kPlatformTypeHWND) == kResultOk, "IPlugView::attached");

  // Keep audio running (a plucked test signal) so the output meter has something to show.
  std::atomic<bool> stopAudio{false};
  std::thread audio([&]() {
    std::vector<double> x(256), l(256), r(256);
    std::vector<const double*> ip = {x.data(), x.data()};
    ip.resize((size_t)nIn);
    std::vector<double*> op = {l.data(), r.data()};
    long long n = 0;
    while (!stopAudio.load())
    {
      for (int i = 0; i < 256; i++, n++)
      {
        const double t = n / 48000.0;
        x[(size_t)i] = 0.3 * std::exp(-3.0 * std::fmod(t, 0.5)) * std::sin(2.0 * 3.14159265358979 * 110.0 * t);
      }
      plug.process(ip.data(), op.data(), 256);
      std::this_thread::sleep_for(std::chrono::microseconds(5333));
    }
  });
  Pump(2500);

  RECT cr;
  GetClientRect(hwnd, &cr);
  const double scale = cr.right / 900.0;
  // Self-test of the capture: cover the editor with a topmost pure-red window.
  // The screenshot must still show the editor, not the red window.
  {
    WNDCLASSW rc{};
    rc.lpfnWndProc = DefWindowProcW;
    rc.hInstance = GetModuleHandleW(nullptr);
    rc.lpszClassName = L"DualNAMGuiShotCover";
    rc.hbrBackground = CreateSolidBrush(RGB(255, 0, 0));
    RegisterClassW(&rc);
    RECT wr;
    GetWindowRect(hwnd, &wr);
    HWND cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, rc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, wr.left,
                                 wr.top, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, rc.hInstance, nullptr);
    Pump(400);
    const std::string probe = (out / "capture_selftest.bmp").string();
    bool redFound = true;
    if (SaveBMP(hwnd, probe))
    {
      FILE* f = fopen(probe.c_str(), "rb");
      std::vector<unsigned char> d;
      if (f)
      {
        fseek(f, 0, SEEK_END);
        d.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        fread(d.data(), 1, d.size(), f);
        fclose(f);
      }
      size_t red = 0, total = 0;
      for (size_t i = 54; i + 2 < d.size(); i += 3, total++)
        if (d[i] < 30 && d[i + 1] < 30 && d[i + 2] > 225) // BGR
          red++;
      redFound = total == 0 || red * 100 > total; // more than 1 % pure red = cover captured
      remove(probe.c_str());
    }
    DestroyWindow(cover);
    Pump(300);
    Check(!redFound, "screenshot captures only the editor window, not windows lying on top of it");
    if (redFound)
    {
      std::printf("aborting: screenshots could contain other windows\n");
      stopAudio = true;
      audio.join();
      view->removed();
      return 1;
    }
  }
  Check(SaveBMP(hwnd, (out / "gui_1_restored.bmp").string()), "screenshot after restore", "gui_1_restored.bmp");

  // Click "Mute A" (switch row of panel A, 4th cell).
  const double muteBefore = plug.getParamNorm(kMuteA);
  Click(hwnd, scale, 387.f, 352.f);
  const double muteAfter = plug.getParamNorm(kMuteA);
  snprintf(buf, sizeof(buf), "(%.0f -> %.0f)", muteBefore, muteAfter);
  Check(muteBefore == 0.0 && muteAfter == 1.0, "click on 'Mute A' switches the parameter on", buf);
  Click(hwnd, scale, 387.f, 352.f);
  Check(plug.getParamNorm(kMuteA) == 0.0, "second click switches it off again");

  // Click B's "IR Bypass" (3rd cell of panel B).
  Click(hwnd, scale, 455.f + 6.f + 2.5f * 103.25f, 352.f);
  Check(plug.getParamNorm(kIRBypassB) == 1.0, "click on 'IR Bypass B' switches it on");
  Pump(300);
  SaveBMP(hwnd, (out / "gui_2_after_clicks.bmp").string());

  // Gate panel: "Gate" on (Threshold / Attack / Release / Position are
  // greyed out while it is off), then the vertical Pre / Post NAM slide switch.
  // Like every slide switch, a click on its track toggles it (Pre on top,
  // Post below); its labels are not clickable.
  {
    const float trackX = 54.f, trackY = 566.f;
    Check(plug.getParamNorm(kGatePosition) == 1.0, "Gate Position defaults to Post NAM");
    Click(hwnd, scale, 77.f, 489.f);
    Check(plug.getParamNorm(kNoiseGateActive) == 1.0, "click on 'Gate' switches the gate on");
    Click(hwnd, scale, trackX, trackY);
    Check(plug.getParamNorm(kGatePosition) == 0.0, "click on the Position switch moves it up to Pre NAM");
    Pump(600);
    SaveBMP(hwnd, (out / "gui_2b_gate_pre.bmp").string());
    Click(hwnd, scale, 100.f, 575.f);
    Check(plug.getParamNorm(kGatePosition) == 0.0, "click on the 'Post NAM' label changes nothing (only the track switches)");
    Click(hwnd, scale, trackX, trackY);
    Check(plug.getParamNorm(kGatePosition) == 1.0, "second click moves it down to Post NAM");
    Pump(600);
    SaveBMP(hwnd, (out / "gui_2c_gate_post.bmp").string());
    Click(hwnd, scale, 77.f, 489.f);
    Check(plug.getParamNorm(kNoiseGateActive) == 0.0, "second click on 'Gate' switches it off");
    Pump(600);
    SaveBMP(hwnd, (out / "gui_2d_gate_off.bmp").string());
    // Like every control of the NAM GUI, greyed-out gate controls stay usable
    // (SetMouseEventsWhenDisabled): the position can be preset with the gate off.
    Click(hwnd, scale, trackX, trackY);
    Check(plug.getParamNorm(kGatePosition) == 0.0 && plug.getParamNorm(kNoiseGateActive) == 0.0,
          "with the gate off (greyed out) the position can still be preset, the gate stays off");
    Click(hwnd, scale, trackX, trackY);
    Pump(300);
  }

  // Browser of model A: the "next file" arrow loads the next .nam in the folder.
  std::vector<char> s0, s1;
  plug.getState(s0);
  DualState before, after;
  ParseState(s0, before);
  Click(hwnd, scale, 124.f, 122.f);
  Pump(1500);
  plug.getState(s1);
  ParseState(s1, after);
  const bool changed = after.nam[0] != before.nam[0] && fs::path(after.nam[0]).extension() == ".nam" &&
                       fs::path(after.nam[0]).parent_path() == fs::path(before.nam[0]).parent_path();
  std::string detail = "(" + fs::path(before.nam[0]).filename().string() + " -> " + fs::path(after.nam[0]).filename().string() + ")";
  Check(changed, "'next file' arrow on model browser A loads the next model", detail.c_str());
  Check(after.nam[1] == before.nam[1] && after.ir[0] == before.ir[0], "channel B and the IRs are untouched");
  Pump(500);
  SaveBMP(hwnd, (out / "gui_3_next_model.bmp").string());

  stopAudio = true;
  audio.join();
  view->removed();
  view = nullptr;
  DestroyWindow(hwnd);
  std::printf("\n%s (%d failure%s)\n", gFail ? "FAILED" : "ALL PASSED", gFail, gFail == 1 ? "" : "s");
  return gFail;
}
