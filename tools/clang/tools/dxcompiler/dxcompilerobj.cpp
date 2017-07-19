///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// dxcompilerobj.cpp                                                         //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// Implements the DirectX Compiler.                                          //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/HLSLMacroExpander.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Sema/SemaHLSL.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "llvm/IR/LLVMContext.h"
#include "dxc/Support/WinIncludes.h"
#include "dxc/HLSL/HLSLExtensionsCodegenHelper.h"
#include "dxc/HLSL/DxilRootSignature.h"
#include "dxcutil.h"
#include "dxc/Support/dxcfilesystem.h"

// SPIRV change starts
#ifdef ENABLE_SPIRV_CODEGEN
#include "clang/SPIRV/EmitSPIRVAction.h"
#endif
// SPIRV change ends


#include "dxc/Support/WinIncludes.h"
#include "dxc/HLSL/DxilContainer.h"

#include "dxc/dxcapi.internal.h"

#include "dxc/Support/dxcapi.use.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/Unicode.h"
#include "dxc/Support/microcom.h"
#include "dxc/Support/FileIOHelper.h"
#include "dxc/Support/dxcapi.impl.h"
#include "dxc/Support/DxcLangExtensionsHelper.h"
#include "dxc/Support/HLSLOptions.h"
#include "dxcetw.h"
#include "dxillib.h"
#include <algorithm>

#define CP_UTF16 1200

using namespace llvm;
using namespace clang;
using namespace hlsl;
using std::string;

// This declaration is used for the locally-linked validator.
HRESULT CreateDxcValidator(_In_ REFIID riid, _Out_ LPVOID *ppv);

// This internal call allows the validator to avoid having to re-deserialize
// the module. It trusts that the caller didn't make any changes and is
// kept internal because the layout of the module class may change based
// on changes across modules, or picking a different compiler version or CRT.
HRESULT RunInternalValidator(_In_ IDxcValidator *pValidator,
                             _In_ llvm::Module *pModule,
                             _In_ llvm::Module *pDebugModule,
                             _In_ IDxcBlob *pShader, UINT32 Flags,
                             _In_ IDxcOperationResult **ppResult);

static void CreateOperationResultFromOutputs(
    IDxcBlob *pResultBlob, dxcutil::DxcArgsFileSystem *msfPtr,
    const std::string &warnings, clang::DiagnosticsEngine &diags,
    _COM_Outptr_ IDxcOperationResult **ppResult) {
  CComPtr<IStream> pErrorStream;
  CComPtr<IDxcBlobEncoding> pErrorBlob;
  msfPtr->GetStdOutpuHandleStream(&pErrorStream);
  dxcutil::CreateOperationResultFromOutputs(pResultBlob, pErrorStream, warnings,
                                            diags.hasErrorOccurred(), ppResult);
}

static void CreateOperationResultFromOutputs(
    AbstractMemoryStream *pOutputStream, dxcutil::DxcArgsFileSystem *msfPtr,
    const std::string &warnings, clang::DiagnosticsEngine &diags,
    _COM_Outptr_ IDxcOperationResult **ppResult) {
  CComPtr<IDxcBlob> pResultBlob;
  IFT(pOutputStream->QueryInterface(&pResultBlob));
  CreateOperationResultFromOutputs(pResultBlob, msfPtr, warnings, diags,
                                   ppResult);
}

class HLSLExtensionsCodegenHelperImpl : public HLSLExtensionsCodegenHelper {
private:
  CompilerInstance &m_CI;
  DxcLangExtensionsHelper &m_langExtensionsHelper;
  std::string m_rootSigDefine;

  // The metadata format is a root node that has pointers to metadata
  // nodes for each define. The metatdata node for a define is a pair
  // of (name, value) metadata strings.
  //
  // Example:
  // !hlsl.semdefs = {!0, !1}
  // !0 = !{!"FOO", !"BAR"}
  // !1 = !{!"BOO", !"HOO"}
  void WriteSemanticDefines(llvm::Module *M, const ParsedSemanticDefineList &defines) {
    // Create all metadata nodes for each define. Each node is a (name, value) pair.
    std::vector<MDNode *> mdNodes;
    for (const ParsedSemanticDefine &define : defines) {
      MDString *name  = MDString::get(M->getContext(), define.Name);
      MDString *value = MDString::get(M->getContext(), define.Value);
      mdNodes.push_back(MDNode::get(M->getContext(), { name, value }));
    }

    // Add root node with pointers to all define metadata nodes.
    NamedMDNode *Root = M->getOrInsertNamedMetadata(m_langExtensionsHelper.GetSemanticDefineMetadataName());
    for (MDNode *node : mdNodes)
      Root->addOperand(node);
  }

