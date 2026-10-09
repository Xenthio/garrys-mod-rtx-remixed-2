#include "vertex_program.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace proxy::vsprog {
namespace {

// Vertices per pass: every register component is an array of this many
// floats, an instruction a loop over it (which the compiler vectorizes).
constexpr unsigned K = 64;

enum Op : uint8_t {
  kNop = 0, kMov = 1, kAdd = 2, kSub = 3, kMad = 4, kMul = 5, kRcp = 6, kRsq = 7, kDp3 = 8, kDp4 = 9, kMin = 10,
  kMax = 11, kSlt = 12, kSge = 13, kExp = 14, kLog = 15, kLit = 16, kDst = 17, kLrp = 18, kFrc = 19, kM4x4 = 20,
  kM4x3 = 21, kM3x4 = 22, kM3x3 = 23, kM3x2 = 24, kDcl = 31, kPow = 32, kCrs = 33, kSgn = 34, kAbs = 35, kNrm = 36,
  kSinCos = 37, kMova = 46, kExpp = 78, kLogp = 79, kDef = 81,
};
// Source parameters of an instruction (-1: not an instruction run here).
int sourceCount(unsigned op, unsigned major) {
  switch (op) {
  case kNop: return 0;
  case kMov: case kRcp: case kRsq: case kExp: case kLog: case kLit: case kFrc: case kAbs: case kNrm: case kExpp:
  case kLogp: return 1;
  case kAdd: case kSub: case kMul: case kDp3: case kDp4: case kMin: case kMax: case kSlt: case kSge: case kDst:
  case kM4x4: case kM4x3: case kM3x4: case kM3x3: case kM3x2: case kPow: case kCrs: return 2;
  case kMad: case kLrp: case kSgn: return 3;
  case kSinCos: return major >= 3 ? 1 : 3;
  default: return -1;
  }
}

// D3DSHADER_PARAM_REGISTER_TYPE
enum : unsigned { kTemp = 0, kInput = 1, kConst = 2, kAddr = 3, kRastOut = 4, kAttrOut = 5, kOutput = 6 };
unsigned registerType(uint32_t token) { return ((token >> 28) & 7) | ((token >> 8) & 0x18); }

// D3DDECLUSAGE
enum : unsigned { kUsagePosition = 0, kUsageBlendWeight = 1, kUsageBlendIndices = 2, kUsageNormal = 3,
                  kUsagePointSize = 4, kUsageTexcoord = 5, kUsageTangent = 6, kUsageBinormal = 7, kUsageColor = 10,
                  kUsageFog = 11 };

// What a register component was computed from (compile-time analysis).
// kFromShape: the other inputs that describe the surface itself (normals,
// tangents, and the flex deltas Source's model shaders add to every position:
// POSITION1, bound to a buffer of zeros on meshes without flexes).
enum : uint8_t { kFromPosition = 1, kFromOther = 2, kFromColor = 4, kFromBones = 8, kThroughPower = 16, kFromShape = 32 };

struct Source {
  uint16_t reg = 0; // dense register
  uint8_t swizzle[4] = {0, 1, 2, 3};
  uint8_t modifier = 0; // 1 negate, 2 abs, 3 -abs
};
struct Instruction {
  uint8_t op = 0, mask = 0, sources = 0;
  bool saturate = false;
  uint16_t dst = 0;
  Source src[3];
  uint16_t rows[4] = {}; // matrix rows (dense registers) of m4x4 .. m3x2
};
struct Constant {
  uint16_t reg, dense;
  bool defined;
  float value[4];
};
struct BoundInput {
  uint16_t dense;
  uint8_t usage, usageIndex;
};

} // namespace

struct Program {
  Info info;
  std::vector<uint32_t> tokens;
  std::vector<Instruction> code;
  std::vector<Constant> constants;
  std::vector<BoundInput> inputs;
  uint16_t outputs[kSlots] = {}; // dense register per slot (0: not written; dense 0 is never an output)
  uint16_t registers = 0;        // dense registers in all
};

namespace {

uint64_t fnv1a(const void *data, size_t size) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i)
    h = (h ^ static_cast<const uint8_t *>(data)[i]) * 1099511628211ull;
  return h;
}

