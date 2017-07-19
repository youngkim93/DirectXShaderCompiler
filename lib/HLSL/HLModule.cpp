///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// HLModule.cpp                                                              //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// HighLevel DX IR module.                                                   //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/HLSL/DxilOperations.h"
#include "dxc/HLSL/DxilShaderModel.h"
#include "dxc/HLSL/DxilCBuffer.h"
#include "dxc/HLSL/HLModule.h"
#include "dxc/HLSL/DxilTypeSystem.h"
#include "dxc/HLSL/DxilRootSignature.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"

using namespace llvm;
using std::string;
using std::vector;
using std::unique_ptr;

namespace hlsl {

//------------------------------------------------------------------------------
//
//  HLModule methods.
//
HLModule::HLModule(Module *pModule)
    : m_Ctx(pModule->getContext())
    , m_pModule(pModule)
    , m_pEntryFunc(nullptr)
    , m_EntryName("")
    , m_pMDHelper(llvm::make_unique<DxilMDHelper>(
          pModule, llvm::make_unique<HLExtraPropertyHelper>(pModule)))
    , m_pDebugInfoFinder(nullptr)
    , m_pSM(nullptr)
    , m_DxilMajor(DXIL::kDxilMajor)
    , m_DxilMinor(DXIL::kDxilMinor)
    , m_pOP(llvm::make_unique<OP>(pModule->getContext(), pModule))
    , m_pTypeSystem(llvm::make_unique<DxilTypeSystem>(pModule)) {
  DXASSERT_NOMSG(m_pModule != nullptr);

  // Pin LLVM dump methods. TODO: make debug-only.
  void (__thiscall Module::*pfnModuleDump)() const = &Module::dump;
  void (__thiscall Type::*pfnTypeDump)() const = &Type::dump;
  m_pUnused = (char *)&pfnModuleDump - (char *)&pfnTypeDump;
}

HLModule::~HLModule() {
}

LLVMContext &HLModule::GetCtx() const { return m_Ctx; }
Module *HLModule::GetModule() const { return m_pModule; }
OP *HLModule::GetOP() const { return m_pOP.get(); }

void HLModule::SetValidatorVersion(unsigned ValMajor, unsigned ValMinor) {
  m_ValMajor = ValMajor;
  m_ValMinor = ValMinor;
}

void HLModule::GetValidatorVersion(unsigned &ValMajor, unsigned &ValMinor) const {
  ValMajor = m_ValMajor;
  ValMinor = m_ValMinor;
}

void HLModule::SetShaderModel(const ShaderModel *pSM) {
  DXASSERT(m_pSM == nullptr, "shader model must not change for the module");
  DXASSERT(pSM != nullptr && pSM->IsValidForDxil(), "shader model must be valid");
  m_pSM = pSM;
  m_pSM->GetDxilVersion(m_DxilMajor, m_DxilMinor);
  m_pMDHelper->SetShaderModel(m_pSM);
  m_RootSignature = llvm::make_unique<RootSignatureHandle>();
}

const ShaderModel *HLModule::GetShaderModel() const {
  return m_pSM;
}

uint32_t HLOptions::GetHLOptionsRaw() const {
  union Cast {
    Cast(const HLOptions &options) {
      hlOptions = options;
    }
    HLOptions hlOptions;
    uint32_t  rawData;
  };
  static_assert(sizeof(uint32_t) == sizeof(HLOptions),
                "size must match to make sure no undefined bits when cast");
  Cast rawCast(*this);
  return rawCast.rawData;
}
void HLOptions::SetHLOptionsRaw(uint32_t data) {
  union Cast {
    Cast(uint32_t data) {
      rawData = data;
    }
    HLOptions hlOptions;
    uint64_t  rawData;
  };

  Cast rawCast(data);
  *this = rawCast.hlOptions;
}

void HLModule::SetHLOptions(HLOptions &opts) {
  m_Options = opts;
}

const HLOptions &HLModule::GetHLOptions() const {
  return m_Options;
}

Function *HLModule::GetEntryFunction() const {
  return m_pEntryFunc;
}

void HLModule::SetEntryFunction(Function *pEntryFunc) {
  m_pEntryFunc = pEntryFunc;
}

const string &HLModule::GetEntryFunctionName() const { return m_EntryName; }
void HLModule::SetEntryFunctionName(const string &name) { m_EntryName = name; }

template<typename T> unsigned 
HLModule::AddResource(vector<unique_ptr<T> > &Vec, unique_ptr<T> pRes) {
  DXASSERT_NOMSG((unsigned)Vec.size() < UINT_MAX);
  unsigned Id = (unsigned)Vec.size();
  Vec.emplace_back(std::move(pRes));
  return Id;
}

unsigned HLModule::AddCBuffer(unique_ptr<DxilCBuffer> pCBuffer) {
  return AddResource<DxilCBuffer>(m_CBuffers, std::move(pCBuffer));
}

DxilCBuffer &HLModule::GetCBuffer(unsigned idx) {
  return *m_CBuffers[idx];
}

const DxilCBuffer &HLModule::GetCBuffer(unsigned idx) const {
  return *m_CBuffers[idx];
}

const vector<unique_ptr<DxilCBuffer> > &HLModule::GetCBuffers() const {
  return m_CBuffers;
}

unsigned HLModule::AddSampler(unique_ptr<DxilSampler> pSampler) {
  return AddResource<DxilSampler>(m_Samplers, std::move(pSampler));
}

DxilSampler &HLModule::GetSampler(unsigned idx) {
  return *m_Samplers[idx];
}

const DxilSampler &HLModule::GetSampler(unsigned idx) const {
  return *m_Samplers[idx];
}

const vector<unique_ptr<DxilSampler> > &HLModule::GetSamplers() const {
  return m_Samplers;
}

unsigned HLModule::AddSRV(unique_ptr<HLResource> pSRV) {
  return AddResource<HLResource>(m_SRVs, std::move(pSRV));
}

HLResource &HLModule::GetSRV(unsigned idx) {
  return *m_SRVs[idx];
}

const HLResource &HLModule::GetSRV(unsigned idx) const {
  return *m_SRVs[idx];
}

const vector<unique_ptr<HLResource> > &HLModule::GetSRVs() const {
  return m_SRVs;
}

unsigned HLModule::AddUAV(unique_ptr<HLResource> pUAV) {
  return AddResource<HLResource>(m_UAVs, std::move(pUAV));
}

HLResource &HLModule::GetUAV(unsigned idx) {
  return *m_UAVs[idx];
}

const HLResource &HLModule::GetUAV(unsigned idx) const {
  return *m_UAVs[idx];
}

const vector<unique_ptr<HLResource> > &HLModule::GetUAVs() const {
  return m_UAVs;
}

void HLModule::RemoveFunction(llvm::Function *F) {
  DXASSERT_NOMSG(F != nullptr);
  m_DxilFunctionPropsMap.erase(F);
  if (m_pTypeSystem.get()->GetFunctionAnnotation(F))
    m_pTypeSystem.get()->EraseFunctionAnnotation(F);
  m_pOP->RemoveFunction(F);
}

template <typename TResource>
bool RemoveResource(std::vector<std::unique_ptr<TResource>> &vec,
                    GlobalVariable *pVariable) {
  for (auto p = vec.begin(), e = vec.end(); p != e; ++p) {
    if ((*p)->GetGlobalSymbol() == pVariable) {
      p = vec.erase(p);
      // Update ID.
      for (e = vec.end();p != e; ++p) {
        unsigned ID = (*p)->GetID()-1;
        (*p)->SetID(ID);
      }
      return true;
    }
  }
  return false;
}
bool RemoveResource(std::vector<GlobalVariable *> &vec,
                    llvm::GlobalVariable *pVariable) {
  for (auto p = vec.begin(), e = vec.end(); p != e; ++p) {
    if (*p == pVariable) {
      vec.erase(p);
      return true;
    }
  }
  return false;
}

void HLModule::RemoveGlobal(llvm::GlobalVariable *GV) {
  RemoveResources(&GV, 1);
}

void HLModule::RemoveResources(llvm::GlobalVariable **ppVariables,
                               unsigned count) {
  DXASSERT_NOMSG(count == 0 || ppVariables != nullptr);
  unsigned resourcesRemoved = count;
  for (unsigned i = 0; i < count; ++i) {
    GlobalVariable *pVariable = ppVariables[i];
    // This could be considerably faster - check variable type to see which
    // resource type this is rather than scanning all lists, and look for
    // usage and removal patterns.
    if (RemoveResource(m_CBuffers, pVariable))
      continue;
    if (RemoveResource(m_SRVs, pVariable))
      continue;
    if (RemoveResource(m_UAVs, pVariable))
      continue;
    if (RemoveResource(m_Samplers, pVariable))
      continue;
    // TODO: do m_TGSMVariables and m_StreamOutputs need maintenance?
    --resourcesRemoved; // Global variable is not a resource?
  }
}

HLModule::tgsm_iterator HLModule::tgsm_begin() {
  return m_TGSMVariables.begin();
}

HLModule::tgsm_iterator HLModule::tgsm_end() {
  return m_TGSMVariables.end();
}

void HLModule::AddGroupSharedVariable(GlobalVariable *GV) {
  m_TGSMVariables.emplace_back(GV);
}

RootSignatureHandle &HLModule::GetRootSignature() {
  return *m_RootSignature;
}

DxilTypeSystem &HLModule::GetTypeSystem() {
  return *m_pTypeSystem;
}

DxilTypeSystem *HLModule::ReleaseTypeSystem() {
  return m_pTypeSystem.release();
}

hlsl::OP *HLModule::ReleaseOP() {
  return m_pOP.release();
}

RootSignatureHandle *HLModule::ReleaseRootSignature() {
  return m_RootSignature.release();
}

std::unordered_map<llvm::Function *, std::unique_ptr<DxilFunctionProps>> &&
HLModule::ReleaseFunctionPropsMap() {
  return std::move(m_DxilFunctionPropsMap);
}

void HLModule::EmitLLVMUsed() {
  if (m_LLVMUsed.empty())
    return;

  vector<llvm::Constant*> GVs;
  GVs.resize(m_LLVMUsed.size());
  for (size_t i = 0, e = m_LLVMUsed.size(); i != e; i++) {
    GVs[i] = ConstantExpr::getAddrSpaceCast(cast<llvm::Constant>(&*m_LLVMUsed[i]), Type::getInt8PtrTy(m_Ctx));
  }

  ArrayType *pATy = ArrayType::get(Type::getInt8PtrTy(m_Ctx), GVs.size());

  GlobalVariable *pGV = new GlobalVariable(*m_pModule, pATy, false,
                                           GlobalValue::AppendingLinkage,
                                           ConstantArray::get(pATy, GVs),
                                           "llvm.used");

  pGV->setSection("llvm.metadata");
}

vector<GlobalVariable* > &HLModule::GetLLVMUsed() {
  return m_LLVMUsed;
}

bool HLModule::HasDxilFunctionProps(llvm::Function *F) {
  return m_DxilFunctionPropsMap.find(F) != m_DxilFunctionPropsMap.end();
}
DxilFunctionProps &HLModule::GetDxilFunctionProps(llvm::Function *F)  {
  DXASSERT(m_DxilFunctionPropsMap.count(F) != 0, "cannot find F in map");
  return *m_DxilFunctionPropsMap[F];
}
void HLModule::AddDxilFunctionProps(llvm::Function *F, std::unique_ptr<DxilFunctionProps> &info) {
  DXASSERT(m_DxilFunctionPropsMap.count(F) == 0, "F already in map, info will be overwritten");
  DXASSERT_NOMSG(info->shaderKind != DXIL::ShaderKind::Invalid);
  m_DxilFunctionPropsMap[F] = std::move(info);
}

DxilFunctionAnnotation *HLModule::GetFunctionAnnotation(llvm::Function *F) {
  return m_pTypeSystem->GetFunctionAnnotation(F);
}
DxilFunctionAnnotation *HLModule::AddFunctionAnnotation(llvm::Function *F) {
  DXASSERT(m_pTypeSystem->GetFunctionAnnotation(F)==nullptr, "function annotation already exist");
  return m_pTypeSystem->AddFunctionAnnotation(F);
}

DxilFunctionAnnotation *HLModule::AddFunctionAnnotationWithFPDenormMode(llvm::Function *F, DXIL::FPDenormMode mode) {
  DXASSERT(m_pTypeSystem->GetFunctionAnnotation(F) == nullptr, "function annotataion already exist");
  DxilFunctionFPFlag flag(0);
  flag.SetFPAllDenormMode(mode);
  return m_pTypeSystem->AddFunctionAnnotationWithFPFlag(F, &flag);
}

void HLModule::AddResourceTypeAnnotation(llvm::Type *Ty,
                                         DXIL::ResourceClass resClass,
                                         DXIL::ResourceKind kind) {
  if (m_ResTypeAnnotation.count(Ty) == 0) {
    m_ResTypeAnnotation.emplace(Ty, std::make_pair(resClass, kind));
  } else {
    DXASSERT(resClass == m_ResTypeAnnotation[Ty].first, "resClass mismatch");
    DXASSERT(kind == m_ResTypeAnnotation[Ty].second, "kind mismatch");
  }
}

DXIL::ResourceClass HLModule::GetResourceClass(llvm::Type *Ty) {
  if (m_ResTypeAnnotation.count(Ty) > 0) {
    return m_ResTypeAnnotation[Ty].first;
  } else {
    return DXIL::ResourceClass::Invalid;
  }
}
DXIL::ResourceKind HLModule::GetResourceKind(llvm::Type *Ty) {
  if (m_ResTypeAnnotation.count(Ty) > 0) {
    return m_ResTypeAnnotation[Ty].second;
  } else {
    return DXIL::ResourceKind::Invalid;
  }
}

static unsigned GetIntAt(MDTuple *tuple, unsigned idx) {
  return DxilMDHelper::ConstMDToUint32(tuple->getOperand(idx));
}

static unsigned GetFloatAt(MDTuple *tuple, unsigned idx) {
  return DxilMDHelper::ConstMDToFloat(tuple->getOperand(idx));
}

DXIL::FPDenormMode HLModule::GetFPDenormMode() const {
  return m_FPDenormMode;
}

void HLModule::SetFPDenormMode(const DXIL::FPDenormMode mode) {
  m_FPDenormMode = mode;
}

static const StringRef kHLDxilFunctionPropertiesMDName           = "dx.fnprops";
static const StringRef kHLDxilOptionsMDName                      = "dx.options";
static const StringRef kHLDxilResourceTypeAnnotationMDName       = "dx.resource.type.annotation";

// DXIL metadata serialization/deserialization.
void HLModule::EmitHLMetadata() {
  m_pMDHelper->EmitDxilVersion(m_DxilMajor, m_DxilMinor);
  m_pMDHelper->EmitValidatorVersion(m_ValMajor, m_ValMinor);
  m_pMDHelper->EmitDxilShaderModel(m_pSM);

  MDTuple *pMDResources = EmitHLResources();
  MDTuple *pMDProperties = EmitHLShaderProperties();

  m_pMDHelper->EmitDxilTypeSystem(GetTypeSystem(), m_LLVMUsed);
  EmitLLVMUsed();
  MDTuple *const pNullMDSig = nullptr;
  MDTuple *pEntry = m_pMDHelper->EmitDxilEntryPointTuple(GetEntryFunction(), m_EntryName, pNullMDSig, pMDResources, pMDProperties);
  vector<MDNode *> Entries;
  Entries.emplace_back(pEntry);
  m_pMDHelper->EmitDxilEntryPoints(Entries);

  {
    NamedMDNode * fnProps = m_pModule->getOrInsertNamedMetadata(kHLDxilFunctionPropertiesMDName);
    for (auto && pair : m_DxilFunctionPropsMap) {
      const hlsl::DxilFunctionProps * props = pair.second.get();
      MDTuple *pProps = m_pMDHelper->EmitDxilFunctionProps(props, pair.first);
      fnProps->addOperand(pProps);
    }

    NamedMDNode * options = m_pModule->getOrInsertNamedMetadata(kHLDxilOptionsMDName);
    uint32_t hlOptions = m_Options.GetHLOptionsRaw();
    options->addOperand(MDNode::get(m_Ctx, m_pMDHelper->Uint32ToConstMD(hlOptions)));

    NamedMDNode * resTyAnnotations = m_pModule->getOrInsertNamedMetadata(kHLDxilResourceTypeAnnotationMDName);
    resTyAnnotations->addOperand(EmitResTyAnnotations());
  }

  if (!m_RootSignature->IsEmpty()) {
    m_pMDHelper->EmitRootSignature(*m_RootSignature.get());
  }
}

void HLModule::LoadHLMetadata() {
  m_pMDHelper->LoadDxilVersion(m_DxilMajor, m_DxilMinor);
  m_pMDHelper->LoadValidatorVersion(m_ValMajor, m_ValMinor);
  m_pMDHelper->LoadDxilShaderModel(m_pSM);
  m_RootSignature = llvm::make_unique<RootSignatureHandle>();

  const llvm::NamedMDNode *pEntries = m_pMDHelper->GetDxilEntryPoints();

  Function *pEntryFunc;
  string EntryName;
  const llvm::MDOperand *pSignatures, *pResources, *pProperties;
  m_pMDHelper->GetDxilEntryPoint(pEntries->getOperand(0), pEntryFunc, EntryName, pSignatures, pResources, pProperties);

  SetEntryFunction(pEntryFunc);
  SetEntryFunctionName(EntryName);

  LoadHLResources(*pResources);
  LoadHLShaderProperties(*pProperties);

  m_pMDHelper->LoadDxilTypeSystem(*m_pTypeSystem.get());

  {
    NamedMDNode * fnProps = m_pModule->getNamedMetadata(kHLDxilFunctionPropertiesMDName);
    size_t propIdx = 0;
    while (propIdx < fnProps->getNumOperands()) {
      MDTuple *pProps = dyn_cast<MDTuple>(fnProps->getOperand(propIdx++));

      std::unique_ptr<hlsl::DxilFunctionProps> props =
          llvm::make_unique<hlsl::DxilFunctionProps>();

      Function *F = m_pMDHelper->LoadDxilFunctionProps(pProps, props.get());

      m_DxilFunctionPropsMap[F] = std::move(props);
    }

    const NamedMDNode * options = m_pModule->getOrInsertNamedMetadata(kHLDxilOptionsMDName);
    const MDNode *MDOptions = options->getOperand(0);
    m_Options.SetHLOptionsRaw(DxilMDHelper::ConstMDToUint32(MDOptions->getOperand(0)));
    NamedMDNode * resTyAnnotations = m_pModule->getOrInsertNamedMetadata(kHLDxilResourceTypeAnnotationMDName);
    const MDNode *MDResTyAnnotations = resTyAnnotations->getOperand(0);
    if (MDResTyAnnotations->getNumOperands())
      LoadResTyAnnotations(MDResTyAnnotations->getOperand(0));
  }

  m_pMDHelper->LoadRootSignature(*m_RootSignature.get());
}

void HLModule::ClearHLMetadata(llvm::Module &M) {
  Module::named_metadata_iterator
    b = M.named_metadata_begin(),
    e = M.named_metadata_end();
  SmallVector<NamedMDNode*, 8> nodes;
  for (; b != e; ++b) {
    StringRef name = b->getName();
    if (name == DxilMDHelper::kDxilVersionMDName ||
        name == DxilMDHelper::kDxilShaderModelMDName ||
        name == DxilMDHelper::kDxilEntryPointsMDName ||
        name == DxilMDHelper::kDxilRootSignatureMDName ||
        name == DxilMDHelper::kDxilResourcesMDName ||
        name == DxilMDHelper::kDxilTypeSystemMDName ||
        name == DxilMDHelper::kDxilValidatorVersionMDName ||
        name == kHLDxilFunctionPropertiesMDName || // TODO: adjust to proper name
        name == kHLDxilResourceTypeAnnotationMDName ||
        name == kHLDxilOptionsMDName ||
        name.startswith(DxilMDHelper::kDxilTypeSystemHelperVariablePrefix)) {
      nodes.push_back(b);
    }
  }
  for (size_t i = 0; i < nodes.size(); ++i) {
    M.eraseNamedMetadata(nodes[i]);
  }
}

MDTuple *HLModule::EmitHLResources() {
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

void HLModule::LoadHLResources(const llvm::MDOperand &MDO) {
  const llvm::MDTuple *pSRVs, *pUAVs, *pCBuffers, *pSamplers;
  m_pMDHelper->GetDxilResources(MDO, pSRVs, pUAVs, pCBuffers, pSamplers);

  // Load SRV records.
  if (pSRVs != nullptr) {
    for (unsigned i = 0; i < pSRVs->getNumOperands(); i++) {
      unique_ptr<HLResource> pSRV(new HLResource);
      m_pMDHelper->LoadDxilSRV(pSRVs->getOperand(i), *pSRV);
      AddSRV(std::move(pSRV));
    }
  }

  // Load UAV records.
  if (pUAVs != nullptr) {
    for (unsigned i = 0; i < pUAVs->getNumOperands(); i++) {
      unique_ptr<HLResource> pUAV(new HLResource);
      m_pMDHelper->LoadDxilUAV(pUAVs->getOperand(i), *pUAV);
      AddUAV(std::move(pUAV));
    }
  }

  // Load CBuffer records.
  if (pCBuffers != nullptr) {
    for (unsigned i = 0; i < pCBuffers->getNumOperands(); i++) {
      unique_ptr<DxilCBuffer> pCB = llvm::make_unique<DxilCBuffer>();
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

llvm::MDTuple *HLModule::EmitResTyAnnotations() {
  vector<Metadata *> MDVals;
  for (auto &resAnnotation : m_ResTypeAnnotation) {
    Metadata *TyMeta =
        ValueAsMetadata::get(UndefValue::get(resAnnotation.first));
    MDVals.emplace_back(TyMeta);
    MDVals.emplace_back(m_pMDHelper->Uint32ToConstMD(
        static_cast<unsigned>(resAnnotation.second.first)));
    MDVals.emplace_back(m_pMDHelper->Uint32ToConstMD(
        static_cast<unsigned>(resAnnotation.second.second)));
  }
  return MDNode::get(m_Ctx, MDVals);
}
void HLModule::LoadResTyAnnotations(const llvm::MDOperand &MDO) {
  if (MDO.get() == nullptr)
    return;

  const MDTuple *pTupleMD = dyn_cast<MDTuple>(MDO.get());
  IFTBOOL(pTupleMD != nullptr, DXC_E_INCORRECT_DXIL_METADATA);
  IFTBOOL((pTupleMD->getNumOperands() & 0x3) == 0,
          DXC_E_INCORRECT_DXIL_METADATA);
  for (unsigned iNode = 0; iNode < pTupleMD->getNumOperands(); iNode += 3) {
    const MDOperand &MDTy = pTupleMD->getOperand(iNode);
    const MDOperand &MDClass = pTupleMD->getOperand(iNode + 1);
    const MDOperand &MDKind = pTupleMD->getOperand(iNode + 2);
    Type *Ty = m_pMDHelper->ValueMDToValue(MDTy)->getType();
    DXIL::ResourceClass resClass = static_cast<DXIL::ResourceClass>(
        DxilMDHelper::ConstMDToUint32(MDClass));
    DXIL::ResourceKind kind =
        static_cast<DXIL::ResourceKind>(DxilMDHelper::ConstMDToUint32(MDKind));
    AddResourceTypeAnnotation(Ty, resClass, kind);
  }
}

MDTuple *HLModule::EmitHLShaderProperties() {
  return nullptr;
}

void HLModule::LoadHLShaderProperties(const MDOperand &MDO) {
  return;
}

MDNode *HLModule::DxilSamplerToMDNode(const DxilSampler &S) {
  MDNode *MD = m_pMDHelper->EmitDxilSampler(S);
  ValueAsMetadata *ResClass =
      m_pMDHelper->Uint32ToConstMD((unsigned)DXIL::ResourceClass::Sampler);

  return MDNode::get(m_Ctx, {ResClass, MD});
}
MDNode *HLModule::DxilSRVToMDNode(const DxilResource &SRV) {
  MDNode *MD = m_pMDHelper->EmitDxilSRV(SRV);
  ValueAsMetadata *ResClass =
      m_pMDHelper->Uint32ToConstMD((unsigned)DXIL::ResourceClass::SRV);

  return MDNode::get(m_Ctx, {ResClass, MD});
}
MDNode *HLModule::DxilUAVToMDNode(const DxilResource &UAV) {
  MDNode *MD = m_pMDHelper->EmitDxilUAV(UAV);
  ValueAsMetadata *ResClass =
      m_pMDHelper->Uint32ToConstMD((unsigned)DXIL::ResourceClass::UAV);

  return MDNode::get(m_Ctx, {ResClass, MD});
}
MDNode *HLModule::DxilCBufferToMDNode(const DxilCBuffer &CB) {
  MDNode *MD = m_pMDHelper->EmitDxilCBuffer(CB);
  ValueAsMetadata *ResClass =
      m_pMDHelper->Uint32ToConstMD((unsigned)DXIL::ResourceClass::CBuffer);

  return MDNode::get(m_Ctx, {ResClass, MD});
}

void HLModule::LoadDxilResourceBaseFromMDNode(MDNode *MD, DxilResourceBase &R) {
  return m_pMDHelper->LoadDxilResourceBaseFromMDNode(MD, R);
}

void HLModule::AddResourceWithGlobalVariableAndMDNode(llvm::Constant *GV,
                                                      llvm::MDNode *MD) {
  IFTBOOL(MD->getNumOperands() >= DxilMDHelper::kHLDxilResourceAttributeNumFields,
          DXC_E_INCORRECT_DXIL_METADATA);

  DxilResource::Class RC =
      static_cast<DxilResource::Class>(m_pMDHelper->ConstMDToUint32(
          MD->getOperand(DxilMDHelper::kHLDxilResourceAttributeClass)));
  const MDOperand &Meta =
      MD->getOperand(DxilMDHelper::kHLDxilResourceAttributeMeta);
  unsigned rangeSize = 1;
  Type *Ty = GV->getType()->getPointerElementType();
  if (ArrayType *AT = dyn_cast<ArrayType>(Ty))
    rangeSize = AT->getNumElements();

  switch (RC) {
  case DxilResource::Class::Sampler: {
    std::unique_ptr<DxilSampler> S = llvm::make_unique<DxilSampler>();
    m_pMDHelper->LoadDxilSampler(Meta, *S);
    S->SetGlobalSymbol(GV);
    S->SetGlobalName(GV->getName());
    S->SetRangeSize(rangeSize);
    AddSampler(std::move(S));
  } break;
  case DxilResource::Class::SRV: {
    std::unique_ptr<HLResource> Res = llvm::make_unique<HLResource>();
    m_pMDHelper->LoadDxilSRV(Meta, *Res);
    Res->SetGlobalSymbol(GV);
    Res->SetGlobalName(GV->getName());
    Res->SetRangeSize(rangeSize);
    AddSRV(std::move(Res));
  } break;
  case DxilResource::Class::UAV: {
    std::unique_ptr<HLResource> Res = llvm::make_unique<HLResource>();
    m_pMDHelper->LoadDxilUAV(Meta, *Res);
    Res->SetGlobalSymbol(GV);
    Res->SetGlobalName(GV->getName());
    Res->SetRangeSize(rangeSize);
    AddUAV(std::move(Res));
  } break;
  default:
    DXASSERT(0, "Invalid metadata for AddResourceWithGlobalVariableAndMDNode");
  }
}

// TODO: Don't check names.
bool HLModule::IsStreamOutputType(llvm::Type *Ty) {
  if (StructType *ST = dyn_cast<StructType>(Ty)) {
    if (ST->getName().startswith("class.PointStream"))
      return true;
    if (ST->getName().startswith("class.LineStream"))
      return true;
    if (ST->getName().startswith("class.TriangleStream"))
      return true;
  }
  return false;
}

bool HLModule::IsStreamOutputPtrType(llvm::Type *Ty) {
  if (!Ty->isPointerTy())
    return false;
  Ty = Ty->getPointerElementType();
  return IsStreamOutputType(Ty);
}

bool HLModule::IsHLSLObjectType(llvm::Type *Ty) {
  if (llvm::StructType *ST = dyn_cast<llvm::StructType>(Ty)) {
    StringRef name = ST->getName();
    if (name.startswith("dx.types.wave_t"))
      return true;

    if (name.endswith("_slice_type"))
      return false;

    name = name.ltrim("class.");
    name = name.ltrim("struct.");

    if (name == "SamplerState")
      return true;
    if (name == "SamplerComparisonState")
      return true;

    if (name.startswith("TriangleStream"))
      return true;
    if (name.startswith("PointStream"))
      return true;
    if (name.startswith("LineStream"))
      return true;

    if (name.startswith("AppendStructuredBuffer"))
      return true;
    if (name.startswith("ConsumeStructuredBuffer"))
      return true;

    if (name.startswith("ConstantBuffer"))
      return true;

    name = name.ltrim("RasterizerOrdered");
    name = name.ltrim("RW");
    if (name == "ByteAddressBuffer")
      return true;

    if (name.startswith("Buffer"))
      return true;
    if (name.startswith("StructuredBuffer"))
      return true;
    if (name.startswith("Texture1D"))
      return true;
    if (name.startswith("Texture1DArray"))
      return true;
    if (name.startswith("Texture2D"))
      return true;
    if (name.startswith("Texture2DArray"))
      return true;
    if (name.startswith("Texture3D"))
      return true;
    if (name.startswith("TextureCube"))
      return true;
    if (name.startswith("TextureCubeArray"))
      return true;
    if (name.startswith("Texture2DMS"))
      return true;
    if (name.startswith("Texture2DMSArray"))
      return true;
  }
  return false;
}

Type *HLModule::GetArrayEltTy(Type *Ty) {
  if (isa<PointerType>(Ty))
    Ty = Ty->getPointerElementType();
  while (isa<ArrayType>(Ty)) {
    Ty = Ty->getArrayElementType();
  }
  return Ty;
}

unsigned
HLModule::GetLegacyCBufferFieldElementSize(DxilFieldAnnotation &fieldAnnotation,
                                           llvm::Type *Ty,
                                           DxilTypeSystem &typeSys) {
  while (isa<ArrayType>(Ty)) {
    Ty = Ty->getArrayElementType();
  }

  // Bytes.
  unsigned compSize = fieldAnnotation.GetCompType().Is64Bit()?8:4;
  unsigned fieldSize = compSize;
  if (Ty->isVectorTy()) {
    fieldSize *= Ty->getVectorNumElements();
  } else if (StructType *ST = dyn_cast<StructType>(Ty)) {
    DxilStructAnnotation *EltAnnotation = typeSys.GetStructAnnotation(ST);
    if (EltAnnotation) {
      fieldSize = EltAnnotation->GetCBufferSize();
    } else {
      // Calculate size when don't have annotation.
      if (fieldAnnotation.HasMatrixAnnotation()) {
        const DxilMatrixAnnotation &matAnnotation =
            fieldAnnotation.GetMatrixAnnotation();
        unsigned rows = matAnnotation.Rows;
        unsigned cols = matAnnotation.Cols;
        if (matAnnotation.Orientation == MatrixOrientation::ColumnMajor) {
          rows = cols;
          cols = matAnnotation.Rows;
        } else if (matAnnotation.Orientation != MatrixOrientation::RowMajor) {
          // Invalid matrix orientation.
          fieldSize = 0;
        }
        fieldSize = (rows - 1) * 16 + cols * 4;
      } else {
        // Cannot find struct annotation.
        fieldSize = 0;
      }
    }
  }
  return fieldSize;
}

bool HLModule::IsStaticGlobal(GlobalVariable *GV) {
  return GV->getLinkage() == GlobalValue::LinkageTypes::InternalLinkage &&
         GV->getType()->getPointerAddressSpace() == DXIL::kDefaultAddrSpace;
}

bool HLModule::IsSharedMemoryGlobal(llvm::GlobalVariable *GV) {
  return GV->getType()->getPointerAddressSpace() == DXIL::kTGSMAddrSpace;
}

void HLModule::GetParameterRowsAndCols(Type *Ty, unsigned &rows, unsigned &cols,
                                       DxilParameterAnnotation &paramAnnotation) {
  if (Ty->isPointerTy())
    Ty = Ty->getPointerElementType();
  // For array input of HS, DS, GS,
  // we need to skip the first level which size is based on primitive type.
  DxilParamInputQual inputQual = paramAnnotation.GetParamInputQual();
  bool skipOneLevelArray = inputQual == DxilParamInputQual::InputPatch;
  skipOneLevelArray |= inputQual == DxilParamInputQual::OutputPatch;
  skipOneLevelArray |= inputQual == DxilParamInputQual::InputPrimitive;

  if (skipOneLevelArray) {
    if (Ty->isArrayTy())
      Ty = Ty->getArrayElementType();
  }

  unsigned arraySize = 1;
  while (Ty->isArrayTy()) {
    arraySize *= Ty->getArrayNumElements();
    Ty = Ty->getArrayElementType();
  }

  rows = 1;
  cols = 1;

  if (paramAnnotation.HasMatrixAnnotation()) {
    const DxilMatrixAnnotation &matrix = paramAnnotation.GetMatrixAnnotation();
    if (matrix.Orientation == MatrixOrientation::RowMajor) {
      rows = matrix.Rows;
      cols = matrix.Cols;
    } else {
      DXASSERT(matrix.Orientation == MatrixOrientation::ColumnMajor, "");
      cols = matrix.Rows;
      rows = matrix.Cols;
    }
  } else if (Ty->isVectorTy())
    cols = Ty->getVectorNumElements();

  rows *= arraySize;
}

// For legacy data layout, everything less than 32 align to 32.
static const StringRef kLegacyLayoutString = "e-m:e-p:32:32-i1:32:32-i8:32:32-i16:32:32-i64:64-f16:32-f80:32-n8:16:32-a:0:32-S32";
const char *HLModule::GetLegacyDataLayoutDesc() {
  return kLegacyLayoutString.data();
}

static Value *MergeGEP(GEPOperator *SrcGEP, GetElementPtrInst *GEP) {
  IRBuilder<> Builder(GEP);
  SmallVector<Value *, 8> Indices;

  // Find out whether the last index in the source GEP is a sequential idx.
  bool EndsWithSequential = false;
  for (gep_type_iterator I = gep_type_begin(*SrcGEP), E = gep_type_end(*SrcGEP);
       I != E; ++I)
    EndsWithSequential = !(*I)->isStructTy();
  if (EndsWithSequential) {
    Value *Sum;
    Value *SO1 = SrcGEP->getOperand(SrcGEP->getNumOperands() - 1);
    Value *GO1 = GEP->getOperand(1);
    if (SO1 == Constant::getNullValue(SO1->getType())) {
      Sum = GO1;
    } else if (GO1 == Constant::getNullValue(GO1->getType())) {
      Sum = SO1;
    } else {
      // If they aren't the same type, then the input hasn't been processed
      // by the loop above yet (which canonicalizes sequential index types to
      // intptr_t).  Just avoid transforming this until the input has been
      // normalized.
      if (SO1->getType() != GO1->getType())
        return nullptr;
      // Only do the combine when GO1 and SO1 are both constants. Only in
      // this case, we are sure the cost after the merge is never more than
      // that before the merge.
      if (!isa<Constant>(GO1) || !isa<Constant>(SO1))
        return nullptr;
      Sum = Builder.CreateAdd(SO1, GO1);
    }

    // Update the GEP in place if possible.
    if (SrcGEP->getNumOperands() == 2) {
      GEP->setOperand(0, SrcGEP->getOperand(0));
      GEP->setOperand(1, Sum);
      return GEP;
    }
    Indices.append(SrcGEP->op_begin() + 1, SrcGEP->op_end() - 1);
    Indices.push_back(Sum);
    Indices.append(GEP->op_begin() + 2, GEP->op_end());
  } else if (isa<Constant>(*GEP->idx_begin()) &&
             cast<Constant>(*GEP->idx_begin())->isNullValue() &&
             SrcGEP->getNumOperands() != 1) {
    // Otherwise we can do the fold if the first index of the GEP is a zero
    Indices.append(SrcGEP->op_begin() + 1, SrcGEP->op_end());
    Indices.append(GEP->idx_begin() + 1, GEP->idx_end());
  }
  if (!Indices.empty())
    return Builder.CreateInBoundsGEP(SrcGEP->getSourceElementType(),
                                     SrcGEP->getOperand(0), Indices,
                                     GEP->getName());
  else
    llvm_unreachable("must merge");
}

void HLModule::MergeGepUse(Value *V) {
  for (auto U = V->user_begin(); U != V->user_end();) {
    auto Use = U++;

    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(*Use)) {
      if (GEPOperator *prevGEP = dyn_cast<GEPOperator>(V)) {
        // merge the 2 GEPs
        Value *newGEP = MergeGEP(prevGEP, GEP);
        // Don't need to replace when GEP is updated in place
        if (newGEP != GEP) {
          GEP->replaceAllUsesWith(newGEP);
          GEP->eraseFromParent();
        }
        MergeGepUse(newGEP);
      } else {
        MergeGepUse(*Use);
      }
    } else if (GEPOperator *GEPOp = dyn_cast<GEPOperator>(*Use)) {
      if (GEPOperator *prevGEP = dyn_cast<GEPOperator>(V)) {
        // merge the 2 GEPs
        Value *newGEP = MergeGEP(prevGEP, GEP);
        // Don't need to replace when GEP is updated in place
        if (newGEP != GEP) {
          GEP->replaceAllUsesWith(newGEP);
          GEP->eraseFromParent();
        }
        MergeGepUse(newGEP);
      } else {
        MergeGepUse(*Use);
      }
    }
  }
  if (V->user_empty()) {
    // Only remove GEP here, root ptr will be removed by DCE.
    if (GetElementPtrInst *I = dyn_cast<GetElementPtrInst>(V))
      I->eraseFromParent();
  }
}

template<typename BuilderTy>
CallInst *HLModule::EmitHLOperationCall(BuilderTy &Builder,
                                           HLOpcodeGroup group, unsigned opcode,
                                           Type *RetType,
                                           ArrayRef<Value *> paramList,
                                           llvm::Module &M) {
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

template
CallInst *HLModule::EmitHLOperationCall(IRBuilder<> &Builder,
                                           HLOpcodeGroup group, unsigned opcode,
                                           Type *RetType,
                                           ArrayRef<Value *> paramList,
                                           llvm::Module &M);

unsigned HLModule::FindCastOp(bool fromUnsigned, bool toUnsigned,
                              llvm::Type *SrcTy, llvm::Type *DstTy) {
  Instruction::CastOps castOp = llvm::Instruction::CastOps::BitCast;

  if (SrcTy->isAggregateType() || DstTy->isAggregateType())
    return llvm::Instruction::CastOps::BitCast;

  uint32_t SrcBitSize = SrcTy->getScalarSizeInBits();
  uint32_t DstBitSize = DstTy->getScalarSizeInBits();
  if (SrcTy->isIntOrIntVectorTy() && DstTy->isIntOrIntVectorTy()) {
    if (SrcBitSize > DstBitSize)
      return Instruction::Trunc;
    if (toUnsigned)
      return Instruction::ZExt;
    else
      return Instruction::SExt;
  }

  if (SrcTy->isFPOrFPVectorTy() && DstTy->isFPOrFPVectorTy()) {
    if (SrcBitSize > DstBitSize)
      return Instruction::FPTrunc;
    else
      return Instruction::FPExt;
  }

  if (SrcTy->isIntOrIntVectorTy() && DstTy->isFPOrFPVectorTy())
    if (fromUnsigned)
      return Instruction::UIToFP;
    else
      return Instruction::SIToFP;

  if (SrcTy->isFPOrFPVectorTy() && DstTy->isIntOrIntVectorTy())
    if (toUnsigned)
      return Instruction::FPToUI;
    else
      return Instruction::FPToSI;

  DXASSERT_NOMSG(0);
  return castOp;
}

bool HLModule::HasPreciseAttributeWithMetadata(Instruction *I) {
  return DxilMDHelper::IsMarkedPrecise(I);
}

void HLModule::MarkPreciseAttributeWithMetadata(Instruction *I) {
  return DxilMDHelper::MarkPrecise(I);
}

void HLModule::ClearPreciseAttributeWithMetadata(Instruction *I) {
  I->setMetadata(DxilMDHelper::kDxilPreciseAttributeMDName, nullptr);
}

static void MarkPreciseAttribute(Function *F) {
  LLVMContext &Ctx = F->getContext();
  MDNode *preciseNode = MDNode::get(
      Ctx, {MDString::get(Ctx, DxilMDHelper::kDxilPreciseAttributeMDName)});

  F->setMetadata(DxilMDHelper::kDxilPreciseAttributeMDName, preciseNode);
}

static void MarkPreciseAttributeOnValWithFunctionCall(
    llvm::Value *V, llvm::IRBuilder<> &Builder, llvm::Module &M) {
  Type *Ty = V->getType();
  Type *EltTy = Ty->getScalarType();

  // TODO: Only do this on basic types.
  
  FunctionType *preciseFuncTy =
      FunctionType::get(Type::getVoidTy(M.getContext()), {EltTy}, false);
  // The function will be deleted after precise propagate.
  std::string preciseFuncName = "dx.attribute.precise.";
  raw_string_ostream mangledNameStr(preciseFuncName);
  EltTy->print(mangledNameStr);
  mangledNameStr.flush();

  Function *preciseFunc =
      cast<Function>(M.getOrInsertFunction(preciseFuncName, preciseFuncTy));
  if (!HLModule::HasPreciseAttribute(preciseFunc))
    MarkPreciseAttribute(preciseFunc);
  if (Ty->isVectorTy()) {
    for (unsigned i = 0; i < Ty->getVectorNumElements(); i++) {
      Value *Elt = Builder.CreateExtractElement(V, i);
      Builder.CreateCall(preciseFunc, {Elt});
    }
  } else
    Builder.CreateCall(preciseFunc, {V});
}

void HLModule::MarkPreciseAttributeOnPtrWithFunctionCall(llvm::Value *Ptr,
                                               llvm::Module &M) {
  for (User *U : Ptr->users()) {
    // Skip load inst.
    if (LoadInst *LI = dyn_cast<LoadInst>(U))
      continue;
    if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
      Value *V = SI->getValueOperand();
      if (isa<Instruction>(V)) {
        // Mark the Value with function call.
        IRBuilder<> Builder(SI);
        MarkPreciseAttributeOnValWithFunctionCall(V, Builder, M);
      }
    } else if (CallInst *CI = dyn_cast<CallInst>(U)) {
      if (CI->getType()->isVoidTy()) {
        IRBuilder<> Builder(CI);
        // For void type, cannot use as function arg.
        // Mark all arg for it?
        for (auto &arg : CI->arg_operands()) {
          MarkPreciseAttributeOnValWithFunctionCall(arg, Builder, M);
        }
      } else {
        IRBuilder<> Builder(CI->getNextNode());
        MarkPreciseAttributeOnValWithFunctionCall(CI, Builder, M);
      }
    } else {
      // Must be GEP here.
      GetElementPtrInst *GEP = cast<GetElementPtrInst>(U);
      MarkPreciseAttributeOnPtrWithFunctionCall(GEP, M);
    }
  }
}

bool HLModule::HasPreciseAttribute(Function *F) {
  MDNode *preciseNode =
      F->getMetadata(DxilMDHelper::kDxilPreciseAttributeMDName);
  return preciseNode != nullptr;
}

void HLModule::MarkDxilResourceAttrib(llvm::Function *F, MDNode *MD) {
  F->setMetadata(DxilMDHelper::kHLDxilResourceAttributeMDName, MD);
}

MDNode *HLModule::GetDxilResourceAttrib(llvm::Function *F) {
  return F->getMetadata(DxilMDHelper::kHLDxilResourceAttributeMDName);
}

void HLModule::MarkDxilResourceAttrib(llvm::Argument *Arg, llvm::MDNode *MD) {
  unsigned i = Arg->getArgNo();
  Function *F = Arg->getParent();
  DxilFunctionAnnotation *FuncAnnot = m_pTypeSystem->GetFunctionAnnotation(F);
  if (!FuncAnnot) {
    DXASSERT(0, "Invalid function");
    return;
  }
  DxilParameterAnnotation &ParamAnnot = FuncAnnot->GetParameterAnnotation(i);
  ParamAnnot.SetResourceAttribute(MD);
}

MDNode *HLModule::GetDxilResourceAttrib(llvm::Argument *Arg) {
  unsigned i = Arg->getArgNo();
  Function *F = Arg->getParent();
  DxilFunctionAnnotation *FuncAnnot = m_pTypeSystem->GetFunctionAnnotation(F);
  if (!FuncAnnot)
    return nullptr;
  DxilParameterAnnotation &ParamAnnot = FuncAnnot->GetParameterAnnotation(i);
  return ParamAnnot.GetResourceAttribute();
}

MDNode *HLModule::GetDxilResourceAttrib(Type *Ty, Module &M) {
  for (Function &F : M.functions()) {
    if (hlsl::GetHLOpcodeGroupByName(&F) == HLOpcodeGroup::HLCreateHandle) {
      Type *ResTy = F.getFunctionType()->getParamType(
          HLOperandIndex::kCreateHandleResourceOpIdx);
      if (ResTy == Ty)
        return GetDxilResourceAttrib(&F);
    }
  }
  return nullptr;
}

DIGlobalVariable *
HLModule::FindGlobalVariableDebugInfo(GlobalVariable *GV,
                                      DebugInfoFinder &DbgInfoFinder) {
  struct GlobalFinder {
    GlobalVariable *GV;
    bool operator()(llvm::DIGlobalVariable *const arg) const {
      return arg->getVariable() == GV;
    }
  };
  GlobalFinder F = {GV};
  DebugInfoFinder::global_variable_iterator Found =
      std::find_if(DbgInfoFinder.global_variables().begin(),
                   DbgInfoFinder.global_variables().end(), F);
  if (Found != DbgInfoFinder.global_variables().end()) {
    return *Found;
  }
  return nullptr;
}

static void AddDIGlobalVariable(DIBuilder &Builder, DIGlobalVariable *LocDIGV,
                                StringRef Name, DIType *DITy,
                                GlobalVariable *GV, DebugInfoFinder &DbgInfoFinder, bool removeLocDIGV) {
  DIGlobalVariable *EltDIGV = Builder.createGlobalVariable(
      LocDIGV->getScope(), Name, GV->getName(), LocDIGV->getFile(),
      LocDIGV->getLine(), DITy, false, GV);

  DICompileUnit *DICU = dyn_cast<DICompileUnit>(LocDIGV->getScope());
  if (!DICU) {
    DISubprogram *DIS = dyn_cast<DISubprogram>(LocDIGV->getScope());
    if (DIS) {
      // Find the DICU which has this Subprogram.
      NamedMDNode *CompileUnits = GV->getParent()->getNamedMetadata("llvm.dbg.cu");
      DXASSERT_NOMSG(CompileUnits);
      for (unsigned I = 0, E = CompileUnits->getNumOperands(); I != E; ++I) {
        auto *CU = cast<DICompileUnit>(CompileUnits->getOperand(I));
        DXASSERT(CU , "Expected valid compile unit");

        for (DISubprogram *SP : CU->getSubprograms()) {
          if (SP == DIS) {
            DICU = CU;
            break;
          }
        }
      }
    }
  }
  DXASSERT_NOMSG(DICU);
  // Add global to CU.
  auto *GlobalVariables = DICU->getRawGlobalVariables();
  DXASSERT_NOMSG(GlobalVariables);
  MDTuple *GTuple = cast<MDTuple>(GlobalVariables);
  std::vector<Metadata *> AllGVs(GTuple->operands().begin(),
                                 GTuple->operands().end());
  if (removeLocDIGV) {
    auto locIt = std::find(AllGVs.begin(), AllGVs.end(), LocDIGV);
    AllGVs.erase(locIt);
  }
  AllGVs.emplace_back(EltDIGV);
  DICU->replaceGlobalVariables(MDTuple::get(GV->getContext(), AllGVs));
  DXVERIFY_NOMSG(DbgInfoFinder.appendGlobalVariable(EltDIGV));
}

void HLModule::CreateElementGlobalVariableDebugInfo(
    GlobalVariable *GV, DebugInfoFinder &DbgInfoFinder, GlobalVariable *EltGV,
    unsigned sizeInBits, unsigned alignInBits, unsigned offsetInBits,
    StringRef eltName) {
  DIGlobalVariable *DIGV = FindGlobalVariableDebugInfo(GV, DbgInfoFinder);
  DXASSERT_NOMSG(DIGV);
  DIBuilder Builder(*GV->getParent());
  DITypeIdentifierMap EmptyMap;

  DIType *DITy = DIGV->getType().resolve(EmptyMap);
  DIScope *DITyScope = DITy->getScope().resolve(EmptyMap);
  // Create Elt type.
  DIType *EltDITy =
      Builder.createMemberType(DITyScope, DITy->getName().str() + eltName.str(),
                               DITy->getFile(), DITy->getLine(), sizeInBits,
                               alignInBits, offsetInBits, /*Flags*/ 0, DITy);

  AddDIGlobalVariable(Builder, DIGV, DIGV->getName().str() + eltName.str(),
                      EltDITy, EltGV, DbgInfoFinder, /*removeDIGV*/false);
}

void HLModule::UpdateGlobalVariableDebugInfo(
    llvm::GlobalVariable *GV, llvm::DebugInfoFinder &DbgInfoFinder,
    llvm::GlobalVariable *NewGV) {
  DIGlobalVariable *DIGV = FindGlobalVariableDebugInfo(GV, DbgInfoFinder);
  DXASSERT_NOMSG(DIGV);
  DIBuilder Builder(*GV->getParent());
  DITypeIdentifierMap EmptyMap;
  DIType *DITy = DIGV->getType().resolve(EmptyMap);

  AddDIGlobalVariable(Builder, DIGV, DIGV->getName(), DITy, NewGV,
                      DbgInfoFinder,/*removeDIGV*/true);
}

DebugInfoFinder &HLModule::GetOrCreateDebugInfoFinder() {
  if (m_pDebugInfoFinder == nullptr) {
    m_pDebugInfoFinder = llvm::make_unique<llvm::DebugInfoFinder>();
    m_pDebugInfoFinder->processModule(*m_pModule);
  }
  return *m_pDebugInfoFinder;
}
//------------------------------------------------------------------------------
//
// Signature methods.
//

HLExtraPropertyHelper::HLExtraPropertyHelper(llvm::Module *pModule)
: DxilExtraPropertyHelper(pModule) {
}

void HLExtraPropertyHelper::EmitSignatureElementProperties(const DxilSignatureElement &SE, 
                                                              vector<Metadata *> &MDVals) {
}

void HLExtraPropertyHelper::LoadSignatureElementProperties(const MDOperand &MDO, 
                                                           DxilSignatureElement &SE) {
  if (MDO.get() == nullptr)
    return;
}

} // namespace hlsl

namespace llvm {
hlsl::HLModule &Module::GetOrCreateHLModule(bool skipInit) {
  std::unique_ptr<hlsl::HLModule> M;
  if (!HasHLModule()) {
    M = llvm::make_unique<hlsl::HLModule>(this);
    if (!skipInit) {
      M->LoadHLMetadata();
    }
    SetHLModule(M.release());
  }
  return GetHLModule();
}

void Module::ResetHLModule() {
  if (HasHLModule()) {
    delete TheHLModule;
    TheHLModule = nullptr;
  }
}

}
