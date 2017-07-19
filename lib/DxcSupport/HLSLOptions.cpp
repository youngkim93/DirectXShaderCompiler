//===--- HLSLOptions.cpp - Driver Options Table ---------------------------===//
///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// HLSLOptions.cpp                                                           //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "llvm/ADT/STLExtras.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/raw_ostream.h"
#include "dxc/Support/Global.h"
#include "dxc/Support/WinIncludes.h"
#include "dxc/Support/HLSLOptions.h"
#include "dxc/Support/Unicode.h"
#include "dxc/Support/dxcapi.use.h"
#include "dxc/HLSL/DxilShaderModel.h"

using namespace llvm::opt;
using namespace dxc;
using namespace hlsl;
using namespace hlsl::options;

#define PREFIX(NAME, VALUE) static const char *const NAME[] = VALUE;
#include "dxc/Support/HLSLOptions.inc"
#undef PREFIX

static const OptTable::Info HlslInfoTable[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM, \
               HELPTEXT, METAVAR)   \
  { PREFIX, NAME, HELPTEXT, METAVAR, OPT_##ID, Option::KIND##Class, PARAM, \
    FLAGS, OPT_##GROUP, OPT_##ALIAS, ALIASARGS },
#include "dxc/Support/HLSLOptions.inc"
#undef OPTION
};

namespace {

  class HlslOptTable : public OptTable {
  public:
    HlslOptTable()
      : OptTable(HlslInfoTable, llvm::array_lengthof(HlslInfoTable)) {}
  };

}

static HlslOptTable *g_HlslOptTable;

std::error_code hlsl::options::initHlslOptTable() {
  DXASSERT(g_HlslOptTable == nullptr, "else double-init");
  g_HlslOptTable = new (std::nothrow) HlslOptTable();
  if (g_HlslOptTable == nullptr)
    return std::error_code(E_OUTOFMEMORY, std::system_category());
  return std::error_code();
}

void hlsl::options::cleanupHlslOptTable() {
  delete g_HlslOptTable;
  g_HlslOptTable = nullptr;
}

const OptTable * hlsl::options::getHlslOptTable() {
  return g_HlslOptTable;
}

void DxcDefines::push_back(llvm::StringRef value) {
  // Skip empty defines.
  if (value.size() > 0) {
    DefineStrings.push_back(value);
  }
}

UINT32 DxcDefines::ComputeNumberOfWCharsNeededForDefines() {
  UINT32 wcharSize = 0;
  for (llvm::StringRef &S : DefineStrings) {
    DXASSERT(S.size() > 0,
             "else DxcDefines::push_back should not have added this");
    const int utf16Length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, S.data(), S.size(), nullptr, 0);
    IFTARG(utf16Length != 0);
    wcharSize += utf16Length + 1; // adding null terminated character
  }
  return wcharSize;
}

void DxcDefines::BuildDefines() {
  // Calculate and prepare the size of the backing buffer.
  DXASSERT(DefineValues == nullptr, "else DxcDefines is already built");
  UINT32 wcharSize = ComputeNumberOfWCharsNeededForDefines();

  DefineValues = new wchar_t[wcharSize];
  DefineVector.resize(DefineStrings.size());

  // Build up the define structures while filling in the backing buffer.
  UINT32 remaining = wcharSize;
  LPWSTR pWriteCursor = DefineValues;
  for (size_t i = 0; i < DefineStrings.size(); ++i) {
    llvm::StringRef &S = DefineStrings[i];
    DxcDefine &D = DefineVector[i];
    const int utf16Length =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, S.data(), S.size(),
                              pWriteCursor, remaining);
    DXASSERT(utf16Length > 0,
             "else it should have failed during size calculation");
    LPWSTR pDefineEnd = pWriteCursor + utf16Length;
    D.Name = pWriteCursor;

    LPWSTR pEquals = std::find(pWriteCursor, pDefineEnd, L'=');
    if (pEquals == pDefineEnd) {
      D.Value = nullptr;
    } else {
      *pEquals = L'\0';
      D.Value = pEquals + 1;
    }

    // Advance past converted characters and include the null terminator.
    pWriteCursor += utf16Length;
    *pWriteCursor = L'\0';
    ++pWriteCursor;

    DXASSERT(pWriteCursor <= DefineValues + wcharSize,
             "else this function is calculating this incorrectly");
    remaining -= (utf16Length + 1);
  }
}

