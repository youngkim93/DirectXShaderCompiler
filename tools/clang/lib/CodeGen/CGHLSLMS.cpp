//===----- CGHLSLMS.cpp - Interface to HLSL Runtime ----------------===//
///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// CGHLSLMS.cpp                                                              //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
//  This provides a class for HLSL code generation.                          //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "CGHLSLRuntime.h"
#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "CGRecordLayout.h"
#include "dxc/HlslIntrinsicOp.h"
#include "dxc/HLSL/HLMatrixLowerHelper.h"
#include "dxc/HLSL/HLModule.h"
#include "dxc/HLSL/HLOperations.h"
#include "dxc/HLSL/DxilOperations.h"
#include "dxc/HLSL/DxilTypeSystem.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/HlslTypes.h"
#include "clang/Frontend/CodeGenOptions.h"
#include "clang/Lex/HLSLMacroExpander.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "dxc/HLSL/DxilRootSignature.h"
#include "dxc/HLSL/DxilCBuffer.h"
#include "clang/Parse/ParseHLSL.h"      // root sig would be in Parser if part of lang
#include "dxc/Support/WinIncludes.h"    // stream support
#include "dxc/dxcapi.h"                 // stream support
#include "dxc/HLSL/HLSLExtensionsCodegenHelper.h"

using namespace clang;
using namespace CodeGen;
using namespace hlsl;
using namespace llvm;
using std::unique_ptr;

static const bool KeepUndefinedTrue = true; // Keep interpolation mode undefined if not set explicitly.

namespace {

/// Use this class to represent HLSL cbuffer in high-level DXIL.
class HLCBuffer : public DxilCBuffer {
public:
  HLCBuffer() = default;
  virtual ~HLCBuffer() = default;

  void AddConst(std::unique_ptr<DxilResourceBase> &pItem);

  std::vector<std::unique_ptr<DxilResourceBase>> &GetConstants();

private:
  std::vector<std::unique_ptr<DxilResourceBase>> constants; // constants inside const buffer
};

//------------------------------------------------------------------------------
//
// HLCBuffer methods.
//
void HLCBuffer::AddConst(std::unique_ptr<DxilResourceBase> &pItem) {
  pItem->SetID(constants.size());
  constants.push_back(std::move(pItem));
}

std::vector<std::unique_ptr<DxilResourceBase>> &HLCBuffer::GetConstants() {
  return constants;
}

class CGMSHLSLRuntime : public CGHLSLRuntime {

private:
  /// Convenience reference to LLVM Context
  llvm::LLVMContext &Context;
  /// Convenience reference to the current module
  llvm::Module &TheModule;

  HLModule *m_pHLModule;
  llvm::Type *CBufferType;
  uint32_t globalCBIndex;
  // TODO: make sure how minprec works
  llvm::DataLayout legacyLayout;
  // decl map to constant id for program
  llvm::DenseMap<HLSLBufferDecl *, uint32_t> constantBufMap;
  // Map for resource type to resource metadata value.
  std::unordered_map<llvm::Type *, MDNode*> resMetadataMap;

  bool  m_bDebugInfo;

  HLCBuffer &GetGlobalCBuffer() {
    return *static_cast<HLCBuffer*>(&(m_pHLModule->GetCBuffer(globalCBIndex)));
  }
  void AddConstant(VarDecl *constDecl, HLCBuffer &CB);
  uint32_t AddSampler(VarDecl *samplerDecl);
  uint32_t AddUAVSRV(VarDecl *decl, hlsl::DxilResourceBase::Class resClass);
  bool SetUAVSRV(SourceLocation loc, hlsl::DxilResourceBase::Class resClass,
                 DxilResource *hlslRes, const RecordDecl *RD);
  uint32_t AddCBuffer(HLSLBufferDecl *D);
  hlsl::DxilResourceBase::Class TypeToClass(clang::QualType Ty);

  // Save the entryFunc so don't need to find it with original name.
  llvm::Function *EntryFunc;
  
  // Map to save patch constant functions
  StringMap<Function *> patchConstantFunctionMap;
  bool IsPatchConstantFunction(const Function *F);

  // List for functions with clip plane.
  std::vector<Function *> clipPlaneFuncList;
  std::unordered_map<Value *, DebugLoc> debugInfoMap;

  DxilRootSignatureVersion  rootSigVer;
  
  Value *EmitHLSLMatrixLoad(CGBuilderTy &Builder, Value *Ptr, QualType Ty);
  void EmitHLSLMatrixStore(CGBuilderTy &Builder, Value *Val, Value *DestPtr,
                           QualType Ty);
  // Flatten the val into scalar val and push into elts and eltTys.
  void FlattenValToInitList(CodeGenFunction &CGF, SmallVector<Value *, 4> &elts,
                       SmallVector<QualType, 4> &eltTys, QualType Ty,
                       Value *val);
  // Push every value on InitListExpr into EltValList and EltTyList.
  void ScanInitList(CodeGenFunction &CGF, InitListExpr *E,
                    SmallVector<Value *, 4> &EltValList,
                    SmallVector<QualType, 4> &EltTyList);

  void FlattenAggregatePtrToGepList(CodeGenFunction &CGF, Value *Ptr,
                                    SmallVector<Value *, 4> &idxList,
                                    clang::QualType Type, llvm::Type *Ty,
                                    SmallVector<Value *, 4> &GepList,
                                    SmallVector<QualType, 4> &EltTyList);
  void LoadFlattenedGepList(CodeGenFunction &CGF, ArrayRef<Value *> GepList,
                            ArrayRef<QualType> EltTyList,
                            SmallVector<Value *, 4> &EltList);
  void StoreFlattenedGepList(CodeGenFunction &CGF, ArrayRef<Value *> GepList,
                             ArrayRef<QualType> GepTyList,
                             ArrayRef<Value *> EltValList,
                             ArrayRef<QualType> SrcTyList);

  void EmitHLSLAggregateCopy(CodeGenFunction &CGF, llvm::Value *SrcPtr,
                                   llvm::Value *DestPtr,
                                   SmallVector<Value *, 4> &idxList,
                                   clang::QualType SrcType,
                                   clang::QualType DestType,
                                   llvm::Type *Ty);

  void EmitHLSLFlatConversionToAggregate(CodeGenFunction &CGF, Value *SrcVal,
                                         llvm::Value *DestPtr,
                                         SmallVector<Value *, 4> &idxList,
                                         QualType Type, QualType SrcType,
                                         llvm::Type *Ty);

  void EmitHLSLRootSignature(CodeGenFunction &CGF, HLSLRootSignatureAttr *RSA,
                             llvm::Function *Fn);

  void CheckParameterAnnotation(SourceLocation SLoc,
                                const DxilParameterAnnotation &paramInfo,
                                bool isPatchConstantFunction);
  void CheckParameterAnnotation(SourceLocation SLoc,
                                DxilParamInputQual paramQual,
                                llvm::StringRef semFullName,
                                bool isPatchConstantFunction);
  void SetEntryFunction();
  SourceLocation SetSemantic(const NamedDecl *decl,
                             DxilParameterAnnotation &paramInfo);

  hlsl::InterpolationMode GetInterpMode(const Decl *decl, CompType compType,
                                        bool bKeepUndefined);
  hlsl::CompType GetCompType(const BuiltinType *BT);
  // save intrinsic opcode
  std::vector<std::pair<Function *, unsigned>> m_IntrinsicMap;
  void AddHLSLIntrinsicOpcodeToFunction(Function *, unsigned opcode);

  // Type annotation related.
  unsigned ConstructStructAnnotation(DxilStructAnnotation *annotation,
                                     const RecordDecl *RD,
                                     DxilTypeSystem &dxilTypeSys);
  unsigned AddTypeAnnotation(QualType Ty, DxilTypeSystem &dxilTypeSys,
                             unsigned &arrayEltSize);

  std::unordered_map<Constant*, DxilFieldAnnotation> m_ConstVarAnnotationMap;

public:
  CGMSHLSLRuntime(CodeGenModule &CGM);

  bool IsHlslObjectType(llvm::Type * Ty) override;

  /// Add resouce to the program
  void addResource(Decl *D) override;
  void FinishCodeGen() override;
  bool IsTrivalInitListExpr(CodeGenFunction &CGF, InitListExpr *E) override;
  Value *EmitHLSLInitListExpr(CodeGenFunction &CGF, InitListExpr *E, Value *DestPtr) override;
  Constant *EmitHLSLConstInitListExpr(CodeGenModule &CGM, InitListExpr *E) override;

  RValue EmitHLSLBuiltinCallExpr(CodeGenFunction &CGF, const FunctionDecl *FD,
                                 const CallExpr *E,
                                 ReturnValueSlot ReturnValue) override;
  void EmitHLSLOutParamConversionInit(
      CodeGenFunction &CGF, const FunctionDecl *FD, const CallExpr *E,
      llvm::SmallVector<LValue, 8> &castArgList,
      llvm::SmallVector<const Stmt *, 8> &argList,
      const std::function<void(const VarDecl *, llvm::Value *)> &TmpArgMap)
      override;
  void EmitHLSLOutParamConversionCopyBack(
      CodeGenFunction &CGF, llvm::SmallVector<LValue, 8> &castArgList) override;

  Value *EmitHLSLMatrixOperationCall(CodeGenFunction &CGF, const clang::Expr *E,
                                     llvm::Type *RetType,
                                     ArrayRef<Value *> paramList) override;

  void EmitHLSLDiscard(CodeGenFunction &CGF) override;

  Value *EmitHLSLMatrixSubscript(CodeGenFunction &CGF, llvm::Type *RetType,
                                 Value *Ptr, Value *Idx, QualType Ty) override;

  Value *EmitHLSLMatrixElement(CodeGenFunction &CGF, llvm::Type *RetType,
                               ArrayRef<Value *> paramList,
                               QualType Ty) override;

  Value *EmitHLSLMatrixLoad(CodeGenFunction &CGF, Value *Ptr,
                            QualType Ty) override;
  void EmitHLSLMatrixStore(CodeGenFunction &CGF, Value *Val, Value *DestPtr,
                           QualType Ty) override;

  void EmitHLSLAggregateCopy(CodeGenFunction &CGF, llvm::Value *SrcPtr,
                                   llvm::Value *DestPtr,
                                   clang::QualType Ty) override;

  void EmitHLSLAggregateStore(CodeGenFunction &CGF, llvm::Value *Val,
                                   llvm::Value *DestPtr,
                                   clang::QualType Ty) override;

  void EmitHLSLFlatConversionToAggregate(CodeGenFunction &CGF, Value *Val,
                                         Value *DestPtr,
                                         QualType Ty,
                                         QualType SrcTy) override;
  Value *EmitHLSLLiteralCast(CodeGenFunction &CGF, Value *Src, QualType SrcType,
                             QualType DstType) override;

  void EmitHLSLFlatConversionAggregateCopy(CodeGenFunction &CGF, llvm::Value *SrcPtr,
                                   clang::QualType SrcTy,
                                   llvm::Value *DestPtr,
                                   clang::QualType DestTy) override;
  void AddHLSLFunctionInfo(llvm::Function *, const FunctionDecl *FD) override;
  void EmitHLSLFunctionProlog(llvm::Function *, const FunctionDecl *FD) override;

  void AddControlFlowHint(CodeGenFunction &CGF, const Stmt &S,
                          llvm::TerminatorInst *TI,
                          ArrayRef<const Attr *> Attrs) override;
  
  void FinishAutoVar(CodeGenFunction &CGF, const VarDecl &D, llvm::Value *V) override;

  /// Get or add constant to the program
  HLCBuffer &GetOrCreateCBuffer(HLSLBufferDecl *D);
};
}

void clang::CompileRootSignature(
    StringRef rootSigStr, DiagnosticsEngine &Diags, SourceLocation SLoc,
    hlsl::DxilRootSignatureVersion rootSigVer,
    hlsl::RootSignatureHandle *pRootSigHandle) {
  std::string OSStr;
  llvm::raw_string_ostream OS(OSStr);
  hlsl::DxilVersionedRootSignatureDesc *D = nullptr;

  if (ParseHLSLRootSignature(rootSigStr.data(), rootSigStr.size(), rootSigVer,
                             &D, SLoc, Diags)) {
    CComPtr<IDxcBlob> pSignature;
    CComPtr<IDxcBlobEncoding> pErrors;
    hlsl::SerializeRootSignature(D, &pSignature, &pErrors, false);
    if (pSignature == nullptr) {
      assert(pErrors != nullptr && "else serialize failed with no msg");
      ReportHLSLRootSigError(Diags, SLoc, (char *)pErrors->GetBufferPointer(),
                             pErrors->GetBufferSize());
      hlsl::DeleteRootSignature(D);
    } else {
      pRootSigHandle->Assign(D, pSignature);
    }
  }
}

//------------------------------------------------------------------------------
//
// CGMSHLSLRuntime methods.
//
CGMSHLSLRuntime::CGMSHLSLRuntime(CodeGenModule &CGM)
    : CGHLSLRuntime(CGM), Context(CGM.getLLVMContext()), EntryFunc(nullptr),
      TheModule(CGM.getModule()), legacyLayout(HLModule::GetLegacyDataLayoutDesc()),
      CBufferType(
          llvm::StructType::create(TheModule.getContext(), "ConstantBuffer")) {
  const hlsl::ShaderModel *SM =
      hlsl::ShaderModel::GetByName(CGM.getCodeGenOpts().HLSLProfile.c_str());
  // Only accept valid, 6.0 shader model.
  if (!SM->IsValid() || SM->GetMajor() != 6) {
    DiagnosticsEngine &Diags = CGM.getDiags();
    unsigned DiagID =
        Diags.getCustomDiagID(DiagnosticsEngine::Error, "invalid profile %0");
    Diags.Report(DiagID) << CGM.getCodeGenOpts().HLSLProfile;
    return;
  }
  // TODO: add AllResourceBound.
  if (CGM.getCodeGenOpts().HLSLAvoidControlFlow && !CGM.getCodeGenOpts().HLSLAllResourcesBound) {
    if (SM->GetMajor() >= 5 && SM->GetMinor() >= 1) {
      DiagnosticsEngine &Diags = CGM.getDiags();
      unsigned DiagID =
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "Gfa option cannot be used in SM_5_1+ unless "
                                "all_resources_bound flag is specified");
      Diags.Report(DiagID);
    }
  }
  // Create HLModule.
  const bool skipInit = true;
  m_pHLModule = &TheModule.GetOrCreateHLModule(skipInit);

  // Set Option.
  HLOptions opts;
  opts.bIEEEStrict = CGM.getCodeGenOpts().UnsafeFPMath;
  opts.bDefaultRowMajor = CGM.getCodeGenOpts().HLSLDefaultRowMajor;
  opts.bDisableOptimizations = CGM.getCodeGenOpts().DisableLLVMOpts;
  opts.bLegacyCBufferLoad = !CGM.getCodeGenOpts().HLSLNotUseLegacyCBufLoad;
  opts.bAllResourcesBound = CGM.getCodeGenOpts().HLSLAllResourcesBound;
  opts.PackingStrategy = CGM.getCodeGenOpts().HLSLSignaturePackingStrategy;
  m_pHLModule->SetHLOptions(opts);

  m_bDebugInfo = CGM.getCodeGenOpts().getDebugInfo() == CodeGenOptions::FullDebugInfo;

  // set profile
  m_pHLModule->SetShaderModel(SM);
  // set entry name
  m_pHLModule->SetEntryFunctionName(CGM.getCodeGenOpts().HLSLEntryFunction);

  // set root signature version.
  if (CGM.getLangOpts().RootSigMinor == 0) {
    rootSigVer = hlsl::DxilRootSignatureVersion::Version_1_0;
  }
  else {
    DXASSERT(CGM.getLangOpts().RootSigMinor == 1,
      "else CGMSHLSLRuntime Constructor needs to be updated");
    rootSigVer = hlsl::DxilRootSignatureVersion::Version_1_1;
  }

  DXASSERT(CGM.getLangOpts().RootSigMajor == 1,
           "else CGMSHLSLRuntime Constructor needs to be updated");

  // add globalCB
  unique_ptr<HLCBuffer> CB = llvm::make_unique<HLCBuffer>();
  std::string globalCBName = "$Globals";
  CB->SetGlobalSymbol(nullptr);
  CB->SetGlobalName(globalCBName);
  globalCBIndex = m_pHLModule->GetCBuffers().size();
  CB->SetID(globalCBIndex);
  CB->SetRangeSize(1);
  CB->SetLowerBound(UINT_MAX);
  DXVERIFY_NOMSG(globalCBIndex == m_pHLModule->AddCBuffer(std::move(CB)));
}

bool CGMSHLSLRuntime::IsHlslObjectType(llvm::Type *Ty) {
  return HLModule::IsHLSLObjectType(Ty);
}

void CGMSHLSLRuntime::AddHLSLIntrinsicOpcodeToFunction(Function *F,
                                                       unsigned opcode) {
  m_IntrinsicMap.emplace_back(F,opcode);
}

void CGMSHLSLRuntime::CheckParameterAnnotation(
    SourceLocation SLoc, const DxilParameterAnnotation &paramInfo,
    bool isPatchConstantFunction) {
  if (!paramInfo.HasSemanticString()) {
    return;
  }
  llvm::StringRef semFullName = paramInfo.GetSemanticStringRef();
  DxilParamInputQual paramQual = paramInfo.GetParamInputQual();
  if (paramQual == DxilParamInputQual::Inout) {
    CheckParameterAnnotation(SLoc, DxilParamInputQual::In, semFullName, isPatchConstantFunction);
    CheckParameterAnnotation(SLoc, DxilParamInputQual::Out, semFullName, isPatchConstantFunction);
    return;
  }
  CheckParameterAnnotation(SLoc, paramQual, semFullName, isPatchConstantFunction);
}

void CGMSHLSLRuntime::CheckParameterAnnotation(
    SourceLocation SLoc, DxilParamInputQual paramQual, llvm::StringRef semFullName,
    bool isPatchConstantFunction) {
  const ShaderModel *SM = m_pHLModule->GetShaderModel();
  DXIL::SigPointKind sigPoint = SigPointFromInputQual(
    paramQual, SM->GetKind(), isPatchConstantFunction);

  llvm::StringRef semName;
  unsigned semIndex;
  Semantic::DecomposeNameAndIndex(semFullName, &semName, &semIndex);

  const Semantic *pSemantic =
      Semantic::GetByName(semName, sigPoint, SM->GetMajor(), SM->GetMinor());
  if (pSemantic->IsInvalid()) {
    DiagnosticsEngine &Diags = CGM.getDiags();
    const ShaderModel *shader = m_pHLModule->GetShaderModel();
    unsigned DiagID =
        Diags.getCustomDiagID(DiagnosticsEngine::Error, "invalid semantic '%0' for %1 %2.%3");
    Diags.Report(SLoc, DiagID) << semName << shader->GetKindName() << shader->GetMajor() << shader->GetMinor();
  }
}

SourceLocation
CGMSHLSLRuntime::SetSemantic(const NamedDecl *decl,
                             DxilParameterAnnotation &paramInfo) {
  for (const hlsl::UnusualAnnotation *it : decl->getUnusualAnnotations()) {
    switch (it->getKind()) {
    case hlsl::UnusualAnnotation::UA_SemanticDecl: {
      const hlsl::SemanticDecl *sd = cast<hlsl::SemanticDecl>(it);
      paramInfo.SetSemanticString(sd->SemanticName);
      return it->Loc;
    }
    }
  }
  return SourceLocation();
}

static DXIL::TessellatorDomain StringToDomain(StringRef domain) {
  if (domain == "isoline")
    return DXIL::TessellatorDomain::IsoLine;
  if (domain == "tri")
    return DXIL::TessellatorDomain::Tri;
  if (domain == "quad")
    return DXIL::TessellatorDomain::Quad;
  return DXIL::TessellatorDomain::Undefined;
}

static DXIL::TessellatorPartitioning StringToPartitioning(StringRef partition) {
  if (partition == "integer")
    return DXIL::TessellatorPartitioning::Integer;
  if (partition == "pow2")
    return DXIL::TessellatorPartitioning::Pow2;
  if (partition == "fractional_even")
    return DXIL::TessellatorPartitioning::FractionalEven;
  if (partition == "fractional_odd")
    return DXIL::TessellatorPartitioning::FractionalOdd;
  return DXIL::TessellatorPartitioning::Undefined;
}

static DXIL::TessellatorOutputPrimitive
StringToTessOutputPrimitive(StringRef primitive) {
  if (primitive == "point")
    return DXIL::TessellatorOutputPrimitive::Point;
  if (primitive == "line")
    return DXIL::TessellatorOutputPrimitive::Line;
  if (primitive == "triangle_cw")
    return DXIL::TessellatorOutputPrimitive::TriangleCW;
  if (primitive == "triangle_ccw")
    return DXIL::TessellatorOutputPrimitive::TriangleCCW;
  return DXIL::TessellatorOutputPrimitive::Undefined;
}

static unsigned AlignTo8Bytes(unsigned offset, bool b8BytesAlign) {
  DXASSERT((offset & 0x3) == 0, "offset should be divisible by 4");
  if (!b8BytesAlign)
    return offset;
  else if ((offset & 0x7) == 0)
    return offset;
  else
    return offset + 4;
}

static unsigned AlignBaseOffset(unsigned baseOffset, unsigned size,
                                 QualType Ty, bool bDefaultRowMajor) {
  bool b8BytesAlign = false;
  if (Ty->isBuiltinType()) {
    const clang::BuiltinType *BT = Ty->getAs<clang::BuiltinType>();
    if (BT->getKind() == clang::BuiltinType::Kind::Double ||
        BT->getKind() == clang::BuiltinType::Kind::LongLong)
      b8BytesAlign = true;
  }

  if (unsigned remainder = (baseOffset & 0xf)) {
    // Align to 4 x 4 bytes.
    unsigned aligned = baseOffset - remainder + 16;
    // If cannot fit in the remainder, need align.
    bool bNeedAlign = (remainder + size) > 16;
    // Array always start aligned.
    bNeedAlign |= Ty->isArrayType();

    if (IsHLSLMatType(Ty)) {
      bool bColMajor = !bDefaultRowMajor;
      if (const AttributedType *AT = dyn_cast<AttributedType>(Ty)) {
        switch (AT->getAttrKind()) {
        case AttributedType::Kind::attr_hlsl_column_major:
          bColMajor = true;
          break;
        case AttributedType::Kind::attr_hlsl_row_major:
          bColMajor = false;
          break;
        default:
          // Do nothing
          break;
        }
      }

      unsigned row, col;
      hlsl::GetHLSLMatRowColCount(Ty, row, col);

      bNeedAlign |= bColMajor && col > 1;
      bNeedAlign |= !bColMajor && row > 1;
    }

    if (bNeedAlign)
      return AlignTo8Bytes(aligned, b8BytesAlign);
    else
      return AlignTo8Bytes(baseOffset, b8BytesAlign);

  } else
    return baseOffset;
}

static unsigned AlignBaseOffset(QualType Ty, unsigned baseOffset,
                                bool bDefaultRowMajor,
                                CodeGen::CodeGenModule &CGM,
                                llvm::DataLayout &layout) {
  QualType paramTy = Ty.getCanonicalType();
  if (const ReferenceType *RefType = dyn_cast<ReferenceType>(paramTy))
    paramTy = RefType->getPointeeType();

  // Get size.
  llvm::Type *Type = CGM.getTypes().ConvertType(paramTy);
  unsigned size = layout.getTypeAllocSize(Type);
  return AlignBaseOffset(baseOffset, size, paramTy, bDefaultRowMajor);
}

static unsigned GetMatrixSizeInCB(QualType Ty, bool defaultRowMajor,
                                  bool b64Bit) {
  bool bColMajor = !defaultRowMajor;
  if (const AttributedType *AT = dyn_cast<AttributedType>(Ty)) {
    switch (AT->getAttrKind()) {
    case AttributedType::Kind::attr_hlsl_column_major:
      bColMajor = true;
      break;
    case AttributedType::Kind::attr_hlsl_row_major:
      bColMajor = false;
      break;
    default:
      // Do nothing
      break;
    }
  }

  unsigned row, col;
  hlsl::GetHLSLMatRowColCount(Ty, row, col);

  unsigned EltSize = b64Bit ? 8 : 4;

  // Align to 4 * 4bytes.
  unsigned alignment = 4 * 4;

  if (bColMajor) {
    unsigned rowSize = EltSize * row;
    // 3x64bit or 4x64bit align to 32 bytes.
    if (rowSize > alignment)
      alignment <<= 1;

    return alignment * (col - 1) + row * EltSize;
  } else {
    unsigned rowSize = EltSize * col;
    // 3x64bit or 4x64bit align to 32 bytes.
    if (rowSize > alignment)
      alignment <<= 1;
    return alignment * (row - 1) + col * EltSize;
  }
}

static CompType::Kind BuiltinTyToCompTy(const BuiltinType *BTy, bool bSNorm,
                                        bool bUNorm) {
  CompType::Kind kind = CompType::Kind::Invalid;

  switch (BTy->getKind()) {
  case BuiltinType::UInt:
    kind = CompType::Kind::U32;
    break;
  case BuiltinType::UShort:
    kind = CompType::Kind::U16;
    break;
  case BuiltinType::ULongLong:
    kind = CompType::Kind::U64;
    break;
  case BuiltinType::Int:
    kind = CompType::Kind::I32;
    break;
  case BuiltinType::Min12Int:
  case BuiltinType::Short:
    kind = CompType::Kind::I16;
    break;
  case BuiltinType::LongLong:
    kind = CompType::Kind::I64;
    break;
  case BuiltinType::Min10Float:
  case BuiltinType::Half:
    if (bSNorm)
      kind = CompType::Kind::SNormF16;
    else if (bUNorm)
      kind = CompType::Kind::UNormF16;
    else
      kind = CompType::Kind::F16;
    break;
  case BuiltinType::Float:
    if (bSNorm)
      kind = CompType::Kind::SNormF32;
    else if (bUNorm)
      kind = CompType::Kind::UNormF32;
    else
      kind = CompType::Kind::F32;
    break;
  case BuiltinType::Double:
    if (bSNorm)
      kind = CompType::Kind::SNormF64;
    else if (bUNorm)
      kind = CompType::Kind::UNormF64;
    else
      kind = CompType::Kind::F64;
    break;
  case BuiltinType::Bool:
    kind = CompType::Kind::I1;
    break;
  }
  return kind;
}

static void ConstructFieldAttributedAnnotation(DxilFieldAnnotation &fieldAnnotation, QualType fieldTy, bool bDefaultRowMajor) {
  QualType Ty = fieldTy;
  if (Ty->isReferenceType())
    Ty = Ty.getNonReferenceType();

  // Get element type.
  if (Ty->isArrayType()) {
    while (isa<clang::ArrayType>(Ty)) {
      const clang::ArrayType *ATy = dyn_cast<clang::ArrayType>(Ty);
      Ty = ATy->getElementType();
    }
  }

  QualType EltTy = Ty;
  if (hlsl::IsHLSLMatType(Ty)) {
    DxilMatrixAnnotation Matrix;
    Matrix.Orientation = bDefaultRowMajor ? MatrixOrientation::RowMajor
                                          : MatrixOrientation::ColumnMajor;
    if (const AttributedType *AT = dyn_cast<AttributedType>(Ty)) {
      switch (AT->getAttrKind()) {
      case AttributedType::Kind::attr_hlsl_column_major:
        Matrix.Orientation = MatrixOrientation::ColumnMajor;
        break;
      case AttributedType::Kind::attr_hlsl_row_major:
        Matrix.Orientation = MatrixOrientation::RowMajor;
        break;
      default:
        // Do nothing
        break;
      }
    }

    unsigned row, col;
    hlsl::GetHLSLMatRowColCount(Ty, row, col);
    Matrix.Cols = col;
    Matrix.Rows = row;
    fieldAnnotation.SetMatrixAnnotation(Matrix);
    EltTy = hlsl::GetHLSLMatElementType(Ty);
  }

  if (hlsl::IsHLSLVecType(Ty))
    EltTy = hlsl::GetHLSLVecElementType(Ty);

  bool bSNorm = false;
  bool bUNorm = false;

  if (const AttributedType *AT = dyn_cast<AttributedType>(Ty)) {
    switch (AT->getAttrKind()) {
    case AttributedType::Kind::attr_hlsl_snorm:
      bSNorm = true;
      break;
    case AttributedType::Kind::attr_hlsl_unorm:
      bUNorm = true;
      break;
    default:
      // Do nothing
      break;
    }
  }

  if (EltTy->isBuiltinType()) {
    const BuiltinType *BTy = EltTy->getAs<BuiltinType>();
    CompType::Kind kind = BuiltinTyToCompTy(BTy, bSNorm, bUNorm);
    fieldAnnotation.SetCompType(kind);
  }
  else
    DXASSERT(!bSNorm && !bUNorm, "snorm/unorm on invalid type, validate at handleHLSLTypeAttr");
}

static void ConstructFieldInterpolation(DxilFieldAnnotation &fieldAnnotation,
                                      FieldDecl *fieldDecl) {
  // Keep undefined for interpMode here.
  InterpolationMode InterpMode = {fieldDecl->hasAttr<HLSLNoInterpolationAttr>(),
                                  fieldDecl->hasAttr<HLSLLinearAttr>(),
                                  fieldDecl->hasAttr<HLSLNoPerspectiveAttr>(),
                                  fieldDecl->hasAttr<HLSLCentroidAttr>(),
                                  fieldDecl->hasAttr<HLSLSampleAttr>()};
  if (InterpMode.GetKind() != InterpolationMode::Kind::Undefined)
    fieldAnnotation.SetInterpolationMode(InterpMode);
}

