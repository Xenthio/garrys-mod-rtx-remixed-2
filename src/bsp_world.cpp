#include "bsp_world.h"

#include "source_fs.h"
#include "texture_categories.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <windows.h>

#include <remix/remix_native_ext.h>

#include <algorithm>
#include <mutex>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace proxy::bsp {
namespace {

// --- BSP (version 21, Left 4 Dead 2 lump order) -----------------------------
enum Lump {
  kEntities = 0,
  kPlanes = 1,
  kTexData = 2,
  kVertexes = 3,
  kVisibility = 4,
  kNodes = 5,
  kTexInfo = 6,
  kFaces = 7,
  kLeafs = 10,
  kLeafFaces = 16,
  kEdges = 12,
  kSurfEdges = 13,
  kModels = 14,
  kDispInfo = 26,
  kDispVerts = 33,
  kGameLump = 35,
  kPakfile = 40,
  kTexDataStringData = 43,
  kTexDataStringTable = 44,
  kOverlays = 45,
  kWorldLights = 15,
  kWorldLightsHdr = 54,
};
// texinfo_t::flags
constexpr int32_t kSurfSky2D = 0x2, kSurfSky = 0x4, kSurfTrans = 0x10, kSurfNoDraw = 0x80, kSurfHint = 0x100,
                  kSurfSkip = 0x200;

#pragma pack(push, 1)
struct Plane {
  float normal[3], dist;
  int32_t type;
};
struct Face {
  uint16_t plane;
  uint8_t side, onNode;
  int32_t firstEdge;
  int16_t numEdges, texInfo, dispInfo, fogVolume;
  uint8_t styles[4];
  int32_t lightOffset;
  float area;
  int32_t lightmapMins[2], lightmapSize[2], origFace;
  uint16_t numPrims, firstPrim;
  uint32_t smoothingGroups;
};
struct TexInfo {
  float textureVecs[2][4], lightmapVecs[2][4];
  int32_t flags, texData;
};
struct TexData {
  float reflectivity[3];
  int32_t nameId, width, height, viewWidth, viewHeight;
};
struct Node {
  int32_t plane, children[2];
  int16_t mins[3], maxs[3];
  uint16_t firstFace, numFaces;
  int16_t area, padding;
};
struct Leaf {
  int32_t contents;
  int16_t cluster, areaFlags;
  int16_t mins[3], maxs[3];
  uint16_t firstLeafFace, numLeafFaces, firstLeafBrush, numLeafBrushes;
  int16_t waterData, padding;
};
struct Model {
  float mins[3], maxs[3], origin[3];
  int32_t headNode, firstFace, numFaces;
};
struct DispInfo {
  float start[3];
  int32_t firstVert, firstTri, power, minTess;
  float smoothingAngle;
  int32_t contents;
  uint16_t face;
  uint8_t rest[176 - 38];
};
struct DispVert {
  float vector[3], dist, alpha;
};
struct Overlay {
  int32_t id;
  int16_t texInfo;
  uint16_t faceCountAndRenderOrder;
  int32_t faces[64];
  float u[2], v[2];
  float uvPoints[4][3]; // corners in the (U, V) basis; z holds U's components
  float origin[3], normal[3];
};
#pragma pack(pop)
static_assert(sizeof(Face) == 56 && sizeof(TexInfo) == 72 && sizeof(TexData) == 32 && sizeof(Node) == 32 &&
                  sizeof(Leaf) == 32 && sizeof(Model) == 48 && sizeof(DispInfo) == 176 && sizeof(DispVert) == 20 && sizeof(Overlay) == 352,
              "BSP layouts");

class Bsp {
public:
  std::vector<uint8_t> data;
  bool open(const std::wstring &path) {
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f)
      return false;
    _fseeki64(f, 0, SEEK_END);
    data.resize(size_t(_ftelli64(f)));
    _fseeki64(f, 0, SEEK_SET);
    const bool ok = fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    int32_t version = 0;
    if (!ok || data.size() < 1036 || std::memcmp(data.data(), "VBSP", 4) != 0)
      return false;
    std::memcpy(&version, data.data() + 4, 4);
    if (version < 19 || version > 21)
      return false;
    // lump_t is {fileofs, filelen, version, fourCC} up to v20 (GMod maps).
    // Left 4 Dead 2's v21 puts the version first; other v21 maps (TF2, CS:GO
    // era) keep the standard order. As NikNaks: a first field of at most 8
    // in lump 0 (planes) is a version, not a file offset.
    int32_t first;
    std::memcpy(&first, data.data() + 8, 4);
    versionFirst = version == 21 && first >= 0 && first <= 8;
    return true;
  }
  bool versionFirst = false;
  template <typename T> std::pair<const T *, size_t> lump(int index) const {
    int32_t header[4];
    std::memcpy(header, data.data() + 8 + index * 16, 16);
    const int32_t ofs = versionFirst ? header[1] : header[0], len = versionFirst ? header[2] : header[1];
    const size_t offset = size_t(ofs), length = size_t(len);
    if (ofs < 0 || len < 0 || offset + length > data.size() || length % sizeof(T))
      return {nullptr, 0};
    // Compressed lumps ("LZMA") are not supported (PC maps are uncompressed).
    if (length >= 4 && std::memcmp(data.data() + offset, "LZMA", 4) == 0)
      return {nullptr, 0};
    return {reinterpret_cast<const T *>(data.data() + offset), length / sizeof(T)};
  }
  int32_t lumpVersion(int index) const {
    int32_t v;
    std::memcpy(&v, data.data() + 8 + index * 16 + (versionFirst ? 0 : 8), 4);
    return v;
  }
};

// --- Materials (VMT) and texture hashes (VTF) --------------------------------
struct KeyValues {
  std::string shader;
  std::map<std::string, std::string> values;                 // top level, lowercase keys
  std::map<std::string, std::map<std::string, std::string>> blocks; // top-level sub-blocks (patch insert/replace)
};

std::string lower(std::string s) {
  for (auto &c : s)
    c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool parseKeyValues(const std::string &text, KeyValues &out) {
  std::vector<std::string> tokens;
  for (size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n')
        ++i;
    } else if (c == '{' || c == '}') {
      tokens.emplace_back(1, c);
      ++i;
    } else if (c == '"') {
      const size_t end = text.find('"', i + 1);
      tokens.push_back(text.substr(i + 1, (end == std::string::npos ? text.size() : end) - i - 1));
      tokens.back().insert(0, 1, '\x01'); // quoted: never a brace
      i = end == std::string::npos ? text.size() : end + 1;
    } else {
      size_t end = i;
      while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != '{' &&
             text[end] != '}' && text[end] != '"')
        ++end;
      tokens.push_back(text.substr(i, end - i));
      i = end;
    }
  }
  auto plain = [](std::string t) { return !t.empty() && t[0] == '\x01' ? t.substr(1) : t; };
  if (tokens.size() < 2 || tokens[1] != "{")
    return false;
  out.shader = lower(plain(tokens[0]));
  size_t i = 2;
  // Skips a block whose '{' was consumed.
  auto skip = [&]() {
    for (int depth = 1; i < tokens.size() && depth > 0; ++i)
      depth += tokens[i] == "{" ? 1 : tokens[i] == "}" ? -1 : 0;
  };
  while (i < tokens.size() && tokens[i] != "}") {
    const std::string key = lower(plain(tokens[i++]));
    if (i >= tokens.size())
      break;
    if (tokens[i] == "{") {
      ++i;
      auto &block = out.blocks[key];
      while (i < tokens.size() && tokens[i] != "}") {
        const std::string k = lower(plain(tokens[i++]));
        if (i >= tokens.size())
          break;
        if (tokens[i] == "{") {
          ++i;
          skip();
        } else {
          block[k] = plain(tokens[i++]);
        }
      }
      ++i;
    } else {
      out.values[key] = plain(tokens[i++]);
    }
  }
  return true;
}

// A material's top-level values, with patch materials (cubemap and water
// variants the compiler writes into the pakfile) applied onto their include.
bool loadMaterial(const std::string &name, KeyValues &out, int depth = 0) {
  std::vector<uint8_t> bytes;
  std::string path = lower(name);
  if (path.rfind("materials/", 0) != 0)
    path = "materials/" + path;
  if (path.size() < 4 || path.compare(path.size() - 4, 4, ".vmt") != 0)
    path += ".vmt";
  if (depth > 4 || !fs::read(path, bytes))
    return false;
  KeyValues kv;
  if (!parseKeyValues(std::string(bytes.begin(), bytes.end()), kv))
    return false;
  if (kv.shader != "patch") {
    out = std::move(kv);
    return true;
  }
  if (!loadMaterial(kv.values["include"], out, depth + 1))
    return false;
  for (const char *section : {"insert", "replace"})
    for (auto &[k, v] : kv.blocks[section])
      out.values[k] = v;
  return true;
}

// dxvk hashes of a texture's three largest mips as D3D would receive them
// (packed rows; the engine may skip top mips by quality settings). Formats the
// proxy does not upload yield none.
std::vector<uint64_t> textureHashes(const std::string &name) {
  const std::string path = "materials/" + lower(name) + ".vtf";
  std::vector<uint8_t> header;
  uint64_t total = 0;
  if (!fs::size(path, total) || !fs::read(path, header, 0, 80) || header.size() < 64 ||
      std::memcmp(header.data(), "VTF", 4) != 0)
    return {};
  uint16_t width, height, frames;
  uint32_t flags;
  int32_t format;
  std::memcpy(&width, header.data() + 16, 2);
  std::memcpy(&height, header.data() + 18, 2);
  std::memcpy(&flags, header.data() + 20, 4);
  std::memcpy(&frames, header.data() + 24, 2);
  std::memcpy(&format, header.data() + 52, 4);
  const uint8_t mips = header[56];
  uint32_t blockBytes = 0, pixelBytes = 0;
  switch (format) {
  case 13: // DXT1
  case 20: // DXT1_ONEBITALPHA
    blockBytes = 8;
    break;
  case 15: // DXT5
    blockBytes = 16;
    break;
  case 12: // BGRA8888
  case 16: // BGRX8888
    pixelBytes = 4;
    break;
  default:
    return {};
  }
  const uint32_t faces = (flags & 0x4000) ? 6 : 1; // environment maps
  // High-resolution mips are stored smallest first and end the file.
  std::vector<uint64_t> hashes;
  uint64_t end = total;
  for (uint32_t mip = 0; mip < std::min<uint32_t>(3, mips); ++mip) {
    const uint32_t w = std::max(1u, uint32_t(width) >> mip), h = std::max(1u, uint32_t(height) >> mip);
    const uint64_t bytes = blockBytes ? uint64_t((w + 3) / 4) * ((h + 3) / 4) * blockBytes : uint64_t(w) * h * pixelBytes;
    const uint64_t block = bytes * std::max<uint16_t>(frames, 1) * faces;
    if (block > end)
      break;
    std::vector<uint8_t> level;
    if (!fs::read(path, level, end - block, bytes) || level.size() != bytes)
      break;
    hashes.push_back(XXH3_64bits(level.data(), level.size()));
    end -= block;
  }
  return hashes;
}

// --- World geometry -----------------------------------------------------------
struct Material {
  std::string name;
  std::vector<uint64_t> candidates; // texture hashes, largest mip first
  uint32_t categories = 0;
  bool alphaTest = false, modulate = false;
  bool owned = false; // ref.handle created here (water)
  uint8_t alphaRef = 179;
  MaterialRef ref;
  bool resolved = false;
};

