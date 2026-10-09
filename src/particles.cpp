#include "particles.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace proxy::particles {
namespace {

constexpr unsigned kRegViewProj = 8; // c8..c11: view * projection, one register per clip-space component

// Linear 0..1 to an sRGB byte, in 1/4096 steps.
const uint8_t *encodeTable() {
  static const std::vector<uint8_t> table = [] {
    std::vector<uint8_t> t(4097);
    for (int i = 0; i <= 4096; ++i) {
      const double c = i / 4096.0;
      const double s = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
      t[size_t(i)] = uint8_t(std::lround(std::clamp(s, 0.0, 1.0) * 255.0));
    }
    return t;
  }();
  return table.data();
}

} // namespace

bool expand(const Request &r, Vertex *out, Result &result, const char **failure) {
  static const char *ignored;
  const char *&why = failure ? *failure : ignored;
  if (!r.program || !r.constants || !r.elements || !r.streams || !out) {
    why = "no vertex program";
    return false;
  }
  const vsprog::Info &info = vsprog::info(*r.program);
  if (!info.supported) {
    why = info.reason;
    return false;
  }
  // Clip x, y, w = M world + t.
  const float *vp = r.constants + kRegViewProj * 4;
  const double m[3][3] = {{vp[0], vp[1], vp[2]}, {vp[4], vp[5], vp[6]}, {vp[12], vp[13], vp[14]}};
  const double t[3] = {vp[3], vp[7], vp[15]};
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  double scale = 0;
  for (const auto &row : m)
    for (double value : row)
      scale = std::max(scale, std::fabs(value));
  if (!(std::fabs(det) > 1e-12 * scale * scale * scale) || !std::isfinite(det)) {
    why = "view-projection not invertible";
    return false;
  }
  float inverse[3][3];
  {
    const double id = 1.0 / det;
    inverse[0][0] = float((m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id);
    inverse[0][1] = float((m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id);
    inverse[0][2] = float((m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id);
    inverse[1][0] = float((m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id);
    inverse[1][1] = float((m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id);
    inverse[1][2] = float((m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id);
    inverse[2][0] = float((m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id);
    inverse[2][1] = float((m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id);
    inverse[2][2] = float((m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id);
  }

  const bool hasColor = info.colorSlot >= 0, hasAlpha = info.alphaSlot >= 0;
  const bool frames = r.frameBlend && (info.written & (1u << (vsprog::kTex0 + 1))) && (info.written & (1u << (vsprog::kTex0 + 2)));
  uint32_t slots = 1u << vsprog::kPosition | 1u << vsprog::kTex0;
  if (hasColor)
    slots |= 1u << info.colorSlot;
  if (hasAlpha)
    slots |= 1u << info.alphaSlot;
  if (frames)
    slots |= 1u << (vsprog::kTex0 + 1) | 1u << (vsprog::kTex0 + 2);
  // The slots in the order vsprog::run writes them.
  thread_local std::vector<float> scratch;
  const size_t block = size_t(r.count) * 4;
  unsigned blocks = 0;
  const float *position = nullptr, *color = nullptr, *alpha = nullptr, *tex0 = nullptr, *tex1 = nullptr,
              *tex2 = nullptr;
  {
    unsigned index[vsprog::kSlots] = {};
    for (unsigned slot = 0; slot < vsprog::kSlots; ++slot)
      if (slots >> slot & 1)
        index[slot] = blocks++;
    if (scratch.size() < block * blocks)
      scratch.resize(block * blocks);
    if (!vsprog::run(*r.program, r.constants, r.elements, r.streams, r.first, r.count, slots, scratch.data())) {
      why = "vertex outside its stream";
      return false;
    }
    const float *base = scratch.data();
    position = base + block * index[vsprog::kPosition];
    tex0 = base + block * index[vsprog::kTex0];
    if (hasColor)
      color = base + block * index[info.colorSlot];
    if (hasAlpha)
      alpha = base + block * index[info.alphaSlot];
    if (frames) {
      tex1 = base + block * index[vsprog::kTex0 + 1];
      tex2 = base + block * index[vsprog::kTex0 + 2];
    }
  }

  // Colours first: the brightest decides the gain.
  thread_local std::vector<float> colors;
  if (colors.size() < block)
    colors.resize(block);
  float brightest = 0;
  bool anyColor = false, anyAlpha = false;
  // COLOR outputs are clamped by the rasterizer; texture coordinates are not.
  const bool clamped = hasColor && info.colorSlot < int(vsprog::kTex0);
  for (uint32_t v = 0; v < r.count; ++v) {
    float c[4] = {1, 1, 1, 1};
    if (color && r.useColor)
      for (int k = 0; k < 3; ++k) {
        const float value = color[size_t(v) * 4 + k];
        c[k] = !(value > 0) ? 0.0f : clamped && value > 1 ? 1.0f : value; // a NaN is 0
      }
    if (alpha && r.useAlpha) {
      const float value = alpha[size_t(v) * 4 + 3];
      c[3] = !(value > 0) ? 0.0f : value > 1 ? 1.0f : value;
    }
    const float self = 1 + r.addSelf * c[3];
    float *to = &colors[size_t(v) * 4];
    for (int k = 0; k < 3; ++k) {
      to[k] = c[k] * r.colorScale[k] * self;
      brightest = std::max(brightest, to[k]);
    }
    to[3] = std::min(c[3] * r.alphaScale, 1.0f);
    anyColor |= to[0] > 0 || to[1] > 0 || to[2] > 0;
    anyAlpha |= to[3] > 0;
  }
  result.gain = brightest > 2.004f ? 4 : brightest > 1.002f ? 2 : 1;
  result.anyColor = anyColor;
  result.anyAlpha = anyAlpha;
  result.secondFrames = 0;

  const uint8_t *encode = encodeTable();
  const float toIndex = 4096.0f / float(result.gain);
  float low[3] = {3.4e38f, 3.4e38f, 3.4e38f}, high[3] = {-3.4e38f, -3.4e38f, -3.4e38f};
  bool finite = true;
  for (uint32_t v = 0; v < r.count; ++v) {
    Vertex &o = out[v];
    const float *p = position + size_t(v) * 4;
    const float x = p[0] - float(t[0]), y = p[1] - float(t[1]), w = p[3] - float(t[2]);
    for (int k = 0; k < 3; ++k) {
      o.position[k] = inverse[k][0] * x + inverse[k][1] * y + inverse[k][2] * w;
      low[k] = std::min(low[k], o.position[k]);
      high[k] = std::max(high[k], o.position[k]);
    }
    finite &= std::isfinite(o.position[0] + o.position[1] + o.position[2]);
    const float *uv = tex0 + size_t(v) * 4;
    if (tex2 && tex2[size_t(v) * 4] > 0.5f) {
      uv = tex1 + size_t(v) * 4;
      ++result.secondFrames;
    }
    o.uv[0] = std::isfinite(uv[0]) ? uv[0] : 0;
    o.uv[1] = std::isfinite(uv[1]) ? uv[1] : 0;
    const float *c = &colors[size_t(v) * 4];
    uint32_t packed = uint32_t(c[3] * 255.0f + 0.5f) << 24;
    for (int k = 0; k < 3; ++k) {
      const int index = int(std::min(c[k] * toIndex, 4096.0f) + 0.5f);
      packed |= uint32_t(encode[index]) << (16 - 8 * k); // r, g, b
    }
    o.color = packed;
  }
  for (int k = 0; k < 3; ++k) {
    result.low[k] = low[k];
    result.high[k] = high[k];
  }
  if (!finite) {
    why = "position not finite";
    return false;
  }
  return true;
}

} // namespace proxy::particles