unsigned CGMSHLSLRuntime::ConstructStructAnnotation(DxilStructAnnotation *annotation,
                                      const RecordDecl *RD,
                                      DxilTypeSystem &dxilTypeSys) {
  unsigned fieldIdx = 0;
  unsigned offset = 0;
  bool bDefaultRowMajor = m_pHLModule->GetHLOptions().bDefaultRowMajor;
  if (const CXXRecordDecl *CXXRD = dyn_cast<CXXRecordDecl>(RD)) {
    if (CXXRD->getNumBases()) {
      // Add base as field.
      for (const auto &I : CXXRD->bases()) {
        const CXXRecordDecl *BaseDecl =
            cast<CXXRecordDecl>(I.getType()->castAs<RecordType>()->getDecl());
        std::string fieldSemName = "";

        QualType parentTy = QualType(BaseDecl->getTypeForDecl(), 0);

        // Align offset.
        offset = AlignBaseOffset(parentTy, offset, bDefaultRowMajor, CGM,
                                 legacyLayout);

        unsigned CBufferOffset = offset;

        unsigned arrayEltSize = 0;
        // Process field to make sure the size of field is ready.
        unsigned size =
            AddTypeAnnotation(parentTy, dxilTypeSys, arrayEltSize);

        // Update offset.
        offset += size;

        if (size > 0) {
          DxilFieldAnnotation &fieldAnnotation =
              annotation->GetFieldAnnotation(fieldIdx++);

          fieldAnnotation.SetCBufferOffset(CBufferOffset);
          fieldAnnotation.SetFieldName(BaseDecl->getNameAsString());
        }
      }
    }
  }

  for (auto fieldDecl : RD->fields()) {
    std::string fieldSemName = "";

    QualType fieldTy = fieldDecl->getType();
    
    // Align offset.
    offset = AlignBaseOffset(fieldTy, offset, bDefaultRowMajor, CGM, legacyLayout);

    unsigned CBufferOffset = offset;

    bool userOffset = false;
    // Try to get info from fieldDecl.
    for (const hlsl::UnusualAnnotation *it :
         fieldDecl->getUnusualAnnotations()) {
      switch (it->getKind()) {
      case hlsl::UnusualAnnotation::UA_SemanticDecl: {
        const hlsl::SemanticDecl *sd = cast<hlsl::SemanticDecl>(it);
        fieldSemName = sd->SemanticName;
      } break;
      case hlsl::UnusualAnnotation::UA_ConstantPacking: {
        const hlsl::ConstantPacking *cp = cast<hlsl::ConstantPacking>(it);
        CBufferOffset = cp->Subcomponent << 2;
        CBufferOffset += cp->ComponentOffset;
        // Change to byte.
        CBufferOffset <<= 2;
        userOffset = true;
      } break;
      case hlsl::UnusualAnnotation::UA_RegisterAssignment: {
        // register assignment only works on global constant.
        DiagnosticsEngine &Diags = CGM.getDiags();
        unsigned DiagID = Diags.getCustomDiagID(
            DiagnosticsEngine::Error,
            "location semantics cannot be specified on members.");
        Diags.Report(it->Loc, DiagID);
        return 0;
      } break;
      default:
        llvm_unreachable("only semantic for input/output");
        break;
      }
    }

    unsigned arrayEltSize = 0;
    // Process field to make sure the size of field is ready.
    unsigned size = AddTypeAnnotation(fieldDecl->getType(), dxilTypeSys, arrayEltSize);

    // Update offset.
    offset += size;
    
    DxilFieldAnnotation &fieldAnnotation = annotation->GetFieldAnnotation(fieldIdx++);

    ConstructFieldAttributedAnnotation(fieldAnnotation, fieldTy, bDefaultRowMajor);
    ConstructFieldInterpolation(fieldAnnotation, fieldDecl);
    if (fieldDecl->hasAttr<HLSLPreciseAttr>())
      fieldAnnotation.SetPrecise();

    fieldAnnotation.SetCBufferOffset(CBufferOffset);
    fieldAnnotation.SetFieldName(fieldDecl->getName());
    if (!fieldSemName.empty())
      fieldAnnotation.SetSemanticString(fieldSemName);
  }

  annotation->SetCBufferSize(offset);
  if (offset == 0) {
    annotation->MarkEmptyStruct();
  }
  return offset;
}

static bool IsElementInputOutputType(QualType Ty) {
  return Ty->isBuiltinType() || hlsl::IsHLSLVecMatType(Ty);
}

// Return the size for constant buffer of each decl.
unsigned CGMSHLSLRuntime::AddTypeAnnotation(QualType Ty,
                                            DxilTypeSystem &dxilTypeSys,
                                            unsigned &arrayEltSize) {
  QualType paramTy = Ty.getCanonicalType();
  if (const ReferenceType *RefType = dyn_cast<ReferenceType>(paramTy))
    paramTy = RefType->getPointeeType();

  // Get size.
  llvm::Type *Type = CGM.getTypes().ConvertType(paramTy);
  unsigned size = legacyLayout.getTypeAllocSize(Type);

  if (IsHLSLMatType(Ty)) {
    unsigned col, row;
    llvm::Type *EltTy = HLMatrixLower::GetMatrixInfo(Type, col, row);
    bool b64Bit = legacyLayout.getTypeAllocSize(EltTy) == 8;
    size = GetMatrixSizeInCB(Ty, m_pHLModule->GetHLOptions().bDefaultRowMajor,
                             b64Bit);
  }
  // Skip element types.
  if (IsElementInputOutputType(paramTy))
    return size;
  else if (IsHLSLStreamOutputType(Ty)) {
    return AddTypeAnnotation(GetHLSLOutputPatchElementType(Ty), dxilTypeSys,
                             arrayEltSize);
  } else if (IsHLSLInputPatchType(Ty))
    return AddTypeAnnotation(GetHLSLInputPatchElementType(Ty), dxilTypeSys,
                             arrayEltSize);
  else if (IsHLSLOutputPatchType(Ty))
    return AddTypeAnnotation(GetHLSLOutputPatchElementType(Ty), dxilTypeSys,
                             arrayEltSize);
  else if (const RecordType *RT = paramTy->getAsStructureType()) {
    RecordDecl *RD = RT->getDecl();
    llvm::StructType *ST = CGM.getTypes().ConvertRecordDeclType(RD);
    // Skip if already created.
    if (DxilStructAnnotation *annotation = dxilTypeSys.GetStructAnnotation(ST)) {
      unsigned structSize = annotation->GetCBufferSize();
      return structSize;
    }
    DxilStructAnnotation *annotation = dxilTypeSys.AddStructAnnotation(ST);

    return ConstructStructAnnotation(annotation, RD, dxilTypeSys);
  } else if (const RecordType *RT = dyn_cast<RecordType>(paramTy)) {
    // For this pointer.
    RecordDecl *RD = RT->getDecl();
    llvm::StructType *ST = CGM.getTypes().ConvertRecordDeclType(RD);
    // Skip if already created.
    if (DxilStructAnnotation *annotation = dxilTypeSys.GetStructAnnotation(ST)) {
      unsigned structSize = annotation->GetCBufferSize();
      return structSize;
    }
    DxilStructAnnotation *annotation = dxilTypeSys.AddStructAnnotation(ST);

    return ConstructStructAnnotation(annotation, RD, dxilTypeSys);
  } else if (IsHLSLResouceType(Ty)) {
    // Save result type info.
    AddTypeAnnotation(GetHLSLResourceResultType(Ty), dxilTypeSys, arrayEltSize);
    // Resource don't count for cbuffer size.
    return 0;
  } else {
    unsigned arraySize = 0;
    QualType arrayElementTy = Ty;
    if (Ty->isConstantArrayType()) {
      const ConstantArrayType *arrayTy =
        CGM.getContext().getAsConstantArrayType(Ty);
      DXASSERT(arrayTy != nullptr, "Must array type here");

      arraySize = arrayTy->getSize().getLimitedValue();
      arrayElementTy = arrayTy->getElementType();
    }
    else if (Ty->isIncompleteArrayType()) {
      const IncompleteArrayType *arrayTy = CGM.getContext().getAsIncompleteArrayType(Ty);
      arrayElementTy = arrayTy->getElementType();
    } else
      DXASSERT(0, "Must array type here");

    unsigned elementSize = AddTypeAnnotation(arrayElementTy, dxilTypeSys, arrayEltSize);
    // Only set arrayEltSize once.
    if (arrayEltSize == 0)
      arrayEltSize = elementSize;
    // Align to 4 * 4bytes.
    unsigned alignedSize = (elementSize + 15) & 0xfffffff0;
    return alignedSize * (arraySize - 1) + elementSize;
  }
}


static DxilResource::Kind KeywordToKind(StringRef keyword) {
  // TODO: refactor for faster search (switch by 1/2/3 first letters, then
  // compare)
  if (keyword == "Texture1D" || keyword == "RWTexture1D" || keyword == "RasterizerOrderedTexture1D")
    return DxilResource::Kind::Texture1D;
  if (keyword == "Texture2D" || keyword == "RWTexture2D" || keyword == "RasterizerOrderedTexture2D")
    return DxilResource::Kind::Texture2D;
  if (keyword == "Texture2DMS" || keyword == "RWTexture2DMS")
    return DxilResource::Kind::Texture2DMS;
  if (keyword == "Texture3D" || keyword == "RWTexture3D" || keyword == "RasterizerOrderedTexture3D")
    return DxilResource::Kind::Texture3D;
  if (keyword == "TextureCube" || keyword == "RWTextureCube")
    return DxilResource::Kind::TextureCube;

  if (keyword == "Texture1DArray" || keyword == "RWTexture1DArray" || keyword == "RasterizerOrderedTexture1DArray")
    return DxilResource::Kind::Texture1DArray;
  if (keyword == "Texture2DArray" || keyword == "RWTexture2DArray" || keyword == "RasterizerOrderedTexture2DArray")
    return DxilResource::Kind::Texture2DArray;
  if (keyword == "Texture2DMSArray" || keyword == "RWTexture2DMSArray")
    return DxilResource::Kind::Texture2DMSArray;
  if (keyword == "TextureCubeArray" || keyword == "RWTextureCubeArray")
    return DxilResource::Kind::TextureCubeArray;

  if (keyword == "ByteAddressBuffer" || keyword == "RWByteAddressBuffer" || keyword == "RasterizerOrderedByteAddressBuffer")
    return DxilResource::Kind::RawBuffer;

  if (keyword == "StructuredBuffer" || keyword == "RWStructuredBuffer" || keyword == "RasterizerOrderedStructuredBuffer")
    return DxilResource::Kind::StructuredBuffer;

  if (keyword == "AppendStructuredBuffer" || keyword == "ConsumeStructuredBuffer")
    return DxilResource::Kind::StructuredBuffer;

  // TODO: this is not efficient.
  bool isBuffer = keyword == "Buffer";
  isBuffer |= keyword == "RWBuffer";
  isBuffer |= keyword == "RasterizerOrderedBuffer";
  if (isBuffer)
    return DxilResource::Kind::TypedBuffer;

  return DxilResource::Kind::Invalid;
}

static DxilSampler::SamplerKind KeywordToSamplerKind(const std::string &keyword) {
  // TODO: refactor for faster search (switch by 1/2/3 first letters, then
  // compare)
  if (keyword == "SamplerState")
    return DxilSampler::SamplerKind::Default;

  if (keyword == "SamplerComparisonState")
    return DxilSampler::SamplerKind::Comparison;

  return DxilSampler::SamplerKind::Invalid;
}

void CGMSHLSLRuntime::AddHLSLFunctionInfo(Function *F, const FunctionDecl *FD) {
  // Add hlsl intrinsic attr
  unsigned intrinsicOpcode;
  StringRef intrinsicGroup;
  if (hlsl::GetIntrinsicOp(FD, intrinsicOpcode, intrinsicGroup)) {
    AddHLSLIntrinsicOpcodeToFunction(F, intrinsicOpcode);
    F->addFnAttr(hlsl::HLPrefix, intrinsicGroup);
    // Save resource type annotation.
    if (const CXXMethodDecl *MD = dyn_cast<CXXMethodDecl>(FD)) {
      const CXXRecordDecl *RD = MD->getParent();
      // For nested case like sample_slice_type.
      if (const CXXRecordDecl *PRD =
              dyn_cast<CXXRecordDecl>(RD->getDeclContext())) {
        RD = PRD;
      }

      QualType recordTy = MD->getASTContext().getRecordType(RD);
      hlsl::DxilResourceBase::Class resClass = TypeToClass(recordTy);
      llvm::Type *Ty = CGM.getTypes().ConvertType(recordTy);
      llvm::FunctionType *FT = F->getFunctionType();
      // Save resource type metadata.
      switch (resClass) {
      case DXIL::ResourceClass::UAV: {
        DxilResource UAV;
        // TODO: save globalcoherent to variable in EmitHLSLBuiltinCallExpr.
        SetUAVSRV(FD->getLocation(), resClass, &UAV, RD);
        // Set global symbol to save type.
        UAV.SetGlobalSymbol(UndefValue::get(Ty));
        MDNode *MD = m_pHLModule->DxilUAVToMDNode(UAV);
        resMetadataMap[Ty] = MD;
      } break;
      case DXIL::ResourceClass::SRV: {
        DxilResource SRV;
        SetUAVSRV(FD->getLocation(), resClass, &SRV, RD);
        // Set global symbol to save type.
        SRV.SetGlobalSymbol(UndefValue::get(Ty));
        MDNode *Meta = m_pHLModule->DxilSRVToMDNode(SRV);
        resMetadataMap[Ty] = Meta;
        if (FT->getNumParams() > 1) {
          QualType paramTy = MD->getParamDecl(0)->getType();
          // Add sampler type.
          if (TypeToClass(paramTy) == DXIL::ResourceClass::Sampler) {
            llvm::Type *Ty = FT->getParamType(1)->getPointerElementType();
            DxilSampler S;
            const RecordType *RT = paramTy->getAs<RecordType>();
            DxilSampler::SamplerKind kind =
                KeywordToSamplerKind(RT->getDecl()->getName());
            S.SetSamplerKind(kind);
            // Set global symbol to save type.
            S.SetGlobalSymbol(UndefValue::get(Ty));
            MDNode *MD = m_pHLModule->DxilSamplerToMDNode(S);
            resMetadataMap[Ty] = MD;
          }
        }
      } break;
      default:
        // Skip OutputStream for GS.
        break;
      }
    }

    StringRef lower;
    if (hlsl::GetIntrinsicLowering(FD, lower))
      hlsl::SetHLLowerStrategy(F, lower);

    // Don't need to add FunctionQual for intrinsic function.
    return;
  }

  // Set entry function
  const std::string &entryName = m_pHLModule->GetEntryFunctionName();
  bool isEntry = FD->getNameAsString() == entryName;
  if (isEntry)
    EntryFunc = F;

  std::unique_ptr<HLFunctionProps> funcProps =
      llvm::make_unique<HLFunctionProps>();

  // Save patch constant function to patchConstantFunctionMap.
  bool isPatchConstantFunction = false;
  if (CGM.getContext().IsPatchConstantFunctionDecl(FD)) {
    isPatchConstantFunction = true;
    if (patchConstantFunctionMap.count(FD->getName()) == 0)
      patchConstantFunctionMap[FD->getName()] = F;
    else {
      // TODO: This is not the same as how fxc handles patch constant functions.
      //  This will fail if more than one function with the same name has a
      //  SV_TessFactor semantic. Fxc just selects the last function defined
      //  that has the matching name when referenced by the patchconstantfunc
      //  attribute from the hull shader currently being compiled.
      // Report error
      DiagnosticsEngine &Diags = CGM.getDiags();
      unsigned DiagID =
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "Multiple definitions for patchconstantfunc.");
      Diags.Report(FD->getLocation(), DiagID);
      return;
    }

    for (ParmVarDecl *parmDecl : FD->parameters()) {
      QualType Ty = parmDecl->getType();
      if (IsHLSLOutputPatchType(Ty)) {
        funcProps->ShaderProps.HS.outputControlPoints =
            GetHLSLOutputPatchCount(parmDecl->getType());
      } else if (IsHLSLInputPatchType(Ty)) {
        funcProps->ShaderProps.HS.inputControlPoints =
            GetHLSLInputPatchCount(parmDecl->getType());
      }
    }
  }

  const ShaderModel *SM = m_pHLModule->GetShaderModel();

  // TODO: how to know VS/PS?
  funcProps->shaderKind = DXIL::ShaderKind::Invalid;

  DiagnosticsEngine &Diags = CGM.getDiags();
  // Geometry shader.
  bool isGS = false;
  if (const HLSLMaxVertexCountAttr *Attr =
          FD->getAttr<HLSLMaxVertexCountAttr>()) {
    isGS = true;
    funcProps->shaderKind = DXIL::ShaderKind::Geometry;
    funcProps->ShaderProps.GS.maxVertexCount = Attr->getCount();
    funcProps->ShaderProps.GS.inputPrimitive = DXIL::InputPrimitive::Undefined;

    if (isEntry && !SM->IsGS()) {
      unsigned DiagID =
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "attribute maxvertexcount only valid for GS.");
      Diags.Report(Attr->getLocation(), DiagID);
      return;
    }
  }
  if (const HLSLInstanceAttr *Attr = FD->getAttr<HLSLInstanceAttr>()) {
    unsigned instanceCount = Attr->getCount();
    funcProps->ShaderProps.GS.instanceCount = instanceCount;
    if (isEntry && !SM->IsGS()) {
      unsigned DiagID =
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "attribute maxvertexcount only valid for GS.");
      Diags.Report(Attr->getLocation(), DiagID);
      return;
    }
  } else {
    // Set default instance count.
    if (isGS)
      funcProps->ShaderProps.GS.instanceCount = 1;
  }

  // Computer shader.
  bool isCS = false;
  if (const HLSLNumThreadsAttr *Attr = FD->getAttr<HLSLNumThreadsAttr>()) {
    isCS = true;
    funcProps->shaderKind = DXIL::ShaderKind::Compute;

    funcProps->ShaderProps.CS.numThreads[0] = Attr->getX();
    funcProps->ShaderProps.CS.numThreads[1] = Attr->getY();
    funcProps->ShaderProps.CS.numThreads[2] = Attr->getZ();

    if (isEntry && !SM->IsCS()) {
      unsigned DiagID = Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "attribute numthreads only valid for CS.");
      Diags.Report(Attr->getLocation(), DiagID);
      return;
    }
  }

  // Hull shader.
  bool isHS = false;
  if (const HLSLPatchConstantFuncAttr *Attr =
          FD->getAttr<HLSLPatchConstantFuncAttr>()) {
    if (isEntry && !SM->IsHS()) {
      unsigned DiagID = Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "attribute patchconstantfunc only valid for HS.");
      Diags.Report(Attr->getLocation(), DiagID);
      return;
    }

    isHS = true;
    funcProps->shaderKind = DXIL::ShaderKind::Hull;
    StringRef funcName = Attr->getFunctionName();

    if (patchConstantFunctionMap.count(funcName) == 1) {
      Function *patchConstFunc = patchConstantFunctionMap[funcName];
      funcProps->ShaderProps.HS.patchConstantFunc = patchConstFunc;
      DXASSERT_NOMSG(m_pHLModule->HasHLFunctionProps(patchConstFunc));
      // Check no inout parameter for patch constant function.
      DxilFunctionAnnotation *patchConstFuncAnnotation =
          m_pHLModule->GetFunctionAnnotation(patchConstFunc);
      for (unsigned i = 0; i < patchConstFuncAnnotation->GetNumParameters();
           i++) {
        if (patchConstFuncAnnotation->GetParameterAnnotation(i)
                .GetParamInputQual() == DxilParamInputQual::Inout) {
          unsigned DiagID = Diags.getCustomDiagID(
              DiagnosticsEngine::Error,
              "Patch Constant function should not have inout param.");
          Diags.Report(Attr->getLocation(), DiagID);
          return;
        }
      }
    } else {
      // TODO: Bring this in line with fxc behavior.  In fxc, patchconstantfunc
      //  selection is based only on name (last function with matching name),
      //  not by whether it has SV_TessFactor output.
      //// Report error
      // DiagnosticsEngine &Diags = CGM.getDiags();
      // unsigned DiagID = Diags.getCustomDiagID(DiagnosticsEngine::Error,
      //                                        "Cannot find
      //                                        patchconstantfunc.");
      // Diags.Report(Attr->getLocation(), DiagID);
    }
  }

  if (const HLSLOutputControlPointsAttr *Attr =
          FD->getAttr<HLSLOutputControlPointsAttr>()) {
    if (isHS) {
      funcProps->ShaderProps.HS.outputControlPoints = Attr->getCount();
    } else if (isEntry && !SM->IsHS()) {
      unsigned DiagID = Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "attribute outputcontrolpoints only valid for HS.");
      Diags.Report(Attr->getLocation(), DiagID);
      return;
    }
  }

  if (const HLSLPartitioningAttr *Attr = FD->getAttr<HLSLPartitioningAttr>()) {
    if (isHS) {
      DXIL::TessellatorPartitioning partition =
          StringToPartitioning(Attr->getScheme());
      funcProps->ShaderProps.HS.partition = partition;
    } else if (isEntry && !SM->IsHS()) {
      unsigned DiagID =
          Diags.getCustomDiagID(DiagnosticsEngine::Warning,
                                "attribute partitioning only valid for HS.");
      Diags.Report(Attr->getLocation(), DiagID);
    }
  }

  if (const HLSLOutputTopologyAttr *Attr =
          FD->getAttr<HLSLOutputTopologyAttr>()) {
    if (isHS) {
      DXIL::TessellatorOutputPrimitive primitive =
          StringToTessOutputPrimitive(Attr->getTopology());
      funcProps->ShaderProps.HS.outputPrimitive = primitive;
    } else if (isEntry && !SM->IsHS()) {
      unsigned DiagID =
          Diags.getCustomDiagID(DiagnosticsEngine::Warning,
                                "attribute outputtopology only valid for HS.");
      Diags.Report(Attr->getLocation(), DiagID);
    }
  }

  if (isHS) {
    funcProps->ShaderProps.HS.maxTessFactor = DXIL::kHSMaxTessFactorUpperBound;
  }

  if (const HLSLMaxTessFactorAttr *Attr =
          FD->getAttr<HLSLMaxTessFactorAttr>()) {
    if (isHS) {
      // TODO: change getFactor to return float.
      llvm::APInt intV(32, Attr->getFactor());
      funcProps->ShaderProps.HS.maxTessFactor = intV.bitsToFloat();
    } else if (isEntry && !SM->IsHS()) {
      unsigned DiagID =
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "attribute maxtessfactor only valid for HS.");
      Diags.Report(Attr->getLocation(), DiagID);
      return;
    }
  }

  // Hull or domain shader.
  bool isDS = false;
  if (const HLSLDomainAttr *Attr = FD->getAttr<HLSLDomainAttr>()) {
    if (isEntry && !SM->IsHS() && !SM->IsDS()) {
      unsigned DiagID =
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "attribute domain only valid for HS or DS.");
      Diags.Report(Attr->getLocation(), DiagID);
      return;
    }

    isDS = !isHS;
    if (isDS)
      funcProps->shaderKind = DXIL::ShaderKind::Domain;

    DXIL::TessellatorDomain domain = StringToDomain(Attr->getDomainType());
    if (isHS)
      funcProps->ShaderProps.HS.domain = domain;
    else
      funcProps->ShaderProps.DS.domain = domain;
  }

  // Vertex shader.
  bool isVS = false;
  if (const HLSLClipPlanesAttr *Attr = FD->getAttr<HLSLClipPlanesAttr>()) {
    if (isEntry && !SM->IsVS()) {
      unsigned DiagID = Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "attribute clipplane only valid for VS.");
      Diags.Report(Attr->getLocation(), DiagID);
      return;
    }

    isVS = true;
    // The real job is done at EmitHLSLFunctionProlog where debug info is
    // available. Only set shader kind here.
    funcProps->shaderKind = DXIL::ShaderKind::Vertex;
  }

  // Pixel shader.
  bool isPS = false;
  if (const HLSLEarlyDepthStencilAttr *Attr =
          FD->getAttr<HLSLEarlyDepthStencilAttr>()) {
    if (isEntry && !SM->IsPS()) {
      unsigned DiagID = Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "attribute earlydepthstencil only valid for PS.");
      Diags.Report(Attr->getLocation(), DiagID);
      return;
    }

    isPS = true;
    funcProps->ShaderProps.PS.EarlyDepthStencil = true;
    funcProps->shaderKind = DXIL::ShaderKind::Pixel;
  }

  unsigned profileAttributes = 0;
  if (isCS)
    profileAttributes++;
  if (isHS)
    profileAttributes++;
  if (isDS)
    profileAttributes++;
  if (isGS)
    profileAttributes++;
  if (isVS)
    profileAttributes++;
  if (isPS)
    profileAttributes++;

  // TODO: check this in front-end and report error.
  DXASSERT(profileAttributes < 2, "profile attributes are mutual exclusive");

  if (isEntry) {
    switch (funcProps->shaderKind) {
    case ShaderModel::Kind::Compute:
    case ShaderModel::Kind::Hull:
    case ShaderModel::Kind::Domain:
    case ShaderModel::Kind::Geometry:
    case ShaderModel::Kind::Vertex:
    case ShaderModel::Kind::Pixel:
      DXASSERT(funcProps->shaderKind == SM->GetKind(),
               "attribute profile not match entry function profile");
      break;
    }
  }

  DxilFunctionAnnotation *FuncAnnotation =
      m_pHLModule->AddFunctionAnnotation(F);
  bool bDefaultRowMajor = m_pHLModule->GetHLOptions().bDefaultRowMajor;

  // Param Info
  unsigned streamIndex = 0;
  unsigned inputPatchCount = 0;
  unsigned outputPatchCount = 0;

  unsigned ArgNo = 0;
  unsigned ParmIdx = 0;

  if (const CXXMethodDecl *MethodDecl = dyn_cast<CXXMethodDecl>(FD)) {
    QualType ThisTy = MethodDecl->getThisType(FD->getASTContext());
    DxilParameterAnnotation &paramAnnotation =
        FuncAnnotation->GetParameterAnnotation(ArgNo++);
    // Construct annoation for this pointer.
    ConstructFieldAttributedAnnotation(paramAnnotation, ThisTy,
                                       bDefaultRowMajor);
  }

  // Ret Info
  QualType retTy = FD->getReturnType();
  DxilParameterAnnotation *pRetTyAnnotation = nullptr;
  if (F->getReturnType()->isVoidTy() && !retTy->isVoidType()) {
    // SRet.
    pRetTyAnnotation = &FuncAnnotation->GetParameterAnnotation(ArgNo++);
  } else {
    pRetTyAnnotation = &FuncAnnotation->GetRetTypeAnnotation();
  }
  DxilParameterAnnotation &retTyAnnotation = *pRetTyAnnotation;
  // keep Undefined here, we cannot decide for struct
  retTyAnnotation.SetInterpolationMode(
      GetInterpMode(FD, CompType::Kind::Invalid, /*bKeepUndefined*/ true)
          .GetKind());
  SourceLocation retTySemanticLoc = SetSemantic(FD, retTyAnnotation);
  retTyAnnotation.SetParamInputQual(DxilParamInputQual::Out);
  if (isEntry) {
    CheckParameterAnnotation(retTySemanticLoc, retTyAnnotation,
                             /*isPatchConstantFunction*/ false);
  }

  ConstructFieldAttributedAnnotation(retTyAnnotation, retTy, bDefaultRowMajor);
  if (FD->hasAttr<HLSLPreciseAttr>())
    retTyAnnotation.SetPrecise();

  for (; ArgNo < F->arg_size(); ++ArgNo, ++ParmIdx) {
    DxilParameterAnnotation &paramAnnotation =
        FuncAnnotation->GetParameterAnnotation(ArgNo);

    const ParmVarDecl *parmDecl = FD->getParamDecl(ParmIdx);

    ConstructFieldAttributedAnnotation(paramAnnotation, parmDecl->getType(),
                                       bDefaultRowMajor);
    if (parmDecl->hasAttr<HLSLPreciseAttr>())
      paramAnnotation.SetPrecise();

    // keep Undefined here, we cannot decide for struct
    InterpolationMode paramIM =
        GetInterpMode(parmDecl, CompType::Kind::Invalid, KeepUndefinedTrue);
    paramAnnotation.SetInterpolationMode(paramIM);
    SourceLocation paramSemanticLoc = SetSemantic(parmDecl, paramAnnotation);

    DxilParamInputQual dxilInputQ = DxilParamInputQual::In;

    if (parmDecl->hasAttr<HLSLInOutAttr>())
      dxilInputQ = DxilParamInputQual::Inout;
    else if (parmDecl->hasAttr<HLSLOutAttr>())
      dxilInputQ = DxilParamInputQual::Out;

    DXIL::InputPrimitive inputPrimitive = DXIL::InputPrimitive::Undefined;

    if (IsHLSLOutputPatchType(parmDecl->getType())) {
      outputPatchCount++;
      if (dxilInputQ != DxilParamInputQual::In) {
        unsigned DiagID = Diags.getCustomDiagID(
            DiagnosticsEngine::Error,
            "OutputPatch should not be out/inout parameter");
        Diags.Report(parmDecl->getLocation(), DiagID);
        continue;
      }
      dxilInputQ = DxilParamInputQual::OutputPatch;
      if (isDS)
        funcProps->ShaderProps.DS.inputControlPoints =
            GetHLSLOutputPatchCount(parmDecl->getType());
    } else if (IsHLSLInputPatchType(parmDecl->getType())) {
      inputPatchCount++;
      if (dxilInputQ != DxilParamInputQual::In) {
        unsigned DiagID = Diags.getCustomDiagID(
            DiagnosticsEngine::Error,
            "InputPatch should not be out/inout parameter");
        Diags.Report(parmDecl->getLocation(), DiagID);
        continue;
      }
      dxilInputQ = DxilParamInputQual::InputPatch;
      if (isHS) {
        funcProps->ShaderProps.HS.inputControlPoints =
            GetHLSLInputPatchCount(parmDecl->getType());
      } else if (isGS) {
        inputPrimitive = (DXIL::InputPrimitive)(
            (unsigned)DXIL::InputPrimitive::ControlPointPatch1 +
            GetHLSLInputPatchCount(parmDecl->getType()) - 1);
      }
    } else if (IsHLSLStreamOutputType(parmDecl->getType())) {
      // TODO: validation this at ASTContext::getFunctionType in
      // AST/ASTContext.cpp
      DXASSERT(dxilInputQ == DxilParamInputQual::Inout,
               "stream output parameter must be inout");
      switch (streamIndex) {
      case 0:
        dxilInputQ = DxilParamInputQual::OutStream0;
        break;
      case 1:
        dxilInputQ = DxilParamInputQual::OutStream1;
        break;
      case 2:
        dxilInputQ = DxilParamInputQual::OutStream2;
        break;
      case 3:
      default:
        // TODO: validation this at ASTContext::getFunctionType in
        // AST/ASTContext.cpp
        DXASSERT(streamIndex == 3, "stream number out of bound");
        dxilInputQ = DxilParamInputQual::OutStream3;
        break;
      }
      DXIL::PrimitiveTopology &streamTopology =
          funcProps->ShaderProps.GS.streamPrimitiveTopologies[streamIndex];
      if (IsHLSLPointStreamType(parmDecl->getType()))
        streamTopology = DXIL::PrimitiveTopology::PointList;
      else if (IsHLSLLineStreamType(parmDecl->getType()))
        streamTopology = DXIL::PrimitiveTopology::LineStrip;
      else {
        DXASSERT(IsHLSLTriangleStreamType(parmDecl->getType()),
                 "invalid StreamType");
        streamTopology = DXIL::PrimitiveTopology::TriangleStrip;
      }

      if (streamIndex > 0) {
        bool bAllPoint =
            streamTopology == DXIL::PrimitiveTopology::PointList &&
            funcProps->ShaderProps.GS.streamPrimitiveTopologies[0] ==
                DXIL::PrimitiveTopology::PointList;
        if (!bAllPoint) {
          DiagnosticsEngine &Diags = CGM.getDiags();
          unsigned DiagID = Diags.getCustomDiagID(
              DiagnosticsEngine::Error, "when multiple GS output streams are "
                                        "used they must be pointlists.");
          Diags.Report(FD->getLocation(), DiagID);
        }
      }

      streamIndex++;
    }

    unsigned GsInputArrayDim = 0;
    if (parmDecl->hasAttr<HLSLTriangleAttr>()) {
      inputPrimitive = DXIL::InputPrimitive::Triangle;
      GsInputArrayDim = 3;
    } else if (parmDecl->hasAttr<HLSLTriangleAdjAttr>()) {
      inputPrimitive = DXIL::InputPrimitive::TriangleWithAdjacency;
      GsInputArrayDim = 6;
    } else if (parmDecl->hasAttr<HLSLPointAttr>()) {
      inputPrimitive = DXIL::InputPrimitive::Point;
      GsInputArrayDim = 1;
    } else if (parmDecl->hasAttr<HLSLLineAdjAttr>()) {
      inputPrimitive = DXIL::InputPrimitive::LineWithAdjacency;
      GsInputArrayDim = 4;
    } else if (parmDecl->hasAttr<HLSLLineAttr>()) {
      inputPrimitive = DXIL::InputPrimitive::Line;
      GsInputArrayDim = 2;
    }

    if (inputPrimitive != DXIL::InputPrimitive::Undefined) {
      // Set to InputPrimitive for GS.
      dxilInputQ = DxilParamInputQual::InputPrimitive;
      if (funcProps->ShaderProps.GS.inputPrimitive ==
          DXIL::InputPrimitive::Undefined) {
        funcProps->ShaderProps.GS.inputPrimitive = inputPrimitive;
      } else if (funcProps->ShaderProps.GS.inputPrimitive != inputPrimitive) {
        DiagnosticsEngine &Diags = CGM.getDiags();
        unsigned DiagID = Diags.getCustomDiagID(
            DiagnosticsEngine::Error, "input parameter conflicts with geometry "
                                      "specifier of previous input parameters");
        Diags.Report(parmDecl->getLocation(), DiagID);
      }
    }

    if (GsInputArrayDim != 0) {
      QualType Ty = parmDecl->getType();
      if (!Ty->isConstantArrayType()) {
        DiagnosticsEngine &Diags = CGM.getDiags();
        unsigned DiagID = Diags.getCustomDiagID(
            DiagnosticsEngine::Error,
            "input types for geometry shader must be constant size arrays");
        Diags.Report(parmDecl->getLocation(), DiagID);
      } else {
        const ConstantArrayType *CAT = cast<ConstantArrayType>(Ty);
        if (CAT->getSize().getLimitedValue() != GsInputArrayDim) {
          StringRef primtiveNames[] = {
              "invalid",     // 0
              "point",       // 1
              "line",        // 2
              "triangle",    // 3
              "lineadj",     // 4
              "invalid",     // 5
              "triangleadj", // 6
          };
          DXASSERT(GsInputArrayDim < llvm::array_lengthof(primtiveNames),
                   "Invalid array dim");
          DiagnosticsEngine &Diags = CGM.getDiags();
          unsigned DiagID = Diags.getCustomDiagID(
              DiagnosticsEngine::Error, "array dimension for %0 must be %1");
          Diags.Report(parmDecl->getLocation(), DiagID)
              << primtiveNames[GsInputArrayDim] << GsInputArrayDim;
        }
      }
    }

    paramAnnotation.SetParamInputQual(dxilInputQ);
    if (isEntry) {
      CheckParameterAnnotation(paramSemanticLoc, paramAnnotation,
                               /*isPatchConstantFunction*/ false);
    }
  }

  if (inputPatchCount > 1) {
    DiagnosticsEngine &Diags = CGM.getDiags();
    unsigned DiagID = Diags.getCustomDiagID(
        DiagnosticsEngine::Error, "may only have one InputPatch parameter");
    Diags.Report(FD->getLocation(), DiagID);
  }
  if (outputPatchCount > 1) {
    DiagnosticsEngine &Diags = CGM.getDiags();
    unsigned DiagID = Diags.getCustomDiagID(
        DiagnosticsEngine::Error, "may only have one OutputPatch parameter");
    Diags.Report(FD->getLocation(), DiagID);
  }

  // Type annotation for parameters and return type.
  DxilTypeSystem &dxilTypeSys = m_pHLModule->GetTypeSystem();
  unsigned arrayEltSize = 0;
  AddTypeAnnotation(FD->getReturnType(), dxilTypeSys, arrayEltSize);

  // Type annotation for this pointer.
  if (const CXXMethodDecl *MFD = dyn_cast<CXXMethodDecl>(FD)) {
    const CXXRecordDecl *RD = MFD->getParent();
    QualType Ty = CGM.getContext().getTypeDeclType(RD);
    AddTypeAnnotation(Ty, dxilTypeSys, arrayEltSize);
  }

  for (const ValueDecl *param : FD->params()) {
    QualType Ty = param->getType();
    AddTypeAnnotation(Ty, dxilTypeSys, arrayEltSize);
  }

  if (isHS) {
    // Check
    Function *patchConstFunc = funcProps->ShaderProps.HS.patchConstantFunc;
    if (m_pHLModule->HasHLFunctionProps(patchConstFunc)) {
      HLFunctionProps &patchProps =
          m_pHLModule->GetHLFunctionProps(patchConstFunc);
      if (patchProps.ShaderProps.HS.outputControlPoints != 0 &&
          patchProps.ShaderProps.HS.outputControlPoints !=
              funcProps->ShaderProps.HS.outputControlPoints) {
        unsigned DiagID = Diags.getCustomDiagID(
            DiagnosticsEngine::Error,
            "Patch constant function's output patch input "
            "should have %0 elements, but has %1.");
        Diags.Report(FD->getLocation(), DiagID)
            << funcProps->ShaderProps.HS.outputControlPoints
            << patchProps.ShaderProps.HS.outputControlPoints;
      }
      if (patchProps.ShaderProps.HS.inputControlPoints != 0 &&
          patchProps.ShaderProps.HS.inputControlPoints !=
              funcProps->ShaderProps.HS.inputControlPoints) {
        unsigned DiagID =
            Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                  "Patch constant function's input patch input "
                                  "should have %0 elements, but has %1.");
        Diags.Report(FD->getLocation(), DiagID)
            << funcProps->ShaderProps.HS.inputControlPoints
            << patchProps.ShaderProps.HS.inputControlPoints;
      }
    }
  }

  // Only add functionProps when exist.
  if (profileAttributes || isPatchConstantFunction)
    m_pHLModule->AddHLFunctionProps(F, funcProps);
}