  SemanticDefineErrorList GetValidatedSemanticDefines(const ParsedSemanticDefineList &defines, ParsedSemanticDefineList &validated, SemanticDefineErrorList &errors) {
    for (const ParsedSemanticDefine &define : defines) {
      DxcLangExtensionsHelper::SemanticDefineValidationResult result = m_langExtensionsHelper.ValidateSemanticDefine(define.Name, define.Value);
        if (result.HasError())
          errors.emplace_back(SemanticDefineError(define.Location, SemanticDefineError::Level::Error, result.Error));
        if (result.HasWarning())
          errors.emplace_back(SemanticDefineError(define.Location, SemanticDefineError::Level::Warning, result.Warning));
        if (!result.HasError())
          validated.emplace_back(define);
    }

    return errors;
  }

public:
  HLSLExtensionsCodegenHelperImpl(CompilerInstance &CI, DxcLangExtensionsHelper &langExtensionsHelper, StringRef rootSigDefine)
  : m_CI(CI), m_langExtensionsHelper(langExtensionsHelper)
  , m_rootSigDefine(rootSigDefine)
  {}

  // Write semantic defines as metadata in the module.
  virtual std::vector<SemanticDefineError> WriteSemanticDefines(llvm::Module *M) override {
    // Grab the semantic defines seen by the parser.
    ParsedSemanticDefineList defines =
      CollectSemanticDefinesParsedByCompiler(m_CI, &m_langExtensionsHelper);

    // Nothing to do if we have no defines.
    SemanticDefineErrorList errors;
    if (!defines.size())
      return errors;

    ParsedSemanticDefineList validated;
    GetValidatedSemanticDefines(defines, validated, errors);
    WriteSemanticDefines(M, validated);
    return errors;
  }

  virtual std::string GetIntrinsicName(UINT opcode) override {
    return m_langExtensionsHelper.GetIntrinsicName(opcode);
  }
  
  virtual bool GetDxilOpcode(UINT opcode, OP::OpCode &dxilOpcode) override {
    UINT dop = static_cast<UINT>(OP::OpCode::NumOpCodes);
    if (m_langExtensionsHelper.GetDxilOpCode(opcode, dop)) {
      if (dop < static_cast<UINT>(OP::OpCode::NumOpCodes)) {
        dxilOpcode = static_cast<OP::OpCode>(dop);
        return true;
      }
    }
    return false;
  }

  virtual HLSLExtensionsCodegenHelper::CustomRootSignature::Status GetCustomRootSignature(CustomRootSignature *out) {
    // Find macro definition in preprocessor.
    Preprocessor &pp = m_CI.getPreprocessor();
    MacroInfo *macro = MacroExpander::FindMacroInfo(pp, m_rootSigDefine);
    if (!macro)
      return CustomRootSignature::NOT_FOUND;

    // Combine tokens into single string
    MacroExpander expander(pp, MacroExpander::STRIP_QUOTES);
    if (!expander.ExpandMacro(macro, &out->RootSignature))
      return CustomRootSignature::NOT_FOUND;

    // Record source location of root signature macro.
    out->EncodedSourceLocation = macro->getDefinitionLoc().getRawEncoding();

    return CustomRootSignature::FOUND;
  }
};

class DxcCompiler : public IDxcCompiler2, public IDxcLangExtensions, public IDxcContainerEvent, public IDxcVersionInfo {
private:
  DXC_MICROCOM_TM_REF_FIELDS()
  DxcLangExtensionsHelper m_langExtensionsHelper;
  CComPtr<IDxcContainerEventsHandler> m_pDxcContainerEventsHandler;

  void CreateDefineStrings(_In_count_(defineCount) const DxcDefine *pDefines,
                           UINT defineCount,
                           std::vector<std::string> &defines) {
    // Not very efficient but also not very important.
    for (UINT32 i = 0; i < defineCount; i++) {
      CW2A utf8Name(pDefines[i].Name, CP_UTF8);
      CW2A utf8Value(pDefines[i].Value, CP_UTF8);
      std::string val(utf8Name.m_psz);
      val += "=";
      val += (pDefines[i].Value) ? utf8Value.m_psz : "1";
      defines.push_back(val);
    }
  }

  void ReadOptsAndValidate(hlsl::options::MainArgs &mainArgs,
                           hlsl::options::DxcOpts &opts,
                           AbstractMemoryStream *pOutputStream,
                           _COM_Outptr_ IDxcOperationResult **ppResult,
                           bool &finished) {
    const llvm::opt::OptTable *table = ::options::getHlslOptTable();
    raw_stream_ostream outStream(pOutputStream);
    if (0 != hlsl::options::ReadDxcOpts(table, hlsl::options::CompilerFlags,
                                        mainArgs, opts, outStream)) {
      CComPtr<IDxcBlob> pErrorBlob;
      IFT(pOutputStream->QueryInterface(&pErrorBlob));
      CComPtr<IDxcBlobEncoding> pErrorBlobWithEncoding;
      outStream.flush();
      IFT(DxcCreateBlobWithEncodingSet(pErrorBlob.p, CP_UTF8,
                                       &pErrorBlobWithEncoding));
      IFT(DxcOperationResult::CreateFromResultErrorStatus(nullptr, pErrorBlobWithEncoding.p, E_INVALIDARG, ppResult));
      finished = true;
      return;
    }
    DXASSERT(!opts.HLSL2015, "else ReadDxcOpts didn't fail for non-isense");
    finished = false;
  }
public:
  DXC_MICROCOM_TM_ADDREF_RELEASE_IMPL()
  DXC_MICROCOM_TM_CTOR(DxcCompiler)
  DXC_LANGEXTENSIONS_HELPER_IMPL(m_langExtensionsHelper)

