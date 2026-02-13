// Copyright 2022 Autodesk, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "pch.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "Resolver.h"
#include <pxr/usd/ar/defineResolver.h>

#if defined(_WIN32)
// The module entry point
// On Windows, we register the resolver in DllMain because TF_REGISTRY_FUNCTION
// static initializers may not run reliably when DLLs are loaded via LoadLibrary.
BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        // Register the resolver with ArResolver as the base class
        // Note: Using ArResolver (not ArDefaultResolver) to avoid TfType ordering issues
        pxr::Ar_DefineResolver<ImageProcessingResolverPlugin, pxr::ArResolver>();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#else
// On non-Windows platforms, use standard USD registration mechanism
PXR_NAMESPACE_USING_DIRECTIVE
AR_DEFINE_RESOLVER(ImageProcessingResolverPlugin, ArDefaultResolver);
#endif