struct Geometry {
  std::vector<remixapi_HardcodedVertex> vertices;
  std::vector<uint32_t> indices;
};

// Faces sharing a spatial cell and draw state: one mesh (a surface per
// material) and one instance.
struct Group {
  bool sky = false, alphaTest = false;
  uint8_t alphaRef = 0;
  uint32_t categories = 0;
  uint64_t id = 0;
  std::map<uint32_t, Geometry> surfaces; // by material index
  remixapi_MeshHandle mesh = nullptr;
  uint32_t builtResolved = ~0u; // resolved materials when last built
};

// One strip of a studio model (LOD 0): the unit the engine draws, so its
// dxvk mesh hash (with the base texture's) keys mod mesh replacements.
struct PropPart {
  remixapi_MeshHandle mesh = nullptr;
  int material = -1;
  uint64_t replacementHash = 0;
  std::vector<float> positions; // model space, for occluders()
  std::vector<uint32_t> indices;
};
struct PropModel {
  std::vector<PropPart> parts;
  bool engineParts = false; // translucent or ignored meshes the engine still draws
};
struct Prop {
  float transform[3][4];
  const PropModel *model = nullptr;
  uint64_t id = 0;
  // Seen by the engine's 3D skybox view: drawn by the engine, not here.
  bool skybox = false;
};

// info_overlay decals, clipped onto their faces; merged per cell and blend.
struct OverlayGroup {
  bool modulate = false; // DecalModulate: multiplies the surface (DESTCOLOR, SRCCOLOR)
  bool sky = false;
  uint64_t id = 0;
  std::map<uint32_t, Geometry> surfaces; // by material index
  remixapi_MeshHandle mesh = nullptr;
  uint32_t builtResolved = ~0u;
};

struct World {
  const remixapi_Interface *api = nullptr;
  std::string map;
  std::vector<Material> materials;
  std::unordered_map<std::string, int> materialIndex;
  std::map<std::pair<std::string, int>, PropModel> propModels; // by model path and skin
  std::vector<Prop> props;
  // Point location at runtime (the camera of a view): BSP tree and leaf areas.
  std::vector<Node> nodes;
  std::vector<Plane> planes;
  std::vector<int16_t> leafAreas;
  int skyArea = -1;
  std::map<std::array<int32_t, 5>, OverlayGroup> overlayGroups;
  std::unordered_map<std::string, int> overlayMaterialIndex;
  uint32_t overlays = 0, overlayTriangles = 0, waterMaterials = 0;
  // Prop placements by 64-unit cell of their origin, for matching engine draws.
  std::unordered_map<uint64_t, std::vector<uint32_t>> propCells;
  std::map<std::array<int32_t, 6>, Group> groups;
  bool skyCamera = false;
  float skyOrigin[3] = {};
  float skyScale = 16;
  uint64_t frames = 0;
  // Statistics.
  uint32_t faces = 0, displacements = 0, skyFaces = 0, skippedTranslucent = 0, skippedIgnored = 0, keptIgnored = 0, triangles = 0;
  uint32_t propModelsFailed = 0, propParts = 0, skyProps = 0;
  std::vector<std::string> failedModels;
  std::vector<Light> lights;
  Occluders occluders;
  bool occludersBuilt = false;
  // Bumped whenever the instances submit() draws change (meshes built,
  // materials resolved, props found in the 3D skybox); a bridge batch
  // recorded at one version is replayed while it lasts.
  uint64_t submitVersion = 0, batchVersion = ~0ull, stableVersion = ~0ull, stableSince = 0;
  bool batchValid = false;
  uint64_t replayedFrames = 0;
  uint32_t recordings = 0;
  uint32_t styledLights = 0, skyboxLights = 0, otherLights = 0;
} g_world;
bool g_active = false;

// The static props' placements for providesProp(), which other threads call
// (l4d2-rtx's model hook on the game thread while the render thread may be
// loading a map): immutable once published.
struct PropIndex {
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
  std::vector<std::array<float, 12>> transforms;
};
std::shared_ptr<const PropIndex> g_propIndex;
std::mutex g_propIndexLock;

void publishPropIndex(std::shared_ptr<const PropIndex> index) {
  std::lock_guard<std::mutex> lock(g_propIndexLock);
  g_propIndex = std::move(index);
}

float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The PVS of a cluster (one bit per cluster), empty without visibility data.
std::vector<uint8_t> pvs(const Bsp &bsp, int cluster) {
  const auto [vis, bytes] = bsp.lump<uint8_t>(kVisibility);
  if (!vis || bytes < 4 || cluster < 0)
    return {};
  int32_t clusters;
  std::memcpy(&clusters, vis, 4);
  if (cluster >= clusters || size_t(8 + clusters * 8) > bytes)
    return {};
  int32_t offset;
  std::memcpy(&offset, vis + 4 + cluster * 8, 4);
  std::vector<uint8_t> out(size_t(clusters + 7) / 8, 0);
  size_t at = size_t(offset), c = 0;
  while (c < out.size() && at < bytes) {
    if (vis[at]) {
      out[c++] = vis[at++];
    } else if (at + 1 < bytes) {
      c += vis[at + 1];
      at += 2;
    } else {
      break;
    }
  }
  return out;
}

int leafAt(const Bsp &bsp, const float p[3]) {
  const auto [nodes, nodeCount] = bsp.lump<Node>(kNodes);
  const auto [planes, planeCount] = bsp.lump<Plane>(kPlanes);
  int index = 0;
  for (int guard = 0; index >= 0 && guard < 4096; ++guard) {
    if (size_t(index) >= nodeCount || size_t(nodes[index].plane) >= planeCount)
      return -1;
    const auto &plane = planes[nodes[index].plane];
    index = nodes[index].children[dot(plane.normal, p) - plane.dist >= 0 ? 0 : 1];
  }
  return index < 0 ? -1 - index : -1;
}

void parseSkyCamera(const Bsp &bsp) {
  const auto [text, length] = bsp.lump<char>(kEntities);
  if (!text)
    return;
  const std::string entities(text, length);
  for (size_t open = entities.find('{'); open != std::string::npos; open = entities.find('{', open + 1)) {
    const size_t close = entities.find('}', open);
    if (close == std::string::npos)
      break;
    KeyValues kv;
    if (!parseKeyValues("entity " + entities.substr(open, close - open + 1), kv) ||
        lower(kv.values["classname"]) != "sky_camera")
      continue;
    if (std::sscanf(kv.values["origin"].c_str(), "%f %f %f", &g_world.skyOrigin[0], &g_world.skyOrigin[1],
                    &g_world.skyOrigin[2]) == 3) {
      g_world.skyCamera = true;
      if (kv.values.count("scale"))
        g_world.skyScale = float(std::atof(kv.values["scale"].c_str()));
      if (!(g_world.skyScale > 0))
        g_world.skyScale = 16;
    }
    return;
  }
}

// Bounds-checked reads from a file image.
template <typename T> bool get(const std::vector<uint8_t> &b, size_t at, T &out) {
  if (at > b.size() || b.size() - at < sizeof(T))
    return false;
  std::memcpy(&out, b.data() + at, sizeof(T));
  return true;
}
int32_t i32(const std::vector<uint8_t> &b, size_t at) {
  int32_t v = 0;
  get(b, at, v);
  return v;
}
std::string cstring(const std::vector<uint8_t> &b, size_t at) {
  if (at >= b.size())
    return {};
  const auto *p = reinterpret_cast<const char *>(b.data() + at);
  return std::string(p, strnlen(p, b.size() - at));
}

