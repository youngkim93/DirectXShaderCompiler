///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// DxilTypeSystem.cpp                                                        //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/HLSL/DxilTypeSystem.h"
#include "dxc/Support/Global.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using std::unique_ptr;
using std::string;
using std::vector;
using std::map;


namespace hlsl {

//------------------------------------------------------------------------------
//
// DxilMatrixAnnotation class methods.
//
DxilMatrixAnnotation::DxilMatrixAnnotation()
: Rows(0)
, Cols(0)
, Orientation(MatrixOrientation::Undefined) {
}


//------------------------------------------------------------------------------
//
// DxilFieldAnnotation class methods.
//
DxilFieldAnnotation::DxilFieldAnnotation()
: m_bPrecise(false)
, m_ResourceAttribute(nullptr)
, m_CBufferOffset(UINT_MAX) {
}

bool DxilFieldAnnotation::IsPrecise() const { return m_bPrecise; }
void DxilFieldAnnotation::SetPrecise(bool b) { m_bPrecise = b; }
bool DxilFieldAnnotation::HasMatrixAnnotation() const { return m_Matrix.Cols != 0; }
const DxilMatrixAnnotation &DxilFieldAnnotation::GetMatrixAnnotation() const { return m_Matrix; }
void DxilFieldAnnotation::SetMatrixAnnotation(const DxilMatrixAnnotation &MA) { m_Matrix = MA; }
bool DxilFieldAnnotation::HasResourceAttribute() const {
  return m_ResourceAttribute;
}
llvm::MDNode *DxilFieldAnnotation::GetResourceAttribute() const {
  return m_ResourceAttribute;
}
void DxilFieldAnnotation::SetResourceAttribute(llvm::MDNode *MD) {
  m_ResourceAttribute = MD;
}
bool DxilFieldAnnotation::HasCBufferOffset() const { return m_CBufferOffset != UINT_MAX; }
unsigned DxilFieldAnnotation::GetCBufferOffset() const { return m_CBufferOffset; }
void DxilFieldAnnotation::SetCBufferOffset(unsigned Offset) { m_CBufferOffset = Offset; }
bool DxilFieldAnnotation::HasCompType() const { return m_CompType.GetKind() != CompType::Kind::Invalid; }
const CompType &DxilFieldAnnotation::GetCompType() const { return m_CompType; }
void DxilFieldAnnotation::SetCompType(CompType::Kind kind) { m_CompType = CompType(kind); }
bool DxilFieldAnnotation::HasSemanticString() const { return !m_Semantic.empty(); }
const std::string &DxilFieldAnnotation::GetSemanticString() const { return m_Semantic; }
llvm::StringRef DxilFieldAnnotation::GetSemanticStringRef() const { return llvm::StringRef(m_Semantic); }
void DxilFieldAnnotation::SetSemanticString(const std::string &SemString) { m_Semantic = SemString; }
bool DxilFieldAnnotation::HasInterpolationMode() const { return !m_InterpMode.IsUndefined(); }
const InterpolationMode &DxilFieldAnnotation::GetInterpolationMode() const { return m_InterpMode; }
void DxilFieldAnnotation::SetInterpolationMode(const InterpolationMode &IM) { m_InterpMode = IM; }
bool DxilFieldAnnotation::HasFieldName() const { return !m_FieldName.empty(); }
const std::string &DxilFieldAnnotation::GetFieldName() const { return m_FieldName; }
void DxilFieldAnnotation::SetFieldName(const std::string &FieldName) { m_FieldName = FieldName; }


//------------------------------------------------------------------------------
//
// DxilStructAnnotation class methods.
//
unsigned DxilStructAnnotation::GetNumFields() const {
  return (unsigned)m_FieldAnnotations.size();
}

DxilFieldAnnotation &DxilStructAnnotation::GetFieldAnnotation(unsigned FieldIdx) {
  return m_FieldAnnotations[FieldIdx];
}

const DxilFieldAnnotation &DxilStructAnnotation::GetFieldAnnotation(unsigned FieldIdx) const {
  return m_FieldAnnotations[FieldIdx];
}

const StructType *DxilStructAnnotation::GetStructType() const {
  return m_pStructType;
}

unsigned DxilStructAnnotation::GetCBufferSize() const { return m_CBufferSize; }
void DxilStructAnnotation::SetCBufferSize(unsigned size) { m_CBufferSize = size; }
void DxilStructAnnotation::MarkEmptyStruct() { m_FieldAnnotations.clear(); }
bool DxilStructAnnotation::IsEmptyStruct() { return m_FieldAnnotations.empty(); }

//------------------------------------------------------------------------------
//
// DxilParameterAnnotation class methods.
//
DxilParameterAnnotation::DxilParameterAnnotation()
: m_inputQual(DxilParamInputQual::In), DxilFieldAnnotation() {
}

DxilParamInputQual DxilParameterAnnotation::GetParamInputQual() const {
  return m_inputQual;
}
void DxilParameterAnnotation::SetParamInputQual(DxilParamInputQual qual) {
  m_inputQual = qual;
}

const std::vector<unsigned> &DxilParameterAnnotation::GetSemanticIndexVec() const {
  return m_semanticIndex;
}

void DxilParameterAnnotation::SetSemanticIndexVec(const std::vector<unsigned> &Vec) {
  m_semanticIndex = Vec;
}

void DxilParameterAnnotation::AppendSemanticIndex(unsigned SemIdx) {
  m_semanticIndex.emplace_back(SemIdx);
}

//------------------------------------------------------------------------------
//
// DxilFunctionAnnotation class methods.
//
unsigned DxilFunctionAnnotation::GetNumParameters() const {
  return (unsigned)m_parameterAnnotations.size();
}

DxilParameterAnnotation &DxilFunctionAnnotation::GetParameterAnnotation(unsigned ParamIdx) {
  return m_parameterAnnotations[ParamIdx];
}

const DxilParameterAnnotation &DxilFunctionAnnotation::GetParameterAnnotation(unsigned ParamIdx) const {
  return m_parameterAnnotations[ParamIdx];
}

DxilParameterAnnotation &DxilFunctionAnnotation::GetRetTypeAnnotation() {
  return m_retTypeAnnotation;
}

const DxilParameterAnnotation &DxilFunctionAnnotation::GetRetTypeAnnotation() const {
  return m_retTypeAnnotation;
}

const Function *DxilFunctionAnnotation::GetFunction() const {
  return m_pFunction;
}

DxilFunctionFPFlag &DxilFunctionAnnotation::GetFlag() {
  return m_fpFlag;
}

const DxilFunctionFPFlag &DxilFunctionAnnotation::GetFlag() const {
  return m_fpFlag;
}

//------------------------------------------------------------------------------
//
// DxilFunctionFPFlag class methods.
//
void DxilFunctionFPFlag::SetFPAllDenormMode(DXIL::FPDenormMode mode) {
  SetFP64DenormMode(mode);
  SetFP32DenormMode(mode);
  SetFP16DenormMode(mode);
}

void DxilFunctionFPFlag::SetFP64DenormMode(DXIL::FPDenormMode mode) {
  m_flag |= (int32_t)mode<<6;
}

void DxilFunctionFPFlag::SetFP32DenormMode(DXIL::FPDenormMode mode) {
  m_flag |= (int32_t)mode<<3;
}

void DxilFunctionFPFlag::SetFP16DenormMode(DXIL::FPDenormMode mode) {
  m_flag |= (int32_t)mode;
}

DXIL::FPDenormMode DxilFunctionFPFlag::GetFP64DenormMode() {
  return (DXIL::FPDenormMode)((m_flag>>6)&0x7);
}

DXIL::FPDenormMode DxilFunctionFPFlag::GetFP32DenormMode() {
  return (DXIL::FPDenormMode)((m_flag>>3)&0x7);
}

DXIL::FPDenormMode DxilFunctionFPFlag::GetFP16DenormMode() {
  return (DXIL::FPDenormMode)(m_flag&0x7);
}

uint32_t DxilFunctionFPFlag::GetFlagValue() {
  return m_flag;
}

const uint32_t DxilFunctionFPFlag::GetFlagValue() const {
  return m_flag;
}

void DxilFunctionFPFlag::SetFlagValue(uint32_t flag) {
  m_flag = flag;
}

//------------------------------------------------------------------------------
//
// DxilStructAnnotationSystem class methods.
//
DxilTypeSystem::DxilTypeSystem(Module *pModule)
: m_pModule(pModule) {
}

DxilStructAnnotation *DxilTypeSystem::AddStructAnnotation(const StructType *pStructType) {
  DXASSERT_NOMSG(m_StructAnnotations.find(pStructType) == m_StructAnnotations.end());
  DxilStructAnnotation *pA = new DxilStructAnnotation();
  m_StructAnnotations[pStructType] = unique_ptr<DxilStructAnnotation>(pA);
  pA->m_pStructType = pStructType;
  pA->m_FieldAnnotations.resize(pStructType->getNumElements());
  return pA;
}

DxilStructAnnotation *DxilTypeSystem::GetStructAnnotation(const StructType *pStructType) {
  auto it = m_StructAnnotations.find(pStructType);
  if (it != m_StructAnnotations.end()) {
    return it->second.get();
  } else {
    return nullptr;
  }
}

const DxilStructAnnotation *
DxilTypeSystem::GetStructAnnotation(const StructType *pStructType) const {
  auto it = m_StructAnnotations.find(pStructType);
  if (it != m_StructAnnotations.end()) {
    return it->second.get();
  } else {
    return nullptr;
  }
}

void DxilTypeSystem::EraseStructAnnotation(const StructType *pStructType) {
  DXASSERT_NOMSG(m_StructAnnotations.count(pStructType));
  m_StructAnnotations.remove_if([pStructType](
      const std::pair<const StructType *, std::unique_ptr<DxilStructAnnotation>>
          &I) { return pStructType == I.first; });
}

DxilTypeSystem::StructAnnotationMap &DxilTypeSystem::GetStructAnnotationMap() {
  return m_StructAnnotations;
}

DxilFunctionAnnotation *DxilTypeSystem::AddFunctionAnnotation(const Function *pFunction) {
  DxilFunctionFPFlag flag;
  flag.SetFlagValue(0);
  DxilFunctionAnnotation *pA = AddFunctionAnnotationWithFPFlag(pFunction, &flag);
  return pA;
}

DxilFunctionAnnotation *DxilTypeSystem::AddFunctionAnnotationWithFPFlag(const Function *pFunction, const DxilFunctionFPFlag *pFlag) {
  DXASSERT_NOMSG(m_FunctionAnnotations.find(pFunction) == m_FunctionAnnotations.end());
  DxilFunctionAnnotation *pA = new DxilFunctionAnnotation();
  m_FunctionAnnotations[pFunction] = unique_ptr<DxilFunctionAnnotation>(pA);
  pA->m_pFunction = pFunction;
  pA->m_parameterAnnotations.resize(pFunction->getFunctionType()->getNumParams());
  pA->GetFlag().SetFlagValue(pFlag->GetFlagValue());
  return pA;
}

DxilFunctionAnnotation *DxilTypeSystem::GetFunctionAnnotation(const Function *pFunction) {
  auto it = m_FunctionAnnotations.find(pFunction);
  if (it != m_FunctionAnnotations.end()) {
    return it->second.get();
  } else {
    return nullptr;
  }
}

const DxilFunctionAnnotation *
DxilTypeSystem::GetFunctionAnnotation(const Function *pFunction) const {
  auto it = m_FunctionAnnotations.find(pFunction);
  if (it != m_FunctionAnnotations.end()) {
    return it->second.get();
  } else {
    return nullptr;
  }
}

void DxilTypeSystem::EraseFunctionAnnotation(const Function *pFunction) {
  DXASSERT_NOMSG(m_FunctionAnnotations.count(pFunction));
  m_FunctionAnnotations.remove_if([pFunction](
      const std::pair<const Function *, std::unique_ptr<DxilFunctionAnnotation>>
          &I) { return pFunction == I.first; });
}

DxilTypeSystem::FunctionAnnotationMap &DxilTypeSystem::GetFunctionAnnotationMap() {
  return m_FunctionAnnotations;
}

StructType *DxilTypeSystem::GetSNormF32Type(unsigned NumComps) {
  return GetNormFloatType(CompType::getSNormF32(), NumComps);
}

StructType *DxilTypeSystem::GetUNormF32Type(unsigned NumComps) {
  return GetNormFloatType(CompType::getUNormF32(), NumComps);
}

StructType *DxilTypeSystem::GetNormFloatType(CompType CT, unsigned NumComps) {
  Type *pCompType = CT.GetLLVMType(m_pModule->getContext());
  DXASSERT_NOMSG(pCompType->isFloatTy());
  Type *pFieldType = pCompType;
  string TypeName;
  raw_string_ostream NameStream(TypeName);
  if (NumComps > 1) {
    (NameStream << "dx.types." << NumComps << "x" << CT.GetName()).flush();
    pFieldType = VectorType::get(pFieldType, NumComps);
  } else {
    (NameStream << "dx.types." << CT.GetName()).flush();
  }
  StructType *pStructType = m_pModule->getTypeByName(TypeName);
  if (pStructType == nullptr) {
    pStructType = StructType::create(m_pModule->getContext(), pFieldType, TypeName);
    DxilStructAnnotation &TA = *AddStructAnnotation(pStructType);
    DxilFieldAnnotation &FA = TA.GetFieldAnnotation(0);
    FA.SetCompType(CT.GetKind());
    DXASSERT_NOMSG(CT.IsSNorm() || CT.IsUNorm());
  }
  return pStructType;
}

void DxilTypeSystem::CopyTypeAnnotation(const llvm::Type *Ty,
                                        const DxilTypeSystem &src) {
  if (isa<PointerType>(Ty))
    Ty = Ty->getPointerElementType();

  while (isa<ArrayType>(Ty))
    Ty = Ty->getArrayElementType();

  // Only struct type has annotation.
  if (!isa<StructType>(Ty))
    return;

  const StructType *ST = cast<StructType>(Ty);
  // Already exist.
  if (GetStructAnnotation(ST))
    return;

  if (const DxilStructAnnotation *annot = src.GetStructAnnotation(ST)) {
    DxilStructAnnotation *dstAnnot = AddStructAnnotation(ST);
    // Copy the annotation.
    *dstAnnot = *annot;
    // Copy field type annotations.
    for (Type *Ty : ST->elements()) {
      CopyTypeAnnotation(Ty, src);
    }
  }
}

void DxilTypeSystem::CopyFunctionAnnotation(const llvm::Function *pDstFunction,
                                            const llvm::Function *pSrcFunction,
                                            const DxilTypeSystem &src) {
  const DxilFunctionAnnotation *annot = src.GetFunctionAnnotation(pSrcFunction);
  // Don't have annotation.
  if (!annot)
    return;
  // Already exist.
  if (GetFunctionAnnotation(pDstFunction))
    return;

  DxilFunctionAnnotation *dstAnnot = AddFunctionAnnotationWithFPFlag(pDstFunction, &src.GetFunctionAnnotation(pSrcFunction)->GetFlag());

  // Copy the annotation.
  *dstAnnot = *annot;

  // Clone ret type annotation.
  CopyTypeAnnotation(pDstFunction->getReturnType(), src);
  // Clone param type annotations.
  for (const Argument &arg : pDstFunction->args()) {
    CopyTypeAnnotation(arg.getType(), src);
  }
}

DXIL::SigPointKind SigPointFromInputQual(DxilParamInputQual Q, DXIL::ShaderKind SK, bool isPC) {
  DXASSERT(Q != DxilParamInputQual::Inout, "Inout not expected for SigPointFromInputQual");
  switch (SK) {
  case DXIL::ShaderKind::Vertex:
    switch (Q) {
    case DxilParamInputQual::In:
      return DXIL::SigPointKind::VSIn;
    case DxilParamInputQual::Out:
      return DXIL::SigPointKind::VSOut;
    }
    break;
  case DXIL::ShaderKind::Hull:
    switch (Q) {
    case DxilParamInputQual::In:
      if (isPC)
        return DXIL::SigPointKind::PCIn;
      else
        return DXIL::SigPointKind::HSIn;
    case DxilParamInputQual::Out:
      if (isPC)
        return DXIL::SigPointKind::PCOut;
      else
        return DXIL::SigPointKind::HSCPOut;
    case DxilParamInputQual::InputPatch:
      return DXIL::SigPointKind::HSCPIn;
    case DxilParamInputQual::OutputPatch:
      return DXIL::SigPointKind::HSCPOut;
    }
    break;
  case DXIL::ShaderKind::Domain:
    switch (Q) {
    case DxilParamInputQual::In:
      return DXIL::SigPointKind::DSIn;
    case DxilParamInputQual::Out:
      return DXIL::SigPointKind::DSOut;
    case DxilParamInputQual::InputPatch:
    case DxilParamInputQual::OutputPatch:
      return DXIL::SigPointKind::DSCPIn;
    }
    break;
  case DXIL::ShaderKind::Geometry:
    switch (Q) {
    case DxilParamInputQual::In:
      return DXIL::SigPointKind::GSIn;
    case DxilParamInputQual::InputPrimitive:
      return DXIL::SigPointKind::GSVIn;
    case DxilParamInputQual::OutStream0:
    case DxilParamInputQual::OutStream1:
    case DxilParamInputQual::OutStream2:
    case DxilParamInputQual::OutStream3:
      return DXIL::SigPointKind::GSOut;
    }
    break;
  case DXIL::ShaderKind::Pixel:
    switch (Q) {
    case DxilParamInputQual::In:
      return DXIL::SigPointKind::PSIn;
    case DxilParamInputQual::Out:
      return DXIL::SigPointKind::PSOut;
    }
    break;
  case DXIL::ShaderKind::Compute:
    switch (Q) {
    case DxilParamInputQual::In:
      return DXIL::SigPointKind::CSIn;
    }
    break;
  }
  return DXIL::SigPointKind::Invalid;
}

} // namespace hlsl