  __override HRESULT STDMETHODCALLTYPE RegisterDxilContainerEventHandler(IDxcContainerEventsHandler *pHandler, UINT64 *pCookie) {
    DXASSERT(m_pDxcContainerEventsHandler == nullptr, "else events handler is already registered");
    *pCookie = 1; // Only one EventsHandler supported 
    m_pDxcContainerEventsHandler = pHandler;
    return S_OK;
  };
  __override HRESULT STDMETHODCALLTYPE UnRegisterDxilContainerEventHandler(UINT64 cookie) {
    DXASSERT(m_pDxcContainerEventsHandler != nullptr, "else unregister should not have been called");
    m_pDxcContainerEventsHandler.Release();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **ppvObject) {
    return DoBasicQueryInterface<IDxcCompiler,
                                 IDxcCompiler2,
                                 IDxcLangExtensions,
                                 IDxcContainerEvent,
                                 IDxcVersionInfo>
                                 (this, iid, ppvObject);
  }

  // Compile a single entry point to the target shader model
  __override HRESULT STDMETHODCALLTYPE Compile(
    _In_ IDxcBlob *pSource,                       // Source text to compile
    _In_opt_ LPCWSTR pSourceName,                 // Optional file name for pSource. Used in errors and include handlers.
    _In_ LPCWSTR pEntryPoint,                     // entry point name
    _In_ LPCWSTR pTargetProfile,                  // shader profile to compile
    _In_count_(argCount) LPCWSTR *pArguments,     // Array of pointers to arguments
    _In_ UINT32 argCount,                         // Number of arguments
    _In_count_(defineCount) const DxcDefine *pDefines,  // Array of defines
    _In_ UINT32 defineCount,                      // Number of defines
    _In_opt_ IDxcIncludeHandler *pIncludeHandler, // user-provided interface to handle #include directives (optional)
    _COM_Outptr_ IDxcOperationResult **ppResult   // Compiler output status, buffer, and errors
  ) {
    return CompileWithDebug(pSource, pSourceName, pEntryPoint, pTargetProfile,
                            pArguments, argCount, pDefines, defineCount,
                            pIncludeHandler, ppResult, nullptr, nullptr);
  }