int materialByName(const std::string &name, bool brush = false);
} // namespace
int areaAt(const float p[3]);
namespace {

// A static prop's model: body 0 of every body part at LOD 0, a part per VTX
// strip (vertices in strip-group order from the VVD, indices rebased), each
// with the dxvk mesh hash of the engine's draw of it.
bool loadPropModel(const std::string &path, int skin, PropModel &out) {
  std::vector<uint8_t> mdl, vvd, vtx;
  const std::string base = path.substr(0, path.size() - 4);
  if (!fs::read(path, mdl) || !fs::read(base + ".vvd", vvd) || !fs::read(base + ".dx90.vtx", vtx) ||
      mdl.size() < 240 || vvd.size() < 64 || vtx.size() < 36)
    return false;
  // studiohdr_t
  const int32_t numTextures = i32(mdl, 204), textureIndex = i32(mdl, 208), numCd = i32(mdl, 212),
                cdIndex = i32(mdl, 216), numSkinRef = i32(mdl, 220), numFamilies = i32(mdl, 224),
                skinIndex = i32(mdl, 228), numBodyParts = i32(mdl, 232), bodyPartIndex = i32(mdl, 236);
  std::vector<std::string> textures, cdPaths;
  for (int t = 0; t < numTextures; ++t) {
    const size_t at = size_t(textureIndex) + size_t(t) * 64; // mstudiotexture_t
    textures.push_back(cstring(mdl, at + i32(mdl, at)));
  }
  for (int c = 0; c < numCd; ++c)
    cdPaths.push_back(lower(cstring(mdl, size_t(i32(mdl, size_t(cdIndex) + c * 4)))));
  const int family = skin >= 0 && skin < numFamilies ? skin : 0;
  auto textureOf = [&](int material) -> int {
    int16_t ref = int16_t(material);
    if (material < numSkinRef)
      get(mdl, size_t(skinIndex) + (size_t(family) * numSkinRef + material) * 2, ref);
    return ref;
  };
  // VVD: LOD 0 vertices through the fixup table.
  const int32_t numFixups = i32(vvd, 48), fixupStart = i32(vvd, 52), vertexStart = i32(vvd, 56);
  std::vector<uint32_t> vertexMap;
  if (numFixups > 0) {
    for (int f = 0; f < numFixups; ++f) {
      const size_t at = size_t(fixupStart) + size_t(f) * 12;
      const int32_t lod = i32(vvd, at), source = i32(vvd, at + 4), count = i32(vvd, at + 8);
      if (lod >= 0 && source >= 0 && count >= 0)
        for (int32_t v = 0; v < count; ++v)
          vertexMap.push_back(uint32_t(source + v));
    }
  } else {
    for (int32_t v = 0; v < i32(vvd, 16); ++v)
      vertexMap.push_back(uint32_t(v));
  }
  // mstudiovertex_t (48 bytes): bone weights 16, position 12, normal 12, texcoord 8.
  auto vvdVertex = [&](uint32_t index, remixapi_HardcodedVertex &v, const uint8_t *&position) {
    if (index >= vertexMap.size())
      return false;
    const size_t at = size_t(vertexStart) + size_t(vertexMap[index]) * 48;
    if (at + 48 > vvd.size())
      return false;
    position = vvd.data() + at + 16;
    std::memcpy(v.position, position, 12);
    std::memcpy(v.normal, vvd.data() + at + 28, 12);
    std::memcpy(v.texcoord, vvd.data() + at + 40, 8);
    v.color = 0xffffffff;
    return true;
  };
  // VTX (OptimizedModel, packed): FileHeader 36, BodyPart 8, Model 8, ModelLOD 12,
  // Mesh 9, StripGroup 25, Strip 27, Vertex 9.
  const int32_t vtxBodyParts = i32(vtx, 28), vtxBodyPartOffset = i32(vtx, 32);
  bool complete = vtxBodyParts >= numBodyParts;
  for (int b = 0; b < numBodyParts && b < vtxBodyParts; ++b) {
    const size_t bodyPart = size_t(bodyPartIndex) + size_t(b) * 16; // mstudiobodyparts_t
    if (i32(mdl, bodyPart + 4) < 1)
      continue;
    const size_t model = bodyPart + i32(mdl, bodyPart + 12); // body 0: first model
    const int32_t numMeshes = i32(mdl, model + 72);
    const size_t meshIndex = model + i32(mdl, model + 76);
    const int32_t modelVertex = i32(mdl, model + 84) / 48;
    const size_t vtxBody = size_t(vtxBodyPartOffset) + size_t(b) * 8;
    const size_t vtxModel = vtxBody + i32(vtx, vtxBody + 4);
    const size_t vtxLod = vtxModel + i32(vtx, vtxModel + 4);
    const int32_t vtxMeshes = i32(vtx, vtxLod);
    const size_t vtxMeshOffset = vtxLod + i32(vtx, vtxLod + 4);
    if (vtxMeshes < numMeshes)
      complete = false;
    for (int m = 0; m < numMeshes && m < vtxMeshes; ++m) {
      const size_t mesh = meshIndex + size_t(m) * 116; // mstudiomesh_t
      const int32_t materialRef = i32(mdl, mesh), meshVertex = i32(mdl, mesh + 12);
      const int texture = textureOf(materialRef);
      if (texture < 0 || size_t(texture) >= textures.size()) {
        complete = false;
        continue;
      }
      int material = -1;
      bool found = false;
      for (const auto &cd : cdPaths) {
        std::string name = cd + lower(textures[texture]);
        uint64_t bytes;
        if (fs::size("materials/" + name + ".vmt", bytes)) {
          material = materialByName(name);
          found = true;
          break;
        }
      }
      if (!found) {
        complete = false;
        continue;
      }
      // Translucent (blended, so still drawn by the engine) or ignored.
      if (material < 0) {
        out.engineParts = true;
        continue;
      }
      // Without a texture hash (format the proxy does not upload) the part
      // still draws, with the default material.
      const auto &candidates = g_world.materials[material].candidates;
      const uint64_t textureHash = candidates.empty() ? 0 : candidates[0];
      const size_t vtxMesh = vtxMeshOffset + size_t(m) * 9;
      const int32_t numGroups = i32(vtx, vtxMesh);
      const size_t groupOffset = vtxMesh + i32(vtx, vtxMesh + 4);
      for (int g = 0; g < numGroups; ++g) {
        const size_t group = groupOffset + size_t(g) * 25;
        const int32_t numVerts = i32(vtx, group), vertOffset = i32(vtx, group + 4), numIndices = i32(vtx, group + 8),
                      indexOffset = i32(vtx, group + 12), numStrips = i32(vtx, group + 16),
                      stripOffset = i32(vtx, group + 20);
        if (numVerts <= 0 || numIndices <= 0)
          continue;
        std::vector<remixapi_HardcodedVertex> vertices(static_cast<size_t>(numVerts));
        std::vector<const uint8_t *> positions(static_cast<size_t>(numVerts));
        bool ok = true;
        for (int v = 0; v < numVerts && ok; ++v) {
          uint16_t original = 0;
          ok = get(vtx, group + vertOffset + size_t(v) * 9 + 4, original) &&
               vvdVertex(uint32_t(modelVertex + meshVertex + original), vertices[v], positions[v]);
        }
        std::vector<uint16_t> indices(static_cast<size_t>(numIndices));
        for (int i = 0; i < numIndices && ok; ++i)
          ok = get(vtx, group + indexOffset + size_t(i) * 2, indices[i]) && indices[i] < numVerts;
        if (!ok) {
          complete = false;
          continue;
        }
        for (int st = 0; st < numStrips; ++st) {
          const size_t strip = group + stripOffset + size_t(st) * 27;
          const int32_t count = i32(vtx, strip), first = i32(vtx, strip + 4);
          uint8_t flags = 0;
          get(vtx, strip + 18, flags); // StripHeader_t::flags: 1 tri list, 2 tri strip
          if (count < 3 || first < 0 || first + count > numIndices || (flags & 2)) {
            complete = false;
            continue;
          }
          const auto begin = indices.begin() + first, end = begin + count;
          const auto [lo, hi] = std::minmax_element(begin, end);
          const uint16_t low = *lo, high = *hi;
          if (low == high)
            continue;
          // dxvk::geometryHash of the draw (dxvk_hash.cpp).
          std::vector<uint16_t> rebased(begin, end);
          for (auto &i : rebased)
            i = uint16_t(i - low);
          const uint64_t indexHash = XXH3_64bits(rebased.data(), rebased.size() * 2);
          std::vector<uint16_t> referenced(begin, end);
          std::sort(referenced.begin(), referenced.end());
          referenced.erase(std::unique(referenced.begin(), referenced.end()), referenced.end());
          uint64_t positionHash = 0;
          for (uint16_t i : referenced)
            positionHash = XXH3_64bits_withSeed(positions[i], 12, positionHash);
          const uint32_t descriptor[4] = {uint32_t(count), uint32_t(high - low + 1), 3, 0};
          uint64_t descriptorHash = 0;
          for (uint32_t field : descriptor)
            descriptorHash = XXH3_64bits_withSeed(&field, 4, descriptorHash);
          const uint64_t geometry = XXH64(&descriptorHash, 8, XXH64(&indexHash, 8, positionHash));

          std::vector<remixapi_HardcodedVertex> partVertices(vertices.begin() + low, vertices.begin() + high + 1);
          std::vector<uint32_t> partIndices(rebased.begin(), rebased.end());
          remixapi_MeshInfoSurfaceTriangles surface = {};
          surface.vertices_values = partVertices.data();
          surface.vertices_count = partVertices.size();
          surface.indices_values = partIndices.data();
          surface.indices_count = partIndices.size();
          remixapi_MeshInfo info = {};
          info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
          info.hash = geometry ^ textureHash;
          info.surfaces_values = &surface;
          info.surfaces_count = 1;
          PropPart part;
          part.material = material;
          part.replacementHash = geometry ^ textureHash;
          if (g_world.api->CreateMesh(&info, &part.mesh) != REMIXAPI_ERROR_CODE_SUCCESS)
            continue;
          for (const auto &v : partVertices)
            part.positions.insert(part.positions.end(), {v.position[0], v.position[1], v.position[2]});
          part.indices = std::move(partIndices);
          out.parts.push_back(part);
          ++g_world.propParts;
        }
      }
    }
  }
  // A partial model would replace the engine's whole prop (its draws are
  // dropped by placement): leave such models to the engine.
  if (!complete) {
    for (auto &part : out.parts)
      if (part.mesh)
        g_world.api->DestroyMesh(part.mesh);
    g_world.propParts -= uint32_t(out.parts.size());
    out.parts.clear();
  }
  return !out.parts.empty();
}

uint64_t propCell(const float origin[3]) {
  const int32_t c[3] = {int32_t(std::floor(origin[0] / 64)), int32_t(std::floor(origin[1] / 64)),
                        int32_t(std::floor(origin[2] / 64))};
  return (uint64_t(uint32_t(c[0]) & 0x1fffff) << 42) | (uint64_t(uint32_t(c[1]) & 0x1fffff) << 21) |
         uint64_t(uint32_t(c[2]) & 0x1fffff);
}

// Source AngleMatrix: rows of the model-to-world 3x4 (columns forward, left, up).
void angleMatrix(const float angles[3], const float origin[3], float m[3][4]) {
  constexpr float kRad = 3.14159265358979f / 180;
  const float sp = std::sin(angles[0] * kRad), cp = std::cos(angles[0] * kRad), sy = std::sin(angles[1] * kRad),
              cy = std::cos(angles[1] * kRad), sr = std::sin(angles[2] * kRad), cr = std::cos(angles[2] * kRad);
  m[0][0] = cp * cy;
  m[1][0] = cp * sy;
  m[2][0] = -sp;
  m[0][1] = sr * sp * cy - cr * sy;
  m[1][1] = sr * sp * sy + cr * cy;
  m[2][1] = sr * cp;
  m[0][2] = cr * sp * cy + sr * sy;
  m[1][2] = cr * sp * sy - sr * cy;
  m[2][2] = cr * cp;
  for (int r = 0; r < 3; ++r)
    m[r][3] = origin[r];
}

// Index into g_world.materials, or -1 for materials left to the engine
// (translucent: its blended draws are kept) or ignored by rtx.conf. brush:
// for the map's own faces, which rtx.conf does not remove (its ignore list
// was made for dxvk-remix and holds world textures; the faces would leave
// holes in the map).
int materialByName(const std::string &name, bool brush) {
  const std::string key = lower(name);
  const std::string cacheKey = brush ? "\x01" + key : key;
  if (auto it = g_world.materialIndex.find(cacheKey); it != g_world.materialIndex.end())
    return it->second;
  int &slot = g_world.materialIndex[cacheKey];
  slot = -1;
  Material m;
  m.name = name;
  KeyValues kv;
  if (loadMaterial(m.name, kv) && kv.shader == "water") {
    // Water: Source draws it from render targets (refraction, reflection),
    // which have no counterpart here. A smooth surface of its fog color
    // (linear), blended by opacity: murky water (short $fogend, inches) is
    // nearly opaque, clear water lets the bottom through.
    float fog[3] = {100, 110, 120};
    const std::string &color = kv.values["$fogcolor"];
    const size_t digits = color.find_first_of("0123456789");
    if (digits != std::string::npos)
      std::sscanf(color.c_str() + digits, "%f %f %f", &fog[0], &fog[1], &fog[2]);
    const float fogEnd = kv.values.count("$fogend") ? float(std::atof(kv.values["$fogend"].c_str())) : 128;
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {std::pow(std::clamp(fog[0], 0.0f, 255.0f) / 255, 2.2f),
                             std::pow(std::clamp(fog[1], 0.0f, 255.0f) / 255, 2.2f),
                             std::pow(std::clamp(fog[2], 0.0f, 255.0f) / 255, 2.2f)};
    opaque.opacityConstant = std::clamp(0.35f + 40.0f / std::max(fogEnd, 1.0f), 0.35f, 0.9f);
    opaque.roughnessConstant = 0.05f;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = XXH3_64bits(key.data(), key.size()) | 1;
    info.filterMode = 1;
    info.wrapModeU = info.wrapModeV = 1;
    if (g_world.api && g_world.api->CreateMaterial(&info, &m.ref.handle) == REMIXAPI_ERROR_CODE_SUCCESS) {
      m.resolved = m.owned = true;
      ++g_world.waterMaterials;
    }
    slot = int(g_world.materials.size());
    g_world.materials.push_back(std::move(m));
    return slot;
  }
  if (!kv.shader.empty()) {
    auto flag = [&](const char *k) { return std::atoi(kv.values[k].c_str()) != 0; };
    if (flag("$translucent") || flag("$additive")) {
      ++g_world.skippedTranslucent;
      return slot;
    }
    m.alphaTest = flag("$alphatest");
    if (kv.values.count("$alphatestreference")) {
      const float r = float(std::atof(kv.values["$alphatestreference"].c_str()));
      if (r > 0)
        m.alphaRef = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255 + 0.5f);
    }
    if (kv.values.count("$basetexture"))
      m.candidates = textureHashes(kv.values["$basetexture"]);
  }
  for (uint64_t h : m.candidates)
    m.categories |= categories::lookup(h);
  if (m.categories & (REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE | REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN)) {
    if (!brush) {
      ++g_world.skippedIgnored;
      return slot;
    }
    m.categories &= ~uint32_t(REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE | REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN);
    ++g_world.keptIgnored;
  }
  slot = int(g_world.materials.size());
  g_world.materials.push_back(std::move(m));
  return slot;
}

