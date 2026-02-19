# Material Shader Architecture in Aurora

## Overview

Aurora supports two renderer backends — **DirectX** (DXR) and **HGI** (Vulkan via USD's Hgi abstraction). Both use the same shared Slang shader code (`MainEntryPoints.slang`) and the same `MaterialShaderSource` system for defining material types. However, the DirectX backend fully supports multiple material shader types (including procedural patterns), while the HGI backend currently only compiles a single closest-hit shader for all instances.

This document explains:

1. How the material UBO is populated (common to both backends)
2. How the `initializeMaterial()` customization hook works
3. How the DirectX backend compiles multiple material types
4. What the HGI backend currently does (and what it's missing)
5. How you would implement a procedural material shader

## How the Material UBO Is Populated

Material property values flow through three stages before reaching the GPU:

### Stage 1: Property Setters → CPU-Side `UniformBuffer`

When a property is set on a material (e.g., from HdAurora or the Aurora public API), it goes through `MaterialBase` (`Libraries/Aurora/Source/MaterialBase.h`):

```cpp
void setFloat(const string& name, float value) override
{
    _uniformBuffer.set(name, value);
    _bIsDirty = true;
}

void setFloat3(const string& name, const float* value) override
{
    _uniformBuffer.set(name, glm::make_vec3(value));
    _bIsDirty = true;
}
```

The `UniformBuffer` class (`Libraries/Aurora/Source/UniformBuffer.h`) packs values at the correct byte offset in an internal `vector<uint32_t> _data` — a CPU-side byte blob whose layout matches the GPU's `MaterialConstants` struct (defined in `DefaultMaterialUniformBuffer.slang`). Each property's offset and alignment are computed from the `UniformBufferDefinition`.

### Stage 2: CPU Buffer → GPU Staging → GPU Upload

In `HGIMaterial::update()` (`Libraries/Aurora/Source/HGI/HGIMaterial.cpp`):

```cpp
void HGIMaterial::update()
{
    // Copy uniform buffer to staging memory.
    void* pStaging = _ubo->handle()->GetCPUStagingAddress();
    ::memcpy_s(pStaging, uniformBuffer().size(), uniformBuffer().data(), uniformBuffer().size());

    // Transfer staging buffer to GPU via blit command.
    pxr::HgiBlitCmdsUniquePtr blitCmds = _pRenderer->hgi()->CreateBlitCmds();
    pxr::HgiBufferCpuToGpuOp blitOp;
    blitOp.byteSize              = uniformBuffer().size();
    blitOp.cpuSourceBuffer       = pStaging;
    blitOp.gpuDestinationBuffer  = _ubo->handle();
    blitCmds->CopyBufferCpuToGpu(blitOp);
    _pRenderer->hgi()->SubmitCmds(blitCmds.get());
}
```

### Stage 3: Scene Update Loop Triggers `update()` for Dirty Materials

In `HGIScene::update()` (`Libraries/Aurora/Source/HGI/HGIScene.cpp`):

```cpp
if (_materials.changedThisFrame())
{
    for (HGIMaterial& mtl : _materials.modified().resources<HGIMaterial>())
    {
        mtl.update();
    }
}
```

### Debugging Breakpoints

| What you want to see | File | Line | What to inspect |
|---|---|---|---|
| Individual property being set | `MaterialBase.h` | `setFloat` / `setFloat3` etc. | `name`, `value` |
| Full packed CPU buffer before GPU upload | `HGIMaterial.cpp` | `memcpy_s` line | Cast `uniformBuffer().data()` to `MaterialConstants*` |
| Which materials are being updated this frame | `HGIScene.cpp` | `mtl.update()` line | `mtl` object |

## The `initializeMaterial()` Customization Hook

The key to supporting different material types is the `initializeMaterial()` function. In `MainEntryPoints.slang` (`Libraries/Aurora/Source/Shaders/MainEntryPoints.slang`), the closest-hit entry point calls:

```hlsl
Material material =
    initializeMaterial(shading, ObjectToWorld3x4(), materialNormal, isGeneratedNormal);
```

This function is **not hardcoded** — it's brought in via `#include "InitializeMaterial.slang"`, and that file is **substituted at compile time** with material-specific code. This is the designated customization point: everything downstream (BSDF evaluation, lighting, indirect rays) operates on the returned `Material` struct and doesn't care how the values were produced.

### The Default Implementation

For the built-in Standard Surface material, `InitializeMaterial.slang` simply delegates to `initializeDefaultMaterial()`:

```hlsl
// From InitializeDefaultMaterialType.slang
Material initializeMaterial(ShadingData shading,
    float3x4 objToWorld,
    out float3 materialNormal, out bool isGeneratedNormal)
{
    return initializeDefaultMaterial(
        shading, objToWorld, materialNormal, isGeneratedNormal);
}
```

And `initializeDefaultMaterial()` (in `Material.slang`) reads all properties from the `MaterialConstants` UBO and samples any bound textures.

## The `MaterialShaderSource` System

Each material type is described by a `MaterialShaderSource` struct (`Libraries/Aurora/Source/MaterialShader.h`):

```cpp
struct MaterialShaderSource
{
    // Unique name (e.g., "Default", "Checker", "WoodGrain")
    string uniqueId;

    // Shader source for material setup — becomes InitializeMaterial.slang
    string setup;

    // Optional shader source for bsdf customization
    string bsdf;

    // Optional function definitions
    string definitions;
};
```

The `setup` field is the critical one — it contains the Slang source code for `initializeMaterial()` that will be injected into the closest-hit shader at compile time.

## The `___Material___` Template Tag

`MainEntryPoints.slang` uses a placeholder tag `___Material___` in all its entry point names:

```hlsl
[shader("closesthit")] void ___Material___RadianceHitShader(
    inout RayPayload rayPayload, in BuiltInTriangleIntersectionAttributes hit) {
    // ...
}

[shader("anyhit")] void ___Material___ShadowAnyHitShader(
    inout ShadowRayPayload rayPayload, in BuiltInTriangleIntersectionAttributes hit) {
    // ...
}
```

At compile time, this tag is replaced with the material's `uniqueId`, producing uniquely named entry points like `DefaultRadianceHitShader`, `CheckerRadianceHitShader`, `WoodGrainRadianceHitShader`, etc.

## DirectX Backend: Full Multi-Material Support

The DirectX backend (`Libraries/Aurora/Source/DirectX/PTShaderLibrary.cpp`) fully implements the multi-material-type pipeline.

### Compilation: One Shader Library Per Material Type

For each unique `MaterialShader`, the DX backend calls `setupCompileJobForShader()`:

```cpp
void PTShaderLibrary::setupCompileJobForShader(const MaterialShader& shader, CompileJob& jobOut)
{
    auto& source = shader.definition().source;

    // Enable/disable entry points based on ref-counts.
    jobOut.code = "#define RADIANCE_HIT " +
        to_string(shader.hasEntryPoint(EntryPointTypes::kRadianceHit)) + "\n";
    // ... more defines ...

    // Replace ___Material___ with the unique material name.
    string entryPointSource =
        regex_replace(CommonShaders::g_sMainEntryPoints, regex("___Material___"), source.uniqueId);
    jobOut.code += entryPointSource;

    // Inject the material-specific InitializeMaterial.slang code.
    jobOut.includes = {
        { "InitializeMaterial.slang", source.setup },       // <-- CUSTOM per material type
        { "Options.slang", _optionsSource },
        { "Definitions.slang", source.definitions },        // <-- CUSTOM per material type
    };

    // Each shader gets unique entry point names.
    jobOut.entryPoints = {
        { "closesthit", compiledShader.entryPoints[EntryPointTypes::kRadianceHit] },
        { "anyhit",     compiledShader.entryPoints[EntryPointTypes::kShadowAnyHit] },
        { "miss",       compiledShader.entryPoints[EntryPointTypes::kLayerMiss] }
    };
}
```

### Pipeline: One DXR Hit Group Per Material Type

Each compiled material shader becomes a separate DXR hit group:

```cpp
// Create hit group for this material type
auto* pShaderSubobject =
    pipelineStateDesc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
pShaderSubobject->SetHitGroupExport(Foundation::s2w(compiledShader.exportName).c_str());

// Set the closest-hit shader import to this material's unique entry point
pShaderSubobject->SetClosestHitShaderImport(
    Foundation::s2w(compiledShader.entryPoints[EntryPointTypes::kRadianceHit]).c_str());
```

### SBT: Per-Instance Hit Group Selection

When building the SBT (shader binding table), each instance's hit group record references the correct hit group for its material type. Instances with a "WoodGrain" material get the `WoodGrainRadianceHitShaderGroup` handle; instances with a "Checker" material get the `CheckerRadianceHitShaderGroup` handle.

### Parallel Compilation

The DX backend compiles material shaders in parallel across threads:

```cpp
for_each(execution::par, compileJobs.begin(), compileJobs.end(),
    [compileFunc](CompileJob& job) { compileFunc(job); });
```

All compiled shader libraries are then linked into a single DXIL blob and used to create the pipeline state object.

## HGI Backend: Single Material Shader (Current Limitation)

The HGI backend (`Libraries/Aurora/Source/HGI/HGIScene.cpp`) currently hardcodes a single closest-hit shader for all instances.

### What It Does

In `createResources()`:

```cpp
// Create the closest hit shader from template text — always "Default"
string mainEntryPointSource = "#define RADIANCE_HIT 1\n" +
    regex_replace(CommonShaders::g_sMainEntryPoints, regex("___Material___"), "Default");

// Always inject the default material's InitializeMaterial.slang
_transpiler->setSource(
    "InitializeMaterial.slang", CommonShaders::g_sInitializeDefaultMaterialType);
_transpiler->setSource("Options.slang", "");
_transpiler->setSource("Definitions.slang", "");

// Transpile and compile ONE closest-hit shader
// ...
string closestHitShaderCode = transpiledGLSL + HGIShaders::g_sInstanceData;
```

In `rebuildPipeline()`:

```cpp
// ALL instances point to the same shader (index 4)
for (size_t i = 0; i < _lstInstances.size(); i++)
{
    pipelineDesc.groups[4 + i].type             = HgiRayTracingShaderGroupTypeTriangles;
    pipelineDesc.groups[4 + i].closestHitShader = 4; // Same for every instance
    pipelineDesc.groups[4 + i].pShaderRecord    = &_lstInstances[i].shaderRecord;
    // ...
}
```

### What It's Missing

The `MaterialShaderSource.setup` field exists and is populated for custom material types, but the HGI scene never reads it. All instances get the default `initializeMaterial()` implementation regardless of their assigned material type.

## Comparison Table

| Feature | DirectX Backend | HGI Backend (current) |
|---|---|---|
| Multiple material shader types | ✅ One compiled hit group per type | ❌ One hardcoded closest-hit shader |
| Procedural patterns in shader code | ✅ Via custom `InitializeMaterial.slang` | ❌ Not supported yet |
| Per-material `initializeMaterial()` | ✅ Injected per compile job | ❌ Always uses `InitializeDefaultMaterialType` |
| `___Material___` tag replacement | ✅ Per unique material ID | ❌ Always replaced with `"Default"` |
| SBT hit groups with different shader handles | ✅ Different handle per material type | ❌ Same handle for all instances |
| Parallel shader compilation | ✅ `std::execution::par` | ❌ N/A (only one shader) |
| The extension point (`MaterialShaderSource`) | ✅ Fully used | ✅ Exists but not wired up |

## How to Extend the HGI Backend for Procedural Materials

To support procedural material shaders in the HGI backend, you would need to modify `HGIScene::createResources()` and `HGIScene::rebuildPipeline()`:

### Step 1: Compile Multiple Closest-Hit Shaders

In `createResources()`, instead of compiling one closest-hit shader, loop through all active material shader types:

```cpp
// Pseudocode — for each unique material shader type:
for (auto& materialShader : activeMaterialShaders)
{
    auto& source = materialShader.definition().source;

    string mainEntryPointSource = "#define RADIANCE_HIT 1\n" +
        regex_replace(CommonShaders::g_sMainEntryPoints,
                      regex("___Material___"), source.uniqueId);

    _transpiler->setSource("InitializeMaterial.slang", source.setup);       // CUSTOM
    _transpiler->setSource("Options.slang", "");
    _transpiler->setSource("Definitions.slang", source.definitions);        // CUSTOM

    string transpiledGLSL, transpilerErrors;
    _transpiler->transpileCode(mainEntryPointSource, transpiledGLSL,
                               transpilerErrors, Transpiler::Language::GLSL);

    string closestHitCode = transpiledGLSL + HGIShaders::g_sInstanceData;

    // Compile and store in a map: materialShader.id() → HgiShaderFunctionHandle
    // ...
}
```

### Step 2: Register All Shaders in the Pipeline

The `pipelineDesc.shaders` array would contain all unique closest-hit shaders:

```
Index 0: RayGen
Index 1: BackgroundMiss
Index 2: RadianceMiss
Index 3: ShadowMiss
Index 4: Default closest-hit
Index 5: Checker closest-hit
Index 6: WoodGrain closest-hit
...
```

### Step 3: Point Each Instance to Its Material's Shader

Instead of hardcoding `closestHitShader = 4`:

```cpp
for (size_t i = 0; i < _lstInstances.size(); i++)
{
    int shaderIdx = shaderIndexForMaterial[_lstInstances[i].instance.material()->shader()->id()];
    pipelineDesc.groups[4 + i].closestHitShader = shaderIdx;
    pipelineDesc.groups[4 + i].pShaderRecord    = &_lstInstances[i].shaderRecord;
    // ...
}
```

The Vulkan SBT would then naturally contain **different shader handles** per hit group entry. Instances using the checker material would get the checker shader's handle in their SBT record.

## Example: A Procedural Checker Material

A custom `InitializeMaterial.slang` for a checker pattern:

```hlsl
Material initializeMaterial(ShadingData shading,
    float3x4 objToWorld,
    out float3 materialNormal, out bool isGeneratedNormal)
{
    // Start with the default material (reads UBO properties)
    Material material = initializeDefaultMaterial(
        shading, objToWorld, materialNormal, isGeneratedNormal);

    // Procedural checker: override baseColor based on UV coordinates
    float2 uv = shading.texCoord * 10.0; // scale factor
    bool checker = (fmod(floor(uv.x) + floor(uv.y), 2.0) == 0.0);
    material.baseColor = checker ? float3(0.1, 0.1, 0.1) : float3(0.9, 0.9, 0.9);

    return material;
}
```

This works because:

- `initializeMaterial()` is the **only** function that `MainEntryPoints.slang` calls for material setup
- Everything downstream (BSDF evaluation, lighting, indirect rays) operates on the returned `Material` struct
- You can call `initializeDefaultMaterial()` first to get all the standard UBO-driven properties, then override specific fields with procedural logic
- Or you can bypass the default entirely and compute everything procedurally

## Key Source Files

| File | Role |
|---|---|
| `Libraries/Aurora/Source/MaterialShader.h` | Defines `MaterialShaderSource`, `MaterialShaderDefinition`, `MaterialShader`, `MaterialShaderLibrary` |
| `Libraries/Aurora/Source/MaterialBase.h` | Property setters that write to the `UniformBuffer` |
| `Libraries/Aurora/Source/UniformBuffer.h` | CPU-side byte buffer matching GPU `MaterialConstants` layout |
| `Libraries/Aurora/Source/HGI/HGIMaterial.cpp` | GPU UBO creation and upload for HGI backend |
| `Libraries/Aurora/Source/HGI/HGIScene.cpp` | HGI pipeline/SBT construction (single closest-hit shader) |
| `Libraries/Aurora/Source/DirectX/PTShaderLibrary.cpp` | DX pipeline/SBT construction (multi-material support) |
| `Libraries/Aurora/Source/Shaders/MainEntryPoints.slang` | Entry point template with `___Material___` tag |
| `Libraries/Aurora/Source/Shaders/InitializeDefaultMaterialType.slang` | Default `initializeMaterial()` implementation |
| `Libraries/Aurora/Source/Shaders/Material.slang` | `initializeDefaultMaterial()`, `Material` struct, texture samplers |
| `Libraries/Aurora/Source/Shaders/DefaultMaterialUniformBuffer.slang` | Auto-generated `MaterialConstants` struct and accessors |
| `Libraries/Aurora/Source/HGI/Shaders/InstanceData.glsl` | GPU-side `shaderRecordEXT` declaration and accessor implementations |
