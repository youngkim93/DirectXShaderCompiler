///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// DxilModule.cpp                                                            //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/Support/Global.h"
#include "dxc/HLSL/DxilOperations.h"
#include "dxc/HLSL/DxilModule.h"
#include "dxc/HLSL/DxilShaderModel.h"
#include "dxc/HLSL/DxilSignatureElement.h"
#include "dxc/HLSL/DxilContainer.h"
#include "dxc/HLSL/DxilRootSignature.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include <unordered_set>

using namespace llvm;
using std::string;
using std::vector;
using std::unique_ptr;


namespace {
class DxilErrorDiagnosticInfo : public DiagnosticInfo {
private:
  const char *m_message;
public:
  DxilErrorDiagnosticInfo(const char *str)
    : DiagnosticInfo(DK_FirstPluginKind, DiagnosticSeverity::DS_Error),
    m_message(str) { }

  __override void print(DiagnosticPrinter &DP) const {
    DP << m_message;
  }
};
} // anon namespace

namespace hlsl {

//------------------------------------------------------------------------------
//
//  DxilModule methods.
//
DxilModule::DxilModule(Module *pModule)
: m_Ctx(pModule->getContext())
, m_pModule(pModule)
, m_pOP(std::make_unique<OP>(pModule->getContext(), pModule))
, m_pTypeSystem(std::make_unique<DxilTypeSystem>(pModule))
, m_pViewIdState(std::make_unique<DxilViewIdState>(this))
, m_pMDHelper(std::make_unique<DxilMDHelper>(pModule, std::make_unique<DxilExtraPropertyHelper>(pModule)))
, m_pDebugInfoFinder(nullptr)
, m_pEntryFunc(nullptr)
, m_EntryName("")
, m_pPatchConstantFunc(nullptr)
, m_pSM(nullptr)
, m_DxilMajor(DXIL::kDxilMajor)
, m_DxilMinor(DXIL::kDxilMinor)
, m_ValMajor(1)
, m_ValMinor(0)
, m_InputPrimitive(DXIL::InputPrimitive::Undefined)
, m_MaxVertexCount(0)
, m_StreamPrimitiveTopology(DXIL::PrimitiveTopology::Undefined)
, m_ActiveStreamMask(0)
, m_NumGSInstances(1)
, m_InputControlPointCount(0)
, m_TessellatorDomain(DXIL::TessellatorDomain::Undefined)
, m_OutputControlPointCount(0)
, m_TessellatorPartitioning(DXIL::TessellatorPartitioning::Undefined)
, m_TessellatorOutputPrimitive(DXIL::TessellatorOutputPrimitive::Undefined)
, m_MaxTessellationFactor(0.f)
, m_RootSignature(nullptr) {
  DXASSERT_NOMSG(m_pModule != nullptr);

  m_NumThreads[0] = m_NumThreads[1] = m_NumThreads[2] = 0;

#if defined(_DEBUG) || defined(DBG)
  // Pin LLVM dump methods.
  void (__thiscall Module::*pfnModuleDump)() const = &Module::dump;
  void (__thiscall Type::*pfnTypeDump)() const = &Type::dump;
  void (__thiscall Function::*pfnViewCFGOnly)() const = &Function::viewCFGOnly;
  m_pUnused = (char *)&pfnModuleDump - (char *)&pfnTypeDump;
  m_pUnused -= (size_t)&pfnViewCFGOnly;
#endif
}

DxilModule::~DxilModule() {
}

DxilModule::ShaderFlags::ShaderFlags():
  m_bDisableOptimizations(false)
, m_bDisableMathRefactoring(false)
, m_bEnableDoublePrecision(false)
, m_bForceEarlyDepthStencil(false)
, m_bEnableRawAndStructuredBuffers(false)
, m_bEnableMinPrecision(false)
, m_bEnableDoubleExtensions(false)
, m_bEnableMSAD(false)
, m_bAllResourcesBound(false)
, m_bViewportAndRTArrayIndex(false)
, m_bInnerCoverage(false)
, m_bStencilRef(false)
, m_bTiledResources(false)
, m_bUAVLoadAdditionalFormats(false)
, m_bLevel9ComparisonFiltering(false)
, m_bCSRawAndStructuredViaShader4X(false)
, m_b64UAVs(false)
, m_UAVsAtEveryStage(false)
, m_bROVS(false)
, m_bWaveOps(false)
, m_bInt64Ops(false)
, m_bViewID(false)
, m_bBarycentrics(false)
, m_align0(0)
, m_align1(0)
{}

LLVMContext &DxilModule::GetCtx() const { return m_Ctx; }
Module *DxilModule::GetModule() const { return m_pModule; }
OP *DxilModule::GetOP() const { return m_pOP.get(); }

void DxilModule::SetShaderModel(const ShaderModel *pSM) {
  DXASSERT(m_pSM == nullptr || (pSM != nullptr && *m_pSM == *pSM), "shader model must not change for the module");
  DXASSERT(pSM != nullptr && pSM->IsValidForDxil(), "shader model must be valid");
  m_pSM = pSM;
  m_pSM->GetDxilVersion(m_DxilMajor, m_DxilMinor);
  m_pMDHelper->SetShaderModel(m_pSM);
  DXIL::ShaderKind shaderKind = pSM->GetKind();
  m_InputSignature.reset(new DxilSignature(shaderKind, DXIL::SignatureKind::Input));
  m_OutputSignature.reset(new DxilSignature(shaderKind, DXIL::SignatureKind::Output));
  m_PatchConstantSignature.reset(new DxilSignature(shaderKind, DXIL::SignatureKind::PatchConstant));
  m_RootSignature.reset(new RootSignatureHandle());
}

const ShaderModel *DxilModule::GetShaderModel() const {
  return m_pSM;
}

void DxilModule::GetDxilVersion(unsigned &DxilMajor, unsigned &DxilMinor) const {
  DxilMajor = m_DxilMajor;
  DxilMinor = m_DxilMinor;
}

void DxilModule::SetValidatorVersion(unsigned ValMajor, unsigned ValMinor) {
  m_ValMajor = ValMajor;
  m_ValMinor = ValMinor;
}

bool DxilModule::UpgradeValidatorVersion(unsigned ValMajor, unsigned ValMinor) {
  if (ValMajor > m_ValMajor || (ValMajor == m_ValMajor && ValMinor > m_ValMinor)) {
    // Module requires higher validator version than previously set
    SetValidatorVersion(ValMajor, ValMinor);
    return true;
  }
  return false;
}

void DxilModule::GetValidatorVersion(unsigned &ValMajor, unsigned &ValMinor) const {
  ValMajor = m_ValMajor;
  ValMinor = m_ValMinor;
}

bool DxilModule::GetMinValidatorVersion(unsigned &ValMajor, unsigned &ValMinor) const {
  if (!m_pSM)
    return false;
  m_pSM->GetMinValidatorVersion(ValMajor, ValMinor);
  if (ValMajor == 1 && ValMinor == 0 && (m_ShaderFlags.GetFeatureInfo() & hlsl::ShaderFeatureInfo_ViewID))
    ValMinor = 1;
  return true;
}

bool DxilModule::UpgradeToMinValidatorVersion() {
  unsigned ValMajor = 1, ValMinor = 0;
  if (GetMinValidatorVersion(ValMajor, ValMinor)) {
    return UpgradeValidatorVersion(ValMajor, ValMinor);
  }
  return false;
}

Function *DxilModule::GetEntryFunction() {
  return m_pEntryFunc;
}

const Function *DxilModule::GetEntryFunction() const {
  return m_pEntryFunc;
}

void DxilModule::SetEntryFunction(Function *pEntryFunc) {
  m_pEntryFunc = pEntryFunc;
}

const string &DxilModule::GetEntryFunctionName() const {
  return m_EntryName;
}

void DxilModule::SetEntryFunctionName(const string &name) {
  m_EntryName = name;
}

llvm::Function *DxilModule::GetPatchConstantFunction() {
  return m_pPatchConstantFunc;
}

const llvm::Function *DxilModule::GetPatchConstantFunction() const {
  return m_pPatchConstantFunc;
}

void DxilModule::SetPatchConstantFunction(llvm::Function *pFunc) {
  m_pPatchConstantFunc = pFunc;
}

unsigned DxilModule::ShaderFlags::GetGlobalFlags() const {
  unsigned Flags = 0;
  Flags |= m_bDisableOptimizations ? DXIL::kDisableOptimizations : 0;
  Flags |= m_bDisableMathRefactoring ? DXIL::kDisableMathRefactoring : 0;
  Flags |= m_bEnableDoublePrecision ? DXIL::kEnableDoublePrecision : 0;
  Flags |= m_bForceEarlyDepthStencil ? DXIL::kForceEarlyDepthStencil : 0;
  Flags |= m_bEnableRawAndStructuredBuffers ? DXIL::kEnableRawAndStructuredBuffers : 0;
  Flags |= m_bEnableMinPrecision ? DXIL::kEnableMinPrecision : 0;
  Flags |= m_bEnableDoubleExtensions ? DXIL::kEnableDoubleExtensions : 0;
  Flags |= m_bEnableMSAD ? DXIL::kEnableMSAD : 0;
  Flags |= m_bAllResourcesBound ? DXIL::kAllResourcesBound : 0;
  return Flags;
}

uint64_t DxilModule::ShaderFlags::GetFeatureInfo() const {
  uint64_t Flags = 0;
  Flags |= m_bEnableDoublePrecision ? hlsl::ShaderFeatureInfo_Doubles : 0;
  Flags |= m_bEnableMinPrecision ? hlsl::ShaderFeatureInfo_MininumPrecision : 0;
  Flags |= m_bEnableDoubleExtensions ? hlsl::ShaderFeatureInfo_11_1_DoubleExtensions : 0;
  Flags |= m_bWaveOps ? hlsl::ShaderFeatureInfo_WaveOps : 0;
  Flags |= m_bInt64Ops ? hlsl::ShaderFeatureInfo_Int64Ops : 0;
  Flags |= m_bROVS ? hlsl::ShaderFeatureInfo_ROVs : 0;
  Flags |= m_bViewportAndRTArrayIndex ? hlsl::ShaderFeatureInfo_ViewportAndRTArrayIndexFromAnyShaderFeedingRasterizer : 0;
  Flags |= m_bInnerCoverage ? hlsl::ShaderFeatureInfo_InnerCoverage : 0;
  Flags |= m_bStencilRef ? hlsl::ShaderFeatureInfo_StencilRef : 0;
  Flags |= m_bTiledResources ? hlsl::ShaderFeatureInfo_TiledResources : 0;
  Flags |= m_bEnableMSAD ? hlsl::ShaderFeatureInfo_11_1_ShaderExtensions : 0;
  Flags |= m_bCSRawAndStructuredViaShader4X ? hlsl::ShaderFeatureInfo_ComputeShadersPlusRawAndStructuredBuffersViaShader4X : 0;
  Flags |= m_UAVsAtEveryStage ? hlsl::ShaderFeatureInfo_UAVsAtEveryStage : 0;
  Flags |= m_b64UAVs ? hlsl::ShaderFeatureInfo_64UAVs : 0;
  Flags |= m_bLevel9ComparisonFiltering ? hlsl::ShaderFeatureInfo_LEVEL9ComparisonFiltering : 0;
  Flags |= m_bUAVLoadAdditionalFormats ? hlsl::ShaderFeatureInfo_TypedUAVLoadAdditionalFormats : 0;
  Flags |= m_bViewID ? hlsl::ShaderFeatureInfo_ViewID : 0;
  Flags |= m_bBarycentrics ? hlsl::ShaderFeatureInfo_Barycentrics : 0;

  return Flags;
}

uint64_t DxilModule::ShaderFlags::GetShaderFlagsRaw() const {
  union Cast {
    Cast(const DxilModule::ShaderFlags &flags) {
      shaderFlags = flags;
    }
    DxilModule::ShaderFlags shaderFlags;
    uint64_t  rawData;
  };
  static_assert(sizeof(uint64_t) == sizeof(DxilModule::ShaderFlags),
                "size must match to make sure no undefined bits when cast");
  Cast rawCast(*this);
  return rawCast.rawData;
}
void DxilModule::ShaderFlags::SetShaderFlagsRaw(uint64_t data) {
  union Cast {
    Cast(uint64_t data) {
      rawData = data;
    }
    DxilModule::ShaderFlags shaderFlags;
    uint64_t  rawData;
  };

  Cast rawCast(data);
  *this = rawCast.shaderFlags;
}

unsigned DxilModule::GetGlobalFlags() const {
  unsigned Flags = m_ShaderFlags.GetGlobalFlags();
  return Flags;
}

static bool IsResourceSingleType(llvm::Type *Ty) {
  if (llvm::ArrayType *arrType = llvm::dyn_cast<llvm::ArrayType>(Ty)) {
    if (arrType->getArrayNumElements() > 1) {
      return false;
    }
    return IsResourceSingleType(arrType->getArrayElementType());
  } else if (llvm::StructType *structType =
                 llvm::dyn_cast<llvm::StructType>(Ty)) {
    if (structType->getStructNumElements() > 1) {
      return false;
    }
    return IsResourceSingleType(structType->getStructElementType(0));
  } else if (llvm::VectorType *vectorType =
                 llvm::dyn_cast<llvm::VectorType>(Ty)) {
    if (vectorType->getNumElements() > 1) {
      return false;
    }
    return IsResourceSingleType(vectorType->getVectorElementType());
  }
  return true;
}

void DxilModule::CollectShaderFlags(ShaderFlags &Flags) {
  bool hasDouble = false;
  // ddiv dfma drcp d2i d2u i2d u2d.
  // fma has dxil op. Others should check IR instruction div/cast.
  bool hasDoubleExtension = false;
  bool has64Int = false;
  bool has16FloatInt = false;
  bool hasWaveOps = false;
  bool hasCheckAccessFully = false;
  bool hasMSAD = false;
  bool hasMulticomponentUAVLoads = false;
  bool hasInnerCoverage = false;
  bool hasViewID = false;
  Type *int16Ty = Type::getInt16Ty(GetCtx());
  Type *int64Ty = Type::getInt64Ty(GetCtx());

  for (Function &F : GetModule()->functions()) {
    for (BasicBlock &BB : F.getBasicBlockList()) {
      for (Instruction &I : BB.getInstList()) {
        // Skip none dxil function call.
        if (CallInst *CI = dyn_cast<CallInst>(&I)) {
          if (!OP::IsDxilOpFunc(CI->getCalledFunction()))
            continue;
        }
        Type *Ty = I.getType();
        bool isDouble = Ty->isDoubleTy();
        bool isHalf = Ty->isHalfTy();
        bool isInt16 = Ty == int16Ty;
        bool isInt64 = Ty == int64Ty;
        if (isa<ExtractElementInst>(&I) ||
            isa<InsertElementInst>(&I))
          continue;
        for (Value *operand : I.operands()) {
          Type *Ty = operand->getType();
          isDouble |= Ty->isDoubleTy();
          isHalf |= Ty->isHalfTy();
          isInt16 |= Ty == int16Ty;
          isInt64 |= Ty == int64Ty;
        }

        if (isDouble) {
          hasDouble = true;
          switch (I.getOpcode()) {
          case Instruction::FDiv:
          case Instruction::UIToFP:
          case Instruction::SIToFP:
          case Instruction::FPToUI:
          case Instruction::FPToSI:
            hasDoubleExtension = true;
            break;
          }
        }
        
        has16FloatInt |= isHalf;
        has16FloatInt |= isInt16;
        has64Int |= isInt64;

        if (CallInst *CI = dyn_cast<CallInst>(&I)) {
          if (!OP::IsDxilOpFunc(CI->getCalledFunction()))
            continue;
          Value *opcodeArg = CI->getArgOperand(DXIL::OperandIndex::kOpcodeIdx);
          ConstantInt *opcodeConst = dyn_cast<ConstantInt>(opcodeArg);
          DXASSERT(opcodeConst, "DXIL opcode arg must be immediate");
          unsigned opcode = opcodeConst->getLimitedValue();
          DXASSERT(opcode < static_cast<unsigned>(DXIL::OpCode::NumOpCodes),
                   "invalid DXIL opcode");
          DXIL::OpCode dxilOp = static_cast<DXIL::OpCode>(opcode);
          if (hlsl::OP::IsDxilOpWave(dxilOp))
            hasWaveOps = true;
          switch (dxilOp) {
          case DXIL::OpCode::CheckAccessFullyMapped:
            hasCheckAccessFully = true;
            break;
          case DXIL::OpCode::Msad:
            hasMSAD = true;
            break;
          case DXIL::OpCode::BufferLoad: {
              Value *resHandle = CI->getArgOperand(DXIL::OperandIndex::kBufferStoreHandleOpIdx);
              CallInst *handleCall = cast<CallInst>(resHandle);

              if (ConstantInt *resClassArg =
                  dyn_cast<ConstantInt>(handleCall->getArgOperand(
                      DXIL::OperandIndex::kCreateHandleResClassOpIdx))) {
                  DXIL::ResourceClass resClass = static_cast<DXIL::ResourceClass>(
                      resClassArg->getLimitedValue());
                  if (resClass == DXIL::ResourceClass::UAV) {
                    if (ConstantInt *resRangeID =
                            dyn_cast<ConstantInt>(handleCall->getArgOperand(
                                DXIL::OperandIndex::kCreateHandleResIDOpIdx))) {
                      DxilResource &res = GetUAV(resRangeID->getLimitedValue());
                      if (res.GetKind() == DXIL::ResourceKind::TypedBuffer) {
                        Value *GV = res.GetGlobalSymbol();
                        llvm::Type *Ty = GV->getType()->getPointerElementType();
                        // Check if uav load is multi component load.
                        if (!IsResourceSingleType(Ty)) {
                          hasMulticomponentUAVLoads = true;
                        }
                      }
                    }
                  }
              }
              else if (PHINode *resClassPhi = dyn_cast<
                  PHINode>(handleCall->getArgOperand(
                      DXIL::OperandIndex::kCreateHandleResClassOpIdx))) {
                  unsigned numOperands = resClassPhi->getNumOperands();
                  for (unsigned i = 0; i < numOperands; i++) {
                      if (ConstantInt *resClassArg = dyn_cast<ConstantInt>(
                          resClassPhi->getIncomingValue(i))) {
                          DXIL::ResourceClass resClass =
                              static_cast<DXIL::ResourceClass>(
                                  resClassArg->getLimitedValue());
                          if (resClass == DXIL::ResourceClass::UAV) {
                            if (ConstantInt *resRangeID = dyn_cast<ConstantInt>(
                                    handleCall->getArgOperand(
                                        DXIL::OperandIndex::
                                            kCreateHandleResIDOpIdx))) {
                              DxilResource &res =
                                  GetUAV(resRangeID->getLimitedValue());
                              if (res.GetKind() ==
                                  DXIL::ResourceKind::TypedBuffer) {
                                Value *GV = res.GetGlobalSymbol();
                                llvm::Type *Ty =
                                    GV->getType()->getPointerElementType();
                                // Check if uav load is multi component load.
                                if (!IsResourceSingleType(Ty)) {
                                  hasMulticomponentUAVLoads = true;
                                  break;
                                }
                              }
                            }
                          }
                      }
                  }
              }
          } break;
          case DXIL::OpCode::TextureLoad: {
            Value *resHandle = CI->getArgOperand(DXIL::OperandIndex::kBufferStoreHandleOpIdx);
            CallInst *handleCall = cast<CallInst>(resHandle);

            if (ConstantInt *resClassArg =
                    dyn_cast<ConstantInt>(handleCall->getArgOperand(
                        DXIL::OperandIndex::kCreateHandleResClassOpIdx))) {
              DXIL::ResourceClass resClass = static_cast<DXIL::ResourceClass>(
                  resClassArg->getLimitedValue());
              if (resClass == DXIL::ResourceClass::UAV) {
                if (ConstantInt *resRangeID =
                        dyn_cast<ConstantInt>(handleCall->getArgOperand(
                            DXIL::OperandIndex::kCreateHandleResIDOpIdx))) {
                  DxilResource &res = GetUAV(resRangeID->getLimitedValue());
                  Value *GV = res.GetGlobalSymbol();
                  llvm::Type *Ty = GV->getType()->getPointerElementType();
                  // Check if uav load is multi component load.
                  if (!IsResourceSingleType(Ty)) {
                    hasMulticomponentUAVLoads = true;
                    break;
                  }
                }
              }
            } else if (PHINode *resClassPhi = dyn_cast<
                           PHINode>(handleCall->getArgOperand(
                           DXIL::OperandIndex::kCreateHandleResClassOpIdx))) {
              unsigned numOperands = resClassPhi->getNumOperands();
              for (unsigned i = 0; i < numOperands; i++) {
                if (ConstantInt *resClassArg = dyn_cast<ConstantInt>(
                        resClassPhi->getIncomingValue(i))) {
                  DXIL::ResourceClass resClass =
                      static_cast<DXIL::ResourceClass>(
                          resClassArg->getLimitedValue());
                  if (resClass == DXIL::ResourceClass::UAV) {
                    if (ConstantInt *resRangeID =
                            dyn_cast<ConstantInt>(handleCall->getArgOperand(
                                DXIL::OperandIndex::kCreateHandleResIDOpIdx))) {
                      DxilResource &res = GetUAV(resRangeID->getLimitedValue());
                      Value *GV = res.GetGlobalSymbol();
                      llvm::Type *Ty = GV->getType()->getPointerElementType();
                      // Check if uav load is multi component load.
                      if (!IsResourceSingleType(Ty)) {
                        hasMulticomponentUAVLoads = true;
                        break;
                      }
                    }
                  }
                }
              }
            }

          } break;
          case DXIL::OpCode::Fma:
            hasDoubleExtension |= isDouble;
            break;
          case DXIL::OpCode::InnerCoverage:
            hasInnerCoverage = true;
            break;
          case DXIL::OpCode::ViewID:
            hasViewID = true;
            break;
          default:
            // Normal opcodes.
            break;
          }
        }
      }
    }

  }

  Flags.SetEnableDoublePrecision(hasDouble);
  Flags.SetInt64Ops(has64Int);
  Flags.SetEnableMinPrecision(has16FloatInt);
  Flags.SetEnableDoubleExtensions(hasDoubleExtension);
  Flags.SetWaveOps(hasWaveOps);
  Flags.SetTiledResources(hasCheckAccessFully);
  Flags.SetEnableMSAD(hasMSAD);
  Flags.SetUAVLoadAdditionalFormats(hasMulticomponentUAVLoads);
  Flags.SetViewID(hasViewID);

  const ShaderModel *SM = GetShaderModel();
  if (SM->IsPS()) {
    bool hasStencilRef = false;
    DxilSignature &outS = GetOutputSignature();
    for (auto &&E : outS.GetElements()) {
      if (E->GetKind() == Semantic::Kind::StencilRef) {
        hasStencilRef = true;
      } else if (E->GetKind() == Semantic::Kind::InnerCoverage) {
        hasInnerCoverage = true;
      }
    }

    Flags.SetStencilRef(hasStencilRef);
    Flags.SetInnerCoverage(hasInnerCoverage);
  }

  bool checkInputRTArrayIndex =
      SM->IsGS() || SM->IsDS() || SM->IsHS() || SM->IsPS();
  if (checkInputRTArrayIndex) {
    bool hasViewportArrayIndex = false;
    bool hasRenderTargetArrayIndex = false;
    DxilSignature &inS = GetInputSignature();
    for (auto &E : inS.GetElements()) {
      if (E->GetKind() == Semantic::Kind::ViewPortArrayIndex) {
        hasViewportArrayIndex = true;
      } else if (E->GetKind() == Semantic::Kind::RenderTargetArrayIndex) {
        hasRenderTargetArrayIndex = true;
      }
    }
    Flags.SetViewportAndRTArrayIndex(hasViewportArrayIndex |
                                     hasRenderTargetArrayIndex);
  }

  bool checkOutputRTArrayIndex =
      SM->IsVS() || SM->IsDS() || SM->IsHS() || SM->IsPS();
  if (checkOutputRTArrayIndex) {
    bool hasViewportArrayIndex = false;
    bool hasRenderTargetArrayIndex = false;
    DxilSignature &outS = GetOutputSignature();
    for (auto &E : outS.GetElements()) {
      if (E->GetKind() == Semantic::Kind::ViewPortArrayIndex) {
        hasViewportArrayIndex = true;
      } else if (E->GetKind() == Semantic::Kind::RenderTargetArrayIndex) {
        hasRenderTargetArrayIndex = true;
      }
    }
    Flags.SetViewportAndRTArrayIndex(hasViewportArrayIndex |
                                     hasRenderTargetArrayIndex);
  }

  unsigned NumUAVs = m_UAVs.size();
  const unsigned kSmallUAVCount = 8;
  if (NumUAVs > kSmallUAVCount)
    Flags.Set64UAVs(true);
  if (NumUAVs && !(SM->IsCS() || SM->IsPS()))
    Flags.SetUAVsAtEveryStage(true);

  bool hasRawAndStructuredBuffer = false;

  for (auto &UAV : m_UAVs) {
    if (UAV->IsROV())
      Flags.SetROVs(true);
    switch (UAV->GetKind()) {
    case DXIL::ResourceKind::RawBuffer:
    case DXIL::ResourceKind::StructuredBuffer:
      hasRawAndStructuredBuffer = true;
      break;
    default:
      // Not raw/structured.
      break;
    }
  }
  for (auto &SRV : m_SRVs) {
    switch (SRV->GetKind()) {
    case DXIL::ResourceKind::RawBuffer:
    case DXIL::ResourceKind::StructuredBuffer:
      hasRawAndStructuredBuffer = true;
      break;
    default:
      // Not raw/structured.
      break;
    }
  }
  
  Flags.SetEnableRawAndStructuredBuffers(hasRawAndStructuredBuffer);

  bool hasCSRawAndStructuredViaShader4X =
      hasRawAndStructuredBuffer && m_pSM->GetMajor() == 4 && m_pSM->IsCS();
  Flags.SetCSRawAndStructuredViaShader4X(hasCSRawAndStructuredViaShader4X);
}

void DxilModule::CollectShaderFlags() {
  CollectShaderFlags(m_ShaderFlags);
}

uint64_t DxilModule::ShaderFlags::GetShaderFlagsRawForCollection() {
  // This should be all the flags that can be set by DxilModule::CollectShaderFlags.
  ShaderFlags Flags;
  Flags.SetEnableDoublePrecision(true);
  Flags.SetInt64Ops(true);
  Flags.SetEnableMinPrecision(true);
  Flags.SetEnableDoubleExtensions(true);
  Flags.SetWaveOps(true);
  Flags.SetTiledResources(true);
  Flags.SetEnableMSAD(true);
  Flags.SetUAVLoadAdditionalFormats(true);
  Flags.SetStencilRef(true);
  Flags.SetInnerCoverage(true);
  Flags.SetViewportAndRTArrayIndex(true);
  Flags.Set64UAVs(true);
  Flags.SetUAVsAtEveryStage(true);
  Flags.SetEnableRawAndStructuredBuffers(true);
  Flags.SetCSRawAndStructuredViaShader4X(true);
  Flags.SetViewID(true);
  Flags.SetBarycentrics(true);
  return Flags.GetShaderFlagsRaw();
}

DXIL::InputPrimitive DxilModule::GetInputPrimitive() const {
  return m_InputPrimitive;
}

void DxilModule::SetInputPrimitive(DXIL::InputPrimitive IP) {
  DXASSERT_NOMSG(m_InputPrimitive == DXIL::InputPrimitive::Undefined);
  DXASSERT_NOMSG(DXIL::InputPrimitive::Undefined < IP && IP < DXIL::InputPrimitive::LastEntry);
  m_InputPrimitive = IP;
}

unsigned DxilModule::GetMaxVertexCount() const {
  DXASSERT_NOMSG(m_MaxVertexCount != 0);
  return m_MaxVertexCount;
}

void DxilModule::SetMaxVertexCount(unsigned Count) {
  DXASSERT_NOMSG(m_MaxVertexCount == 0);
  m_MaxVertexCount = Count;
}

DXIL::PrimitiveTopology DxilModule::GetStreamPrimitiveTopology() const {
  return m_StreamPrimitiveTopology;
}

void DxilModule::SetStreamPrimitiveTopology(DXIL::PrimitiveTopology Topology) {
  m_StreamPrimitiveTopology = Topology;
}

bool DxilModule::HasMultipleOutputStreams() const {
  if (!m_pSM->IsGS()) {
    return false;
  } else {
    unsigned NumStreams = (m_ActiveStreamMask & 0x1) + 
                          ((m_ActiveStreamMask & 0x2) >> 1) + 
                          ((m_ActiveStreamMask & 0x4) >> 2) + 
                          ((m_ActiveStreamMask & 0x8) >> 3);
    DXASSERT_NOMSG(NumStreams <= DXIL::kNumOutputStreams);
    return NumStreams > 1;
  }
}

unsigned DxilModule::GetOutputStream() const {
  if (!m_pSM->IsGS()) {
    return 0;
  } else {
    DXASSERT_NOMSG(!HasMultipleOutputStreams());
    switch (m_ActiveStreamMask) {
    case 0x1: return 0;
    case 0x2: return 1;
    case 0x4: return 2;
    case 0x8: return 3;
    default: DXASSERT_NOMSG(false);
    }
    return (unsigned)(-1);
  }
}

unsigned DxilModule::GetGSInstanceCount() const {
  return m_NumGSInstances;
}

void DxilModule::SetGSInstanceCount(unsigned Count) {
  m_NumGSInstances = Count;
}

bool DxilModule::IsStreamActive(unsigned Stream) const {
  return (m_ActiveStreamMask & (1<<Stream)) != 0;
}

void DxilModule::SetStreamActive(unsigned Stream, bool bActive) {
  if (bActive) {
    m_ActiveStreamMask |= (1<<Stream);
  } else {
    m_ActiveStreamMask &= ~(1<<Stream);
  }
}

void DxilModule::SetActiveStreamMask(unsigned Mask) {
  m_ActiveStreamMask = Mask;
}

unsigned DxilModule::GetActiveStreamMask() const {
  return m_ActiveStreamMask;
}

unsigned DxilModule::GetInputControlPointCount() const {
  return m_InputControlPointCount;
}

void DxilModule::SetInputControlPointCount(unsigned NumICPs) {
  m_InputControlPointCount = NumICPs;
}

DXIL::TessellatorDomain DxilModule::GetTessellatorDomain() const {
  return m_TessellatorDomain;
}

void DxilModule::SetTessellatorDomain(DXIL::TessellatorDomain TessDomain) {
  m_TessellatorDomain = TessDomain;
}

unsigned DxilModule::GetOutputControlPointCount() const {
  return m_OutputControlPointCount;
}

void DxilModule::SetOutputControlPointCount(unsigned NumOCPs) {
  m_OutputControlPointCount = NumOCPs;
}

DXIL::TessellatorPartitioning DxilModule::GetTessellatorPartitioning() const {
  return m_TessellatorPartitioning;
}

void DxilModule::SetTessellatorPartitioning(DXIL::TessellatorPartitioning TessPartitioning) {
  m_TessellatorPartitioning = TessPartitioning;
}

DXIL::TessellatorOutputPrimitive DxilModule::GetTessellatorOutputPrimitive() const {
  return m_TessellatorOutputPrimitive;
}

void DxilModule::SetTessellatorOutputPrimitive(DXIL::TessellatorOutputPrimitive TessOutputPrimitive) {
  m_TessellatorOutputPrimitive = TessOutputPrimitive;
}

float DxilModule::GetMaxTessellationFactor() const {
  return m_MaxTessellationFactor;
}

void DxilModule::SetMaxTessellationFactor(float MaxTessellationFactor) {
  m_MaxTessellationFactor = MaxTessellationFactor;
}

template<typename T> unsigned 
DxilModule::AddResource(vector<unique_ptr<T> > &Vec, unique_ptr<T> pRes) {
  DXASSERT_NOMSG((unsigned)Vec.size() < UINT_MAX);
  unsigned Id = (unsigned)Vec.size();
  Vec.emplace_back(std::move(pRes));
  return Id;
}

unsigned DxilModule::AddCBuffer(unique_ptr<DxilCBuffer> pCB) {
  return AddResource<DxilCBuffer>(m_CBuffers, std::move(pCB));
}

DxilCBuffer &DxilModule::GetCBuffer(unsigned idx) {
  return *m_CBuffers[idx];
}

const DxilCBuffer &DxilModule::GetCBuffer(unsigned idx) const {
  return *m_CBuffers[idx];
}

const vector<unique_ptr<DxilCBuffer> > &DxilModule::GetCBuffers() const {
  return m_CBuffers;
}

unsigned DxilModule::AddSampler(unique_ptr<DxilSampler> pSampler) {
  return AddResource<DxilSampler>(m_Samplers, std::move(pSampler));
}

DxilSampler &DxilModule::GetSampler(unsigned idx) {
  return *m_Samplers[idx];
}

const DxilSampler &DxilModule::GetSampler(unsigned idx) const {
  return *m_Samplers[idx];
}

const vector<unique_ptr<DxilSampler> > &DxilModule::GetSamplers() const {
  return m_Samplers;
}

unsigned DxilModule::AddSRV(unique_ptr<DxilResource> pSRV) {
  return AddResource<DxilResource>(m_SRVs, std::move(pSRV));
}

DxilResource &DxilModule::GetSRV(unsigned idx) {
  return *m_SRVs[idx];
}

const DxilResource &DxilModule::GetSRV(unsigned idx) const {
  return *m_SRVs[idx];
}

const vector<unique_ptr<DxilResource> > &DxilModule::GetSRVs() const {
  return m_SRVs;
}

unsigned DxilModule::AddUAV(unique_ptr<DxilResource> pUAV) {
  return AddResource<DxilResource>(m_UAVs, std::move(pUAV));
}

DxilResource &DxilModule::GetUAV(unsigned idx) {
  return *m_UAVs[idx];
}

const DxilResource &DxilModule::GetUAV(unsigned idx) const {
  return *m_UAVs[idx];
}

const vector<unique_ptr<DxilResource> > &DxilModule::GetUAVs() const {
  return m_UAVs;
}

void DxilModule::LoadDxilResourceBaseFromMDNode(MDNode *MD, DxilResourceBase &R) {
  return m_pMDHelper->LoadDxilResourceBaseFromMDNode(MD, R);
}
void DxilModule::LoadDxilResourceFromMDNode(llvm::MDNode *MD, DxilResource &R) {
  return m_pMDHelper->LoadDxilResourceFromMDNode(MD, R);
}
void DxilModule::LoadDxilSamplerFromMDNode(llvm::MDNode *MD, DxilSampler &S) {
  return m_pMDHelper->LoadDxilSamplerFromMDNode(MD, S);
}

template <typename TResource>
static void RemoveResources(std::vector<std::unique_ptr<TResource>> &vec,
                    std::unordered_set<unsigned> &immResID) {
  for (std::vector<std::unique_ptr<TResource>>::iterator p = vec.begin(); p != vec.end();) {
    std::vector<std::unique_ptr<TResource>>::iterator c = p++;
    if (immResID.count((*c)->GetID()) == 0) {
      p = vec.erase(c);
    }
  }
}

static void CollectUsedResource(Value *resID,
                                std::unordered_set<Value *> &usedResID) {
  if (usedResID.count(resID) > 0)
    return;

  usedResID.insert(resID);
  if (ConstantInt *cResID = dyn_cast<ConstantInt>(resID)) {
    // Do nothing
  } else if (ZExtInst *ZEI = dyn_cast<ZExtInst>(resID)) {
    if (ZEI->getSrcTy()->isIntegerTy()) {
      IntegerType *ITy = cast<IntegerType>(ZEI->getSrcTy());
      if (ITy->getBitWidth() == 1) {
        usedResID.insert(ConstantInt::get(ZEI->getDestTy(), 0));
        usedResID.insert(ConstantInt::get(ZEI->getDestTy(), 1));
      }
    }
  } else if (SelectInst *SI = dyn_cast<SelectInst>(resID)) {
    CollectUsedResource(SI->getTrueValue(), usedResID);
    CollectUsedResource(SI->getFalseValue(), usedResID);
  } else {
    PHINode *Phi = cast<PHINode>(resID);
    for (Use &U : Phi->incoming_values()) {
      CollectUsedResource(U.get(), usedResID);
    }
  }
}

static void ConvertUsedResource(std::unordered_set<unsigned> &immResID,
                                std::unordered_set<Value *> &usedResID) {
  for (Value *V : usedResID) {
    if (ConstantInt *cResID = dyn_cast<ConstantInt>(V)) {
      immResID.insert(cResID->getLimitedValue());
    }
  }
}

void DxilModule::RemoveFunction(llvm::Function *F) {
  DXASSERT_NOMSG(F != nullptr);
  if (m_pTypeSystem.get()->GetFunctionAnnotation(F))
    m_pTypeSystem.get()->EraseFunctionAnnotation(F);
  m_pOP->RemoveFunction(F);
}

void DxilModule::RemoveUnusedResources() {
  hlsl::OP *hlslOP = GetOP();
  Function *createHandleFunc = hlslOP->GetOpFunc(DXIL::OpCode::CreateHandle, Type::getVoidTy(GetCtx()));
  if (createHandleFunc->user_empty()) {
    m_CBuffers.clear();
    m_UAVs.clear();
    m_SRVs.clear();
    m_Samplers.clear();
    createHandleFunc->eraseFromParent();
    return;
  }

  std::unordered_set<Value *> usedUAVID;
  std::unordered_set<Value *> usedSRVID;
  std::unordered_set<Value *> usedSamplerID;
  std::unordered_set<Value *> usedCBufID;
  // Collect used ID.
  for (User *U : createHandleFunc->users()) {
    CallInst *CI = cast<CallInst>(U);
    Value *vResClass =
        CI->getArgOperand(DXIL::OperandIndex::kCreateHandleResClassOpIdx);
    ConstantInt *cResClass = cast<ConstantInt>(vResClass);
    DXIL::ResourceClass resClass =
        static_cast<DXIL::ResourceClass>(cResClass->getLimitedValue());
    // Skip unused resource handle.
    if (CI->user_empty())
      continue;

    Value *resID =
        CI->getArgOperand(DXIL::OperandIndex::kCreateHandleResIDOpIdx);
    switch (resClass) {
    case DXIL::ResourceClass::CBuffer:
      CollectUsedResource(resID, usedCBufID);
      break;
    case DXIL::ResourceClass::Sampler:
      CollectUsedResource(resID, usedSamplerID);
      break;
    case DXIL::ResourceClass::SRV:
      CollectUsedResource(resID, usedSRVID);
      break;
    case DXIL::ResourceClass::UAV:
      CollectUsedResource(resID, usedUAVID);
      break;
    default:
      DXASSERT(0, "invalid res class");
      break;
    }
  }

  std::unordered_set<unsigned> immUAVID;
  std::unordered_set<unsigned> immSRVID;
  std::unordered_set<unsigned> immSamplerID;
  std::unordered_set<unsigned> immCBufID;
  ConvertUsedResource(immUAVID, usedUAVID);
  RemoveResources(m_UAVs, immUAVID);
  ConvertUsedResource(immSRVID, usedSRVID);
  ConvertUsedResource(immSamplerID, usedSamplerID);
  ConvertUsedResource(immCBufID, usedCBufID);

  RemoveResources(m_SRVs, immSRVID);
  RemoveResources(m_Samplers, immSamplerID);
  RemoveResources(m_CBuffers, immCBufID);
}

DxilSignature &DxilModule::GetInputSignature() {
  return *m_InputSignature;
}

const DxilSignature &DxilModule::GetInputSignature() const {
  return *m_InputSignature;
}

DxilSignature &DxilModule::GetOutputSignature() {
  return *m_OutputSignature;
}

const DxilSignature &DxilModule::GetOutputSignature() const {
  return *m_OutputSignature;
}

DxilSignature &DxilModule::GetPatchConstantSignature() {
  return *m_PatchConstantSignature;
}

const DxilSignature &DxilModule::GetPatchConstantSignature() const {
  return *m_PatchConstantSignature;
}

const RootSignatureHandle &DxilModule::GetRootSignature() const {
  return *m_RootSignature;
}

void DxilModule::StripRootSignatureFromMetadata() {
  NamedMDNode *pRootSignatureNamedMD = GetModule()->getNamedMetadata(DxilMDHelper::kDxilRootSignatureMDName);
  if (pRootSignatureNamedMD) {
    GetModule()->eraseNamedMetadata(pRootSignatureNamedMD);
  }
}

void DxilModule::UpdateValidatorVersionMetadata() {
  m_pMDHelper->EmitValidatorVersion(m_ValMajor, m_ValMinor);
}

void DxilModule::ResetInputSignature(DxilSignature *pValue) {
  m_InputSignature.reset(pValue);
}

void DxilModule::ResetOutputSignature(DxilSignature *pValue) {
  m_OutputSignature.reset(pValue);
}

void DxilModule::ResetPatchConstantSignature(DxilSignature *pValue) {
  m_PatchConstantSignature.reset(pValue);
}

void DxilModule::ResetRootSignature(RootSignatureHandle *pValue) {
  m_RootSignature.reset(pValue);
}

DxilTypeSystem &DxilModule::GetTypeSystem() {
  return *m_pTypeSystem;
}

DxilViewIdState &DxilModule::GetViewIdState() {
  return *m_pViewIdState;
}
const DxilViewIdState &DxilModule::GetViewIdState() const {
  return *m_pViewIdState;
}

void DxilModule::ResetTypeSystem(DxilTypeSystem *pValue) {
  m_pTypeSystem.reset(pValue);
}

void DxilModule::ResetOP(hlsl::OP *hlslOP) {
  m_pOP.reset(hlslOP);
}

void DxilModule::EmitLLVMUsed() {
  if (m_LLVMUsed.empty())
    return;

  vector<llvm::Constant*> GVs;
  Type *pI8PtrType = Type::getInt8PtrTy(m_Ctx, DXIL::kDefaultAddrSpace);

  GVs.resize(m_LLVMUsed.size());
  for (size_t i = 0, e = m_LLVMUsed.size(); i != e; i++) {
    Constant *pConst = cast<Constant>(&*m_LLVMUsed[i]);
    PointerType * pPtrType = dyn_cast<PointerType>(pConst->getType());
    if (pPtrType->getPointerAddressSpace() != DXIL::kDefaultAddrSpace) {
      // Cast pointer to addrspace 0, as LLVMUsed elements must have the same type.
      GVs[i] = ConstantExpr::getAddrSpaceCast(pConst, pI8PtrType);
    } else {
      GVs[i] = ConstantExpr::getPointerCast(pConst, pI8PtrType);
    }
  }

  ArrayType *pATy = ArrayType::get(pI8PtrType, GVs.size());

  StringRef llvmUsedName = "llvm.used";

  if (GlobalVariable *oldGV = m_pModule->getGlobalVariable(llvmUsedName)) {
    oldGV->eraseFromParent();
  }

  GlobalVariable *pGV = new GlobalVariable(*m_pModule, pATy, false,
                                           GlobalValue::AppendingLinkage,
                                           ConstantArray::get(pATy, GVs),
                                           llvmUsedName);

  pGV->setSection("llvm.metadata");
}

vector<GlobalVariable* > &DxilModule::GetLLVMUsed() {
  return m_LLVMUsed;
}

// DXIL metadata serialization/deserialization.
void DxilModule::EmitDxilMetadata() {
  m_pMDHelper->EmitDxilVersion(m_DxilMajor, m_DxilMinor);
  m_pMDHelper->EmitValidatorVersion(m_ValMajor, m_ValMinor);
  m_pMDHelper->EmitDxilShaderModel(m_pSM);

  MDTuple *pMDProperties = EmitDxilShaderProperties();

  MDTuple *pMDSignatures = m_pMDHelper->EmitDxilSignatures(*m_InputSignature, 
                                                           *m_OutputSignature,
                                                           *m_PatchConstantSignature);
  MDTuple *pMDResources = EmitDxilResources();
  m_pMDHelper->EmitDxilTypeSystem(GetTypeSystem(), m_LLVMUsed);
  if (!m_pSM->IsCS() &&
      (m_ValMajor > 1 || (m_ValMajor == 1 && m_ValMinor >= 1))) {
    m_pMDHelper->EmitDxilViewIdState(GetViewIdState());
  }
  EmitLLVMUsed();
  MDTuple *pEntry = m_pMDHelper->EmitDxilEntryPointTuple(GetEntryFunction(), m_EntryName, pMDSignatures, pMDResources, pMDProperties);
  vector<MDNode *> Entries;
  Entries.emplace_back(pEntry);
  m_pMDHelper->EmitDxilEntryPoints(Entries);

  if (!m_RootSignature->IsEmpty()) {
    m_pMDHelper->EmitRootSignature(*m_RootSignature.get());
  }
}

bool DxilModule::IsKnownNamedMetaData(llvm::NamedMDNode &Node) {
  return DxilMDHelper::IsKnownNamedMetaData(Node);
}

void DxilModule::LoadDxilMetadata() {
  m_pMDHelper->LoadDxilVersion(m_DxilMajor, m_DxilMinor);
  m_pMDHelper->LoadValidatorVersion(m_ValMajor, m_ValMinor);
  const ShaderModel *loadedModule;
  m_pMDHelper->LoadDxilShaderModel(loadedModule);
  SetShaderModel(loadedModule);
  DXASSERT(m_InputSignature != nullptr, "else SetShaderModel didn't create input signature");

  const llvm::NamedMDNode *pEntries = m_pMDHelper->GetDxilEntryPoints();
  IFTBOOL(pEntries->getNumOperands() == 1, DXC_E_INCORRECT_DXIL_METADATA);

  Function *pEntryFunc;
  string EntryName;
  const llvm::MDOperand *pSignatures, *pResources, *pProperties;
  m_pMDHelper->GetDxilEntryPoint(pEntries->getOperand(0), pEntryFunc, EntryName, pSignatures, pResources, pProperties);

  SetEntryFunction(pEntryFunc);
  SetEntryFunctionName(EntryName);

  LoadDxilShaderProperties(*pProperties);

  m_pMDHelper->LoadDxilSignatures(*pSignatures, *m_InputSignature,
                                  *m_OutputSignature, *m_PatchConstantSignature);
  LoadDxilResources(*pResources);

  m_pMDHelper->LoadDxilTypeSystem(*m_pTypeSystem.get());

  m_pMDHelper->LoadRootSignature(*m_RootSignature.get());

  m_pMDHelper->LoadDxilViewIdState(*m_pViewIdState.get());
}

MDTuple *DxilModule::EmitDxilResources() {
  // Emit SRV records.
  MDTuple *pTupleSRVs = nullptr;
  if (!m_SRVs.empty()) {
    vector<Metadata *> MDVals;
    for (size_t i = 0; i < m_SRVs.size(); i++) {
      MDVals.emplace_back(m_pMDHelper->EmitDxilSRV(*m_SRVs[i]));
    }
    pTupleSRVs = MDNode::get(m_Ctx, MDVals);
  }

  // Emit UAV records.
  MDTuple *pTupleUAVs = nullptr;
  if (!m_UAVs.empty()) {
    vector<Metadata *> MDVals;
    for (size_t i = 0; i < m_UAVs.size(); i++) {
      MDVals.emplace_back(m_pMDHelper->EmitDxilUAV(*m_UAVs[i]));
    }
    pTupleUAVs = MDNode::get(m_Ctx, MDVals);
  }

  // Emit CBuffer records.
  MDTuple *pTupleCBuffers = nullptr;
  if (!m_CBuffers.empty()) {
    vector<Metadata *> MDVals;
    for (size_t i = 0; i < m_CBuffers.size(); i++) {
      MDVals.emplace_back(m_pMDHelper->EmitDxilCBuffer(*m_CBuffers[i]));
    }
    pTupleCBuffers = MDNode::get(m_Ctx, MDVals);
  }

  // Emit Sampler records.
  MDTuple *pTupleSamplers = nullptr;
  if (!m_Samplers.empty()) {
    vector<Metadata *> MDVals;
    for (size_t i = 0; i < m_Samplers.size(); i++) {
      MDVals.emplace_back(m_pMDHelper->EmitDxilSampler(*m_Samplers[i]));
    }
    pTupleSamplers = MDNode::get(m_Ctx, MDVals);
  }

  if (pTupleSRVs != nullptr || pTupleUAVs != nullptr || pTupleCBuffers != nullptr || pTupleSamplers != nullptr) {
    return m_pMDHelper->EmitDxilResourceTuple(pTupleSRVs, pTupleUAVs, pTupleCBuffers, pTupleSamplers);
  } else {
    return nullptr;
  }
}

void DxilModule::LoadDxilResources(const llvm::MDOperand &MDO) {
  if (MDO.get() == nullptr)
    return;

  const llvm::MDTuple *pSRVs, *pUAVs, *pCBuffers, *pSamplers;
  m_pMDHelper->GetDxilResources(MDO, pSRVs, pUAVs, pCBuffers, pSamplers);

  // Load SRV records.
  if (pSRVs != nullptr) {
    for (unsigned i = 0; i < pSRVs->getNumOperands(); i++) {
      unique_ptr<DxilResource> pSRV(new DxilResource);
      m_pMDHelper->LoadDxilSRV(pSRVs->getOperand(i), *pSRV);
      AddSRV(std::move(pSRV));
    }
  }

  // Load UAV records.
  if (pUAVs != nullptr) {
    for (unsigned i = 0; i < pUAVs->getNumOperands(); i++) {
      unique_ptr<DxilResource> pUAV(new DxilResource);
      m_pMDHelper->LoadDxilUAV(pUAVs->getOperand(i), *pUAV);
      AddUAV(std::move(pUAV));
    }
  }

  // Load CBuffer records.
  if (pCBuffers != nullptr) {
    for (unsigned i = 0; i < pCBuffers->getNumOperands(); i++) {
      unique_ptr<DxilCBuffer> pCB(new DxilCBuffer);
      m_pMDHelper->LoadDxilCBuffer(pCBuffers->getOperand(i), *pCB);
      AddCBuffer(std::move(pCB));
    }
  }

  // Load Sampler records.
  if (pSamplers != nullptr) {
    for (unsigned i = 0; i < pSamplers->getNumOperands(); i++) {
      unique_ptr<DxilSampler> pSampler(new DxilSampler);
      m_pMDHelper->LoadDxilSampler(pSamplers->getOperand(i), *pSampler);
      AddSampler(std::move(pSampler));
    }
  }
}

MDTuple *DxilModule::EmitDxilShaderProperties() {
  vector<Metadata *> MDVals;

  // DXIL shader flags.
  uint64_t Flags = m_ShaderFlags.GetShaderFlagsRaw();
  if (Flags != 0) {
    MDVals.emplace_back(m_pMDHelper->Uint32ToConstMD(DxilMDHelper::kDxilShaderFlagsTag));
    MDVals.emplace_back(m_pMDHelper->Uint64ToConstMD(Flags));
  }

  // Compute shader.
  if (m_pSM->IsCS()) {
    MDVals.emplace_back(m_pMDHelper->Uint32ToConstMD(DxilMDHelper::kDxilNumThreadsTag));
    vector<Metadata *> NumThreadVals;
    NumThreadVals.emplace_back(m_pMDHelper->Uint32ToConstMD(m_NumThreads[0]));
    NumThreadVals.emplace_back(m_pMDHelper->Uint32ToConstMD(m_NumThreads[1]));
    NumThreadVals.emplace_back(m_pMDHelper->Uint32ToConstMD(m_NumThreads[2]));
    MDVals.emplace_back(MDNode::get(m_Ctx, NumThreadVals));
  }

  // Geometry shader.
  if (m_pSM->IsGS()) {
    MDVals.emplace_back(m_pMDHelper->Uint32ToConstMD(DxilMDHelper::kDxilGSStateTag));
    MDTuple *pMDTuple = m_pMDHelper->EmitDxilGSState(m_InputPrimitive,
                                                     m_MaxVertexCount,
                                                     GetActiveStreamMask(),
                                                     m_StreamPrimitiveTopology,
                                                     m_NumGSInstances);
    MDVals.emplace_back(pMDTuple);
  }

  // Domain shader.
  if (m_pSM->IsDS()) {
    MDVals.emplace_back(m_pMDHelper->Uint32ToConstMD(DxilMDHelper::kDxilDSStateTag));
    MDTuple *pMDTuple = m_pMDHelper->EmitDxilDSState(m_TessellatorDomain,
                                                     m_InputControlPointCount);
    MDVals.emplace_back(pMDTuple);
  }

  // Hull shader.
  if (m_pSM->IsHS()) {
    MDVals.emplace_back(m_pMDHelper->Uint32ToConstMD(DxilMDHelper::kDxilHSStateTag));
    MDTuple *pMDTuple = m_pMDHelper->EmitDxilHSState(m_pPatchConstantFunc,
                                                     m_InputControlPointCount,
                                                     m_OutputControlPointCount,
                                                     m_TessellatorDomain,
                                                     m_TessellatorPartitioning,
                                                     m_TessellatorOutputPrimitive,
                                                     m_MaxTessellationFactor);
    MDVals.emplace_back(pMDTuple);
  }

  if (!MDVals.empty())
    return MDNode::get(m_Ctx, MDVals);
  else
    return nullptr;
}

void DxilModule::LoadDxilShaderProperties(const MDOperand &MDO) {
  if (MDO.get() == nullptr)
    return;

  const MDTuple *pTupleMD = dyn_cast<MDTuple>(MDO.get());
  IFTBOOL(pTupleMD != nullptr, DXC_E_INCORRECT_DXIL_METADATA);
  IFTBOOL((pTupleMD->getNumOperands() & 0x1) == 0, DXC_E_INCORRECT_DXIL_METADATA);

  for (unsigned iNode = 0; iNode < pTupleMD->getNumOperands(); iNode += 2) {
    unsigned Tag = DxilMDHelper::ConstMDToUint32(pTupleMD->getOperand(iNode));
    const MDOperand &MDO = pTupleMD->getOperand(iNode + 1);
    IFTBOOL(MDO.get() != nullptr, DXC_E_INCORRECT_DXIL_METADATA);

    switch (Tag) {
    case DxilMDHelper::kDxilShaderFlagsTag:
      m_ShaderFlags.SetShaderFlagsRaw(DxilMDHelper::ConstMDToUint64(MDO));
      break;

    case DxilMDHelper::kDxilNumThreadsTag: {
      MDNode *pNode = cast<MDNode>(MDO.get());
      m_NumThreads[0] = DxilMDHelper::ConstMDToUint32(pNode->getOperand(0));
      m_NumThreads[1] = DxilMDHelper::ConstMDToUint32(pNode->getOperand(1));
      m_NumThreads[2] = DxilMDHelper::ConstMDToUint32(pNode->getOperand(2));
      break;
    }

    case DxilMDHelper::kDxilGSStateTag: {
      m_pMDHelper->LoadDxilGSState(MDO, m_InputPrimitive, m_MaxVertexCount, m_ActiveStreamMask, 
                                   m_StreamPrimitiveTopology, m_NumGSInstances);
      break;
    }

    case DxilMDHelper::kDxilDSStateTag:
      m_pMDHelper->LoadDxilDSState(MDO, m_TessellatorDomain, m_InputControlPointCount);
      break;

    case DxilMDHelper::kDxilHSStateTag:
      m_pMDHelper->LoadDxilHSState(MDO,
                                   m_pPatchConstantFunc,
                                   m_InputControlPointCount,
                                   m_OutputControlPointCount,
                                   m_TessellatorDomain,
                                   m_TessellatorPartitioning,
                                   m_TessellatorOutputPrimitive,
                                   m_MaxTessellationFactor);
      break;

    default:
      DXASSERT(false, "Unknown extended shader properties tag");
      break;
    }
  }
}

void DxilModule::StripDebugRelatedCode() {
  // Remove all users of global resources.
  for (GlobalVariable &GV : m_pModule->globals()) {
    if (GV.hasInternalLinkage())
      continue;
    if (GV.getType()->getPointerAddressSpace() == DXIL::kTGSMAddrSpace)
      continue;
    for (auto git = GV.user_begin(); git != GV.user_end();) {
      User *U = *(git++);
      // Try to remove load of GV.
      if (LoadInst *LI = dyn_cast<LoadInst>(U)) {
        for (auto it = LI->user_begin(); it != LI->user_end();) {
          Instruction *LIUser = cast<Instruction>(*(it++));
          if (StoreInst *SI = dyn_cast<StoreInst>(LIUser)) {
            Value *Ptr = SI->getPointerOperand();
            SI->eraseFromParent();
            if (Instruction *PtrInst = dyn_cast<Instruction>(Ptr)) {
              if (Ptr->user_empty())
                PtrInst->eraseFromParent();
            }
          }
        }
        if (LI->user_empty())
          LI->eraseFromParent();
      } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(U)) {
        for (auto GEPIt = GEP->user_begin(); GEPIt != GEP->user_end();) {
          User *GEPU = *(GEPIt++);
          // Try to remove load of GEP.
          if (LoadInst *LI = dyn_cast<LoadInst>(GEPU)) {
            for (auto it = LI->user_begin(); it != LI->user_end();) {
              Instruction *LIUser = cast<Instruction>(*(it++));
              if (StoreInst *SI = dyn_cast<StoreInst>(LIUser)) {
                Value *Ptr = SI->getPointerOperand();
                SI->eraseFromParent();
                if (Instruction *PtrInst = dyn_cast<Instruction>(Ptr)) {
                  if (Ptr->user_empty())
                    PtrInst->eraseFromParent();
                }
              }
              if (LI->user_empty())
                LI->eraseFromParent();
            }
          }
        }
        if (GEP->user_empty())
          GEP->eraseFromParent();
      }
    }
  }
}
DebugInfoFinder &DxilModule::GetOrCreateDebugInfoFinder() {
  if (m_pDebugInfoFinder == nullptr) {
    m_pDebugInfoFinder = std::make_unique<llvm::DebugInfoFinder>();
    m_pDebugInfoFinder->processModule(*m_pModule);
  }
  return *m_pDebugInfoFinder;
}