struct Compiler {
  Program &p;
  unsigned major = 2;
  // Dense register numbers: 0 is a scratch register that absorbs writes to
  // outputs nobody reads (vs_3_0 outputs of other usages).
  uint16_t temps[32] = {}, ins[16] = {}, consts[256] = {}, outs[16] = {}, slots[kSlots] = {};
  int8_t outputSlot[16]; // vs_3_0: o# -> Slot (-1: not declared, -2: another usage)
  std::vector<std::array<uint8_t, 4>> taint;
  std::vector<uint8_t> writes; // components written, per dense register
  uint16_t next = 1;
  const char *failure = nullptr;

  explicit Compiler(Program &program) : p(program) {
    std::fill(std::begin(outputSlot), std::end(outputSlot), int8_t(-1));
    taint.push_back({});
    writes.push_back(0);
  }
  void fail(const char *why) {
    if (!failure)
      failure = why;
  }
  uint16_t fresh() {
    taint.push_back({});
    writes.push_back(0);
    return next++;
  }
  uint16_t slotRegister(unsigned slot) {
    if (!slots[slot]) {
      slots[slot] = fresh();
      p.outputs[slot] = slots[slot];
      p.info.written |= 1u << slot;
    }
    return slots[slot];
  }
  uint16_t constant(unsigned n) {
    if (n >= 256) {
      fail("constant register out of range");
      return 0;
    }
    if (!consts[n]) {
      consts[n] = fresh();
      p.constants.push_back({uint16_t(n), consts[n], false, {}});
    }
    return consts[n];
  }
  // The dense register of a parameter token (0 and a failure when it has none).
  uint16_t resolve(uint32_t token, bool destination) {
    const unsigned type = registerType(token), n = token & 0x7ff;
    switch (type) {
    case kTemp:
      if (n >= 32)
        break;
      if (!temps[n])
        temps[n] = fresh();
      return temps[n];
    case kInput:
      if (n >= 16 || destination)
        break;
      if (!ins[n]) { // read without a dcl (vs_1_1 allows none): zero
        ins[n] = fresh();
      }
      return ins[n];
    case kConst:
      if (destination)
        break;
      return constant(n);
    case kRastOut:
      if (!destination || n > 2)
        break;
      return slotRegister(n == 0 ? kPosition : n == 1 ? kFog : kPointSize);
    case kAttrOut:
      if (!destination || n > 1)
        break;
      return slotRegister(kColor0 + n);
    case kOutput:
      if (n >= 16)
        break;
      if (major < 3) {
        if (!destination || n > 7)
          break;
        return slotRegister(kTex0 + n);
      }
      if (outputSlot[n] >= 0)
        return slotRegister(unsigned(outputSlot[n]));
      if (!outs[n])
        outs[n] = fresh();
      return outs[n];
    default: break;
    }
    fail(type == kAddr ? "address register (skinning)" : "register type");
    return 0;
  }
};

} // namespace