  // Compile a single entry point to the target shader model with debug information.
  __override HRESULT STDMETHODCALLTYPE CompileWithDebug(
    _In_ IDxcBlob *pSource,                       // Source text to compile
    _In_opt_ LPCWSTR pSourceName,                 // Optional file name for pSource. Used in errors and include handlers.
    _In_ LPCWSTR pEntryPoint,                     // Entry point name
    _In_ LPCWSTR pTargetProfile,                  // Shader profile to compile
    _In_count_(argCount) LPCWSTR *pArguments,     // Array of pointers to arguments
    _In_ UINT32 argCount,                         // Number of arguments
    _In_count_(defineCount) const DxcDefine *pDefines,  // Array of defines
    _In_ UINT32 defineCount,                      // Number of defines
    _In_opt_ IDxcIncludeHandler *pIncludeHandler, // user-provided interface to handle #include directives (optional)
    _COM_Outptr_ IDxcOperationResult **ppResult,  // Compiler output status, buffer, and errors
    _Outptr_opt_result_z_ LPWSTR *ppDebugBlobName,// Suggested file name for debug blob.
    _COM_Outptr_opt_ IDxcBlob **ppDebugBlob       // Debug blob
  ) {
    if (pSource == nullptr || ppResult == nullptr ||
        (defineCount > 0 && pDefines == nullptr) ||
        (argCount > 0 && pArguments == nullptr) || pEntryPoint == nullptr ||
        pTargetProfile == nullptr)
      return E_INVALIDARG;
    *ppResult = nullptr;
    AssignToOutOpt(nullptr, ppDebugBlobName);
    AssignToOutOpt(nullptr, ppDebugBlob);

    HRESULT hr = S_OK;
    CComPtr<IDxcBlobEncoding> utf8Source;
    CComPtr<AbstractMemoryStream> pOutputStream;
    CHeapPtr<wchar_t> DebugBlobName;
    DxcEtw_DXCompilerCompile_Start();
    DxcThreadMalloc TM(m_pMalloc);
    IFC(hlsl::DxcGetBlobAsUtf8(pSource, &utf8Source));

    try {
      CComPtr<IDxcBlob> pOutputBlob;
      dxcutil::DxcArgsFileSystem *msfPtr =
        dxcutil::CreateDxcArgsFileSystem(utf8Source, pSourceName, pIncludeHandler);
      std::unique_ptr<::llvm::sys::fs::MSFileSystem> msf(msfPtr);

      ::llvm::sys::fs::AutoPerThreadSystem pts(msf.get());
      IFTLLVM(pts.error_code());

      IFT(CreateMemoryStream(m_pMalloc, &pOutputStream));
      IFT(pOutputStream.QueryInterface(&pOutputBlob));

      int argCountInt;
      IFT(UIntToInt(argCount, &argCountInt));
      hlsl::options::MainArgs mainArgs(argCountInt, pArguments, 0);
      hlsl::options::DxcOpts opts;
      bool finished;
      ReadOptsAndValidate(mainArgs, opts, pOutputStream, ppResult, finished);
      if (finished) {
        hr = S_OK;
        goto Cleanup;
      }
      if (opts.DisplayIncludeProcess)
        msfPtr->EnableDisplayIncludeProcess();

      // Prepare UTF8-encoded versions of API values.
      CW2A pUtf8EntryPoint(pEntryPoint, CP_UTF8);
      CW2A pUtf8TargetProfile(pTargetProfile, CP_UTF8);
      CW2A utf8SourceName(pSourceName, CP_UTF8);
      const char *pUtf8SourceName = utf8SourceName.m_psz;
      if (pUtf8SourceName == nullptr) {
        if (opts.InputFile.empty()) {
          pUtf8SourceName = "input.hlsl";
        }
        else {
          pUtf8SourceName = opts.InputFile.data();
        }
      }
      // Set target profile.
      opts.TargetProfile = pUtf8TargetProfile.m_psz;

      IFT(msfPtr->RegisterOutputStream(L"output.bc", pOutputStream));
      IFT(msfPtr->CreateStdStreams(m_pMalloc));

      StringRef Data((LPSTR)utf8Source->GetBufferPointer(),
                     utf8Source->GetBufferSize());
      std::unique_ptr<llvm::MemoryBuffer> pBuffer(
          llvm::MemoryBuffer::getMemBufferCopy(Data, pUtf8SourceName));

      // Not very efficient but also not very important.
      std::vector<std::string> defines;
      CreateDefineStrings(pDefines, defineCount, defines);
      CreateDefineStrings(opts.Defines.data(), opts.Defines.size(), defines);

      // Setup a compiler instance.
      std::string warnings;
      raw_string_ostream w(warnings);
      raw_stream_ostream outStream(pOutputStream.p);
      llvm::LLVMContext llvmContext; // LLVMContext should outlive CompilerInstance
      CompilerInstance compiler;
      std::unique_ptr<TextDiagnosticPrinter> diagPrinter =
          std::make_unique<TextDiagnosticPrinter>(w, &compiler.getDiagnosticOpts());
      SetupCompilerForCompile(compiler, &m_langExtensionsHelper, utf8SourceName, diagPrinter.get(), defines, opts, pArguments, argCount);
      msfPtr->SetupForCompilerInstance(compiler);

      // The clang entry point (cc1_main) would now create a compiler invocation
      // from arguments, but for this path we're exclusively trying to compile
      // to LLVM bitcode and then package that into a DXBC blob.
      //
      // With the compiler invocation built from command line arguments, the
      // next step is to call ExecuteCompilerInvocation, which creates a
      // FrontendAction* of EmitBCAction, which is a CodeGenAction, which is an
      // ASTFrontendAction. That sets up a BackendConsumer as the ASTConsumer.
      compiler.getFrontendOpts().OutputFile = "output.bc";
      compiler.WriteDefaultOutputDirectly = true;
      compiler.setOutStream(&outStream);

      compiler.getLangOpts().HLSLEntryFunction =
      compiler.getCodeGenOpts().HLSLEntryFunction = pUtf8EntryPoint.m_psz;
      compiler.getCodeGenOpts().HLSLProfile = pUtf8TargetProfile.m_psz;

      unsigned rootSigMajor = 0;
      unsigned rootSigMinor = 0;
      if (compiler.getCodeGenOpts().HLSLProfile == "rootsig_1_1") {
        rootSigMajor = 1;
        rootSigMinor = 1;
      } else if (compiler.getCodeGenOpts().HLSLProfile == "rootsig_1_0") {
        rootSigMajor = 1;
        rootSigMinor = 0;
      }
      compiler.getLangOpts().IsHLSLLibrary = opts.IsLibraryProfile();

      // NOTE: this calls the validation component from dxil.dll; the built-in
      // validator can be used as a fallback.
      bool produceFullContainer = !opts.CodeGenHighLevel && !opts.AstDump && !opts.OptDump && rootSigMajor == 0;

      bool needsValidation = produceFullContainer && !opts.DisableValidation &&
                             !opts.IsLibraryProfile();

      if (needsValidation) {
        UINT32 majorVer, minorVer;
        dxcutil::GetValidatorVersion(&majorVer, &minorVer);
        compiler.getCodeGenOpts().HLSLValidatorMajorVer = majorVer;
        compiler.getCodeGenOpts().HLSLValidatorMinorVer = minorVer;
      }

      if (opts.AstDump) {
        clang::ASTDumpAction dumpAction;
        // Consider - ASTDumpFilter, ASTDumpLookups
        compiler.getFrontendOpts().ASTDumpDecls = true;
        FrontendInputFile file(utf8SourceName.m_psz, IK_HLSL);
        dumpAction.BeginSourceFile(compiler, file);
        dumpAction.Execute();
        dumpAction.EndSourceFile();
        outStream.flush();
      }
      else if (opts.OptDump) {
        EmitOptDumpAction action(&llvmContext);
        FrontendInputFile file(utf8SourceName.m_psz, IK_HLSL);
        action.BeginSourceFile(compiler, file);
        action.Execute();
        action.EndSourceFile();
        outStream.flush();
      }
      else if (rootSigMajor) {
        HLSLRootSignatureAction action(
            compiler.getCodeGenOpts().HLSLEntryFunction, rootSigMajor,
            rootSigMinor);
        FrontendInputFile file(utf8SourceName.m_psz, IK_HLSL);
        action.BeginSourceFile(compiler, file);
        action.Execute();
        action.EndSourceFile();
        outStream.flush();
        // Don't do work to put in a container if an error has occurred
        bool compileOK = !compiler.getDiagnostics().hasErrorOccurred();
        if (compileOK) {
          auto rootSigHandle = action.takeRootSigHandle();

          CComPtr<AbstractMemoryStream> pContainerStream;
          IFT(CreateMemoryStream(m_pMalloc, &pContainerStream));
          SerializeDxilContainerForRootSignature(rootSigHandle.get(),
                                                 pContainerStream);

          pOutputBlob.Release();
          IFT(pContainerStream.QueryInterface(&pOutputBlob));
        }
      }
      // SPIRV change starts
#ifdef ENABLE_SPIRV_CODEGEN
      else if (opts.GenSPIRV) {
          clang::EmitSPIRVAction action;
          FrontendInputFile file(utf8SourceName.m_psz, IK_HLSL);
          action.BeginSourceFile(compiler, file);
          action.Execute();
          action.EndSourceFile();
          outStream.flush();
      }
#endif
      // SPIRV change ends
      else {
        EmitBCAction action(&llvmContext);
        FrontendInputFile file(utf8SourceName.m_psz, IK_HLSL);
        bool compileOK;
        if (action.BeginSourceFile(compiler, file)) {
          action.Execute();
          action.EndSourceFile();
          compileOK = !compiler.getDiagnostics().hasErrorOccurred();
        }
        else {
          compileOK = false;
        }
        outStream.flush();

        SerializeDxilFlags SerializeFlags = SerializeDxilFlags::None;
        if (opts.DebugInfo) {
          SerializeFlags = SerializeDxilFlags::IncludeDebugNamePart;
          // Unless we want to strip it right away, include it in the container.
          if (!opts.StripDebug || ppDebugBlob == nullptr) {
            SerializeFlags |= SerializeDxilFlags::IncludeDebugInfoPart;
          }
        }
        if (opts.DebugNameForSource) {
          SerializeFlags |= SerializeDxilFlags::DebugNameDependOnSource;
        }

        // Don't do work to put in a container if an error has occurred
        // Do not create a container when there is only a a high-level representation in the module.
        if (compileOK && !opts.CodeGenHighLevel) {
          HRESULT valHR = S_OK;

          if (needsValidation) {
            valHR = dxcutil::ValidateAndAssembleToContainer(
                action.takeModule(), pOutputBlob, m_pMalloc, SerializeFlags,
                pOutputStream, opts.DebugInfo, compiler.getDiagnostics());
          } else {
            dxcutil::AssembleToContainer(action.takeModule(),
                                                 pOutputBlob, m_pMalloc,
                                                 SerializeFlags, pOutputStream);
          }

          // Callback after valid DXIL is produced
          if (SUCCEEDED(valHR)) {
            CComPtr<IDxcBlob> pTargetBlob;
            if (m_pDxcContainerEventsHandler != nullptr) {
              HRESULT hr = m_pDxcContainerEventsHandler->OnDxilContainerBuilt(pOutputBlob, &pTargetBlob);
              if (SUCCEEDED(hr) && pTargetBlob != nullptr) {
                std::swap(pOutputBlob, pTargetBlob);
              }
            }

            if (ppDebugBlobName && produceFullContainer) {
              const DxilContainerHeader *pContainer = reinterpret_cast<DxilContainerHeader *>(pOutputBlob->GetBufferPointer());
              DXASSERT(IsValidDxilContainer(pContainer, pOutputBlob->GetBufferSize()), "else invalid container generated");
              auto it = std::find_if(begin(pContainer), end(pContainer),
                DxilPartIsType(DFCC_ShaderDebugName));
              if (it != end(pContainer)) {
                const char *pDebugName;
                if (GetDxilShaderDebugName(*it, &pDebugName, nullptr) && pDebugName && *pDebugName) {
                  IFTBOOL(Unicode::UTF8BufferToUTF16ComHeap(pDebugName, &DebugBlobName), DXC_E_CONTAINER_INVALID);
                }
              }
            }
          }
        }
      }

      // Add std err to warnings.
      msfPtr->WriteStdErrToStream(w);

      CreateOperationResultFromOutputs(pOutputBlob, msfPtr, warnings,
                                       compiler.getDiagnostics(), ppResult);

      // On success, return values. After assigning ppResult, nothing should fail.
      HRESULT status;
      DXVERIFY_NOMSG(SUCCEEDED((*ppResult)->GetStatus(&status)));
      if (SUCCEEDED(status)) {
        if (opts.DebugInfo && ppDebugBlob) {
          DXVERIFY_NOMSG(SUCCEEDED(pOutputStream.QueryInterface(ppDebugBlob)));
        }
        if (ppDebugBlobName) {
          *ppDebugBlobName = DebugBlobName.Detach();
        }
      }

      hr = S_OK;
    }
    CATCH_CPP_ASSIGN_HRESULT();
  Cleanup:
    DxcEtw_DXCompilerCompile_Stop(hr);
    return hr;
  }