constexpr uint64_t kBatchId = 0x42535057; // "BSPW"

void destroyMeshes() {
  if (g_world.batchValid && g_world.api) {
    using BatchFn = remixapi_ErrorCode(REMIXAPI_CALL *)(uint64_t);
    if (auto drop = reinterpret_cast<BatchFn>(
            GetProcAddress(GetModuleHandleW(L"remix_bridge.dll"), "openremix_bridge_DeleteBatch")))
      drop(kBatchId);
  }
  for (auto &m : g_world.materials)
    if (m.owned && m.ref.handle && g_world.api)
      g_world.api->DestroyMaterial(m.ref.handle);
  for (auto &[key, group] : g_world.overlayGroups)
    if (group.mesh && g_world.api)
      g_world.api->DestroyMesh(group.mesh);
  for (auto &[key, model] : g_world.propModels)
    for (auto &part : model.parts)
      if (part.mesh && g_world.api)
        g_world.api->DestroyMesh(part.mesh);
  for (auto &[key, group] : g_world.groups)
    if (group.mesh && g_world.api)
      g_world.api->DestroyMesh(group.mesh);
}

// Static props (game lump "sprp", version 9/10 placements of at least 72 bytes).
template <typename InSky> void loadProps(const Bsp &bsp, const InSky &inSky) {
  // OPENREMIX_PROXY_BSP_PROPS=0 leaves static props to the engine.
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_PROPS", v, sizeof(v)) && v[0] == '0');
  }();
  if (!enabled)
    return;
  const auto &file = bsp.data;
  const auto [directory, bytes] = bsp.lump<uint8_t>(kGameLump);
  if (!directory || bytes < 4)
    return;
  int32_t count;
  std::memcpy(&count, directory, 4);
  size_t start = 0, length = 0;
  uint16_t version = 0;
  for (int32_t i = 0; i < count && size_t(4 + (i + 1) * 16) <= bytes; ++i) {
    int32_t id, offset, size;
    uint16_t flags;
    std::memcpy(&id, directory + 4 + i * 16, 4);
    std::memcpy(&flags, directory + 8 + i * 16, 2);
    std::memcpy(&version, directory + 10 + i * 16, 2);
    std::memcpy(&offset, directory + 12 + i * 16, 4);
    std::memcpy(&size, directory + 16 + i * 16, 4);
    if (id == 0x73707270 && !(flags & 1) && offset > 0 && size > 0 && size_t(offset) + size <= file.size()) {
      start = size_t(offset);
      length = size_t(size);
      break;
    }
  }
  // sprp v4 (HL2-era) .. v12: origin 0, angles 12, model 24, skin 32 in all.
  if (!length || version < 4)
    return;
  const std::vector<uint8_t> lump(file.begin() + start, file.begin() + start + length);
  const int32_t names = i32(lump, 0);
  size_t at = 4 + size_t(std::max(names, 0)) * 128;
  const int32_t leaves = i32(lump, at);
  at += 4 + size_t(std::max(leaves, 0)) * 2;
  const int32_t props = i32(lump, at);
  at += 4;
  if (names <= 0 || props <= 0 || at > lump.size())
    return;
  const size_t stride = (lump.size() - at) / size_t(props);
  if (stride < 56) // v4 records are 56 bytes (GMod's gm_construct: v6, 64)
    return;
  for (int32_t p = 0; p < props; ++p) {
    const size_t rec = at + size_t(p) * stride;
    float origin[3], angles[3];
    uint16_t modelIndex = 0;
    int32_t skin = 0;
    if (!get(lump, rec, origin) || !get(lump, rec + 12, angles) || !get(lump, rec + 24, modelIndex) ||
        !get(lump, rec + 32, skin) || modelIndex >= names)
      continue;
    const std::string path = lower(cstring(lump, 4 + size_t(modelIndex) * 128));
    auto [it, fresh] = g_world.propModels.try_emplace({path, skin});
    if (fresh && !loadPropModel(path, skin, it->second)) {
      ++g_world.propModelsFailed;
      g_world.failedModels.push_back(path);
    }
    if (it->second.parts.empty())
      continue;
    // 3D skybox props stay with the engine: they are seen only through the
    // sky camera, which the engine's sky view already draws in full.
    if (inSky(origin)) {
      ++g_world.skyProps;
      continue;
    }
    Prop prop;
    angleMatrix(angles, origin, prop.transform);
    prop.model = &it->second;
    prop.id = XXH3_64bits_withSeed(&p, sizeof(p), 0x70726f70ull) | 1;
    g_world.propCells[propCell(origin)].push_back(uint32_t(g_world.props.size()));
    g_world.props.push_back(prop);
  }
}

// A face's triangles in world space (displacements tessellated).
std::vector<std::array<std::array<float, 3>, 3>> faceTriangles(const Bsp &bsp, int32_t f) {
  const auto [vertexes, vertexCount] = bsp.lump<std::array<float, 3>>(kVertexes);
  const auto [edges, edgeCount] = bsp.lump<std::array<uint16_t, 2>>(kEdges);
  const auto [surfEdges, surfEdgeCount] = bsp.lump<int32_t>(kSurfEdges);
  const auto [faces, faceCount] = bsp.lump<Face>(kFaces);
  const auto [dispInfos, dispInfoCount] = bsp.lump<DispInfo>(kDispInfo);
  const auto [dispVerts, dispVertCount] = bsp.lump<DispVert>(kDispVerts);
  std::vector<std::array<std::array<float, 3>, 3>> out;
  if (f < 0 || size_t(f) >= faceCount)
    return out;
  const Face &face = faces[f];
  std::vector<std::array<float, 3>> corners;
  for (int e = 0; e < face.numEdges; ++e) {
    const size_t se = size_t(face.firstEdge + e);
    if (se >= surfEdgeCount)
      return out;
    const int32_t edge = surfEdges[se];
    const size_t ei = size_t(edge < 0 ? -edge : edge);
    if (ei >= edgeCount || edges[ei][edge < 0 ? 1 : 0] >= vertexCount)
      return out;
    corners.push_back(vertexes[edges[ei][edge < 0 ? 1 : 0]]);
  }
  if (face.dispInfo < 0) {
    for (size_t k = 1; k + 1 < corners.size(); ++k)
      out.push_back({corners[0], corners[k], corners[k + 1]});
    return out;
  }
  if (!dispInfos || size_t(face.dispInfo) >= dispInfoCount || corners.size() != 4)
    return out;
  const DispInfo &di = dispInfos[face.dispInfo];
  int first = 0;
  float best = 1e30f;
  for (int c = 0; c < 4; ++c) {
    float d2 = 0;
    for (int k = 0; k < 3; ++k)
      d2 += (corners[c][k] - di.start[k]) * (corners[c][k] - di.start[k]);
    if (d2 < best) {
      best = d2;
      first = c;
    }
  }
  const int size = (1 << di.power) + 1;
  if (di.power < 2 || di.power > 4 || di.firstVert < 0 || size_t(di.firstVert + size * size) > dispVertCount)
    return out;
  std::vector<std::array<float, 3>> grid(size_t(size) * size);
  for (int v = 0; v < size; ++v)
    for (int u = 0; u < size; ++u) {
      const float t = float(v) / float(size - 1), s = float(u) / float(size - 1);
      const DispVert &dv = dispVerts[di.firstVert + v * size + u];
      for (int k = 0; k < 3; ++k) {
        const float left = corners[first][k] + (corners[(first + 1) % 4][k] - corners[first][k]) * t;
        const float right = corners[(first + 3) % 4][k] + (corners[(first + 2) % 4][k] - corners[(first + 3) % 4][k]) * t;
        grid[size_t(v) * size + u][k] = left + (right - left) * s + dv.vector[k] * dv.dist;
      }
    }
  for (int v = 0; v + 1 < size; ++v)
    for (int u = 0; u + 1 < size; ++u) {
      const auto &a = grid[size_t(v) * size + u], &b = grid[size_t(v) * size + u + 1],
                 &c = grid[size_t(v + 1) * size + u], &d = grid[size_t(v + 1) * size + u + 1];
      out.push_back({a, c, d});
      out.push_back({a, d, b});
    }
  return out;
}

// Overlay materials (usually translucent decals), by name.
int overlayMaterial(const std::string &name, bool &modulate) {
  const std::string key = lower(name);
  if (auto it = g_world.overlayMaterialIndex.find(key); it != g_world.overlayMaterialIndex.end()) {
    modulate = it->second < 0 ? false : g_world.materials[it->second].modulate;
    return it->second;
  }
  int &slot = g_world.overlayMaterialIndex[key];
  slot = -1;
  Material m;
  m.name = name;
  KeyValues kv;
  if (!loadMaterial(m.name, kv) || !kv.values.count("$basetexture"))
    return slot;
  m.modulate = kv.shader == "decalmodulate";
  m.candidates = textureHashes(kv.values["$basetexture"]);
  for (uint64_t h : m.candidates)
    m.categories |= categories::lookup(h);
  if (m.categories & (REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE | REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN))
    return slot;
  modulate = m.modulate;
  slot = int(g_world.materials.size());
  g_world.materials.push_back(std::move(m));
  return slot;
}

