// A2 model inside the ResamplingContainer exactly as ResamplingNAM wires it:
// does the output depend on the host block size, and does anything throw?
#define NOMINMAX
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>
namespace iplug { static const double PI = 3.141592653589793; }
#define DEFAULT_BLOCK_SIZE 1024
#include "dsp/ResamplingContainer/ResamplingContainer.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

int main(int argc, char** argv)
{
  const double host = 44100.0, model = 48000.0;
  const int n = 44100;
  std::vector<double> x(n);
  for (int i = 0; i < n; i++) x[i] = 0.25 * std::sin(2 * 3.14159265358979 * 110.0 * i / host) * std::exp(-3.0 * std::fmod(i / host, 0.5));
  std::vector<double> ref;
  for (int block : {512, 256, 128, 64})
  {
    auto m = nam::get_dsp(std::filesystem::u8path(argv[1]));
    dsp::ResamplingContainer<double, 1, 12> rc(model);
    rc.Reset(host, block);
    const int encMax = (int)std::ceil(block / (host / model));
    m->ResetAndPrewarm(host, encMax);
    int throws = 0, maxChunk = 0, calls = 0;
    auto f = [&](double** in, double** out, int nn) { maxChunk = std::max(maxChunk, nn); calls++; m->process(in, out, nn); };
    std::vector<double> y(n, 0.0), ib(block), ob(block);
    for (int pos = 0; pos + block <= n; pos += block)
    {
      std::copy(x.begin() + pos, x.begin() + pos + block, ib.begin());
      double* pi = ib.data(); double* po = ob.data();
      try { rc.ProcessBlock(&pi, &po, block, f); } catch (const std::exception& e) { throws++; if (throws < 3) std::printf("  throw: %s\n", e.what()); }
      std::copy(ob.begin(), ob.end(), y.begin() + pos);
    }
    if (block == 512) ref = y;
    double md = 0; for (int i = 0; i < n - 512; i++) md = std::max(md, std::fabs(y[i] - ref[i]));
    std::printf("block %3d: encapsulated max %d, largest chunk %d, calls %d, throws %d, max diff vs 512: %.3g\n", block, encMax, maxChunk, calls, throws, md);
  }
}