std::shared_ptr<const Program> compile(const uint32_t *tokens, size_t maxTokens) {
  if (!tokens || maxTokens < 2 || (tokens[0] >> 16) != 0xfffe)
    return nullptr;
  auto program = std::make_shared<Program>();
  Program &p = *program;
  Compiler c(p);
  c.major = (tokens[0] >> 8) & 0xff;
  p.info.version = uint16_t(tokens[0] & 0xffff);
  if (c.major < 1 || c.major > 3)
    c.fail("shader version");

  size_t at = 1;
  bool ended = false;
  while (at < maxTokens) {
    const uint32_t token = tokens[at];
    const unsigned op = token & 0xffff;
    if (op == 0xffff) {
      ended = true;
      ++at;
      break;
    }
    if (op == 0xfffe) { // comment
      at += 1 + ((token >> 16) & 0x7fff);
      continue;
    }
    const size_t first = at + 1; // the instruction's parameters
    size_t length;
    if (c.major >= 2) {
      length = (token >> 24) & 0xf;
    } else if (op == kDcl) {
      length = 2;
    } else if (op == kDef) {
      length = 5;
    } else {
      const int sources = sourceCount(op, c.major);
      if (sources < 0) {
        c.fail("instruction");
        break; // its length is unknown
      }
      length = op == kNop ? 0 : 1 + size_t(sources);
    }
    at = first + length;
    if (at > maxTokens)
      return nullptr;
    if (token & 0x10000000)
      c.fail("predicated instruction");
    if (op == kNop)
      continue;
    if (op == kDcl) {
      if (length < 2)
        return nullptr;
      const uint32_t usage = tokens[first], reg = tokens[first + 1];
      const unsigned type = registerType(reg), n = reg & 0x7ff, what = usage & 0x1f, index = (usage >> 16) & 0xf;
      if (type == kInput && n < 16) {
        if (!c.ins[n])
          c.ins[n] = c.fresh();
        p.inputs.push_back({c.ins[n], uint8_t(what), uint8_t(index)});
        p.info.inputs.push_back({uint8_t(n), uint8_t(what), uint8_t(index)});
        const uint8_t from =
            what == kUsagePosition && index == 0                        ? kFromPosition
            : what == kUsageColor                                       ? kFromColor
            : what == kUsageBlendWeight || what == kUsageBlendIndices ? kFromBones
            : what == kUsagePosition || what == kUsageNormal || what == kUsageTangent || what == kUsageBinormal
                ? kFromShape
                : kFromOther;
        c.taint[c.ins[n]] = {from, from, from, from};
      } else if (type == kOutput && c.major >= 3 && n < 16) {
        int slot = -2;
        if (what == kUsagePosition && index == 0)
          slot = kPosition;
        else if (what == kUsageColor && index < 2)
          slot = int(kColor0 + index);
        else if (what == kUsageTexcoord && index < 8)
          slot = int(kTex0 + index);
        else if (what == kUsageFog)
          slot = kFog;
        else if (what == kUsagePointSize)
          slot = kPointSize;
        c.outputSlot[n] = int8_t(slot);
      }
      continue;
    }
    if (op == kDef) {
      if (length < 5)
        return nullptr;
      const unsigned n = tokens[first] & 0x7ff;
      const uint16_t dense = c.constant(n);
      for (auto &constant : p.constants)
        if (constant.dense == dense) {
          constant.defined = true;
          std::memcpy(constant.value, &tokens[first + 1], 16);
        }
      continue;
    }
    const int sources = sourceCount(op, c.major);
    if (sources < 0 || op == kMova) {
      c.fail(op == kMova ? "address register (skinning)" : "instruction (flow control or unknown)");
      continue;
    }
    Instruction ins;
    ins.op = uint8_t(op);
    ins.sources = uint8_t(sources);
    size_t param = first;
    if (param >= at) {
      c.fail("truncated instruction");
      continue;
    }
    const uint32_t dst = tokens[param++];
    ins.mask = uint8_t((dst >> 16) & 0xf);
    ins.saturate = ((dst >> 20) & 1) != 0;
    if (dst & 0x2000)
      c.fail("relative addressing");
    ins.dst = c.resolve(dst, true);
    for (int s = 0; s < sources; ++s) {
      if (param >= at) {
        c.fail("truncated instruction");
        break;
      }
      const uint32_t src = tokens[param++];
      if (src & 0x2000) { // relative addressing: c[a0 + n] (vs_2_0 adds a token)
        c.fail("relative addressing (skinning)");
        if (c.major >= 2)
          ++param;
      }
      Source &out = ins.src[s];
      out.reg = c.resolve(src, false);
      for (int k = 0; k < 4; ++k)
        out.swizzle[k] = uint8_t((src >> (16 + 2 * k)) & 3);
      switch ((src >> 24) & 0xf) {
      case 0: break;
      case 1: out.modifier = 1; break;
      case 11: out.modifier = 2; break;
      case 12: out.modifier = 3; break;
      default: c.fail("source modifier");
      }
    }
    // Matrix products read the registers after their second parameter.
    const unsigned rows = op == kM4x4 || op == kM3x4 ? 4 : op == kM4x3 || op == kM3x3 ? 3 : op == kM3x2 ? 2 : 0;
    if (rows && sources == 2 && param - 1 < at) {
      const uint32_t src = tokens[param - 1];
      for (unsigned r = 0; r < rows; ++r)
        ins.rows[r] = c.resolve((src & ~0x7ffu) | ((src & 0x7ff) + r), false);
    }
    if (c.failure)
      continue;

    // What the result was computed from, per component.
    auto from = [&](int s, int component) { return c.taint[ins.src[s].reg][ins.src[s].swizzle[component]]; };
    uint8_t all[3] = {};
    for (int s = 0; s < sources; ++s)
      for (int k = 0; k < 4; ++k)
        all[s] |= from(s, k);
    std::array<uint8_t, 4> result = {};
    switch (op) {
    case kMov: case kAdd: case kSub: case kMad: case kMul: case kMin: case kMax: case kSlt: case kSge: case kLrp:
    case kFrc: case kAbs: case kSgn:
      for (int k = 0; k < 4; ++k)
        for (int s = 0; s < (op == kSgn ? 1 : sources); ++s)
          result[k] |= from(s, k);
      break;
    case kExp: case kLog: case kPow: case kExpp: case kLogp: {
      uint8_t value = kThroughPower;
      for (int s = 0; s < sources; ++s)
        value |= from(s, 0);
      result = {value, value, value, value};
      break;
    }
    case kM4x4: case kM4x3: case kM3x4: case kM3x3: case kM3x2: {
      uint8_t value = all[0];
      for (unsigned r = 0; r < rows; ++r)
        for (int k = 0; k < 4; ++k)
          value |= c.taint[ins.rows[r]][k];
      result = {value, value, value, value};
      break;
    }
    default: { // scalar and whole-vector results
      const uint8_t value = uint8_t(all[0] | all[1] | all[2]);
      result = {value, value, value, value};
    }
    }
    for (int k = 0; k < 4; ++k)
      if (ins.mask >> k & 1)
        c.taint[ins.dst][k] = result[k];
    c.writes[ins.dst] |= ins.mask;
    p.code.push_back(ins);
  }
  if (!ended)
    return nullptr;

  p.tokens.assign(tokens, tokens + at);
  p.registers = c.next;
  Info &info = p.info;
  info.tokens = uint32_t(at);
  info.hash = fnv1a(tokens, at * sizeof(uint32_t));
  info.instructions = uint32_t(p.code.size());
  for (const auto &constant : p.constants)
    if (!constant.defined)
      info.constants.push_back(constant.reg);
  std::sort(info.constants.begin(), info.constants.end());
  if (!c.failure && !(info.written & (1u << kPosition)))
    c.fail("no position output");
  info.supported = !c.failure;
  info.reason = c.failure ? c.failure : "";
  if (!info.supported) {
    // Skinning shaders are the ones this cannot run; say so.
    for (const auto &input : info.inputs)
      info.skinned |= input.usage == kUsageBlendWeight || input.usage == kUsageBlendIndices;
    return program;
  }
  auto slotTaint = [&](unsigned slot) {
    uint8_t value = 0;
    if (p.outputs[slot])
      for (uint8_t component : c.taint[p.outputs[slot]])
        value |= component;
    return value;
  };
  const uint8_t position = slotTaint(kPosition);
  info.expands = (position & (kFromOther | kFromColor)) != 0;
  info.skinned = (position & kFromBones) != 0;
  // The vertex colour (see Info::colorSlot).
  constexpr uint8_t kFromGeometry = kFromOther | kFromPosition | kFromBones | kFromShape;
  auto colorTaint = [&](unsigned slot) { // of the written part of a slot's rgb; 0xff: none written
    const uint16_t reg = p.outputs[slot];
    if (!reg || !(c.writes[reg] & 7))
      return uint8_t(0xff);
    uint8_t value = 0;
    for (int k = 0; k < 3; ++k)
      if (c.writes[reg] >> k & 1)
        value |= c.taint[reg][k];
    return uint8_t(value & 0x7f);
  };
  for (unsigned slot = kTex0; slot <= kTex7 && info.colorSlot < 0; ++slot) {
    const uint8_t value = colorTaint(slot);
    if (value != 0xff && (value & kFromColor) && !(value & kFromGeometry))
      info.colorSlot = int(slot);
  }
  if (info.colorSlot < 0)
    if (const uint8_t value = colorTaint(kColor0); value != 0xff && ((value & kFromColor) || !(value & kFromGeometry)))
      info.colorSlot = kColor0;
  if (info.colorSlot >= 0) {
    info.colorLinear = (colorTaint(unsigned(info.colorSlot)) & kThroughPower) != 0;
    auto alphaFrom = [&](unsigned slot) {
      const uint16_t reg = p.outputs[slot];
      return reg && (c.writes[reg] & 8) && ((c.taint[reg][3] & kFromColor) || !(c.taint[reg][3] & kFromGeometry));
    };
    if (c.writes[p.outputs[info.colorSlot]] & 8)
      info.alphaSlot = info.colorSlot;
    else if (alphaFrom(kColor0))
      info.alphaSlot = kColor0;
  }
  return program;
}