// info_overlay: the overlay's quad (in its plane's U/V basis) clipped against
// each listed face's triangles seen along the overlay normal, then placed on
// the triangle by its barycentric coordinates and lifted off it like the
// proxy's other decals.
template <typename InSky> void loadOverlays(const Bsp &bsp, const InSky &inSky) {
  const auto [overlays, count] = bsp.lump<Overlay>(kOverlays);
  const auto [texInfos, texInfoCount] = bsp.lump<TexInfo>(kTexInfo);
  const auto [texDatas, texDataCount] = bsp.lump<TexData>(kTexData);
  const auto [stringTable, stringTableCount] = bsp.lump<int32_t>(kTexDataStringTable);
  const auto [strings, stringsLength] = bsp.lump<char>(kTexDataStringData);
  if (!overlays || bsp.lumpVersion(kOverlays) != 0)
    return;
  constexpr float kOffset = 0.1f, kCell = 2048;
  struct Point {
    float p[2], uv[2];
  };
  for (size_t o = 0; o < count; ++o) {
    const Overlay &ov = overlays[o];
    if (ov.texInfo < 0 || size_t(ov.texInfo) >= texInfoCount)
      continue;
    const int32_t td = texInfos[ov.texInfo].texData;
    if (td < 0 || size_t(td) >= texDataCount)
      continue;
    const int32_t nameId = texDatas[td].nameId;
    if (nameId < 0 || size_t(nameId) >= stringTableCount || size_t(stringTable[nameId]) >= stringsLength)
      continue;
    const char *name = strings + stringTable[nameId];
    bool modulate = false;
    const int material = overlayMaterial(std::string(name, strnlen(name, stringsLength - stringTable[nameId])), modulate);
    if (material < 0)
      continue;
    // Basis: N, U (packed into the corners' z), V = N x U.
    float n[3], u[3], v[3];
    std::memcpy(n, ov.normal, 12);
    for (int k = 0; k < 3; ++k)
      u[k] = ov.uvPoints[k][2];
    v[0] = n[1] * u[2] - n[2] * u[1];
    v[1] = n[2] * u[0] - n[0] * u[2];
    v[2] = n[0] * u[1] - n[1] * u[0];
    const float quadUv[4][2] = {{ov.u[0], ov.v[0]}, {ov.u[0], ov.v[1]}, {ov.u[1], ov.v[1]}, {ov.u[1], ov.v[0]}};
    std::vector<Point> quad(4);
    for (int c = 0; c < 4; ++c)
      quad[c] = {{ov.uvPoints[c][0], ov.uvPoints[c][1]}, {quadUv[c][0], quadUv[c][1]}};
    // 3D skybox overlays stay with the engine, like its faces.
    if (inSky(ov.origin))
      continue;
    const bool sky = false;
    std::array<int32_t, 5> key = {int32_t(std::floor(ov.origin[0] / kCell)), int32_t(std::floor(ov.origin[1] / kCell)),
                                  int32_t(std::floor(ov.origin[2] / kCell)), modulate, sky};
    auto &group = g_world.overlayGroups[key];
    group.modulate = modulate;
    group.sky = sky;
    group.id = XXH3_64bits_withSeed(key.data(), sizeof(key), 0x6f766c79ull) | 1;
    Geometry &g = group.surfaces[uint32_t(material)];
    const uint32_t faceCount = ov.faceCountAndRenderOrder & 0x3fff;
    for (uint32_t fi = 0; fi < faceCount && fi < 64; ++fi) {
      for (const auto &tri : faceTriangles(bsp, ov.faces[fi])) {
        float t2[3][2];
        for (int k = 0; k < 3; ++k) {
          const float rel[3] = {tri[k][0] - ov.origin[0], tri[k][1] - ov.origin[1], tri[k][2] - ov.origin[2]};
          t2[k][0] = dot(rel, u);
          t2[k][1] = dot(rel, v);
        }
        const float area = (t2[1][0] - t2[0][0]) * (t2[2][1] - t2[0][1]) - (t2[2][0] - t2[0][0]) * (t2[1][1] - t2[0][1]);
        if (std::fabs(area) < 1e-4f)
          continue; // edge-on to the overlay
        // Clip the quad against the triangle's edges (Sutherland-Hodgman).
        std::vector<Point> poly = quad;
        for (int e = 0; e < 3 && poly.size() >= 3; ++e) {
          const float *a = t2[e], *b = t2[(e + 1) % 3];
          auto side = [&](const Point &p) {
            return ((b[0] - a[0]) * (p.p[1] - a[1]) - (b[1] - a[1]) * (p.p[0] - a[0])) * (area > 0 ? 1.0f : -1.0f);
          };
          std::vector<Point> next;
          for (size_t i = 0; i < poly.size(); ++i) {
            const Point &p = poly[i], &q = poly[(i + 1) % poly.size()];
            const float sp = side(p), sq = side(q);
            if (sp >= 0)
              next.push_back(p);
            if ((sp >= 0) != (sq >= 0)) {
              const float t = sp / (sp - sq);
              next.push_back({{p.p[0] + (q.p[0] - p.p[0]) * t, p.p[1] + (q.p[1] - p.p[1]) * t},
                              {p.uv[0] + (q.uv[0] - p.uv[0]) * t, p.uv[1] + (q.uv[1] - p.uv[1]) * t}});
            }
          }
          poly = std::move(next);
        }
        if (poly.size() < 3)
          continue;
        const float e1[3] = {tri[1][0] - tri[0][0], tri[1][1] - tri[0][1], tri[1][2] - tri[0][2]};
        const float e2[3] = {tri[2][0] - tri[0][0], tri[2][1] - tri[0][1], tri[2][2] - tri[0][2]};
        float tn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float len = std::sqrt(dot(tn, tn));
        if (len < 1e-8f)
          continue;
        for (float &x : tn)
          x /= dot(tn, n) < 0 ? -len : len;
        const uint32_t base = uint32_t(g.vertices.size());
        for (const Point &p : poly) {
          // Barycentric coordinates in the overlay plane.
          const float w1 = ((p.p[0] - t2[0][0]) * (t2[2][1] - t2[0][1]) - (t2[2][0] - t2[0][0]) * (p.p[1] - t2[0][1])) / area;
          const float w2 = ((t2[1][0] - t2[0][0]) * (p.p[1] - t2[0][1]) - (p.p[0] - t2[0][0]) * (t2[1][1] - t2[0][1])) / area;
          remixapi_HardcodedVertex vtx = {};
          for (int k = 0; k < 3; ++k)
            vtx.position[k] = tri[0][k] + e1[k] * w1 + e2[k] * w2 + tn[k] * kOffset;
          std::memcpy(vtx.normal, tn, 12);
          vtx.texcoord[0] = p.uv[0];
          vtx.texcoord[1] = p.uv[1];
          vtx.color = 0xffffffff;
          g.vertices.push_back(vtx);
        }
        for (uint32_t k = 1; k + 1 < poly.size(); ++k)
          g.indices.insert(g.indices.end(), {base, base + k, base + k + 1});
        g_world.overlayTriangles += uint32_t(poly.size() - 2);
      }
    }
    ++g_world.overlays;
  }
  for (auto it = g_world.overlayGroups.begin(); it != g_world.overlayGroups.end();) {
    for (auto s = it->second.surfaces.begin(); s != it->second.surfaces.end();)
      s = s->second.indices.empty() ? it->second.surfaces.erase(s) : std::next(s);
    it = it->second.surfaces.empty() ? g_world.overlayGroups.erase(it) : std::next(it);
  }
}

// dworldlight_t, lump version 0 (88 bytes; HL2/GMod maps) and version 1
// (L4D2: shadow_cast_offset, 100 bytes).
struct WorldLightV0 {
  float origin[3], intensity[3], normal[3];
  int32_t cluster, type, style;
  float stopdot, stopdot2, exponent, radius, constantAttn, linearAttn, quadraticAttn;
  int32_t flags, texinfo, owner;
};
static_assert(sizeof(WorldLightV0) == 88);
struct WorldLight {
  float origin[3], intensity[3], normal[3], shadowCastOffset[3];
  int32_t cluster, type, style;
  float stopdot, stopdot2, exponent, radius, constantAttn, linearAttn, quadraticAttn;
  int32_t flags, texinfo, owner;
};
static_assert(sizeof(WorldLight) == 100);
enum { kEmitPoint = 1, kEmitSpot = 2, kEmitSky = 3 };

// The map's static lights (HDR worldlights; LDR when the map has no HDR
// lighting). Lights with a light style (switchable, flickering) and lights
// in the 3D skybox stay with the engine's per-model light state.
// OPENREMIX_PROXY_BSP_LIGHTS=0 leaves all lights to the engine.
template <typename InSky> void loadLights(const Bsp &bsp, const InSky &inSky) {
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_LIGHTS", v, sizeof(v)) && v[0] == '0');
  }();
  if (!enabled)
    return;
  int index = kWorldLightsHdr;
  if (!bsp.lump<uint8_t>(index).second)
    index = kWorldLights;
  std::vector<WorldLight> lights;
  if (bsp.lumpVersion(index) == 1) {
    const auto [v1, n] = bsp.lump<WorldLight>(index);
    if (v1)
      lights.assign(v1, v1 + n);
  } else if (bsp.lumpVersion(index) == 0) {
    const auto [v0, n] = bsp.lump<WorldLightV0>(index);
    for (size_t i = 0; v0 && i < n; ++i) {
      WorldLight w = {};
      std::memcpy(w.origin, v0[i].origin, 36);
      std::memcpy(&w.cluster, &v0[i].cluster, sizeof(WorldLightV0) - 36);
      lights.push_back(w);
    }
  }
  const size_t count = lights.size();
  for (size_t i = 0; i < count; ++i) {
    const WorldLight &w = lights[i];
    if (w.type != kEmitPoint && w.type != kEmitSpot && w.type != kEmitSky) {
      ++g_world.otherLights; // surface, Quake and sky ambient lights
      continue;
    }
    // The sun keeps its light (GMod's light_environment often has a style,
    // e.g. 33 on gm_construct, for day/night control).
    if (w.style != 0 && w.type != kEmitSky) {
      ++g_world.styledLights;
      continue;
    }
    if (w.type != kEmitSky && inSky(w.origin)) {
      ++g_world.skyboxLights;
      continue;
    }
    // As the engine passes it to models (WorldLightToMaterialLight).
    Light l;
    l.type = w.type == kEmitPoint ? 1 : w.type == kEmitSpot ? 3 : 2;
    std::memcpy(l.color, w.intensity, 12);
    std::memcpy(l.position, w.origin, 12);
    std::memcpy(l.direction, w.normal, 12);
    l.attenuation[0] = w.constantAttn;
    l.attenuation[1] = w.linearAttn;
    l.attenuation[2] = w.quadraticAttn;
    l.thetaDot = w.stopdot;
    l.phiDot = w.stopdot2;
    l.hash = 0x6c696768745f6273ull ^ (uint64_t(i) * 0x9e3779b97f4a7c15ull);
    g_world.lights.push_back(l);
  }
}

// Leafs as lump version 1 records: version 0 (HL2-era v19 maps) adds a
// 24-byte ambient cube per leaf after the same first 30 bytes.
std::vector<Leaf> leafsOf(const Bsp &bsp) {
  std::vector<Leaf> out;
  const int32_t version = bsp.lumpVersion(kLeafs);
  const size_t stride = version == 0 ? 56 : version == 1 ? sizeof(Leaf) : 0;
  const auto [bytes, size] = bsp.lump<uint8_t>(kLeafs);
  if (!stride || !bytes || size % stride)
    return out;
  out.resize(size / stride);
  for (size_t i = 0; i < out.size(); ++i)
    std::memcpy(&out[i], bytes + i * stride, std::min<size_t>(30, sizeof(Leaf)));
  return out;
}

