// dynamics.h - loudness target and dynamic range control done by the plugin itself.
//
// The Fraunhofer decoder applies the Loudness target and DRC settings only to streams that
// carry MPEG-H loudness / MPEG-D DRC metadata. 360 Reality Audio files carry none, so for them
// the plugin does the same jobs on the decoded PCM:
//   - Loudness target: a level offset from the decoder's default -24 LKFS
//     (-16 LKFS = 8 dB louder, -31 LKFS = 7 dB quieter)
//   - DRC effect: a linked feed-forward compressor with one preset per effect type
//   - a peak limiter at the end, so a louder target cannot clip
//
// Header only, no allocations after setup().

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dynamics {

struct Preset {
  float thresholdDb; // compression starts here (signal level, dBFS)
  float ratio;
  float attackMs;
  float releaseMs;
  float makeupDb;    // brings the average level back up after compressing
  float presenceDb;  // dialog enhancement: lift around 2.5 kHz
};

// Index = MPEG-D DRC effect type (1..6). 0 is unused.
static const Preset kPresets[7] = {
    {0, 1, 1, 1, 0, 0},
    {-32, 3.0f, 5, 250, 11, 0},  // 1 late night: quiet loud parts, lift quiet ones
    {-30, 4.0f, 3, 150, 13, 0},  // 2 noisy environment: strongest, keeps everything audible
    {-26, 2.5f, 5, 200, 7, 0},   // 3 limited playback range (small speakers)
    {-36, 2.0f, 10, 300, 11, 0}, // 4 low playback level: lift quiet details
    {-28, 2.0f, 5, 200, 6, 3},   // 5 dialog enhancement: light compression, clearer voices
    {-24, 2.0f, 10, 300, 4, 0},  // 6 general compression
};

class Processor {
 public:
  // targetLkfs: e.g. -16; nan or 0 = decoder default (-24). effect: DRC effect type, -1..6
  // (-100 or 0 or -1 = no compression).
  void setup(int channels, int sampleRate, float targetLkfs, int effect) {
    m_ch = channels;
    m_fs = static_cast<float>(sampleRate);
    float gainDb = 0;
    if (targetLkfs < -1.0f) gainDb = targetLkfs - (-24.0f);
    m_comp = effect >= 1 && effect <= 6;
    if (m_comp) {
      const Preset& p = kPresets[effect];
      m_thr = p.thresholdDb;
      m_slope = 1.0f - 1.0f / p.ratio;
      m_att = std::exp(-1.0f / (p.attackMs * 0.001f * m_fs));
      m_rel = std::exp(-1.0f / (p.releaseMs * 0.001f * m_fs));
      gainDb += p.makeupDb;
      setPresence(p.presenceDb);
    } else {
      setPresence(0);
    }
    m_gain = std::pow(10.0f, gainDb / 20.0f);
    m_active = m_comp || std::fabs(gainDb) > 0.01f;
    reset();
  }

  bool active() const { return m_active; }

  void reset() {
    m_env = -120.0f;
    m_lim = 1.0f;
    for (int c = 0; c < kMaxCh; c++) m_x1[c] = m_x2[c] = m_y1[c] = m_y2[c] = 0;
  }

  // Interleaved int32 PCM, processed in place.
  void process(int32_t* pcm, int frames) {
    if (!m_active) return;
    const float toF = 1.0f / 2147483648.0f;
    float v[kMaxCh];
    for (int f = 0; f < frames; f++) {
      int32_t* s = pcm + static_cast<size_t>(f) * m_ch;
      float peak = 0;
      for (int c = 0; c < m_ch; c++) {
        float x = s[c] * toF;
        if (m_presence) { // peaking EQ, RBJ
          float y = m_b0 * x + m_b1 * m_x1[c] + m_b2 * m_x2[c] - m_a1 * m_y1[c] - m_a2 * m_y2[c];
          m_x2[c] = m_x1[c]; m_x1[c] = x; m_y2[c] = m_y1[c]; m_y1[c] = y;
          x = y;
        }
        v[c] = x;
        peak = std::max(peak, std::fabs(x));
      }
      float g = m_gain;
      if (m_comp) {
        // linked detector in dB with attack/release smoothing
        float lvl = 20.0f * std::log10(peak + 1e-9f);
        float coef = lvl > m_env ? m_att : m_rel;
        m_env = lvl + coef * (m_env - lvl);
        float over = m_env - m_thr;
        if (over > 0) g *= std::pow(10.0f, -over * m_slope / 20.0f);
      }
      // peak limiter at -1.5 dBFS (room for peaks between samples): instant attack, ~1 s recovery
      float out = peak * g * m_lim;
      if (out > 0.84f) m_lim = 0.84f / (peak * g + 1e-12f);
      else m_lim = std::min(1.0f, m_lim + 0.00002f);
      g *= m_lim;
      for (int c = 0; c < m_ch; c++) {
        float y = std::max(-1.0f, std::min(0.9999999f, v[c] * g));
        s[c] = static_cast<int32_t>(y * 2147483647.0f);
      }
    }
  }

 private:
  static const int kMaxCh = 24;

  void setPresence(float db) {
    m_presence = db > 0.01f;
    if (!m_presence) return;
    double A = std::pow(10.0, db / 40.0), w = 2 * 3.14159265358979 * 2500.0 / m_fs;
    double al = std::sin(w) / (2 * 0.8), cw = std::cos(w), a0 = 1 + al / A;
    m_b0 = float((1 + al * A) / a0); m_b1 = float(-2 * cw / a0); m_b2 = float((1 - al * A) / a0);
    m_a1 = float(-2 * cw / a0); m_a2 = float((1 - al / A) / a0);
  }

  int m_ch = 2;
  float m_fs = 48000;
  bool m_active = false, m_comp = false, m_presence = false;
  float m_gain = 1, m_thr = 0, m_slope = 0, m_att = 0, m_rel = 0, m_env = -120, m_lim = 1;
  float m_b0 = 1, m_b1 = 0, m_b2 = 0, m_a1 = 0, m_a2 = 0;
  float m_x1[kMaxCh], m_x2[kMaxCh], m_y1[kMaxCh], m_y2[kMaxCh];
};

} // namespace dynamics
