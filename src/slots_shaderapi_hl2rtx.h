// IShaderAPIDX8 (ShaderApi030) slot names for Half-Life 2 RTX (shaderapidx9.dll
// of the Portal RTX engine build, "portal-trex", Nov 2026; 284 slots).
// Derived as slots_shaderapi_l4d2.h was, with l4d2-rtx/proxy/tools:
//   rtti_vtables.py shaderapidx9.dll: CShaderAPIDx8's IShaderAPIDX8 subobject
//     (+0x22c, the object CreateInterface("ShaderApi030") hands out), vtable
//     RVA 0x86e64, 284 slots (IShaderAPIDX8's own abstract vtable has as many).
//   sdk_vtables.py --json IShaderAPI on Source SDK 2013
//     (ValveSoftware/source-sdk-2013, src/public/shaderapi/ishaderapi.h,
//     ShaderApi030: MSVC slot order, callee-popped stack bytes), then
//     align_slots.py (stack bytes and string anchors per slot).
//   The alignment, read against the disassembly: binary slot N is SDK slot N
//   below 99 and N + 1 from 99 to 253: IShaderDynamicAPI ends at
//   SetDepthFeatheringPixelShaderConstant (98), without SDK 2013's
//   GetPixelFogCombo1, so ClearBuffers is 99 and Bind 113 (GMod's ShaderApi030:
//   114). 254 on are IShaderAPIDX8's own (no public header): "-", or named
//   where the stack sizes agree with the rest of IShaderAPI.
// Evidence per slot: "code" (read in the disassembly: what game.h uses, and
// the slots around the one-slot shift), "anchor" (a string that names the
// method), "forwards..." (the stack size is the callee's), otherwise
// stack-size agreement. "-" = no SDK counterpart.
#pragma once
namespace proxy::slots {
inline const char *const kShaderApi[284] = {
    "SetViewports", // 0
    "GetViewports", // 1
    "CurrentTime", // 2
    "GetLightmapDimensions", // 3
    "GetSceneFogMode", // 4 code
    "GetSceneFogColor", // 5 code
    "MatrixMode", // 6
    "PushMatrix", // 7
    "PopMatrix", // 8
    "LoadMatrix", // 9
    "MultMatrix", // 10
    "MultMatrixLocal", // 11
    "GetMatrix", // 12 code
    "LoadIdentity", // 13
    "LoadCameraToWorld", // 14
    "Ortho", // 15
    "PerspectiveX", // 16
    "PickMatrix", // 17
    "Rotate", // 18
    "Translate", // 19
    "Scale", // 20
    "ScaleXY", // 21
    "Color3f", // 22
    "Color3fv", // 23
    "Color4f", // 24
    "Color4fv", // 25
    "Color3ub", // 26
    "Color3ubv", // 27
    "Color4ub", // 28
    "Color4ubv", // 29
    "SetVertexShaderConstant", // 30
    "SetPixelShaderConstant", // 31
    "SetDefaultState", // 32
    "GetWorldSpaceCameraPosition", // 33
    "GetCurrentNumBones", // 34 code
    "GetCurrentLightCombo", // 35
    "GetCurrentFogType", // 36
    "SetTextureTransformDimension", // 37
    "DisableTextureTransform", // 38
    "SetBumpEnvMatrix", // 39
    "SetVertexShaderIndex", // 40
    "SetPixelShaderIndex", // 41
    "GetBackBufferDimensions", // 42
    "GetMaxLights", // 43
    "GetLight", // 44
    "SetPixelShaderFogParams", // 45
    "SetVertexShaderStateAmbientLightCube", // 46
    "SetPixelShaderStateAmbientLightCube", // 47
    "CommitPixelShaderLighting", // 48
    "GetVertexModifyBuilder", // 49
    "InFlashlightMode", // 50
    "GetFlashlightState", // 51
    "InEditorMode", // 52
    "GetBoundMorphFormat", // 53
    "BindStandardTexture", // 54
    "GetRenderTargetEx", // 55
    "SetToneMappingScaleLinear", // 56
    "GetToneMappingScaleLinear", // 57
    "GetLightMapScaleFactor", // 58
    "LoadBoneMatrix", // 59
    "PerspectiveOffCenterX", // 60
    "SetFloatRenderingParameter", // 61
    "SetIntRenderingParameter", // 62
    "SetVectorRenderingParameter", // 63
    "GetFloatRenderingParameter", // 64
    "GetIntRenderingParameter", // 65
    "GetVectorRenderingParameter", // 66
    "SetStencilEnable", // 67
    "SetStencilFailOperation", // 68
    "SetStencilZFailOperation", // 69
    "SetStencilPassOperation", // 70
    "SetStencilCompareFunction", // 71
    "SetStencilReferenceValue", // 72
    "SetStencilTestMask", // 73
    "SetStencilWriteMask", // 74
    "ClearStencilBufferRectangle", // 75
    "GetDXLevelDefaults", // 76
    "GetFlashlightStateEx", // 77
    "GetAmbientLightCubeLuminance", // 78
    "GetDX9LightState", // 79
    "GetPixelFogCombo", // 80
    "BindStandardVertexTexture", // 81
    "IsHWMorphingEnabled", // 82
    "GetStandardTextureDimensions", // 83
    "SetBooleanVertexShaderConstant", // 84
    "SetIntegerVertexShaderConstant", // 85
    "SetBooleanPixelShaderConstant", // 86
    "SetIntegerPixelShaderConstant", // 87
    "ShouldWriteDepthToDestAlpha", // 88
    "PushDeformation", // 89
    "PopDeformation", // 90
    "GetNumActiveDeformations", // 91
    "GetPackedDeformationInformation", // 92
    "MarkUnusedVertexFields", // 93 code
    "ExecuteCommandBuffer", // 94 code
    "SetStandardTextureHandle", // 95 code
    "GetCurrentColorCorrection", // 96
    "SetPSNearAndFarZ", // 97 code
    "SetDepthFeatheringPixelShaderConstant", // 98 code
    "ClearBuffers", // 99
    "ClearColor3ub", // 100
    "ClearColor4ub", // 101
    "BindVertexShader", // 102
    "BindGeometryShader", // 103
    "BindPixelShader", // 104
    "SetRasterState", // 105
    "SetMode", // 106
    "ChangeVideoMode", // 107
    "TakeSnapshot", // 108
    "TexMinFilter", // 109 anchor
    "TexMagFilter", // 110 anchor
    "TexWrap", // 111 anchor
    "CopyRenderTargetToTexture", // 112
    "Bind", // 113 code
    "FlushBufferedPrimitives", // 114 code
    "GetDynamicMesh", // 115
    "GetDynamicMeshEx", // 116
    "IsTranslucent", // 117
    "IsAlphaTested", // 118
    "UsesVertexAndPixelShaders", // 119
    "IsDepthWriteEnabled", // 120
    "ComputeVertexFormat", // 121
    "ComputeVertexUsage", // 122
    "BeginPass", // 123
    "RenderPass", // 124
    "SetNumBoneWeights", // 125
    "SetLight", // 126 code
    "SetLightingOrigin", // 127 code
    "SetAmbientLight", // 128
    "SetAmbientLightCube", // 129
    "ShadeMode", // 130
    "CullMode", // 131 anchor
    "ForceDepthFuncEquals", // 132
    "OverrideDepthEnable", // 133
    "SetHeightClipZ", // 134
    "SetHeightClipMode", // 135
    "SetClipPlane", // 136
    "EnableClipPlane", // 137
    "SetSkinningMatrices", // 138
    "GetNearestSupportedFormat", // 139
    "GetNearestRenderTargetFormat", // 140
    "DoRenderTargetsNeedSeparateDepthBuffer", // 141
    "CreateTexture", // 142
    "DeleteTexture", // 143
    "CreateDepthTexture", // 144
    "IsTexture", // 145
    "IsTextureResident", // 146
    "ModifyTexture", // 147
    "TexImage2D", // 148
    "TexSubImage2D", // 149
    "TexImageFromVTF", // 150
    "TexLock", // 151
    "TexUnlock", // 152
    "TexSetPriority", // 153
    "BindTexture", // 154
    "SetRenderTarget", // 155
    "ClearBuffersObeyStencil", // 156
    "ReadPixels", // 157
    "ReadPixels", // 158
    "FlushHardware", // 159
    "BeginFrame", // 160
    "EndFrame", // 161
    "SelectionMode", // 162
    "SelectionBuffer", // 163
    "ClearSelectionNames", // 164
    "LoadSelectionName", // 165
    "PushSelectionName", // 166
    "PopSelectionName", // 167
    "ForceHardwareSync", // 168
    "ClearSnapshots", // 169
    "FogStart", // 170
    "FogEnd", // 171
    "SetFogZ", // 172
    "SceneFogColor3ub", // 173
    "SceneFogMode", // 174
    "CanDownloadTextures", // 175
    "ResetRenderState", // 176
    "GetCurrentDynamicVBSize", // 177
    "DestroyVertexBuffers", // 178
    "EvictManagedResources", // 179
    "SetAnisotropicLevel", // 180
    "SyncToken", // 181
    "SetStandardVertexShaderConstants", // 182
    "CreateOcclusionQueryObject", // 183
    "DestroyOcclusionQueryObject", // 184
    "BeginOcclusionQueryDrawing", // 185
    "EndOcclusionQueryDrawing", // 186
    "OcclusionQuery_GetNumPixelsRendered", // 187 anchor
    "SetFlashlightState", // 188
    "ClearVertexAndPixelShaderRefCounts", // 189
    "PurgeUnusedVertexAndPixelShaders", // 190
    "DXSupportLevelChanged", // 191
    "EnableUserClipTransformOverride", // 192
    "UserClipTransform", // 193
    "ComputeMorphFormat", // 194
    "SetRenderTargetEx", // 195
    "CopyRenderTargetToTextureEx", // 196
    "CopyTextureToRenderTargetEx", // 197
    "HandleDeviceLost", // 198
    "EnableLinearColorSpaceFrameBuffer", // 199
    "SetFullScreenTextureHandle", // 200
    "SetFastClipPlane", // 201
    "EnableFastClip", // 202
    "GetMaxToRender", // 203 forwards to the mesh manager
    "GetMaxVerticesToRender", // 204
    "GetMaxIndicesToRender", // 205
    "DisableAllLocalLights", // 206
    "CompareSnapshots", // 207
    "GetFlexMesh", // 208
    "SetFlashlightStateEx", // 209
    "SupportsMSAAMode", // 210
    "OwnGPUResources", // 211
    "GetFogDistances", // 212 code
    "BeginPIXEvent", // 213 code
    "EndPIXEvent", // 214 code
    "SetPIXMarker", // 215 code
    "EnableAlphaToCoverage", // 216
    "DisableAlphaToCoverage", // 217
    "ComputeVertexDescription", // 218
    "SupportsShadowDepthTextures", // 219
    "SetDisallowAccess", // 220
    "EnableShaderShaderMutex", // 221
    "ShaderLock", // 222
    "ShaderUnlock", // 223
    "GetShadowDepthTextureFormat", // 224
    "SupportsFetch4", // 225
    "SetShadowDepthBiasFactors", // 226
    "BindVertexBuffer", // 227
    "BindIndexBuffer", // 228
    "Draw", // 229 forwards to the mesh manager
    "PerformFullScreenStencilOperation", // 230
    "SetScissorRect", // 231
    "SupportsCSAAMode", // 232
    "InvalidateDelayedShaderConstants", // 233
    "GammaToLinear_HardwareSpecific", // 234
    "LinearToGamma_HardwareSpecific", // 235
    "SetLinearToGammaConversionTextures", // 236
    "GetNullTextureFormat", // 237
    "BindVertexTexture", // 238
    "EnableHWMorphing", // 239
    "SetFlexWeights", // 240
    "FogMaxDensity", // 241
    "CreateTextures", // 242
    "AcquireThreadOwnership", // 243
    "ReleaseThreadOwnership", // 244
    "SupportsNormalMapCompression", // 245
    "EnableBuffer2FramesAhead", // 246
    "PrintfVA", // 247
    "Printf", // 248 cdecl (varargs): pops nothing
    "Knob", // 249
    "OverrideAlphaWriteEnable", // 250
    "OverrideColorWriteEnable", // 251
    "ClearBuffersObeyStencilEx", // 252
    "CopyRenderTargetToScratchTexture", // 253
    "-", // 254
    "-", // 255
    "-", // 256
    "-", // 257
    "-", // 258
    "-", // 259
    "-", // 260
    "-", // 261
    "-", // 262
    "-", // 263
    "-", // 264
    "-", // 265
    "-", // 266
    "-", // 267
    "-", // 268
    "-", // 269
    "-", // 270
    "UnlockRect", // 271 stack size only
    "TexLodClamp", // 272 stack size only
    "TexLodBias", // 273 stack size only
    "-", // 274
    "CopyTextureToTexture", // 275 stack size only
    "VertexFormatSize", // 276 stack size only
    "-", // 277
    "SceneFogRadial", // 278 stack size only
    "-", // 279
    "-", // 280
    "GetSceneFogRadial", // 281 stack size only
    "-", // 282
    "-", // 283
};
} // namespace proxy::slots
