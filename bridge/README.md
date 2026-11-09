# OpenRemix x86 bridge

Lets a 32-bit game use the x64-only OpenRemix runtime.

- `remix_bridge.dll` (x86) exports the Remix API (`remixapi_InitializeLibrary`,
  the 0.1003 interface table and the `remix_native_ext.h` named exports, listed
  below). Load it in place of `remix.dll`, and resolve named exports from it:
  `GetProcAddress(GetModuleHandleW(L"remix_bridge.dll"), ...)` (there is no
  `remix.dll` in the game process).
- `openremix/openremix_bridge_host.exe` (x64) is launched by the client. It
  loads `openremix/remix.dll` and runs every runtime call on one thread.

## Transport

Shared memory with a single-producer ring (`src/common/protocol.h`). Structs
are re-encoded by the client in their x64 layout; pointers become offsets that
the host resolves in place, so bulk data is handed to the runtime straight
from shared memory. Payloads over 64 KiB go into separate DATA records, and
payloads over 1/8 of the ring go in chunked blobs.

- `Create*` calls return right away. The client assigns the handle ids, and
  the host logs any failures (`openremix_bridge_GetAsyncErrorCount`).
- Getters, `Startup`, `Shutdown`, `SetConfigVariable`, `SetConfigDefaultEXT`
  and `SetFullscreen` wait for the host. A synchronous call waits for every
  queued frame, so nothing called per frame is synchronous.
- `Present` blocks only if more than `OPENREMIX_BRIDGE_MAX_QUEUED_FRAMES`
  frames (default 2) are queued.
- D3D9/D3D11 interop and object picking return `FEATURE_UNAVAILABLE` or are
  not exported.

### Named exports

| Export | Behaviour |
| --- | --- |
| `SetConfigDefaultEXT` | synchronous (callers fall back to `SetConfigVariable` on failure) |
| `UpdateTextureEXT`, `UpdateUITextureRegionEXT` | asynchronous: `SUCCESS` means queued. Rows are repacked tightly, so a sub-rectangle of a wider buffer (`rowPitch` > row bytes) sends only the region. Refusals (e.g. a texture not created dynamic) are logged by the host. |
| `AddSceneInstancesEXT`, `SetSceneInstanceEXT`, `ChangeSceneInstancesEXT`, `RemoveSceneInstancesEXT`, `DrawSceneInstancesEXT` | asynchronous. Instance handles are client ids returned at once; the host maps them to the runtime's. A batch the runtime refuses adds nothing: its ids draw nothing and later changes to them are dropped (logged, counted in `openremix_bridge_GetAsyncErrorCount`). |
| `RegisterHostShaderEXT`, `DestroyHostShaderEXT` | asynchronous. Shader handles are client ids; a shader the runtime refuses maps to none, and the runtime skips passes using it (`skippedShaders`). |
| `SubmitHostPassesEXT`, `UpdateHostTargetEXT`, `UpdateHostVolumeEXT` | asynchronous. Host target handles are the game's own values, passed through. |
| `GetHostPassesEnabledEXT`, `GetHostPassStatisticsEXT` | answered from shared memory, no round trip: the host refreshes the values after `Startup`, every `Present` and every setting change, so they describe the last frame the host executed (up to the queued frames behind the game). |
| `Get*StatisticsEXT`, `GetBackendCapabilitiesEXT`, `GetFramePerformanceEXT`, `GetFrameBreakdownEXT`, `GetGpuPassTimingsEXT`, `GetDenoisingCapabilitiesEXT` | synchronous (diagnostics; avoid calling them every frame) |
| `SetVisibilityPartitionEXT`, `ConfigureUSDModsEXT`, `GetUSDModsDiagnosticEXT`, `PollInputEvents`, `GetWindowMetrics`, `GetPresentationCount` | synchronous |
| `SetFogState`, `SetScreenTint`, `RegisterUITexture`, `SubmitUIDrawList`, `SetTextInputEnabled`, ... | asynchronous |

Exports for optional runtime functions that the loaded `remix.dll` lacks
return `FEATURE_UNAVAILABLE` immediately (the host reports what it found).

Bridge-only exports: `openremix_bridge_BeginRecording` / `EndRecording(id)` /
`ReplayBatch(id)` / `DeleteBatch(id)` (record the `DrawInstance` and
`DrawLightInstance` calls in between, replay them with one command),
`openremix_bridge_GetAsyncErrorCount`, and `remixapi_bridge_InjectInputEXT`
(forward game-window input to the runtime's UI).

### Structs

Every struct in `remix_c.h` and `remix_native_ext.h` is re-encoded from its
x86 layout, including every `pNext` extension. Pointers the generator cannot
infer are annotated in `tools/gen_layouts.py` (`ARRAYS`, `BLOBS`); a pointer
without an annotation fails generation. Not carried as struct fields:

- `remixapi_VisibilityPartitionEXT`'s arrays travel through their own command
  (sizes come from several fields).
- `void *` and function pointers without an annotation (none at present) would
  arrive as null.

While the client waits for the host, it dispatches cross-process sent messages.
Without this, the runtime's window setup deadlocks against the game's window
thread.

Environment variables: `OPENREMIX_BRIDGE_HOST` (host exe path),
`OPENREMIX_BRIDGE_RING_MB` (8-512, default 64), `OPENREMIX_BRIDGE_TRACE=1`
(logs each executed command to `openremix_bridge_host.log`).

## Building

The OpenRemix headers are not vendored; point the build at a checkout.

```sh
R=/path/to/openremix/public/include
meson setup --cross-file cross/i686-w64-mingw32.txt  -Dopenremix_include=$R _build/i686
meson setup --cross-file cross/x86_64-w64-mingw32.txt -Dopenremix_include=$R \
  -Dsdl_include=/path/to/SDL3/include _build/x86_64
meson compile -C _build/i686 && meson compile -C _build/x86_64
```

`src/common/layouts.gen.h` is generated from the headers (needs Python
libclang):

```sh
python3 tools/gen_layouts.py --include $R --out src/common/layouts.gen.h [--check]
```

## Smoke test

`bridge_smoke_x86.exe` (through the bridge) and `bridge_smoke_x64.exe` (direct)
render the same scene; `bridge_smoke <frames> <fixed-time> <out.png>` writes a
screenshot so the two outputs can be compared pixel for pixel. Besides the
transport paths (inline, DATA record, blob, Indirect) it exercises scene
instances, a per-instance material, a dynamic texture and a UI texture updated
from strided sub-rectangles, and host passes (registered shaders drawing a quad
with push constants, a uniform buffer, vertices and indices; a host target
copied into the frame). Host-pass pipelines compile in the background, so run
once to warm the pipeline cache before comparing: on a cold cache the passes
may still be missing from the screenshot frame.

Layout: `remix_bridge.dll` and `bridge_smoke_x86.exe` in one directory,
`openremix/` beside them containing the host, `remix.dll`, `SDL3.dll` and the
MinGW runtime DLLs that `remix.dll` needs.