bool DxcOpts::IsRootSignatureProfile() {
  return TargetProfile == "rootsig_1_0" ||
      TargetProfile == "rootsig_1_1";
}

bool DxcOpts::IsLibraryProfile() {
  return TargetProfile.startswith("lib_");
}

MainArgs::MainArgs(int argc, const wchar_t **argv, int skipArgCount) {
  if (argc > skipArgCount) {
    Utf8StringVector.reserve(argc - skipArgCount);
    Utf8CharPtrVector.reserve(argc - skipArgCount);
    for (int i = skipArgCount; i < argc; ++i) {
      Utf8StringVector.emplace_back(Unicode::UTF16ToUTF8StringOrThrow(argv[i]));
      Utf8CharPtrVector.push_back(Utf8StringVector.back().data());
    }
  }
}

MainArgs::MainArgs(llvm::ArrayRef<llvm::StringRef> args) {
  Utf8StringVector.reserve(args.size());
  Utf8CharPtrVector.reserve(args.size());
  for (llvm::StringRef str : args) {
    Utf8StringVector.emplace_back(str.str());
    Utf8CharPtrVector.push_back(Utf8StringVector.back().data());
  }
}

MainArgs& MainArgs::operator=(const MainArgs &other) {
  Utf8StringVector.clear();
  Utf8CharPtrVector.clear();
  for (const std::string &str : other.Utf8StringVector) {
    Utf8StringVector.emplace_back(str);
    Utf8CharPtrVector.push_back(Utf8StringVector.back().data());
  }
  return *this;
}

StringRefUtf16::StringRefUtf16(llvm::StringRef value) {
  if (!value.empty())
    m_value = Unicode::UTF8ToUTF16StringOrThrow(value.data());
}

