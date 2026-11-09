#pragma once

#include <cstdint>
#include <cstdio>

namespace proxy {
struct Kind;
}

// Where each vertex buffer came from: the shader API reports every buffer it
// makes to IVBAllocTracker (VBAllocTracker001) as CountVB(buffer, dynamic,
// size, vertex size, vertex format), and engine code brackets its own mesh
// creation with TrackMeshAllocations(name) ("R_StudioCreateStaticMeshes",
// ...). A Lua Mesh() is made outside any bracket, which tells the meshes Lua
// builds (InfMap terrain, scripted props) from the engine's.
namespace proxy::vb_origins {

// Hooks the tracker's slots on its wrapper kind (CountVB 0, UnCountVB 1,
// TrackMeshAllocations 2).
void install(Kind *kind);

struct Origin {
  const char *allocator = nullptr; // interned bracket name; nullptr outside any
  uint64_t format = 0;             // VertexFormat_t
  int vertexSize = 0;
  bool dynamic = false;
};

// The origin of a D3D vertex buffer (IDirect3DVertexBuffer9), or false when
// the tracker never reported it.
bool find(const void *buffer, Origin &out);

// Diagnostics: an unlit draw from this buffer (render thread), and the
// per-origin counts since the last call (stats file).
void countUnlit(const void *buffer);
void write(FILE *out, double frames);

} // namespace proxy::vb_origins
