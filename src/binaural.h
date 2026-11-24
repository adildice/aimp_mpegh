// binaural.h - renders a 3D loudspeaker layout to headphones (two channels).
//
// Every virtual loudspeaker is placed at its real direction (azimuth and elevation) and
// filtered separately for each ear with a structural head model (Brown & Duda, 1998):
//   - interaural time difference: the sound reaches the far ear later
//   - head shadow: the far ear hears fewer high frequencies
//   - pinna echoes: short reflections from the outer ear that encode elevation
//   - shoulder echo, and a gentle high cut for sources behind the listener
// A small room (early reflections plus a short diffuse tail) moves the sound out of the
// head, the way Sony's 360 Reality Audio rendering does on headphones.
//
// Header only, no dependencies, no allocations after setup().

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace binaural {

struct Speaker {
  float az;   // degrees, 0 = front, positive = left
  float el;   // degrees, positive = up
  bool lfe;
};

// MPEG CICP 19 (7.1.4) channel order as delivered by the Fraunhofer decoder.
static const Speaker kCicp19[12] = {
    {30, 0, false},    {-30, 0, false},   {0, 0, false},    {0, 0, true},
    {90, 0, false},    {-90, 0, false},   {135, 0, false},  {-135, 0, false},
    {30, 35, false},   {-30, 35, false},  {135, 35, false}, {-135, 35, false}};

// MPEG CICP 12 (7.1 rear) fallback: L R C LFE Ls Rs Lsr Rsr.
static const Speaker kCicp12[8] = {
    {30, 0, false},   {-30, 0, false}, {0, 0, false},    {0, 0, true},
    {110, 0, false},  {-110, 0, false}, {150, 0, false}, {-150, 0, false}};

static const float kPi = 3.14159265358979f;

// Fixed-length delay line with linear interpolation.
class Delay {
 public:
  void init(int maxLen) {
    int n = 1;
    while (n < maxLen + 2) n <<= 1;
    m_buf.assign(n, 0.0f);
    m_mask = n - 1;
    m_pos = 0;
  }
  void push(float x) {
    m_pos = (m_pos + 1) & m_mask;
    m_buf[m_pos] = x;
  }
  float tap(float d) const { // d samples into the past, d >= 0
    int i = static_cast<int>(d);
    float f = d - i;
    float a = m_buf[(m_pos - i) & m_mask];
    float b = m_buf[(m_pos - i - 1) & m_mask];
    return a + (b - a) * f;
  }
  void clear() { std::fill(m_buf.begin(), m_buf.end(), 0.0f); }

 private:
  std::vector<float> m_buf;
  int m_mask = 0;
  int m_pos = 0;
};

// One ear's view of one speaker.
struct EarPath {
  float itd = 0;            // samples
  float b0 = 1, b1 = 0, a1 = 0, z = 0; // head shadow (1st-order shelf)
  float pinnaD[5] = {0}, pinnaG[5] = {0};
  float shoulderD = 0, shoulderG = 0;
  float rearA = 0, rearZ = 0; // one-pole lowpass blend for rear sources
  float rearMix = 0;
  float gain = 1;
};

class Renderer {
 public:
  // channels: decoder output layout (12 = CICP 19, 8 = CICP 12). room: 0..1.
  bool setup(const Speaker* spk, int channels, int sampleRate, float room) {
    m_n = channels;
    m_spk = spk;
    m_fs = static_cast<float>(sampleRate);
    m_room = std::max(0.0f, std::min(1.0f, room));
    const float a = 0.0875f, c = 343.0f; // head radius (m), speed of sound
    const float w0 = c / a;
    m_in.resize(m_n);
    m_path.resize(m_n * 2);
    int maxDelay = static_cast<int>(m_fs * 0.004f) + 8;
    for (int ch = 0; ch < m_n; ch++) {
      m_in[ch].init(maxDelay);
      for (int ear = 0; ear < 2; ear++) {
        EarPath& p = m_path[ch * 2 + ear];
        if (spk[ch].lfe) {
          p = EarPath();
          p.gain = 0.5f;
          continue;
        }
        float az = spk[ch].az * kPi / 180.0f;
        float el = spk[ch].el * kPi / 180.0f;
        // direction vector: x front, y left, z up
        float x = std::cos(el) * std::cos(az), y = std::cos(el) * std::sin(az), zz = std::sin(el);
        float earY = ear == 0 ? 1.0f : -1.0f;  // left ear points +y
        float cosT = std::max(-1.0f, std::min(1.0f, y * earY));
        float theta = std::acos(cosT); // 0 = straight at this ear, pi = opposite side
        (void)x;

        // ITD (Brown & Duda / Woodworth), relative to the earliest ear
        float t = theta < kPi / 2 ? -a / c * cosT : a / c * (theta - kPi / 2);
        p.itd = (t + a / c) * m_fs;

        // head shadow: H(s) = (1 + s*alpha/(2w0)) / (1 + s/(2w0)), bilinear transform
        float thetaDeg = theta * 180.0f / kPi;
        float alpha = 1.05f + 0.95f * std::cos(thetaDeg / 150.0f * kPi);
        float k = 2.0f * m_fs;
        float A = alpha / (2 * w0), B = 1.0f / (2 * w0);
        float n0 = 1 + A * k, n1 = 1 - A * k, d0 = 1 + B * k, d1 = 1 - B * k;
        p.b0 = n0 / d0; p.b1 = n1 / d0; p.a1 = d1 / d0;

        // pinna echoes: delays depend on azimuth seen from this ear and on elevation
        static const float Ak[5] = {1, 5, 5, 5, 5}, Bk[5] = {2, 4, 7, 11, 13},
                           Dk[5] = {1, 0.5f, 0.5f, 0.5f, 0.5f}, Rk[5] = {0.5f, -1, 0.5f, -0.25f, 0.25f};
        float earAz = std::atan2(x, y * earY) ; // angle around this ear, 0 = facing the ear axis... front ~ +90deg
        float latAz = kPi / 2 - earAz;          // 0 at the side, positive toward the front
        float elDeg = zz > 1 ? 90 : el * 180.0f / kPi;
        float scale = m_fs / 44100.0f;
        for (int i = 0; i < 5; i++) {
          float tau = Ak[i] * std::cos(latAz / 2) * std::sin(Dk[i] * (90.0f - elDeg) * kPi / 180.0f) + Bk[i];
          p.pinnaD[i] = std::max(1.0f, tau * scale);
          p.pinnaG[i] = Rk[i] * 0.35f;
        }

        // shoulder reflection (stronger for low sources)
        p.shoulderD = (0.0012f * (1.0f - 0.4f * std::sin(el))) * m_fs;
        p.shoulderG = 0.18f * (1.0f - std::max(0.0f, std::sin(el)));

        // sources behind: the pinna shadows highs (front/back cue)
        float back = std::max(0.0f, -std::cos(az) * std::cos(el));
        p.rearMix = 0.55f * back;
        float fc = 5000.0f;
        p.rearA = std::exp(-2.0f * kPi * fc / m_fs);
        p.gain = 1.0f;
      }
    }
    setupRoom();
    m_out = 0.22f; // speakers add up in both ears: keep headroom (level matched to Amazon's render)
    return true;
  }

  void reset() {
    for (auto& d : m_in) d.clear();
    for (auto& p : m_path) { p.z = 0; p.rearZ = 0; }
    for (auto& d : m_er) d.clear();
    for (auto& d : m_fdn) d.clear();
    std::memset(m_fdnLp, 0, sizeof(m_fdnLp));
    m_lim = 1.0f;
  }

  // in: interleaved int32 frames of m_n channels; out: interleaved int32 stereo.
  void process(const int32_t* in, int32_t* out, int frames) {
    const float toF = 1.0f / 2147483648.0f;
    for (int f = 0; f < frames; f++) {
      const int32_t* s = in + static_cast<size_t>(f) * m_n;
      float L = 0, R = 0, mono = 0, side = 0;
      for (int ch = 0; ch < m_n; ch++) {
        float x = s[ch] * toF;
        Delay& d = m_in[ch];
        d.push(x);
        if (!m_spk[ch].lfe) {
          mono += x;
          side += x * (m_spk[ch].az >= 0 ? 1.0f : -1.0f) * (m_spk[ch].az == 0 ? 0.0f : 1.0f);
        }
        for (int ear = 0; ear < 2; ear++) {
          EarPath& p = m_path[ch * 2 + ear];
          float v;
          if (m_spk[ch].lfe) {
            v = x * p.gain;
          } else {
            float base = p.itd;
            v = d.tap(base);
            for (int i = 0; i < 5; i++) v += p.pinnaG[i] * d.tap(base + p.pinnaD[i]);
            v += p.shoulderG * d.tap(base + p.shoulderD);
            // head shadow
            float y = p.b0 * v + p.z;
            p.z = p.b1 * v - p.a1 * y;
            v = y;
            // rear high cut
            p.rearZ = v + p.rearA * (p.rearZ - v);
            v = v + p.rearMix * (p.rearZ - v);
          }
          (ear == 0 ? L : R) += v;
        }
      }
      // room: early reflections and a short tail, fed from the speaker mix
      float rl = 0, rr = 0;
      if (m_room > 0) room(mono * 0.25f, side * 0.25f, rl, rr);
      L = (L + rl) * m_out;
      R = (R + rr) * m_out;
      // soft peak limiter
      float pk = std::max(std::fabs(L), std::fabs(R)) * m_lim;
      if (pk > 0.98f) m_lim = 0.98f / std::max(std::fabs(L), std::fabs(R));
      else m_lim = std::min(1.0f, m_lim + 0.00002f);
      L *= m_lim; R *= m_lim;
      out[f * 2] = toI(L);
      out[f * 2 + 1] = toI(R);
    }
  }

 private:
  static int32_t toI(float v) {
    v = std::max(-1.0f, std::min(0.9999999f, v));
    return static_cast<int32_t>(v * 2147483647.0f);
  }

  void setupRoom() {
    // early reflections (ms, gain L, gain R): a medium studio-sized room
    static const float er[8][3] = {{7.1f, 0.42f, 0.25f}, {9.4f, 0.22f, 0.40f}, {13.3f, 0.30f, 0.18f},
                                   {16.8f, 0.16f, 0.29f}, {21.2f, 0.20f, 0.14f}, {24.9f, 0.12f, 0.19f},
                                   {29.7f, 0.13f, 0.09f}, {35.3f, 0.07f, 0.11f}};
    m_er.resize(1);
    m_er[0].init(static_cast<int>(m_fs * 0.05f));
    for (int i = 0; i < 8; i++) {
      m_erD[i] = er[i][0] * 0.001f * m_fs;
      m_erGL[i] = er[i][1];
      m_erGR[i] = er[i][2];
    }
    // 4-line feedback delay network for the diffuse tail (RT60 about 0.45 s)
    static const float len[4] = {29.3f, 37.1f, 41.9f, 53.7f};
    m_fdn.resize(4);
    for (int i = 0; i < 4; i++) {
      m_fdnLen[i] = len[i] * 0.001f * m_fs;
      m_fdn[i].init(static_cast<int>(m_fdnLen[i]) + 4);
      // bigger room setting = longer tail: RT60 from 0.3 s (Light) to 0.9 s (Maximum)
      float rt60 = 0.25f + 0.65f * m_room;
      m_fdnG[i] = std::pow(10.0f, -3.0f * (len[i] * 0.001f) / rt60);
      m_fdnLp[i] = 0;
    }
    m_fdnDamp = std::exp(-2.0f * kPi * 6000.0f / m_fs);
    // Levels: Default (0.3) puts the room about 14 dB under the music, Maximum about 4 dB.
    m_erMix = 3.9f * m_room;
    m_tailMix = 2.1f * m_room;
  }

  void room(float mono, float side, float& outL, float& outR) {
    Delay& e = m_er[0];
    e.push(mono);
    float el = 0, er = 0;
    for (int i = 0; i < 8; i++) {
      float v = e.tap(m_erD[i]);
      el += v * m_erGL[i];
      er += v * m_erGR[i];
    }
    el += side * 0.1f; er -= side * 0.1f;
    // FDN with a Hadamard mix
    float o[4];
    for (int i = 0; i < 4; i++) {
      float v = m_fdn[i].tap(m_fdnLen[i]);
      m_fdnLp[i] = v + m_fdnDamp * (m_fdnLp[i] - v);
      o[i] = m_fdnLp[i] * m_fdnG[i];
    }
    float h0 = o[0] + o[1] + o[2] + o[3], h1 = o[0] - o[1] + o[2] - o[3];
    float h2 = o[0] + o[1] - o[2] - o[3], h3 = o[0] - o[1] - o[2] + o[3];
    float feed = (el + er) * 0.5f;
    m_fdn[0].push(h0 * 0.5f + feed);
    m_fdn[1].push(h1 * 0.5f + feed);
    m_fdn[2].push(h2 * 0.5f + feed);
    m_fdn[3].push(h3 * 0.5f + feed);
    outL = el * m_erMix + (o[0] + o[2]) * m_tailMix;
    outR = er * m_erMix + (o[1] + o[3]) * m_tailMix;
  }

  int m_n = 0;
  const Speaker* m_spk = nullptr;
  float m_fs = 48000, m_room = 0.3f, m_out = 0.5f, m_lim = 1.0f;
  std::vector<Delay> m_in;
  std::vector<EarPath> m_path;
  std::vector<Delay> m_er, m_fdn;
  float m_erD[8], m_erGL[8], m_erGR[8];
  float m_fdnLen[4], m_fdnG[4], m_fdnLp[4], m_fdnDamp = 0;
  float m_erMix = 0, m_tailMix = 0;
};

} // namespace binaural