bool build(const Bsp &bsp) {
  const auto [planes, planeCount] = bsp.lump<Plane>(kPlanes);
  const auto [vertexes, vertexCount] = bsp.lump<std::array<float, 3>>(kVertexes);
  const auto [edges, edgeCount] = bsp.lump<std::array<uint16_t, 2>>(kEdges);
  const auto [surfEdges, surfEdgeCount] = bsp.lump<int32_t>(kSurfEdges);
  const auto [texInfos, texInfoCount] = bsp.lump<TexInfo>(kTexInfo);
  const auto [texDatas, texDataCount] = bsp.lump<TexData>(kTexData);
  const auto [stringTable, stringTableCount] = bsp.lump<int32_t>(kTexDataStringTable);
  const auto [strings, stringsLength] = bsp.lump<char>(kTexDataStringData);
  const auto [faces, faceCount] = bsp.lump<Face>(kFaces);
  const auto [models, modelCount] = bsp.lump<Model>(kModels);
  const std::vector<Leaf> leafStore = leafsOf(bsp);
  const Leaf *leafs = leafStore.empty() ? nullptr : leafStore.data();
  const size_t leafCount = leafStore.size();
  const auto [dispInfos, dispInfoCount] = bsp.lump<DispInfo>(kDispInfo);
  const auto [dispVerts, dispVertCount] = bsp.lump<DispVert>(kDispVerts);
  if (!planes || !vertexes || !edges || !surfEdges || !texInfos || !texDatas || !stringTable || !strings || !faces ||
      !models || !modelCount || bsp.lumpVersion(kFaces) > 1 || !leafs)
    return false;

  // The 3D skybox is a sealed room: its own BSP area (Leaf::areaFlags low 9
  // bits). Faces referenced by leaves of the sky camera's area, and points
  // inside them, belong to it. (The PVS of the camera's cluster is not
  // enough: skybox geometry spans clusters that cluster does not list.)
  parseSkyCamera(bsp);
  int skyArea = -1;
  if (g_world.skyCamera && leafs) {
    const int leaf = leafAt(bsp, g_world.skyOrigin);
    if (leaf >= 0 && size_t(leaf) < leafCount)
      skyArea = leafs[leaf].areaFlags & 0x1ff;
  }
  g_world.skyArea = skyArea;
  {
    const auto [nodes, nodeCount] = bsp.lump<Node>(kNodes);
    if (nodes)
      g_world.nodes.assign(nodes, nodes + nodeCount);
    g_world.planes.assign(planes, planes + planeCount);
    for (size_t l = 0; leafs && l < leafCount; ++l)
      g_world.leafAreas.push_back(int16_t(leafs[l].areaFlags & 0x1ff));
  }
  std::vector<uint8_t> skyFace(faceCount, 0);
  if (skyArea >= 0) {
    const auto [leafFaces, leafFaceCount] = bsp.lump<uint16_t>(kLeafFaces);
    for (size_t l = 0; leafFaces && l < leafCount; ++l) {
      if ((leafs[l].areaFlags & 0x1ff) != skyArea)
        continue;
      for (uint32_t k = 0; k < leafs[l].numLeafFaces; ++k) {
        const size_t at = size_t(leafs[l].firstLeafFace) + k;
        if (at < leafFaceCount && leafFaces[at] < faceCount)
          skyFace[leafFaces[at]] = 1;
      }
    }
  }
  auto inSky = [&](const float p[3]) {
    if (skyArea < 0)
      return false;
    const int leaf = leafAt(bsp, p);
    return leaf >= 0 && size_t(leaf) < leafCount && (leafs[leaf].areaFlags & 0x1ff) == skyArea;
  };

  // Materials by texdata.
  std::vector<int> materialOf(texDataCount, -2); // -2 unvisited
  auto material = [&](int texData) -> int {
    if (texData < 0 || size_t(texData) >= texDataCount)
      return -1;
    int &slot = materialOf[texData];
    if (slot != -2)
      return slot;
    slot = -1;
    const int32_t nameId = texDatas[texData].nameId;
    if (nameId < 0 || size_t(nameId) >= stringTableCount || size_t(stringTable[nameId]) >= stringsLength)
      return slot;
    const char *name = strings + stringTable[nameId];
    return slot = materialByName(std::string(name, strnlen(name, stringsLength - stringTable[nameId])), true);
  };

  constexpr float kCell = 2048;
  auto groupFor = [&](const float centroid[3], bool sky, int materialIndex) -> Geometry & {
    const auto &m = g_world.materials[materialIndex];
    std::array<int32_t, 6> key = {int32_t(std::floor(centroid[0] / kCell)), int32_t(std::floor(centroid[1] / kCell)),
                                  int32_t(std::floor(centroid[2] / kCell)), sky,
                                  m.alphaTest ? m.alphaRef + 1 : 0, int32_t(m.categories)};
    auto &group = g_world.groups[key];
    if (!group.id) {
      group.sky = sky;
      group.alphaTest = m.alphaTest;
      group.alphaRef = m.alphaRef;
      group.categories = m.categories;
      group.id = XXH3_64bits_withSeed(key.data(), sizeof(key), 0x6273707764ull) | 1;
    }
    return group.surfaces[uint32_t(materialIndex)];
  };
  auto vertex = [&](const float p[3], const float n[3], const TexInfo &ti, const TexData &td) {
    remixapi_HardcodedVertex v = {};
    std::memcpy(v.position, p, 12);
    std::memcpy(v.normal, n, 12);
    const float w = td.width > 0 ? float(td.width) : 1, h = td.height > 0 ? float(td.height) : 1;
    v.texcoord[0] = (dot(ti.textureVecs[0], p) + ti.textureVecs[0][3]) / w;
    v.texcoord[1] = (dot(ti.textureVecs[1], p) + ti.textureVecs[1][3]) / h;
    v.color = 0xffffffff;
    return v;
  };

  const Model &world = models[0];
  for (int32_t f = world.firstFace; f < world.firstFace + world.numFaces && size_t(f) < faceCount; ++f) {
    const Face &face = faces[f];
    if (face.texInfo < 0 || size_t(face.texInfo) >= texInfoCount || face.numEdges < 3 ||
        size_t(face.plane) >= planeCount)
      continue;
    const TexInfo &ti = texInfos[face.texInfo];
    if (ti.flags & (kSurfSky2D | kSurfSky | kSurfNoDraw | kSurfHint | kSurfSkip))
      continue;
    if (ti.flags & kSurfTrans) {
      ++g_world.skippedTranslucent;
      continue;
    }
    const int m = material(ti.texData);
    if (m < 0)
      continue;
    const TexData &td = texDatas[ti.texData];
    float normal[3];
    for (int k = 0; k < 3; ++k)
      normal[k] = planes[face.plane].normal[k] * (face.side ? -1.0f : 1.0f);
    std::vector<std::array<float, 3>> corners;
    for (int e = 0; e < face.numEdges; ++e) {
      const size_t se = size_t(face.firstEdge + e);
      if (se >= surfEdgeCount)
        break;
      const int32_t edge = surfEdges[se];
      const size_t ei = size_t(edge < 0 ? -edge : edge);
      if (ei >= edgeCount)
        break;
      const uint16_t vi = edges[ei][edge < 0 ? 1 : 0];
      if (vi >= vertexCount)
        break;
      corners.push_back(vertexes[vi]);
    }
    if (corners.size() != size_t(face.numEdges))
      continue;
    float centroid[3] = {};
    for (auto &c : corners)
      for (int k = 0; k < 3; ++k)
        centroid[k] += c[k] / float(corners.size());
    // Into the leaf in front of the face.
    const float probe[3] = {centroid[0] + normal[0], centroid[1] + normal[1], centroid[2] + normal[2]};
    // The 3D skybox is left to the engine (l4d2-rtx keeps its draws).
    if (skyFace[size_t(f)] != 0 || inSky(probe)) {
      ++g_world.skyFaces;
      continue;
    }
    const bool sky = false;

    if (face.dispInfo >= 0 && dispInfos && size_t(face.dispInfo) < dispInfoCount && corners.size() == 4) {
      // Displacement: a (2^power+1)^2 grid over the face, offset per vertex;
      // the corners start at the one nearest the displacement's start.
      const DispInfo &di = dispInfos[face.dispInfo];
      int first = 0;
      float best = 1e30f;
      for (int c = 0; c < 4; ++c) {
        float d2 = 0;
        for (int k = 0; k < 3; ++k)
          d2 += (corners[c][k] - di.start[k]) * (corners[c][k] - di.start[k]);
        if (d2 < best) {
          best = d2;
          first = c;
        }
      }
      std::array<float, 3> p[4];
      for (int c = 0; c < 4; ++c)
        p[c] = corners[(first + c) % 4];
      const int size = (1 << di.power) + 1;
      if (di.power < 2 || di.power > 4 || di.firstVert < 0 || size_t(di.firstVert + size * size) > dispVertCount)
        continue;
      Geometry &g = groupFor(centroid, sky, m);
      const uint32_t base = uint32_t(g.vertices.size());
      for (int v = 0; v < size; ++v) {
        const float t = float(v) / float(size - 1);
        float left[3], right[3];
        for (int k = 0; k < 3; ++k) {
          left[k] = p[0][k] + (p[1][k] - p[0][k]) * t;
          right[k] = p[3][k] + (p[2][k] - p[3][k]) * t;
        }
        for (int u = 0; u < size; ++u) {
          const float s = float(u) / float(size - 1);
          const DispVert &dv = dispVerts[di.firstVert + v * size + u];
          float flat[3], pos[3];
          for (int k = 0; k < 3; ++k) {
            flat[k] = left[k] + (right[k] - left[k]) * s;
            pos[k] = flat[k] + dv.vector[k] * dv.dist;
          }
          // Texture coordinates follow the undisplaced surface.
          auto out = vertex(flat, normal, ti, td);
          std::memcpy(out.position, pos, 12);
          std::memset(out.normal, 0, 12);
          g.vertices.push_back(out);
        }
      }
      for (int v = 0; v + 1 < size; ++v)
        for (int u = 0; u + 1 < size; ++u) {
          const uint32_t i = base + uint32_t(v * size + u), right = i + 1, down = i + uint32_t(size),
                         diagonal = down + 1;
          if ((v * size + u) % 2)
            g.indices.insert(g.indices.end(), {i, down, right, right, down, diagonal});
          else
            g.indices.insert(g.indices.end(), {i, down, diagonal, i, diagonal, right});
        }
      // Smooth normals from the triangles, facing the base surface's side.
      for (size_t i = g.indices.size() - size_t(size - 1) * (size - 1) * 6; i + 2 < g.indices.size(); i += 3) {
        auto &a = g.vertices[g.indices[i]], &b = g.vertices[g.indices[i + 1]], &c = g.vertices[g.indices[i + 2]];
        const float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
        const float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        if (dot(n, normal) < 0)
          for (float &x : n)
            x = -x;
        for (auto *vtx : {&a, &b, &c})
          for (int k = 0; k < 3; ++k)
            vtx->normal[k] += n[k];
      }
      for (size_t v = base; v < g.vertices.size(); ++v) {
        float *n = g.vertices[v].normal;
        const float len = std::sqrt(dot(n, n));
        for (int k = 0; k < 3; ++k)
          n[k] = len > 1e-12f ? n[k] / len : normal[k];
      }
      g_world.triangles += uint32_t((size - 1) * (size - 1) * 2);
      ++g_world.displacements;
    } else if (face.dispInfo < 0) {
      Geometry &g = groupFor(centroid, sky, m);
      const uint32_t base = uint32_t(g.vertices.size());
      for (auto &c : corners)
        g.vertices.push_back(vertex(c.data(), normal, ti, td));
      for (uint32_t k = 1; k + 1 < corners.size(); ++k)
        g.indices.insert(g.indices.end(), {base, base + k, base + k + 1});
      g_world.triangles += uint32_t(corners.size() - 2);
      ++g_world.faces;
    }
  }
  loadProps(bsp, inSky);
  loadOverlays(bsp, inSky);
  loadLights(bsp, inSky);
  return !g_world.groups.empty();
}

// The map's potentially visible sets for the runtime's light selection
// (remixapi_SetVisibilityPartitionEXT): BSP nodes, leaf clusters and the
// decompressed cluster PVS rows. OPENREMIX_PROXY_PVS=0 disables it.
struct PartitionStats {
  uint32_t nodes = 0, leaves = 0, clusters = 0;
  int result = -1;
} g_partition;