const Info &info(const Program &program) { return program.info; }
const std::vector<uint32_t> &tokens(const Program &program) { return program.tokens; }

namespace {

inline float *component(float *registers, unsigned reg, unsigned k) { return registers + (size_t(reg) * 4 + k) * K; }

// IEEE half to float.
inline float half(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
  uint32_t bits;
  if (exponent == 0) {
    if (!mantissa) {
      bits = sign;
    } else {
      int shift = 0;
      uint32_t m = mantissa;
      while (!(m & 0x400)) {
        m <<= 1;
        ++shift;
      }
      bits = sign | uint32_t(127 - 15 - shift + 1) << 23 | (m & 0x3ff) << 13;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | mantissa << 13;
  } else {
    bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
  }
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

// Bytes an element of a D3DDECLTYPE occupies (0: not read here).
unsigned elementBytes(unsigned type) {
  static const uint8_t bytes[17] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 0, 0, 4, 8};
  return type < 17 ? bytes[type] : 0;
}

// Loads `n` vertices of one element into a register (x, y, z, w arrays).
void load(const uint8_t *data, uint32_t stride, unsigned type, unsigned n, float *x, float *y, float *z, float *w) {
  float *const out[4] = {x, y, z, w};
  auto floats = [&](unsigned count) {
    for (unsigned i = 0; i < n; ++i) {
      float v[4] = {0, 0, 0, 1};
      std::memcpy(v, data + size_t(i) * stride, count * 4);
      for (int k = 0; k < 4; ++k)
        out[k][i] = v[k];
    }
  };
  auto shorts = [&](unsigned count, float scale, bool isUnsigned) {
    for (unsigned i = 0; i < n; ++i) {
      float v[4] = {0, 0, 0, 1};
      for (unsigned k = 0; k < count; ++k) {
        uint16_t raw;
        std::memcpy(&raw, data + size_t(i) * stride + k * 2, 2);
        v[k] = (isUnsigned ? float(raw) : float(int16_t(raw))) * scale;
      }
      for (int k = 0; k < 4; ++k)
        out[k][i] = v[k];
    }
  };
  switch (type) {
  case 0: floats(1); break;
  case 1: floats(2); break;
  case 2: floats(3); break;
  case 3: floats(4); break;
  case 4: // D3DCOLOR: bytes B, G, R, A
    for (unsigned i = 0; i < n; ++i) {
      const uint8_t *c = data + size_t(i) * stride;
      x[i] = c[2] * (1.0f / 255), y[i] = c[1] * (1.0f / 255), z[i] = c[0] * (1.0f / 255), w[i] = c[3] * (1.0f / 255);
    }
    break;
  case 5: // UBYTE4
  case 8: // UBYTE4N
    for (unsigned i = 0; i < n; ++i) {
      const uint8_t *c = data + size_t(i) * stride;
      const float scale = type == 8 ? 1.0f / 255 : 1.0f;
      x[i] = c[0] * scale, y[i] = c[1] * scale, z[i] = c[2] * scale, w[i] = c[3] * scale;
    }
    break;
  case 6: shorts(2, 1, false); break;
  case 7: shorts(4, 1, false); break;
  case 9: shorts(2, 1.0f / 32767, false); break;
  case 10: shorts(4, 1.0f / 32767, false); break;
  case 11: shorts(2, 1.0f / 65535, true); break;
  case 12: shorts(4, 1.0f / 65535, true); break;
  case 15:
  case 16:
    for (unsigned i = 0; i < n; ++i) {
      float v[4] = {0, 0, 0, 1};
      for (unsigned k = 0; k < (type == 15 ? 2u : 4u); ++k) {
        uint16_t raw;
        std::memcpy(&raw, data + size_t(i) * stride + k * 2, 2);
        v[k] = half(raw);
      }
      for (int k = 0; k < 4; ++k)
        out[k][i] = v[k];
    }
    break;
  default: break;
  }
}

// Results of rcp, rsq and log are finite, as DXVK computes them for shader
// models 1 and 2 (and as the hardware of their time did): x * rcp(0) is 0
// for x = 0, where an infinity would make it a NaN.
inline float reciprocal(float x) { return x == 0 ? FLT_MAX : std::clamp(1.0f / x, -FLT_MAX, FLT_MAX); }
inline float reciprocalRoot(float x) {
  x = std::fabs(x);
  return x == 0 ? FLT_MAX : std::min(1.0f / std::sqrt(x), FLT_MAX);
}
inline float logarithm(float x) {
  x = std::fabs(x);
  return x == 0 ? -FLT_MAX : std::log2(x);
}

void execute(const Program &p, float *registers) {
  alignas(32) float result[4][K], modified[3][4][K];
  for (const Instruction &ins : p.code) {
    // Component `k` of source `s`, after its swizzle and modifier.
    auto arg = [&](unsigned s, unsigned k) -> const float * {
      const Source &src = ins.src[s];
      const float *value = component(registers, src.reg, src.swizzle[k]);
      if (!src.modifier)
        return value;
      float *out = modified[s][k];
      if (src.modifier == 1)
        for (unsigned i = 0; i < K; ++i)
          out[i] = -value[i];
      else if (src.modifier == 2)
        for (unsigned i = 0; i < K; ++i)
          out[i] = std::fabs(value[i]);
      else
        for (unsigned i = 0; i < K; ++i)
          out[i] = -std::fabs(value[i]);
      return out;
    };
    bool scalar = false; // result[0] is every component's value
    switch (ins.op) {
    case kMov:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1)
          std::memcpy(result[k], arg(0, k), sizeof(float) * K);
      break;
    case kAdd:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k), *b = arg(1, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] + b[i];
        }
      break;
    case kSub:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k), *b = arg(1, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] - b[i];
        }
      break;
    case kMul:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k), *b = arg(1, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] * b[i];
        }
      break;
    case kMad:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k), *b = arg(1, k), *c = arg(2, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] * b[i] + c[i];
        }
      break;
    case kLrp: // s0 * (s1 - s2) + s2
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k), *b = arg(1, k), *c = arg(2, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] * (b[i] - c[i]) + c[i];
        }
      break;
    case kMin:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k), *b = arg(1, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] < b[i] ? a[i] : b[i];
        }
      break;
    case kMax:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k), *b = arg(1, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] >= b[i] ? a[i] : b[i];
        }
      break;
    case kSlt:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k), *b = arg(1, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] < b[i] ? 1.0f : 0.0f;
        }
      break;
    case kSge:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k), *b = arg(1, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] >= b[i] ? 1.0f : 0.0f;
        }
      break;
    case kFrc:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] - std::floor(a[i]);
        }
      break;
    case kAbs:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = std::fabs(a[i]);
        }
      break;
    case kSgn:
      for (unsigned k = 0; k < 4; ++k)
        if (ins.mask >> k & 1) {
          const float *a = arg(0, k);
          for (unsigned i = 0; i < K; ++i)
            result[k][i] = a[i] > 0 ? 1.0f : a[i] < 0 ? -1.0f : 0.0f;
        }
      break;
    case kDp3:
    case kDp4: {
      const unsigned n = ins.op == kDp3 ? 3 : 4;
      const float *a = arg(0, 0), *b = arg(1, 0);
      for (unsigned i = 0; i < K; ++i)
        result[0][i] = a[i] * b[i];
      for (unsigned k = 1; k < n; ++k) {
        a = arg(0, k), b = arg(1, k);
        for (unsigned i = 0; i < K; ++i)
          result[0][i] += a[i] * b[i];
      }
      scalar = true;
      break;
    }
    case kRcp: {
      const float *a = arg(0, 0);
      for (unsigned i = 0; i < K; ++i)
        result[0][i] = reciprocal(a[i]);
      scalar = true;
      break;
    }
    case kRsq: {
      const float *a = arg(0, 0);
      for (unsigned i = 0; i < K; ++i)
        result[0][i] = reciprocalRoot(a[i]);
      scalar = true;
      break;
    }
    case kExp:
    case kExpp: {
      const float *a = arg(0, 0);
      for (unsigned i = 0; i < K; ++i)
        result[0][i] = std::exp2(a[i]);
      scalar = true;
      break;
    }
    case kLog:
    case kLogp: {
      const float *a = arg(0, 0);
      for (unsigned i = 0; i < K; ++i)
        result[0][i] = logarithm(a[i]);
      scalar = true;
      break;
    }
    case kPow: {
      const float *a = arg(0, 0), *b = arg(1, 0);
      for (unsigned i = 0; i < K; ++i)
        result[0][i] = std::pow(std::fabs(a[i]), b[i]);
      scalar = true;
      break;
    }
    case kNrm: {
      const float *x = arg(0, 0), *y = arg(0, 1), *z = arg(0, 2), *w = arg(0, 3);
      for (unsigned i = 0; i < K; ++i) {
        const float scale = reciprocalRoot(x[i] * x[i] + y[i] * y[i] + z[i] * z[i]);
        const float vx = x[i] * scale, vy = y[i] * scale, vz = z[i] * scale, vw = w[i] * scale;
        result[0][i] = vx, result[1][i] = vy, result[2][i] = vz, result[3][i] = vw;
      }
      break;
    }
    case kCrs: {
      const float *ax = arg(0, 0), *ay = arg(0, 1), *az = arg(0, 2);
      const float *bx = arg(1, 0), *by = arg(1, 1), *bz = arg(1, 2);
      for (unsigned i = 0; i < K; ++i) {
        const float vx = ay[i] * bz[i] - az[i] * by[i], vy = az[i] * bx[i] - ax[i] * bz[i],
                    vz = ax[i] * by[i] - ay[i] * bx[i];
        result[0][i] = vx, result[1][i] = vy, result[2][i] = vz, result[3][i] = 0;
      }
      break;
    }
    case kSinCos: {
      const float *a = arg(0, 0);
      for (unsigned i = 0; i < K; ++i) {
        result[0][i] = std::cos(a[i]);
        result[1][i] = std::sin(a[i]);
        result[2][i] = result[3][i] = 0;
      }
      break;
    }
    case kLit: {
      const float *x = arg(0, 0), *y = arg(0, 1), *w = arg(0, 3);
      for (unsigned i = 0; i < K; ++i) {
        const float power = std::clamp(w[i], -127.9961f, 127.9961f);
        result[0][i] = 1;
        result[1][i] = x[i] > 0 ? x[i] : 0;
        result[2][i] = x[i] > 0 && y[i] > 0 ? std::pow(y[i], power) : 0;
        result[3][i] = 1;
      }
      break;
    }
    case kDst: {
      const float *ay = arg(0, 1), *az = arg(0, 2), *by = arg(1, 1), *bw = arg(1, 3);
      for (unsigned i = 0; i < K; ++i) {
        result[0][i] = 1;
        result[1][i] = ay[i] * by[i];
        result[2][i] = az[i];
        result[3][i] = bw[i];
      }
      break;
    }
    case kM4x4: case kM4x3: case kM3x4: case kM3x3: case kM3x2: {
      const unsigned n = ins.op == kM4x4 || ins.op == kM4x3 ? 4 : 3;
      const unsigned rows = ins.op == kM4x4 || ins.op == kM3x4 ? 4 : ins.op == kM3x2 ? 2 : 3;
      const float *a[4] = {arg(0, 0), arg(0, 1), arg(0, 2), n == 4 ? arg(0, 3) : nullptr};
      for (unsigned r = 0; r < rows; ++r) {
        const float *row = component(registers, ins.rows[r], 0);
        for (unsigned i = 0; i < K; ++i)
          result[r][i] = a[0][i] * row[i];
        for (unsigned k = 1; k < n; ++k) {
          row = component(registers, ins.rows[r], k);
          for (unsigned i = 0; i < K; ++i)
            result[r][i] += a[k][i] * row[i];
        }
      }
      break;
    }
    default: break;
    }
    for (unsigned k = 0; k < 4; ++k)
      if (ins.mask >> k & 1) {
        float *out = component(registers, ins.dst, k);
        const float *value = result[scalar ? 0 : k];
        if (ins.saturate)
          for (unsigned i = 0; i < K; ++i)
            out[i] = value[i] < 0 ? 0.0f : value[i] > 1 ? 1.0f : value[i];
        else
          std::memcpy(out, value, sizeof(float) * K);
      }
  }
}

} // namespace

