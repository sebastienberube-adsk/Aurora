# How Material Shaders Work in Aurora's HGI Ray Tracing Pipeline

## The Core Problem

In rasterization, you simply bind a different pixel shader and set of resources before each draw call. In ray tracing there is no "draw call per object" — you issue a single `TraceRays` dispatch that covers the entire screen, and the GPU must figure out *which* shader code and *which* material data to use when a ray hits each piece of geometry. The **Shader Binding Table (SBT)** is the data structure that makes this possible.

## Background: What is a Shader Binding Table?

An SBT is a GPU buffer containing a sequence of **shader records**. Each record consists of:

1. **A shader group handle** — an opaque, driver-provided blob (typically 32 bytes) that the GPU uses to jump to the correct compiled shader code.
2. **Inline user data** (the "shader record data") — arbitrary bytes placed immediately after the handle, which become accessible as a special read-only buffer in the shader.

The SBT is divided into regions: **ray generation**, **miss**, and **hit groups**. When a ray hits geometry, the GPU computes which hit group record to use based on a formula involving the instance index and ray type, then executes the shader from that record — with the per-record data available.

## How Aurora Populates the SBT (CPU Side)

The entire flow is orchestrated by `HGIScene`. When the scene changes, it calls:

1. `rebuildInstanceList()` — gathers per-instance data
2. `rebuildPipeline()` — compiles shaders and creates the RT pipeline with SBT records
3. `rebuildAccelerationStructure()` — builds TLAS with per-instance SBT offsets
4. `rebuildResourceBindings()` — binds global resources (textures, UBOs, TLAS)

### Step 1: Building Per-Instance Shader Records

In `rebuildInstanceList()`, for each scene instance, Aurora builds an `InstanceShaderRecord` struct (defined in `Libraries/Aurora/Source/HGI/HGIScene.h`):

```cpp
struct InstanceShaderRecord
{
    // Geometry data.
    HGIGeometryBuffers geometry;

    // Index into material array.
    uint64_t material;

    // Index into texture sampler array for material's textures.
    int baseColorTextureIndex = -1;
    int specularRoughnessTextureIndex = -1;
    int normalTextureIndex            = -1;
    int opacityTextureIndex           = -1;

    // Geometry flags.
    unsigned int hasNormals   = true;
    unsigned int hasTangents  = false;
    unsigned int hasTexCoords = true;
};
```

This struct is filled by copying **GPU device addresses** of the instance's vertex/index/normal buffers from the geometry, and the material UBO's device address, plus texture array indices (from `Libraries/Aurora/Source/HGI/HGIScene.cpp`):

```cpp
// Create a hit group shader record for instance.
InstanceShaderRecord record;
record.geometry = instance.hgiGeometry()->bufferAddresses;
record.material = pMtl->ubo()->GetDeviceAddress();
// ...
if (pBaseColorImage)
{
    record.baseColorTextureIndex = (int)_lstImages.size();
    _lstImages.push_back(pBaseColorImage);
}
// ... similar for specular roughness, opacity, normal textures ...
```

So each instance's shader record carries: pointers to its **geometry buffers**, a pointer to its **material constants UBO**, and **indices** into a global texture array for each texture slot.

### Step 2: Attaching Shader Records to Pipeline Groups

In `rebuildPipeline()`, the pipeline is created with 5 compiled shaders (raygen, 3 miss, 1 closest-hit) and N+4 shader groups. Groups 0–3 are for raygen and miss. For each instance `i`, a triangle hit group is created pointing to the **same** closest-hit shader (index 4) but with a **unique shader record payload** (from `Libraries/Aurora/Source/HGI/HGIScene.cpp`):

```cpp
// Triangle shader groups for each instance.
for (size_t i = 0; i < _lstInstances.size(); i++)
{
    size_t shaderRecordStride = sizeof(_lstInstances[i].shaderRecord);
    // Triangle miss shader group with closest hit shader.
    pipelineDesc.groups[4 + i].type             = HgiRayTracingShaderGroupTypeTriangles;
    pipelineDesc.groups[4 + i].closestHitShader = 4; // Index within shader array above.
    // Add the hit group record structure to the shader record
    pipelineDesc.groups[4 + i].pShaderRecord      = &_lstInstances[i].shaderRecord;
    pipelineDesc.groups[4 + i].shaderRecordLength = shaderRecordStride;
}
```

**Key insight**: All instances share the **same compiled shader code** (the closest-hit shader at index 4), but each has a different **shader record payload** containing that instance's specific geometry pointers, material UBO address, and texture indices.

### Step 3: The Vulkan Backend Builds the Actual SBT Buffer

The Hgi Vulkan backend (`HgiVulkanRayTracingPipeline::BuildShaderBindingTable()` in `USD_src/pxr/imaging/hgiVulkan/rayTracingPipeline.cpp`) does the low-level work:

