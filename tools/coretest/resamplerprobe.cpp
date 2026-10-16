// Measures what the AudioDSPTools ResamplingContainer (as used by ResamplingNAM)
// actually does: real delay vs GetLatency(), and block-size dependence.
#include <cmath>
#include <cstdio>
#include <vector>
#define NOMINMAX
namespace iplug { static const double PI = 3.141592653589793; }
#define DEFAULT_BLOCK_SIZE 1024
#include "dsp/ResamplingContainer/ResamplingContainer.h"

static std::vector<double> Run(double hostSR, double modelSR, int block, const std::vector<double>& x, int* reported)
{
  dsp::ResamplingContainer<double, 1, 12> rc(modelSR);
  rc.Reset(hostSR, block);
  *reported = rc.GetLatency();
  std::vector<double> y(x.size(), 0.0), ib(block), ob(block);
  auto ident = [](double** in, double** out, int n) { for (int i = 0; i < n; i++) out[0][i] = in[0][i]; };
  for (size_t pos = 0; pos + block <= x.size(); pos += block)
  {
    std::copy(x.begin() + pos, x.begin() + pos + block, ib.begin());
    double* pi = ib.data(); double* po = ob.data();
    rc.ProcessBlock(&pi, &po, block, ident);
    std::copy(ob.begin(), ob.end(), y.begin() + pos);
  }
  return y;
}

int main()
{
  const int n = 48000;
  for (auto [h, m] : {std::pair<double,double>{44100, 48000}, {48000, 44100}, {96000, 48000}, {88200, 48000}})
  {
    // band-limited test: sum of sines; impulse response via a single-sample impulse
    std::vector<double> imp(n, 0.0); imp[2000] = 1.0;
    std::vector<double> sig(n);
    for (int i = 0; i < n; i++) sig[i] = 0.3*std::sin(2*3.14159265358979*997.0*i/h) + 0.2*std::sin(2*3.14159265358979*5013.0*i/h);
    std::vector<double> ref;
    for (int block : {512, 256, 128, 64, 100})
    {
      int rep = 0;
      auto yi = Run(h, m, block, imp, &rep);
      // centroid of the impulse response => group delay
      double s = 0, sw = 0; int peak = 0; double pv = 0;
      for (int i = 0; i < n; i++) { s += yi[i]*yi[i]*i; sw += yi[i]*yi[i]; if (std::fabs(yi[i]) > pv) { pv = std::fabs(yi[i]); peak = i; } }
      auto ys = Run(h, m, block, sig, &rep);
      if (block == 512) ref = ys;
      double md = 0; for (int i = 4000; i < n - 1000; i++) md = std::max(md, std::fabs(ys[i]-ref[i]));
      // 1 kHz tone round trip, null after removing the (rounded) group delay
      std::vector<double> tone(n);
      for (int i = 0; i < n; i++)
        tone[i] = 0.25 * std::sin(2 * 3.14159265358979 * 1000.0 * i / h);
      auto yt = Run(h, m, block, tone, &rep);
      const int d = (int)std::lround(s / sw - 2000);
      double e = 0, r = 0, ry = 0;
      for (int i = 4000; i < n - 1000; i++)
      {
        e += (yt[i + d] - tone[i]) * (yt[i + d] - tone[i]);
        r += tone[i] * tone[i];
        ry += yt[i + d] * yt[i + d];
      }
      std::printf("host %6.0f model %6.0f block %3d: reported %2d  impulse peak at +%d  energy centroid +%.2f  max diff vs 512: %.3g"
                  "  | 1 kHz round trip: level %.4f dB, null %.1f dB\n",
                  h, m, block, rep, peak - 2000, s / sw - 2000, md, 10 * std::log10(ry / r), 10 * std::log10(e / r));
    }
  }
}
