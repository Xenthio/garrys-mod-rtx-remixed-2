// IShaderAPIDX8 (ShaderApi029) slot names for Portal 2 (shaderapidx9.dll of Jun
// 2026, the Vulkan-era update; 280 slots, L4D2's has 274).
// CreateInterface("ShaderApi029") hands out &g_ShaderAPIDX8 + 0x248 (its
// InterfaceReg returns 0x100d7288, ShaderDevice001 0x100d7040): CShaderAPIDx8's
// IShaderAPIDX8 subobject, vtable RVA 0x70a54 by its RTTI (offset 0x248 in the
// complete object locator). The material system reaches it through the
// SetMode factory (g_pShaderAPI).
// Derived from three sources:
//   * L4D2's table (slots_shaderapi_l4d2.h) aligned to this binary by code:
//     each slot's function (an adjustor or jump thunk followed) up to its
//     first ret, absolute addresses and call targets masked, compared with
//     L4D2's (difflib ratio, callee-popped bytes and referenced strings), and a
//     global alignment over both vtables. "= L4D2 n": the same code as L4D2's
//     slot n (ratio >= 0.8), "~ L4D2 n" similar (>= 0.5), "L4D2 n by
//     position" only where the alignment puts it.
//   * The Portal 2 SDK header (alliedmodders hl2sdk, branch portal2,
//     public/shaderapi/ishaderapi.h, ShaderApi029) in MSVC slot order
//     (scratchpad tools/sdk_vtables.py), aligned by stack bytes and the L4D2
//     names: the names below 241. The binary lacks the header's
//     SetAmbientLightColor (24), FlushBufferedPrimitives (78) and
//     EvictManagedResources (145), as L4D2's does, and has ten slots neither
//     the header nor L4D2 has ("new", 14, 55, 61, 63, 148, 181, 190, 241,
//     247, 274). Against L4D2: ShouldWriteDepthToDestAlpha (41) and
//     FlipCullMode (98) come back; L4D2's two float setters after its fog
//     start and end (its 135 and 136, which its table calls FogStart and
//     FogEnd: those are its 133 and 134), its 257 and its last two are gone.
//   * The disassembly, for every slot game.h uses and their neighbours
//     ("code"). 241 on are IShaderAPIDX8's own (no public header): "-", or
//     L4D2's name where the code is L4D2's.
// "pops n": the callee pops other than the SDK header says (the 2026 build's
// signatures: BindStandardTexture, SetTextureFilterMode, OverrideDepthEnable,
// CreateDepthTexture and BindTexture take one argument more; L4D2's
// GetNearestSupportedFormat already did).
#pragma once
namespace proxy::slots {
inline const char *const kShaderApi[280] = {
    "CurrentTime", // 0 ~ L4D2 0
    "GetLightmapDimensions", // 1 = L4D2 1
    "GetSceneFogMode", // 2 code: returns the fog mode SceneFogMode (142) stores
    "GetSceneFogColor", // 3 code: copies the 3 bytes SceneFogColor3ub (141) stores
    "SetVertexShaderConstant", // 4 = L4D2 4
    "SetPixelShaderConstant", // 5 ~ L4D2 5
    "SetDefaultState", // 6 ~ L4D2 6
    "GetWorldSpaceCameraPosition", // 7 ~ L4D2 7
    "GetWorldSpaceCameraDirection", // 8 ~ L4D2 8
    "GetCurrentNumBones", // 9 code: returns what SetNumBoneWeights (91) stores
    "GetCurrentFogType", // 10 = L4D2 10
    "SetVertexShaderIndex", // 11 = L4D2 11
    "SetPixelShaderIndex", // 12 = L4D2 12
    "GetBackBufferDimensions", // 13 = L4D2 13
    "-", // 14 new: returns a device structure (ShaderDevice +0xc4)
    "GetCurrentRenderTargetDimensions", // 15 = L4D2 14
    "GetCurrentViewport", // 16 = L4D2 15
    "SetPixelShaderFogParams", // 17 ~ L4D2 16
    "InFlashlightMode", // 18 = L4D2 17
    "GetFlashlightState", // 19 = L4D2 18
    "InEditorMode", // 20 = L4D2 19
    "BindStandardTexture", // 21 pops 12 (Portal 2 SDK header: 8); L4D2 20 by position
    "GetRenderTargetEx", // 22 = L4D2 21
    "SetToneMappingScaleLinear", // 23 ~ L4D2 22
    "GetToneMappingScaleLinear", // 24 ~ L4D2 23
    "SetFloatRenderingParameter", // 25 = L4D2 24
    "SetIntRenderingParameter", // 26 = L4D2 25
    "SetVectorRenderingParameter", // 27 = L4D2 26
    "GetFloatRenderingParameter", // 28 = L4D2 27
    "GetIntRenderingParameter", // 29 = L4D2 28
    "GetVectorRenderingParameter", // 30 ~ L4D2 29
    "GetFlashlightStateEx", // 31 = L4D2 30
    "GetDX9LightState", // 32 ~ L4D2 31
    "GetPixelFogCombo", // 33 = L4D2 32
    "BindStandardVertexTexture", // 34 = L4D2 33
    "IsHWMorphingEnabled", // 35 ~ L4D2 34
    "GetStandardTextureDimensions", // 36 = L4D2 35
    "SetBooleanVertexShaderConstant", // 37 ~ L4D2 36
    "SetIntegerVertexShaderConstant", // 38 = L4D2 37
    "SetBooleanPixelShaderConstant", // 39 ~ L4D2 38
    "SetIntegerPixelShaderConstant", // 40 = L4D2 39
    "ShouldWriteDepthToDestAlpha", // 41 not in L4D2 (in the Portal 2 SDK header)
    "GetMatrix", // 42 code: copies the top of the mode's matrix stack (64 bytes)
    "PushDeformation", // 43 = L4D2 41
    "PopDeformation", // 44 ~ L4D2 42
    "GetNumActiveDeformations", // 45 ~ L4D2 43
    "GetPackedDeformationInformation", // 46 = L4D2 44
    "MarkUnusedVertexFields", // 47 = L4D2 45
    "ExecuteCommandBuffer", // 48 ~ L4D2 46
    "GetCurrentColorCorrection", // 49 = L4D2 47
    "GetTextureRenderingParameter", // 50 = L4D2 48
    "SetScreenSizeForVPOS", // 51 ~ L4D2 49
    "SetVSNearAndFarZ", // 52 ~ L4D2 50
    "GetFarZ", // 53 ~ L4D2 51
    "SinglePassFlashlightModeEnabled", // 54 ~ L4D2 52
    "-", // 55 new: returns a VMatrix by value (copies 64 bytes)
    "SetDepthFeatheringPixelShaderConstant", // 56 ~ L4D2 53
    "GetFlashlightShaderInfo", // 57 ~ L4D2 54
    "GetFlashlightAmbientOcclusion", // 58 ~ L4D2 55
    "SetTextureFilterMode", // 59 pops 12 (Portal 2 SDK header: 8); L4D2 56 by position
    "GetTessellationMode", // 60 ~ L4D2 57
    "-", // 61 new: forwards to IShaderUtil +0x88
    "GetSubDHeight", // 62 L4D2 58 by position
    "-", // 63 new: returns a byte at +0x9; L4D2 59 by position
    "SetViewports", // 64 L4D2 60 by position
    "GetViewports", // 65 ~ L4D2 61
    "ClearBuffers", // 66 ~ L4D2 62
    "ClearColor3ub", // 67 = L4D2 63
    "ClearColor4ub", // 68 = L4D2 64
    "BindVertexShader", // 69 = L4D2 65
    "BindGeometryShader", // 70 = L4D2 66
    "BindPixelShader", // 71 = L4D2 67
    "SetRasterState", // 72 = L4D2 68
    "SetMode", // 73 = L4D2 69
    "ChangeVideoMode", // 74 = L4D2 70
    "TakeSnapshot", // 75 ~ L4D2 71
    "TexMinFilter", // 76 = L4D2 72
    "TexMagFilter", // 77 = L4D2 73
    "TexWrap", // 78 ~ L4D2 74
    "CopyRenderTargetToTexture", // 79 = L4D2 75
    "Bind", // 80 code: stores m_pMaterial [this+0x2c00]; CMatRenderContext::Bind calls [vtable+0x140]
    "GetDynamicMesh", // 81 = L4D2 77
    "GetDynamicMeshEx", // 82 = L4D2 78
    "IsTranslucent", // 83 L4D2 79 by position
    "IsAlphaTested", // 84 ~ L4D2 80
    "UsesVertexAndPixelShaders", // 85 = L4D2 81
    "IsDepthWriteEnabled", // 86 ~ L4D2 82
    "ComputeVertexFormat", // 87 = L4D2 83
    "ComputeVertexUsage", // 88 ~ L4D2 84
    "BeginPass", // 89 ~ L4D2 85
    "RenderPass", // 90 ~ L4D2 86
    "SetNumBoneWeights", // 91 code
    "SetLights", // 92 code: count clamped to 4, copies count * 0x58 bytes
    "SetLightingOrigin", // 93 ~ L4D2 89
    "SetLightingState", // 94 = L4D2 90
    "SetAmbientLightCube", // 95 = L4D2 91
    "ShadeMode", // 96 = L4D2 92
    "CullMode", // 97 = L4D2 93
    "FlipCullMode", // 98 not in L4D2 (in the Portal 2 SDK header)
    "ForceDepthFuncEquals", // 99 = L4D2 94
    "OverrideDepthEnable", // 100 pops 12 (Portal 2 SDK header: 8); L4D2 95 by position
    "SetHeightClipZ", // 101 ~ L4D2 96
    "SetHeightClipMode", // 102 ~ L4D2 97
    "SetClipPlane", // 103 ~ L4D2 98
    "EnableClipPlane", // 104 ~ L4D2 99
    "GetNearestSupportedFormat", // 105 pops 8 (Portal 2 SDK header: 4); = L4D2 100
    "GetNearestRenderTargetFormat", // 106 = L4D2 101
    "DoRenderTargetsNeedSeparateDepthBuffer", // 107 ~ L4D2 102
    "CreateTexture", // 108 = L4D2 103
    "DeleteTexture", // 109 L4D2 104 by position
    "CreateDepthTexture", // 110 pops 24 (Portal 2 SDK header: 20); L4D2 105 by position
    "IsTexture", // 111 ~ L4D2 106
    "IsTextureResident", // 112 = L4D2 107
    "ModifyTexture", // 113 ~ L4D2 108
    "TexImage2D", // 114 ~ L4D2 109
    "TexSubImage2D", // 115 ~ L4D2 110
    "TexLock", // 116 = L4D2 111
    "TexUnlock", // 117 ~ L4D2 112
    "UpdateTexture", // 118 ~ L4D2 113
    "LockTex", // 119 L4D2 114 by position
    "UnlockTex", // 120 = L4D2 115
    "TexSetPriority", // 121 = L4D2 116
    "BindTexture", // 122 pops 12 (Portal 2 SDK header: 8); L4D2 117 by position
    "SetRenderTarget", // 123 = L4D2 118
    "ClearBuffersObeyStencil", // 124 = L4D2 119
    "ReadPixels", // 125 ~ L4D2 120
    "ReadPixels", // 126 = L4D2 121
    "FlushHardware", // 127 = L4D2 122
    "BeginFrame", // 128 L4D2 123 by position
    "EndFrame", // 129 = L4D2 124; anchor "TextureList"
    "SelectionMode", // 130 ~ L4D2 125
    "SelectionBuffer", // 131 ~ L4D2 126
    "ClearSelectionNames", // 132 ~ L4D2 127
    "LoadSelectionName", // 133 ~ L4D2 128
    "PushSelectionName", // 134 ~ L4D2 129
    "PopSelectionName", // 135 ~ L4D2 130
    "ForceHardwareSync", // 136 ~ L4D2 131
    "ClearSnapshots", // 137 ~ L4D2 132
    "FogStart", // 138 code: writes the fog start GetFogDistances reads
    "FogEnd", // 139 code: the fog end
    "SetFogZ", // 140 code: the fog z
    "SceneFogColor3ub", // 141 code
    "SceneFogMode", // 142 code
    "CanDownloadTextures", // 143 ~ L4D2 140
    "ResetRenderState", // 144 L4D2 141 by position
    "GetCurrentDynamicVBSize", // 145 ~ L4D2 142
    "DestroyVertexBuffers", // 146 ~ L4D2 143
    "SetAnisotropicLevel", // 147 ~ L4D2 144
    "-", // 148 new: zeroes a 28-byte struct it is handed
    "-", // 149 L4D2 145 by position
    "SyncToken", // 150 = L4D2 146
    "SetStandardVertexShaderConstants", // 151 L4D2 147 by position
    "CreateOcclusionQueryObject", // 152 = L4D2 148
    "DestroyOcclusionQueryObject", // 153 = L4D2 149
    "BeginOcclusionQueryDrawing", // 154 = L4D2 150
    "EndOcclusionQueryDrawing", // 155 = L4D2 151
    "OcclusionQuery_GetNumPixelsRendered", // 156 L4D2 152 by position
    "SetFlashlightState", // 157 = L4D2 153
    "ClearVertexAndPixelShaderRefCounts", // 158 = L4D2 154
    "PurgeUnusedVertexAndPixelShaders", // 159 = L4D2 155
    "DXSupportLevelChanged", // 160 = L4D2 156
    "EnableUserClipTransformOverride", // 161 ~ L4D2 157
    "UserClipTransform", // 162 ~ L4D2 158
    "SetRenderTargetEx", // 163 ~ L4D2 159
    "-", // 164 L4D2 160 by position; anchor "Refraction overhead"
    "CopyRenderTargetToTextureEx", // 165 ~ L4D2 161
    "HandleDeviceLost", // 166 ~ L4D2 162
    "EnableLinearColorSpaceFrameBuffer", // 167 = L4D2 163
    "SetFullScreenTextureHandle", // 168 = L4D2 164
    "SetFastClipPlane", // 169 ~ L4D2 165
    "EnableFastClip", // 170 ~ L4D2 166
    "GetMaxToRender", // 171 = L4D2 167
    "GetMaxVerticesToRender", // 172 = L4D2 168
    "GetMaxIndicesToRender", // 173 = L4D2 169
    "SetStencilState", // 174 L4D2 170 by position
    "ClearStencilBufferRectangle", // 175 ~ L4D2 171
    "DisableAllLocalLights", // 176 L4D2 172 by position
    "CompareSnapshots", // 177 = L4D2 173
    "GetFlexMesh", // 178 = L4D2 174
    "SetFlashlightStateEx", // 179 ~ L4D2 175
    "SupportsMSAAMode", // 180 = L4D2 176
    "-", // 181 new: an empty stub (ret 4)
    "OwnGPUResources", // 182 = L4D2 177
    "GetFogDistances", // 183 code: start, end, fog z
    "BeginPIXEvent", // 184 code: an empty stub (ret 8), folded with 186
    "EndPIXEvent", // 185 code: empty (ret)
    "SetPIXMarker", // 186 code: the empty stub of 184 (ret 8); CMatRenderContext::SetPIXMarker (its slot 137) jumps to [g_pShaderAPI vtable+0x2e8]
    "EnableAlphaToCoverage", // 187 L4D2 182 by position
    "DisableAlphaToCoverage", // 188 L4D2 183 by position
    "ComputeVertexDescription", // 189 = L4D2 184
    "-", // 190 new: forwards (2 args) to another interface's +0x74
    "SetDisallowAccess", // 191 = L4D2 185
    "EnableShaderShaderMutex", // 192 = L4D2 186
    "ShaderLock", // 193 = L4D2 187
    "ShaderUnlock", // 194 = L4D2 188
    "SetShadowDepthBiasFactors", // 195 L4D2 189 by position
    "BindVertexBuffer", // 196 = L4D2 190
    "BindIndexBuffer", // 197 = L4D2 191
    "Draw", // 198 = L4D2 192
    "PerformFullScreenStencilOperation", // 199 = L4D2 193
    "SetScissorRect", // 200 L4D2 194 by position
    "SupportsCSAAMode", // 201 = L4D2 195
    "InvalidateDelayedShaderConstants", // 202 ~ L4D2 196
    "GammaToLinear_HardwareSpecific", // 203 ~ L4D2 197
    "LinearToGamma_HardwareSpecific", // 204 ~ L4D2 198
    "SetLinearToGammaConversionTextures", // 205 ~ L4D2 199
    "BindVertexTexture", // 206 ~ L4D2 200
    "EnableHWMorphing", // 207 ~ L4D2 201
    "SetFlexWeights", // 208 = L4D2 202
    "FogMaxDensity", // 209 ~ L4D2 203
    "CreateTextures", // 210 ~ L4D2 204
    "AcquireThreadOwnership", // 211 = L4D2 205
    "ReleaseThreadOwnership", // 212 ~ L4D2 206
    "EnableBuffer2FramesAhead", // 213 = L4D2 207
    "FlipCulling", // 214 = L4D2 208
    "SetTextureRenderingParameter", // 215 = L4D2 209
    "EnableSinglePassFlashlightMode", // 216 = L4D2 210
    "MatrixMode", // 217 ~ L4D2 211
    "PushMatrix", // 218 L4D2 212 by position
    "PopMatrix", // 219 L4D2 213 by position
    "LoadMatrix", // 220 L4D2 214 by position
    "MultMatrix", // 221 ~ L4D2 215
    "MultMatrixLocal", // 222 ~ L4D2 216
    "LoadIdentity", // 223 ~ L4D2 217
    "LoadCameraToWorld", // 224 L4D2 218 by position
    "Ortho", // 225 ~ L4D2 219
    "PerspectiveX", // 226 ~ L4D2 220
    "PickMatrix", // 227 L4D2 221 by position
    "Rotate", // 228 ~ L4D2 222
    "Translate", // 229 ~ L4D2 223
    "Scale", // 230 ~ L4D2 224
    "ScaleXY", // 231 ~ L4D2 225
    "PerspectiveOffCenterX", // 232 ~ L4D2 226
    "LoadBoneMatrix", // 233 L4D2 227 by position
    "SetStandardTextureHandle", // 234 ~ L4D2 228
    "DrawInstances", // 235 L4D2 229 by position
    "OverrideAlphaWriteEnable", // 236 = L4D2 230
    "OverrideColorWriteEnable", // 237 = L4D2 231
    "ClearBuffersObeyStencilEx", // 238 = L4D2 232
    "OnPresent", // 239 code: = L4D2 233
    "UpdateGameTime", // 240 code: stores the time (L4D2 234)
    "-", // 241 new: returns false
    "-", // 242 L4D2 235 by position
    "-", // 243 = L4D2 236
    "-", // 244 = L4D2 237
    "-", // 245 = L4D2 238
    "-", // 246 = L4D2 239
    "-", // 247 new: forwards to a global interface's +0xf8
    "-", // 248 = L4D2 240
    "-", // 249 = L4D2 241; anchor "-tools"
    "-", // 250 ~ L4D2 242
    "-", // 251 = L4D2 243
    "-", // 252 = L4D2 244
    "-", // 253 = L4D2 245
    "GetD3DTexture", // 254 = L4D2 246
    "-", // 255 ~ L4D2 247
    "-", // 256 ~ L4D2 248
    "-", // 257 = L4D2 249
    "GetBufferedState", // 258 L4D2 250 by position
    "GetCullMode", // 259 = L4D2 251
    "ComputeFillRate", // 260 = L4D2 252
    "IsInSelectionMode", // 261 ~ L4D2 253
    "RegisterSelectionHit", // 262 = L4D2 254
    "GetBoundMaterial", // 263 ~ L4D2 255
    "-", // 264 ~ L4D2 256
    "-", // 265 ~ L4D2 258
    "-", // 266 L4D2 259 by position
    "-", // 267 = L4D2 260
    "-", // 268 ~ L4D2 261
    "-", // 269 L4D2 262 by position
    "-", // 270 ~ L4D2 263
    "-", // 271 ~ L4D2 264
    "-", // 272 = L4D2 265
    "-", // 273 = L4D2 266
    "-", // 274 new
    "-", // 275 = L4D2 267
    "-", // 276 = L4D2 268
    "-", // 277 L4D2 269 by position
    "-", // 278 L4D2 270 by position
    "-", // 279 ~ L4D2 271
};
} // namespace proxy::slots
