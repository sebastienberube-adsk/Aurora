// Copyright 2023 Autodesk, Inc.
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

#include "Transpiler.h"
#include <slang.h>

BEGIN_AURORA

// Slang UUID compare operator, needed by casting operations.
bool operator==(const SlangUUID& aIn, const SlangUUID& bIn)
{
    // Use the largest type the honors the alignment of Guid
    typedef uint32_t CmpType;
    union GuidCompare
    {
        SlangUUID guid;
        CmpType data[sizeof(SlangUUID) / sizeof(CmpType)];
    };
    // Type pun - so compiler can 'see' the pun and not break aliasing rules
    const CmpType* a = reinterpret_cast<const GuidCompare&>(aIn).data;
    const CmpType* b = reinterpret_cast<const GuidCompare&>(bIn).data;
    // Make the guid comparison a single branch, by not using short circuit
    return ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3])) == 0;
}

// Slang string blob.  Simple wrapper around std::string that can be used by Slang compiler.
struct StringSlangBlob : public ISlangBlob, ISlangCastable
{
    StringSlangBlob(const string& str) : _str(str) {}
    virtual ~StringSlangBlob() = default;

    // Get a pointer to the string.
    virtual SLANG_NO_THROW void const* SLANG_MCALL getBufferPointer() override
    {
        return _str.data();
    }

    // Get the length of the string.
    virtual SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() override { return _str.length(); }

    // Default queryInterface implementation for Slang type system.
    virtual SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(
        SlangUUID const& uuid, void** outObject) SLANG_OVERRIDE
    {
        *outObject = getInterface(uuid);

        return *outObject ? SLANG_OK : SLANG_E_NOT_IMPLEMENTED;
    }

    // Do not implement ref counting, just return 1.
    virtual SLANG_NO_THROW uint32_t SLANG_MCALL addRef() SLANG_OVERRIDE { return 1; }
    virtual SLANG_NO_THROW uint32_t SLANG_MCALL release() SLANG_OVERRIDE { return 1; }

    // Default castAs implementation for Slang type system.
    void* castAs(const SlangUUID& guid) override { return getInterface(guid); }

    // Allow casting as Unknown and Blob, but nothing else.
    void* getInterface(const SlangUUID& uuid)
    {
        if (uuid == ISlangUnknown::getTypeGuid() || uuid == ISlangBlob::getTypeGuid())
        {
            return static_cast<ISlangBlob*>(this);
        }

        return nullptr;
    }

    // The actual string data.
    const string& _str;
};

// Slang filesystem that reads from a simple lookup table of strings.
struct AuroraSlangFileSystem : public ISlangFileSystem
{
    AuroraSlangFileSystem(const std::map<std::string, const std::string&>& fileText) :
        _fileText(fileText)
    {
    }
    virtual ~AuroraSlangFileSystem() = default;

    virtual SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(
        char const* path, ISlangBlob** outBlob) override
    {
        // Is if we already have blob for this path, return that.
        auto iter = _fileBlobs.find(path);
        if (iter != _fileBlobs.end())
        {
            *outBlob = iter->second.get();

            return SLANG_OK;
        }

        // Read from the text file map.
        auto shaderIter = _fileText.find(path);
        if (shaderIter != _fileText.end())
        {
            // Create a blob from the text file string, add to the blob map, and return it.
            _fileBlobs[path] = make_unique<StringSlangBlob>(shaderIter->second);
            *outBlob         = _fileBlobs[path].get();
            return SLANG_OK;
        }

        return SLANG_FAIL;
    }

    // Slang type interface not needed, just return null.
    virtual SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(
        SlangUUID const& /* uuid*/, void** /* outObject*/) SLANG_OVERRIDE
    {
        return SLANG_E_NOT_IMPLEMENTED;
    }

    // Do not implement ref counting, just return 1.
    virtual SLANG_NO_THROW uint32_t SLANG_MCALL addRef() SLANG_OVERRIDE { return 1; }
    virtual SLANG_NO_THROW uint32_t SLANG_MCALL release() SLANG_OVERRIDE { return 1; }

    // Slang type interface not needed, just return null.
    void* castAs(const SlangUUID&) override { return nullptr; }

    // Set a string directly as blob in file blobs map.
    void setSource(const string& name, const string& code)
    {
        _fileBlobs[name] = make_unique<StringSlangBlob>(code);
    }

    // Get source code by name (for debugging/logging).
    string getSource(const string& name) const
    {
        auto blobIter = _fileBlobs.find(name);
        if (blobIter != _fileBlobs.end())
        {
            return string((const char*)blobIter->second->getBufferPointer(), 
                         blobIter->second->getBufferSize());
        }
        auto textIter = _fileText.find(name);
        if (textIter != _fileText.end())
        {
            return textIter->second;
        }
        return "";
    }

    // Map of string blobs.
    map<string, unique_ptr<StringSlangBlob>> _fileBlobs;

    // Map of file strings.
    const std::map<std::string, const std::string&>& _fileText;
};

Transpiler::Transpiler(const std::map<std::string, const std::string&>& fileText)
{
    _pFileSystem = make_unique<AuroraSlangFileSystem>(fileText);
    _pSession    = spCreateSession();
}

Transpiler::~Transpiler()
{
    if (_pSession)
    {
        spDestroySession(_pSession);
        _pSession = nullptr;
    }
}

void Transpiler::setSource(const string& name, const string& code)
{
    _pFileSystem->setSource(name, code);
}

