#include "engine_materials.h"
#include "material_parameters.h"
#include "bsp_world.h"
#include "game.h"
#include "remix_output.h"
#ifndef PROXY_GAME_GMOD
#include "pe_module.h"
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The material system binds a material with IShaderAPI::Bind(IMaterial *)
// (CMatRenderContext::Bind, after the material's bind proxies ran; with a
// queued material system the proxies' writes and the Bind reach the render
// thread in that order). CShaderAPIDx8::Bind flushes the primitives buffered
// for the previous material before it stores the new one, so every D3D draw
// belongs to the material bound last.
//
// GMod (x64): everything below is for one build of materialsystem.dll and of
// the shader API (shaderapidx9_orig.dll). install() requires each function
// used at its RVA with these instruction bytes, the offsets read are the ones
// in those instructions, and per object the vtable must be the class's:
// anything else (another build, CMaterialSubRect, a material var of another
// class) is not read and counts as Lit.
//
//   CMaterial (IMaterial, materialsystem.dll): 0 GetName, 40 ShaderParamCount
//     (movzx eax, byte [rcx+0x32]), 41 GetShaderParams (mov rax, [rcx+0x38]),
//     49 GetShaderName (the shader at [rcx+0x20], its slot 0).
//   CMaterialVar (IMaterialVar): 1 GetName, 6 GetStringValue, 17 IsDefined
//     (type: low nibble of [rcx+0x24], 6 undefined, 3 texture), 26
//     GetIntValueInternal, 27 GetFloatValueInternal, 29 GetVecValueInternal()
//     (-> float[4]). GetStringValue's texture case reads the ITexture at
//     [rcx+0x39] (-1: env_cubemap) and calls its slot 0, GetName.
//     The getters forward to a queue-thread twin ([rcx+0x29]) only while the
//     calling thread's render context has a call queue; on the thread that
//     executes Bind they return the var's own value, as the shaders read it.
//   CMaterialSubRect (a rectangle of an atlas material: the world's bullet
//     holes, blood and scorch marks are "decals/x_subrect" in "decals/
//     decals_mod2x" or "decals/decals_lit"): 0 GetName (its own name symbol at
//     +0x14), 7 InMaterialPage (true), 10 GetMaterialPage (mov rax, [rcx+8]),
//     the CMaterial every other call forwards to. Optional: without it such
//     materials stay unread.
//   CShaderAPIDx8 (IShaderAPI, ShaderApi030): 114 Bind(IMaterial *). Between
//     two sub-rectangles of one page it neither flushes nor stores (they draw
//     as one batch), but the hook still sees every call.
//
// Left 4 Dead 2 (x86, materialsystem.dll and shaderapidx9_orig.dll of build
// 2.2.x, Sep 2026): the same interfaces in an older layout, found instead of
// assumed. The classes' vtables by their RTTI type names, each slot used
// checked by its code (the bytes below, with the offsets they read), so
// another build that moved a field or a slot is refused, not misread.
// Disassembled offline (objdump -d of both modules):
//   CShaderAPIDx8 (IShaderAPIDX8, ShaderApi029; its vtable at +0x21c of the
//     object CreateInterface hands out): 76 Bind(IMaterial *), the slot
//     CMatRenderContext::Bind calls on the material system's shader API after
//     the bind proxies (call [edx+0x130]). It compares the pages of two
//     sub-rectangles (InMaterialPage, GetMaterialPage: slots 7 and 10) and
//     stores m_pMaterial ([this+0x2bd4]) without a flush: L4D2 has no
//     buffered meshes (no CBufferedMeshDX8 class), every draw follows its Bind.
//   CMaterial: 0 GetName (add ecx,0x18: its name symbol; jmp CUtlSymbol::
//     String), 40 ShaderParamCount (movzx eax, byte [ecx+0x22]), 41
//     GetShaderParams (mov eax,[ecx+0x24]), 49 GetShaderName (the shader at
//     [ecx+0x14], its slot 0; no PrecacheVars as in GMod's).
//   CMaterialVar: the GMod slots (1 GetName, 6 GetStringValue, 17 IsDefined,
//     26 GetIntValueInternal, 27 GetFloatValueInternal, 29
//     GetVecValueInternal()); its fields: string +4, int +8, float = vector
//     +0xc, type the low nibble of +0x1c (0x80 there: a fake var), its
//     queue-thread twin's index +0x1d (0xff: none), name symbol +0x1e, ITexture
//     (texture vars) +0x24. The getters forward to the twin only while the
//     calling thread's render context has a call queue (CMaterialSystem slot
//     137: the thread's render context's slot 221, null in CMatRenderContext),
//     as in GMod.
//   CMaterialSubRect: 0 GetName (its name symbol at +0x10), 7 InMaterialPage
//     (true), 10 GetMaterialPage (mov eax,[ecx+4]).
//   MaterialVarFlags_t (materialsystem's table of the $flags names, by bit):
//     SDK 2013's, $model at bit 11.
//
// Half-Life 2 RTX (x86, materialsystem.dll and shaderapidx9_orig.dll of the
// Portal RTX engine build, Nov 2026): found and checked as in L4D2, with
// L4D2's field offsets but SDK 2013's code:
//   CShaderAPIDx8 (IShaderAPIDX8, ShaderApi030; vtable at +0x22c): 113
//     Bind(IMaterial *). It compares the pages of two sub-rectangles as L4D2's
//     does, then flushes the buffered primitives (FlushBufferedPrimitives,
//     slot 114: call [eax+0x1c8]) before it stores m_pMaterial
//     ([this+0xde34]), as GMod's: the draws flushed belong to the previous
//     material, which the hook (after the original) still has.
//   CMaterial: L4D2's slots and fields; GetShaderName calls PrecacheVars
//     (slot 63) first, as GMod's.
//   CMaterialVar: L4D2's slots and fields; the getters ask CMaterialSystem
//     slot 139 (call [eax+0x22c]) for the thread's call queue.
//   CMaterialSubRect: its name symbol at +0xc, the page at +4.
//   MaterialVarFlags_t: SDK 2013's.
//
// Team Fortress 2 (x64, bin/x64 materialsystem.dll and shaderapidx9_orig.dll
// of Nov 2026): found and checked as in L4D2 (the classes by their x64 RTTI),
// GMod's slots with a layout of its own:
//   CShaderAPIDx8 (ShaderApi030, its IShaderAPIDX8 subobject at +0x388): 114
//     Bind(IMaterial *), as GMod's: compares the pages of two sub-rectangles,
//     then flushes (slot 115: call [rax+0x398]) before it stores m_pMaterial
//     ([this+0x2df0]).
//   CMaterial: 0 GetName (add rcx,0x20: its name symbol), 40 ShaderParamCount
//     (movzx eax, byte [rcx+0x2a]), 41 GetShaderParams (mov rax,[rcx+0x50]),
//     49 GetShaderName (PrecacheVars, slot 63, first; the shader at
//     [rcx+0x18]).
//   CMaterialVar: GMod's slots; name symbol +0x26, type the low nibble of
//     +0x24 (as GMod), string +8, int +0x10, float = vector +0x14, ITexture
//     (texture vars) +0x30. The getters ask CMaterialSystem slot 148 (call
//     [rax+0x4a0]) for the thread's call queue and while there is one forward
//     to a twin in a static array (index byte +0x25). GetStringValue's jump
//     table holds RVAs.
//   CMaterialSubRect: its name symbol at +0x14, the page at +8 (as GMod).
//   MaterialVarFlags_t: SDK 2013's ($model bit 11, checked in its table).
//
// Portal 2 (x86, materialsystem.dll and shaderapidx9_orig.dll of Jun 2026):
// L4D2's code and fields, found and checked as there, with these changes:
//   CShaderAPIDx8 (IShaderAPIDX8, ShaderApi029; vtable at +0x248): 80
//     Bind(IMaterial *) (CMatRenderContext::Bind calls [edx+0x140]), L4D2's
//     code with m_pMaterial at [this+0x2c00] (no buffered meshes either).
//   CMaterial, CMaterialSubRect: L4D2's slots, fields and code.
//   CMaterialVar: one slot more at 1 (a second texture getter beside
//     GetTextureValue, 0), so GetName 2, GetStringValue 7, IsDefined 18,
//     GetIntValueInternal 27, GetFloatValueInternal 28, GetVecValueInternal
//     30; L4D2's fields. GetName looks its symbol up under a lock (other code,
//     the symbol at +0x1e still); the getters ask CMaterialSystem slot 162
//     (call [eax+0x288]) for the thread's call queue.
//   MaterialVarFlags_t: L4D2's ($model at bit 11; bit 9 $pseudotranslucent).
namespace proxy::engine_materials {
namespace {

#ifdef PROXY_GAME_GMOD
constexpr uintptr_t kMaterialVtable = 0xadf08, kVarVtable = 0xb3260;
constexpr unsigned kSlotGetName = 0, kSlotParamCount = 40, kSlotParams = 41, kSlotShaderName = 49;
constexpr unsigned kVarGetName = 1, kVarGetString = 6, kVarIsDefined = 17, kVarGetInt = 26, kVarGetFloat = 27,
                   kVarGetVec = 29;
constexpr uintptr_t kGetName = 0xe7e0, kParamCount = 0x11cd0, kParams = 0xf020, kShaderName = 0xefd0;
constexpr uintptr_t kVarName = 0x26b10, kVarString = 0x26ba0, kVarDefined = 0x27080, kVarInt = 0x26840,
                    kVarFloat = 0x26720, kVarVec = 0x26f60;
constexpr uintptr_t kSubRectVtable = 0xaf050, kSubRectName = 0x15420, kSubRectInPage = 0xfe50, kSubRectPage = 0x12980;
constexpr unsigned kSlotInPage = 7, kSlotPage = 10;
constexpr size_t kSubRectPageOffset = 0x8, kSubRectNameOffset = 0x14;
constexpr unsigned kSlotBind = 114;
constexpr uintptr_t kBind = 0x11540;

// CMaterial: m_VarCount, m_pShaderParams, m_Name (CUtlSymbol), m_pShader.
constexpr size_t kMaterialCount = 0x32, kMaterialParams = 0x38, kMaterialName = 0x28, kMaterialShader = 0x20;
// CMaterialVar: type nibble, ITexture * of a texture var.
constexpr size_t kVarType = 0x24, kVarTexture = 0x39;
#else
// L4D2, Half-Life 2 RTX, TF2, Portal 2: as the code checked at install reads
// them (see above).
constexpr unsigned kSlotGetName = 0, kSlotParamCount = 40, kSlotParams = 41, kSlotShaderName = 49;
#ifdef PROXY_GAME_PORTAL2
constexpr unsigned kVarGetName = 2, kVarGetString = 7, kVarIsDefined = 18, kVarGetInt = 27, kVarGetFloat = 28,
                   kVarGetVec = 30;
#else
constexpr unsigned kVarGetName = 1, kVarGetString = 6, kVarIsDefined = 17, kVarGetInt = 26, kVarGetFloat = 27,
                   kVarGetVec = 29;
#endif
constexpr unsigned kSlotInPage = 7, kSlotPage = 10;
constexpr unsigned kSlotBind = game::kSlotBind;
#ifdef PROXY_GAME_TF2
constexpr size_t kSubRectPageOffset = 0x8, kSubRectNameOffset = 0x14;
constexpr size_t kMaterialCount = 0x2a, kMaterialParams = 0x50, kMaterialName = 0x20, kMaterialShader = 0x18;
constexpr size_t kVarType = 0x24, kVarTexture = 0x30;
#else
#if defined(PROXY_GAME_L4D2) || defined(PROXY_GAME_PORTAL2)
constexpr size_t kSubRectPageOffset = 0x4, kSubRectNameOffset = 0x10;
#else
constexpr size_t kSubRectPageOffset = 0x4, kSubRectNameOffset = 0xc;
#endif
constexpr size_t kMaterialCount = 0x22, kMaterialParams = 0x24, kMaterialName = 0x18, kMaterialShader = 0x14;
constexpr size_t kVarType = 0x1c, kVarTexture = 0x24;
#endif
#endif
// MaterialVarType_t: float 0, string 1, vector 2, texture 3, int 4, fourcc 5, undefined 6, matrix 7, material 8.
constexpr uint8_t kTypeFloat = 0, kTypeString = 1, kTypeVector = 2, kTypeTexture = 3, kTypeInt = 4, kTypeUndefined = 6;
// MaterialVarFlags_t.
// $model is bit 11 in both games (each materialsystem's table of the $flags
// names; bit 2 is $no_draw).
constexpr int kFlagModel = 1 << 11;
constexpr int kFlagVertexColor = 1 << 4, kFlagVertexAlpha = 1 << 5, kFlagSelfIllum = 1 << 6, kFlagAdditive = 1 << 7,
              kFlagAlphaTest = 1 << 8, kFlagDecal = 1 << 16, kFlagEnvmapSphere = 1 << 17,
              kFlagBaseAlphaEnvmapMask = 1 << 20, kFlagTranslucent = 1 << 21, kFlagNormalAlphaEnvmapMask = 1 << 22,
              kFlagEnvmapMode = 1 << 25;

#ifdef PROXY_GAME_GMOD
struct Expected {
  bool shaderApi; // else materialsystem.dll
  uintptr_t rva;
  const char *bytes; // hex, space separated
  const char *what;
};
constexpr Expected kExpected[] = {
    // mov rsi,rcx; mov rdi,rdx; mov rcx,[rcx+0x2d10] (m_pMaterial); ... InMaterialPage on both
    {true, 0x11540,
     "48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B FA 48 8B 89 10 2D 00 00 48 85 C9 74 47 48 85 D2 74 42 48 8B 01 FF "
     "50 38",
     "Bind entry"},
    // mov rax,[rsi]; mov rcx,rsi; call [rax+0x398] (FlushBufferedPrimitives, slot 115); mov [rsi+0x2d10],rdi
    {true, 0x115d0, "48 8B 06 48 8B CE FF 90 98 03 00 00 48 89 BE 10 2D 00 00", "Bind flushes, then stores"},
    // add rcx,0x28; jmp CUtlSymbol::String
    {false, kGetName, "48 83 C1 28 E9", "CMaterial::GetName"},
    {false, kParamCount, "0F B6 41 32 C3", "CMaterial::ShaderParamCount"},
    {false, kParams, "48 8B 41 38 C3", "CMaterial::GetShaderParams"},
    // PrecacheVars (returns at once on a precached material); mov rcx,[rbx+0x20]; ... jmp [rax] / "shader not loaded"
    {false, kShaderName,
     "40 53 48 83 EC 30 48 8B 01 45 33 C9 45 33 C0 C7 44 24 20 00 00 00 00 33 D2 48 8B D9 FF 90 00 02 00 00 48 8B 4B "
     "20 48 85 C9 74 0B 48 8B 01 48 83 C4 30 5B 48 FF 20",
     "CMaterial::GetShaderName"},
    // movzx ebx, word [rcx+0x26] (the name symbol); 0xffff: none
    {false, kVarName, "40 53 48 83 EC 20 0F B7 59 26 B8 FF FF 00 00 66 3B D8", "CMaterialVar::GetName"},
    {false, kVarDefined, "0F B6 41 24 24 0F 3C 06 0F 95 C0 C3", "CMaterialVar::IsDefined"},
    // ...; mov eax,[rbx+0x10]; add rsp,0x20; pop rbx; ret
    {false, kVarInt,
     "40 53 48 83 EC 20 48 8B D9 48 8B 0D 00 48 0B 00 48 8B 01 FF 90 70 04 00 00 48 85 C0 74 18 48 8B 4B 29 48 85 C9 "
     "74 0F 48 8B 01 48 83 C4 20 5B 48 FF A0 D0 00 00 00 8B 43 10 48 83 C4 20 5B C3",
     "CMaterialVar::GetIntValueInternal"},
    // ...; movss xmm0,[rbx+0x14]; ...
    {false, kVarFloat,
     "40 53 48 83 EC 20 48 8B D9 48 8B 0D 20 49 0B 00 48 8B 01 FF 90 70 04 00 00 48 85 C0 74 18 48 8B 4B 29 48 85 C9 "
     "74 0F 48 8B 01 48 83 C4 20 5B 48 FF A0 D8 00 00 00 F3 0F 10 43 14 48 83 C4 20 5B C3",
     "CMaterialVar::GetFloatValueInternal"},
    // ...; lea rax,[rbx+0x14]; ...
    {false, kVarVec,
     "40 53 48 83 EC 20 48 8B D9 48 8B 0D E0 40 0B 00 48 8B 01 FF 90 70 04 00 00 48 85 C0 74 18 48 8B 4B 29 48 85 C9 "
     "74 0F 48 8B 01 48 83 C4 20 5B 48 FF A0 E8 00 00 00 48 8D 43 14 48 83 C4 20 5B C3",
     "CMaterialVar::GetVecValueInternal"},
    {false, kVarString, "41 56 48 83 EC 30 4C 8B F1", "CMaterialVar::GetStringValue"},
    // Its jump table entry for type 3 (texture): the case below.
    {false, 0x26e3c, "A4 6D 02 00", "GetStringValue texture case"},
    // mov rcx,[r14+0x39]; cmp rcx,-1; jne +0x12
    {false, 0x26da4, "49 8B 4E 39 48 83 F9 FF 75 12", "texture var's ITexture"},
    // mov rax,[rcx]; call [rax]
    {false, 0x26dc0, "48 8B 01 FF 10", "ITexture::GetName call"},
};
#endif

struct Module {
  uintptr_t base = 0, size = 0;
  bool open(HMODULE module) {
    if (!module)
      return false;
    base = reinterpret_cast<uintptr_t>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
      return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC)
      return false;
    size = nt->OptionalHeader.SizeOfImage;
    return true;
  }
  bool inside(uintptr_t address, size_t bytes) const {
    return address >= base && bytes <= size && address - base <= size - bytes;
  }
  bool bytesAt(uintptr_t rva, const char *hex) const {
    for (const char *p = hex; *p; ++rva) {
      char *end = nullptr;
      const unsigned long byte = std::strtoul(p, &end, 16);
      if (end == p || rva >= size || *reinterpret_cast<const uint8_t *>(base + rva) != uint8_t(byte))
        return false;
      for (p = end; *p == ' ';)
        ++p;
    }
    return true;
  }
} g_materialSystem, g_shaderApi;

std::atomic<bool> g_installed{false}, g_subRects{false};
std::atomic<const char *> g_status{"not installed"}, g_detail{""};
// The classes' vtables (addresses), set by install.
uintptr_t g_materialVtable = 0, g_varVtable = 0, g_subRectVtable = 0;

// Which of the lookup's answers reach the proxy (see install).
enum Use : unsigned { kUseBound = 1, kUseName = 2, kUseShading = 4, kUseParams = 8, kUseAll = 15 };
unsigned g_uses = kUseAll;
std::string g_usesText = "all"; // for the statistics
#ifndef PROXY_GAME_GMOD
// OPENREMIX_PROXY_MATERIALS: 0 none, 1 all (the default), else a comma list.
void readUses() {
  static const struct {
    const char *name;
    Use use;
  } kGroups[] = {{"bound", kUseBound}, {"name", kUseName}, {"shading", kUseShading}, {"params", kUseParams}};
  char v[128] = {};
  const DWORD n = GetEnvironmentVariableA("OPENREMIX_PROXY_MATERIALS", v, sizeof(v));
  const std::string text = n && n < sizeof(v) ? v : "1";
  std::string unknown;
  const bool all = text == "1" || _stricmp(text.c_str(), "all") == 0;
  g_uses = all ? unsigned(kUseAll) : 0u;
  for (size_t at = 0; !all && text != "0" && at < text.size();) {
    const size_t end = std::min(text.find(',', at), text.size());
    const std::string item = text.substr(at, end - at);
    at = end + 1;
    bool known = false;
    for (const auto &g : kGroups)
      if (_stricmp(item.c_str(), g.name) == 0) {
        g_uses |= g.use;
        known = true;
      }
    if (!known && !item.empty())
      unknown += " \"" + item + "\"";
  }
  if (g_uses == kUseAll) {
    g_usesText = "all";
  } else if (!g_uses) {
    g_usesText = "none (read and counted only)";
  } else {
    g_usesText.clear();
    for (const auto &g : kGroups)
      if (g_uses & g.use)
        g_usesText += (g_usesText.empty() ? "" : ",") + std::string(g.name);
  }
  g_usesText += " (OPENREMIX_PROXY_MATERIALS=" + text + (unknown.empty() ? "" : "; not known:" + unknown) + ")";
}
#endif

using NameFn = const char *(PROXY_THISCALL *)(void *);
using IntFn = int(PROXY_THISCALL *)(void *);
using FloatFn = float(PROXY_THISCALL *)(void *);
using VecFn = const float *(PROXY_THISCALL *)(void *);
NameFn g_materialName = nullptr, g_shaderName = nullptr, g_varName = nullptr, g_subRectName = nullptr,
       g_varString = nullptr;
IntFn g_varInt = nullptr;
FloatFn g_varFloat = nullptr;
VecFn g_varVec = nullptr;

std::string lower(const char *text) {
  std::string out = text ? text : "";
  for (char &c : out)
    c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// What is known of a material, while it keeps this identity (a reloaded
// material gets new vars; a freed one's address may be reused).
struct Entry {
  void **params = nullptr;
  void *shader = nullptr;
  uint16_t nameSymbol = 0;
  uint8_t count = 0;
  mutable ParameterNames parameterNames;
  Shading::Mode kind = Shading::Lit; // by shader: Lit (nothing to read), Unlit, SelfIllumBaseAlpha (capable)
  // Indices into the shader params (-1: the shader has no such parameter).
  int16_t flags = -1, color = -1, color2 = -1, tint = -1, mask = -1, maskScale = -1, envmapMask = -1,
          envmapMaskAlpha = -1, fresnel = -1, fresnelRange = -1, base = -1;
  // PBR inputs (pbr_rules), of the lit surface shaders.
  int16_t bump = -1, ssbump = -1, phong = -1, phongExponent = -1, phongExponentTexture = -1, baseAlphaPhongMask = -1,
          envmap = -1, envmapTint = -1, phongAlbedoTint = -1, blendTintByBaseAlpha = -1, phongBoost = -1,
          surfaceProp = -1;
  // ExoPBR.
  int16_t tex1 = -1, tex2 = -1, mrao = -1, mraoScale = -1, normal = -1;
  int16_t model = -1;
  bool isExoPbr = false;
  bool pbrCapable = false;
  std::string name, shaderName;
  bool decalShader = false, modulate = false, lightmapped = false; // by shader name
  // A texture var's name, by the ITexture last seen in it.
  struct TextureName {
    void *texture = nullptr;
    std::string name;
  } maskTexture, baseTexture, bumpTexture, exponentTexture, envmapMaskTexture,
    tex1Texture, tex2Texture, mraoTexture, normalTexture;
};
std::unordered_map<void *, Entry> g_entries; // render thread
Shading g_current;                           // render thread
const Entry *g_currentEntry = nullptr;       // render thread
Bound g_bound;                               // render thread
// Sub-rectangle materials' own names, while they keep their name symbol.
struct SubRectName {
  uint16_t symbol = 0;
  std::string name;
};
std::unordered_map<void *, SubRectName> g_subRectNames; // render thread
const char *g_currentName = "";                         // render thread

struct Seen {
  std::string shader;
  Shading::Mode mode = Shading::Lit;
  float tint[3] = {};
  float maskScale = 1;
  std::string mask;
  uint32_t binds = 0;
};
struct Statistics {
  std::atomic<uint32_t> binds{0}, unlit{0}, baseAlpha{0}, mask{0}, maskAlpha{0};
  std::atomic<uint32_t> otherClass{0}, noParams{0}, foreignVar{0}, noMaskTexture{0}, subRects{0}, subRectNoPage{0},
      decals{0};
  std::mutex lock;
  std::map<std::string, uint32_t> shaders; // distinct materials by shader name, since launch
  std::map<std::string, Seen> seen;        // unlit and self-illuminated materials bound this interval
  size_t entries = 0;
} g_stats;

uint8_t varType(void *var) { return *(static_cast<const uint8_t *>(var) + kVarType) & 0xf; }

// A param the engine's getters can be called on.
void *paramAt(const Entry &e, int index) {
  if (index < 0 || index >= e.count)
    return nullptr;
  void *var = e.params[index];
  if (!var || *static_cast<uintptr_t *>(var) != g_varVtable) {
    g_stats.foreignVar.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  return var;
}

const std::vector<uint16_t> &namedParameters(const Entry &entry, const char *name) {
  return entry.parameterNames.find(name, entry.count, [&](uint16_t index) -> const char * {
    void *var = paramAt(entry, index);
    return var ? g_varName(var) : nullptr;
  });
}

bool defined(void *var) { return var && varType(var) != kTypeUndefined; }

// The name of the texture a var holds ("" when it holds none).
const std::string &textureName(Entry::TextureName &cache, void *var) {
  static const std::string none;
  if (!var || varType(var) != kTypeTexture)
    return none;
  void *texture;
  std::memcpy(&texture, static_cast<const uint8_t *>(var) + kVarTexture, sizeof(texture));
  if (!texture || texture == reinterpret_cast<void *>(~uintptr_t(0)))
    return none;
  if (texture != cache.texture) {
    // An ITexture of the material system (its vtable is in that module).
    const uintptr_t vtable = *static_cast<uintptr_t *>(texture);
    if (!g_materialSystem.inside(vtable, sizeof(void *)) ||
        !g_materialSystem.inside(*reinterpret_cast<uintptr_t *>(vtable), 1))
      return none;
    cache.name = lower(reinterpret_cast<NameFn>(*reinterpret_cast<void **>(vtable))(texture));
    cache.texture = texture;
  }
  return cache.name;
}

std::string textureNameOfTexture(void *texture) {
  if (!texture || texture == reinterpret_cast<void *>(~uintptr_t(0)) || !g_materialSystem.base)
    return {};
  // An ITexture of the material system (its vtable is in that module).
  const uintptr_t vtable = *static_cast<uintptr_t *>(texture);
  if (!g_materialSystem.inside(vtable, sizeof(void *)) ||
      !g_materialSystem.inside(*reinterpret_cast<uintptr_t *>(vtable), 1))
    return {};
  const char *name = reinterpret_cast<NameFn>(*reinterpret_cast<void **>(vtable))(texture);
  return name ? lower(name) : std::string();
}

void resolve(Entry &e, void *material) {
  e.name = lower(g_materialName(material));
  // Mapbase's game shaders as the stock ones (game::kShaderPrefix).
  e.shaderName = game::stockShaderName(g_shaderName(material));
  e.kind = shaderMode(e.shaderName.c_str());
  {
    // "DecalModulate_dx9", "DecalBaseTimesLightmapAlphaBlendSelfIllum_DX9", ...
    const std::string shaderLower = lower(e.shaderName.c_str());
    e.decalShader = shaderLower.compare(0, 5, "decal") == 0;
    e.modulate = shaderLower.compare(0, 13, "decalmodulate") == 0;
    for (const char *world : {"lightmappedgeneric", "worldvertextransition", "lightmapped_4wayblend", "decalbasetimeslightmap"})
      e.lightmapped |= shaderLower.compare(0, std::strlen(world), world) == 0;
    e.pbrCapable = pbr::capable(shaderLower);
  }
  {
    std::lock_guard<std::mutex> lock(g_stats.lock);
    ++g_stats.shaders[e.shaderName];
  }
  struct Wanted {
    const char *name;
    int16_t Entry::*index;
  };
  static const Wanted wanted[] = {
      {"$flags", &Entry::flags},
      {"$color", &Entry::color},
      {"$color2", &Entry::color2},
      {"$selfillumtint", &Entry::tint},
      {"$selfillummask", &Entry::mask},
      {"$selfillummaskscale", &Entry::maskScale},
      {"$envmapmask", &Entry::envmapMask},
      {"$selfillum_envmapmask_alpha", &Entry::envmapMaskAlpha},
      {"$selfillumfresnel", &Entry::fresnel},
      {"$selfillumfresnelminmaxexp", &Entry::fresnelRange},
      {"$basetexture", &Entry::base},
      {"$bumpmap", &Entry::bump},
      {"$ssbump", &Entry::ssbump},
      {"$phong", &Entry::phong},
      {"$phongexponent", &Entry::phongExponent},
      {"$phongexponenttexture", &Entry::phongExponentTexture},
      {"$basemapalphaphongmask", &Entry::baseAlphaPhongMask},
      {"$envmap", &Entry::envmap},
      {"$envmaptint", &Entry::envmapTint},
      {"$phongalbedotint", &Entry::phongAlbedoTint},
      {"$blendtintbybasealpha", &Entry::blendTintByBaseAlpha},
      {"$phongboost", &Entry::phongBoost},
      {"$surfaceprop", &Entry::surfaceProp},
      {"$mraotexture", &Entry::mrao},
      {"$mraoscale", &Entry::mraoScale},
      {"$normaltexture", &Entry::normal},
      {"$texture1", &Entry::tex1},
      {"$texture2", &Entry::tex2},
      {"$model", &Entry::model},
  };
  for (int i = 0; i < e.count; ++i) {
    void *var = paramAt(e, i);
    const char *name = var ? g_varName(var) : nullptr;
    if (!name || name[0] != '$')
      continue;
    for (const Wanted &w : wanted)
      if (e.*w.index < 0 && _stricmp(name, w.name) == 0)
        e.*w.index = int16_t(i);
  }
  const std::string shaderLower = lower(e.shaderName.c_str());
  e.isExoPbr = pbr::isScreenspacePbr(shaderLower) && (e.tex1 >= 0 || e.tex2 >= 0);
}

void vec3(const Entry &e, int index, float out[3]) {
  void *var = paramAt(e, index);
  if (!defined(var))
    return;
  const float *v = g_varVec(var);
  for (int c = 0; c < 3; ++c)
    if (std::isfinite(v[c]))
      out[c] = v[c];
}

void read(Entry &e, Shading &out) {
  void *flagsVar = paramAt(e, e.flags);
  const int flags = flagsVar ? g_varInt(flagsVar) : 0;
  if (e.kind == Shading::Unlit) {
    out.mode = Shading::Unlit;
    out.vertexColor = (flags & kFlagVertexColor) != 0;
    out.vertexAlpha = (flags & kFlagVertexAlpha) != 0;
    // $color x $color2, gamma values the shaders linearize.
    float color[3] = {1, 1, 1}, color2[3] = {1, 1, 1};
    vec3(e, e.color, color);
    vec3(e, e.color2, color2);
    for (int c = 0; c < 3; ++c)
      out.tint[c] = std::pow(std::max(color[c] * color2[c], 0.0f), 2.2f);
    return;
  }
  // $selfillum_envmapmask_alpha (a float; the shaders have it without
  // $selfillum): 8 x the envmap mask's alpha, overbright above 1/8.
  void *alphaVar = paramAt(e, e.envmapMaskAlpha);
  const bool maskAlpha = !(flags & kFlagSelfIllum) && defined(alphaVar) && g_varFloat(alphaVar) != 0;
  if (!(flags & kFlagSelfIllum) && !maskAlpha)
    return;
  vec3(e, e.tint, out.tint); // linear: the shaders multiply the linear albedo by it
  for (float &c : out.tint)
    c = std::max(c, 0.0f);
  if (maskAlpha) {
    const std::string &name = textureName(e.maskTexture, paramAt(e, e.envmapMask));
    if (name.empty()) {
      g_stats.noMaskTexture.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    out.mode = Shading::SelfIllumMaskAlpha;
    out.mask = name;
    out.maskScale = 8;
    return;
  }
  out.mode = Shading::SelfIllumBaseAlpha;
  if (void *maskVar = paramAt(e, e.mask); defined(maskVar)) {
    const std::string &name = textureName(e.maskTexture, maskVar);
    if (name.empty()) {
      // The shader samples the mask instead of the base alpha: without its
      // texture nothing is known to glow.
      g_stats.noMaskTexture.fetch_add(1, std::memory_order_relaxed);
      out.mode = Shading::Lit;
      return;
    }
    out.mode = Shading::SelfIllumMask;
    out.mask = name;
    if (void *scale = paramAt(e, e.maskScale); defined(scale)) {
      const float value = g_varFloat(scale);
      if (std::isfinite(value) && value > 0)
        out.maskScale = value;
    }
  }
  if (void *fresnel = paramAt(e, e.fresnel); defined(fresnel) && g_varInt(fresnel) != 0) {
    float range[3] = {0, 1, 1};
    vec3(e, e.fresnelRange, range);
    out.maskScale *= fresnelShare(range[0], range[1], range[2]);
    if (!(out.maskScale > 0))
      out.mode = Shading::Lit;
  }
}

// The material's PBR inputs, as its shader reads them now.
void readPbr(Entry &e, int flags, Shading &out) {
  auto number = [&](int index) {
    void *var = paramAt(e, index);
    return defined(var) ? g_varFloat(var) : 0.0f;
  };
  pbr::Inputs in;
  in.shader = lower(e.shaderName.c_str());
  in.isExoPbr = e.isExoPbr;
  in.base = out.base;
  in.bump = textureName(e.bumpTexture, paramAt(e, e.bump));
  if (in.bump.empty()) in.bump = textureName(e.normalTexture, paramAt(e, e.normal));
  in.mrao = textureName(e.mraoTexture, paramAt(e, e.mrao));
  vec3(e, e.mraoScale, in.mraoScale);
  in.envmapMask = textureName(e.envmapMaskTexture, paramAt(e, e.envmapMask));
  in.phongExponentTexture = textureName(e.exponentTexture, paramAt(e, e.phongExponentTexture));

  if (e.isExoPbr) {
    if (e.tex1 >= 0)
      in.arm = textureName(e.tex1Texture, paramAt(e, e.tex1));       // $texture1 = ARM
    if (e.tex2 >= 0 && in.bump.empty())
      in.bump = textureName(e.tex2Texture, paramAt(e, e.tex2));      // $texture2 = Normal
  }

  in.ssbump = number(e.ssbump) != 0;
  in.phong = number(e.phong) != 0;
  in.envmap = defined(paramAt(e, e.envmap));
  in.baseAlphaPhongMask = number(e.baseAlphaPhongMask) != 0;
  in.baseAlphaEnvmapMask = (flags & kFlagBaseAlphaEnvmapMask) != 0;
  in.normalAlphaEnvmapMask = (flags & kFlagNormalAlphaEnvmapMask) != 0;
  in.envmapSphere = (flags & kFlagEnvmapSphere) != 0;
  in.envmapMode = (flags & kFlagEnvmapMode) != 0;
  in.phongExponent = number(e.phongExponent);
  if (defined(paramAt(e, e.envmapTint))) {
    in.hasEnvmapTint = true;
    vec3(e, e.envmapTint, in.envmapTint);
  }
  in.phongAlbedoTint = number(e.phongAlbedoTint) != 0;
  in.blendTintByBaseAlpha = number(e.blendTintByBaseAlpha) != 0;
  if (defined(paramAt(e, e.color2))) {
    in.hasColor2 = true;
    vec3(e, e.color2, in.color2);
  }
  in.mwb = pbr::mwbBase(in.base);
  if (void *boost = paramAt(e, e.phongBoost); defined(boost))
    in.phongBoost = g_varFloat(boost);
  if (void *prop = paramAt(e, e.surfaceProp); defined(prop) && varType(prop) == kTypeString)
    in.surfaceProp = lower(g_varString(prop));
  in.luminance = bsp::textureLuminance;
  out.pbr = pbr::convert(in);
}

#ifndef PROXY_GAME_GMOD
// The code checked, per game (see above): the same functions, as each
// compiler built them.
struct Code {
  const char *bind;           // IShaderAPI::Bind, from its entry
  const char *bindTail;       // ... and at bindTailOffset (null: none)
  size_t bindTailOffset;
  const char *materialName;   // CMaterial::GetName
  const char *paramCount;     // CMaterial::ShaderParamCount
  const char *params;         // CMaterial::GetShaderParams
  const char *shaderName;     // CMaterial::GetShaderName
  const char *varName;        // CMaterialVar::GetName
  const char *varDefined;     // CMaterialVar::IsDefined
  const char *varInt;         // CMaterialVar::GetIntValueInternal
  const char *varFloat;       // CMaterialVar::GetFloatValueInternal
  const char *varVec;         // CMaterialVar::GetVecValueInternal
  const char *varString;      // CMaterialVar::GetStringValue, up to its jump table's address (x64: RVA)
  const char *stringCase;     // ... its string case
  const char *textureCase;    // ... its texture case
  const char *subRectName;    // CMaterialSubRect::GetName
  const char *subRectPage;    // CMaterialSubRect::GetMaterialPage
};
#ifdef PROXY_GAME_L4D2
// The CMaterialVar getters: whether the calling thread's render context has a
// call queue (g_pMaterialSystem, slot 137), and while it does the queue
// thread's twin; then the value (the tail).
#define VAR_GETTER(tail)                                                                                              \
  "56 8B F1 8B 0D ? ? ? ? 8B 01 8B 90 24 02 00 00 FF D2 85 C0 74 2E F6 46 1C 80 75 28 8A 46 1D 3C FF 74 21 8B 0D ? ? " \
  "? ? 0F B6 C0 8D 34 80 8B 01 8B 90 24 02 00 00 8D 34 F5 ? ? ? ? FF D2 85 C0 75 D2 " tail
constexpr Code kCode = {
    // push esi; mov esi,[ebp+8]; push edi; mov edi,ecx; mov ecx,[edi+0x2bd4]
    // (m_pMaterial); ... InMaterialPage (slot 7) on both; ...; GetMaterialPage
    // (slot 10) on both; ...; mov [edi+0x2bd4],esi; ...; ret 4
    "55 8B EC 56 8B 75 08 57 8B F9 8B 8F D4 2B 00 00 85 C9 74 ? 85 F6 74 ? 8B 01 8B 50 1C FF D2 84 C0 74 ? 8B 06 "
    "8B 50 1C 8B CE FF D2 84 C0 74 ? 8B 8F D4 2B 00 00 8B 01 8B 50 28 53 FF D2 8B D8 8B 06 8B 50 28 8B CE FF D2 "
    "3B D8 0F 95 C0 5B 84 C0 74 ? 89 B7 D4 2B 00 00 5F 5E 5D C2 04 00",
    nullptr, 0,
    // add ecx,0x18 (the name symbol); jmp CUtlSymbol::String
    "83 C1 18 E9",
    "0F B6 41 22 C3",
    "8B 41 24 C3",
    // cmp [ecx+0x14],0; je; mov ecx,[ecx+0x14]; mov eax,[ecx]; mov edx,[eax]; jmp edx (IShader::GetName)
    "83 79 14 00 74 09 8B 49 14 8B 01 8B 10 FF E2",
    // push esi; movzx esi, word [ecx+0x1e] (the name symbol); 0xffff: none
    "56 0F B7 71 1E B8 FF FF 00 00 66 3B F0",
    // mov al,[ecx+0x1c]; and al,0xf; xor ecx,ecx; cmp al,6; setne al; ret
    "8A 41 1C 24 0F 33 C9 3C 06 0F 95 C0 C3",
    // ...; mov eax,[esi+8]; pop esi; ret
    VAR_GETTER("8B 46 08 5E C3"),
    // ...; fld dword [esi+0xc]; pop esi; ret
    VAR_GETTER("D9 46 0C 5E C3"),
    // ...; lea eax,[esi+0xc]; pop esi; ret
    VAR_GETTER("8D 46 0C 5E C3"),
    // The twin as above (in ebx); movzx eax, byte [ebx+0x1c]; and eax,0xf;
    // cmp eax,8; ja; jmp [eax*4+table] (by type)
    "55 8B EC 83 EC 08 53 8B D9 8B 0D ? ? ? ? 8B 01 8B 90 24 02 00 00 56 57 FF D2 85 C0 74 2F 90 F6 43 1C 80 75 28 8A "
    "43 1D 3C FF 74 21 8B 0D ? ? ? ? 0F B6 C0 8D 1C 80 8B 01 8B 90 24 02 00 00 8D 1C DD ? ? ? ? FF D2 85 C0 75 D2 0F "
    "B6 43 1C 83 E0 0F 83 F8 08 0F 87 ? ? ? ? FF 24 85",
    // mov eax,[ebx+4]; ...; ret
    "8B 43 04 5F 5E 5B 8B E5 5D C3",
    // cmp [ebx+0x24],-1; jne; ... mov eax,"env_cubemap"; ...; mov ecx,[ebx+0x24]; mov edx,[ecx]; mov eax,[edx]; call eax
    "83 7B 24 FF 75 0C 5F 5E B8 ? ? ? ? 5B 8B E5 5D C3 8B 4B 24 8B 11 8B 02 FF D0",
    // add ecx,0x10 (its name symbol); jmp CUtlSymbol::String
    "83 C1 10 E9",
    // mov eax,[ecx+4] (the page)
    "8B 41 04 C3",
};
#elif defined(PROXY_GAME_PORTAL2)
// Portal 2: L4D2's code; the getters ask slot 162 (call [eax+0x288]).
#define VAR_GETTER(tail)                                                                                              \
  "56 8B F1 8B 0D ? ? ? ? 8B 01 8B 90 88 02 00 00 FF D2 85 C0 74 2E F6 46 1C 80 75 28 8A 46 1D 3C FF 74 21 8B 0D ? ? " \
  "? ? 0F B6 C0 8D 34 80 8B 01 8B 90 88 02 00 00 8D 34 F5 ? ? ? ? FF D2 85 C0 75 D2 " tail
constexpr Code kCode = {
    // L4D2's Bind with m_pMaterial at [edi+0x2c00]: ... InMaterialPage
    // (slot 7) on both; ...; GetMaterialPage (slot 10) on both; ...; mov
    // [edi+0x2c00],esi; ...; ret 4
    "55 8B EC 56 8B 75 08 57 8B F9 8B 8F 00 2C 00 00 85 C9 74 ? 85 F6 74 ? 8B 01 8B 50 1C FF D2 84 C0 74 ? 8B 06 "
    "8B 50 1C 8B CE FF D2 84 C0 74 ? 8B 8F 00 2C 00 00 8B 01 8B 50 28 53 FF D2 8B D8 8B 06 8B 50 28 8B CE FF D2 "
    "3B D8 0F 95 C0 5B 84 C0 74 ? 89 B7 00 2C 00 00 5F 5E 5D C2 04 00",
    nullptr, 0,
    // add ecx,0x18 (the name symbol); jmp CUtlSymbol::String
    "83 C1 18 E9",
    "0F B6 41 22 C3",
    "8B 41 24 C3",
    // cmp [ecx+0x14],0; je; mov ecx,[ecx+0x14]; mov eax,[ecx]; mov edx,[eax]; jmp edx (IShader::GetName)
    "83 79 14 00 74 09 8B 49 14 8B 01 8B 10 FF E2",
    // push ebp; mov ebp,esp; push ecx; movzx eax, word [ecx+0x1e] (the name
    // symbol); mov ecx,0xffff; push esi; cmp ax,cx (0xffff: none)
    "55 8B EC 51 0F B7 41 1E B9 FF FF 00 00 56 66 3B C1",
    // mov al,[ecx+0x1c]; and al,0xf; xor ecx,ecx; cmp al,6; setne al; ret
    "8A 41 1C 24 0F 33 C9 3C 06 0F 95 C0 C3",
    // ...; mov eax,[esi+8]; pop esi; ret
    VAR_GETTER("8B 46 08 5E C3"),
    // ...; fld dword [esi+0xc]; pop esi; ret
    VAR_GETTER("D9 46 0C 5E C3"),
    // ...; lea eax,[esi+0xc]; pop esi; ret
    VAR_GETTER("8D 46 0C 5E C3"),
    // The twin as above (in ebx); movzx eax, byte [ebx+0x1c]; and eax,0xf;
    // cmp eax,8; ja; jmp [eax*4+table] (by type)
    "55 8B EC 83 EC 08 53 8B D9 8B 0D ? ? ? ? 8B 01 8B 90 88 02 00 00 56 57 FF D2 85 C0 74 2F 90 F6 43 1C 80 75 28 8A "
    "43 1D 3C FF 74 21 8B 0D ? ? ? ? 0F B6 C0 8D 1C 80 8B 01 8B 90 88 02 00 00 8D 1C DD ? ? ? ? FF D2 85 C0 75 D2 0F "
    "B6 43 1C 83 E0 0F 83 F8 08 0F 87 ? ? ? ? FF 24 85",
    // mov eax,[ebx+4]; ...; ret
    "8B 43 04 5F 5E 5B 8B E5 5D C3",
    // cmp [ebx+0x24],-1; jne; ... mov eax,"env_cubemap"; ...; mov ecx,[ebx+0x24]; mov edx,[ecx]; mov eax,[edx]; call eax
    "83 7B 24 FF 75 0C 5F 5E B8 ? ? ? ? 5B 8B E5 5D C3 8B 4B 24 8B 11 8B 02 FF D0",
    // add ecx,0x10 (its name symbol); jmp CUtlSymbol::String
    "83 C1 10 E9",
    // mov eax,[ecx+4] (the page)
    "8B 41 04 C3",
};
#elif defined(PROXY_GAME_HL2RTX)
// Half-Life 2 RTX: the getters ask slot 139 (call [eax+0x22c]).
#define VAR_GETTER(tail)                                                                                              \
  "56 8B F1 8B 0D ? ? ? ? 8B 01 FF 90 2C 02 00 00 85 C0 74 2C F6 46 1C 80 75 26 8A 46 1D 3C FF 74 1F 8B 0D ? ? ? ? " \
  "0F B6 C0 8D 04 80 8D 34 C5 ? ? ? ? 8B 01 FF 90 2C 02 00 00 85 C0 75 D4 " tail
constexpr Code kCode = {
    // push ebx; mov ebx,ecx; push edi; mov edi,[ebp+8]; mov ecx,[ebx+0xde34]
    // (m_pMaterial); ... InMaterialPage (slot 7) on both; ...; GetMaterialPage
    // (slot 10) on both; ...; cmp ecx,edi (the same material: nothing to do)
    "55 8B EC 53 8B D9 57 8B 7D 08 8B 8B 34 DE 00 00 85 C9 74 ? 85 FF 74 ? 8B 01 8B 40 1C FF D0 84 C0 74 ? 8B 07 "
    "8B CF 8B 40 1C FF D0 84 C0 74 ? 8B 8B 34 DE 00 00 56 8B 01 FF 50 28 8B 17 8B CF 8B F0 FF 52 28 3B F0 5E 0F "
    "95 C0 EB ? 8B 8B 34 DE 00 00 3B CF 75",
    // mov eax,[ebx]; mov ecx,ebx; call [eax+0x1c8] (FlushBufferedPrimitives,
    // slot 114); mov [ebx+0xde34],edi; pop edi; pop ebx; pop ebp; ret 4
    "8B 03 8B CB FF 90 C8 01 00 00 89 BB 34 DE 00 00 5F 5B 5D C2 04 00", 0x77,
    // add ecx,0x18 (the name symbol); jmp CUtlSymbol::String
    "83 C1 18 E9",
    "0F B6 41 22 C3",
    "8B 41 24 C3",
    // push esi; mov esi,ecx; PrecacheVars(0,0,0,0) (slot 63); mov ecx,[esi+0x14];
    // pop esi; test ecx,ecx; je; mov eax,[ecx]; jmp [eax] (IShader::GetName)
    "56 8B F1 6A 00 6A 00 6A 00 8B 06 6A 00 FF 90 FC 00 00 00 8B 4E 14 5E 85 C9 74 04 8B 01 FF 20",
    // push esi; movzx esi, word [ecx+0x1e] (the name symbol); 0xffff: none
    "56 0F B7 71 1E B8 FF FF 00 00 66 3B F0",
    // mov cl,[ecx+0x1c]; xor eax,eax; and cl,0xf; cmp cl,6; setne al; ret
    "8A 49 1C 33 C0 80 E1 0F 80 F9 06 0F 95 C0 C3",
    // ...; mov eax,[esi+8]; pop esi; ret
    VAR_GETTER("8B 46 08 5E C3"),
    // ...; fld dword [esi+0xc]; pop esi; ret
    VAR_GETTER("D9 46 0C 5E C3"),
    // ...; lea eax,[esi+0xc]; pop esi; ret
    VAR_GETTER("8D 46 0C 5E C3"),
    // The twin as above (in ebx); movzx eax, byte [ebx+0x1c]; and eax,0xf;
    // cmp eax,8; ja; jmp [eax*4+table] (by type)
    "55 8B EC 83 EC 08 53 8B D9 8B 0D ? ? ? ? 56 57 8B 01 FF 90 2C 02 00 00 85 C0 74 2F 8D 49 00 F6 43 1C 80 75 26 8A "
    "43 1D 3C FF 74 1F 8B 0D ? ? ? ? 0F B6 C0 8D 04 80 8D 1C C5 ? ? ? ? 8B 01 FF 90 2C 02 00 00 85 C0 75 D4 0F B6 43 "
    "1C 83 E0 0F 83 F8 08 0F 87 ? ? ? ? FF 24 85",
    // mov eax,[ebx+4]; ...; ret
    "8B 43 04 5F 5E 5B 8B E5 5D C3",
    // mov ecx,[ebx+0x24]; cmp ecx,-1; jne; ... mov eax,"env_cubemap"; ...;
    // mov ecx,[ebx+0x24]; mov eax,[ecx]; call [eax]
    "8B 4B 24 83 F9 FF 75 0F 5F 5E B8 ? ? ? ? 5B 8B E5 5D C3 8B 4B 24 8B 01 FF 10",
    // add ecx,0xc (its name symbol); jmp CUtlSymbol::String
    "83 C1 0C E9",
    // mov eax,[ecx+4] (the page)
    "8B 41 04 C3",
};
#elif defined(PROXY_GAME_TF2)
// TF2 (x64): the getters ask g_pMaterialSystem's slot 148 (call [rax+0x4a0])
// for the thread's call queue; while there is one, a var that is not fake
// (byte [+0x24] >= 0) and belongs to the material system ([+0x38]) forwards
// to its queue-thread twin, in a 64-byte array indexed by byte [+0x25]; then
// the value (the tail, after its je to the same tail).
#define VAR_GETTER(tail)                                                                                              \
  "40 53 48 83 EC 20 48 8B D9 48 8B 0D ? ? ? ? 48 8B 01 FF 90 A0 04 00 00 48 85 C0 74 ? 48 89 7C 24 30 48 8D 3D ? ? " \
  "? ? 66 0F 1F 44 00 00 80 7B 24 00 7C 2D 48 8B 05 ? ? ? ? 48 39 43 38 75 20 0F B6 5B 25 48 8B 0D ? ? ? ? 48 C1 E3 " \
  "06 48 03 DF 48 8B 01 FF 90 A0 04 00 00 48 85 C0 75 CD " tail
constexpr Code kCode = {
    // mov rsi,rcx; mov rdi,rdx; mov rcx,[rcx+0x2df0] (m_pMaterial); ...
    // InMaterialPage (slot 7) on both
    "48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B FA 48 8B 89 F0 2D 00 00 48 85 C9 74 ? 48 85 D2 74 ? 48 8B 01 FF "
    "50 38",
    // mov rax,[rsi]; mov rcx,rsi; call [rax+0x398] (FlushBufferedPrimitives,
    // slot 115); mov [rsi+0x2df0],rdi
    "48 8B 06 48 8B CE FF 90 98 03 00 00 48 89 BE F0 2D 00 00", 0x8d,
    // add rcx,0x20 (the name symbol); jmp CUtlSymbol::String
    "48 83 C1 20 E9",
    "0F B6 41 2A C3",
    "48 8B 41 50 C3",
    // PrecacheVars (slot 63; returns at once on a precached material); mov
    // rcx,[rbx+0x18]; ... jmp [rax] (IShader::GetName)
    "40 53 48 83 EC 30 48 8B 01 45 33 C9 45 33 C0 C7 44 24 20 00 00 00 00 33 D2 48 8B D9 FF 90 F8 01 00 00 48 8B 4B "
    "18 48 85 C9 74 0B 48 8B 01 48 83 C4 30 5B 48 FF 20",
    // movzx ebx, word [rcx+0x26] (the name symbol); 0xffff: none
    "40 53 48 83 EC 20 0F B7 59 26 B8 FF FF 00 00 66 3B D8",
    // movzx eax, byte [rcx+0x24]; and al,0xf; cmp al,6; setne al; ret
    "0F B6 41 24 24 0F 3C 06 0F 95 C0 C3",
    // ...; mov eax,[rbx+0x10]; ...; ret
    VAR_GETTER("8B 43 10 48 8B 7C 24 30 48 83 C4 20 5B C3"),
    // ...; movss xmm0,[rbx+0x14]; ...; ret
    VAR_GETTER("F3 0F 10 43 14 48 8B 7C 24 30 48 83 C4 20 5B C3"),
    // ...; lea rax,[rbx+0x14]; ...; ret
    VAR_GETTER("48 8B 7C 24 30 48 8D 43 14 48 83 C4 20 5B C3"),
    // The twin as above (in rbx); movzx eax, byte [rbx+0x24]; and eax,0xf;
    // cmp eax,8; ja; lea rdx,[image base]; mov ecx,[rdx+rax*4+table] (by
    // type); add rcx,rdx; jmp rcx
    "40 53 57 48 83 EC 28 48 8B D9 48 8B 0D ? ? ? ? 48 8B 01 FF 90 A0 04 00 00 48 85 C0 74 3A 48 8D 3D ? ? ? ? 80 7B "
    "24 00 7C 2D 48 8B 05 ? ? ? ? 48 39 43 38 75 20 0F B6 5B 25 48 8B 0D ? ? ? ? 48 C1 E3 06 48 03 DF 48 8B 01 FF 90 "
    "A0 04 00 00 48 85 C0 75 CD 0F B6 43 24 83 E0 0F 48 89 6C 24 40 48 89 74 24 48 83 F8 08 0F 87 ? ? ? ? 48 8D 15 ? "
    "? ? ? 48 98 8B 8C 82",
    // mov rax,[rbx+8]; ...; ret
    "48 8B 43 08 48 8B 74 24 48 48 8B 6C 24 40 48 83 C4 28 5F 5B C3",
    // mov rcx,[rbx+0x30]; cmp rcx,-1; jne; lea rax,"env_cubemap"; ...; ret;
    // mov rax,[rcx]; call [rax] (ITexture::GetName)
    "48 8B 4B 30 48 83 F9 FF 75 18 48 8D 05 ? ? ? ? 48 8B 74 24 48 48 8B 6C 24 40 48 83 C4 28 5F 5B C3 48 8B 01 FF 10",
    // add rcx,0x14 (its name symbol); jmp CUtlSymbol::String
    "48 83 C1 14 E9",
    // mov rax,[rcx+8] (the page)
    "48 8B 41 08 C3",
};
#elif defined(PROXY_GAME_EZ2)
// Entropy : Zero 2: the getters ask slot 138 (call [eax+0x228]); m_pMaterial is at 0x2b38.
#define VAR_GETTER(tail)                                                                                              \
  "56 8B F1 8B 0D ? ? ? ? 8B 01 FF 90 28 02 00 00 85 C0 74 2C F6 46 1C 80 75 26 8A 46 1D 3C FF 74 1F 8B 0D ? ? ? ? " \
  "0F B6 C0 8D 04 80 8D 34 C5 ? ? ? ? 8B 01 FF 90 28 02 00 00 85 C0 75 D4 " tail
constexpr Code kCode = {
    // push ebx; mov ebx,ecx; push edi; mov edi,[ebp+8]; mov ecx,[ebx+0x2b38]
    // (m_pMaterial); ... InMaterialPage (slot 7) on both; ...; GetMaterialPage
    // (slot 10) on both; ...; cmp ecx,edi (the same material: nothing to do)
    "55 8B EC 53 8B D9 57 8B 7D 08 8B 8B 38 2B 00 00 85 C9 74 ? 85 FF 74 ? 8B 01 8B 40 1C FF D0 84 C0 74 ? 8B 07 "
    "8B CF 8B 40 1C FF D0 84 C0 74 ? 8B 8B 38 2B 00 00 56 8B 01 FF 50 28 8B 17 8B CF 8B F0 FF 52 28 3B F0 5E 0F "
    "95 C0 EB ? 8B 8B 38 2B 00 00 3B CF 75",
    // mov eax,[ebx]; mov ecx,ebx; call [eax+0x1c8] (FlushBufferedPrimitives,
    // slot 114); mov [ebx+0x2b38],edi; pop edi; pop ebx; pop ebp; ret 4
    "8B 03 8B CB FF 90 C8 01 00 00 89 BB 38 2B 00 00 5F 5B 5D C2 04 00", 0x77,
    // add ecx,0x18 (the name symbol); jmp CUtlSymbol::String
    "83 C1 18 E9",
    "0F B6 41 22 C3",
    "8B 41 24 C3",
    // push esi; mov esi,ecx; PrecacheVars(0,0,0,0) (slot 63); mov ecx,[esi+0x14];
    // pop esi; test ecx,ecx; je; mov eax,[ecx]; jmp [eax] (IShader::GetName)
    "56 8B F1 6A 00 6A 00 6A 00 8B 06 6A 00 FF 90 FC 00 00 00 8B 4E 14 5E 85 C9 74 04 8B 01 FF 20",
    // push esi; movzx esi, word [ecx+0x1e] (the name symbol); 0xffff: none
    "56 0F B7 71 1E B8 FF FF 00 00 66 3B F0",
    // mov cl,[ecx+0x1c]; xor eax,eax; and cl,0xf; cmp cl,6; setne al; ret
    "8A 49 1C 33 C0 80 E1 0F 80 F9 06 0F 95 C0 C3",
    // ...; mov eax,[esi+8]; pop esi; ret
    VAR_GETTER("8B 46 08 5E C3"),
    // ...; fld dword [esi+0xc]; pop esi; ret
    VAR_GETTER("D9 46 0C 5E C3"),
    // ...; lea eax,[esi+0xc]; pop esi; ret
    VAR_GETTER("8D 46 0C 5E C3"),
    // The twin as above (in ebx); movzx eax, byte [ebx+0x1c]; and eax,0xf;
    // cmp eax,8; ja; jmp [eax*4+table] (by type)
    "55 8B EC 83 EC 08 53 8B D9 8B 0D ? ? ? ? 56 57 8B 01 FF 90 28 02 00 00 85 C0 74 2F 8D 49 00 F6 43 1C 80 75 26 8A "
    "43 1D 3C FF 74 1F 8B 0D ? ? ? ? 0F B6 C0 8D 04 80 8D 1C C5 ? ? ? ? 8B 01 FF 90 28 02 00 00 85 C0 75 D4 0F B6 43 "
    "1C 83 E0 0F 83 F8 08 0F 87 ? ? ? ? FF 24 85",
    // mov eax,[ebx+4]; ...; ret
    "8B 43 04 5F 5E 5B 8B E5 5D C3",
    // mov ecx,[ebx+0x24]; cmp ecx,-1; jne; ... mov eax,"env_cubemap"; ...;
    // mov ecx,[ebx+0x24]; mov eax,[ecx]; call [eax]
    "8B 4B 24 83 F9 FF 75 0F 5F 5E B8 ? ? ? ? 5B 8B E5 5D C3 8B 4B 24 8B 01 FF 10",
    // add ecx,0xc (its name symbol); jmp CUtlSymbol::String
    "83 C1 0C E9",
    // mov eax,[ecx+4] (the page)
    "8B 41 04 C3",
};
#endif

// The number of bytes a pattern names (the jump table's address follows
// GetStringValue's).
size_t patternBytes(const char *text) {
  size_t n = 0;
  for (const char *p = text; *p;) {
    while (*p == ' ')
      ++p;
    if (!*p)
      break;
    ++n;
    while (*p && *p != ' ')
      ++p;
  }
  return n;
}

// L4D2, Half-Life 2 RTX, TF2: the classes by their RTTI, each function used
// checked against kCode.
bool installFound(const void *const *shaderApiVtable) {
  static pe::Module materials, shaderApi;
  if (!materials.open("materialsystem.dll")) {
    g_status = "no materialsystem.dll";
    return false;
  }
  if (!shaderApi.open("shaderapidx9_orig.dll") || !shaderApi.holds(shaderApiVtable) ||
      !shaderApi.holds(shaderApiVtable + kSlotBind)) {
    g_status = "no shader API module";
    g_detail = "shaderapidx9_orig.dll";
    return false;
  }
  auto code = [](const pe::Module &m, const void *function, const char *bytes, size_t offset = 0) {
    return m.inCode(function) && pe::bytesAt(m, reinterpret_cast<uintptr_t>(function) - m.base + offset, bytes);
  };
  static char bindName[40];
  std::snprintf(bindName, sizeof(bindName), "IShaderAPI::Bind (slot %u)", kSlotBind);
  if (!code(shaderApi, shaderApiVtable[kSlotBind], kCode.bind) ||
      (kCode.bindTail && !code(shaderApi, shaderApiVtable[kSlotBind], kCode.bindTail, kCode.bindTailOffset))) {
    g_status = "code mismatch";
    g_detail = bindName;
    return false;
  }
  void **material = pe::rttiVtable(materials, ".?AVCMaterial@@");
  void **var = pe::rttiVtable(materials, ".?AVCMaterialVar@@");
  if (!material || !var) {
    g_status = "class not found (RTTI)";
    g_detail = material ? "CMaterialVar" : "CMaterial";
    return false;
  }
  struct Slot {
    void **vtable;
    unsigned slot;
    const char *bytes;
    const char *name;
  };
  const Slot slots[] = {
      {material, kSlotGetName, kCode.materialName, "CMaterial::GetName"},
      {material, kSlotParamCount, kCode.paramCount, "CMaterial::ShaderParamCount"},
      {material, kSlotParams, kCode.params, "CMaterial::GetShaderParams"},
      {material, kSlotShaderName, kCode.shaderName, "CMaterial::GetShaderName"},
      {var, kVarGetName, kCode.varName, "CMaterialVar::GetName"},
      {var, kVarIsDefined, kCode.varDefined, "CMaterialVar::IsDefined"},
      {var, kVarGetInt, kCode.varInt, "CMaterialVar::GetIntValueInternal"},
      {var, kVarGetFloat, kCode.varFloat, "CMaterialVar::GetFloatValueInternal"},
      {var, kVarGetVec, kCode.varVec, "CMaterialVar::GetVecValueInternal"},
      {var, kVarGetString, kCode.varString, "CMaterialVar::GetStringValue"},
  };
  for (const Slot &s : slots)
    if (!materials.holds(s.vtable + s.slot) || !code(materials, s.vtable[s.slot], s.bytes)) {
      g_status = "code mismatch";
      g_detail = s.name;
      return false;
    }
  // GetStringValue's jump table (its address in the jump above; x64: an RVA,
  // its entries RVAs too): the string case returns the var's string; the
  // texture case returns "env_cubemap" for the ITexture -1 at kVarTexture,
  // else formats that texture's name (its slot 0).
  {
    const auto *string = static_cast<const uint8_t *>(var[kVarGetString]);
    uint32_t table;
    std::memcpy(&table, string + patternBytes(kCode.varString), sizeof(table));
#ifdef _WIN64
    const auto *entries = materials.at<const uint32_t>(table);
    auto at = [&](unsigned type) -> const void * { return materials.at<const uint8_t>(entries[type]); };
    const bool inside = materials.has(table, (kTypeTexture + 1) * sizeof(uint32_t));
#else
    const void *const *cases = reinterpret_cast<const void *const *>(uintptr_t(table));
    auto at = [&](unsigned type) { return cases[type]; };
    const bool inside = materials.holds(cases) && materials.holds(cases + kTypeTexture);
#endif
    if (!inside || !code(materials, at(kTypeString), kCode.stringCase) ||
        !code(materials, at(kTypeTexture), kCode.textureCase)) {
      g_status = "code mismatch";
      g_detail = "CMaterialVar::GetStringValue's string and texture cases";
      return false;
    }
  }
  g_materialVtable = reinterpret_cast<uintptr_t>(material);
  g_varVtable = reinterpret_cast<uintptr_t>(var);
  g_materialName = reinterpret_cast<NameFn>(material[kSlotGetName]);
  g_shaderName = reinterpret_cast<NameFn>(material[kSlotShaderName]);
  g_varName = reinterpret_cast<NameFn>(var[kVarGetName]);
  g_varString = reinterpret_cast<NameFn>(var[kVarGetString]);
  g_varInt = reinterpret_cast<IntFn>(var[kVarGetInt]);
  g_varFloat = reinterpret_cast<FloatFn>(var[kVarGetFloat]);
  g_varVec = reinterpret_cast<VecFn>(var[kVarGetVec]);
  // Sub-rectangle materials (optional, as in GMod): GetName (its name symbol,
  // CUtlSymbol::String); mov al,1; the page.
  if (void **subRect = pe::rttiVtable(materials, ".?AVCMaterialSubRect@@");
      subRect && materials.holds(subRect + kSlotPage) && code(materials, subRect[kSlotGetName], kCode.subRectName) &&
      code(materials, subRect[kSlotInPage], "B0 01 C3") && code(materials, subRect[kSlotPage], kCode.subRectPage)) {
    g_subRectName = reinterpret_cast<NameFn>(subRect[kSlotGetName]);
    g_subRectVtable = reinterpret_cast<uintptr_t>(subRect);
    g_subRects = true;
  }
  return true;
}
#undef VAR_GETTER
#endif

} // namespace

bool install(const void *const *shaderApiVtable) {
  static std::atomic<bool> done{false};
  if (done.exchange(true))
    return g_installed;
#ifndef PROXY_GAME_GMOD
  readUses();
#endif
  char v[8] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_EMISSIVE", v, sizeof(v)) && v[0] == '0') {
    g_status = "disabled (OPENREMIX_PROXY_EMISSIVE=0)";
    return false;
  }
  if (!g_materialSystem.open(GetModuleHandleA("materialsystem.dll"))) {
    g_status = "no materialsystem.dll";
    return false;
  }
  HMODULE shaderApi = nullptr;
  if (!shaderApiVtable ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(shaderApiVtable), &shaderApi) ||
      !g_shaderApi.open(shaderApi)) {
    g_status = "no shader API module";
    return false;
  }
#ifdef PROXY_GAME_GMOD
  if (!g_shaderApi.inside(reinterpret_cast<uintptr_t>(shaderApiVtable), (kSlotBind + 1) * sizeof(void *)) ||
      reinterpret_cast<uintptr_t>(shaderApiVtable[kSlotBind]) != g_shaderApi.base + kBind) {
    g_status = "slot mismatch";
    g_detail = "IShaderAPI::Bind";
    return false;
  }
  struct Slot {
    uintptr_t vtable;
    unsigned slot;
    uintptr_t rva;
    const char *name;
  };
  const Slot slots[] = {
      {kMaterialVtable, kSlotGetName, kGetName, "CMaterial::GetName"},
      {kMaterialVtable, kSlotParamCount, kParamCount, "CMaterial::ShaderParamCount"},
      {kMaterialVtable, kSlotParams, kParams, "CMaterial::GetShaderParams"},
      {kMaterialVtable, kSlotShaderName, kShaderName, "CMaterial::GetShaderName"},
      {kVarVtable, kVarGetName, kVarName, "CMaterialVar::GetName"},
      {kVarVtable, kVarGetString, kVarString, "CMaterialVar::GetStringValue"},
      {kVarVtable, kVarIsDefined, kVarDefined, "CMaterialVar::IsDefined"},
      {kVarVtable, kVarGetInt, kVarInt, "CMaterialVar::GetIntValueInternal"},
      {kVarVtable, kVarGetFloat, kVarFloat, "CMaterialVar::GetFloatValueInternal"},
      {kVarVtable, kVarGetVec, kVarVec, "CMaterialVar::GetVecValueInternal"},
  };
  for (const Slot &s : slots) {
    const uintptr_t entry = s.vtable + s.slot * sizeof(void *);
    if (entry + sizeof(void *) > g_materialSystem.size ||
        *reinterpret_cast<uintptr_t *>(g_materialSystem.base + entry) != g_materialSystem.base + s.rva) {
      g_status = "slot mismatch";
      g_detail = s.name;
      return false;
    }
  }
  for (const Expected &expected : kExpected)
    if (!(expected.shaderApi ? g_shaderApi : g_materialSystem).bytesAt(expected.rva, expected.bytes)) {
      g_status = "code mismatch";
      g_detail = expected.what;
      return false;
    }
  auto at = [](uintptr_t rva) { return reinterpret_cast<void *>(g_materialSystem.base + rva); };
  g_materialVtable = g_materialSystem.base + kMaterialVtable;
  g_varVtable = g_materialSystem.base + kVarVtable;
  g_materialName = reinterpret_cast<NameFn>(at(kGetName));
  g_shaderName = reinterpret_cast<NameFn>(at(kShaderName));
  g_varName = reinterpret_cast<NameFn>(at(kVarName));
  g_varString = reinterpret_cast<NameFn>(at(kVarString));
  g_varInt = reinterpret_cast<IntFn>(at(kVarInt));
  g_varFloat = reinterpret_cast<FloatFn>(at(kVarFloat));
  g_varVec = reinterpret_cast<VecFn>(at(kVarVec));
  // Sub-rectangle materials: their page is read instead. On another layout
  // they stay unread, as before.
  {
    auto slot = [](unsigned index) {
      const uintptr_t entry = kSubRectVtable + index * sizeof(void *);
      return entry + sizeof(void *) <= g_materialSystem.size
                 ? *reinterpret_cast<uintptr_t *>(g_materialSystem.base + entry) - g_materialSystem.base
                 : 0;
    };
    if (slot(kSlotGetName) == kSubRectName && slot(kSlotInPage) == kSubRectInPage && slot(kSlotPage) == kSubRectPage &&
        g_materialSystem.bytesAt(kSubRectName, "48 83 C1 14 E9") && g_materialSystem.bytesAt(kSubRectInPage, "B0 01 C3") &&
        g_materialSystem.bytesAt(kSubRectPage, "48 8B 41 08 C3")) {
      g_subRectName = reinterpret_cast<NameFn>(at(kSubRectName));
      g_subRectVtable = g_materialSystem.base + kSubRectVtable;
      g_subRects = true;
    }
  }
  g_status = g_subRects ? "ok (IShaderAPI::Bind read, sub-rectangle materials through their page)"
                        : "ok (IShaderAPI::Bind read; CMaterialSubRect layout not recognized: decal atlases unread)";
#else
  if (!installFound(shaderApiVtable))
    return false;
  g_status = g_subRects ? "ok (IShaderAPI::Bind read, classes by RTTI, sub-rectangle materials through their page)"
                        : "ok (IShaderAPI::Bind read, classes by RTTI; CMaterialSubRect not recognized: decal atlases unread)";
#endif
  g_installed = true;
  return true;
}

namespace {
void bindMaterial(void *material) {
  g_current = Shading{};
  g_currentEntry = nullptr;
  g_bound = Bound{};
  g_currentName = "";
  if (!g_installed.load(std::memory_order_relaxed) || !material)
    return;
  g_stats.binds.fetch_add(1, std::memory_order_relaxed);
  // A sub-rectangle of an atlas: its page is the material the draw uses.
  const char *subRectName = nullptr;
  if (g_subRects.load(std::memory_order_relaxed) &&
      *static_cast<uintptr_t *>(material) == g_subRectVtable) {
    void *page;
    uint16_t symbol;
    std::memcpy(&page, static_cast<const uint8_t *>(material) + kSubRectPageOffset, sizeof(page));
    std::memcpy(&symbol, static_cast<const uint8_t *>(material) + kSubRectNameOffset, sizeof(symbol));
    if (!page) {
      g_stats.subRectNoPage.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (g_subRectNames.size() > 4096)
      g_subRectNames.clear();
    SubRectName &own = g_subRectNames[material];
    if (own.name.empty() || own.symbol != symbol) {
      own.symbol = symbol;
      own.name = lower(g_subRectName(material));
    }
    subRectName = own.name.c_str();
    material = page;
    g_stats.subRects.fetch_add(1, std::memory_order_relaxed);
  }
  // CMaterial only (its vars are its own).
  if (*static_cast<uintptr_t *>(material) != g_materialVtable) {
    g_stats.otherClass.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto *bytes = static_cast<const uint8_t *>(material);
  void **params;
  void *shader;
  uint16_t nameSymbol;
  std::memcpy(&params, bytes + kMaterialParams, sizeof(params));
  std::memcpy(&shader, bytes + kMaterialShader, sizeof(shader));
  std::memcpy(&nameSymbol, bytes + kMaterialName, sizeof(nameSymbol));
  const uint8_t count = bytes[kMaterialCount];
  if (!params || !count || !shader) {
    g_stats.noParams.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (g_entries.size() > 32768)
    g_entries.clear();
  Entry &e = g_entries[material];
  if (e.params != params || e.count != count || e.shader != shader || e.nameSymbol != nameSymbol) {
    e = Entry{};
    e.params = params;
    e.count = count;
    e.shader = shader;
    e.nameSymbol = nameSymbol;
    resolve(e, material);
  }
  g_currentEntry = &e;
  g_currentName = subRectName ? subRectName : e.name.c_str();
  {
    void *flagsVar = paramAt(e, e.flags);
    const int flags = flagsVar ? g_varInt(flagsVar) : 0;
    g_bound.known = true;
    g_bound.flags = flags;
    g_bound.decal = (flags & kFlagDecal) != 0;
    g_bound.decalShader = e.decalShader;
    g_bound.modulate = e.modulate;
    g_bound.lightmapped = e.lightmapped;
    g_bound.vertexColor = (flags & kFlagVertexColor) != 0;
    g_bound.vertexAlpha = (flags & kFlagVertexAlpha) != 0;
    g_bound.translucent = (flags & kFlagTranslucent) != 0;
    g_bound.additive = (flags & kFlagAdditive) != 0;
    g_bound.alphaTest = (flags & kFlagAlphaTest) != 0;
    g_bound.isModel = (flags & kFlagModel) != 0 || (e.model >= 0 && defined(paramAt(e, e.model)) && g_varInt(paramAt(e, e.model)) != 0);
    g_bound.subRect = subRectName != nullptr;
    g_bound.shader = e.shaderName.c_str();
    if (g_bound.decal || g_bound.decalShader)
      g_stats.decals.fetch_add(1, std::memory_order_relaxed);
  }
  if (e.kind != Shading::Lit)
    read(e, g_current);
  // Draws are matched to the material by its base texture as well
  // (remix::onMaterialBound): some reach D3D while another one is bound.
  if (e.base >= 0)
    if (void *var = e.params[e.base]; var && *static_cast<uintptr_t *>(var) == g_varVtable)
      g_current.base = textureName(e.baseTexture, var);
  if (e.pbrCapable && pbr::enabled()) {
    void *flagsVar = paramAt(e, e.flags);
    readPbr(e, flagsVar ? g_varInt(flagsVar) : 0, g_current);
  }
  if (g_uses & kUseShading)
  remix::onMaterialBound(g_current, e.name.c_str());
  if (g_current.mode == Shading::Lit)
    return;
  switch (g_current.mode) {
  case Shading::Unlit: g_stats.unlit.fetch_add(1, std::memory_order_relaxed); break;
  case Shading::SelfIllumBaseAlpha: g_stats.baseAlpha.fetch_add(1, std::memory_order_relaxed); break;
  case Shading::SelfIllumMask: g_stats.mask.fetch_add(1, std::memory_order_relaxed); break;
  default: g_stats.maskAlpha.fetch_add(1, std::memory_order_relaxed); break;
  }
  std::lock_guard<std::mutex> lock(g_stats.lock);
  g_stats.entries = g_entries.size();
  if (g_stats.seen.size() < 256 || g_stats.seen.count(e.name)) {
    Seen &seen = g_stats.seen[e.name];
    seen.shader = e.shaderName;
    seen.mode = g_current.mode;
    std::copy_n(g_current.tint, 3, seen.tint);
    seen.maskScale = g_current.maskScale;
    seen.mask = g_current.mask;
    ++seen.binds;
  }
}

// Dev aid: bin/shaderapi_proxy_binds.request (its contents: a count, default
// 64; "params" after it: with each material's defined parameters) describes
// the next distinct materials bound in bin/binds_probe.txt: their class, name
// (and page), shader, $flags, what bound() makes of them and the shading
// read, whatever OPENREMIX_PROXY_MATERIALS lets through. Polled once a second.
struct BindProbe {
  uint32_t left = 0;
  bool params = false;
  std::unordered_set<void *> seen;
  ULONGLONG checked = 0;
  std::wstring dir;
} g_bindProbe; // render thread

void serviceBindProbe() {
  const ULONGLONG now = GetTickCount64();
  if (now - g_bindProbe.checked < 1000)
    return;
  g_bindProbe.checked = now;
  if (g_bindProbe.dir.empty()) {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH] = {};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&serviceBindProbe), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    g_bindProbe.dir = path;
    g_bindProbe.dir.resize(g_bindProbe.dir.find_last_of(L"\\/") + 1);
  }
  const std::wstring request = g_bindProbe.dir + L"shaderapi_proxy_binds.request";
  if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES)
    return;
  char line[64] = {};
  if (FILE *in = _wfopen(request.c_str(), L"r")) {
    if (!std::fgets(line, sizeof(line), in))
      line[0] = 0;
    fclose(in);
  }
  DeleteFileW(request.c_str());
  const int count = std::atoi(line);
  g_bindProbe.left = count > 0 ? uint32_t(count) : 64;
  g_bindProbe.params = std::strstr(line, "params") != nullptr;
  g_bindProbe.seen.clear();
  if (FILE *out = _wfopen((g_bindProbe.dir + L"binds_probe.txt").c_str(), L"w")) {
    fprintf(out, "== the next %u distinct materials bound; answers to the proxy: %s\n", g_bindProbe.left,
            g_usesText.c_str());
    fclose(out);
  }
}

void probeBind(void *material) {
  if (!g_bindProbe.seen.insert(material).second)
    return;
  --g_bindProbe.left;
  FILE *out = _wfopen((g_bindProbe.dir + L"binds_probe.txt").c_str(), L"a");
  if (!out)
    return;
  const uintptr_t vtable = *static_cast<uintptr_t *>(material);
  const char *kind = vtable == g_materialVtable                 ? "CMaterial"
                     : g_subRects && vtable == g_subRectVtable ? "CMaterialSubRect"
                                                                : "other class";
  const Bound &b = g_bound;
  const Shading &c = g_current;
  static const char *const modes[] = {"lit", "unlit", "selfillum base alpha", "selfillum mask", "selfillum envmap mask alpha"};
  fprintf(out, "%s", kind);
  if (*kind == 'o')
    fprintf(out, " (vtable %s+0x%llx)", g_materialSystem.inside(vtable, 1) ? "materialsystem" : "?",
            static_cast<unsigned long long>(g_materialSystem.inside(vtable, 1) ? vtable - g_materialSystem.base : vtable));
  if (!b.known) {
    fprintf(out, " %p: not read\n", material);
    fclose(out);
    return;
  }
  fprintf(out,
          " %s%s%s | shader %s flags %08X:%s%s%s%s%s%s%s%s%s%s | %s base \"%s\" tint %.2f %.2f %.2f",
          g_currentName, b.subRect ? " in " : "", b.subRect && g_currentEntry ? g_currentEntry->name.c_str() : "",
          b.shader, unsigned(b.flags), b.decal ? " decal" : "", b.decalShader ? " decal-shader" : "",
          b.modulate ? " modulate" : "", b.lightmapped ? " lightmapped" : "", b.vertexColor ? " vertexcolor" : "",
          b.vertexAlpha ? " vertexalpha" : "", b.translucent ? " translucent" : "", b.additive ? " additive" : "",
          b.alphaTest ? " alphatest" : "", b.isModel ? " model" : "", modes[c.mode], c.base.c_str(), c.tint[0],
          c.tint[1], c.tint[2]);
  if (!c.mask.empty())
    fprintf(out, " mask %s x %.2f", c.mask.c_str(), c.maskScale);
  fprintf(out, " pbr %d\n", int(c.pbr.active));
  if (g_bindProbe.params)
    describeCurrent(out, "    ");
  fclose(out);
}
} // namespace

void onBind(void *material) {
  bindMaterial(material);
  if (!g_installed.load(std::memory_order_relaxed) || !material)
    return;
  serviceBindProbe();
  if (g_bindProbe.left)
    probeBind(material);
}

// The answers to the proxy (see install): what was read, or "not known".
const Shading &current() {
  static const Shading none;
  return g_uses & kUseShading ? g_current : none;
}

const char *currentName() { return g_uses & kUseName ? g_currentName : ""; }

const Bound &bound() {
  static const Bound none;
  return g_uses & kUseBound ? g_bound : none;
}

const Shading &currentRead() { return g_current; }
const char *nameRead() { return g_currentName; }
const Bound &boundRead() { return g_bound; }

bool currentIsLitOpaque() {
  const Entry *e = g_uses & kUseBound ? g_currentEntry : nullptr;
  if (!e)
    return false;
  bool lit = false;
  for (const char *shader : {"lightmappedgeneric", "vertexlitgeneric", "worldvertextransition", "lightmapped_4wayblend", "screenspace_general"})
    lit |= _strnicmp(e->shaderName.c_str(), shader, std::strlen(shader)) == 0;
  if (!lit)
    return false;
  void *flags = paramAt(*e, e->flags);
  return flags && !(g_varInt(flags) & kFlagAdditive);
}

std::string currentTextureName(const char *name) {
  const Entry *e = g_uses & kUseParams ? g_currentEntry : nullptr;
  if (!e)
    return {};
  for (const uint16_t i : namedParameters(*e, name)) {
    void *var = paramAt(*e, i);
    if (!var || varType(var) != kTypeTexture)
      continue;
    void *texture;
    std::memcpy(&texture, static_cast<const uint8_t *>(var) + kVarTexture, sizeof(texture));
    return textureNameOfTexture(texture);
  }
  return {};
}

bool currentFloat(const char *name, float &out) {
  const Entry *e = g_uses & kUseParams ? g_currentEntry : nullptr;
  if (!e)
    return false;
  for (const uint16_t i : namedParameters(*e, name)) {
    void *var = paramAt(*e, i);
    if (!var)
      continue;
    const uint8_t type = varType(var);
    if (type != kTypeFloat && type != kTypeVector && type != kTypeInt)
      continue;
    const float value = g_varFloat(var); // a vector's first component, an int as a float
    if (!std::isfinite(value))
      return false;
    out = value;
    return true;
  }
  return false;
}

bool currentVector(const char *name, float out[3]) {
  const Entry *e = g_uses & kUseParams ? g_currentEntry : nullptr;
  if (!e)
    return false;
  for (const uint16_t i : namedParameters(*e, name)) {
    void *var = paramAt(*e, i);
    if (!var || varType(var) != kTypeVector)
      continue;
    const float *v = g_varVec(var);
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
      return false;
    std::copy_n(v, 3, out);
    return true;
  }
  return false;
}

void describeCurrent(FILE *out, const char *indent) {
  const Entry *e = g_currentEntry;
  if (!e) {
    fprintf(out, "%s(material not read)\n", indent);
    return;
  }
  Entry::TextureName texture;
  for (int i = 0; i < e->count; ++i) {
    void *var = paramAt(*e, i);
    if (!var)
      continue;
    const uint8_t type = varType(var);
    const char *name = g_varName(var);
    if (!name || type == kTypeUndefined)
      continue;
    if (type == kTypeFloat) {
      fprintf(out, "%s%s %g\n", indent, name, g_varFloat(var));
    } else if (type == kTypeInt) {
      fprintf(out, "%s%s %d\n", indent, name, g_varInt(var));
    } else if (type == kTypeVector) {
      const float *v = g_varVec(var);
      fprintf(out, "%s%s [%g %g %g %g]\n", indent, name, v[0], v[1], v[2], v[3]);
    } else if (type == kTypeTexture) {
      fprintf(out, "%s%s texture %s\n", indent, name, textureName(texture, var).c_str());
    }
  }
}

void dumpStatistics(FILE *out) {
  const char *detail = g_detail.load();
  fprintf(out, "engine materials: %s%s%s\n", g_status.load(), *detail ? ": " : "", detail);
  if (!g_installed)
    return;
  if (!game::kGMod)
    fprintf(out, "   answers to the proxy: %s\n", g_usesText.c_str());
  static ULONGLONG last = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  const double seconds = now > last ? double(now - last) / 1000 : 1;
  last = now;
  const double baseAlpha = g_stats.baseAlpha.exchange(0) / seconds, mask = g_stats.mask.exchange(0) / seconds,
               maskAlpha = g_stats.maskAlpha.exchange(0) / seconds;
  fprintf(out,
          "   binds per second %.0f: unlit %.1f, self-illuminated %.1f (base alpha %.1f, $selfillummask %.1f, envmap "
          "mask alpha %.1f) | not read: other material classes %.1f, without params %.1f, foreign vars %.1f, mask "
          "var without a texture %.1f\n"
          "   decal materials bound per second ($decal or a Decal* shader) %.1f | sub-rectangle materials (decal "
          "atlases) %.1f, read through their page: %s; without a page %.1f\n",
          double(g_stats.binds.exchange(0)) / seconds, double(g_stats.unlit.exchange(0)) / seconds,
          baseAlpha + mask + maskAlpha, baseAlpha, mask, maskAlpha, double(g_stats.otherClass.exchange(0)) / seconds,
          double(g_stats.noParams.exchange(0)) / seconds, double(g_stats.foreignVar.exchange(0)) / seconds,
          double(g_stats.noMaskTexture.exchange(0)) / seconds, double(g_stats.decals.exchange(0)) / seconds,
          double(g_stats.subRects.exchange(0)) / seconds, g_subRects ? "yes" : "no (layout not recognized)",
          double(g_stats.subRectNoPage.exchange(0)) / seconds);
  std::lock_guard<std::mutex> lock(g_stats.lock);
  fprintf(out, "   materials seen since launch, by shader (%zu known now):", g_stats.entries);
  for (const auto &[shader, count] : g_stats.shaders)
    fprintf(out, " %s %u", shader.empty() ? "(none)" : shader.c_str(), count);
  fprintf(out, "\n");
  // The most bound first.
  std::vector<const std::pair<const std::string, Seen> *> order;
  for (const auto &item : g_stats.seen)
    order.push_back(&item);
  std::sort(order.begin(), order.end(),
            [](const auto *a, const auto *b) { return a->second.binds > b->second.binds; });
  static const char *const modes[] = {"lit", "unlit", "selfillum base alpha", "selfillum mask", "selfillum envmap mask alpha"};
  for (size_t i = 0; i < order.size() && i < 24; ++i) {
    const Seen &s = order[i]->second;
    fprintf(out, "     %s (%s): %s, tint %.2f %.2f %.2f", order[i]->first.c_str(), s.shader.c_str(), modes[s.mode],
            s.tint[0], s.tint[1], s.tint[2]);
    if (!s.mask.empty())
      fprintf(out, ", mask %s x %.2f", s.mask.c_str(), s.maskScale);
    fprintf(out, ", %.1f binds/s\n", double(s.binds) / seconds);
  }
  if (order.size() > 24)
    fprintf(out, "     ... and %zu more\n", order.size() - 24);
  g_stats.seen.clear();
}

std::string textureNameOf(void *texture) { return textureNameOfTexture(texture); }

} // namespace proxy::engine_materials
