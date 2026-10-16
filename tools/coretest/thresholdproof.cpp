// Checks whether the 1.2.0 threshold migration
//     T_new = T_old - max(InputGain_A, InputGain_B)   [dB, active chains only]
// reproduces the 1.1.0 gate decision when the detector moves from AFTER the
// input gains to BEFORE them.
//
// 1.1.0 (NeuralAmpModeler.cpp, _ProcessSubBlock): the side chain fed to the
// unmodified dsp::noise_gate::Trigger was  s[n] = max_c |g_c * src_c[n]|  over
// the active chains, with TriggerParams(0.01, T, 0.1, 0.005, 0.01, 0.05).
// The Trigger squares s, smooths it (level), works in 10*log10(level) and
// compares against T. If src_A == src_B == x (all mono routings), then
// s = G*|x| with G = max(g_A, g_B), level scales by G^2, i.e. +20*log10(G) dB,
// so "level(G*x) vs T" == "level(x) vs T - 20*log10(G)" -- except where the
// Trigger clamps the level (floor -120 dB, ceiling +30 dB).
//
// This program runs the real Trigger both ways and compares the gain
// reductions it produces, then repeats with independent L/R sources (1.1.0
// "Stereo Independent"), where no single offset can be exact.
//
//   thresholdproof          exit 0 = mono cases equivalent (above -60 dB)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/NoiseGate.h"

static std::vector<double> GainDb(const std::vector<double>& sidechain, double threshold, double sr)
{
  dsp::noise_gate::Trigger trig;
  dsp::noise_gate::Gain gain;
  trig.AddListener(&gain);
  trig.SetSampleRate(sr);
  trig.SetParams(dsp::noise_gate::TriggerParams(0.01, threshold, 0.1, 0.005, 0.01, 0.05));
  const int blk = 128;
  std::vector<double> out(sidechain.size()), sc(blk), one(blk, 1.0);
  for (size_t pos = 0; pos < sidechain.size(); pos += blk)
  {
    const int n = (int)std::min<size_t>(blk, sidechain.size() - pos);
    std::copy(sidechain.begin() + pos, sidechain.begin() + pos + n, sc.begin());
    double* p[2] = {sc.data(), sc.data()};
    trig.Process(p, 2, n);
    double* q[2] = {one.data(), one.data()};
    double** g = gain.Process(q, 2, n);
    for (int i = 0; i < n; i++)
      out[pos + i] = 20.0 * std::log10(std::max(g[0][i], 1e-300));
  }
  return out;
}

// Plucked notes with silence and a -90 dBFS noise floor between them.
static std::vector<double> Guitar(int n, double sr, unsigned seed)
{
  std::vector<double> x((size_t)n);
  for (int i = 0; i < n; i++)
  {
    const double t = i / sr;
    const double ph = std::fmod(t, 0.6);
    const double env = ph < 0.35 ? std::exp(-6.0 * ph) : 0.0;
    seed = seed * 1664525u + 1013904223u;
    const double noise = ((double)(seed >> 8) / 8388608.0 - 1.0) * 3.0e-5;
    x[(size_t)i] = 0.3 * env * std::sin(2 * 3.14159265358979 * 110.0 * t) + noise;
  }
  return x;
}

// The Trigger's level follower and threshold comparison, exactly as in
// NoiseGate.cpp:41 and :56-58 (alpha from the 10 ms half-life, power clamped to
// [1e-12, 1000], compared in 10*log10). Returns, per sample, whether the level
// is at or above the threshold -- the gate's open/close DECISION.
static std::vector<char> Decisions(const std::vector<double>& sidechain, double threshold, double sr,
                                   std::vector<char>* clamped)
{
  const double alpha = std::pow(0.5, 1.0 / (0.01 * sr));
  const double beta = 1.0 - alpha;
  const double minPower = std::pow(10.0, -120.0 / 10.0);
  double level = minPower;
  std::vector<char> d(sidechain.size());
  if (clamped)
    clamped->assign(sidechain.size(), 0);
  for (size_t i = 0; i < sidechain.size(); i++)
  {
    const double raw = alpha * level + beta * sidechain[i] * sidechain[i];
    level = std::clamp(raw, minPower, 1000.0);
    if (clamped && raw != level)
      (*clamped)[i] = 1;
    d[i] = 10.0 * std::log10(level) >= threshold;
  }
  return d;
}