hlsl::DxilModule *hlsl::DxilModule::TryGetDxilModule(llvm::Module *pModule) {
  LLVMContext &Ctx = pModule->getContext();
  std::string diagStr;
  raw_string_ostream diagStream(diagStr);

  hlsl::DxilModule *pDxilModule = nullptr;
  // TODO: add detail error in DxilMDHelper.
  try {
    pDxilModule = &pModule->GetOrCreateDxilModule();
  } catch (const ::hlsl::Exception &hlslException) {
    diagStream << "load dxil metadata failed -";
    try {
      const char *msg = hlslException.what();
      if (msg == nullptr || *msg == '\0')
        diagStream << " error code " << hlslException.hr << "\n";
      else
        diagStream << msg;
    } catch (...) {
      diagStream << " unable to retrieve error message.\n";
    }
    Ctx.diagnose(DxilErrorDiagnosticInfo(diagStream.str().c_str()));
  } catch (...) {
    Ctx.diagnose(DxilErrorDiagnosticInfo("load dxil metadata failed - unknown error.\n"));
  }
  return pDxilModule;
}

// Check if the instruction has fast math flags configured to indicate
// the instruction is precise.
// Precise fast math flags means none of the fast math flags are set.
bool DxilModule::HasPreciseFastMathFlags(const Instruction *inst) {
  return isa<FPMathOperator>(inst) && !inst->getFastMathFlags().any();
}

