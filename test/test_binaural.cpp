// Sanity check for binaural.h: an impulse from each virtual speaker should reach the
// nearer ear first and louder. Prints arrival delay difference and level difference per ear.
#include <cstdio>
#include <cmath>
#include "../src/binaural.h"

int main() {
  const int fs = 48000, n = 2048;
  int fails = 0;
  for (int ch = 0; ch < 12; ch++) {
    const binaural::Speaker& s = binaural::kCicp19[ch];
    if (s.lfe) continue;
    binaural::Renderer r;
    r.setup(binaural::kCicp19, 12, fs, 0.0f);
    std::vector<int32_t> in(n * 12, 0), out(n * 2, 0);
    in[ch] = 1 << 28;
    r.process(in.data(), out.data(), n);
    int firstL = -1, firstR = -1;
    double eL = 0, eR = 0;
    for (int i = 0; i < n; i++) {
      double l = out[i * 2] / 2147483648.0, rr = out[i * 2 + 1] / 2147483648.0;
      if (firstL < 0 && std::fabs(l) > 1e-4) firstL = i;
      if (firstR < 0 && std::fabs(rr) > 1e-4) firstR = i;
      eL += l * l; eR += rr * rr;
    }
    double ild = 10 * std::log10(eL / eR);
    bool ok = true;
    if (s.az > 1 && !(firstL <= firstR && ild > 0)) ok = false;
    if (s.az < -1 && !(firstR <= firstL && ild < 0)) ok = false;
    if (std::fabs(s.az) < 1 && std::fabs(ild) > 0.1) ok = false;
    if (!ok) fails++;
    std::printf("ch%2d az %5.0f el %3.0f  arrival L %3d R %3d  ILD %+6.2f dB  %s\n", ch, s.az, s.el,
                firstL, firstR, ild, ok ? "ok" : "FAIL");
  }
  // full-scale noise must not clip or produce NaN
  binaural::Renderer r;
  r.setup(binaural::kCicp19, 12, fs, 0.3f);
  std::vector<int32_t> in(4800 * 12), out(4800 * 2);
  unsigned seed = 1;
  for (auto& v : in) { seed = seed * 1664525u + 1013904223u; v = static_cast<int32_t>(seed); }
  r.process(in.data(), out.data(), 4800);
  long long peak = 0;
  for (int v : out) peak = std::max<long long>(peak, std::llabs(static_cast<long long>(v)));
  std::printf("full-scale noise peak %.3f of full scale\n", peak / 2147483648.0);
  std::printf(fails ? "FAILED %d\n" : "all ok\n", fails);
  return fails ? 1 : 0;
}