void CGMSHLSLRuntime::EmitHLSLFunctionProlog(Function *F, const FunctionDecl *FD) {
  // Support clip plane need debug info which not available when create function attribute.
  if (const HLSLClipPlanesAttr *Attr = FD->getAttr<HLSLClipPlanesAttr>()) {
    HLFunctionProps &funcProps = m_pHLModule->GetHLFunctionProps(F);
    // Initialize to null.
    memset(funcProps.ShaderProps.VS.clipPlanes, 0, sizeof(funcProps.ShaderProps.VS.clipPlanes));
    // Create global for each clip plane, and use the clip plane val as init val.
    auto AddClipPlane = [&](Expr *clipPlane, unsigned idx) {
      if (DeclRefExpr *decl = dyn_cast<DeclRefExpr>(clipPlane)) {
        const VarDecl *VD = cast<VarDecl>(decl->getDecl());
        Constant *clipPlaneVal = CGM.GetAddrOfGlobalVar(VD);
        funcProps.ShaderProps.VS.clipPlanes[idx] = clipPlaneVal;
        if (m_bDebugInfo) {
          CodeGenFunction CGF(CGM);
          ApplyDebugLocation applyDebugLoc(CGF, clipPlane);
          debugInfoMap[clipPlaneVal] = CGF.Builder.getCurrentDebugLocation();
        }
      } else {
        // Must be a MemberExpr.
        const MemberExpr *ME = cast<MemberExpr>(clipPlane);
        CodeGenFunction CGF(CGM);
        CodeGen::LValue LV = CGF.EmitMemberExpr(ME);
        Value *addr = LV.getAddress();
        funcProps.ShaderProps.VS.clipPlanes[idx] = cast<Constant>(addr);
        if (m_bDebugInfo) {
          CodeGenFunction CGF(CGM);
          ApplyDebugLocation applyDebugLoc(CGF, clipPlane);
          debugInfoMap[addr] = CGF.Builder.getCurrentDebugLocation();
        }
      }
    };

    if (Expr *clipPlane = Attr->getClipPlane1())
      AddClipPlane(clipPlane, 0);
    if (Expr *clipPlane = Attr->getClipPlane2())
      AddClipPlane(clipPlane, 1);
    if (Expr *clipPlane = Attr->getClipPlane3())
      AddClipPlane(clipPlane, 2);
    if (Expr *clipPlane = Attr->getClipPlane4())
      AddClipPlane(clipPlane, 3);
    if (Expr *clipPlane = Attr->getClipPlane5())
      AddClipPlane(clipPlane, 4);
    if (Expr *clipPlane = Attr->getClipPlane6())
      AddClipPlane(clipPlane, 5);

    clipPlaneFuncList.emplace_back(F);
  }
}

void CGMSHLSLRuntime::AddControlFlowHint(CodeGenFunction &CGF, const Stmt &S,
                                         llvm::TerminatorInst *TI,
                                         ArrayRef<const Attr *> Attrs) {
  // Build hints.
  bool bNoBranchFlatten = true;
  bool bBranch = false;
  bool bFlatten = false;

  std::vector<DXIL::ControlFlowHint> hints;
  for (const auto *Attr : Attrs) {
    if (isa<HLSLBranchAttr>(Attr)) {
      hints.emplace_back(DXIL::ControlFlowHint::Branch);
      bNoBranchFlatten = false;
      bBranch = true;
    }
    else if (isa<HLSLFlattenAttr>(Attr)) {
      hints.emplace_back(DXIL::ControlFlowHint::Flatten);
      bNoBranchFlatten = false;
      bFlatten = true;
    } else if (isa<HLSLForceCaseAttr>(Attr)) {
      if (isa<SwitchStmt>(&S)) {
        hints.emplace_back(DXIL::ControlFlowHint::ForceCase);
      }
    }
    // Ignore fastopt, allow_uav_condition and call for now.
  }

  if (bNoBranchFlatten) {
    // CHECK control flow option.
    if (CGF.CGM.getCodeGenOpts().HLSLPreferControlFlow)
      hints.emplace_back(DXIL::ControlFlowHint::Branch);
    else if (CGF.CGM.getCodeGenOpts().HLSLAvoidControlFlow)
      hints.emplace_back(DXIL::ControlFlowHint::Flatten);
  }

  if (bFlatten && bBranch) {
    DiagnosticsEngine &Diags = CGM.getDiags();
    unsigned DiagID = Diags.getCustomDiagID(
        DiagnosticsEngine::Error,
        "can't use branch and flatten attributes together");
    Diags.Report(S.getLocStart(), DiagID);
  }

  if (hints.size()) {      
    // Add meta data to the instruction.
    MDNode *hintsNode = DxilMDHelper::EmitControlFlowHints(Context, hints);
    TI->setMetadata(DxilMDHelper::kDxilControlFlowHintMDName, hintsNode);
  }
}

void CGMSHLSLRuntime::FinishAutoVar(CodeGenFunction &CGF, const VarDecl &D, llvm::Value *V) {
  if (D.hasAttr<HLSLPreciseAttr>()) {
    AllocaInst *AI = cast<AllocaInst>(V);
    HLModule::MarkPreciseAttributeWithMetadata(AI);
  }
  // Add type annotation for local variable.
  DxilTypeSystem &typeSys = m_pHLModule->GetTypeSystem();
  unsigned arrayEltSize = 0;
  AddTypeAnnotation(D.getType(), typeSys, arrayEltSize);
}

hlsl::InterpolationMode CGMSHLSLRuntime::GetInterpMode(const Decl *decl,
                                                       CompType compType,
                                                       bool bKeepUndefined) {
  InterpolationMode Interp(
      decl->hasAttr<HLSLNoInterpolationAttr>(), decl->hasAttr<HLSLLinearAttr>(),
      decl->hasAttr<HLSLNoPerspectiveAttr>(), decl->hasAttr<HLSLCentroidAttr>(),
      decl->hasAttr<HLSLSampleAttr>());
  DXASSERT(Interp.IsValid(), "otherwise front-end missing validation");
  if (Interp.IsUndefined() && !bKeepUndefined) {
    // Type-based default: linear for floats, constant for others.
    if (compType.IsFloatTy())
      Interp = InterpolationMode::Kind::Linear;
    else
      Interp = InterpolationMode::Kind::Constant;
  }
  return Interp;
}

hlsl::CompType CGMSHLSLRuntime::GetCompType(const BuiltinType *BT) {

  hlsl::CompType ElementType = hlsl::CompType::getInvalid();
  switch (BT->getKind()) {
  case BuiltinType::Bool:
    ElementType = hlsl::CompType::getI1();
    break;
  case BuiltinType::Double:
    ElementType = hlsl::CompType::getF64();
    break;
  case BuiltinType::Float:
    ElementType = hlsl::CompType::getF32();
    break;
  case BuiltinType::Min10Float:
  case BuiltinType::Half:
    ElementType = hlsl::CompType::getF16();
    break;
  case BuiltinType::Int:
    ElementType = hlsl::CompType::getI32();
    break;
  case BuiltinType::LongLong:
    ElementType = hlsl::CompType::getI64();
    break;
  case BuiltinType::Min12Int:
  case BuiltinType::Short:
    ElementType = hlsl::CompType::getI16();
    break;
  case BuiltinType::UInt:
    ElementType = hlsl::CompType::getU32();
    break;
  case BuiltinType::ULongLong:
    ElementType = hlsl::CompType::getU64();
    break;
  case BuiltinType::UShort:
    ElementType = hlsl::CompType::getU16();
    break;
  default:
    llvm_unreachable("unsupported type");
    break;
  }

  return ElementType;
}

/// Add resouce to the program
void CGMSHLSLRuntime::addResource(Decl *D) {
  if (HLSLBufferDecl *BD = dyn_cast<HLSLBufferDecl>(D))
    GetOrCreateCBuffer(BD);
  else if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
    hlsl::DxilResourceBase::Class resClass = TypeToClass(VD->getType());
    // skip decl has init which is resource.
    if (VD->hasInit() && resClass != DXIL::ResourceClass::Invalid)
      return;
    // skip static global.
    if (!VD->isExternallyVisible())
      return;

    if (D->hasAttr<HLSLGroupSharedAttr>()) {
      GlobalVariable *GV = cast<GlobalVariable>(CGM.GetAddrOfGlobalVar(VD));
      m_pHLModule->AddGroupSharedVariable(GV);
      return;
    }

    switch (resClass) {
    case hlsl::DxilResourceBase::Class::Sampler:
      AddSampler(VD);
      break;
    case hlsl::DxilResourceBase::Class::UAV:
    case hlsl::DxilResourceBase::Class::SRV:
      AddUAVSRV(VD, resClass);
      break;
    case hlsl::DxilResourceBase::Class::Invalid: {
      // normal global constant, add to global CB
      HLCBuffer &globalCB = GetGlobalCBuffer();
      AddConstant(VD, globalCB);
      break;
    }
    case DXIL::ResourceClass::CBuffer:
      DXASSERT(0, "cbuffer should not be here");
      break;
    }
  }
}

// TODO: collect such helper utility functions in one place.
static DxilResourceBase::Class KeywordToClass(const std::string &keyword) {
  // TODO: refactor for faster search (switch by 1/2/3 first letters, then
  // compare)
  if (keyword == "SamplerState")
    return DxilResourceBase::Class::Sampler;
  
  if (keyword == "SamplerComparisonState")
    return DxilResourceBase::Class::Sampler;

  if (keyword == "ConstantBuffer")
    return DxilResourceBase::Class::CBuffer;

  if (keyword == "TextureBuffer")
    return DxilResourceBase::Class::SRV;

  bool isSRV = keyword == "Buffer";
  isSRV |= keyword == "ByteAddressBuffer";
  isSRV |= keyword == "StructuredBuffer";
  isSRV |= keyword == "Texture1D";
  isSRV |= keyword == "Texture1DArray";
  isSRV |= keyword == "Texture2D";
  isSRV |= keyword == "Texture2DArray";
  isSRV |= keyword == "Texture3D";
  isSRV |= keyword == "TextureCube";
  isSRV |= keyword == "TextureCubeArray";
  isSRV |= keyword == "Texture2DMS";
  isSRV |= keyword == "Texture2DMSArray";
  if (isSRV)
    return DxilResourceBase::Class::SRV;

  bool isUAV = keyword == "RWBuffer";
  isUAV |= keyword == "RWByteAddressBuffer";
  isUAV |= keyword == "RWStructuredBuffer";
  isUAV |= keyword == "RWTexture1D";
  isUAV |= keyword == "RWTexture1DArray";
  isUAV |= keyword == "RWTexture2D";
  isUAV |= keyword == "RWTexture2DArray";
  isUAV |= keyword == "RWTexture3D";
  isUAV |= keyword == "RWTextureCube";
  isUAV |= keyword == "RWTextureCubeArray";
  isUAV |= keyword == "RWTexture2DMS";
  isUAV |= keyword == "RWTexture2DMSArray";
  isUAV |= keyword == "AppendStructuredBuffer";
  isUAV |= keyword == "ConsumeStructuredBuffer";
  isUAV |= keyword == "RasterizerOrderedBuffer";
  isUAV |= keyword == "RasterizerOrderedByteAddressBuffer";
  isUAV |= keyword == "RasterizerOrderedStructuredBuffer";
  isUAV |= keyword == "RasterizerOrderedTexture1D";
  isUAV |= keyword == "RasterizerOrderedTexture1DArray";
  isUAV |= keyword == "RasterizerOrderedTexture2D";
  isUAV |= keyword == "RasterizerOrderedTexture2DArray";
  isUAV |= keyword == "RasterizerOrderedTexture3D";
  if (isUAV)
    return DxilResourceBase::Class::UAV;

  return DxilResourceBase::Class::Invalid;
}

// This should probably be refactored to ASTContextHLSL, and follow types
// rather than do string comparisons.
DXIL::ResourceClass
hlsl::GetResourceClassForType(const clang::ASTContext &context,
                              clang::QualType Ty) {
  Ty = Ty.getCanonicalType();
  if (const clang::ArrayType *arrayType = context.getAsArrayType(Ty)) {
    return GetResourceClassForType(context, arrayType->getElementType());
  } else if (const RecordType *RT = Ty->getAsStructureType()) {
    return KeywordToClass(RT->getDecl()->getName());
  } else if (const RecordType *RT = Ty->getAs<RecordType>()) {
    if (const ClassTemplateSpecializationDecl *templateDecl =
            dyn_cast<ClassTemplateSpecializationDecl>(RT->getDecl())) {
      return KeywordToClass(templateDecl->getName());
    }
  }

  return hlsl::DxilResourceBase::Class::Invalid;
}

hlsl::DxilResourceBase::Class CGMSHLSLRuntime::TypeToClass(clang::QualType Ty) {
  return hlsl::GetResourceClassForType(CGM.getContext(), Ty);
}

uint32_t CGMSHLSLRuntime::AddSampler(VarDecl *samplerDecl) {
  llvm::Constant *val = CGM.GetAddrOfGlobalVar(samplerDecl);

  unique_ptr<DxilSampler> hlslRes(new DxilSampler);
  hlslRes->SetLowerBound(UINT_MAX);
  hlslRes->SetGlobalSymbol(cast<llvm::GlobalVariable>(val));
  hlslRes->SetGlobalName(samplerDecl->getName());
  QualType VarTy = samplerDecl->getType();
  if (const clang::ArrayType *arrayType =
          CGM.getContext().getAsArrayType(VarTy)) {
    if (arrayType->isConstantArrayType()) {
      uint32_t arraySize =
          cast<ConstantArrayType>(arrayType)->getSize().getLimitedValue();
      hlslRes->SetRangeSize(arraySize);
    } else {
      hlslRes->SetRangeSize(UINT_MAX);
    }
    // use elementTy
    VarTy = arrayType->getElementType();
    // Support more dim.
    while (const clang::ArrayType *arrayType =
               CGM.getContext().getAsArrayType(VarTy)) {
      unsigned rangeSize = hlslRes->GetRangeSize();
      if (arrayType->isConstantArrayType()) {
        uint32_t arraySize =
            cast<ConstantArrayType>(arrayType)->getSize().getLimitedValue();
        if (rangeSize != UINT_MAX)
          hlslRes->SetRangeSize(rangeSize * arraySize);
      } else
        hlslRes->SetRangeSize(UINT_MAX);
      // use elementTy
      VarTy = arrayType->getElementType();
    }
  } else
    hlslRes->SetRangeSize(1);

  const RecordType *RT = VarTy->getAs<RecordType>();
  DxilSampler::SamplerKind kind = KeywordToSamplerKind(RT->getDecl()->getName());

  hlslRes->SetSamplerKind(kind);

  for (hlsl::UnusualAnnotation *it : samplerDecl->getUnusualAnnotations()) {
    switch (it->getKind()) {
    case hlsl::UnusualAnnotation::UA_RegisterAssignment: {
      hlsl::RegisterAssignment *ra = cast<hlsl::RegisterAssignment>(it);
      hlslRes->SetLowerBound(ra->RegisterNumber);
      hlslRes->SetSpaceID(ra->RegisterSpace);
      break;
    }
    default:
      llvm_unreachable("only register for sampler");
      break;
    }
  }

  hlslRes->SetID(m_pHLModule->GetSamplers().size());
  return m_pHLModule->AddSampler(std::move(hlslRes));
}

static void CollectScalarTypes(std::vector<llvm::Type *> &scalarTys, llvm::Type *Ty) {
  if (llvm::StructType *ST = dyn_cast<llvm::StructType>(Ty)) {
    for (llvm::Type *EltTy : ST->elements()) {
      CollectScalarTypes(scalarTys, EltTy);
    }
  } else if (llvm::ArrayType *AT = dyn_cast<llvm::ArrayType>(Ty)) {
    llvm::Type *EltTy = AT->getElementType();
    for (unsigned i=0;i<AT->getNumElements();i++) {
      CollectScalarTypes(scalarTys, EltTy);
    }
  } else if (llvm::VectorType *VT = dyn_cast<llvm::VectorType>(Ty)) {
    llvm::Type *EltTy = VT->getElementType();
    for (unsigned i=0;i<VT->getNumElements();i++) {
      CollectScalarTypes(scalarTys, EltTy);
    }
  } else {
    scalarTys.emplace_back(Ty);
  }
}


static void CollectScalarTypes(std::vector<QualType> &ScalarTys, QualType Ty) {
  if (Ty->isRecordType()) {
    if (hlsl::IsHLSLMatType(Ty)) {
      QualType EltTy = hlsl::GetHLSLMatElementType(Ty);
      unsigned row = 0;
      unsigned col = 0;
      hlsl::GetRowsAndCols(Ty, row, col);
      unsigned size = col*row;
      for (unsigned i = 0; i < size; i++) {
        CollectScalarTypes(ScalarTys, EltTy);
      }
    } else if (hlsl::IsHLSLVecType(Ty)) {
      QualType EltTy = hlsl::GetHLSLVecElementType(Ty);
      unsigned row = 0;
      unsigned col = 0;
      hlsl::GetRowsAndColsForAny(Ty, row, col);
      unsigned size = col;
      for (unsigned i = 0; i < size; i++) {
        CollectScalarTypes(ScalarTys, EltTy);
      }
    } else {
      const RecordType *RT = Ty->getAsStructureType();
      // For CXXRecord.
      if (!RT)
        RT = Ty->getAs<RecordType>();
      RecordDecl *RD = RT->getDecl();
      for (FieldDecl *field : RD->fields())
        CollectScalarTypes(ScalarTys, field->getType());
    }
  } else if (Ty->isArrayType()) {
    const clang::ArrayType *AT = Ty->getAsArrayTypeUnsafe();
    QualType EltTy = AT->getElementType();
    // Set it to 5 for unsized array.
    unsigned size = 5;
    if (AT->isConstantArrayType()) {
      size = cast<ConstantArrayType>(AT)->getSize().getLimitedValue();
    }
    for (unsigned i=0;i<size;i++) {
      CollectScalarTypes(ScalarTys, EltTy);
    }
  } else {
    ScalarTys.emplace_back(Ty);
  }
}

bool CGMSHLSLRuntime::SetUAVSRV(SourceLocation loc,
                                hlsl::DxilResourceBase::Class resClass,
                                DxilResource *hlslRes, const RecordDecl *RD) {
  hlsl::DxilResource::Kind kind = KeywordToKind(RD->getName());
  hlslRes->SetKind(kind);

  // Get the result type from handle field.
  FieldDecl *FD = *(RD->field_begin());
  DXASSERT(FD->getName() == "h", "must be handle field");
  QualType resultTy = FD->getType();
  // Type annotation for result type of resource.
  DxilTypeSystem &dxilTypeSys = m_pHLModule->GetTypeSystem();
  unsigned arrayEltSize = 0;
  AddTypeAnnotation(QualType(RD->getTypeForDecl(),0), dxilTypeSys, arrayEltSize);

  if (kind == hlsl::DxilResource::Kind::Texture2DMS ||
      kind == hlsl::DxilResource::Kind::Texture2DMSArray) {
    const ClassTemplateSpecializationDecl *templateDecl =
        dyn_cast<ClassTemplateSpecializationDecl>(RD);
    const clang::TemplateArgument &sampleCountArg =
        templateDecl->getTemplateArgs()[1];
    uint32_t sampleCount = sampleCountArg.getAsIntegral().getLimitedValue();
    hlslRes->SetSampleCount(sampleCount);
  }

  if (kind != hlsl::DxilResource::Kind::StructuredBuffer) {
    QualType Ty = resultTy;
    QualType EltTy = Ty;
    if (hlsl::IsHLSLVecType(Ty)) {
      EltTy = hlsl::GetHLSLVecElementType(Ty);
    } else if (hlsl::IsHLSLMatType(Ty)) {
      EltTy = hlsl::GetHLSLMatElementType(Ty);
    } else if (resultTy->isAggregateType()) {
      // Struct or array in a none-struct resource.
      std::vector<QualType> ScalarTys;
      CollectScalarTypes(ScalarTys, resultTy);
      unsigned size = ScalarTys.size();
      if (size == 0) {
        DiagnosticsEngine &Diags = CGM.getDiags();
        unsigned DiagID = Diags.getCustomDiagID(
            DiagnosticsEngine::Error,
            "object's templated type must have at least one element");
        Diags.Report(loc, DiagID);
        return false;
      }
      if (size > 4) {
        DiagnosticsEngine &Diags = CGM.getDiags();
        unsigned DiagID = Diags.getCustomDiagID(
            DiagnosticsEngine::Error, "elements of typed buffers and textures "
                                      "must fit in four 32-bit quantities");
        Diags.Report(loc, DiagID);
        return false;
      }

      EltTy = ScalarTys[0];
      for (QualType ScalarTy : ScalarTys) {
        if (ScalarTy != EltTy) {
          DiagnosticsEngine &Diags = CGM.getDiags();
          unsigned DiagID = Diags.getCustomDiagID(
              DiagnosticsEngine::Error,
              "all template type components must have the same type");
          Diags.Report(loc, DiagID);
          return false;
        }
      }
    }

    EltTy = EltTy.getCanonicalType();
    bool bSNorm = false;
    bool bUNorm = false;

    if (const AttributedType *AT = dyn_cast<AttributedType>(Ty)) {
      switch (AT->getAttrKind()) {
      case AttributedType::Kind::attr_hlsl_snorm:
        bSNorm = true;
        break;
      case AttributedType::Kind::attr_hlsl_unorm:
        bUNorm = true;
        break;
      default:
        // Do nothing
        break;
      }
    }

    if (EltTy->isBuiltinType()) {
      const BuiltinType *BTy = EltTy->getAs<BuiltinType>();
      CompType::Kind kind = BuiltinTyToCompTy(BTy, bSNorm, bUNorm);
      // 64bits types are implemented with u32.
      if (kind == CompType::Kind::U64 || kind == CompType::Kind::I64 ||
          kind == CompType::Kind::SNormF64 ||
          kind == CompType::Kind::UNormF64 || kind == CompType::Kind::F64) {
        kind = CompType::Kind::U32;
      }
      hlslRes->SetCompType(kind);
    } else {
      DXASSERT(!bSNorm && !bUNorm, "snorm/unorm on invalid type");
    }
  }

  hlslRes->SetROV(RD->getName().startswith("RasterizerOrdered"));

  if (kind == hlsl::DxilResource::Kind::TypedBuffer ||
      kind == hlsl::DxilResource::Kind::StructuredBuffer) {
    const ClassTemplateSpecializationDecl *templateDecl =
        dyn_cast<ClassTemplateSpecializationDecl>(RD);

    const clang::TemplateArgument &retTyArg =
        templateDecl->getTemplateArgs()[0];
    llvm::Type *retTy = CGM.getTypes().ConvertType(retTyArg.getAsType());

    uint32_t strideInBytes = legacyLayout.getTypeAllocSize(retTy);
    hlslRes->SetElementStride(strideInBytes);
  }

  if (resClass == hlsl::DxilResourceBase::Class::SRV) {
    if (hlslRes->IsGloballyCoherent()) {
      DiagnosticsEngine &Diags = CGM.getDiags();
      unsigned DiagID = Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "globallycoherent can only be used with "
                                    "Unordered Access View buffers.");
      Diags.Report(loc, DiagID);
      return false;
    }

    hlslRes->SetRW(false);
    hlslRes->SetID(m_pHLModule->GetSRVs().size());
  } else {
    hlslRes->SetRW(true);
    hlslRes->SetID(m_pHLModule->GetUAVs().size());
  }
  return true;
}

uint32_t CGMSHLSLRuntime::AddUAVSRV(VarDecl *decl,
                                    hlsl::DxilResourceBase::Class resClass) {
  llvm::GlobalVariable *val =
      cast<llvm::GlobalVariable>(CGM.GetAddrOfGlobalVar(decl));

  QualType VarTy = decl->getType().getCanonicalType();

  unique_ptr<HLResource> hlslRes(new HLResource);
  hlslRes->SetLowerBound(UINT_MAX);
  hlslRes->SetGlobalSymbol(val);
  hlslRes->SetGlobalName(decl->getName());
  if (const clang::ArrayType *arrayType =
          CGM.getContext().getAsArrayType(VarTy)) {
    if (arrayType->isConstantArrayType()) {
      uint32_t arraySize =
          cast<ConstantArrayType>(arrayType)->getSize().getLimitedValue();
      hlslRes->SetRangeSize(arraySize);
    } else
      hlslRes->SetRangeSize(UINT_MAX);
    // use elementTy
    VarTy = arrayType->getElementType();
    // Support more dim.
    while (const clang::ArrayType *arrayType =
               CGM.getContext().getAsArrayType(VarTy)) {
      unsigned rangeSize = hlslRes->GetRangeSize();
      if (arrayType->isConstantArrayType()) {
        uint32_t arraySize =
            cast<ConstantArrayType>(arrayType)->getSize().getLimitedValue();
        if (rangeSize != UINT_MAX)
          hlslRes->SetRangeSize(rangeSize * arraySize);
      } else
        hlslRes->SetRangeSize(UINT_MAX);
      // use elementTy
      VarTy = arrayType->getElementType();
    }
  } else
    hlslRes->SetRangeSize(1);

  for (hlsl::UnusualAnnotation *it : decl->getUnusualAnnotations()) {
    switch (it->getKind()) {
    case hlsl::UnusualAnnotation::UA_RegisterAssignment: {
      hlsl::RegisterAssignment *ra = cast<hlsl::RegisterAssignment>(it);
      hlslRes->SetLowerBound(ra->RegisterNumber);
      hlslRes->SetSpaceID(ra->RegisterSpace);
      break;
    }
    default:
      llvm_unreachable("only register for uav/srv");
      break;
    }
  }

  const RecordType *RT = VarTy->getAs<RecordType>();
  RecordDecl *RD = RT->getDecl();

  if (decl->hasAttr<HLSLGloballyCoherentAttr>()) {
    hlslRes->SetGloballyCoherent(true);
  }

  if (!SetUAVSRV(decl->getLocation(), resClass, hlslRes.get(), RD))
    return 0;

  if (resClass == hlsl::DxilResourceBase::Class::SRV) {
    return m_pHLModule->AddSRV(std::move(hlslRes));
  } else {
    return m_pHLModule->AddUAV(std::move(hlslRes));
  }
}

