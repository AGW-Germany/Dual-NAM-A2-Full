// Verifies against the NeuralAmpModelerCore that is compiled into the plugin
// that SetSlimmableSize(1.0) on an A2 container selects the A2 Full submodel
// (8 channels) and not A2 Lite (3 channels), and that A2 Full runs on the
// A2 fast path.
//
//   slimtest <A2.nam> <A2_full_only.nam> <A2_lite_only.nam>
//
// Exit code 0 = all checks passed.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

#include "NAM/container.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"
#include "NAM/wavenet/a2_fast.h"
#include "json.hpp"

static int gFailures = 0;
static void Check(bool ok, const std::string& what)
{
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok)
    gFailures++;
}

static std::vector<NAM_SAMPLE> TestSignal(int n, double sr)
{
  // Plucked-string-like test: decaying harmonics plus a little noise, peaks ~0.5.
  std::vector<NAM_SAMPLE> x((size_t)n);
  unsigned seed = 12345u;
  for (int i = 0; i < n; i++)
  {
    const double t = i / sr;
    const double env = std::exp(-3.0 * std::fmod(t, 0.5));
    double v = 0.0;
    for (int h = 1; h <= 6; h++)
      v += std::sin(2.0 * 3.14159265358979 * 110.0 * h * t) / h;
    seed = seed * 1664525u + 1013904223u;
    const double noise = ((double)(seed >> 8) / 8388608.0 - 1.0) * 0.01;
    x[(size_t)i] = (NAM_SAMPLE)(0.25 * env * v + noise);
  }
  return x;
}

static std::vector<NAM_SAMPLE> Render(nam::DSP& model, const std::vector<NAM_SAMPLE>& in, int block)
{
  model.Reset(48000.0, block);
  std::vector<NAM_SAMPLE> out(in.size(), 0.0);
  std::vector<NAM_SAMPLE> ib((size_t)block), ob((size_t)block);
  for (size_t pos = 0; pos < in.size(); pos += (size_t)block)
  {
    const int n = (int)std::min((size_t)block, in.size() - pos);
    std::copy(in.begin() + pos, in.begin() + pos + n, ib.begin());
    NAM_SAMPLE* pi = ib.data();
    NAM_SAMPLE* po = ob.data();
    model.process(&pi, &po, n);
    std::copy(ob.begin(), ob.begin() + n, out.begin() + pos);
  }
  return out;
}

static double MaxDiff(const std::vector<NAM_SAMPLE>& a, const std::vector<NAM_SAMPLE>& b)
{
  double m = 0.0;
  for (size_t i = 0; i < a.size(); i++)
    m = std::max(m, (double)std::fabs(a[i] - b[i]));
  return m;
}
static double Rms(const std::vector<NAM_SAMPLE>& a)
{
  double s = 0.0;
  for (auto v : a)
    s += (double)v * v;
  return std::sqrt(s / (double)a.size());
}

