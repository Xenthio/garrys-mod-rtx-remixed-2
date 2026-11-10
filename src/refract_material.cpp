#include "refract_material.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace proxy::refract {
namespace {
std::string get(const Values &values, const char *key) {
  const auto it = values.find(key);
  return it == values.end() ? std::string() : it->second;
}

std::string lower(std::string value) {
  for (auto &c : value)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

bool number(const std::string &text, float &value) {
  std::istringstream stream(text);
  float parsed;
  if (!(stream >> parsed) || !std::isfinite(parsed))
    return false;
  stream >> std::ws;
  if (!stream.eof())
    return false;
  value = parsed;
  return true;
}

bool colour(std::string text, std::array<float, 3> &value) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  const auto end = text.find_last_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return false;
  text = text.substr(begin, end - begin + 1);
  const bool bytes = text.front() == '{';
  if (text.front() == '[' || bytes) {
    if (text.back() != (bytes ? '}' : ']'))
      return false;
    text = text.substr(1, text.size() - 2);
    std::istringstream stream(text);
    if (!(stream >> value[0] >> value[1] >> value[2]))
      return false;
    stream >> std::ws;
    if (!stream.eof())
      return false;
    for (auto &component : value)
      component /= bytes ? 255.f : 1.f;
  } else {
    float scalar;
    if (!number(text, scalar))
      return false;
    value.fill(scalar);
  }
  return std::all_of(value.begin(), value.end(), [](float v) { return std::isfinite(v) && v >= 0; });
}
} // namespace

bool describe(const Values &values, const Values &proxies, Description &output, std::string &reason) {
  Description result;
  auto fail = [&](const std::string &key) {
    reason = "unsupported Refract input: " + key;
    return false;
  };
  for (const char *key : {"$flowmap", "$normalmap2", "$dudvmap2", "$localrefract", "$masked",
                          "$vertexcolor", "$vertexalpha", "$bumptransform", "$normalmaptransform"}) {
    const auto value = get(values, key);
    if (!value.empty() && value != "0")
      return fail(key);
  }
  result.normalTexture = lower(get(values, "$normalmap"));
  if (result.normalTexture.empty())
    result.normalTexture = lower(get(values, "$bumpmap"));
  result.tintTexture = lower(get(values, "$refracttinttexture"));
  if (values.count("$refracttint") && !colour(get(values, "$refracttint"), result.tint))
    return fail("$refracttint");
  float flag = 0;
  if (values.count("$nocull") && !number(get(values, "$nocull"), flag))
    return fail("$nocull");
  result.noCull = flag != 0;
  for (const char *key : {"$bumpframe", "$refracttinttextureframe"}) {
    float frame = 0;
    if (values.count(key) && (!number(get(values, key), frame) || frame != 0))
      return fail(key);
  }
  for (const auto &entry : proxies) {
    // KeyValues retains an empty marker for each nested proxy block.
    if ((entry.first == "animatedtexture" || entry.first == "texturescroll") && entry.second.empty())
      continue;
    if (entry.first != "texturescroll.texturescrollvar" &&
        entry.first != "texturescroll.texturescrollrate" &&
        entry.first != "texturescroll.texturescrollangle" &&
        entry.first != "animatedtexture.animatedtexturevar" &&
        entry.first != "animatedtexture.animatedtextureframenumvar" &&
        entry.first != "animatedtexture.animatedtextureframerate")
      return fail("proxy " + entry.first);
  }
  if (proxies.count("texturescroll.texturescrollvar")) {
    if (lower(get(proxies, "texturescroll.texturescrollvar")) != "$bumptransform")
      return fail("TextureScroll target");
    float rate = 0, angle = 0;
    if (!number(get(proxies, "texturescroll.texturescrollrate"), rate) ||
        !number(get(proxies, "texturescroll.texturescrollangle"), angle))
      return fail("TextureScroll rate/angle");
    // Reduce before converting to radians, avoiding overflow for finite inputs.
    angle = std::remainder(angle, 360.f) * 0.01745329252f;
    result.scroll = {rate * std::cos(angle), rate * std::sin(angle)};
  } else if (proxies.count("texturescroll.texturescrollrate") || proxies.count("texturescroll.texturescrollangle")) {
    return fail("TextureScroll missing target");
  }
  if (proxies.count("animatedtexture.animatedtexturevar")) {
    const auto target = lower(get(proxies, "animatedtexture.animatedtexturevar"));
    if ((target != "$normalmap" && target != "$dudvmap") ||
        lower(get(proxies, "animatedtexture.animatedtextureframenumvar")) != "$bumpframe")
      return fail("AnimatedTexture target/frame variable");
    result.animatedTexture = lower(get(values, target.c_str()));
    if (result.animatedTexture.empty() || result.normalTexture.empty() ||
        !number(get(proxies, "animatedtexture.animatedtextureframerate"), result.framesPerSecond) ||
        result.framesPerSecond < 0)
      return fail("AnimatedTexture source/rate");
  } else if (proxies.count("animatedtexture.animatedtextureframenumvar") ||
             proxies.count("animatedtexture.animatedtextureframerate")) {
    return fail("AnimatedTexture missing target");
  }
  output = std::move(result);
  reason.clear();
  return true;
}

bool passiveTint(const std::vector<uint8_t> &rgba, const std::array<float, 3> &tint,
                 std::vector<float> &linear) {
  if (rgba.empty() || rgba.size() % 4 ||
      !std::all_of(tint.begin(), tint.end(), [](float v) { return std::isfinite(v) && v >= 0; }))
    return false;
  std::vector<float> result(rgba.size(), 1.f);
  for (size_t pixel = 0; pixel < rgba.size(); pixel += 4) {
    for (size_t channel = 0; channel < 3; ++channel) {
      const float encoded = rgba[pixel + channel] / 255.f;
      const float decoded = encoded <= 0.04045f ? encoded / 12.92f
                                               : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
      result[pixel + channel] = std::min(1.f, 2.f * decoded * tint[channel]);
    }
  }
  linear = std::move(result);
  return true;
}
} // namespace proxy::refract