bool Transpiler::transpileCode(
    const string& shaderCode, string& codeOut, string& errorOut, Language target)
{
    // Dummy file name to use as container for shader code.
    const string codeFileName = "__shaderCode";

    // Set the shader code "file".
    setSource(codeFileName, shaderCode);

    // Transpile the shader code.
    bool res = transpile(codeFileName, codeOut, errorOut, target);

    // Clear the shader source to release memory.
    setSource(codeFileName, "");

    return res;
}

bool Transpiler::transpile(
    const string& shaderName, string& codeOut, string& errorOut, Language target)
{
    // Clear result.
    errorOut.clear();
    codeOut.clear();

    // Validate session pointer.
    if (!_pSession)
    {
        errorOut = "Slang session is null";
        AU_ERROR("Transpiler::transpile(%s) - _pSession is null!", shaderName.c_str());
        return false;
    }

    // IMPORTANT: All Slang ICompileRequest operations MUST use the C API wrappers (sp* functions)
    // instead of C++ virtual methods. The slang.h header and slang.dll have a vtable layout
    // mismatch for ICompileRequest — virtual calls dispatch to wrong functions and crash.
    // The sp* functions are direct DLL exports that bypass the vtable entirely.
    // This affects both spCreateSession() and slang::createGlobalSession() sessions equally.

    // Create compile request.
    SlangCompileRequest* pRequest = spCreateCompileRequest(_pSession);
    if (!pRequest)
    {
        errorOut = "Failed to create Slang compile request";
        AU_ERROR("Transpiler::transpile(%s) - spCreateCompileRequest returned null!",
                 shaderName.c_str());
        return false;
    }

    // Set the file system and compile flags.
    spSetFileSystem(pRequest, _pFileSystem.get());
    spSetCompileFlags(pRequest, SLANG_COMPILE_FLAG_NO_MANGLING);

    // Create code gen target (with GLSL or HLSL language as required).
    const int targetIndex =
        spAddCodeGenTarget(pRequest, target == Language::GLSL ? SLANG_GLSL : SLANG_HLSL);

    if (targetIndex < 0)
    {
        errorOut = "Failed to add code gen target";
        AU_ERROR("Transpiler::transpile(%s) - spAddCodeGenTarget returned %d",
                 shaderName.c_str(), targetIndex);
        spDestroyCompileRequest(pRequest);
        return false;
    }

    // Set target flags to generate whole program.
    spSetTargetFlags(pRequest, targetIndex, SLANG_TARGET_FLAG_GENERATE_WHOLE_PROGRAM);

    // Add translation unit from Slang to target language.
    const int translationUnitIndex =
        spAddTranslationUnit(pRequest, SLANG_SOURCE_LANGUAGE_SLANG, nullptr);

    if (translationUnitIndex < 0)
    {
        errorOut = "Failed to add translation unit";
        AU_ERROR("Transpiler::transpile(%s) - spAddTranslationUnit returned %d",
                 shaderName.c_str(), translationUnitIndex);
        spDestroyCompileRequest(pRequest);
        return false;
    }

    // Use standard line directives (with filename).
    spSetTargetLineDirectiveMode(pRequest, targetIndex, SLANG_LINE_DIRECTIVE_MODE_STANDARD);
    // Use column major matrix format.
    spSetMatrixLayoutMode(pRequest, SLANG_MATRIX_LAYOUT_COLUMN_MAJOR);

    // Set shader name as source file name (file system will look up it up from file text map).
    spAddTranslationUnitSourceFile(pRequest, translationUnitIndex, shaderName.c_str());

    // Set DIRECTX preprocessor directive.
    spAddPreprocessorDefine(pRequest, "DIRECTX", target == Language::GLSL ? "0" : "1");

    // Compile.
    SlangResult compileRes = spCompile(pRequest);

    if (SLANG_FAILED(compileRes))
    {
        const char* diagnostics = spGetDiagnosticOutput(pRequest);
        errorOut = (diagnostics && diagnostics[0]) ? diagnostics : "(no diagnostics available)";
        AU_ERROR("Slang compilation failed for %s (result=0x%08X):\n%s",
                 shaderName.c_str(), (unsigned int)compileRes, errorOut.c_str());
        spDestroyCompileRequest(pRequest);
        return false;
    }

    // Get blob for result.
    ISlangBlob* pOutBlob = nullptr;
    SlangResult blobRes = spGetTargetCodeBlob(pRequest, targetIndex, &pOutBlob);

    if (SLANG_FAILED(blobRes) || pOutBlob == nullptr)
    {
        const char* diagnostics = spGetDiagnosticOutput(pRequest);
        errorOut = (diagnostics && diagnostics[0]) ? diagnostics : "(no diagnostics available)";
        AU_ERROR("Failed to get target code blob for %s (result=0x%08X).\nDiagnostics:\n%s",
                 shaderName.c_str(), (unsigned int)blobRes, errorOut.c_str());
        spDestroyCompileRequest(pRequest);
        return false;
    }

    // Get the buffer pointer.
    const char* pBuffer = (const char*)pOutBlob->getBufferPointer();
    if (pBuffer == nullptr)
    {
        AU_ERROR("Target code blob buffer pointer is null for %s", shaderName.c_str());
        spDestroyCompileRequest(pRequest);
        return false;
    }

    codeOut = pBuffer;

    // Destroy compile request.
    spDestroyCompileRequest(pRequest);

    return true;
}

END_AURORA