static bool IsResourceInType(const clang::ASTContext &context,
                              clang::QualType Ty) {
  Ty = Ty.getCanonicalType();
  if (const clang::ArrayType *arrayType = context.getAsArrayType(Ty)) {
    return IsResourceInType(context, arrayType->getElementType());
  } else if (const RecordType *RT = Ty->getAsStructureType()) {
    if (KeywordToClass(RT->getDecl()->getName()) != DxilResourceBase::Class::Invalid)
      return true;
    const CXXRecordDecl* typeRecordDecl = RT->getAsCXXRecordDecl();
    if (typeRecordDecl && !typeRecordDecl->isImplicit()) {
      for (auto field : typeRecordDecl->fields()) {
        if (IsResourceInType(context, field->getType()))
          return true;
      }
    }
  } else if (const RecordType *RT = Ty->getAs<RecordType>()) {
    if (const ClassTemplateSpecializationDecl *templateDecl =
            dyn_cast<ClassTemplateSpecializationDecl>(RT->getDecl())) {
      if (KeywordToClass(templateDecl->getName()) != DxilResourceBase::Class::Invalid)
        return true;
    }
  }

  return false; // no resources found
}

void CGMSHLSLRuntime::AddConstant(VarDecl *constDecl, HLCBuffer &CB) {
  if (constDecl->getStorageClass() == SC_Static) {
    // For static inside cbuffer, take as global static.
    // Don't add to cbuffer.
    CGM.EmitGlobal(constDecl);
    return;
  }
  // Search defined structure for resource objects and fail
  if (CB.GetRangeSize() > 1 &&
      IsResourceInType(CGM.getContext(), constDecl->getType())) {
    DiagnosticsEngine &Diags = CGM.getDiags();
    unsigned DiagID = Diags.getCustomDiagID(
        DiagnosticsEngine::Error,
        "object types not supported in cbuffer/tbuffer view arrays.");
    Diags.Report(constDecl->getLocation(), DiagID);
    return;
  }
  llvm::Constant *constVal = CGM.GetAddrOfGlobalVar(constDecl);

  bool isGlobalCB = CB.GetID() == globalCBIndex;
  uint32_t offset = 0;
  bool userOffset = false;
  for (hlsl::UnusualAnnotation *it : constDecl->getUnusualAnnotations()) {
    switch (it->getKind()) {
    case hlsl::UnusualAnnotation::UA_ConstantPacking: {
      if (!isGlobalCB) {
        // TODO: check cannot mix packoffset elements with nonpackoffset
        // elements in a cbuffer.
        hlsl::ConstantPacking *cp = cast<hlsl::ConstantPacking>(it);
        offset = cp->Subcomponent << 2;
        offset += cp->ComponentOffset;
        // Change to byte.
        offset <<= 2;
        userOffset = true;
      } else {
        DiagnosticsEngine &Diags = CGM.getDiags();
        unsigned DiagID = Diags.getCustomDiagID(
            DiagnosticsEngine::Error,
            "packoffset is only allowed in a constant buffer.");
        Diags.Report(it->Loc, DiagID);
      }
      break;
    }
    case hlsl::UnusualAnnotation::UA_RegisterAssignment: {
      if (isGlobalCB) {
        RegisterAssignment *ra = cast<RegisterAssignment>(it);
        offset = ra->RegisterNumber << 2;
        // Change to byte.
        offset <<= 2;
        userOffset = true;
      }
      break;
    }
    case hlsl::UnusualAnnotation::UA_SemanticDecl:
      // skip semantic on constant
      break;
    }
  }

  std::unique_ptr<DxilResourceBase> pHlslConst = llvm::make_unique<DxilResourceBase>(DXIL::ResourceClass::Invalid);
  pHlslConst->SetLowerBound(UINT_MAX);
  pHlslConst->SetGlobalSymbol(cast<llvm::GlobalVariable>(constVal));
  pHlslConst->SetGlobalName(constDecl->getName());

  if (userOffset) {
    pHlslConst->SetLowerBound(offset);
  }
  
  DxilTypeSystem &dxilTypeSys = m_pHLModule->GetTypeSystem();
  // Just add type annotation here.
  // Offset will be allocated later.
  QualType Ty = constDecl->getType();
  if (CB.GetRangeSize() != 1) {
    while (Ty->isArrayType()) {
      Ty = Ty->getAsArrayTypeUnsafe()->getElementType();
    }
  }
  unsigned arrayEltSize = 0;
  unsigned size = AddTypeAnnotation(Ty, dxilTypeSys, arrayEltSize);
  pHlslConst->SetRangeSize(size);

  CB.AddConst(pHlslConst);

  // Save fieldAnnotation for the const var.
  DxilFieldAnnotation fieldAnnotation;
  if (userOffset)
    fieldAnnotation.SetCBufferOffset(offset);

  // Get the nested element type.
  if (Ty->isArrayType()) {
    while (const ConstantArrayType *arrayTy =
               CGM.getContext().getAsConstantArrayType(Ty)) {
      Ty = arrayTy->getElementType();
    }
  }
  bool bDefaultRowMajor = m_pHLModule->GetHLOptions().bDefaultRowMajor;
  ConstructFieldAttributedAnnotation(fieldAnnotation, Ty, bDefaultRowMajor);
  m_ConstVarAnnotationMap[constVal] = fieldAnnotation;
}

uint32_t CGMSHLSLRuntime::AddCBuffer(HLSLBufferDecl *D) {
  unique_ptr<HLCBuffer> CB = llvm::make_unique<HLCBuffer>();

  // setup the CB
  CB->SetGlobalSymbol(nullptr);
  CB->SetGlobalName(D->getNameAsString());
  CB->SetLowerBound(UINT_MAX);
  if (!D->isCBuffer()) {
    CB->SetKind(DXIL::ResourceKind::TBuffer);
  }

  // the global variable will only used once by the createHandle?
  // SetHandle(llvm::Value *pHandle);

  for (hlsl::UnusualAnnotation *it : D->getUnusualAnnotations()) {
    switch (it->getKind()) {
    case hlsl::UnusualAnnotation::UA_RegisterAssignment: {
      hlsl::RegisterAssignment *ra = cast<hlsl::RegisterAssignment>(it);
      uint32_t regNum = ra->RegisterNumber;
      uint32_t regSpace = ra->RegisterSpace;
      CB->SetSpaceID(regSpace);
      CB->SetLowerBound(regNum);
      break;
    }
    case hlsl::UnusualAnnotation::UA_SemanticDecl:
      // skip semantic on constant buffer
      break;
    case hlsl::UnusualAnnotation::UA_ConstantPacking:
      llvm_unreachable("no packoffset on constant buffer");
      break;
    }
  }

  // Add constant
  if (D->isConstantBufferView()) {
    VarDecl *constDecl = cast<VarDecl>(*D->decls_begin());
    CB->SetRangeSize(1);
    QualType Ty = constDecl->getType();
    if (Ty->isArrayType()) {
      if (!Ty->isIncompleteArrayType()) {
        unsigned arraySize = 1;
        while (Ty->isArrayType()) {
          Ty = Ty->getCanonicalTypeUnqualified();
          const ConstantArrayType *AT = cast<ConstantArrayType>(Ty);
          arraySize *= AT->getSize().getLimitedValue();
          Ty = AT->getElementType();
        }
        CB->SetRangeSize(arraySize);
      } else {
        CB->SetRangeSize(UINT_MAX);
      }
    }
    AddConstant(constDecl, *CB.get());
  } else {
    auto declsEnds = D->decls_end();
    CB->SetRangeSize(1);
    for (auto it = D->decls_begin(); it != declsEnds; it++) {
      if (VarDecl *constDecl = dyn_cast<VarDecl>(*it))
        AddConstant(constDecl, *CB.get());
      else if (isa<EmptyDecl>(*it)) {
      } else if (isa<CXXRecordDecl>(*it)) {
      } else {
        HLSLBufferDecl *inner = cast<HLSLBufferDecl>(*it);
        GetOrCreateCBuffer(inner);
      }
    }
  }

  CB->SetID(m_pHLModule->GetCBuffers().size());
  return m_pHLModule->AddCBuffer(std::move(CB));
}

HLCBuffer &CGMSHLSLRuntime::GetOrCreateCBuffer(HLSLBufferDecl *D) {
  if (constantBufMap.count(D) != 0) {
    uint32_t cbIndex = constantBufMap[D];
    return *static_cast<HLCBuffer*>(&(m_pHLModule->GetCBuffer(cbIndex)));
  }

  uint32_t cbID = AddCBuffer(D);
  constantBufMap[D] = cbID;
  return *static_cast<HLCBuffer*>(&(m_pHLModule->GetCBuffer(cbID)));
}

bool CGMSHLSLRuntime::IsPatchConstantFunction(const Function *F) {
  DXASSERT_NOMSG(F != nullptr);
  for (auto && p : patchConstantFunctionMap) {
    if (p.second == F) return true;
  }
  return false;
}

void CGMSHLSLRuntime::SetEntryFunction() {
  if (EntryFunc == nullptr) {
    DiagnosticsEngine &Diags = CGM.getDiags();
    unsigned DiagID = Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                            "cannot find entry function %0");
    Diags.Report(DiagID) << CGM.getCodeGenOpts().HLSLEntryFunction;
    return;
  }

  m_pHLModule->SetEntryFunction(EntryFunc);
}

// Here the size is CB size. So don't need check type.
static unsigned AlignCBufferOffset(unsigned offset, unsigned size, llvm::Type *Ty) {
  // offset is already 4 bytes aligned.
  bool b8BytesAlign = Ty->isDoubleTy();
  if (llvm::IntegerType *IT = dyn_cast<llvm::IntegerType>(Ty)) {
    b8BytesAlign = IT->getBitWidth() > 32;
  }

  // Align it to 4 x 4bytes.
  if (unsigned remainder = (offset & 0xf)) {
    unsigned aligned = offset - remainder + 16;
    // If cannot fit in the remainder, need align.
    bool bNeedAlign = (remainder + size) > 16;
    // Array always start aligned.
    bNeedAlign |= Ty->isArrayTy();
    if (bNeedAlign)
      return AlignTo8Bytes(aligned, b8BytesAlign);
    else
      return AlignTo8Bytes(offset, b8BytesAlign);
  } else
    return offset;
}

static unsigned AllocateDxilConstantBuffer(HLCBuffer &CB) {
  unsigned offset = 0;

  // Scan user allocated constants first.
  // Update offset.
  for (const std::unique_ptr<DxilResourceBase> &C : CB.GetConstants()) {
    if (C->GetLowerBound() == UINT_MAX)
      continue;
    unsigned size = C->GetRangeSize();
    unsigned nextOffset = size + C->GetLowerBound();
    if (offset < nextOffset)
      offset = nextOffset;
  }

  // Alloc after user allocated constants.
  for (const std::unique_ptr<DxilResourceBase> &C : CB.GetConstants()) {
    if (C->GetLowerBound() != UINT_MAX)
      continue;

    unsigned size = C->GetRangeSize();
    llvm::Type *Ty = C->GetGlobalSymbol()->getType()->getPointerElementType();
    // Align offset.
    offset = AlignCBufferOffset(offset, size, Ty);
    if (C->GetLowerBound() == UINT_MAX) {
      C->SetLowerBound(offset);
    }
    offset += size;
  }
  return offset;
}

static void AllocateDxilConstantBuffers(HLModule *pHLModule) {
  for (unsigned i = 0; i < pHLModule->GetCBuffers().size(); i++) {
    HLCBuffer &CB = *static_cast<HLCBuffer*>(&(pHLModule->GetCBuffer(i)));
    unsigned size = AllocateDxilConstantBuffer(CB);
    CB.SetSize(size);
  }
}

static void ReplaceUseInFunction(Value *V, Value *NewV, Function *F,
                                 IRBuilder<> &Builder) {
  for (auto U = V->user_begin(); U != V->user_end(); ) {
    User *user = *(U++);
    if (Instruction *I = dyn_cast<Instruction>(user)) {
      if (I->getParent()->getParent() == F) {
        // replace use with GEP if in F
        for (unsigned i = 0; i < I->getNumOperands(); i++) {
          if (I->getOperand(i) == V)
            I->setOperand(i, NewV);
        }
      }
    } else {
      // For constant operator, create local clone which use GEP.
      // Only support GEP and bitcast.
      if (GEPOperator *GEPOp = dyn_cast<GEPOperator>(user)) {
        std::vector<Value *> idxList(GEPOp->idx_begin(), GEPOp->idx_end());
        Value *NewGEP = Builder.CreateInBoundsGEP(NewV, idxList);
        ReplaceUseInFunction(GEPOp, NewGEP, F, Builder);
      } else if (GlobalVariable *GV = dyn_cast<GlobalVariable>(user)) {
        // Change the init val into NewV with Store.
        GV->setInitializer(nullptr);
        Builder.CreateStore(NewV, GV);
      } else {
        // Must be bitcast here.
        BitCastOperator *BC = cast<BitCastOperator>(user);
        Value *NewBC = Builder.CreateBitCast(NewV, BC->getType());
        ReplaceUseInFunction(BC, NewBC, F, Builder);
      }
    }
  }
}

void MarkUsedFunctionForConst(Value *V, std::unordered_set<Function*> &usedFunc) {
  for (auto U = V->user_begin(); U != V->user_end();) {
    User *user = *(U++);
    if (Instruction *I = dyn_cast<Instruction>(user)) {
      Function *F = I->getParent()->getParent();
      usedFunc.insert(F);
    } else {
      // For constant operator, create local clone which use GEP.
      // Only support GEP and bitcast.
      if (GEPOperator *GEPOp = dyn_cast<GEPOperator>(user)) {
        MarkUsedFunctionForConst(GEPOp, usedFunc);
      } else if (GlobalVariable *GV = dyn_cast<GlobalVariable>(user)) {
        MarkUsedFunctionForConst(GV, usedFunc);
      } else {
        // Must be bitcast here.
        BitCastOperator *BC = cast<BitCastOperator>(user);
        MarkUsedFunctionForConst(BC, usedFunc);
      }
    }
  }
}

static Function * GetOrCreateHLCreateHandle(HLModule &HLM, llvm::Type *HandleTy,
    ArrayRef<Value*> paramList, MDNode *MD) {
  SmallVector<llvm::Type *, 4> paramTyList;
  for (Value *param : paramList) {
    paramTyList.emplace_back(param->getType());
  }

  llvm::FunctionType *funcTy =
      llvm::FunctionType::get(HandleTy, paramTyList, false);
  llvm::Module &M = *HLM.GetModule();
  Function *CreateHandle = GetOrCreateHLFunctionWithBody(M, funcTy, HLOpcodeGroup::HLCreateHandle,
      /*opcode*/ 0, "");
  if (CreateHandle->empty()) {
    // Add body.
    BasicBlock *BB =
        BasicBlock::Create(CreateHandle->getContext(), "Entry", CreateHandle);
    IRBuilder<> Builder(BB);
    // Just return undef to make a body.
    Builder.CreateRet(UndefValue::get(HandleTy));
    // Mark resource attribute.
    HLM.MarkDxilResourceAttrib(CreateHandle, MD);
  }
  return CreateHandle;
}

static bool CreateCBufferVariable(HLCBuffer &CB,
    HLModule &HLM, llvm::Type *HandleTy) {
  bool bUsed = false;
  // Build Struct for CBuffer.
  SmallVector<llvm::Type*, 4> Elements;
  for (const std::unique_ptr<DxilResourceBase> &C : CB.GetConstants()) {
    Value *GV = C->GetGlobalSymbol();
    if (GV->hasNUsesOrMore(1))
      bUsed = true;
    // Global variable must be pointer type.
    llvm::Type *Ty = GV->getType()->getPointerElementType();
    Elements.emplace_back(Ty);
  }
  // Don't create CBuffer variable for unused cbuffer.
  if (!bUsed)
    return false;

  llvm::Module &M = *HLM.GetModule();

  bool isCBArray = CB.GetRangeSize() != 1;
  llvm::GlobalVariable *cbGV = nullptr;
  llvm::Type *cbTy = nullptr;

  unsigned cbIndexDepth = 0;
  if (!isCBArray) {
    llvm::StructType *CBStructTy =
        llvm::StructType::create(Elements, CB.GetGlobalName());
    cbGV = new llvm::GlobalVariable(M, CBStructTy, /*IsConstant*/ true,
                                    llvm::GlobalValue::ExternalLinkage,
                                    /*InitVal*/ nullptr, CB.GetGlobalName());
    cbTy = cbGV->getType();
  } else {
    // For array of ConstantBuffer, create array of struct instead of struct of
    // array.
    DXASSERT(CB.GetConstants().size() == 1,
             "ConstantBuffer should have 1 constant");
    Value *GV = CB.GetConstants()[0]->GetGlobalSymbol();
    llvm::Type *CBEltTy =
        GV->getType()->getPointerElementType()->getArrayElementType();
    cbIndexDepth = 1;
    while (CBEltTy->isArrayTy()) {
      CBEltTy = CBEltTy->getArrayElementType();
      cbIndexDepth++;
    }

    // Add one level struct type to match normal case.
    llvm::StructType *CBStructTy =
        llvm::StructType::create({CBEltTy}, CB.GetGlobalName());

    llvm::ArrayType *CBArrayTy =
        llvm::ArrayType::get(CBStructTy, CB.GetRangeSize());
    cbGV = new llvm::GlobalVariable(M, CBArrayTy, /*IsConstant*/ true,
                                    llvm::GlobalValue::ExternalLinkage,
                                    /*InitVal*/ nullptr, CB.GetGlobalName());

    cbTy = llvm::PointerType::get(CBStructTy,
                                  cbGV->getType()->getPointerAddressSpace());
  }

  CB.SetGlobalSymbol(cbGV);

  llvm::Type *opcodeTy = llvm::Type::getInt32Ty(M.getContext());
  llvm::Type *idxTy = opcodeTy;
  Constant *zeroIdx = ConstantInt::get(opcodeTy, 0);

  MDNode *MD = HLM.DxilCBufferToMDNode(CB);

  Value *HandleArgs[] = { zeroIdx, cbGV, zeroIdx };
  Function *CreateHandleFunc = GetOrCreateHLCreateHandle(HLM, HandleTy, HandleArgs, MD);

  llvm::FunctionType *SubscriptFuncTy =
      llvm::FunctionType::get(cbTy, { opcodeTy, HandleTy, idxTy}, false);

  Function *subscriptFunc =
      GetOrCreateHLFunction(M, SubscriptFuncTy, HLOpcodeGroup::HLSubscript,
                            (unsigned)HLSubscriptOpcode::CBufferSubscript);
  Constant *opArg = ConstantInt::get(opcodeTy, (unsigned)HLSubscriptOpcode::CBufferSubscript);
  Value *args[] = { opArg, nullptr, zeroIdx };

  llvm::LLVMContext &Context = M.getContext();
  llvm::Type *i32Ty = llvm::Type::getInt32Ty(Context);
  Value *zero = ConstantInt::get(i32Ty, (uint64_t)0);

  std::vector<Value *> indexArray(CB.GetConstants().size());
  std::vector<std::unordered_set<Function*>> constUsedFuncList(CB.GetConstants().size());

  for (const std::unique_ptr<DxilResourceBase> &C : CB.GetConstants()) {
    Value *idx = ConstantInt::get(i32Ty, C->GetID());
    indexArray[C->GetID()] = idx;

    Value *GV = C->GetGlobalSymbol();
    MarkUsedFunctionForConst(GV, constUsedFuncList[C->GetID()]);
  }

  for (Function &F : M.functions()) {
    if (F.isDeclaration())
      continue;

    if (GetHLOpcodeGroupByName(&F) != HLOpcodeGroup::NotHL)
      continue;

    IRBuilder<> Builder(F.getEntryBlock().getFirstInsertionPt());

    // create HL subscript to make all the use of cbuffer start from it.
    HandleArgs[HLOperandIndex::kCreateHandleResourceOpIdx] = cbGV;
    CallInst *Handle = Builder.CreateCall(CreateHandleFunc, HandleArgs);
    args[HLOperandIndex::kSubscriptObjectOpIdx] = Handle;
    Instruction *cbSubscript =
        cast<Instruction>(Builder.CreateCall(subscriptFunc, {args}));

    // Replace constant var with GEP pGV
    for (const std::unique_ptr<DxilResourceBase> &C : CB.GetConstants()) {
      Value *GV = C->GetGlobalSymbol();
      if (constUsedFuncList[C->GetID()].count(&F) == 0)
        continue;

      Value *idx = indexArray[C->GetID()];
      if (!isCBArray) {
        Instruction *GEP = cast<Instruction>(
            Builder.CreateInBoundsGEP(cbSubscript, {zero, idx}));
        // TODO: make sure the debug info is synced to GEP.
        // GEP->setDebugLoc(GV);
        ReplaceUseInFunction(GV, GEP, &F, Builder);
        // Delete if no use in F.
        if (GEP->user_empty())
          GEP->eraseFromParent();
      } else {
        for (auto U = GV->user_begin(); U != GV->user_end();) {
          User *user = *(U++);
          if (user->user_empty())
            continue;
          Instruction *I = dyn_cast<Instruction>(user);
          if (I && I->getParent()->getParent() != &F)
            continue;

          IRBuilder<> *instBuilder = &Builder;
          unique_ptr<IRBuilder<>> B;
          if (I) {
            B = llvm::make_unique<IRBuilder<>>(I);
            instBuilder = B.get();
          }

          GEPOperator *GEPOp = cast<GEPOperator>(user);
          std::vector<Value *> idxList;

          DXASSERT(GEPOp->getNumIndices() >= 1 + cbIndexDepth,
                   "must indexing ConstantBuffer array");
          idxList.reserve(GEPOp->getNumIndices() - (cbIndexDepth - 1));

          gep_type_iterator GI = gep_type_begin(*GEPOp),
                            E = gep_type_end(*GEPOp);
          idxList.push_back(GI.getOperand());
          // change array index with 0 for struct index.
          idxList.push_back(zero);
          GI++;
          Value *arrayIdx = GI.getOperand();
          GI++;
          for (unsigned curIndex = 1; GI != E && curIndex < cbIndexDepth;
               ++GI, ++curIndex) {
            arrayIdx = instBuilder->CreateMul(
                arrayIdx, Builder.getInt32(GI->getArrayNumElements()));
            arrayIdx = instBuilder->CreateAdd(arrayIdx, GI.getOperand());
          }

          for (; GI != E; ++GI) {
            idxList.push_back(GI.getOperand());
          }

          HandleArgs[HLOperandIndex::kCreateHandleIndexOpIdx] = arrayIdx;
          CallInst *Handle =
              instBuilder->CreateCall(CreateHandleFunc, HandleArgs);
          args[HLOperandIndex::kSubscriptObjectOpIdx] = Handle;
          args[HLOperandIndex::kSubscriptIndexOpIdx] = arrayIdx;

          Instruction *cbSubscript =
              cast<Instruction>(instBuilder->CreateCall(subscriptFunc, {args}));

          Instruction *NewGEP = cast<Instruction>(
              instBuilder->CreateInBoundsGEP(cbSubscript, idxList));

          ReplaceUseInFunction(GEPOp, NewGEP, &F, *instBuilder);
        }
      }
    }
    // Delete if no use in F.
    if (cbSubscript->user_empty()) {
      cbSubscript->eraseFromParent();
      Handle->eraseFromParent();
    }
  }
  return true;
}

static void ConstructCBufferAnnotation(
    HLCBuffer &CB, DxilTypeSystem &dxilTypeSys,
    std::unordered_map<Constant *, DxilFieldAnnotation> &AnnotationMap) {
  Value *GV = CB.GetGlobalSymbol();

  llvm::StructType *CBStructTy =
          dyn_cast<llvm::StructType>(GV->getType()->getPointerElementType());

  if (!CBStructTy) {
    // For Array of ConstantBuffer.
    llvm::ArrayType *CBArrayTy =
        cast<llvm::ArrayType>(GV->getType()->getPointerElementType());
    CBStructTy = cast<llvm::StructType>(CBArrayTy->getArrayElementType());
  }

  DxilStructAnnotation *CBAnnotation =
      dxilTypeSys.AddStructAnnotation(CBStructTy);
  CBAnnotation->SetCBufferSize(CB.GetSize());

  // Set fieldAnnotation for each constant var.
  for (const std::unique_ptr<DxilResourceBase> &C : CB.GetConstants()) {
    Constant *GV = C->GetGlobalSymbol();
    DxilFieldAnnotation &fieldAnnotation =
        CBAnnotation->GetFieldAnnotation(C->GetID());
    fieldAnnotation = AnnotationMap[GV];
    // This is after CBuffer allocation.
    fieldAnnotation.SetCBufferOffset(C->GetLowerBound());
    fieldAnnotation.SetFieldName(C->GetGlobalName());
  }
}

static void ConstructCBuffer(
    HLModule *pHLModule,
    llvm::Type *CBufferType,
    std::unordered_map<Constant *, DxilFieldAnnotation> &AnnotationMap) {
  DxilTypeSystem &dxilTypeSys = pHLModule->GetTypeSystem();
  llvm::Type *HandleTy = pHLModule->GetOP()->GetHandleType();
  for (unsigned i = 0; i < pHLModule->GetCBuffers().size(); i++) {
    HLCBuffer &CB = *static_cast<HLCBuffer*>(&(pHLModule->GetCBuffer(i)));
    if (CB.GetConstants().size() == 0) {
      // Create Fake variable for cbuffer which is empty.
      llvm::GlobalVariable *pGV = new llvm::GlobalVariable(
          *pHLModule->GetModule(), CBufferType, true,
          llvm::GlobalValue::ExternalLinkage, nullptr, CB.GetGlobalName());
      CB.SetGlobalSymbol(pGV);
    } else {
      bool bCreated =
          CreateCBufferVariable(CB, *pHLModule, HandleTy);
      if (bCreated)
        ConstructCBufferAnnotation(CB, dxilTypeSys, AnnotationMap);
      else {
        // Create Fake variable for cbuffer which is unused.
        llvm::GlobalVariable *pGV = new llvm::GlobalVariable(
            *pHLModule->GetModule(), CBufferType, true,
            llvm::GlobalValue::ExternalLinkage, nullptr, CB.GetGlobalName());
        CB.SetGlobalSymbol(pGV);
      }
    }
    // Clear the constants which useless now.
    CB.GetConstants().clear();
  }
}

static void ReplaceBoolVectorSubscript(CallInst *CI) {
  Value *Ptr = CI->getArgOperand(0);
  Value *Idx = CI->getArgOperand(1);
  Value *IdxList[] = {ConstantInt::get(Idx->getType(), 0), Idx};

  for (auto It = CI->user_begin(), E = CI->user_end(); It != E;) {
    Instruction *user = cast<Instruction>(*(It++));

    IRBuilder<> Builder(user);
    Value *GEP = Builder.CreateInBoundsGEP(Ptr, IdxList);

    if (LoadInst *LI = dyn_cast<LoadInst>(user)) {
      Value *NewLd = Builder.CreateLoad(GEP);
      Value *cast = Builder.CreateZExt(NewLd, LI->getType());
      LI->replaceAllUsesWith(cast);
      LI->eraseFromParent();
    } else {
      // Must be a store inst here.
      StoreInst *SI = cast<StoreInst>(user);
      Value *V = SI->getValueOperand();
      Value *cast =
          Builder.CreateICmpNE(V, llvm::ConstantInt::get(V->getType(), 0));
      Builder.CreateStore(cast, GEP);
      SI->eraseFromParent();
    }
  }
  CI->eraseFromParent();
}

static void ReplaceBoolVectorSubscript(Function *F) {
  for (auto It = F->user_begin(), E = F->user_end(); It != E; ) {
    User *user = *(It++);
    CallInst *CI = cast<CallInst>(user);
    ReplaceBoolVectorSubscript(CI);
  }
}

// Add function body for intrinsic if possible.
static Function *CreateOpFunction(llvm::Module &M, Function *F,
                                  llvm::FunctionType *funcTy,
                                  HLOpcodeGroup group, unsigned opcode) {
  Function *opFunc = nullptr;

  llvm::Type *opcodeTy = llvm::Type::getInt32Ty(M.getContext());
  if (group == HLOpcodeGroup::HLIntrinsic) {
    IntrinsicOp intriOp = static_cast<IntrinsicOp>(opcode);
    switch (intriOp) {
    case IntrinsicOp::MOP_Append: 
    case IntrinsicOp::MOP_Consume: {
      bool bAppend = intriOp == IntrinsicOp::MOP_Append;
      llvm::Type *handleTy = funcTy->getParamType(HLOperandIndex::kHandleOpIdx);
      // Don't generate body for OutputStream::Append.
      if (bAppend && HLModule::IsStreamOutputPtrType(handleTy)) {
        opFunc = GetOrCreateHLFunction(M, funcTy, group, opcode);
        break;
      }

      opFunc = GetOrCreateHLFunctionWithBody(M, funcTy, group, opcode,
                                             bAppend ? "append" : "consume");
      llvm::Type *counterTy = llvm::Type::getInt32Ty(M.getContext());
      llvm::FunctionType *IncCounterFuncTy =
          llvm::FunctionType::get(counterTy, {opcodeTy, handleTy}, false);
      unsigned counterOpcode = bAppend ? (unsigned)IntrinsicOp::MOP_IncrementCounter:
          (unsigned)IntrinsicOp::MOP_DecrementCounter;
      Function *incCounterFunc =
          GetOrCreateHLFunction(M, IncCounterFuncTy, group,
                                counterOpcode);

      llvm::Type *idxTy = counterTy;
      llvm::Type *valTy = bAppend ?
          funcTy->getParamType(HLOperandIndex::kAppendValOpIndex):funcTy->getReturnType();
      llvm::Type *subscriptTy = valTy;
      if (!valTy->isPointerTy()) {
        // Return type for subscript should be pointer type.
        subscriptTy = llvm::PointerType::get(valTy, 0);
      }

      llvm::FunctionType *SubscriptFuncTy =
          llvm::FunctionType::get(subscriptTy, {opcodeTy, handleTy, idxTy}, false);

      Function *subscriptFunc =
          GetOrCreateHLFunction(M, SubscriptFuncTy, HLOpcodeGroup::HLSubscript,
                                (unsigned)HLSubscriptOpcode::DefaultSubscript);

      BasicBlock *BB = BasicBlock::Create(opFunc->getContext(), "Entry", opFunc);
      IRBuilder<> Builder(BB);
      auto argIter = opFunc->args().begin();
      // Skip the opcode arg.
      argIter++;
      Argument *thisArg = argIter++;
      // int counter = IncrementCounter/DecrementCounter(Buf);
      Value *incCounterOpArg =
          ConstantInt::get(idxTy, counterOpcode);
      Value *counter =
          Builder.CreateCall(incCounterFunc, {incCounterOpArg, thisArg});
      // Buf[counter];
      Value *subscriptOpArg = ConstantInt::get(
          idxTy, (unsigned)HLSubscriptOpcode::DefaultSubscript);
      Value *subscript =
          Builder.CreateCall(subscriptFunc, {subscriptOpArg, thisArg, counter});

      if (bAppend) {
        Argument *valArg = argIter;
        // Buf[counter] = val;
        if (valTy->isPointerTy()) {
          unsigned size = M.getDataLayout().getTypeAllocSize(subscript->getType()->getPointerElementType());
          Builder.CreateMemCpy(subscript, valArg, size, 1);
        } else
          Builder.CreateStore(valArg, subscript);
        Builder.CreateRetVoid();
      } else {
        // return Buf[counter];
        if (valTy->isPointerTy())
          Builder.CreateRet(subscript);
        else {
          Value *retVal = Builder.CreateLoad(subscript);
          Builder.CreateRet(retVal);
        }
      }
    } break;
    case IntrinsicOp::IOP_sincos: {
      opFunc = GetOrCreateHLFunctionWithBody(M, funcTy, group, opcode, "sincos");
      llvm::Type *valTy = funcTy->getParamType(HLOperandIndex::kTrinaryOpSrc0Idx);

      llvm::FunctionType *sinFuncTy =
          llvm::FunctionType::get(valTy, {opcodeTy, valTy}, false);
      unsigned sinOp = static_cast<unsigned>(IntrinsicOp::IOP_sin);
      unsigned cosOp = static_cast<unsigned>(IntrinsicOp::IOP_cos);
      Function *sinFunc = GetOrCreateHLFunction(M, sinFuncTy, group, sinOp);
      Function *cosFunc = GetOrCreateHLFunction(M, sinFuncTy, group, cosOp);

      BasicBlock *BB = BasicBlock::Create(opFunc->getContext(), "Entry", opFunc);
      IRBuilder<> Builder(BB);
      auto argIter = opFunc->args().begin();
      // Skip the opcode arg.
      argIter++;
      Argument *valArg = argIter++;
      Argument *sinPtrArg = argIter++;
      Argument *cosPtrArg = argIter++;

      Value *sinOpArg =
          ConstantInt::get(opcodeTy, sinOp);
      Value *sinVal = Builder.CreateCall(sinFunc, {sinOpArg, valArg});
      Builder.CreateStore(sinVal, sinPtrArg);

      Value *cosOpArg =
          ConstantInt::get(opcodeTy, cosOp);
      Value *cosVal = Builder.CreateCall(cosFunc, {cosOpArg, valArg});
      Builder.CreateStore(cosVal, cosPtrArg);
      // Ret.
      Builder.CreateRetVoid();
    } break;
    default:
      opFunc = GetOrCreateHLFunction(M, funcTy, group, opcode);
      break;
    }
  }
  else if (group == HLOpcodeGroup::HLExtIntrinsic) {
    llvm::StringRef fnName = F->getName();
    llvm::StringRef groupName = GetHLOpcodeGroupNameByAttr(F);
    opFunc = GetOrCreateHLFunction(M, funcTy, group, &groupName, &fnName, opcode);
  }
  else {
    opFunc = GetOrCreateHLFunction(M, funcTy, group, opcode);
  }

  // Add attribute
  if (F->hasFnAttribute(Attribute::ReadNone))
    opFunc->addFnAttr(Attribute::ReadNone);
  if (F->hasFnAttribute(Attribute::ReadOnly))
    opFunc->addFnAttr(Attribute::ReadOnly);
  return opFunc;
}