1. Queries driver shader group handles via `vkGetRayTracingShaderGroupHandlesKHR`
2. For each group, writes into the appropriate SBT region:
   - The **shader handle** (driver blob)
   - The **shader record data** (the `InstanceShaderRecord` bytes from `pShaderRecord`/`shaderRecordLength`)
   - Alignment padding to meet the required stride

```cpp
void HgiVulkanRayTracingPipeline::WriteShaderGroup(...) {
    // ... copy aligned shader handle ...
    // Copy shader record buffer after the shader handle.
    if (group.shaderRecordLength) {
        size_t bufferIndex = buffer.size();
        buffer.resize(bufferIndex + group.shaderRecordLength);
        memcpy((void*)&buffer[bufferIndex], group.pShaderRecord, group.shaderRecordLength);
    }
    // Pad buffer to ensure correct alignment.
    // ...
}
```

The result is three GPU buffers (raygen SBT, miss SBT, hit SBT) that are passed to `vkCmdTraceRaysKHR`.

### Step 4: Linking TLAS Instances to SBT Entries

In `rebuildAccelerationStructure()`, each TLAS instance is given a `groupIndex` equal to its index `i` (from `Libraries/Aurora/Source/HGI/HGIScene.cpp`):

```cpp
inst.groupIndex = static_cast<uint32_t>(i);
```

The Vulkan backend maps this to `VkAccelerationStructureInstanceKHR::instanceShaderBindingTableRecordOffset` (from `USD_src/pxr/imaging/hgiVulkan/accelerationStructure.cpp`):

```cpp
instances[i].instanceShaderBindingTableRecordOffset = desc.instances[i].groupIndex;
```

This is the crucial link: when a ray hits TLAS instance `i`, the GPU uses this offset to index into the hit SBT region, landing on the record that contains instance `i`'s geometry + material data.

## How It Works on the GPU (Shader Side)

The closest-hit shader sees the per-instance data through the Vulkan `shaderRecordEXT` storage block, declared in `Libraries/Aurora/Source/HGI/Shaders/InstanceData.glsl`:

```glsl
// Shader record for hit shaders. Must match HitGroupShaderRecord struct in HGIScene.h.
layout(shaderRecordEXT, std430) buffer InstanceShaderRecord
{
    // Geometry data.
    Indices indices;
    Positions positions;
    Normals normals;
    Tangents tangents;
    TexCoords texcoords;

    // Material data.
    Materials material;

    // Index into texture sampler array for material's textures.
    int baseColorTextureIndex;
    int specularRoughnessTextureIndex;
    int normalTextureIndex;
    int opacityTextureIndex;

    // Geometry flags.
    uint hasNormals;
    uint hasTangents;
    uint hasTexCoords;
} instance;
```

The first five fields (`indices`, `positions`, etc.) are **buffer reference** types — i.e., GPU pointers (64-bit device addresses) that point directly into that instance's vertex/index buffers. The `material` field is also a buffer reference pointing to the material's UBO.

This file also provides the concrete implementations of the forward-declared accessor functions that the shared Slang path-tracing code calls:

```glsl
// Implementation for forward declared material accessor function in Material.hlsli.
MaterialConstants getMaterial() {
    return instance.material.m[0];
}

// Implementation for forward declared texture sample function in Material.hlsli.
vec4 sampleBaseColorTexture(vec2 uv, float level) {
    return texture(textureSamplers[nonuniformEXT(instance.baseColorTextureIndex)], uv);
}
// ... similar for specular roughness, normal, opacity textures ...
```

## The Full Material Evaluation Chain

When a ray hits instance `i`, here is the chain:

1. **GPU selects SBT record** `hit[i]` based on `instanceShaderBindingTableRecordOffset`
2. **Closest-hit shader starts** — the `shaderRecordEXT` block is automatically populated from the SBT record for instance `i`
3. The entry point (from `MainEntryPoints.slang`) calls **`initializeMaterial()`** — a Slang function from `InitializeDefaultMaterialType.slang`
4. `initializeMaterial()` calls **`getMaterial()`** — which is forward-declared in `Material.slang` and **implemented in `InstanceData.glsl`** as `instance.material.m[0]`, i.e. it dereferences the device-address pointer from the SBT to load the `MaterialConstants` struct from the instance's UBO
5. The `MaterialConstants` values are used to populate a `Material` struct with all StandardSurface properties (base color, roughness, metalness, etc.)
6. If textures are present (e.g. `hasBaseColorTex`), `sampleBaseColorTexture()` is called — also implemented in `InstanceData.glsl` — which indexes into the **global combined sampler/texture array** (binding 3) using the `baseColorTextureIndex` from the SBT record, with `nonuniformEXT` to handle non-uniform indexing
7. The populated `Material` struct is passed to the **BSDF evaluation and shading functions** (`shadeEmission`, `shadeDirectionalLight`, `shadeIndirectLight`, etc.)

## Visual Summary