int main(int argc, char** argv)
{
  if (argc < 4)
  {
    std::printf("usage: slimtest <A2.nam> <A2_full_only.nam> <A2_lite_only.nam>\n");
    return 2;
  }
  const std::string containerPath = argv[1], fullPath = argv[2], litePath = argv[3];

  // 1. The A2 file is a container with Lite + Full, and Full is the 8-channel shape.
  {
    std::ifstream f(containerPath);
    nlohmann::json j;
    f >> j;
    Check(j["architecture"] == "SlimmableContainer", "A2.nam is a SlimmableContainer");
    const auto& subs = j["config"]["submodels"];
    Check(subs.size() == 2, "A2.nam holds exactly 2 submodels");
    for (const auto& s : subs)
    {
      int channels = 0;
      const bool isA2 = nam::wavenet::a2_fast::is_a2_shape(s["model"]["config"], &channels);
      std::printf("       submodel max_value=%.2f  is_a2_shape=%d  channels=%d\n", s["max_value"].get<double>(),
                  (int)isA2, channels);
    }
    int chFull = 0, chLite = 0;
    Check(nam::wavenet::a2_fast::is_a2_shape(subs[1]["model"]["config"], &chFull) && chFull == 8,
          "submodel at max_value 1.0 is A2 Full (8 channels) and matches the A2 fast-path shape");
    Check(nam::wavenet::a2_fast::is_a2_shape(subs[0]["model"]["config"], &chLite) && chLite == 3,
          "submodel at max_value 0.5 is A2 Lite (3 channels)");
  }

  // 2. The standalone Full model is instantiated as the A2 fast-path class.
  auto full = nam::get_dsp(std::filesystem::u8path(fullPath));
  auto lite = nam::get_dsp(std::filesystem::u8path(litePath));
  const std::string fullType = typeid(*full).name();
  const std::string liteType = typeid(*lite).name();
  std::printf("       A2_full_only type: %s\n       A2_lite_only type: %s\n", fullType.c_str(), liteType.c_str());
  Check(fullType.find("A2FastModel<8>") != std::string::npos, "A2 Full runs on A2FastModel<8> (fast path enabled)");
  Check(liteType.find("A2FastModel<3>") != std::string::npos, "A2 Lite runs on A2FastModel<3>");

  const auto x = TestSignal(48000 * 2, 48000.0);
  const auto yFull = Render(*full, x, 256);
  const auto yLite = Render(*lite, x, 256);
  std::printf("       rms(in)=%.4f rms(full)=%.4f rms(lite)=%.4f max|full-lite|=%.4f\n", Rms(x), Rms(yFull),
              Rms(yLite), MaxDiff(yFull, yLite));
  Check(MaxDiff(yFull, yLite) > 1e-3, "Full and Lite produce clearly different output (test is discriminating)");

  // 3. Container + SetSlimmableSize(1.0)  ==  Full, bit for bit.
  {
    auto c = nam::get_dsp(std::filesystem::u8path(containerPath));
    auto* slim = dynamic_cast<nam::SlimmableModel*>(c.get());
    Check(slim != nullptr, "A2.nam loads as a SlimmableModel");
    if (slim)
    {
      slim->SetSlimmableSize(1.0);
      const auto y = Render(*c, x, 256);
      std::printf("       SetSlimmableSize(1.0): max|y-full|=%.3g  max|y-lite|=%.3g\n", MaxDiff(y, yFull),
                  MaxDiff(y, yLite));
      Check(MaxDiff(y, yFull) == 0.0, "SetSlimmableSize(1.0) output is bit-identical to A2 Full");
      Check(MaxDiff(y, yLite) > 1e-3, "SetSlimmableSize(1.0) output is NOT A2 Lite");
    }
  }

  // 4. Document the trap: 0.0 (the default of an unset "Slim" control) selects Lite.
  {
    auto c = nam::get_dsp(std::filesystem::u8path(containerPath));
    auto* slim = dynamic_cast<nam::SlimmableModel*>(c.get());
    if (slim)
    {
      slim->SetSlimmableSize(0.0);
      const auto y = Render(*c, x, 256);
      Check(MaxDiff(y, yLite) == 0.0, "SetSlimmableSize(0.0) selects A2 Lite (why the plugin must set 1.0)");
    }
  }

  // 5. Freshly loaded container without any call: which one is active?
  {
    auto c = nam::get_dsp(std::filesystem::u8path(containerPath));
    const auto y = Render(*c, x, 256);
    std::printf("       container default (no SetSlimmableSize): %s\n",
                MaxDiff(y, yFull) == 0.0 ? "Full" : (MaxDiff(y, yLite) == 0.0 ? "Lite" : "neither"));
  }

  std::printf("\n%s (%d failure%s)\n", gFailures ? "FAILED" : "ALL PASSED", gFailures, gFailures == 1 ? "" : "s");
  return gFailures ? 1 : 0;
}