static Value *CreateHandleFromResPtr(
    Value *ResPtr, HLModule &HLM, llvm::Type *HandleTy,
    std::unordered_map<llvm::Type *, MDNode *> &resMetaMap,
    IRBuilder<> &Builder) {
  llvm::Type *objTy = ResPtr->getType()->getPointerElementType();
  DXASSERT(resMetaMap.count(objTy), "cannot find resource type");
  MDNode *MD = resMetaMap[objTy];
  // Load to make sure resource only have Ld/St use so mem2reg could remove
  // temp resource.
  Value *ldObj = Builder.CreateLoad(ResPtr);
  Value *opcode = Builder.getInt32(0);
  Value *args[] = {opcode, ldObj};
  Function *CreateHandle = GetOrCreateHLCreateHandle(HLM, HandleTy, args, MD);
  CallInst *Handle = Builder.CreateCall(CreateHandle, args);
  return Handle;
}

static void AddOpcodeParamForIntrinsic(HLModule &HLM, Function *F,
                                       unsigned opcode, llvm::Type *HandleTy,
    std::unordered_map<llvm::Type *, MDNode*> &resMetaMap) {
  llvm::Module &M = *HLM.GetModule();
  llvm::FunctionType *oldFuncTy = F->getFunctionType();

  SmallVector<llvm::Type *, 4> paramTyList;
  // Add the opcode param
  llvm::Type *opcodeTy = llvm::Type::getInt32Ty(M.getContext());
  paramTyList.emplace_back(opcodeTy);
  paramTyList.append(oldFuncTy->param_begin(), oldFuncTy->param_end());

  for (unsigned i = 1; i < paramTyList.size(); i++) {
    llvm::Type *Ty = paramTyList[i];
    if (Ty->isPointerTy()) {
      Ty = Ty->getPointerElementType();
      if (HLModule::IsHLSLObjectType(Ty) &&
          // StreamOutput don't need handle.
          !HLModule::IsStreamOutputType(Ty)) {
        // Use handle type for object type.
        // This will make sure temp object variable only used by createHandle.
        paramTyList[i] = HandleTy;
      }
    }
  }

  HLOpcodeGroup group = hlsl::GetHLOpcodeGroup(F);

  if (group == HLOpcodeGroup::HLSubscript &&
      opcode == static_cast<unsigned>(HLSubscriptOpcode::VectorSubscript)) {
    llvm::FunctionType *FT = F->getFunctionType();
    llvm::Type *VecArgTy = FT->getParamType(0);
    llvm::VectorType *VType =
        cast<llvm::VectorType>(VecArgTy->getPointerElementType());
    llvm::Type *Ty = VType->getElementType();
    DXASSERT(Ty->isIntegerTy(), "Only bool could use VectorSubscript");
    llvm::IntegerType *ITy = cast<IntegerType>(Ty);

    DXASSERT_LOCALVAR(ITy, ITy->getBitWidth() == 1, "Only bool could use VectorSubscript");

    // The return type is i8*.
    // Replace all uses with i1*.
    ReplaceBoolVectorSubscript(F);
    return;
  }

  bool isDoubleSubscriptFunc = group == HLOpcodeGroup::HLSubscript &&
      opcode == static_cast<unsigned>(HLSubscriptOpcode::DoubleSubscript);

  llvm::Type *RetTy = oldFuncTy->getReturnType();

  if (isDoubleSubscriptFunc) {
    CallInst *doubleSub = cast<CallInst>(*F->user_begin());
   
    // Change currentIdx type into coord type.
    auto U = doubleSub->user_begin();
    Value *user = *U;
    CallInst *secSub = cast<CallInst>(user);
    unsigned coordIdx = HLOperandIndex::kSubscriptIndexOpIdx;
    // opcode operand not add yet, so the index need -1.
    if (GetHLOpcodeGroupByName(secSub->getCalledFunction()) == HLOpcodeGroup::NotHL)
      coordIdx -= 1;
    
    Value *coord = secSub->getArgOperand(coordIdx);

    llvm::Type *coordTy = coord->getType();
    paramTyList[HLOperandIndex::kSubscriptIndexOpIdx] = coordTy;
    // Add the sampleIdx or mipLevel parameter to the end.
    paramTyList.emplace_back(opcodeTy);
    // Change return type to be resource ret type.
    // opcode operand not add yet, so the index need -1.
    Value *objPtr = doubleSub->getArgOperand(HLOperandIndex::kSubscriptObjectOpIdx-1);
    // Must be a GEP
    GEPOperator *objGEP = cast<GEPOperator>(objPtr);
    gep_type_iterator GEPIt = gep_type_begin(objGEP), E = gep_type_end(objGEP);
    llvm::Type *resTy = nullptr;
    while (GEPIt != E) {
      if (HLModule::IsHLSLObjectType(*GEPIt)) {
        resTy = *GEPIt;
        break;
      }
      GEPIt++;
    }

    DXASSERT(resTy, "must find the resource type");
    // Change object type to handle type.
    paramTyList[HLOperandIndex::kSubscriptObjectOpIdx] = HandleTy;
    // Change RetTy into pointer of resource reture type.
    RetTy = cast<StructType>(resTy)->getElementType(0)->getPointerTo();

    llvm::Type *sliceTy = objGEP->getType()->getPointerElementType();
    DXIL::ResourceClass RC = HLM.GetResourceClass(sliceTy);
    DXIL::ResourceKind RK = HLM.GetResourceKind(sliceTy);
    HLM.AddResourceTypeAnnotation(resTy, RC, RK);
  }

  llvm::FunctionType *funcTy =
      llvm::FunctionType::get(RetTy, paramTyList, false);

  Function *opFunc = CreateOpFunction(M, F, funcTy, group, opcode);
  StringRef lower = hlsl::GetHLLowerStrategy(F);
  if (!lower.empty())
    hlsl::SetHLLowerStrategy(opFunc, lower);

  for (auto user = F->user_begin(); user != F->user_end();) {
    // User must be a call.
    CallInst *oldCI = cast<CallInst>(*(user++));

    SmallVector<Value *, 4> opcodeParamList;
    Value *opcodeConst = Constant::getIntegerValue(opcodeTy, APInt(32, opcode));
    opcodeParamList.emplace_back(opcodeConst);

    opcodeParamList.append(oldCI->arg_operands().begin(),
                           oldCI->arg_operands().end());
    IRBuilder<> Builder(oldCI);

    if (isDoubleSubscriptFunc) {
      // Change obj to the resource pointer.
      Value *objVal = opcodeParamList[HLOperandIndex::kSubscriptObjectOpIdx];
      GEPOperator *objGEP = cast<GEPOperator>(objVal);
      SmallVector<Value *, 8> IndexList;
      IndexList.append(objGEP->idx_begin(), objGEP->idx_end());
      Value *lastIndex = IndexList.back();
      ConstantInt *constIndex = cast<ConstantInt>(lastIndex);
      DXASSERT_LOCALVAR(constIndex, constIndex->getLimitedValue() == 1, "last index must 1");
      // Remove the last index.
      IndexList.pop_back();
      objVal = objGEP->getPointerOperand();
      if (IndexList.size() > 1)
        objVal = Builder.CreateInBoundsGEP(objVal, IndexList);

      Value *Handle =
          CreateHandleFromResPtr(objVal, HLM, HandleTy, resMetaMap, Builder);
      // Change obj to the resource pointer.
      opcodeParamList[HLOperandIndex::kSubscriptObjectOpIdx] = Handle;

      // Set idx and mipIdx.
      Value *mipIdx = opcodeParamList[HLOperandIndex::kSubscriptIndexOpIdx];
      auto U = oldCI->user_begin();
      Value *user = *U;
      CallInst *secSub = cast<CallInst>(user);
      unsigned idxOpIndex = HLOperandIndex::kSubscriptIndexOpIdx;
      if (GetHLOpcodeGroupByName(secSub->getCalledFunction()) == HLOpcodeGroup::NotHL)
        idxOpIndex--;
      Value *idx = secSub->getArgOperand(idxOpIndex);

      DXASSERT(secSub->hasOneUse(), "subscript should only has one use");

      // Add the sampleIdx or mipLevel parameter to the end.
      opcodeParamList[HLOperandIndex::kSubscriptIndexOpIdx] = idx;
      opcodeParamList.emplace_back(mipIdx);
      // Insert new call before secSub to make sure idx is ready to use.
      Builder.SetInsertPoint(secSub);
    }

    for (unsigned i = 1; i < opcodeParamList.size(); i++) {
      Value *arg = opcodeParamList[i];
      llvm::Type *Ty = arg->getType();
      if (Ty->isPointerTy()) {
        Ty = Ty->getPointerElementType();
        if (HLModule::IsHLSLObjectType(Ty) &&
          // StreamOutput don't need handle.
          !HLModule::IsStreamOutputType(Ty)) {
          // Use object type directly, not by pointer.
          // This will make sure temp object variable only used by ld/st.
          if (GEPOperator *argGEP = dyn_cast<GEPOperator>(arg)) {
            std::vector<Value*> idxList(argGEP->idx_begin(), argGEP->idx_end());
            // Create instruction to avoid GEPOperator.
            GetElementPtrInst *GEP = GetElementPtrInst::CreateInBounds(argGEP->getPointerOperand(), 
                idxList);
            Builder.Insert(GEP);
            arg = GEP;
          }
          Value *Handle = CreateHandleFromResPtr(arg, HLM, HandleTy,
                                                 resMetaMap, Builder);
          opcodeParamList[i] = Handle;
        }
      }
    }

    Value *CI = Builder.CreateCall(opFunc, opcodeParamList);
    if (!isDoubleSubscriptFunc) {
      // replace new call and delete the old call
      oldCI->replaceAllUsesWith(CI);
      oldCI->eraseFromParent();
    } else {
      // For double script.
      // Replace single users use with new CI.
      auto U = oldCI->user_begin();
      Value *user = *U;
      CallInst *secSub = cast<CallInst>(user);
      secSub->replaceAllUsesWith(CI);
      secSub->eraseFromParent();
      oldCI->eraseFromParent();
    }
  }
  // delete the function
  F->eraseFromParent();
}

static void AddOpcodeParamForIntrinsics(HLModule &HLM
    , std::vector<std::pair<Function *, unsigned>> &intrinsicMap,
    std::unordered_map<llvm::Type *, MDNode*> &resMetaMap) {
  llvm::Type *HandleTy = HLM.GetOP()->GetHandleType();
  for (auto mapIter : intrinsicMap) {
    Function *F = mapIter.first;
    if (F->user_empty()) {
      // delete the function
      F->eraseFromParent();
      continue;
    }

    unsigned opcode = mapIter.second;
    AddOpcodeParamForIntrinsic(HLM, F, opcode, HandleTy, resMetaMap);
  }
}

static Value *CastLdValue(Value *Ptr, llvm::Type *FromTy, llvm::Type *ToTy, IRBuilder<> &Builder) {
  if (ToTy->isVectorTy()) {
    unsigned vecSize = ToTy->getVectorNumElements();
    if (vecSize == 1 && ToTy->getVectorElementType() == FromTy) {
      Value *V = Builder.CreateLoad(Ptr);
      // ScalarToVec1Splat
      // Change scalar into vec1.
      Value *Vec1 = UndefValue::get(ToTy);
      return Builder.CreateInsertElement(Vec1, V, (uint64_t)0);
    } else if (FromTy->isVectorTy() && vecSize == 1) {
      Value *V = Builder.CreateLoad(Ptr);
      // VectorTrunc
      // Change vector into vec1.
      return Builder.CreateShuffleVector(V, V, {0});
    } else if (FromTy->isArrayTy()) {
      llvm::Type *FromEltTy = FromTy->getArrayElementType();

      llvm::Type *ToEltTy = ToTy->getVectorElementType();
      if (FromTy->getArrayNumElements() == vecSize && FromEltTy == ToEltTy) {
        // ArrayToVector.
        Value *NewLd = UndefValue::get(ToTy);
        Value *zeroIdx = Builder.getInt32(0);
        for (unsigned i = 0; i < vecSize; i++) {
          Value *GEP = Builder.CreateInBoundsGEP(
              Ptr, {zeroIdx, Builder.getInt32(i)});
          Value *Elt = Builder.CreateLoad(GEP);
          NewLd = Builder.CreateInsertElement(NewLd, Elt, i);
        }
        return NewLd;
      }
    }
  } else if (FromTy == Builder.getInt1Ty()) {
    Value *V = Builder.CreateLoad(Ptr);
    // BoolCast
    DXASSERT_NOMSG(ToTy->isIntegerTy());
    return Builder.CreateZExt(V, ToTy);
  }

  return nullptr;
}

static Value  *CastStValue(Value *Ptr, Value *V, llvm::Type *FromTy, llvm::Type *ToTy, IRBuilder<> &Builder) {
  if (ToTy->isVectorTy()) {
    unsigned vecSize = ToTy->getVectorNumElements();
    if (vecSize == 1 && ToTy->getVectorElementType() == FromTy) {
      // ScalarToVec1Splat
      // Change vec1 back to scalar.
      Value *Elt = Builder.CreateExtractElement(V, (uint64_t)0);
      return Elt;
    } else if (FromTy->isVectorTy() && vecSize == 1) {
      // VectorTrunc
      // Change vec1 into vector.
      // Should not happen.
      // Reported error at Sema::ImpCastExprToType.
      DXASSERT_NOMSG(0);
    } else if (FromTy->isArrayTy()) {
      llvm::Type *FromEltTy = FromTy->getArrayElementType();

      llvm::Type *ToEltTy = ToTy->getVectorElementType();
      if (FromTy->getArrayNumElements() == vecSize && FromEltTy == ToEltTy) {
        // ArrayToVector.
        Value *zeroIdx = Builder.getInt32(0);
        for (unsigned i = 0; i < vecSize; i++) {
          Value *Elt = Builder.CreateExtractElement(V, i);
          Value *GEP = Builder.CreateInBoundsGEP(
              Ptr, {zeroIdx, Builder.getInt32(i)});
          Builder.CreateStore(Elt, GEP);
        }
        // The store already done.
        // Return null to ignore use of the return value.
        return nullptr;
      }
    }
  } else if (FromTy == Builder.getInt1Ty()) {
    // BoolCast
    // Change i1 to ToTy.
    DXASSERT_NOMSG(ToTy->isIntegerTy());
    Value *CastV = Builder.CreateICmpNE(V, ConstantInt::get(V->getType(), 0));
    return CastV;
  }

  return nullptr;
}

static bool SimplifyBitCastLoad(LoadInst *LI, llvm::Type *FromTy, llvm::Type *ToTy, Value *Ptr) {
  IRBuilder<> Builder(LI);
  // Cast FromLd to ToTy.
  Value *CastV = CastLdValue(Ptr, FromTy, ToTy, Builder);
  if (CastV) {
    LI->replaceAllUsesWith(CastV);
    return true;
  } else {
    return false;
  }
}

static bool SimplifyBitCastStore(StoreInst *SI, llvm::Type *FromTy, llvm::Type *ToTy, Value *Ptr) {
  IRBuilder<> Builder(SI);
  Value *V = SI->getValueOperand();
  // Cast Val to FromTy.
  Value *CastV = CastStValue(Ptr, V, FromTy, ToTy, Builder);
  if (CastV) {
    Builder.CreateStore(CastV, Ptr);
    return true;
  } else {
    return false;
  }
}

static bool SimplifyBitCastGEP(GEPOperator *GEP, llvm::Type *FromTy, llvm::Type *ToTy, Value *Ptr) {
  if (ToTy->isVectorTy()) {
    unsigned vecSize = ToTy->getVectorNumElements();
    if (vecSize == 1 && ToTy->getVectorElementType() == FromTy) {
      // ScalarToVec1Splat
      GEP->replaceAllUsesWith(Ptr);
      return true;
    } else if (FromTy->isVectorTy() && vecSize == 1) {
      // VectorTrunc
      DXASSERT_NOMSG(
          !isa<llvm::VectorType>(GEP->getType()->getPointerElementType()));
      IRBuilder<> Builder(FromTy->getContext());
      if (Instruction *I = dyn_cast<Instruction>(GEP))
        Builder.SetInsertPoint(I);
      std::vector<Value *> idxList(GEP->idx_begin(), GEP->idx_end());
      Value *NewGEP = Builder.CreateInBoundsGEP(Ptr, idxList);
      GEP->replaceAllUsesWith(NewGEP);
      return true;
    } else if (FromTy->isArrayTy()) {
      llvm::Type *FromEltTy = FromTy->getArrayElementType();

      llvm::Type *ToEltTy = ToTy->getVectorElementType();
      if (FromTy->getArrayNumElements() == vecSize && FromEltTy == ToEltTy) {
        // ArrayToVector.
      }
    }
  } else if (FromTy == llvm::Type::getInt1Ty(FromTy->getContext())) {
    // BoolCast
  }
  return false;
}

static void SimplifyBitCast(BitCastOperator *BC, std::vector<Instruction *> &deadInsts) {
  Value *Ptr = BC->getOperand(0);
  llvm::Type *FromTy = Ptr->getType();
  llvm::Type *ToTy = BC->getType();

  if (!FromTy->isPointerTy() || !ToTy->isPointerTy())
    return;

  FromTy = FromTy->getPointerElementType();
  ToTy = ToTy->getPointerElementType();
  // Take care case like %2 = bitcast %struct.T* %1 to <1 x float>*.
  if (FromTy->isStructTy()) {
    IRBuilder<> Builder(FromTy->getContext());
    if (Instruction *I = dyn_cast<Instruction>(BC))
      Builder.SetInsertPoint(I);

    Value *zeroIdx = Builder.getInt32(0);
    unsigned nestLevel = 1;
    while (llvm::StructType *ST = dyn_cast<llvm::StructType>(FromTy)) {
      FromTy = ST->getElementType(0);
      nestLevel++;
    }
    std::vector<Value *> idxList(nestLevel, zeroIdx);
    Ptr = Builder.CreateGEP(Ptr, idxList);
  }

  for (User *U : BC->users()) {
    if (LoadInst *LI = dyn_cast<LoadInst>(U)) {
      if (SimplifyBitCastLoad(LI, FromTy, ToTy, Ptr))
        deadInsts.emplace_back(LI);
    } else if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
      if (SimplifyBitCastStore(SI, FromTy, ToTy, Ptr))
        deadInsts.emplace_back(SI);
    } else if (GEPOperator *GEP = dyn_cast<GEPOperator>(U)) {
      if (SimplifyBitCastGEP(GEP, FromTy, ToTy, Ptr))
        if (Instruction *I = dyn_cast<Instruction>(GEP))
          deadInsts.emplace_back(I);
    } else if (CallInst *CI = dyn_cast<CallInst>(U)) {
      // Skip function call.
    } else {
      DXASSERT(0, "not support yet");
    }
  }
}

typedef float(__cdecl *FloatUnaryEvalFuncType)(float);
typedef double(__cdecl *DoubleUnaryEvalFuncType)(double);

typedef float(__cdecl *FloatBinaryEvalFuncType)(float, float);
typedef double(__cdecl *DoubleBinaryEvalFuncType)(double, double);

static Value * EvalUnaryIntrinsic(CallInst *CI,
                               FloatUnaryEvalFuncType floatEvalFunc,
                               DoubleUnaryEvalFuncType doubleEvalFunc) {
  Value *V = CI->getArgOperand(0);
  ConstantFP *fpV = cast<ConstantFP>(V);
  llvm::Type *Ty = CI->getType();
  Value *Result = nullptr;
  if (Ty->isDoubleTy()) {
    double dV = fpV->getValueAPF().convertToDouble();
    Value *dResult = ConstantFP::get(V->getType(), doubleEvalFunc(dV));

    CI->replaceAllUsesWith(dResult);
    Result = dResult;
  } else {
    DXASSERT_NOMSG(Ty->isFloatTy());
    float fV = fpV->getValueAPF().convertToFloat();
    Value *dResult = ConstantFP::get(V->getType(), floatEvalFunc(fV));

    CI->replaceAllUsesWith(dResult);
    Result = dResult;
  }

  CI->eraseFromParent();
  return Result;
}

static Value * EvalBinaryIntrinsic(CallInst *CI,
                               FloatBinaryEvalFuncType floatEvalFunc,
                               DoubleBinaryEvalFuncType doubleEvalFunc) {
  Value *V0 = CI->getArgOperand(0);
  ConstantFP *fpV0 = cast<ConstantFP>(V0);
  Value *V1 = CI->getArgOperand(1);
  ConstantFP *fpV1 = cast<ConstantFP>(V1);
  llvm::Type *Ty = CI->getType();
  Value *Result = nullptr;
  if (Ty->isDoubleTy()) {
    double dV0 = fpV0->getValueAPF().convertToDouble();
    double dV1 = fpV1->getValueAPF().convertToDouble();
    Value *dResult = ConstantFP::get(V0->getType(), doubleEvalFunc(dV0, dV1));
    CI->replaceAllUsesWith(dResult);
    Result = dResult;
  } else {
    DXASSERT_NOMSG(Ty->isFloatTy());
    float fV0 = fpV0->getValueAPF().convertToFloat();
    float fV1 = fpV1->getValueAPF().convertToFloat();
    Value *dResult = ConstantFP::get(V0->getType(), floatEvalFunc(fV0, fV1));

    CI->replaceAllUsesWith(dResult);
    Result = dResult;
  }

  CI->eraseFromParent();
  return Result;
}

static Value * TryEvalIntrinsic(CallInst *CI, IntrinsicOp intriOp) {
  switch (intriOp) {
  case IntrinsicOp::IOP_tan: {
    return EvalUnaryIntrinsic(CI, tanf, tan);
  } break;
  case IntrinsicOp::IOP_tanh: {
    return EvalUnaryIntrinsic(CI, tanhf, tanh);
  } break;
  case IntrinsicOp::IOP_sin: {
    return EvalUnaryIntrinsic(CI, sinf, sin);
  } break;
  case IntrinsicOp::IOP_sinh: {
    return EvalUnaryIntrinsic(CI, sinhf, sinh);
  } break;
  case IntrinsicOp::IOP_cos: {
    return EvalUnaryIntrinsic(CI, cosf, cos);
  } break;
  case IntrinsicOp::IOP_cosh: {
    return EvalUnaryIntrinsic(CI, coshf, cosh);
  } break;
  case IntrinsicOp::IOP_asin: {
    return EvalUnaryIntrinsic(CI, asinf, asin);
  } break;
  case IntrinsicOp::IOP_acos: {
    return EvalUnaryIntrinsic(CI, acosf, acos);
  } break;
  case IntrinsicOp::IOP_atan: {
    return EvalUnaryIntrinsic(CI, atanf, atan);
  } break;
  case IntrinsicOp::IOP_atan2: {
    Value *V0 = CI->getArgOperand(0);
    ConstantFP *fpV0 = cast<ConstantFP>(V0);

    Value *V1 = CI->getArgOperand(1);
    ConstantFP *fpV1 = cast<ConstantFP>(V1);

    llvm::Type *Ty = CI->getType();
    Value *Result = nullptr;
    if (Ty->isDoubleTy()) {
      double dV0 = fpV0->getValueAPF().convertToDouble();
      double dV1 = fpV1->getValueAPF().convertToDouble();
      Value *atanV = ConstantFP::get(CI->getType(), atan(dV0 / dV1));
      CI->replaceAllUsesWith(atanV);
      Result = atanV;
    } else {
      DXASSERT_NOMSG(Ty->isFloatTy());
      float fV0 = fpV0->getValueAPF().convertToFloat();
      float fV1 = fpV1->getValueAPF().convertToFloat();
      Value *atanV = ConstantFP::get(CI->getType(), atanf(fV0 / fV1));
      CI->replaceAllUsesWith(atanV);
      Result = atanV;
    }
    CI->eraseFromParent();
    return Result;
  } break;
  case IntrinsicOp::IOP_sqrt: {
    return EvalUnaryIntrinsic(CI, sqrtf, sqrt);
  } break;
  case IntrinsicOp::IOP_rsqrt: {
    auto rsqrtF = [](float v) -> float { return 1.0 / sqrtf(v); };
    auto rsqrtD = [](double v) -> double { return 1.0 / sqrt(v); };

    return EvalUnaryIntrinsic(CI, rsqrtF, rsqrtD);
  } break;
  case IntrinsicOp::IOP_exp: {
    return EvalUnaryIntrinsic(CI, expf, exp);
  } break;
  case IntrinsicOp::IOP_exp2: {
    return EvalUnaryIntrinsic(CI, exp2f, exp2);
  } break;
  case IntrinsicOp::IOP_log: {
    return EvalUnaryIntrinsic(CI, logf, log);
  } break;
  case IntrinsicOp::IOP_log10: {
    return EvalUnaryIntrinsic(CI, log10f, log10);
  } break;
  case IntrinsicOp::IOP_log2: {
    return EvalUnaryIntrinsic(CI, log2f, log2);
  } break;
  case IntrinsicOp::IOP_pow: {
    return EvalBinaryIntrinsic(CI, powf, pow);
  } break;
  case IntrinsicOp::IOP_max: {
    auto maxF = [](float a, float b) -> float { return a > b ? a:b; };
    auto maxD = [](double a, double b) -> double { return a > b ? a:b; };
    return EvalBinaryIntrinsic(CI, maxF, maxD);
  } break;
  case IntrinsicOp::IOP_min: {
    auto minF = [](float a, float b) -> float { return a < b ? a:b; };
    auto minD = [](double a, double b) -> double { return a < b ? a:b; };
    return EvalBinaryIntrinsic(CI, minF, minD);
  } break;
  case IntrinsicOp::IOP_rcp: {
    auto rcpF = [](float v) -> float { return 1.0 / v; };
    auto rcpD = [](double v) -> double { return 1.0 / v; };

    return EvalUnaryIntrinsic(CI, rcpF, rcpD);
  } break;
  case IntrinsicOp::IOP_ceil: {
    return EvalUnaryIntrinsic(CI, ceilf, ceil);
  } break;
  case IntrinsicOp::IOP_floor: {
    return EvalUnaryIntrinsic(CI, floorf, floor);
  } break;
  case IntrinsicOp::IOP_round: {
    return EvalUnaryIntrinsic(CI, roundf, round);
  } break;
  case IntrinsicOp::IOP_trunc: {
    return EvalUnaryIntrinsic(CI, truncf, trunc);
  } break;
  case IntrinsicOp::IOP_frac: {
    auto fracF = [](float v) -> float {
      int exp = 0;
      return frexpf(v, &exp);
    };
    auto fracD = [](double v) -> double {
      int exp = 0;
      return frexp(v, &exp);
    };

    return EvalUnaryIntrinsic(CI, fracF, fracD);
  } break;
  case IntrinsicOp::IOP_isnan: {
    Value *V = CI->getArgOperand(0);
    ConstantFP *fV = cast<ConstantFP>(V);
    bool isNan = fV->getValueAPF().isNaN();
    Constant *cNan = ConstantInt::get(CI->getType(), isNan ? 1 : 0);
    CI->replaceAllUsesWith(cNan);
    CI->eraseFromParent();
    return cNan;
  } break;
  default:
    return nullptr;
  }
}

static void SimpleTransformForHLDXIR(Instruction *I,
                                     std::vector<Instruction *> &deadInsts) {

  unsigned opcode = I->getOpcode();
  switch (opcode) {
  case Instruction::BitCast: {
    BitCastOperator *BCI = cast<BitCastOperator>(I);
    SimplifyBitCast(BCI, deadInsts);
  } break;
  case Instruction::Load: {
    LoadInst *ldInst = cast<LoadInst>(I);
    DXASSERT(!HLMatrixLower::IsMatrixType(ldInst->getType()),
                      "matrix load should use HL LdStMatrix");
    Value *Ptr = ldInst->getPointerOperand();
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(Ptr)) {
      if (BitCastOperator *BCO = dyn_cast<BitCastOperator>(CE)) {
        SimplifyBitCast(BCO, deadInsts);
      }
    }
  } break;
  case Instruction::Store: {
    StoreInst *stInst = cast<StoreInst>(I);
    Value *V = stInst->getValueOperand();
    DXASSERT_LOCALVAR(V, !HLMatrixLower::IsMatrixType(V->getType()),
                      "matrix store should use HL LdStMatrix");
    Value *Ptr = stInst->getPointerOperand();
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(Ptr)) {
      if (BitCastOperator *BCO = dyn_cast<BitCastOperator>(CE)) {
        SimplifyBitCast(BCO, deadInsts);
      }
    }
  } break;
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::Shl: {
    llvm::BinaryOperator *BO = cast<llvm::BinaryOperator>(I);
    Value *op2 = BO->getOperand(1);
    IntegerType *Ty = cast<IntegerType>(BO->getType()->getScalarType());
    unsigned bitWidth = Ty->getBitWidth();
    // Clamp op2 to 0 ~ bitWidth-1
    if (ConstantInt *cOp2 = dyn_cast<ConstantInt>(op2)) {
      unsigned iOp2 = cOp2->getLimitedValue();
      unsigned clampedOp2 = iOp2 & (bitWidth - 1);
      if (iOp2 != clampedOp2) {
        BO->setOperand(1, ConstantInt::get(op2->getType(), clampedOp2));
      }
    } else {
      Value *mask = ConstantInt::get(op2->getType(), bitWidth - 1);
      IRBuilder<> Builder(I);
      op2 = Builder.CreateAnd(op2, mask);
      BO->setOperand(1, op2);
    }
  } break;
  }
}