```
                     ┌─────────────────────────────────────┐
  vkCmdTraceRaysKHR  │   Hit Shader Binding Table (SBT)    │
  ─────────────────► │                                     │
                     │  Record 0: [handle][ShaderRecord₀]  │──► Instance 0's data
                     │  Record 1: [handle][ShaderRecord₁]  │──► Instance 1's data
                     │  Record 2: [handle][ShaderRecord₂]  │──► Instance 2's data
                     │  ...                                 │
                     └────────────────┬────────────────────┘
                                      │
  When ray hits instance i:           │ GPU indexes by
  TLAS instance i has                 │ instanceSBTRecordOffset = i
  SBT offset = i ─────────────────────┘
                                      │
                                      ▼
                     ┌────────────────────────────────────────┐
                     │  ShaderRecord_i (shaderRecordEXT)      │
                     │  ┌──────────────────────────────────┐  │
                     │  │ indices    → GPU addr of IB      │  │
                     │  │ positions  → GPU addr of VB      │  │
                     │  │ normals    → GPU addr of NB      │  │
                     │  │ texcoords  → GPU addr of UVB     │  │
                     │  │ material   → GPU addr of UBO     │──┼──► MaterialConstants
                     │  │ baseColorTexIdx = 3              │──┼──► textureSamplers[3]
                     │  │ specRoughnessTexIdx = 7          │──┼──► textureSamplers[7]
                     │  │ hasNormals = 1                   │  │
                     │  └──────────────────────────────────┘  │
                     └────────────────────────────────────────┘
                                      │
                     Closest-hit shader (same code for all instances):
                        getMaterial()     → dereferences material pointer → MaterialConstants
                        sampleBaseColor() → textureSamplers[baseColorTexIdx]
                        initializeMaterial() → populates Material struct
                        shade...() functions → BSDF eval, lighting, indirect
```

## Key Design Points

1. **One shader, many records**: Aurora uses a *single* compiled closest-hit shader for all instances. The per-material variation is entirely in the **data** (UBO contents + texture indices), not in code. This is simpler than having unique shader variants per material type.

2. **Buffer Device Addresses (BDA)**: Geometry and material data are accessed via 64-bit GPU pointers stored directly in the SBT record, using `GL_EXT_buffer_reference`. This avoids needing separate descriptor set bindings for each instance.

3. **Bindless Textures**: All textures from all instances are collected into a single `sampler2D textureSamplers[]` array (binding 3). Each SBT record stores integer indices into this array. `nonuniformEXT` is used because different invocations in a shader wave/subgroup may index different textures.

4. **Slang Transpilation**: Aurora's material shaders are written in Slang (a HLSL-like language). The Slang `Transpiler` transpiles them to GLSL for the Vulkan/HGI backend. The transpiled code forward-declares accessor functions (`getMaterial()`, `sampleBaseColorTexture()`, etc.) that are implemented in raw GLSL (`InstanceData.glsl`) and appended to the transpiled code before compilation.

5. **DirectX comparison**: On the DirectX backend, the same conceptual approach is used but with DXR's local root signatures and `ByteAddressBuffer` instead of Vulkan's `shaderRecordEXT` and buffer device addresses. The `Material.slang` file uses `#if DIRECTX` to switch between the two access patterns.

## Key Source Files

| File | Role |
|------|------|
| `Libraries/Aurora/Source/HGI/HGIScene.h` | Defines `InstanceShaderRecord`, `HGIInstance`, `HGIScene` |
| `Libraries/Aurora/Source/HGI/HGIScene.cpp` | Builds SBT records, pipeline, TLAS, resource bindings |
| `Libraries/Aurora/Source/HGI/HGIGeometry.h` | Defines `HGIGeometryBuffers` (device addresses for vertex data) |
| `Libraries/Aurora/Source/HGI/HGIMaterial.h/.cpp` | Material UBO creation and upload |
| `Libraries/Aurora/Source/HGI/Shaders/InstanceData.glsl` | GPU-side `shaderRecordEXT` declaration and accessor implementations |
| `Libraries/Aurora/Source/Shaders/Material.slang` | Forward-declares `getMaterial()` and texture samplers; defines `initializeDefaultMaterial()` |
| `Libraries/Aurora/Source/Shaders/DefaultMaterialUniformBuffer.slang` | Auto-generated `MaterialConstants` struct and accessor functions |
| `Libraries/Aurora/Source/Shaders/InitializeDefaultMaterialType.slang` | Bridges `initializeMaterial()` to `initializeDefaultMaterial()` |
| `Libraries/Aurora/Source/Shaders/MainEntryPoints.slang` | Closest-hit / any-hit / miss entry point template |
| `USD_src/pxr/imaging/hgi/rayTracingPipeline.h` | Hgi-level pipeline descriptor (groups, shader records) |
| `USD_src/pxr/imaging/hgiVulkan/rayTracingPipeline.cpp` | Vulkan SBT construction from pipeline groups |
| `USD_src/pxr/imaging/hgiVulkan/rayTracingCmds.cpp` | `vkCmdTraceRaysKHR` dispatch with SBT regions |
| `USD_src/pxr/imaging/hgiVulkan/accelerationStructure.cpp` | Maps `groupIndex` to `instanceShaderBindingTableRecordOffset` |