void sendPartition(const Bsp *bsp) {
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_PVS", v, sizeof(v)) && v[0] == '0');
  }();
  static auto set = reinterpret_cast<PFN_remixapi_SetVisibilityPartitionEXT>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_SetVisibilityPartitionEXT"));
  if (!set)
    return;
  g_partition = {};
  if (!bsp || !enabled) {
    g_partition.result = set(nullptr);
    return;
  }
  const auto [nodes, nodeCount] = bsp->lump<Node>(kNodes);
  const auto [planes, planeCount] = bsp->lump<Plane>(kPlanes);
  const std::vector<Leaf> leafStore = leafsOf(*bsp);
  const Leaf *leafs = leafStore.empty() ? nullptr : leafStore.data();
  const size_t leafCount = leafStore.size();
  const auto [vis, visBytes] = bsp->lump<uint8_t>(kVisibility);
  if (!nodes || !planes || !leafs || !vis || visBytes < 4) {
    g_partition.result = set(nullptr);
    return;
  }
  int32_t clusters = 0;
  std::memcpy(&clusters, vis, 4);
  if (clusters <= 0 || size_t(4 + clusters * 8) > visBytes) {
    g_partition.result = set(nullptr);
    return;
  }
  std::vector<float> nodePlanes(nodeCount * 4);
  std::vector<int32_t> children(nodeCount * 2);
  for (size_t i = 0; i < nodeCount; ++i) {
    const Plane &plane = planes[size_t(nodes[i].plane) < planeCount ? nodes[i].plane : 0];
    std::memcpy(&nodePlanes[i * 4], plane.normal, 12);
    nodePlanes[i * 4 + 3] = plane.dist;
    children[i * 2] = nodes[i].children[0];
    children[i * 2 + 1] = nodes[i].children[1];
  }
  std::vector<int32_t> leafClusters(leafCount);
  for (size_t i = 0; i < leafCount; ++i)
    leafClusters[i] = leafs[i].cluster < clusters ? leafs[i].cluster : -1;
  const uint32_t rowBytes = uint32_t(clusters + 7) / 8;
  std::vector<uint8_t> rows(size_t(clusters) * rowBytes, 0);
  for (int32_t c = 0; c < clusters; ++c) {
    const auto row = pvs(*bsp, c);
    if (row.size() == rowBytes)
      std::memcpy(&rows[size_t(c) * rowBytes], row.data(), rowBytes);
    else
      std::memset(&rows[size_t(c) * rowBytes], 0xff, rowBytes); // unknown: everything visible
  }
  remixapi_VisibilityPartitionEXT info = {};
  info.structSize = sizeof(info);
  info.nodeCount = uint32_t(nodeCount);
  info.planes = nodePlanes.data();
  info.children = children.data();
  info.leafCount = uint32_t(leafCount);
  info.leafClusters = leafClusters.data();
  info.clusterCount = uint32_t(clusters);
  info.rowBytes = rowBytes;
  info.visibility = rows.data();
  g_partition = {uint32_t(nodeCount), uint32_t(leafCount), uint32_t(clusters), int(set(&info))};
}

std::wstring gameRoot() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring root = exe;
  return root.substr(0, root.find_last_of(L"\\/"));
}

} // namespace

bool load(const remixapi_Interface &api, const std::string &requestedMap) {
  std::string mapName = lower(requestedMap);
  if (mapName.rfind("maps/", 0) == 0)
    mapName.erase(0, 5);
  if (mapName.size() > 4 && mapName.compare(mapName.size() - 4, 4, ".bsp") == 0)
    mapName.resize(mapName.size() - 4);
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_WORLD", v, sizeof(v)) && v[0] == '0');
  }();
  if (!mapName.empty() && lower(mapName) == g_world.map && g_active)
    return true;
  publishPropIndex(nullptr);
  destroyMeshes();
  g_world = World{};
  g_world.api = &api;
  g_active = false;
  if (mapName.empty()) {
    sendPartition(nullptr);
    return false;
  }
  static bool initialized = false;
  if (!initialized) {
    fs::init(gameRoot());
    initialized = true;
  }
  Bsp bsp;
  const std::wstring path = fs::locate("maps/" + lower(mapName) + ".bsp");
  if (path.empty() || !bsp.open(path)) {
    sendPartition(nullptr);
    return false;
  }
  sendPartition(&bsp);
  if (!enabled)
    return false;
  const auto [pak, pakBytes] = bsp.lump<uint8_t>(kPakfile);
  fs::setPakfile(pak ? std::vector<uint8_t>(pak, pak + pakBytes) : std::vector<uint8_t>());
  g_world.map = lower(mapName);
  g_active = build(bsp);
  if (g_active && !g_world.props.empty()) {
    auto index = std::make_shared<PropIndex>();
    index->cells = g_world.propCells;
    for (const auto &prop : g_world.props) {
      std::array<float, 12> t;
      std::memcpy(t.data(), prop.transform, sizeof(prop.transform));
      if (prop.model->engineParts)
        t[3] = NAN; // never matches: the engine still draws part of it
      index->transforms.push_back(t);
    }
    publishPropIndex(std::move(index));
  }
  fs::setPakfile({});
  if (!g_active)
    g_world = World{};
  return g_active;
}

bool active() { return g_active; }

bool overlaysActive() { return g_active && !g_world.overlayGroups.empty(); }

bool skyCamera(float origin[3], float &scale) {
  if (!g_active || !g_world.skyCamera || !g_world.skyFaces)
    return false;
  std::memcpy(origin, g_world.skyOrigin, 12);
  scale = g_world.skyScale;
  return true;
}

void submit(MaterialLookup lookup) {
  if (!g_active || !g_world.api)
    return;
  const auto &api = *g_world.api;
  const uint64_t frame = g_world.frames++;
  // Materials resolve once the engine has uploaded their textures (at map
  // load, as a rule): every frame at first, then twice a second.
  if (frame < 600 || frame % 30 == 0)
    for (auto &m : g_world.materials) {
      if (m.resolved)
        continue;
      for (uint64_t h : m.candidates) {
        auto ref = lookup(h);
        if (ref.handle) {
          m.ref = std::move(ref);
          m.resolved = true;
          ++g_world.submitVersion;
          break;
        }
      }
    }
  // Meshes are built when their materials are known (or after a grace
  // period, with the default material), and rebuilt as late ones resolve.
  auto build = [&](auto &group) {
    uint32_t resolved = 0;
    for (auto &[index, geometry] : group.surfaces)
      resolved += g_world.materials[index].resolved;
    if (group.mesh && resolved == group.builtResolved)
      return;
    if (!group.mesh && resolved < group.surfaces.size() && frame < 30)
      return;
    std::vector<remixapi_MeshInfoSurfaceTriangles> surfaces;
    for (auto &[index, geometry] : group.surfaces) {
      remixapi_MeshInfoSurfaceTriangles s = {};
      s.vertices_values = geometry.vertices.data();
      s.vertices_count = geometry.vertices.size();
      s.indices_values = geometry.indices.data();
      s.indices_count = geometry.indices.size();
      s.material = g_world.materials[index].ref.handle;
      surfaces.push_back(s);
    }
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = group.id ^ resolved;
    info.surfaces_values = surfaces.data();
    info.surfaces_count = uint32_t(surfaces.size());
    remixapi_MeshHandle mesh = nullptr;
    if (api.CreateMesh(&info, &mesh) != REMIXAPI_ERROR_CODE_SUCCESS)
      return;
    if (group.mesh)
      api.DestroyMesh(group.mesh);
    group.mesh = mesh;
    group.builtResolved = resolved;
    ++g_world.submitVersion;
  };
  for (auto &[key, group] : g_world.groups)
    build(group);
  for (auto &[key, group] : g_world.overlayGroups)
    build(group);
  // The instances below change rarely: once the submission has been stable
  // for a while it is recorded as a bridge batch (openremix_bridge_*) and
  // replayed with one command per frame instead of thousands of DrawInstance
  // encodings. OPENREMIX_PROXY_BSP_BATCH=0 always submits directly.
  using BeginFn = remixapi_ErrorCode(REMIXAPI_CALL *)(void);
  using BatchFn = remixapi_ErrorCode(REMIXAPI_CALL *)(uint64_t);
  static const HMODULE bridge = GetModuleHandleW(L"remix_bridge.dll");
  static const bool batchEnabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_BATCH", v, sizeof(v)) && v[0] == '0');
  }();
  static const auto beginRecording =
      bridge && batchEnabled ? reinterpret_cast<BeginFn>(GetProcAddress(bridge, "openremix_bridge_BeginRecording")) : nullptr;
  static const auto endRecording =
      bridge ? reinterpret_cast<BatchFn>(GetProcAddress(bridge, "openremix_bridge_EndRecording")) : nullptr;
  static const auto replayBatch =
      bridge ? reinterpret_cast<BatchFn>(GetProcAddress(bridge, "openremix_bridge_ReplayBatch")) : nullptr;
  if (g_world.submitVersion != g_world.stableVersion) {
    g_world.stableVersion = g_world.submitVersion;
    g_world.stableSince = frame;
  }
  if (g_world.batchValid && g_world.batchVersion == g_world.submitVersion && replayBatch &&
      replayBatch(kBatchId) == REMIXAPI_ERROR_CODE_SUCCESS) {
    ++g_world.replayedFrames;
    return;
  }
  g_world.batchValid = false;
  const bool record = beginRecording && endRecording && replayBatch && frame - g_world.stableSince >= 30 &&
                      g_world.batchVersion != g_world.submitVersion &&
                      beginRecording() == REMIXAPI_ERROR_CODE_SUCCESS;
  // Overlays: static decals, blended over the surface below.
  for (auto &[key, group] : g_world.overlayGroups) {
    if (!group.mesh)
      continue;
    remixapi_InstanceInfoBlendEXT blend = {};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaTestCompareOp = 7;
    blend.alphaBlendEnabled = 1;
    // VkBlendFactor: SRC_ALPHA 6, ONE_MINUS_SRC_ALPHA 7, DST_COLOR 4, SRC_COLOR 2.
    blend.srcColorBlendFactor = group.modulate ? 4 : 6;
    blend.dstColorBlendFactor = group.modulate ? 2 : 7;
    blend.srcAlphaBlendFactor = blend.srcColorBlendFactor;
    blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
    blend.writeMask = 15;
    remixapi_InstanceInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    info.pNext = &blend;
    info.mesh = group.mesh;
    info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC | (group.sky ? REMIXAPI_INSTANCE_CATEGORY_BIT_SKY : 0);
    info.transform.matrix[0][0] = info.transform.matrix[1][1] = info.transform.matrix[2][2] = 1;
    info.doubleSided = 1;
    api.DrawInstance(&info);
  }
  for (auto &[key, group] : g_world.groups) {
    if (!group.mesh)
      continue;
    // The material's alpha state comes from the instance (useDrawCallAlphaState).
    remixapi_InstanceInfoBlendEXT blend = {};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaTestEnabled = group.alphaTest;
    blend.alphaTestReferenceValue = group.alphaRef;
    blend.alphaTestCompareOp = group.alphaTest ? 6 : 7; // VK_COMPARE_OP_GREATER_OR_EQUAL : ALWAYS
    remixapi_InstanceIdentityEXT identity = {};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.pNext = &blend;
    identity.instanceId = group.id;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    remixapi_InstanceInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    // 3D skybox geometry is drawn by the sky camera, like the engine's.
    info.pNext = group.sky ? static_cast<void *>(&blend) : static_cast<void *>(&identity);
    info.mesh = group.mesh;
    info.categoryFlags = group.categories | (group.sky ? REMIXAPI_INSTANCE_CATEGORY_BIT_SKY : 0);
    info.transform.matrix[0][0] = info.transform.matrix[1][1] = info.transform.matrix[2][2] = 1;
    info.doubleSided = 1;
    api.DrawInstance(&info);
  }
  for (const auto &prop : g_world.props) {
    if (prop.skybox)
      continue;
    for (const auto &part : prop.model->parts) {
      const auto &m = g_world.materials[part.material];
      remixapi_InstanceInfoMaterialOverrideEXT material = {};
      material.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_OVERRIDE_EXT;
      material.material = m.ref.handle;
      remixapi_InstanceInfoBlendEXT blend = {};
      blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      blend.pNext = m.ref.handle ? &material : nullptr;
      blend.alphaTestEnabled = m.alphaTest;
      blend.alphaTestReferenceValue = m.alphaRef;
      blend.alphaTestCompareOp = m.alphaTest ? 6 : 7;
      remixapi_InstanceInfoReplacementHashEXT replacement = {};
      replacement.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_REPLACEMENT_HASH_EXT;
      replacement.pNext = &blend;
      replacement.meshHash = part.replacementHash;
      remixapi_InstanceIdentityEXT identity = {};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.pNext = &replacement;
      identity.instanceId = prop.id ^ part.replacementHash;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      identity.rasterVisible = 1;
      remixapi_InstanceInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      info.pNext = &identity;
      info.mesh = part.mesh;
      info.categoryFlags = m.categories;
      std::memcpy(info.transform.matrix, prop.transform, sizeof(info.transform.matrix));
      info.doubleSided = 1;
      api.DrawInstance(&info);
    }
  }
  if (record) {
    // Attempted once per version: a failed recording is not retried.
    g_world.batchVersion = g_world.submitVersion;
    g_world.batchValid = endRecording(kBatchId) == REMIXAPI_ERROR_CODE_SUCCESS;
    ++g_world.recordings;
  }
}