namespace hlsl {
namespace options {

/// Reads all options from the given argument strings, populates opts, and
/// validates reporting errors and warnings.
int ReadDxcOpts(const OptTable *optionTable, unsigned flagsToInclude,
  const MainArgs &argStrings, DxcOpts &opts,
  llvm::raw_ostream &errors) {
  DXASSERT_NOMSG(optionTable != nullptr);

  unsigned missingArgIndex = 0, missingArgCount = 0;
  InputArgList Args = optionTable->ParseArgs(
    argStrings.getArrayRef(), missingArgIndex, missingArgCount, flagsToInclude);
  opts.ShowHelp = Args.hasFlag(OPT_help, OPT_INVALID, false);
  if (opts.ShowHelp) {
    return 0;
  }

  if (missingArgCount) {
    errors << "Argument to '" << Args.getArgString(missingArgIndex)
      << "' is missing.";
    return 1;
  }

  if (!Args.hasArg(hlsl::options::OPT_Qunused_arguments)) {
    for (const Arg *A : Args.filtered(OPT_UNKNOWN)) {
      errors << "Unknown argument: '" << A->getAsString(Args).c_str() << "'";
      return 1;
    }
  }

  // Add macros from the command line.

  for (const Arg *A : Args.filtered(OPT_D)) {
    opts.Defines.push_back(A->getValue());
    // If supporting OPT_U and included in filter, handle undefs.
  }
  opts.Defines.BuildDefines(); // Must be called after all defines are pushed back

  opts.ExternalLib = Args.getLastArgValue(OPT_external_lib);
  opts.ExternalFn = Args.getLastArgValue(OPT_external_fn);

  // Verify consistency for external library support.
  if (opts.ExternalLib.empty()) {
    if (!opts.ExternalFn.empty()) {
      errors << "External function cannot be specified without an external "
        "library name.";
      return 1;
    }
  }
  else {
    if (opts.ExternalFn.empty()) {
      errors << "External library name requires specifying an external "
        "function name.";
      return 1;
    }
  }
  DXASSERT(opts.ExternalLib.empty() == opts.ExternalFn.empty(),
           "else flow above is incorrect");

  // when no-warnings option is present, do not output warnings.
  opts.OutputWarnings = Args.hasFlag(OPT_INVALID, OPT_no_warnings, true);
  opts.EntryPoint = Args.getLastArgValue(OPT_entrypoint);
  // Entry point is required in arguments only for drivers; APIs take this through an argument.
  // The value should default to 'main', but we let the caller apply this policy.

  opts.TargetProfile = Args.getLastArgValue(OPT_target_profile);

  if (opts.IsLibraryProfile()) {
    if (Args.getLastArg(OPT_entrypoint)) {
      errors << "cannot specify entry point for a library";
      return 1;
    } else {
      // Set entry point to impossible name.
      opts.EntryPoint = "lib.no::entry";
    }
  }

  llvm::StringRef ver = Args.getLastArgValue(OPT_hlsl_version);
  opts.HLSL2015 = opts.HLSL2016 = opts.HLSL2017 = false;
  if (ver.empty() || ver == "2016") { opts.HLSL2016 = true; }   // Default to 2016
  else if           (ver == "2015") { opts.HLSL2015 = true; }
  else if           (ver == "2017") { opts.HLSL2017 = true; }
  else {
    errors << "Unknown HLSL version";
    return 1;
  }
  if (opts.HLSL2015 && !(flagsToInclude & HlslFlags::ISenseOption)) {
    errors << "HLSL Version 2015 is only supported for language services";
    return 1;
  }

  // AssemblyCodeHex not supported (Fx)
  // OutputLibrary not supported (Fl)
  opts.AssemblyCode = Args.getLastArgValue(OPT_Fc);
  opts.DebugFile = Args.getLastArgValue(OPT_Fd);
  opts.ExtractPrivateFile = Args.getLastArgValue(OPT_getprivate);
  opts.OutputObject = Args.getLastArgValue(OPT_Fo);
  opts.OutputHeader = Args.getLastArgValue(OPT_Fh);
  opts.OutputWarningsFile = Args.getLastArgValue(OPT_Fe);
  opts.UseColor = Args.hasFlag(OPT_Cc, OPT_INVALID);
  opts.UseInstructionNumbers = Args.hasFlag(OPT_Ni, OPT_INVALID);
  opts.UseInstructionByteOffsets = Args.hasFlag(OPT_No, OPT_INVALID);
  opts.UseHexLiterals = Args.hasFlag(OPT_Lx, OPT_INVALID);
  opts.Preprocess = Args.getLastArgValue(OPT_P);
  opts.AstDump = Args.hasFlag(OPT_ast_dump, OPT_INVALID, false);
  opts.GenSPIRV = Args.hasFlag(OPT_spirv, OPT_INVALID, false); // SPIRV change
  opts.CodeGenHighLevel = Args.hasFlag(OPT_fcgl, OPT_INVALID, false);
  opts.DebugInfo = Args.hasFlag(OPT__SLASH_Zi, OPT_INVALID, false);
  opts.DebugNameForBinary = Args.hasFlag(OPT_Zsb, OPT_INVALID, false);
  opts.DebugNameForSource = Args.hasFlag(OPT_Zsb, OPT_INVALID, false);
  opts.VariableName = Args.getLastArgValue(OPT_Vn);
  opts.InputFile = Args.getLastArgValue(OPT_INPUT);
  opts.ForceRootSigVer = Args.getLastArgValue(OPT_force_rootsig_ver);
  opts.PrivateSource = Args.getLastArgValue(OPT_setprivate);
  opts.RootSignatureSource = Args.getLastArgValue(OPT_setrootsignature);
  opts.VerifyRootSignatureSource = Args.getLastArgValue(OPT_verifyrootsignature);
  opts.RootSignatureDefine = Args.getLastArgValue(OPT_rootsig_define);

  if (!opts.ForceRootSigVer.empty() && opts.ForceRootSigVer != "rootsig_1_0" &&
      opts.ForceRootSigVer != "rootsig_1_1") {
    errors << "Unsupported value '" << opts.ForceRootSigVer
           << "' for root signature profile.";
    return 1;
  }

  opts.IEEEStrict = Args.hasFlag(OPT_Gis, OPT_INVALID, false);

  opts.FPDenormalMode = Args.getLastArgValue(OPT_fdenormal_fp_math);
  // Check if a given denormalized value is valid
  if (!opts.FPDenormalMode.empty()) {
    if (!(opts.FPDenormalMode.equals_lower("ieee") ||
          opts.FPDenormalMode.equals_lower("ftz") ||
          opts.FPDenormalMode.equals_lower("undefined"))) {
      errors << "Unsupported value '" << opts.FPDenormalMode
          << "' for fdenormal_fp_math option.";
      return 1;
    }
    if (opts.TargetProfile.empty() || !opts.TargetProfile.endswith_lower("6_2")) {
      errors << "fdenormal_fp_math option is only allowed for shader model 6.2 and above.";
      return 1;
    }
  }

  if (Arg *A = Args.getLastArg(OPT_O0, OPT_O1, OPT_O2, OPT_O3)) {
    if (A->getOption().matches(OPT_O0))
      opts.OptLevel = 0;
    if (A->getOption().matches(OPT_O1))
      opts.OptLevel = 1;
    if (A->getOption().matches(OPT_O2))
      opts.OptLevel = 2;
    if (A->getOption().matches(OPT_O3))
      opts.OptLevel = 3;
  }
  else
    opts.OptLevel = 3;
  opts.OptDump = Args.hasFlag(OPT_Odump, OPT_INVALID, false);

  opts.DisableOptimizations = Args.hasFlag(OPT_Od, OPT_INVALID, false);
  if (opts.DisableOptimizations)
    opts.OptLevel = 0;

  opts.DisableValidation = Args.hasFlag(OPT_VD, OPT_INVALID, false);

  opts.AllResourcesBound = Args.hasFlag(OPT_all_resources_bound, OPT_INVALID, false);
  opts.ColorCodeAssembly = Args.hasFlag(OPT_Cc, OPT_INVALID, false);
  opts.DefaultRowMajor = Args.hasFlag(OPT_Zpr, OPT_INVALID, false);
  opts.DefaultColMajor = Args.hasFlag(OPT_Zpc, OPT_INVALID, false);
  opts.DumpBin = Args.hasFlag(OPT_dumpbin, OPT_INVALID, false);
  opts.NotUseLegacyCBufLoad = Args.hasFlag(OPT_not_use_legacy_cbuf_load, OPT_INVALID, false);
  opts.PackPrefixStable = Args.hasFlag(OPT_pack_prefix_stable, OPT_INVALID, false);
  opts.PackOptimized = Args.hasFlag(OPT_pack_optimized, OPT_INVALID, false);
  opts.DisplayIncludeProcess = Args.hasFlag(OPT_H, OPT_INVALID, false);
  opts.WarningAsError = Args.hasFlag(OPT__SLASH_WX, OPT_INVALID, false);
  opts.AvoidFlowControl = Args.hasFlag(OPT_Gfa, OPT_INVALID, false);
  opts.PreferFlowControl = Args.hasFlag(OPT_Gfp, OPT_INVALID, false);
  opts.RecompileFromBinary = Args.hasFlag(OPT_recompile, OPT_INVALID, false);
  opts.StripDebug = Args.hasFlag(OPT_Qstrip_debug, OPT_INVALID, false);
  opts.StripRootSignature = Args.hasFlag(OPT_Qstrip_rootsignature, OPT_INVALID, false);
  opts.StripPrivate = Args.hasFlag(OPT_Qstrip_priv, OPT_INVALID, false);
  opts.StripReflection = Args.hasFlag(OPT_Qstrip_reflect, OPT_INVALID, false);
  opts.ExtractRootSignature = Args.hasFlag(OPT_extractrootsignature, OPT_INVALID, false);
  opts.DisassembleColorCoded = Args.hasFlag(OPT_Cc, OPT_INVALID, false);
  opts.DisassembleInstNumbers = Args.hasFlag(OPT_Ni, OPT_INVALID, false);
  opts.DisassembleByteOffset = Args.hasFlag(OPT_No, OPT_INVALID, false);
  opts.DisaseembleHex = Args.hasFlag(OPT_Lx, OPT_INVALID, false);

  if (opts.DefaultColMajor && opts.DefaultRowMajor) {
    errors << "Cannot specify /Zpr and /Zpc together, use /? to get usage information";
    return 1;
  }
  if (opts.AvoidFlowControl && opts.PreferFlowControl) {
    errors << "Cannot specify /Gfa and /Gfp together, use /? to get usage information";
    return 1;
  }
  if (opts.PackPrefixStable && opts.PackOptimized) {
    errors << "Cannot specify /pack_prefix_stable and /pack_optimized together, use /? to get usage information";
    return 1;
  }
  // TODO: more fxc option check.
  // ERR_RES_MAY_ALIAS_ONLY_IN_CS_5
  // ERR_NOT_ABLE_TO_FLATTEN on if that contain side effects
  // TODO: other front-end error.
  // ERR_RESOURCE_NOT_IN_TEMPLATE
  // ERR_COMPLEX_TEMPLATE_RESOURCE
  // ERR_RESOURCE_BIND_CONFLICT
  // ERR_TEMPLATE_VAR_CONFLICT
  // ERR_ATTRIBUTE_PARAM_SIDE_EFFECT

  if ((flagsToInclude & hlsl::options::DriverOption) && opts.InputFile.empty()) {
    // Input file is required in arguments only for drivers; APIs take this through an argument.
    errors << "Required input file argument is missing. use -help to get more information.";
    return 1;
  }
  if (opts.OutputHeader.empty() && !opts.VariableName.empty()) {
    errors << "Cannot specify a header variable name when not writing a header.";
    return 1;
  }

  if (!opts.Preprocess.empty() &&
      (!opts.OutputHeader.empty() || !opts.OutputObject.empty() ||
       !opts.OutputWarnings || !opts.OutputWarningsFile.empty())) {
    errors << "Preprocess cannot be specified with other options.";
    return 1;
  }

  if (opts.DumpBin) {
    if (opts.DisplayIncludeProcess || opts.AstDump) {
      errors << "Cannot perform actions related to sources from a binary file.";
      return 1;
    }
    if (opts.AllResourcesBound || opts.AvoidFlowControl ||
        opts.CodeGenHighLevel || opts.DebugInfo || opts.DefaultColMajor ||
        opts.DefaultRowMajor || opts.Defines.size() != 0 ||
        opts.DisableOptimizations || 
        !opts.EntryPoint.empty() || !opts.ForceRootSigVer.empty() ||
        opts.PreferFlowControl || !opts.TargetProfile.empty()) {
      errors << "Cannot specify compilation options when reading a binary file.";
      return 1;
    }
  }

  if ((flagsToInclude & hlsl::options::DriverOption) &&
      opts.TargetProfile.empty() && !opts.DumpBin && opts.Preprocess.empty() && !opts.RecompileFromBinary) {
    // Target profile is required in arguments only for drivers when compiling;
    // APIs take this through an argument.
    errors << "Target profile argument is missing";
    return 1;
  }

  if (!opts.DebugNameForBinary && !opts.DebugNameForSource) {
    opts.DebugNameForSource = true;
  }
  else if (opts.DebugNameForBinary && opts.DebugNameForSource) {
    errors << "Cannot specify both /Zss and /Zsb";
    return 1;
  }

  opts.Args = std::move(Args);
  return 0;
}

/// Sets up the specified DxcDllSupport instance as per the given options.
int SetupDxcDllSupport(const DxcOpts &opts, dxc::DxcDllSupport &dxcSupport,
                       llvm::raw_ostream &errors) {
  if (!opts.ExternalLib.empty()) {
    DXASSERT(!opts.ExternalFn.empty(), "else ReadDxcOpts should have failed");
    StringRefUtf16 externalLib(opts.ExternalLib);
    HRESULT hrLoad =
        dxcSupport.InitializeForDll(externalLib, opts.ExternalFn.data());
    if (DXC_FAILED(hrLoad)) {
      errors << "Unable to load support for external DLL " << opts.ExternalLib
             << " with function " << opts.ExternalFn << " - error 0x";
      errors.write_hex(hrLoad);
      return 1;
    }
  }
  return 0;
}

void CopyArgsToWStrings(const InputArgList &inArgs, unsigned flagsToInclude,
                        std::vector<std::wstring> &outArgs) {
  ArgStringList stringList;
  for (const Arg *A : inArgs) {
    if (A->getOption().hasFlag(flagsToInclude)) {
      A->renderAsInput(inArgs, stringList);
    }
  }
  for (const char *argText : stringList) {
    outArgs.emplace_back(Unicode::UTF8ToUTF16StringOrThrow(argText));
  }
}

} } // hlsl::options