  // Preprocess source text
  __override HRESULT STDMETHODCALLTYPE Preprocess(
    _In_ IDxcBlob *pSource,                       // Source text to preprocess
    _In_opt_ LPCWSTR pSourceName,                 // Optional file name for pSource. Used in errors and include handlers.
    _In_count_(argCount) LPCWSTR *pArguments,     // Array of pointers to arguments
    _In_ UINT32 argCount,                         // Number of arguments
    _In_count_(defineCount) const DxcDefine *pDefines,  // Array of defines
    _In_ UINT32 defineCount,                      // Number of defines
    _In_opt_ IDxcIncludeHandler *pIncludeHandler, // user-provided interface to handle #include directives (optional)
    _COM_Outptr_ IDxcOperationResult **ppResult   // Preprocessor output status, buffer, and errors
    ) {
    if (pSource == nullptr || ppResult == nullptr ||
        (defineCount > 0 && pDefines == nullptr) ||
        (argCount > 0 && pArguments == nullptr))
      return E_INVALIDARG;
    *ppResult = nullptr;

    HRESULT hr = S_OK;
    DxcEtw_DXCompilerPreprocess_Start();
    DxcThreadMalloc TM(m_pMalloc);
    CComPtr<IDxcBlobEncoding> utf8Source;
    IFC(hlsl::DxcGetBlobAsUtf8(pSource, &utf8Source));

    try {
      CComPtr<AbstractMemoryStream> pOutputStream;
      dxcutil::DxcArgsFileSystem *msfPtr = dxcutil::CreateDxcArgsFileSystem(utf8Source, pSourceName, pIncludeHandler);
      std::unique_ptr<::llvm::sys::fs::MSFileSystem> msf(msfPtr);

      ::llvm::sys::fs::AutoPerThreadSystem pts(msf.get());
      IFTLLVM(pts.error_code());

      IFT(CreateMemoryStream(m_pMalloc, &pOutputStream));

      const llvm::opt::OptTable *table = ::options::getHlslOptTable();
      int argCountInt;
      IFT(UIntToInt(argCount, &argCountInt));
      hlsl::options::MainArgs mainArgs(argCountInt, pArguments, 0);
      hlsl::options::DxcOpts opts;
      bool finished;
      ReadOptsAndValidate(mainArgs, opts, pOutputStream, ppResult, finished);
      if (finished) {
        hr = S_OK;
        goto Cleanup;
      }

      // Prepare UTF8-encoded versions of API values.
      CW2A utf8SourceName(pSourceName, CP_UTF8);
      const char *pUtf8SourceName = utf8SourceName.m_psz;
      if (pUtf8SourceName == nullptr) {
        if (opts.InputFile.empty()) {
          pUtf8SourceName = "input.hlsl";
        }
        else {
          pUtf8SourceName = opts.InputFile.data();
        }
      }

      IFT(msfPtr->RegisterOutputStream(L"output.hlsl", pOutputStream));
      IFT(msfPtr->CreateStdStreams(m_pMalloc));

      StringRef Data((LPSTR)utf8Source->GetBufferPointer(),
        utf8Source->GetBufferSize());
      std::unique_ptr<llvm::MemoryBuffer> pBuffer(
        llvm::MemoryBuffer::getMemBufferCopy(Data, pUtf8SourceName));

      // Not very efficient but also not very important.
      std::vector<std::string> defines;
      CreateDefineStrings(pDefines, defineCount, defines);

      // Setup a compiler instance.
      std::string warnings;
      raw_string_ostream w(warnings);
      raw_stream_ostream outStream(pOutputStream.p);
      CompilerInstance compiler;
      std::unique_ptr<TextDiagnosticPrinter> diagPrinter =
          std::make_unique<TextDiagnosticPrinter>(w, &compiler.getDiagnosticOpts());
      SetupCompilerForCompile(compiler, &m_langExtensionsHelper, utf8SourceName, diagPrinter.get(), defines, opts, pArguments, argCount);
      msfPtr->SetupForCompilerInstance(compiler);

      // The clang entry point (cc1_main) would now create a compiler invocation
      // from arguments, but for this path we're exclusively trying to preproces
      // to text.
      compiler.getFrontendOpts().OutputFile = "output.hlsl";
      compiler.WriteDefaultOutputDirectly = true;
      compiler.setOutStream(&outStream);

      // These settings are back-compatible with fxc.
      clang::PreprocessorOutputOptions &PPOutOpts =
          compiler.getPreprocessorOutputOpts();
      PPOutOpts.ShowCPP = 1;            // Print normal preprocessed output.
      PPOutOpts.ShowComments = 0;       // Show comments.
      PPOutOpts.ShowLineMarkers = 1;    // Show \#line markers.
      PPOutOpts.UseLineDirectives = 1;  // Use \#line instead of GCC-style \# N.
      PPOutOpts.ShowMacroComments = 0;  // Show comments, even in macros.
      PPOutOpts.ShowMacros = 0;         // Print macro definitions.
      PPOutOpts.RewriteIncludes = 0;    // Preprocess include directives only.

      FrontendInputFile file(utf8SourceName.m_psz, IK_HLSL);
      clang::PrintPreprocessedAction action;
      if (action.BeginSourceFile(compiler, file)) {
        action.Execute();
        action.EndSourceFile();
      }
      outStream.flush();

      // Add std err to warnings.
      msfPtr->WriteStdErrToStream(w);

      CreateOperationResultFromOutputs(pOutputStream, msfPtr, warnings,
        compiler.getDiagnostics(), ppResult);
      hr = S_OK;
    }
    CATCH_CPP_ASSIGN_HRESULT();
  Cleanup:
    DxcEtw_DXCompilerPreprocess_Stop(hr);
    return hr;
  }