bool run(const Program &p, const float *constants, const Element *elements, const Stream streams[4], uint32_t first,
         uint32_t count, uint32_t slots, float *out) {
  if (!p.info.supported || !constants || !elements || !out)
    return false;
  thread_local std::vector<float> file;
  const size_t wanted = size_t(p.registers) * 4 * K;
  if (file.size() < wanted)
    file.resize(wanted);
  float *registers = file.data();
  std::fill_n(registers, wanted, 0.0f);
  for (const Constant &constant : p.constants) {
    const float *value = constant.defined ? constant.value : constants + size_t(constant.reg) * 4;
    for (unsigned k = 0; k < 4; ++k)
      std::fill_n(component(registers, constant.dense, k), K, value[k]);
  }
  // The declaration's element of each input (w 1 where there is none).
  struct Bound {
    const uint8_t *data = nullptr; // the element in vertex `first`
    uint32_t stride = 0;
    uint8_t type = 0;
    uint16_t dense = 0;
  } bound[16];
  unsigned bindings = 0;
  for (const BoundInput &input : p.inputs) {
    const Element *element = nullptr;
    for (const Element *e = elements; e->stream != 0xff; ++e)
      if (e->usage == input.usage && e->usageIndex == input.usageIndex) {
        element = e;
        break;
      }
    std::fill_n(component(registers, input.dense, 3), K, 1.0f);
    if (!element || element->stream >= 4 || bindings >= 16)
      continue;
    const Stream &stream = streams[element->stream];
    const unsigned bytes = elementBytes(element->type);
    if (!stream.data || !bytes)
      continue;
    // Every vertex of the range lies inside the stream.
    const uint64_t last = uint64_t(first) + (count ? count - 1 : 0);
    if (last * stream.stride + element->offset + bytes > stream.size)
      return false;
    bound[bindings++] = {stream.data + size_t(first) * stream.stride + element->offset, stream.stride, element->type,
                         input.dense};
  }
  // Unwritten output components: 0, w 1.
  for (unsigned slot = 0; slot < kSlots; ++slot)
    if (p.outputs[slot])
      std::fill_n(component(registers, p.outputs[slot], 3), K, 1.0f);

  for (uint32_t start = 0; start < count; start += K) {
    const unsigned n = std::min<uint32_t>(K, count - start);
    for (unsigned b = 0; b < bindings; ++b) {
      float *x = component(registers, bound[b].dense, 0);
      load(bound[b].data + size_t(start) * bound[b].stride, bound[b].stride, bound[b].type, n, x, x + K, x + 2 * K,
           x + 3 * K);
    }
    execute(p, registers);
    float *slotOut = out;
    for (unsigned slot = 0; slot < kSlots; ++slot) {
      if (!(slots >> slot & 1))
        continue;
      float *to = slotOut + size_t(start) * 4;
      if (p.outputs[slot]) {
        const float *x = component(registers, p.outputs[slot], 0);
        for (unsigned i = 0; i < n; ++i) {
          to[i * 4] = x[i];
          to[i * 4 + 1] = x[K + i];
          to[i * 4 + 2] = x[2 * K + i];
          to[i * 4 + 3] = x[3 * K + i];
        }
      } else {
        for (unsigned i = 0; i < n; ++i) {
          to[i * 4] = to[i * 4 + 1] = to[i * 4 + 2] = 0;
          to[i * 4 + 3] = 1;
        }
      }
      slotOut += size_t(count) * 4;
    }
  }
  return true;
}

} // namespace proxy::vsprog