// Do simple transform to make later lower pass easier.
static void SimpleTransformForHLDXIR(llvm::Module *pM) {
  std::vector<Instruction *> deadInsts;
  for (Function &F : pM->functions()) {
    for (BasicBlock &BB : F.getBasicBlockList()) {
      for (BasicBlock::iterator Iter = BB.begin(); Iter != BB.end(); ) {
        Instruction *I = (Iter++);
        SimpleTransformForHLDXIR(I, deadInsts);
      }
    }
  }

  for (Instruction * I : deadInsts)
    I->dropAllReferences();
  for (Instruction * I : deadInsts)
    I->eraseFromParent();
  deadInsts.clear();

  for (GlobalVariable &GV : pM->globals()) {
    if (HLModule::IsStaticGlobal(&GV)) {
      for (User *U : GV.users()) {
        if (BitCastOperator *BCO = dyn_cast<BitCastOperator>(U)) {
          SimplifyBitCast(BCO, deadInsts);
        }
      }
    }
  }

  for (Instruction * I : deadInsts)
    I->dropAllReferences();
  for (Instruction * I : deadInsts)
    I->eraseFromParent();
}

void CGMSHLSLRuntime::FinishCodeGen() {
  SetEntryFunction();

  // If at this point we haven't determined the entry function it's an error.
  if (m_pHLModule->GetEntryFunction() == nullptr) {
    assert(CGM.getDiags().hasErrorOccurred() &&
           "else SetEntryFunction should have reported this condition");
    return;
  }

  // Remove all useless functions.
  if (!CGM.getCodeGenOpts().HLSLHighLevel) {
    Function *patchConstantFunc = nullptr;
    if (m_pHLModule->GetShaderModel()->IsHS()) {
      patchConstantFunc = m_pHLModule->GetHLFunctionProps(EntryFunc)
                              .ShaderProps.HS.patchConstantFunc;
    }

    std::unordered_set<Function *> DeadFuncSet;

    for (auto FIt = TheModule.functions().begin(),
              FE = TheModule.functions().end();
         FIt != FE;) {
      Function *F = FIt++;
      if (F != EntryFunc && F != patchConstantFunc && !F->isDeclaration()) {
        if (F->user_empty())
          F->eraseFromParent();
        else
          DeadFuncSet.insert(F);
      }
    }

    while (!DeadFuncSet.empty()) {
      bool noUpdate = true;
      for (auto FIt = DeadFuncSet.begin(), FE = DeadFuncSet.end(); FIt != FE;) {
        Function *F = *(FIt++);
        if (F->user_empty()) {
          DeadFuncSet.erase(F);
          F->eraseFromParent();
          noUpdate = false;
        }
      }
      // Avoid dead loop.
      if (noUpdate)
        break;
    }
  }

  // Create copy for clip plane.
  for (Function *F : clipPlaneFuncList) {
    HLFunctionProps &props = m_pHLModule->GetHLFunctionProps(F);
    IRBuilder<> Builder(F->getEntryBlock().getFirstInsertionPt());

    for (unsigned i = 0; i < DXIL::kNumClipPlanes; i++) {
      Value *clipPlane = props.ShaderProps.VS.clipPlanes[i];
      if (!clipPlane)
        continue;
      if (m_bDebugInfo) {
        Builder.SetCurrentDebugLocation(debugInfoMap[clipPlane]);
      }
      llvm::Type *Ty = clipPlane->getType()->getPointerElementType();
      // Constant *zeroInit = ConstantFP::get(Ty, 0);
      GlobalVariable *GV = new llvm::GlobalVariable(
          TheModule, Ty, /*IsConstant*/ false, // constant false to store.
          llvm::GlobalValue::ExternalLinkage,
          /*InitVal*/ nullptr, Twine("SV_ClipPlane") + Twine(i));
      Value *initVal = Builder.CreateLoad(clipPlane);
      Builder.CreateStore(initVal, GV);
      props.ShaderProps.VS.clipPlanes[i] = GV;
    }
  }

  // Allocate constant buffers.
  AllocateDxilConstantBuffers(m_pHLModule);
  // TODO: create temp variable for constant which has store use.

  // Create Global variable and type annotation for each CBuffer.
  ConstructCBuffer(m_pHLModule, CBufferType, m_ConstVarAnnotationMap);

  // add global call to entry func
  auto AddGlobalCall = [&](StringRef globalName, Instruction *InsertPt) {
    GlobalVariable *GV = TheModule.getGlobalVariable(globalName);
    if (GV) {
      if (ConstantArray *CA = dyn_cast<ConstantArray>(GV->getInitializer())) {

        IRBuilder<> Builder(InsertPt);
        for (User::op_iterator i = CA->op_begin(), e = CA->op_end(); i != e;
             ++i) {
          if (isa<ConstantAggregateZero>(*i))
            continue;
          ConstantStruct *CS = cast<ConstantStruct>(*i);
          if (isa<ConstantPointerNull>(CS->getOperand(1)))
            continue;

          // Must have a function or null ptr.
          if (!isa<Function>(CS->getOperand(1)))
            continue;
          Function *Ctor = cast<Function>(CS->getOperand(1));
          assert(Ctor->getReturnType()->isVoidTy() && Ctor->arg_size() == 0 &&
                 "function type must be void (void)");
          Builder.CreateCall(Ctor);
        }
        // remove the GV
        GV->eraseFromParent();
      }
    }
  };
  // need this for "llvm.global_dtors"?
  AddGlobalCall("llvm.global_ctors",
                EntryFunc->getEntryBlock().getFirstInsertionPt());

  // translate opcode into parameter for intrinsic functions
  AddOpcodeParamForIntrinsics(*m_pHLModule, m_IntrinsicMap, resMetadataMap);

  // Pin entry point and constant buffers, mark everything else internal.
  for (Function &f : m_pHLModule->GetModule()->functions()) {
    if (&f == m_pHLModule->GetEntryFunction() || IsPatchConstantFunction(&f) ||
        f.isDeclaration()) {
      f.setLinkage(GlobalValue::LinkageTypes::ExternalLinkage);
    } else {
      f.setLinkage(GlobalValue::LinkageTypes::InternalLinkage);
    }
    // Skip no inline functions.
    if (f.hasFnAttribute(llvm::Attribute::NoInline))
      continue;
    // Always inline.
    f.addFnAttr(llvm::Attribute::AlwaysInline);
  }

  // Do simple transform to make later lower pass easier.
  SimpleTransformForHLDXIR(m_pHLModule->GetModule());

  // Handle lang extensions if provided.
  if (CGM.getCodeGenOpts().HLSLExtensionsCodegen) {
    // Add semantic defines for extensions if any are available.
    HLSLExtensionsCodegenHelper::SemanticDefineErrorList errors =
      CGM.getCodeGenOpts().HLSLExtensionsCodegen->WriteSemanticDefines(m_pHLModule->GetModule());

    DiagnosticsEngine &Diags = CGM.getDiags();
    for (const HLSLExtensionsCodegenHelper::SemanticDefineError& error : errors) {
      DiagnosticsEngine::Level level = DiagnosticsEngine::Error;
      if (error.IsWarning())
        level = DiagnosticsEngine::Warning;
      unsigned DiagID = Diags.getCustomDiagID(level, "%0");
      Diags.Report(SourceLocation::getFromRawEncoding(error.Location()), DiagID) << error.Message();
    }

    // Add root signature from a #define. Overrides root signature in function attribute.
    {
      using Status = HLSLExtensionsCodegenHelper::CustomRootSignature::Status;
      HLSLExtensionsCodegenHelper::CustomRootSignature customRootSig;
      Status status = CGM.getCodeGenOpts().HLSLExtensionsCodegen->GetCustomRootSignature(&customRootSig);
      if (status == Status::FOUND) {
          CompileRootSignature(customRootSig.RootSignature, Diags,
                               SourceLocation::getFromRawEncoding(customRootSig.EncodedSourceLocation),
                               rootSigVer, &m_pHLModule->GetRootSignature());
      }
    }
  }

}

RValue CGMSHLSLRuntime::EmitHLSLBuiltinCallExpr(CodeGenFunction &CGF,
                                                const FunctionDecl *FD,
                                                const CallExpr *E,
                                                ReturnValueSlot ReturnValue) {
  StringRef name = FD->getName();

  const Decl *TargetDecl = E->getCalleeDecl();
  llvm::Value *Callee = CGF.EmitScalarExpr(E->getCallee());
  RValue RV = CGF.EmitCall(E->getCallee()->getType(), Callee, E, ReturnValue,
                      TargetDecl);
  if (RV.isScalar() && RV.getScalarVal() != nullptr) {
    if (CallInst *CI = dyn_cast<CallInst>(RV.getScalarVal())) {
      Function *F = CI->getCalledFunction();
      HLOpcodeGroup group = hlsl::GetHLOpcodeGroup(F);
      if (group == HLOpcodeGroup::HLIntrinsic) {
        bool allOperandImm = true;
        for (auto &operand : CI->arg_operands()) {
          bool isImm = isa<ConstantInt>(operand) || isa<ConstantFP>(operand);
          if (!isImm) {
            allOperandImm = false;
            break;
          }
        }
        if (allOperandImm) {
          unsigned intrinsicOpcode;
          StringRef intrinsicGroup;
          hlsl::GetIntrinsicOp(FD, intrinsicOpcode, intrinsicGroup);
          IntrinsicOp opcode = static_cast<IntrinsicOp>(intrinsicOpcode);
          if (Value *Result = TryEvalIntrinsic(CI, opcode)) {
            RV = RValue::get(Result);
          }
        }
      }
    }
  }
  return RV;
}

static HLOpcodeGroup GetHLOpcodeGroup(const clang::Stmt::StmtClass stmtClass) {
  switch (stmtClass) {
  case Stmt::CStyleCastExprClass:
  case Stmt::ImplicitCastExprClass:
  case Stmt::CXXFunctionalCastExprClass:
    return HLOpcodeGroup::HLCast;
  case Stmt::InitListExprClass:
    return HLOpcodeGroup::HLInit;
  case Stmt::BinaryOperatorClass:
  case Stmt::CompoundAssignOperatorClass:
    return HLOpcodeGroup::HLBinOp;
  case Stmt::UnaryOperatorClass:
    return HLOpcodeGroup::HLUnOp;
  case Stmt::ExtMatrixElementExprClass:
    return HLOpcodeGroup::HLSubscript;
  case Stmt::CallExprClass:
    return HLOpcodeGroup::HLIntrinsic;
  case Stmt::ConditionalOperatorClass:
    return HLOpcodeGroup::HLSelect;
  default:
    llvm_unreachable("not support operation");
  }
}

// NOTE: This table must match BinaryOperator::Opcode
static const HLBinaryOpcode BinaryOperatorKindMap[] = {
    HLBinaryOpcode::Invalid, // PtrMemD
    HLBinaryOpcode::Invalid, // PtrMemI
    HLBinaryOpcode::Mul, HLBinaryOpcode::Div, HLBinaryOpcode::Rem,
    HLBinaryOpcode::Add, HLBinaryOpcode::Sub, HLBinaryOpcode::Shl,
    HLBinaryOpcode::Shr, HLBinaryOpcode::LT, HLBinaryOpcode::GT,
    HLBinaryOpcode::LE, HLBinaryOpcode::GE, HLBinaryOpcode::EQ,
    HLBinaryOpcode::NE, HLBinaryOpcode::And, HLBinaryOpcode::Xor,
    HLBinaryOpcode::Or, HLBinaryOpcode::LAnd, HLBinaryOpcode::LOr,
    HLBinaryOpcode::Invalid, // Assign,
    // The assign part is done by matrix store
    HLBinaryOpcode::Mul,     // MulAssign
    HLBinaryOpcode::Div,     // DivAssign
    HLBinaryOpcode::Rem,     // RemAssign
    HLBinaryOpcode::Add,     // AddAssign
    HLBinaryOpcode::Sub,     // SubAssign
    HLBinaryOpcode::Shl,     // ShlAssign
    HLBinaryOpcode::Shr,     // ShrAssign
    HLBinaryOpcode::And,     // AndAssign
    HLBinaryOpcode::Xor,     // XorAssign
    HLBinaryOpcode::Or,      // OrAssign
    HLBinaryOpcode::Invalid, // Comma
};

// NOTE: This table must match UnaryOperator::Opcode
static const HLUnaryOpcode UnaryOperatorKindMap[] = {
    HLUnaryOpcode::PostInc, HLUnaryOpcode::PostDec,
    HLUnaryOpcode::PreInc,  HLUnaryOpcode::PreDec,
    HLUnaryOpcode::Invalid, // AddrOf,
    HLUnaryOpcode::Invalid, // Deref,
    HLUnaryOpcode::Plus,    HLUnaryOpcode::Minus,
    HLUnaryOpcode::Not,     HLUnaryOpcode::LNot,
    HLUnaryOpcode::Invalid, // Real,
    HLUnaryOpcode::Invalid, // Imag,
    HLUnaryOpcode::Invalid, // Extension
};

static bool IsRowMajorMatrix(QualType Ty, bool bDefaultRowMajor) {
  if (const AttributedType *AT = Ty->getAs<AttributedType>()) {
    if (AT->getAttrKind() == AttributedType::attr_hlsl_row_major)
      return true;
    else if (AT->getAttrKind() == AttributedType::attr_hlsl_column_major)
      return false;
    else
      return bDefaultRowMajor;
  } else {
    return bDefaultRowMajor;
  }
}

static bool IsUnsigned(QualType Ty) {
  Ty = Ty.getCanonicalType().getNonReferenceType();

  if (hlsl::IsHLSLVecMatType(Ty))
    Ty = CGHLSLRuntime::GetHLSLVecMatElementType(Ty);

  if (Ty->isExtVectorType())
    Ty = Ty->getAs<clang::ExtVectorType>()->getElementType();

  return Ty->isUnsignedIntegerType();
}

static unsigned GetHLOpcode(const Expr *E) {
  switch (E->getStmtClass()) {
  case Stmt::CompoundAssignOperatorClass:
  case Stmt::BinaryOperatorClass: {
    const clang::BinaryOperator *binOp = cast<clang::BinaryOperator>(E);
    HLBinaryOpcode binOpcode = BinaryOperatorKindMap[binOp->getOpcode()];
    if (HasUnsignedOpcode(binOpcode)) {
      if (IsUnsigned(binOp->getLHS()->getType())) {
        binOpcode = GetUnsignedOpcode(binOpcode);
      }
    }
    return static_cast<unsigned>(binOpcode);
  }
  case Stmt::UnaryOperatorClass: {
    const UnaryOperator *unOp = cast<clang::UnaryOperator>(E);
    HLUnaryOpcode unOpcode = UnaryOperatorKindMap[unOp->getOpcode()];
    return static_cast<unsigned>(unOpcode);
  }
  case Stmt::ImplicitCastExprClass:
  case Stmt::CStyleCastExprClass: {
    const CastExpr *CE = cast<CastExpr>(E);
    bool toUnsigned = IsUnsigned(E->getType());
    bool fromUnsigned = IsUnsigned(CE->getSubExpr()->getType());
    if (toUnsigned && fromUnsigned)
      return static_cast<unsigned>(HLCastOpcode::UnsignedUnsignedCast);
    else if (toUnsigned)
      return static_cast<unsigned>(HLCastOpcode::ToUnsignedCast);
    else if (fromUnsigned)
      return static_cast<unsigned>(HLCastOpcode::FromUnsignedCast);
    else
      return static_cast<unsigned>(HLCastOpcode::DefaultCast);
  }
  default:
    return 0;
  }
}

static Value *
EmitHLSLMatrixOperationCallImp(CGBuilderTy &Builder, HLOpcodeGroup group,
                               unsigned opcode, llvm::Type *RetType,
                               ArrayRef<Value *> paramList, llvm::Module &M) {
  SmallVector<llvm::Type *, 4> paramTyList;
  // Add the opcode param
  llvm::Type *opcodeTy = llvm::Type::getInt32Ty(M.getContext());
  paramTyList.emplace_back(opcodeTy);
  for (Value *param : paramList) {
    paramTyList.emplace_back(param->getType());
  }

  llvm::FunctionType *funcTy =
      llvm::FunctionType::get(RetType, paramTyList, false);

  Function *opFunc = GetOrCreateHLFunction(M, funcTy, group, opcode);

  SmallVector<Value *, 4> opcodeParamList;
  Value *opcodeConst = Constant::getIntegerValue(opcodeTy, APInt(32, opcode));
  opcodeParamList.emplace_back(opcodeConst);
  opcodeParamList.append(paramList.begin(), paramList.end());

  return Builder.CreateCall(opFunc, opcodeParamList);
}

static Value *EmitHLSLArrayInit(CGBuilderTy &Builder, HLOpcodeGroup group,
                                unsigned opcode, llvm::Type *RetType,
                                ArrayRef<Value *> paramList, llvm::Module &M) {
  // It's a matrix init.
  if (!RetType->isVoidTy())
    return EmitHLSLMatrixOperationCallImp(Builder, group, opcode, RetType,
                                          paramList, M);
  Value *arrayPtr = paramList[0];
  llvm::ArrayType *AT =
      cast<llvm::ArrayType>(arrayPtr->getType()->getPointerElementType());
  // Avoid the arrayPtr.
  unsigned paramSize = paramList.size() - 1;
  // Support simple case here.
  if (paramSize == AT->getArrayNumElements()) {
    bool typeMatch = true;
    llvm::Type *EltTy = AT->getArrayElementType();
    if (EltTy->isAggregateType()) {
      // Aggregate Type use pointer in initList.
      EltTy = llvm::PointerType::get(EltTy, 0);
    }
    for (unsigned i = 1; i < paramList.size(); i++) {
      if (paramList[i]->getType() != EltTy) {
        typeMatch = false;
        break;
      }
    }
    // Both size and type match.
    if (typeMatch) {
      bool isPtr = EltTy->isPointerTy();
      llvm::Type *i32Ty = llvm::Type::getInt32Ty(EltTy->getContext());
      Constant *zero = ConstantInt::get(i32Ty, 0);

      for (unsigned i = 1; i < paramList.size(); i++) {
        Constant *idx = ConstantInt::get(i32Ty, i - 1);
        Value *GEP = Builder.CreateInBoundsGEP(arrayPtr, {zero, idx});
        Value *Elt = paramList[i];

        if (isPtr) {
          Elt = Builder.CreateLoad(Elt);
        }

        Builder.CreateStore(Elt, GEP);
      }
      // The return value will not be used.
      return nullptr;
    }
  }
  // Other case will be lowered in later pass.
  return EmitHLSLMatrixOperationCallImp(Builder, group, opcode, RetType,
                                        paramList, M);
}

void CGMSHLSLRuntime::FlattenValToInitList(CodeGenFunction &CGF, SmallVector<Value *, 4> &elts,
                                      SmallVector<QualType, 4> &eltTys,
                                      QualType Ty, Value *val) {
  CGBuilderTy &Builder = CGF.Builder;
  llvm::Type *valTy = val->getType();

  if (valTy->isPointerTy()) {
    llvm::Type *valEltTy = valTy->getPointerElementType();
    if (valEltTy->isVectorTy() || 
        valEltTy->isSingleValueType()) {
      Value *ldVal = Builder.CreateLoad(val);
      FlattenValToInitList(CGF, elts, eltTys, Ty, ldVal);
    } else if (HLMatrixLower::IsMatrixType(valEltTy)) {
      Value *ldVal = EmitHLSLMatrixLoad(Builder, val, Ty);
      FlattenValToInitList(CGF, elts, eltTys, Ty, ldVal);
    } else {
      llvm::Type *i32Ty = llvm::Type::getInt32Ty(valTy->getContext());
      Value *zero = ConstantInt::get(i32Ty, 0);
      if (llvm::ArrayType *AT = dyn_cast<llvm::ArrayType>(valEltTy)) {
        QualType EltTy = Ty->getAsArrayTypeUnsafe()->getElementType();
        for (unsigned i = 0; i < AT->getArrayNumElements(); i++) {
          Value *gepIdx = ConstantInt::get(i32Ty, i);
          Value *EltPtr = Builder.CreateInBoundsGEP(val, {zero, gepIdx});
          FlattenValToInitList(CGF, elts, eltTys, EltTy,EltPtr);
        }
      } else {
        // Struct.
        StructType *ST = cast<StructType>(valEltTy);
        if (HLModule::IsHLSLObjectType(ST)) {
          // Save object directly like basic type.
          elts.emplace_back(Builder.CreateLoad(val));
          eltTys.emplace_back(Ty);
        } else {
          RecordDecl *RD = Ty->getAsStructureType()->getDecl();
          const CGRecordLayout& RL = CGF.getTypes().getCGRecordLayout(RD);

          // Take care base.
          if (const CXXRecordDecl *CXXRD = dyn_cast<CXXRecordDecl>(RD)) {
            if (CXXRD->getNumBases()) {
              for (const auto &I : CXXRD->bases()) {
                const CXXRecordDecl *BaseDecl = cast<CXXRecordDecl>(
                    I.getType()->castAs<RecordType>()->getDecl());
                if (BaseDecl->field_empty())
                  continue;
                QualType parentTy = QualType(BaseDecl->getTypeForDecl(), 0);
                unsigned i = RL.getNonVirtualBaseLLVMFieldNo(BaseDecl);
                Value *gepIdx = ConstantInt::get(i32Ty, i);
                Value *EltPtr = Builder.CreateInBoundsGEP(val, {zero, gepIdx});
                FlattenValToInitList(CGF, elts, eltTys, parentTy, EltPtr);
              }
            }
          }

          for (auto fieldIter = RD->field_begin(), fieldEnd = RD->field_end();
               fieldIter != fieldEnd; ++fieldIter) {
            unsigned i = RL.getLLVMFieldNo(*fieldIter);
            Value *gepIdx = ConstantInt::get(i32Ty, i);
            Value *EltPtr = Builder.CreateInBoundsGEP(val, {zero, gepIdx});
            FlattenValToInitList(CGF, elts, eltTys, fieldIter->getType(), EltPtr);
          }
        }
      }
    }
  } else {
    if (HLMatrixLower::IsMatrixType(valTy)) {
      unsigned col, row;
      llvm::Type *EltTy = HLMatrixLower::GetMatrixInfo(valTy, col, row);
      // All matrix Value should be row major.
      // Init list is row major in scalar.
      // So the order is match here, just cast to vector.
      unsigned matSize = col * row;
      bool isRowMajor = IsRowMajorMatrix(Ty, m_pHLModule->GetHLOptions().bDefaultRowMajor);

      HLCastOpcode opcode = isRowMajor ? HLCastOpcode::RowMatrixToVecCast
                                       : HLCastOpcode::ColMatrixToVecCast;
      // Cast to vector.
      val = EmitHLSLMatrixOperationCallImp(
          Builder, HLOpcodeGroup::HLCast,
          static_cast<unsigned>(opcode),
          llvm::VectorType::get(EltTy, matSize), {val}, TheModule);
      valTy = val->getType();
    }

    if (valTy->isVectorTy()) {
      QualType EltTy = GetHLSLVecMatElementType(Ty);
      unsigned vecSize = valTy->getVectorNumElements();
      for (unsigned i = 0; i < vecSize; i++) {
        Value *Elt = Builder.CreateExtractElement(val, i);
        elts.emplace_back(Elt);
        eltTys.emplace_back(EltTy);
      }
    } else {
      DXASSERT(valTy->isSingleValueType(), "must be single value type here");
      elts.emplace_back(val);
      eltTys.emplace_back(Ty);
    }
  }  
}

// Cast elements in initlist if not match the target type.
// idx is current element index in initlist, Ty is target type.
static void AddMissingCastOpsInInitList(SmallVector<Value *, 4> &elts, SmallVector<QualType, 4> &eltTys, unsigned &idx, QualType Ty, CodeGenFunction &CGF) {
  if (Ty->isArrayType()) {
    const clang::ArrayType *AT = Ty->getAsArrayTypeUnsafe();
    // Must be ConstantArrayType here.
    unsigned arraySize = cast<ConstantArrayType>(AT)->getSize().getLimitedValue();
    QualType EltTy = AT->getElementType();
    for (unsigned i = 0; i < arraySize; i++)
      AddMissingCastOpsInInitList(elts, eltTys, idx, EltTy, CGF);
  } else if (IsHLSLVecType(Ty)) {
    QualType EltTy = GetHLSLVecElementType(Ty);
    unsigned vecSize = GetHLSLVecSize(Ty);
    for (unsigned i=0;i< vecSize;i++)
      AddMissingCastOpsInInitList(elts, eltTys, idx, EltTy, CGF);
  } else if (IsHLSLMatType(Ty)) {
    QualType EltTy = GetHLSLMatElementType(Ty);
    unsigned row, col;
    GetHLSLMatRowColCount(Ty, row, col);
    unsigned matSize = row*col;
    for (unsigned i = 0; i < matSize; i++)
      AddMissingCastOpsInInitList(elts, eltTys, idx, EltTy, CGF);
  } else if (Ty->isRecordType()) {
    if (HLModule::IsHLSLObjectType(CGF.ConvertType(Ty))) {
      // Skip hlsl object.
      idx++;
    } else {
      const RecordType *RT = Ty->getAsStructureType();
      // For CXXRecord.
      if (!RT)
        RT = Ty->getAs<RecordType>();
      RecordDecl *RD = RT->getDecl();
      // Take care base.
      if (const CXXRecordDecl *CXXRD = dyn_cast<CXXRecordDecl>(RD)) {
        if (CXXRD->getNumBases()) {
          for (const auto &I : CXXRD->bases()) {
            const CXXRecordDecl *BaseDecl = cast<CXXRecordDecl>(
                I.getType()->castAs<RecordType>()->getDecl());
            if (BaseDecl->field_empty())
              continue;
            QualType parentTy = QualType(BaseDecl->getTypeForDecl(), 0);
            AddMissingCastOpsInInitList(elts, eltTys, idx, parentTy, CGF);
          }
        }
      }
      for (FieldDecl *field : RD->fields())
        AddMissingCastOpsInInitList(elts, eltTys, idx, field->getType(), CGF);
    }
  }
  else {
    // Basic type.
    Value *val = elts[idx];
    llvm::Type *srcTy = val->getType();
    llvm::Type *dstTy = CGF.ConvertType(Ty);
    if (srcTy != dstTy) {
      Instruction::CastOps castOp =
          static_cast<Instruction::CastOps>(HLModule::FindCastOp(
              IsUnsigned(eltTys[idx]), IsUnsigned(Ty), srcTy, dstTy));
      elts[idx] = CGF.Builder.CreateCast(castOp, val, dstTy);
    }
    idx++;
  }
}

static void StoreInitListToDestPtr(Value *DestPtr,
                                   SmallVector<Value *, 4> &elts, unsigned &idx,
                                   QualType Type, CodeGenTypes &Types, bool bDefaultRowMajor,
                                   CGBuilderTy &Builder, llvm::Module &M) {
  llvm::Type *Ty = DestPtr->getType()->getPointerElementType();
  llvm::Type *i32Ty = llvm::Type::getInt32Ty(Ty->getContext());

  if (Ty->isVectorTy()) {
    Value *Result = UndefValue::get(Ty);
    for (unsigned i = 0; i < Ty->getVectorNumElements(); i++)
      Result = Builder.CreateInsertElement(Result, elts[idx + i], i);
    Builder.CreateStore(Result, DestPtr);
    idx += Ty->getVectorNumElements();
  } else if (HLMatrixLower::IsMatrixType(Ty)) {
    bool isRowMajor =
        IsRowMajorMatrix(Type, bDefaultRowMajor);

    unsigned row, col;
    HLMatrixLower::GetMatrixInfo(Ty, col, row);
    std::vector<Value *> matInitList(col * row);
    for (unsigned i = 0; i < col; i++) {
      for (unsigned r = 0; r < row; r++) {
        unsigned matIdx = i * row + r;
        matInitList[matIdx] = elts[idx + matIdx];
      }
    }
    idx += row * col;
    Value *matVal =
        EmitHLSLMatrixOperationCallImp(Builder, HLOpcodeGroup::HLInit,
                                       /*opcode*/ 0, Ty, matInitList, M);
    // matVal return from HLInit is row major.
    // If DestPtr is row major, just store it directly.
    if (!isRowMajor) {
      // ColMatStore need a col major value.
      // Cast row major matrix into col major.
      // Then store it.
      Value *colMatVal = EmitHLSLMatrixOperationCallImp(
          Builder, HLOpcodeGroup::HLCast,
          static_cast<unsigned>(HLCastOpcode::RowMatrixToColMatrix), Ty,
          {matVal}, M);
      EmitHLSLMatrixOperationCallImp(
          Builder, HLOpcodeGroup::HLMatLoadStore,
          static_cast<unsigned>(HLMatLoadStoreOpcode::ColMatStore), Ty,
          {DestPtr, colMatVal}, M);
    } else {
      EmitHLSLMatrixOperationCallImp(
          Builder, HLOpcodeGroup::HLMatLoadStore,
          static_cast<unsigned>(HLMatLoadStoreOpcode::RowMatStore), Ty,
          {DestPtr, matVal}, M);
    }
  } else if (Ty->isStructTy()) {
    if (HLModule::IsHLSLObjectType(Ty)) {
      Builder.CreateStore(elts[idx], DestPtr);
      idx++;
    } else {
      Constant *zero = ConstantInt::get(i32Ty, 0);

      const RecordType *RT = Type->getAsStructureType();
      // For CXXRecord.
      if (!RT)
        RT = Type->getAs<RecordType>();
      RecordDecl *RD = RT->getDecl();
      const CGRecordLayout &RL = Types.getCGRecordLayout(RD);
      // Take care base.
      if (const CXXRecordDecl *CXXRD = dyn_cast<CXXRecordDecl>(RD)) {
        if (CXXRD->getNumBases()) {
          for (const auto &I : CXXRD->bases()) {
            const CXXRecordDecl *BaseDecl = cast<CXXRecordDecl>(
                I.getType()->castAs<RecordType>()->getDecl());
            if (BaseDecl->field_empty())
              continue;
            QualType parentTy = QualType(BaseDecl->getTypeForDecl(), 0);
            unsigned i = RL.getNonVirtualBaseLLVMFieldNo(BaseDecl);
            Constant *gepIdx = ConstantInt::get(i32Ty, i);
            Value *GEP = Builder.CreateInBoundsGEP(DestPtr, {zero, gepIdx});
            StoreInitListToDestPtr(GEP, elts, idx, parentTy, Types,
                                   bDefaultRowMajor, Builder, M);
          }
        }
      }
      for (FieldDecl *field : RD->fields()) {
        unsigned i = RL.getLLVMFieldNo(field);
        Constant *gepIdx = ConstantInt::get(i32Ty, i);
        Value *GEP = Builder.CreateInBoundsGEP(DestPtr, {zero, gepIdx});
        StoreInitListToDestPtr(GEP, elts, idx, field->getType(), Types,
                               bDefaultRowMajor, Builder, M);
      }
    }
  } else if (Ty->isArrayTy()) {
    Constant *zero = ConstantInt::get(i32Ty, 0);
    QualType EltType = Type->getAsArrayTypeUnsafe()->getElementType();
    for (unsigned i = 0; i < Ty->getArrayNumElements(); i++) {
      Constant *gepIdx = ConstantInt::get(i32Ty, i);
      Value *GEP = Builder.CreateInBoundsGEP(DestPtr, {zero, gepIdx});
      StoreInitListToDestPtr(GEP, elts, idx, EltType, Types, bDefaultRowMajor,
                             Builder, M);
    }
  } else {
    DXASSERT(Ty->isSingleValueType(), "invalid type");
    llvm::Type *i1Ty = Builder.getInt1Ty();
    Value *V = elts[idx];
    if (V->getType() == i1Ty &&
        DestPtr->getType()->getPointerElementType() != i1Ty) {
      V = Builder.CreateZExt(V, DestPtr->getType()->getPointerElementType());
    }
    Builder.CreateStore(V, DestPtr);
    idx++;
  }
}