int main()
{
  const double sr = 48000.0;
  const int n = (int)(3.0 * sr);
  const auto x = Guitar(n, sr, 1u);
  int failures = 0;

  std::printf("A) Open/close DECISION of the 1.1.0 level detector, mono routing\n");
  std::printf("%8s %8s %8s | %8s | %s\n", "T_old", "gA dB", "gB dB", "T_new", "samples with different decision (outside level clamping)");
  {
    struct D
    {
      double t, ga, gb;
    } cases[] = {{-70, 6, -3}, {-60, 12, 12}, {-50, -10, 4}, {-85, 10, 0}};
    for (const auto& m : cases)
    {
      const double gA = std::pow(10.0, m.ga / 20.0), gB = std::pow(10.0, m.gb / 20.0);
      std::vector<double> sOld((size_t)n), sNew((size_t)n);
      for (int i = 0; i < n; i++)
      {
        sOld[(size_t)i] = std::max(std::fabs(gA * x[(size_t)i]), std::fabs(gB * x[(size_t)i]));
        sNew[(size_t)i] = std::fabs(x[(size_t)i]);
      }
      const double tNew = m.t - std::max(m.ga, m.gb);
      std::vector<char> cOld, cNew;
      const auto dOld = Decisions(sOld, m.t, sr, &cOld), dNew = Decisions(sNew, tNew, sr, &cNew);
      int diff = 0, diffClamped = 0, opens = 0;
      for (int i = 0; i < n; i++)
      {
        opens += (i > 0 && dOld[(size_t)i] && !dOld[(size_t)i - 1]) ? 1 : 0;
        if (dOld[(size_t)i] != dNew[(size_t)i])
          (cOld[(size_t)i] || cNew[(size_t)i]) ? diffClamped++ : diff++;
      }
      failures += diff ? 1 : 0;
      std::printf("%8.0f %8.0f %8.0f | %8.0f | %d of %d (%d within clamping; %d openings)  %s\n", m.t, m.ga, m.gb, tNew,
                  diff, n, diffClamped, opens, diff ? "DIFFERENT" : "IDENTICAL DECISIONS");
    }
  }
  std::printf("\nB) Complete gain trajectory of the 1.1.0 gate\n");

  std::printf("Mono routing (src_A == src_B): side chain = max(gA,gB)*|x|\n");
  int trajectoryDiffers = 0;
  std::printf("%8s %8s %8s | %10s | %22s\n", "T_old", "gA dB", "gB dB", "T_new", "max |gain diff| (dB, where gain > -60 dB)");
  struct M
  {
    double t, ga, gb;
  } mono[] = {{-70, 6, -3}, {-80, 0, 0}, {-60, 12, 12}, {-50, -10, 4}, {-90, 15, 0}};
  for (const auto& m : mono)
  {
    const double gA = std::pow(10.0, m.ga / 20.0), gB = std::pow(10.0, m.gb / 20.0);
    std::vector<double> sOld((size_t)n), sNew((size_t)n);
    for (int i = 0; i < n; i++)
    {
      sOld[(size_t)i] = std::max(std::fabs(gA * x[(size_t)i]), std::fabs(gB * x[(size_t)i]));
      sNew[(size_t)i] = std::fabs(x[(size_t)i]);
    }
    const double tNew = m.t - std::max(m.ga, m.gb);
    const auto gOld = GainDb(sOld, m.t, sr), gNew = GainDb(sNew, tNew, sr);
    double maxDiff = 0.0, maxDiffAll = 0.0;
    for (size_t i = 0; i < gOld.size(); i++)
    {
      const double d = std::fabs(gOld[i] - gNew[i]);
      maxDiffAll = std::max(maxDiffAll, d);
      if (std::max(gOld[i], gNew[i]) > -60.0)
        maxDiff = std::max(maxDiff, d);
    }
    const bool ok = maxDiff < 1e-6;
    trajectoryDiffers += ok ? 0 : 1;
    std::printf("%8.0f %8.0f %8.0f | %10.0f | %10.3g   (everywhere incl. deep reduction: %.3g)  %s\n", m.t, m.ga, m.gb,
                tNew, maxDiff, maxDiffAll, ok ? "EQUIVALENT" : "DIFFERENT");
  }

  std::printf("\nStereo Independent (src_A = L, src_B = R, different signals): side chain = max(gA|L|, gB|R|)\n");
  const auto r = Guitar(n, sr, 77u);
  std::vector<double> rr((size_t)n);
  for (int i = 0; i < n; i++)
    rr[(size_t)i] = 0.3 * r[(size_t)((i + 9000) % n)]; // other note timing, 10 dB quieter
  struct S
  {
    double t, ga, gb;
  } st[] = {{-70, 6, -3}, {-70, 0, 0}};
  for (const auto& m : st)
  {
    const double gA = std::pow(10.0, m.ga / 20.0), gB = std::pow(10.0, m.gb / 20.0);
    std::vector<double> sOld((size_t)n), sNew((size_t)n);
    for (int i = 0; i < n; i++)
    {
      sOld[(size_t)i] = std::max(std::fabs(gA * x[(size_t)i]), std::fabs(gB * rr[(size_t)i]));
      sNew[(size_t)i] = std::fabs(x[(size_t)i]); // 1.2.0: detector on the left input only
    }
    for (double tNew : {m.t - std::max(m.ga, m.gb), m.t - m.ga})
    {
      const auto gOld = GainDb(sOld, m.t, sr), gNew = GainDb(sNew, tNew, sr);
      double maxDiff = 0.0;
      for (size_t i = 0; i < gOld.size(); i++)
        if (std::max(gOld[i], gNew[i]) > -60.0)
          maxDiff = std::max(maxDiff, std::fabs(gOld[i] - gNew[i]));
      std::printf("%8.0f %8.0f %8.0f | T_new %6.0f | max |gain diff| %.3g dB  (not expected to be exact)\n", m.t, m.ga,
                  m.gb, tNew, maxDiff);
    }
  }
  std::printf("\nResult: decisions %s; full gain trajectory %s (old gate: depth and slew rate depend on T).\n",
              failures ? "DIFFER" : "IDENTICAL", trajectoryDiffers ? "NOT identical" : "identical");
  return failures;
}