  // Disassemble a shader.
  __override HRESULT STDMETHODCALLTYPE Disassemble(
    _In_ IDxcBlob *pProgram,                      // Program to disassemble.
    _COM_Outptr_ IDxcBlobEncoding** ppDisassembly // Disassembly text.
    ) {
    if (pProgram == nullptr || ppDisassembly == nullptr)
      return E_INVALIDARG;

    *ppDisassembly = nullptr;

    HRESULT hr = S_OK;
    DxcEtw_DXCompilerDisassemble_Start();
    DxcThreadMalloc TM(m_pMalloc); 
    try {
      ::llvm::sys::fs::MSFileSystem *msfPtr;
      IFT(CreateMSFileSystemForDisk(&msfPtr));
      std::unique_ptr<::llvm::sys::fs::MSFileSystem> msf(msfPtr);

      ::llvm::sys::fs::AutoPerThreadSystem pts(msf.get());
      IFTLLVM(pts.error_code());

      std::string StreamStr;
      raw_string_ostream Stream(StreamStr);

      IFC(dxcutil::Disassemble(pProgram, Stream));

      IFT(DxcCreateBlobWithEncodingOnHeapCopy(
          StreamStr.c_str(), StreamStr.size(), CP_UTF8, ppDisassembly));

      return S_OK;
    }
    CATCH_CPP_ASSIGN_HRESULT();
  Cleanup:
    DxcEtw_DXCompilerDisassemble_Stop(hr);
    return hr;
  }