void CGMSHLSLRuntime::ScanInitList(CodeGenFunction &CGF, InitListExpr *E,
                                   SmallVector<Value *, 4> &EltValList,
                                   SmallVector<QualType, 4> &EltTyList) {
  unsigned NumInitElements = E->getNumInits();
  for (unsigned i = 0; i != NumInitElements; ++i) {
    Expr *init = E->getInit(i);
    QualType iType = init->getType();
    if (InitListExpr *initList = dyn_cast<InitListExpr>(init)) {
      ScanInitList(CGF, initList, EltValList, EltTyList);
    } else if (CodeGenFunction::hasScalarEvaluationKind(iType)) {
      llvm::Value *initVal = CGF.EmitScalarExpr(init);
      FlattenValToInitList(CGF, EltValList, EltTyList, iType, initVal);
    } else {
      AggValueSlot Slot =
          CGF.CreateAggTemp(init->getType(), "Agg.InitList.tmp");
      CGF.EmitAggExpr(init, Slot);
      llvm::Value *aggPtr = Slot.getAddr();
      FlattenValToInitList(CGF, EltValList, EltTyList, iType, aggPtr);
    }

  }
}
// Is Type of E match Ty.
static bool ExpTypeMatch(Expr *E, QualType Ty, ASTContext &Ctx, CodeGenTypes &Types) {
  if (InitListExpr *initList = dyn_cast<InitListExpr>(E)) {
    unsigned NumInitElements = initList->getNumInits();

    // Skip vector and matrix type.
    if (Ty->isVectorType())
      return false;
    if (hlsl::IsHLSLVecMatType(Ty))
      return false;

    if (Ty->isStructureOrClassType()) {
      RecordDecl *record = Ty->castAs<RecordType>()->getDecl();
      bool bMatch = true;
      auto It = record->field_begin();
      auto ItEnd = record->field_end();
      unsigned i = 0;
      for (auto it = record->field_begin(), end = record->field_end();
           it != end; it++) {
        if (i == NumInitElements) {
          bMatch = false;
          break;
        }
        Expr *init = initList->getInit(i++);
        QualType EltTy = it->getType();
        bMatch &= ExpTypeMatch(init, EltTy, Ctx, Types);
        if (!bMatch)
          break;
      }
      bMatch &= i == NumInitElements;
      if (bMatch && initList->getType()->isVoidType()) {
        initList->setType(Ty);
      }
      return bMatch;
    } else if (Ty->isArrayType() && !Ty->isIncompleteArrayType()) {
      const ConstantArrayType *AT = Ctx.getAsConstantArrayType(Ty);
      QualType EltTy = AT->getElementType();
      unsigned size = AT->getSize().getZExtValue();

      if (size != NumInitElements)
        return false;

      bool bMatch = true;
      for (unsigned i = 0; i != NumInitElements; ++i) {
        Expr *init = initList->getInit(i);
        bMatch &= ExpTypeMatch(init, EltTy, Ctx, Types);
        if (!bMatch)
          break;
      }
      if (bMatch && initList->getType()->isVoidType()) {
        initList->setType(Ty);
      }
      return bMatch;
    } else {
      return false;
    }
  } else {
    llvm::Type *ExpTy = Types.ConvertType(E->getType());
    llvm::Type *TargetTy = Types.ConvertType(Ty);
    return ExpTy == TargetTy;
  }
}

bool CGMSHLSLRuntime::IsTrivalInitListExpr(CodeGenFunction &CGF,
                                           InitListExpr *E) {
  QualType Ty = E->getType();
  return ExpTypeMatch(E, Ty, CGF.getContext(), CGF.getTypes());
}

Value *CGMSHLSLRuntime::EmitHLSLInitListExpr(CodeGenFunction &CGF, InitListExpr *E,
      // The destPtr when emiting aggregate init, for normal case, it will be null.
      Value *DestPtr) {
  if (DestPtr && E->getNumInits() == 1) {
    llvm::Type *ExpTy = CGF.ConvertType(E->getType());
    llvm::Type *TargetTy = CGF.ConvertType(E->getInit(0)->getType());
    if (ExpTy == TargetTy) {
      Expr *Expr = E->getInit(0);
      LValue LV = CGF.EmitLValue(Expr);
      if (LV.isSimple()) {
        Value *SrcPtr = LV.getAddress();
        SmallVector<Value *, 4> idxList;
        EmitHLSLAggregateCopy(CGF, SrcPtr, DestPtr, idxList, Expr->getType(),
                              E->getType(), SrcPtr->getType());
        return nullptr;
      }
    }
  }

  SmallVector<Value *, 4> EltValList;
  SmallVector<QualType, 4> EltTyList;
  
  ScanInitList(CGF, E, EltValList, EltTyList);
  
  QualType ResultTy = E->getType();
  unsigned idx = 0;
  // Create cast if need.
  AddMissingCastOpsInInitList(EltValList, EltTyList, idx, ResultTy, CGF);
  DXASSERT(idx == EltValList.size(), "size must match");

  llvm::Type *RetTy = CGF.ConvertType(ResultTy);
  if (DestPtr) {
    SmallVector<Value *, 4> ParamList;
    DXASSERT(RetTy->isAggregateType(), "");
    ParamList.emplace_back(DestPtr);
    ParamList.append(EltValList.begin(), EltValList.end());
    idx = 0;
    bool bDefaultRowMajor = m_pHLModule->GetHLOptions().bDefaultRowMajor;
    StoreInitListToDestPtr(DestPtr, EltValList, idx, ResultTy, CGF.getTypes(),
                           bDefaultRowMajor, CGF.Builder, TheModule);
    return nullptr;
  }

  if (IsHLSLVecType(ResultTy)) {
    Value *Result = UndefValue::get(RetTy);
    for (unsigned i = 0; i < RetTy->getVectorNumElements(); i++)
      Result = CGF.Builder.CreateInsertElement(Result, EltValList[i], i);
    return Result;
  } else {
    // Must be matrix here.
    DXASSERT(IsHLSLMatType(ResultTy), "must be matrix type here.");
    return EmitHLSLMatrixOperationCallImp(CGF.Builder, HLOpcodeGroup::HLInit,
                                          /*opcode*/ 0, RetTy, EltValList,
                                          TheModule);
  }
}

static void FlatConstToList(Constant *C, SmallVector<Constant *, 4> &EltValList,
                            QualType Type, CodeGenTypes &Types,
                            bool bDefaultRowMajor) {
  llvm::Type *Ty = C->getType();
  if (llvm::VectorType *VT = dyn_cast<llvm::VectorType>(Ty)) {
    // Type is only for matrix. Keep use Type to next level.
    for (unsigned i = 0; i < VT->getNumElements(); i++) {
      FlatConstToList(C->getAggregateElement(i), EltValList, Type, Types,
                      bDefaultRowMajor);
    }
  } else if (HLMatrixLower::IsMatrixType(Ty)) {
    bool isRowMajor = IsRowMajorMatrix(Type, bDefaultRowMajor);
    // matrix type is struct { vector<Ty, row> [col] };
    // Strip the struct level here.
    Constant *matVal = C->getAggregateElement((unsigned)0);
    const RecordType *RT = Type->getAs<RecordType>();
    RecordDecl *RD = RT->getDecl();
    QualType EltTy = RD->field_begin()->getType();
    // When scan, init list scalars is row major.
    if (isRowMajor) {
      // Don't change the major for row major value.
      FlatConstToList(matVal, EltValList, EltTy, Types, bDefaultRowMajor);
    } else {
      // Save to tmp list.
      SmallVector<Constant *, 4> matEltList;
      FlatConstToList(matVal, matEltList, EltTy, Types, bDefaultRowMajor);
      unsigned row, col;
      HLMatrixLower::GetMatrixInfo(Ty, col, row);
      // Change col major value to row major.
      for (unsigned r = 0; r < row; r++)
        for (unsigned c = 0; c < col; c++) {
          unsigned colMajorIdx = c * row + r;
          EltValList.emplace_back(matEltList[colMajorIdx]);
        }
    }
  } else if (llvm::ArrayType *AT = dyn_cast<llvm::ArrayType>(Ty)) {
    QualType EltTy = Type->getAsArrayTypeUnsafe()->getElementType();
    for (unsigned i = 0; i < AT->getNumElements(); i++) {
      FlatConstToList(C->getAggregateElement(i), EltValList, EltTy, Types,
                      bDefaultRowMajor);
    }
  } else if (llvm::StructType *ST = dyn_cast<llvm::StructType>(Ty)) {
    RecordDecl *RD = Type->getAsStructureType()->getDecl();
    const CGRecordLayout &RL = Types.getCGRecordLayout(RD);
    // Take care base.
    if (const CXXRecordDecl *CXXRD = dyn_cast<CXXRecordDecl>(RD)) {
      if (CXXRD->getNumBases()) {
        for (const auto &I : CXXRD->bases()) {
          const CXXRecordDecl *BaseDecl =
              cast<CXXRecordDecl>(I.getType()->castAs<RecordType>()->getDecl());
          if (BaseDecl->field_empty())
            continue;
          QualType parentTy = QualType(BaseDecl->getTypeForDecl(), 0);
          unsigned i = RL.getNonVirtualBaseLLVMFieldNo(BaseDecl);
          FlatConstToList(C->getAggregateElement(i), EltValList, parentTy,
                          Types, bDefaultRowMajor);
        }
      }
    }

    for (auto fieldIter = RD->field_begin(), fieldEnd = RD->field_end();
         fieldIter != fieldEnd; ++fieldIter) {
      unsigned i = RL.getLLVMFieldNo(*fieldIter);

      FlatConstToList(C->getAggregateElement(i), EltValList,
                      fieldIter->getType(), Types, bDefaultRowMajor);
    }
  } else {
    EltValList.emplace_back(C);
  }
}