int areaAt(const float p[3]) {
  int index = 0;
  for (int guard = 0; index >= 0 && guard < 4096; ++guard) {
    if (size_t(index) >= g_world.nodes.size() || size_t(g_world.nodes[index].plane) >= g_world.planes.size())
      return -1;
    const auto &plane = g_world.planes[g_world.nodes[index].plane];
    index = g_world.nodes[index].children[dot(plane.normal, p) - plane.dist >= 0 ? 0 : 1];
  }
  const int leaf = index < 0 ? -1 - index : -1;
  return leaf >= 0 && size_t(leaf) < g_world.leafAreas.size() ? g_world.leafAreas[leaf] : -1;
}

bool isStaticProp(const float *modelRows, const float eye[3]) {
  if (!g_active || g_world.props.empty())
    return false;
  // A view from inside the 3D skybox (its camera's area): the engine draws
  // the skybox's props; a placement it draws there is a skybox prop.
  // The eye's area changes only when the camera moves: one tree walk per view.
  static float lastEye[3] = {NAN, NAN, NAN};
  static bool lastSkyView = false;
  if (std::memcmp(lastEye, eye, sizeof(lastEye)) != 0) {
    std::memcpy(lastEye, eye, sizeof(lastEye));
    lastSkyView = g_world.skyArea >= 0 && areaAt(eye) == g_world.skyArea;
  }
  const bool skyView = lastSkyView;
  // A model is drawn as several consecutive draws with the same placement:
  // the last answer stands while placement, view and the props' skybox flags
  // are unchanged.
  static float lastRows[12] = {NAN};
  static bool lastSkyViewForRows = false, lastResult = false;
  static uint64_t lastVersion = ~0ull;
  if (std::memcmp(lastRows, modelRows, sizeof(lastRows)) == 0 && lastSkyViewForRows == skyView &&
      lastVersion == g_world.submitVersion)
    return lastResult;
  auto remember = [&](bool result) {
    std::memcpy(lastRows, modelRows, sizeof(lastRows));
    lastSkyViewForRows = skyView;
    lastVersion = g_world.submitVersion;
    return lastResult = result;
  };
  const float origin[3] = {modelRows[3], modelRows[7], modelRows[11]};
  // The origin's cell, and its neighbors only within the match tolerance of
  // a cell boundary.
  bool nearEdge = false;
  for (float v : origin) {
    const float f = v / 64 - std::floor(v / 64);
    nearEdge |= f * 64 < .5f || f * 64 > 63.5f;
  }
  uint64_t checked[8];
  int n = 0;
  for (int corner = 0; corner < (nearEdge ? 8 : 1); ++corner) {
    const float probe[3] = {origin[0] + (corner & 1 ? .5f : -.5f), origin[1] + (corner & 2 ? .5f : -.5f),
                            origin[2] + (corner & 4 ? .5f : -.5f)};
    const uint64_t cell = nearEdge ? propCell(probe) : propCell(origin);
    if (std::find(checked, checked + n, cell) != checked + n)
      continue;
    checked[n++] = cell;
    auto it = g_world.propCells.find(cell);
    if (it == g_world.propCells.end())
      continue;
    for (uint32_t index : it->second) {
      const auto &t = g_world.props[index].transform;
      bool same = true;
      for (int r = 0; r < 3 && same; ++r)
        for (int c = 0; c < 4 && same; ++c)
          same = std::fabs(t[r][c] - modelRows[r * 4 + c]) < (c == 3 ? .1f : 2e-3f);
      if (same) {
        if (skyView) {
          if (!g_world.props[index].skybox) {
            g_world.props[index].skybox = true;
            ++g_world.submitVersion;
          }
          return remember(false);
        }
        return remember(!g_world.props[index].skybox);
      }
    }
  }
  return remember(false);
}

const Occluders &occluders() {
  static const Occluders none;
  if (!g_active)
    return none;
  auto &o = g_world.occluders;
  if (g_world.occludersBuilt)
    return o;
  g_world.occludersBuilt = true;
  static uint64_t version = 0;
  o = Occluders{};
  o.version = ++version;
  auto add = [&](const float *positions, size_t vertexCount, size_t stride, const std::vector<uint32_t> &indices,
                 const float (*transform)[4]) {
    const uint32_t base = uint32_t(o.positions.size() / 3);
    for (size_t v = 0; v < vertexCount; ++v) {
      const float *p = positions + v * stride;
      if (transform)
        for (int r = 0; r < 3; ++r)
          o.positions.push_back(transform[r][0] * p[0] + transform[r][1] * p[1] + transform[r][2] * p[2] +
                                transform[r][3]);
      else
        o.positions.insert(o.positions.end(), {p[0], p[1], p[2]});
    }
    for (uint32_t i : indices)
      o.indices.push_back(base + i);
  };
  // Opaque surfaces only: alpha-tested ones (fences, foliage) and water let
  // light through.
  auto opaque = [](int material) {
    const auto &m = g_world.materials[size_t(material)];
    return !m.alphaTest && !m.owned;
  };
  static_assert(sizeof(remixapi_HardcodedVertex) % sizeof(float) == 0);
  for (const auto &[key, group] : g_world.groups) {
    if (group.sky)
      continue;
    for (const auto &[material, geometry] : group.surfaces)
      if (opaque(int(material)))
        add(geometry.vertices.front().position, geometry.vertices.size(),
            sizeof(remixapi_HardcodedVertex) / sizeof(float), geometry.indices, nullptr);
  }
  for (const auto &prop : g_world.props) {
    const float origin[3] = {prop.transform[0][3], prop.transform[1][3], prop.transform[2][3]};
    if (prop.skybox || (g_world.skyArea >= 0 && areaAt(origin) == g_world.skyArea))
      continue;
    for (const auto &part : prop.model->parts)
      if (part.material >= 0 && opaque(part.material) && !part.positions.empty())
        add(part.positions.data(), part.positions.size() / 3, 3, part.indices, prop.transform);
  }
  return o;
}

const std::vector<Light> &lights() {
  static const std::vector<Light> none;
  return g_active ? g_world.lights : none;
}

bool providesLight(int type, const float position[3], const float direction[3]) {
  if (!g_active)
    return false;
  for (const auto &l : g_world.lights) {
    if (l.type != type)
      continue;
    if (type == 2 ? dot(l.direction, direction) > 0.9999f
                  : std::fabs(l.position[0] - position[0]) < .01f && std::fabs(l.position[1] - position[1]) < .01f &&
                        std::fabs(l.position[2] - position[2]) < .01f)
      return true;
  }
  return false;
}

bool providesProp(const float *rows) {
  std::shared_ptr<const PropIndex> index;
  {
    std::lock_guard<std::mutex> lock(g_propIndexLock);
    index = g_propIndex;
  }
  if (!index || !rows)
    return false;
  const float origin[3] = {rows[3], rows[7], rows[11]};
  uint64_t checked[8];
  int n = 0;
  for (int corner = 0; corner < 8; ++corner) {
    const float probe[3] = {origin[0] + (corner & 1 ? .5f : -.5f), origin[1] + (corner & 2 ? .5f : -.5f),
                            origin[2] + (corner & 4 ? .5f : -.5f)};
    const uint64_t cell = propCell(probe);
    if (std::find(checked, checked + n, cell) != checked + n)
      continue;
    checked[n++] = cell;
    auto it = index->cells.find(cell);
    if (it == index->cells.end())
      continue;
    for (uint32_t i : it->second) {
      const auto &t = index->transforms[i];
      bool same = true;
      for (int k = 0; k < 12 && same; ++k)
        same = std::fabs(t[k] - rows[k]) < (k % 4 == 3 ? .1f : 2e-3f);
      if (same)
        return true;
    }
  }
  return false;
}

std::string materialName(uint64_t textureHash) {
  for (const auto &m : g_world.materials)
    for (uint64_t h : m.candidates)
      if (h == textureHash)
        return m.name;
  return {};
}

void dumpStatistics(FILE *out) {
  if (g_partition.result >= 0 || g_partition.clusters)
    fprintf(out, "   visibility partition: %u nodes, %u leaves, %u clusters -> %d\n", g_partition.nodes,
            g_partition.leaves, g_partition.clusters, g_partition.result);
  if (!g_active) {
    fprintf(out, "   BSP world: inactive\n");
    return;
  }
  uint32_t resolved = 0, meshes = 0;
  for (auto &m : g_world.materials)
    resolved += m.resolved;
  for (auto &[key, group] : g_world.groups)
    meshes += group.mesh != nullptr;
  fprintf(out,
          "   BSP world %s: faces %u displacements %u (sky %u) triangles %u | materials %u/%zu resolved | groups %zu "
          "(meshes %u) | left to the engine: translucent %u, ignored %u | rtx.conf-ignored brush materials kept %u | sky camera %d\n"
          "   BSP static props: %zu placements (%u in the 3D skybox left to the engine), %zu models (%u failed), %u parts | overlays %u "
          "(%u triangles, %zu groups) | water materials %u\n",
          g_world.map.c_str(), g_world.faces, g_world.displacements, g_world.skyFaces, g_world.triangles, resolved,
          g_world.materials.size(), g_world.groups.size(), meshes, g_world.skippedTranslucent, g_world.skippedIgnored, g_world.keptIgnored,
          int(g_world.skyCamera), g_world.props.size(), g_world.skyProps, g_world.propModels.size(),
          g_world.propModelsFailed, g_world.propParts, g_world.overlays, g_world.overlayTriangles,
          g_world.overlayGroups.size(), g_world.waterMaterials);
  fprintf(out, "   BSP world batch: %u recordings, valid %d, replayed frames %llu of %llu\n", g_world.recordings,
          int(g_world.batchValid), (unsigned long long)g_world.replayedFrames, (unsigned long long)g_world.frames);
  fprintf(out, "   BSP lights: %zu static (left to the engine: styled %u, 3D skybox %u, other types %u)\n",
          g_world.lights.size(), g_world.styledLights, g_world.skyboxLights, g_world.otherLights);
  for (auto &path : g_world.failedModels)
    fprintf(out, "     prop model left to the engine: %s\n", path.c_str());
  for (auto &m : g_world.materials)
    if (!m.resolved)
      fprintf(out, "     unresolved material %s (%zu texture hashes)\n", m.name.c_str(), m.candidates.size());
}

} // namespace proxy::bsp