  void SetupCompilerForCompile(CompilerInstance &compiler,
                               _In_ DxcLangExtensionsHelper *helper,
                               _In_ LPCSTR pMainFile, _In_ TextDiagnosticPrinter *diagPrinter,
                               _In_ std::vector<std::string>& defines,
                               _In_ hlsl::options::DxcOpts &Opts,
                               _In_count_(argCount) LPCWSTR *pArguments,
                               _In_ UINT32 argCount) {
    // Setup a compiler instance.
    std::shared_ptr<TargetOptions> targetOptions(new TargetOptions);
    targetOptions->Triple = "dxil-ms-dx";
    compiler.HlslLangExtensions = helper;
    compiler.createDiagnostics(diagPrinter, false);
    compiler.createFileManager();
    compiler.createSourceManager(compiler.getFileManager());
    compiler.setTarget(
        TargetInfo::CreateTargetInfo(compiler.getDiagnostics(), targetOptions));

    compiler.getFrontendOpts().Inputs.push_back(FrontendInputFile(pMainFile, IK_HLSL));
    // Setup debug information.
    if (Opts.DebugInfo) {
      CodeGenOptions &CGOpts = compiler.getCodeGenOpts();
      CGOpts.setDebugInfo(CodeGenOptions::FullDebugInfo);
      CGOpts.DebugColumnInfo = 1;
      CGOpts.DwarfVersion = 4; // Latest version.
      // TODO: consider
      // DebugPass, DebugCompilationDir, DwarfDebugFlags, SplitDwarfFile
    }

    clang::PreprocessorOptions &PPOpts(compiler.getPreprocessorOpts());
    for (size_t i = 0; i < defines.size(); ++i) {
      PPOpts.addMacroDef(defines[i]);
    }

    // Pick additional arguments.
    clang::HeaderSearchOptions &HSOpts = compiler.getHeaderSearchOpts();
    HSOpts.UseBuiltinIncludes = 0;
    // Consider: should we force-include '.' if the source file is relative?
    for (const llvm::opt::Arg *A : Opts.Args.filtered(options::OPT_I)) {
      const bool IsFrameworkFalse = false;
      const bool IgnoreSysRoot = true;
      if (dxcutil::IsAbsoluteOrCurDirRelative(A->getValue())) {
        HSOpts.AddPath(A->getValue(), frontend::Angled, IsFrameworkFalse, IgnoreSysRoot);
      }
      else {
        std::string s("./");
        s += A->getValue();
        HSOpts.AddPath(s, frontend::Angled, IsFrameworkFalse, IgnoreSysRoot);
      }
    }

    // Apply root signature option.
    unsigned rootSigMinor;
    if (Opts.ForceRootSigVer.empty() || Opts.ForceRootSigVer == "rootsig_1_1") {
      rootSigMinor = 1;
    }
    else {
      DXASSERT(Opts.ForceRootSigVer == "rootsig_1_0",
               "else opts should have been rejected");
      rootSigMinor = 0;
    }
    compiler.getLangOpts().RootSigMajor = 1;
    compiler.getLangOpts().RootSigMinor = rootSigMinor;
    compiler.getLangOpts().HLSL2015 = Opts.HLSL2015;
    compiler.getLangOpts().HLSL2016 = Opts.HLSL2016;
    compiler.getLangOpts().HLSL2017 = Opts.HLSL2017;

    if (Opts.WarningAsError)
      compiler.getDiagnostics().setWarningsAsErrors(true);

    if (Opts.IEEEStrict)
      compiler.getCodeGenOpts().UnsafeFPMath = true;

    if (Opts.FPDenormalMode.empty() || Opts.FPDenormalMode.equals_lower(StringRef("undefined"))) {
      compiler.getCodeGenOpts().HLSLFlushFPDenorm = DXIL::FPDenormMode::Undefined;
    }
    else if (Opts.FPDenormalMode.equals_lower(StringRef("ftz"))) {
      compiler.getCodeGenOpts().HLSLFlushFPDenorm = DXIL::FPDenormMode::FTZ;
    }
    else {
      DXASSERT(Opts.FPDenormalMode.equals_lower(StringRef("ieee")), "else opts should have been rejected");
      compiler.getCodeGenOpts().HLSLFlushFPDenorm = DXIL::FPDenormMode::IEEE;
    }

    if (Opts.DisableOptimizations)
      compiler.getCodeGenOpts().DisableLLVMOpts = true;

    compiler.getCodeGenOpts().OptimizationLevel = Opts.OptLevel;
    if (Opts.OptLevel >= 3)
      compiler.getCodeGenOpts().UnrollLoops = true;

    compiler.getCodeGenOpts().HLSLHighLevel = Opts.CodeGenHighLevel;
    compiler.getCodeGenOpts().HLSLAllResourcesBound = Opts.AllResourcesBound;
    compiler.getCodeGenOpts().HLSLDefaultRowMajor = Opts.DefaultRowMajor;
    compiler.getCodeGenOpts().HLSLPreferControlFlow = Opts.PreferFlowControl;
    compiler.getCodeGenOpts().HLSLAvoidControlFlow = Opts.AvoidFlowControl;
    compiler.getCodeGenOpts().HLSLNotUseLegacyCBufLoad = Opts.NotUseLegacyCBufLoad;
    compiler.getCodeGenOpts().HLSLDefines = defines;
    compiler.getCodeGenOpts().MainFileName = pMainFile;

    // Translate signature packing options
    if (Opts.PackPrefixStable)
      compiler.getCodeGenOpts().HLSLSignaturePackingStrategy = (unsigned)DXIL::PackingStrategy::PrefixStable;
    else if (Opts.PackOptimized)
      compiler.getCodeGenOpts().HLSLSignaturePackingStrategy = (unsigned)DXIL::PackingStrategy::Optimized;
    else
      compiler.getCodeGenOpts().HLSLSignaturePackingStrategy = (unsigned)DXIL::PackingStrategy::Default;

    // Constructing vector of wide strings to pass in to codegen. Just passing
    // in pArguments will expose ownership of memory to both CodeGenOptions and
    // this caller, which can lead to unexpected behavior.
    for (UINT32 i = 0; i != argCount; ++i) {
      compiler.getCodeGenOpts().HLSLArguments.emplace_back(
          Unicode::UTF16ToUTF8StringOrThrow(pArguments[i]));
    }
    // Overrding default set of loop unroll.
    if (Opts.PreferFlowControl)
      compiler.getCodeGenOpts().UnrollLoops = false;
    if (Opts.AvoidFlowControl)
      compiler.getCodeGenOpts().UnrollLoops = true;

    // always inline for hlsl
    compiler.getCodeGenOpts().setInlining(
        clang::CodeGenOptions::OnlyAlwaysInlining);

    compiler.getCodeGenOpts().HLSLExtensionsCodegen = std::make_shared<HLSLExtensionsCodegenHelperImpl>(compiler, m_langExtensionsHelper, Opts.RootSignatureDefine);
  }

  // IDxcVersionInfo
  __override HRESULT STDMETHODCALLTYPE GetVersion(_Out_ UINT32 *pMajor, _Out_ UINT32 *pMinor) {
    if (pMajor == nullptr || pMinor == nullptr)
      return E_INVALIDARG;
    *pMajor = DXIL::kDxilMajor;
    *pMinor = DXIL::kDxilMinor;
    return S_OK;
  }
  __override HRESULT STDMETHODCALLTYPE GetFlags(_Out_ UINT32 *pFlags) {
    if (pFlags == nullptr)
      return E_INVALIDARG;
    *pFlags = DxcVersionInfoFlags_None;
#ifdef _DEBUG
    *pFlags |= DxcVersionInfoFlags_Debug;
#endif
    return S_OK;
  }

};

HRESULT CreateDxcCompiler(_In_ REFIID riid, _Out_ LPVOID* ppv) {
  *ppv = nullptr;
  CComPtr<DxcCompiler> result;
  try {
    result = DxcCompiler::Alloc(DxcGetThreadMallocNoRef());
    IFROOM(result.p);
  }
  CATCH_CPP_RETURN_HRESULT();
  return result.p->QueryInterface(riid, ppv);
}