static bool ScanConstInitList(CodeGenModule &CGM, InitListExpr *E,
                              SmallVector<Constant *, 4> &EltValList,
                              CodeGenTypes &Types, bool bDefaultRowMajor) {
  unsigned NumInitElements = E->getNumInits();
  for (unsigned i = 0; i != NumInitElements; ++i) {
    Expr *init = E->getInit(i);
    QualType iType = init->getType();
    if (InitListExpr *initList = dyn_cast<InitListExpr>(init)) {
      if (!ScanConstInitList(CGM, initList, EltValList, Types,
                             bDefaultRowMajor))
        return false;
    } else if (DeclRefExpr *ref = dyn_cast<DeclRefExpr>(init)) {
      if (VarDecl *D = dyn_cast<VarDecl>(ref->getDecl())) {
        if (!D->hasInit())
          return false;
        if (Constant *initVal = CGM.EmitConstantInit(*D)) {
          FlatConstToList(initVal, EltValList, iType, Types, bDefaultRowMajor);
        } else {
          return false;
        }
      } else {
        return false;
      }
    } else if (hlsl::IsHLSLMatType(iType)) {
      return false;
    } else if (CodeGenFunction::hasScalarEvaluationKind(iType)) {
      if (Constant *initVal = CGM.EmitConstantExpr(init, iType)) {
        FlatConstToList(initVal, EltValList, iType, Types, bDefaultRowMajor);
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

static Constant *BuildConstInitializer(QualType Type, unsigned &offset,
                                       SmallVector<Constant *, 4> &EltValList,
                                       CodeGenTypes &Types,
                                       bool bDefaultRowMajor);

static Constant *BuildConstVector(llvm::VectorType *VT, unsigned &offset,
                                  SmallVector<Constant *, 4> &EltValList,
                                  QualType Type, CodeGenTypes &Types) {
  SmallVector<Constant *, 4> Elts;
  QualType EltTy = hlsl::GetHLSLVecElementType(Type);
  for (unsigned i = 0; i < VT->getNumElements(); i++) {
    Elts.emplace_back(BuildConstInitializer(EltTy, offset, EltValList, Types,
                                            // Vector don't need major.
                                            /*bDefaultRowMajor*/ false));
  }
  return llvm::ConstantVector::get(Elts);
}

static Constant *BuildConstMatrix(llvm::Type *Ty, unsigned &offset,
                                  SmallVector<Constant *, 4> &EltValList,
                                  QualType Type, CodeGenTypes &Types,
                                  bool bDefaultRowMajor) {
  QualType EltTy = hlsl::GetHLSLMatElementType(Type);
  unsigned col, row;
  HLMatrixLower::GetMatrixInfo(Ty, col, row);
  llvm::ArrayType *AT = cast<llvm::ArrayType>(Ty->getStructElementType(0));
  // Save initializer elements first.
  // Matrix initializer is row major.
  SmallVector<Constant *, 16> elts;
  for (unsigned i = 0; i < col * row; i++) {
    elts.emplace_back(BuildConstInitializer(EltTy, offset, EltValList, Types,
                                            bDefaultRowMajor));
  }

  bool isRowMajor = IsRowMajorMatrix(Type, bDefaultRowMajor);

  SmallVector<Constant *, 16> majorElts(elts.begin(), elts.end());
  if (!isRowMajor) {
    // cast row major to col major.
    for (unsigned c = 0; c < col; c++) {
      SmallVector<Constant *, 4> rows;
      for (unsigned r = 0; r < row; r++) {
        unsigned rowMajorIdx = r * col + c;
        unsigned colMajorIdx = c * row + r;
        majorElts[colMajorIdx] = elts[rowMajorIdx];
      }
    }
  }
  // The type is vector<element, col>[row].
  SmallVector<Constant *, 4> rows;
  unsigned idx = 0;
  for (unsigned r = 0; r < row; r++) {
    SmallVector<Constant *, 4> cols;
    for (unsigned c = 0; c < col; c++) {
      cols.emplace_back(majorElts[idx++]);
    }
    rows.emplace_back(llvm::ConstantVector::get(cols));
  }
  Constant *mat = llvm::ConstantArray::get(AT, rows);
  return llvm::ConstantStruct::get(cast<llvm::StructType>(Ty), mat);
}

static Constant *BuildConstArray(llvm::ArrayType *AT, unsigned &offset,
                                 SmallVector<Constant *, 4> &EltValList,
                                 QualType Type, CodeGenTypes &Types,
                                 bool bDefaultRowMajor) {
  SmallVector<Constant *, 4> Elts;
  QualType EltType = QualType(Type->getArrayElementTypeNoTypeQual(), 0);
  for (unsigned i = 0; i < AT->getNumElements(); i++) {
    Elts.emplace_back(BuildConstInitializer(EltType, offset, EltValList, Types,
                                            bDefaultRowMajor));
  }
  return llvm::ConstantArray::get(AT, Elts);
}

static Constant *BuildConstStruct(llvm::StructType *ST, unsigned &offset,
                                  SmallVector<Constant *, 4> &EltValList,
                                  QualType Type, CodeGenTypes &Types,
                                  bool bDefaultRowMajor) {
  SmallVector<Constant *, 4> Elts;

  const RecordType *RT = Type->getAsStructureType();
  if (!RT)
    RT = Type->getAs<RecordType>();
  const RecordDecl *RD = RT->getDecl();

  if (const CXXRecordDecl *CXXRD = dyn_cast<CXXRecordDecl>(RD)) {
    if (CXXRD->getNumBases()) {
      // Add base as field.
      for (const auto &I : CXXRD->bases()) {
        const CXXRecordDecl *BaseDecl =
            cast<CXXRecordDecl>(I.getType()->castAs<RecordType>()->getDecl());
        // Skip empty struct.
        if (BaseDecl->field_empty())
          continue;

        // Add base as a whole constant. Not as element.
        Elts.emplace_back(BuildConstInitializer(I.getType(), offset, EltValList,
                                                Types, bDefaultRowMajor));
      }
    }
  }

  for (auto fieldIter = RD->field_begin(), fieldEnd = RD->field_end();
       fieldIter != fieldEnd; ++fieldIter) {
    Elts.emplace_back(BuildConstInitializer(
        fieldIter->getType(), offset, EltValList, Types, bDefaultRowMajor));
  }

  return llvm::ConstantStruct::get(ST, Elts);
}

static Constant *BuildConstInitializer(QualType Type, unsigned &offset,
                                       SmallVector<Constant *, 4> &EltValList,
                                       CodeGenTypes &Types,
                                       bool bDefaultRowMajor) {
  llvm::Type *Ty = Types.ConvertType(Type);
  if (llvm::VectorType *VT = dyn_cast<llvm::VectorType>(Ty)) {
    return BuildConstVector(VT, offset, EltValList, Type, Types);
  } else if (llvm::ArrayType *AT = dyn_cast<llvm::ArrayType>(Ty)) {
    return BuildConstArray(AT, offset, EltValList, Type, Types,
                           bDefaultRowMajor);
  } else if (HLMatrixLower::IsMatrixType(Ty)) {
    return BuildConstMatrix(Ty, offset, EltValList, Type, Types,
                            bDefaultRowMajor);
  } else if (StructType *ST = dyn_cast<llvm::StructType>(Ty)) {
    return BuildConstStruct(ST, offset, EltValList, Type, Types,
                            bDefaultRowMajor);
  } else {
    // Scalar basic types.
    Constant *Val = EltValList[offset++];
    if (Val->getType() == Ty) {
      return Val;
    } else {
      IRBuilder<> Builder(Ty->getContext());
      // Don't cast int to bool. bool only for scalar.
      if (Ty == Builder.getInt1Ty() && Val->getType() == Builder.getInt32Ty())
        return Val;
      Instruction::CastOps castOp =
          static_cast<Instruction::CastOps>(HLModule::FindCastOp(
              IsUnsigned(Type), IsUnsigned(Type), Val->getType(), Ty));
      return cast<Constant>(Builder.CreateCast(castOp, Val, Ty));
    }
  }
}

Constant *CGMSHLSLRuntime::EmitHLSLConstInitListExpr(CodeGenModule &CGM,
                                                     InitListExpr *E) {
  bool bDefaultRowMajor = m_pHLModule->GetHLOptions().bDefaultRowMajor;
  SmallVector<Constant *, 4> EltValList;
  if (!ScanConstInitList(CGM, E, EltValList, CGM.getTypes(), bDefaultRowMajor))
    return nullptr;

  QualType Type = E->getType();
  unsigned offset = 0;
  return BuildConstInitializer(Type, offset, EltValList, CGM.getTypes(),
                               bDefaultRowMajor);
}

Value *CGMSHLSLRuntime::EmitHLSLMatrixOperationCall(
    CodeGenFunction &CGF, const clang::Expr *E, llvm::Type *RetType,
    ArrayRef<Value *> paramList) {
  HLOpcodeGroup group = GetHLOpcodeGroup(E->getStmtClass());
  unsigned opcode = GetHLOpcode(E);
  if (group == HLOpcodeGroup::HLInit)
    return EmitHLSLArrayInit(CGF.Builder, group, opcode, RetType, paramList,
                             TheModule);
  else
    return EmitHLSLMatrixOperationCallImp(CGF.Builder, group, opcode, RetType,
                                          paramList, TheModule);
}

void CGMSHLSLRuntime::EmitHLSLDiscard(CodeGenFunction &CGF) {
  EmitHLSLMatrixOperationCallImp(
      CGF.Builder, HLOpcodeGroup::HLIntrinsic,
      static_cast<unsigned>(IntrinsicOp::IOP_clip),
      llvm::Type::getVoidTy(CGF.getLLVMContext()),
      {ConstantFP::get(llvm::Type::getFloatTy(CGF.getLLVMContext()), -1.0f)},
      TheModule);
}

Value *CGMSHLSLRuntime::EmitHLSLLiteralCast(CodeGenFunction &CGF, Value *Src,
                                            QualType SrcType,
                                            QualType DstType) {
  auto &Builder = CGF.Builder;
  llvm::Type *DstTy = CGF.ConvertType(DstType);
  bool bDstSigned = DstType->isSignedIntegerType();

  if (ConstantInt *CI = dyn_cast<ConstantInt>(Src)) {
    APInt v = CI->getValue();
    if (llvm::IntegerType *IT = dyn_cast<llvm::IntegerType>(DstTy)) {
      v = v.trunc(IT->getBitWidth());
      switch (IT->getBitWidth()) {
      case 32:
        return Builder.getInt32(v.getLimitedValue());
      case 64:
        return Builder.getInt64(v.getLimitedValue());
      case 16:
        return Builder.getInt16(v.getLimitedValue());
      case 8:
        return Builder.getInt8(v.getLimitedValue());
      default:
        return nullptr;
      }
    } else {
      DXASSERT_NOMSG(DstTy->isFloatingPointTy());
      int64_t val = v.getLimitedValue();
      if (v.isNegative())
        val = 0-v.abs().getLimitedValue();
      if (DstTy->isDoubleTy())
        return ConstantFP::get(DstTy, (double)val);
      else if (DstTy->isFloatTy())
        return ConstantFP::get(DstTy, (float)val);
      else {
        if (bDstSigned)
          return Builder.CreateSIToFP(Src, DstTy);
        else
          return Builder.CreateUIToFP(Src, DstTy);
      }
    }
  } else if (ConstantFP *CF = dyn_cast<ConstantFP>(Src)) {
    APFloat v = CF->getValueAPF();
    double dv = v.convertToDouble();
    if (llvm::IntegerType *IT = dyn_cast<llvm::IntegerType>(DstTy)) {
      switch (IT->getBitWidth()) {
      case 32:
        return Builder.getInt32(dv);
      case 64:
        return Builder.getInt64(dv);
      case 16:
        return Builder.getInt16(dv);
      case 8:
        return Builder.getInt8(dv);
      default:
        return nullptr;
      }
    } else {
      if (DstTy->isFloatTy()) {
        float fv = dv;
        return ConstantFP::get(DstTy->getContext(), APFloat(fv));
      } else {
        return Builder.CreateFPTrunc(Src, DstTy);
      }
    }
  } else if (UndefValue *UV = dyn_cast<UndefValue>(Src)) {
    return UndefValue::get(DstTy);
  } else {
    Instruction *I = cast<Instruction>(Src);
    if (SelectInst *SI = dyn_cast<SelectInst>(I)) {
      Value *T = SI->getTrueValue();
      Value *F = SI->getFalseValue();
      Value *Cond = SI->getCondition();
      if (isa<llvm::ConstantInt>(T) && isa<llvm::ConstantInt>(F)) {
        llvm::APInt lhs = cast<llvm::ConstantInt>(T)->getValue();
        llvm::APInt rhs = cast<llvm::ConstantInt>(F)->getValue();
        if (DstTy == Builder.getInt32Ty()) {
          T = Builder.getInt32(lhs.getLimitedValue());
          F = Builder.getInt32(rhs.getLimitedValue());
          Value *Sel = Builder.CreateSelect(Cond, T, F, "cond");
          return Sel;
        } else if (DstTy->isFloatingPointTy()) {
          T = ConstantFP::get(DstTy, int64_t(lhs.getLimitedValue()));
          F = ConstantFP::get(DstTy, int64_t(rhs.getLimitedValue()));
          Value *Sel = Builder.CreateSelect(Cond, T, F, "cond");
          return Sel;
        }
      } else if (isa<llvm::ConstantFP>(T) && isa<llvm::ConstantFP>(F)) {
        llvm::APFloat lhs = cast<llvm::ConstantFP>(T)->getValueAPF();
        llvm::APFloat rhs = cast<llvm::ConstantFP>(F)->getValueAPF();
        double ld = lhs.convertToDouble();
        double rd = rhs.convertToDouble();
        if (DstTy->isFloatTy()) {
          float lf = ld;
          float rf = rd;
          T = ConstantFP::get(DstTy->getContext(), APFloat(lf));
          F = ConstantFP::get(DstTy->getContext(), APFloat(rf));
          Value *Sel = Builder.CreateSelect(Cond, T, F, "cond");
          return Sel;
        } else if (DstTy == Builder.getInt32Ty()) {
          T = Builder.getInt32(ld);
          F = Builder.getInt32(rd);
          Value *Sel = Builder.CreateSelect(Cond, T, F, "cond");
          return Sel;
        } else if (DstTy == Builder.getInt64Ty()) {
          T = Builder.getInt64(ld);
          F = Builder.getInt64(rd);
          Value *Sel = Builder.CreateSelect(Cond, T, F, "cond");
          return Sel;
        }
      }
    }
    // TODO: support other opcode if need.
    return nullptr;
  }
}

Value *CGMSHLSLRuntime::EmitHLSLMatrixSubscript(CodeGenFunction &CGF,
                                                llvm::Type *RetType,
                                                llvm::Value *Ptr,
                                                llvm::Value *Idx,
                                                clang::QualType Ty) {
  bool isRowMajor =
      IsRowMajorMatrix(Ty, m_pHLModule->GetHLOptions().bDefaultRowMajor);
  unsigned opcode =
      isRowMajor ? static_cast<unsigned>(HLSubscriptOpcode::RowMatSubscript)
                 : static_cast<unsigned>(HLSubscriptOpcode::ColMatSubscript);
  Value *matBase = Ptr;
  DXASSERT(matBase->getType()->isPointerTy(),
           "matrix subscript should return pointer");

  RetType =
      llvm::PointerType::get(RetType->getPointerElementType(),
                             matBase->getType()->getPointerAddressSpace());

  // Lower mat[Idx] into real idx.
  SmallVector<Value *, 8> args;
  args.emplace_back(Ptr);
  unsigned row, col;
  hlsl::GetHLSLMatRowColCount(Ty, row, col);
  if (isRowMajor) {
    Value *cCol = ConstantInt::get(Idx->getType(), col);
    Value *Base = CGF.Builder.CreateMul(cCol, Idx);
    for (unsigned i = 0; i < col; i++) {
      Value *c = ConstantInt::get(Idx->getType(), i);
      // r * col + c
      Value *matIdx = CGF.Builder.CreateAdd(Base, c);
      args.emplace_back(matIdx);
    }
  } else {
    for (unsigned i = 0; i < col; i++) {
      Value *cMulRow = ConstantInt::get(Idx->getType(), i * row);
      // c * row + r
      Value *matIdx = CGF.Builder.CreateAdd(cMulRow, Idx);
      args.emplace_back(matIdx);
    }
  }

  Value *matSub =
      EmitHLSLMatrixOperationCallImp(CGF.Builder, HLOpcodeGroup::HLSubscript,
                                     opcode, RetType, args, TheModule);
  return matSub;
}

Value *CGMSHLSLRuntime::EmitHLSLMatrixElement(CodeGenFunction &CGF,
                                              llvm::Type *RetType,
                                              ArrayRef<Value *> paramList,
                                              QualType Ty) {
  bool isRowMajor =
      IsRowMajorMatrix(Ty, m_pHLModule->GetHLOptions().bDefaultRowMajor);
  unsigned opcode =
      isRowMajor ? static_cast<unsigned>(HLSubscriptOpcode::RowMatElement)
                 : static_cast<unsigned>(HLSubscriptOpcode::ColMatElement);

  Value *matBase = paramList[0];
  DXASSERT(matBase->getType()->isPointerTy(),
           "matrix element should return pointer");

  RetType =
      llvm::PointerType::get(RetType->getPointerElementType(),
                             matBase->getType()->getPointerAddressSpace());

  Value *idx = paramList[HLOperandIndex::kMatSubscriptSubOpIdx-1];

  // Lower _m00 into real idx.

  // -1 to avoid opcode param which is added in EmitHLSLMatrixOperationCallImp.
  Value *args[] = {paramList[HLOperandIndex::kMatSubscriptMatOpIdx - 1],
                   paramList[HLOperandIndex::kMatSubscriptSubOpIdx - 1]};
  // For all zero idx. Still all zero idx.
  if (ConstantAggregateZero *zeros = dyn_cast<ConstantAggregateZero>(idx)) {
    Constant *zero = zeros->getAggregateElement((unsigned)0);
    std::vector<Constant *> elts(zeros->getNumElements() >> 1, zero);
    args[HLOperandIndex::kMatSubscriptSubOpIdx - 1] = ConstantVector::get(elts);
  } else {
    ConstantDataSequential *elts = cast<ConstantDataSequential>(idx);
    unsigned count = elts->getNumElements();
    unsigned row, col;
    hlsl::GetHLSLMatRowColCount(Ty, row, col);
    std::vector<Constant *> idxs(count >> 1);
    for (unsigned i = 0; i < count; i += 2) {
      unsigned rowIdx = elts->getElementAsInteger(i);
      unsigned colIdx = elts->getElementAsInteger(i + 1);
      unsigned matIdx = 0;
      if (isRowMajor) {
        matIdx = rowIdx * col + colIdx;
      } else {
        matIdx = colIdx * row + rowIdx;
      }
      idxs[i >> 1] = CGF.Builder.getInt32(matIdx);
    }
    args[HLOperandIndex::kMatSubscriptSubOpIdx - 1] = ConstantVector::get(idxs);
  }

  return EmitHLSLMatrixOperationCallImp(CGF.Builder, HLOpcodeGroup::HLSubscript,
                                        opcode, RetType, args, TheModule);
}

Value *CGMSHLSLRuntime::EmitHLSLMatrixLoad(CGBuilderTy &Builder, Value *Ptr,
                                           QualType Ty) {
  bool isRowMajor =
      IsRowMajorMatrix(Ty, m_pHLModule->GetHLOptions().bDefaultRowMajor);
  unsigned opcode =
      isRowMajor
          ? static_cast<unsigned>(HLMatLoadStoreOpcode::RowMatLoad)
          : static_cast<unsigned>(HLMatLoadStoreOpcode::ColMatLoad);

  Value *matVal = EmitHLSLMatrixOperationCallImp(
      Builder, HLOpcodeGroup::HLMatLoadStore, opcode,
      Ptr->getType()->getPointerElementType(), {Ptr}, TheModule);
  if (!isRowMajor) {
    // ColMatLoad will return a col major matrix.
    // All matrix Value should be row major.
    // Cast it to row major.
    matVal = EmitHLSLMatrixOperationCallImp(
        Builder, HLOpcodeGroup::HLCast,
        static_cast<unsigned>(HLCastOpcode::ColMatrixToRowMatrix),
        matVal->getType(), {matVal}, TheModule);
  }
  return matVal;
}
void CGMSHLSLRuntime::EmitHLSLMatrixStore(CGBuilderTy &Builder, Value *Val,
                                          Value *DestPtr, QualType Ty) {
  bool isRowMajor =
      IsRowMajorMatrix(Ty, m_pHLModule->GetHLOptions().bDefaultRowMajor);
  unsigned opcode =
      isRowMajor
          ? static_cast<unsigned>(HLMatLoadStoreOpcode::RowMatStore)
          : static_cast<unsigned>(HLMatLoadStoreOpcode::ColMatStore);

  if (!isRowMajor) {
    // All matrix Value should be row major.
    // ColMatStore need a col major value.
    // Cast it to row major.
    Val = EmitHLSLMatrixOperationCallImp(
        Builder, HLOpcodeGroup::HLCast,
        static_cast<unsigned>(HLCastOpcode::RowMatrixToColMatrix),
        Val->getType(), {Val}, TheModule);
  }

  EmitHLSLMatrixOperationCallImp(Builder, HLOpcodeGroup::HLMatLoadStore, opcode,
                                 Val->getType(), {DestPtr, Val}, TheModule);
}

Value *CGMSHLSLRuntime::EmitHLSLMatrixLoad(CodeGenFunction &CGF, Value *Ptr,
                                           QualType Ty) {
  return EmitHLSLMatrixLoad(CGF.Builder, Ptr, Ty);
}
void CGMSHLSLRuntime::EmitHLSLMatrixStore(CodeGenFunction &CGF, Value *Val,
                                          Value *DestPtr, QualType Ty) {
  EmitHLSLMatrixStore(CGF.Builder, Val, DestPtr, Ty);
}

// Copy data from srcPtr to destPtr.
static void SimplePtrCopy(Value *DestPtr, Value *SrcPtr,
                          ArrayRef<Value *> idxList, CGBuilderTy &Builder) {
  if (idxList.size() > 1) {
    DestPtr = Builder.CreateInBoundsGEP(DestPtr, idxList);
    SrcPtr = Builder.CreateInBoundsGEP(SrcPtr, idxList);
  }
  llvm::LoadInst *ld = Builder.CreateLoad(SrcPtr);
  Builder.CreateStore(ld, DestPtr);
}
// Get Element val from SrvVal with extract value.
static Value *GetEltVal(Value *SrcVal, ArrayRef<Value*> idxList,
    CGBuilderTy &Builder) {
  Value *Val = SrcVal;
  // Skip beginning pointer type.
  for (unsigned i = 1; i < idxList.size(); i++) {
    ConstantInt *idx = cast<ConstantInt>(idxList[i]);
    llvm::Type *Ty = Val->getType();
    if (Ty->isAggregateType()) {
      Val = Builder.CreateExtractValue(Val, idx->getLimitedValue());
    }
  }
  return Val;
}
// Copy srcVal to destPtr.
static void SimpleValCopy(Value *DestPtr, Value *SrcVal,
                       ArrayRef<Value*> idxList,
                       CGBuilderTy &Builder) {
  Value *DestGEP = Builder.CreateInBoundsGEP(DestPtr, idxList);
  Value *Val = GetEltVal(SrcVal, idxList, Builder);

  Builder.CreateStore(Val, DestGEP);
}

static void SimpleCopy(Value *Dest, Value *Src,
                       ArrayRef<Value *> idxList,
                       CGBuilderTy &Builder) {
  if (Src->getType()->isPointerTy())
    SimplePtrCopy(Dest, Src, idxList, Builder);
  else
    SimpleValCopy(Dest, Src, idxList, Builder);
}

void CGMSHLSLRuntime::FlattenAggregatePtrToGepList(
    CodeGenFunction &CGF, Value *Ptr, SmallVector<Value *, 4> &idxList,
    clang::QualType Type, llvm::Type *Ty, SmallVector<Value *, 4> &GepList,
    SmallVector<QualType, 4> &EltTyList) {
  if (llvm::PointerType *PT = dyn_cast<llvm::PointerType>(Ty)) {
    Constant *idx = Constant::getIntegerValue(
        IntegerType::get(Ty->getContext(), 32), APInt(32, 0));
    idxList.emplace_back(idx);

    FlattenAggregatePtrToGepList(CGF, Ptr, idxList, Type, PT->getElementType(),
                                 GepList, EltTyList);

    idxList.pop_back();
  } else if (HLMatrixLower::IsMatrixType(Ty)) {
    // Use matLd/St for matrix.
    unsigned col, row;
    llvm::Type *EltTy = HLMatrixLower::GetMatrixInfo(Ty, col, row);
    llvm::PointerType *EltPtrTy =
        llvm::PointerType::get(EltTy, Ptr->getType()->getPointerAddressSpace());
    QualType EltQualTy = hlsl::GetHLSLMatElementType(Type);

    Value *matPtr = CGF.Builder.CreateInBoundsGEP(Ptr, idxList);

    // Flatten matrix to elements.
    for (unsigned r = 0; r < row; r++) {
      for (unsigned c = 0; c < col; c++) {
        ConstantInt *cRow = CGF.Builder.getInt32(r);
        ConstantInt *cCol = CGF.Builder.getInt32(c);
        Constant *CV = llvm::ConstantVector::get({cRow, cCol});
        GepList.push_back(
            EmitHLSLMatrixElement(CGF, EltPtrTy, {matPtr, CV}, Type));
        EltTyList.push_back(EltQualTy);
      }
    }

  } else if (StructType *ST = dyn_cast<StructType>(Ty)) {
    if (HLModule::IsHLSLObjectType(ST)) {
      // Avoid split HLSL object.
      Value *GEP = CGF.Builder.CreateInBoundsGEP(Ptr, idxList);
      GepList.push_back(GEP);
      EltTyList.push_back(Type);
      return;
    }
    const clang::RecordType *RT = Type->getAsStructureType();
    RecordDecl *RD = RT->getDecl();

    auto fieldIter = RD->field_begin();

    const CGRecordLayout &RL = CGF.getTypes().getCGRecordLayout(RD);

    if (const CXXRecordDecl *CXXRD = dyn_cast<CXXRecordDecl>(RD)) {
      if (CXXRD->getNumBases()) {
        // Add base as field.
        for (const auto &I : CXXRD->bases()) {
          const CXXRecordDecl *BaseDecl =
              cast<CXXRecordDecl>(I.getType()->castAs<RecordType>()->getDecl());
          // Skip empty struct.
          if (BaseDecl->field_empty())
            continue;

          QualType parentTy = QualType(BaseDecl->getTypeForDecl(), 0);
          llvm::Type *parentType = CGF.ConvertType(parentTy);

          unsigned i = RL.getNonVirtualBaseLLVMFieldNo(BaseDecl);
          Constant *idx = llvm::Constant::getIntegerValue(
              IntegerType::get(Ty->getContext(), 32), APInt(32, i));
          idxList.emplace_back(idx);

          FlattenAggregatePtrToGepList(CGF, Ptr, idxList, parentTy, parentType,
                                       GepList, EltTyList);
          idxList.pop_back();
        }
      }
    }

    for (auto fieldIter = RD->field_begin(), fieldEnd = RD->field_end();
         fieldIter != fieldEnd; ++fieldIter) {
      unsigned i = RL.getLLVMFieldNo(*fieldIter);
      llvm::Type *ET = ST->getElementType(i);

      Constant *idx = llvm::Constant::getIntegerValue(
          IntegerType::get(Ty->getContext(), 32), APInt(32, i));
      idxList.emplace_back(idx);

      FlattenAggregatePtrToGepList(CGF, Ptr, idxList, fieldIter->getType(), ET,
                                   GepList, EltTyList);

      idxList.pop_back();
    }

  } else if (llvm::ArrayType *AT = dyn_cast<llvm::ArrayType>(Ty)) {
    llvm::Type *ET = AT->getElementType();

    QualType EltType = CGF.getContext().getBaseElementType(Type);

    for (uint32_t i = 0; i < AT->getNumElements(); i++) {
      Constant *idx = Constant::getIntegerValue(
          IntegerType::get(Ty->getContext(), 32), APInt(32, i));
      idxList.emplace_back(idx);

      FlattenAggregatePtrToGepList(CGF, Ptr, idxList, EltType, ET, GepList,
                                   EltTyList);

      idxList.pop_back();
    }
  } else if (llvm::VectorType *VT = dyn_cast<llvm::VectorType>(Ty)) {
    // Flatten vector too.
    QualType EltTy = hlsl::GetHLSLVecElementType(Type);
    for (uint32_t i = 0; i < VT->getNumElements(); i++) {
      Constant *idx = CGF.Builder.getInt32(i);
      idxList.emplace_back(idx);

      Value *GEP = CGF.Builder.CreateInBoundsGEP(Ptr, idxList);
      GepList.push_back(GEP);
      EltTyList.push_back(EltTy);

      idxList.pop_back();
    }
  } else {
    Value *GEP = CGF.Builder.CreateInBoundsGEP(Ptr, idxList);
    GepList.push_back(GEP);
    EltTyList.push_back(Type);
  }
}

void CGMSHLSLRuntime::LoadFlattenedGepList(CodeGenFunction &CGF,
                                           ArrayRef<Value *> GepList,
                                           ArrayRef<QualType> EltTyList,
                                           SmallVector<Value *, 4> &EltList) {
  unsigned eltSize = GepList.size();
  for (unsigned i = 0; i < eltSize; i++) {
    Value *Ptr = GepList[i];
    QualType Type = EltTyList[i];
    // Everying is element type.
    EltList.push_back(CGF.Builder.CreateLoad(Ptr));
  }
}

void CGMSHLSLRuntime::StoreFlattenedGepList(CodeGenFunction &CGF, ArrayRef<Value *> GepList,
    ArrayRef<QualType> GepTyList, ArrayRef<Value *> EltValList, ArrayRef<QualType> SrcTyList) {
  unsigned eltSize = GepList.size();
  for (unsigned i = 0; i < eltSize; i++) {
    Value *Ptr = GepList[i];
    QualType DestType = GepTyList[i];
    Value *Val = EltValList[i];
    QualType SrcType = SrcTyList[i];

    llvm::Type *Ty = Ptr->getType()->getPointerElementType();
    // Everything is element type.
    if (Ty != Val->getType()) {
      Instruction::CastOps castOp =
          static_cast<Instruction::CastOps>(HLModule::FindCastOp(
              IsUnsigned(SrcType), IsUnsigned(DestType), Val->getType(), Ty));

      Val = CGF.Builder.CreateCast(castOp, Val, Ty);
    }
    CGF.Builder.CreateStore(Val, Ptr);
  }
}

// Copy data from SrcPtr to DestPtr.
// For matrix, use MatLoad/MatStore.
// For matrix array, EmitHLSLAggregateCopy on each element.
// For struct or array, use memcpy.
// Other just load/store.
void CGMSHLSLRuntime::EmitHLSLAggregateCopy(
    CodeGenFunction &CGF, llvm::Value *SrcPtr, llvm::Value *DestPtr,
    SmallVector<Value *, 4> &idxList, clang::QualType SrcType,
    clang::QualType DestType, llvm::Type *Ty) {
  if (llvm::PointerType *PT = dyn_cast<llvm::PointerType>(Ty)) {
    Constant *idx = Constant::getIntegerValue(
        IntegerType::get(Ty->getContext(), 32), APInt(32, 0));
    idxList.emplace_back(idx);

    EmitHLSLAggregateCopy(CGF, SrcPtr, DestPtr, idxList, SrcType, DestType,
                          PT->getElementType());

    idxList.pop_back();
  } else if (HLMatrixLower::IsMatrixType(Ty)) {
    // Use matLd/St for matrix.
    Value *srcGEP = CGF.Builder.CreateInBoundsGEP(SrcPtr, idxList);
    Value *dstGEP = CGF.Builder.CreateInBoundsGEP(DestPtr, idxList);
    Value *ldMat = EmitHLSLMatrixLoad(CGF, srcGEP, SrcType);
    EmitHLSLMatrixStore(CGF, ldMat, dstGEP, DestType);
  } else if (StructType *ST = dyn_cast<StructType>(Ty)) {
    if (HLModule::IsHLSLObjectType(ST)) {
      // Avoid split HLSL object.
      SimpleCopy(DestPtr, SrcPtr, idxList, CGF.Builder);
      return;
    }
    Value *srcGEP = CGF.Builder.CreateInBoundsGEP(SrcPtr, idxList);
    Value *dstGEP = CGF.Builder.CreateInBoundsGEP(DestPtr, idxList);
    unsigned size = this->TheModule.getDataLayout().getTypeAllocSize(ST);
    // Memcpy struct.
    CGF.Builder.CreateMemCpy(dstGEP, srcGEP, size, 1);
  } else if (llvm::ArrayType *AT = dyn_cast<llvm::ArrayType>(Ty)) {
    if (!HLMatrixLower::IsMatrixArrayPointer(llvm::PointerType::get(Ty,0))) {
      Value *srcGEP = CGF.Builder.CreateInBoundsGEP(SrcPtr, idxList);
      Value *dstGEP = CGF.Builder.CreateInBoundsGEP(DestPtr, idxList);
      unsigned size = this->TheModule.getDataLayout().getTypeAllocSize(AT);
      // Memcpy non-matrix array.
      CGF.Builder.CreateMemCpy(dstGEP, srcGEP, size, 1);
    } else {
      llvm::Type *ET = AT->getElementType();
      QualType EltDestType = CGF.getContext().getBaseElementType(DestType);
      QualType EltSrcType = CGF.getContext().getBaseElementType(SrcType);

      for (uint32_t i = 0; i < AT->getNumElements(); i++) {
        Constant *idx = Constant::getIntegerValue(
            IntegerType::get(Ty->getContext(), 32), APInt(32, i));
        idxList.emplace_back(idx);

        EmitHLSLAggregateCopy(CGF, SrcPtr, DestPtr, idxList, EltSrcType,
                              EltDestType, ET);

        idxList.pop_back();
      }
    }
  } else {
    SimpleCopy(DestPtr, SrcPtr, idxList, CGF.Builder);
  }
}

void CGMSHLSLRuntime::EmitHLSLAggregateCopy(CodeGenFunction &CGF, llvm::Value *SrcPtr,
    llvm::Value *DestPtr,
    clang::QualType Ty) {
    SmallVector<Value *, 4> idxList;
    EmitHLSLAggregateCopy(CGF, SrcPtr, DestPtr, idxList, Ty, Ty, SrcPtr->getType());
}

void CGMSHLSLRuntime::EmitHLSLFlatConversionAggregateCopy(CodeGenFunction &CGF, llvm::Value *SrcPtr,
    clang::QualType SrcTy,
    llvm::Value *DestPtr,
    clang::QualType DestTy) {
    // It is possiable to implement EmitHLSLAggregateCopy, EmitHLSLAggregateStore the same way.
    // But split value to scalar will generate many instruction when src type is same as dest type.
    SmallVector<Value *, 4> idxList;
    SmallVector<Value *, 4> SrcGEPList;
    SmallVector<QualType, 4> SrcEltTyList;
    FlattenAggregatePtrToGepList(CGF, SrcPtr, idxList, SrcTy, SrcPtr->getType(), SrcGEPList,
                        SrcEltTyList);

    SmallVector<Value *, 4> LdEltList;
    LoadFlattenedGepList(CGF, SrcGEPList, SrcEltTyList, LdEltList);

    idxList.clear();
    SmallVector<Value *, 4> DestGEPList;
    SmallVector<QualType, 4> DestEltTyList;
    FlattenAggregatePtrToGepList(CGF, DestPtr, idxList, DestTy, DestPtr->getType(), DestGEPList, DestEltTyList);

    StoreFlattenedGepList(CGF, DestGEPList, DestEltTyList, LdEltList, SrcEltTyList);
}

void CGMSHLSLRuntime::EmitHLSLAggregateStore(CodeGenFunction &CGF, llvm::Value *SrcVal,
    llvm::Value *DestPtr,
    clang::QualType Ty) {
    DXASSERT(0, "aggregate return type will use SRet, no aggregate store should exist");
}

static void SimpleFlatValCopy(Value *DestPtr, Value *SrcVal, QualType Ty,
                              QualType SrcTy, ArrayRef<Value *> idxList,
                              CGBuilderTy &Builder) {
  Value *DestGEP = Builder.CreateInBoundsGEP(DestPtr, idxList);
  llvm::Type *ToTy = DestGEP->getType()->getPointerElementType();

  llvm::Type *EltToTy = ToTy;
  if (llvm::VectorType *VT = dyn_cast<llvm::VectorType>(ToTy)) {
    EltToTy = VT->getElementType();
  }

  if (EltToTy != SrcVal->getType()) {
    Instruction::CastOps castOp =
        static_cast<Instruction::CastOps>(HLModule::FindCastOp(
            IsUnsigned(SrcTy), IsUnsigned(Ty), SrcVal->getType(), ToTy));

    SrcVal = Builder.CreateCast(castOp, SrcVal, EltToTy);
  }

  if (llvm::VectorType *VT = dyn_cast<llvm::VectorType>(ToTy)) {
    llvm::VectorType *VT1 = llvm::VectorType::get(EltToTy, 1);
    Value *V1 =
        Builder.CreateInsertElement(UndefValue::get(VT1), SrcVal, (uint64_t)0);
    std::vector<int> shufIdx(VT->getNumElements(), 0);
    Value *Vec = Builder.CreateShuffleVector(V1, V1, shufIdx);
    Builder.CreateStore(Vec, DestGEP);
  } else
    Builder.CreateStore(SrcVal, DestGEP);
}

void CGMSHLSLRuntime::EmitHLSLFlatConversionToAggregate(
    CodeGenFunction &CGF, Value *SrcVal, llvm::Value *DestPtr,
    SmallVector<Value *, 4> &idxList, QualType Type, QualType SrcType,
    llvm::Type *Ty) {
  if (llvm::PointerType *PT = dyn_cast<llvm::PointerType>(Ty)) {
    Constant *idx = Constant::getIntegerValue(
        IntegerType::get(Ty->getContext(), 32), APInt(32, 0));
    idxList.emplace_back(idx);

    EmitHLSLFlatConversionToAggregate(CGF, SrcVal, DestPtr, idxList, Type,
                                      SrcType, PT->getElementType());

    idxList.pop_back();
  } else if (HLMatrixLower::IsMatrixType(Ty)) {
    // Use matLd/St for matrix.
    Value *dstGEP = CGF.Builder.CreateInBoundsGEP(DestPtr, idxList);
    unsigned row, col;
    llvm::Type *EltTy = HLMatrixLower::GetMatrixInfo(Ty, col, row);

    llvm::VectorType *VT1 = llvm::VectorType::get(EltTy, 1);
    if (EltTy != SrcVal->getType()) {
      Instruction::CastOps castOp =
          static_cast<Instruction::CastOps>(HLModule::FindCastOp(
              IsUnsigned(SrcType), IsUnsigned(Type), SrcVal->getType(), EltTy));

      SrcVal = CGF.Builder.CreateCast(castOp, SrcVal, EltTy);
    }

    Value *V1 = CGF.Builder.CreateInsertElement(UndefValue::get(VT1), SrcVal,
                                                (uint64_t)0);
    std::vector<int> shufIdx(col * row, 0);

    Value *VecMat = CGF.Builder.CreateShuffleVector(V1, V1, shufIdx);
    Value *MatInit = EmitHLSLMatrixOperationCallImp(
        CGF.Builder, HLOpcodeGroup::HLInit, 0, Ty, {VecMat}, TheModule);
    EmitHLSLMatrixStore(CGF, MatInit, dstGEP, Type);
  } else if (StructType *ST = dyn_cast<StructType>(Ty)) {
    DXASSERT(!HLModule::IsHLSLObjectType(ST), "cannot cast to hlsl object, Sema should reject");

    const clang::RecordType *RT = Type->getAsStructureType();
    RecordDecl *RD = RT->getDecl();
    auto fieldIter = RD->field_begin();

    const CGRecordLayout &RL = CGF.getTypes().getCGRecordLayout(RD);
    // Take care base.
    if (const CXXRecordDecl *CXXRD = dyn_cast<CXXRecordDecl>(RD)) {
      if (CXXRD->getNumBases()) {
        for (const auto &I : CXXRD->bases()) {
          const CXXRecordDecl *BaseDecl =
              cast<CXXRecordDecl>(I.getType()->castAs<RecordType>()->getDecl());
          if (BaseDecl->field_empty())
            continue;
          QualType parentTy = QualType(BaseDecl->getTypeForDecl(), 0);
          unsigned i = RL.getNonVirtualBaseLLVMFieldNo(BaseDecl);

          llvm::Type *ET = ST->getElementType(i);
          Constant *idx = llvm::Constant::getIntegerValue(
              IntegerType::get(Ty->getContext(), 32), APInt(32, i));
          idxList.emplace_back(idx);
          EmitHLSLFlatConversionToAggregate(CGF, SrcVal, DestPtr, idxList,
                                            parentTy, SrcType, ET);
          idxList.pop_back();
        }
      }
    }
    for (auto fieldIter = RD->field_begin(), fieldEnd = RD->field_end();
         fieldIter != fieldEnd; ++fieldIter) {
      unsigned i = RL.getLLVMFieldNo(*fieldIter);
      llvm::Type *ET = ST->getElementType(i);

      Constant *idx = llvm::Constant::getIntegerValue(
          IntegerType::get(Ty->getContext(), 32), APInt(32, i));
      idxList.emplace_back(idx);

      EmitHLSLFlatConversionToAggregate(CGF, SrcVal, DestPtr, idxList,
                                        fieldIter->getType(), SrcType, ET);

      idxList.pop_back();
    }

  } else if (llvm::ArrayType *AT = dyn_cast<llvm::ArrayType>(Ty)) {
    llvm::Type *ET = AT->getElementType();

    QualType EltType = CGF.getContext().getBaseElementType(Type);

    for (uint32_t i = 0; i < AT->getNumElements(); i++) {
      Constant *idx = Constant::getIntegerValue(
          IntegerType::get(Ty->getContext(), 32), APInt(32, i));
      idxList.emplace_back(idx);

      EmitHLSLFlatConversionToAggregate(CGF, SrcVal, DestPtr, idxList, EltType,
                                        SrcType, ET);

      idxList.pop_back();
    }
  } else {
    SimpleFlatValCopy(DestPtr, SrcVal, Type, SrcType, idxList, CGF.Builder);
  }
}

void CGMSHLSLRuntime::EmitHLSLFlatConversionToAggregate(CodeGenFunction &CGF,
                                                        Value *Val,
                                                        Value *DestPtr,
                                                        QualType Ty,
                                                        QualType SrcTy) {
  if (SrcTy->isBuiltinType()) {
    SmallVector<Value *, 4> idxList;
    // Add first 0 for DestPtr.
    Constant *idx = Constant::getIntegerValue(
        IntegerType::get(Val->getContext(), 32), APInt(32, 0));
    idxList.emplace_back(idx);

    EmitHLSLFlatConversionToAggregate(
        CGF, Val, DestPtr, idxList, Ty, SrcTy,
        DestPtr->getType()->getPointerElementType());
  }
  else {
    SmallVector<Value *, 4> idxList;
    SmallVector<Value *, 4> DestGEPList;
    SmallVector<QualType, 4> DestEltTyList;
    FlattenAggregatePtrToGepList(CGF, DestPtr, idxList, Ty, DestPtr->getType(), DestGEPList, DestEltTyList);

    SmallVector<Value *, 4> EltList;
    SmallVector<QualType, 4> EltTyList;
    FlattenValToInitList(CGF, EltList, EltTyList, SrcTy, Val);

    StoreFlattenedGepList(CGF, DestGEPList, DestEltTyList, EltList, EltTyList);
  }
}

void CGMSHLSLRuntime::EmitHLSLRootSignature(CodeGenFunction &CGF,
                                            HLSLRootSignatureAttr *RSA,
                                            Function *Fn) {
  // Only parse root signature for entry function.
  if (Fn != EntryFunc)
    return;

  StringRef StrRef = RSA->getSignatureName();
  DiagnosticsEngine &Diags = CGF.getContext().getDiagnostics();
  SourceLocation SLoc = RSA->getLocation();

  clang::CompileRootSignature(StrRef, Diags, SLoc, rootSigVer, &m_pHLModule->GetRootSignature());
}

void CGMSHLSLRuntime::EmitHLSLOutParamConversionInit(
    CodeGenFunction &CGF, const FunctionDecl *FD, const CallExpr *E,
    llvm::SmallVector<LValue, 8> &castArgList,
    llvm::SmallVector<const Stmt *, 8> &argList,
    const std::function<void(const VarDecl *, llvm::Value *)> &TmpArgMap) {
  // Special case: skip first argument of CXXOperatorCall (it is "this").
  unsigned ArgsToSkip = isa<CXXOperatorCallExpr>(E) ? 1 : 0;

  for (uint32_t i = 0; i < FD->getNumParams(); i++) {
    const ParmVarDecl *Param = FD->getParamDecl(i);
    const Expr *Arg = E->getArg(i+ArgsToSkip);
    QualType ParamTy = Param->getType().getNonReferenceType();

    if (!Param->isModifierOut())
      continue;

    // get original arg
    LValue argLV = CGF.EmitLValue(Arg);

    // create temp Var
    VarDecl *tmpArg =
        VarDecl::Create(CGF.getContext(), const_cast<FunctionDecl *>(FD),
                        SourceLocation(), SourceLocation(),
                        /*IdentifierInfo*/ nullptr, ParamTy,
                        CGF.getContext().getTrivialTypeSourceInfo(ParamTy),
                        StorageClass::SC_Auto);

    // Aggregate type will be indirect param convert to pointer type.
    // So don't update to ReferenceType, use RValue for it.
    bool isAggregateType = (ParamTy->isArrayType() || ParamTy->isRecordType()) &&
      !hlsl::IsHLSLVecMatType(ParamTy);

    const DeclRefExpr *tmpRef = DeclRefExpr::Create(
        CGF.getContext(), NestedNameSpecifierLoc(), SourceLocation(), tmpArg,
        /*enclosing*/ false, tmpArg->getLocation(), ParamTy,
        isAggregateType ? VK_RValue : VK_LValue);

    // update the arg
    argList[i] = tmpRef;

    // create alloc for the tmp arg
    Value *tmpArgAddr = nullptr;
    BasicBlock *InsertBlock = CGF.Builder.GetInsertBlock();
    Function *F = InsertBlock->getParent();
    BasicBlock *EntryBlock = &F->getEntryBlock();

    if (ParamTy->isBooleanType()) {
      // Create i32 for bool.
      ParamTy = CGM.getContext().IntTy;
    }
    // Make sure the alloca is in entry block to stop inline create stacksave.
    IRBuilder<> Builder(EntryBlock->getFirstInsertionPt());
    tmpArgAddr = Builder.CreateAlloca(CGF.ConvertType(ParamTy));

      
    // add it to local decl map
    TmpArgMap(tmpArg, tmpArgAddr);

    LValue tmpLV = LValue::MakeAddr(tmpArgAddr, ParamTy, argLV.getAlignment(),
                                    CGF.getContext());

    // save for cast after call
    castArgList.emplace_back(tmpLV);
    castArgList.emplace_back(argLV);

    bool isObject = HLModule::IsHLSLObjectType(
        tmpArgAddr->getType()->getPointerElementType());

    // cast before the call
    if (Param->isModifierIn() &&
        // Don't copy object
        !isObject) {
      QualType ArgTy = Arg->getType();
      Value *outVal = nullptr;
      bool isAggrageteTy = ParamTy->isAggregateType();
      isAggrageteTy &= !IsHLSLVecMatType(ParamTy);
      if (!isAggrageteTy) {
        if (!IsHLSLMatType(ParamTy)) {
          RValue outRVal = CGF.EmitLoadOfLValue(argLV, SourceLocation());
          outVal = outRVal.getScalarVal();
        } else {
          Value *argAddr = argLV.getAddress();
          outVal = EmitHLSLMatrixLoad(CGF, argAddr, ArgTy);
        }

        llvm::Type *ToTy = tmpArgAddr->getType()->getPointerElementType();
        Instruction::CastOps castOp =
            static_cast<Instruction::CastOps>(HLModule::FindCastOp(
                IsUnsigned(argLV.getType()), IsUnsigned(tmpLV.getType()),
                outVal->getType(), ToTy));

        Value *castVal = CGF.Builder.CreateCast(castOp, outVal, ToTy);
        if (!HLMatrixLower::IsMatrixType(ToTy))
          CGF.Builder.CreateStore(castVal, tmpArgAddr);
        else
          EmitHLSLMatrixStore(CGF, castVal, tmpArgAddr, ParamTy);
      } else {
        SmallVector<Value *, 4> idxList;
        EmitHLSLAggregateCopy(CGF, argLV.getAddress(), tmpLV.getAddress(),
                              idxList, ArgTy, ParamTy,
                              argLV.getAddress()->getType());
      }
    }
  }
}

void CGMSHLSLRuntime::EmitHLSLOutParamConversionCopyBack(
    CodeGenFunction &CGF, llvm::SmallVector<LValue, 8> &castArgList) {
  for (uint32_t i = 0; i < castArgList.size(); i += 2) {
    // cast after the call
    LValue tmpLV = castArgList[i];
    LValue argLV = castArgList[i + 1];
    QualType ArgTy = argLV.getType().getNonReferenceType();
    QualType ParamTy = tmpLV.getType().getNonReferenceType();

    Value *tmpArgAddr = tmpLV.getAddress();
    
    Value *outVal = nullptr;

    bool isAggrageteTy = ArgTy->isAggregateType();
    isAggrageteTy &= !IsHLSLVecMatType(ArgTy);

    bool isObject = HLModule::IsHLSLObjectType(
       tmpArgAddr->getType()->getPointerElementType());
    if (!isObject) {
      if (!isAggrageteTy) {
        if (!IsHLSLMatType(ParamTy))
          outVal = CGF.Builder.CreateLoad(tmpArgAddr);
        else
          outVal = EmitHLSLMatrixLoad(CGF, tmpArgAddr, ParamTy);

        llvm::Type *ToTy = CGF.ConvertType(ArgTy);
        llvm::Type *FromTy = outVal->getType();
        Value *castVal = outVal;
        if (ToTy == FromTy) {
          // Don't need cast.
        } else if (ToTy->getScalarType() == FromTy->getScalarType()) {
          if (ToTy->getScalarType() == ToTy) {
            DXASSERT(FromTy->isVectorTy() &&
                         FromTy->getVectorNumElements() == 1,
                     "must be vector of 1 element");
            castVal = CGF.Builder.CreateExtractElement(outVal, (uint64_t)0);
          } else {
            DXASSERT(!FromTy->isVectorTy(), "must be scalar type");
            DXASSERT(ToTy->isVectorTy() && ToTy->getVectorNumElements() == 1,
                     "must be vector of 1 element");
            castVal = UndefValue::get(ToTy);
            castVal =
                CGF.Builder.CreateInsertElement(castVal, outVal, (uint64_t)0);
          }
        } else {
          Instruction::CastOps castOp =
              static_cast<Instruction::CastOps>(HLModule::FindCastOp(
                  IsUnsigned(tmpLV.getType()), IsUnsigned(argLV.getType()),
                  outVal->getType(), ToTy));

          castVal = CGF.Builder.CreateCast(castOp, outVal, ToTy);
        }
        if (!HLMatrixLower::IsMatrixType(ToTy))
          CGF.EmitStoreThroughLValue(RValue::get(castVal), argLV);
        else {
          Value *destPtr = argLV.getAddress();
          EmitHLSLMatrixStore(CGF, castVal, destPtr, ArgTy);
        }
      } else {
        SmallVector<Value *, 4> idxList;
        EmitHLSLAggregateCopy(CGF, tmpLV.getAddress(), argLV.getAddress(),
                              idxList, ParamTy, ArgTy,
                              argLV.getAddress()->getType());
      }
    } else
      tmpArgAddr->replaceAllUsesWith(argLV.getAddress());
  }
}

CGHLSLRuntime *CodeGen::CreateMSHLSLRuntime(CodeGenModule &CGM) {
  return new CGMSHLSLRuntime(CGM);
}