// Set fast math flags configured to indicate the instruction is precise.
void DxilModule::SetPreciseFastMathFlags(llvm::Instruction *inst) {
  assert(isa<FPMathOperator>(inst));
  inst->copyFastMathFlags(FastMathFlags());
}

// True if fast math flags are preserved across serialization/deserialization
// of the dxil module.
//
// We need to check for this when querying fast math flags for preciseness
// otherwise we will be overly conservative by reporting instructions precise
// because their fast math flags were not preserved.
//
// Currently we restrict it to the instruction types that have fast math
// preserved in the bitcode. We can expand this by converting fast math
// flags to dx.precise metadata during serialization and back to fast
// math flags during deserialization.
bool DxilModule::PreservesFastMathFlags(const llvm::Instruction *inst) {
  return
    isa<FPMathOperator>(inst) && (isa<BinaryOperator>(inst) || isa<FCmpInst>(inst));
}

bool DxilModule::IsPrecise(const Instruction *inst) const {
  if (m_ShaderFlags.GetDisableMathRefactoring())
    return true;
  else if (DxilMDHelper::IsMarkedPrecise(inst))
    return true;
  else if (PreservesFastMathFlags(inst))
    return HasPreciseFastMathFlags(inst);
  else
    return false;
}

} // namespace hlsl

namespace llvm {
hlsl::DxilModule &Module::GetOrCreateDxilModule(bool skipInit) {
  std::unique_ptr<hlsl::DxilModule> M;
  if (!HasDxilModule()) {
    M = std::make_unique<hlsl::DxilModule>(this);
    if (!skipInit) {
      M->LoadDxilMetadata();
    }
    SetDxilModule(M.release());
  }
  return GetDxilModule();
}

void Module::ResetDxilModule() {
  if (HasDxilModule()) {
    delete TheDxilModule;
    TheDxilModule = nullptr;
  }
}

}
