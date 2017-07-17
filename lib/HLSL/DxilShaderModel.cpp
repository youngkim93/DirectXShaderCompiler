///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// DxilShaderModel.cpp                                                       //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/HLSL/DxilShaderModel.h"
#include "dxc/HLSL/DxilSemantic.h"
#include "dxc/Support/Global.h"


namespace hlsl {

ShaderModel::ShaderModel(Kind Kind, unsigned Major, unsigned Minor, const char *pszName,
                         unsigned NumInputRegs, unsigned NumOutputRegs,
                         bool bUAVs, bool bTypedUavs,  unsigned NumUAVRegs)
: m_Kind(Kind)
, m_Major(Major)
, m_Minor(Minor)
, m_pszName(pszName)
, m_NumInputRegs(NumInputRegs)
, m_NumOutputRegs(NumOutputRegs)
, m_bUAVs(bUAVs)
, m_bTypedUavs(bTypedUavs)
, m_NumUAVRegs(NumUAVRegs) {
}

bool ShaderModel::operator==(const ShaderModel &other) const {
    return m_Kind          == other.m_Kind
        && m_Major         == other.m_Major
        && m_Minor         == other.m_Minor
        && strcmp(m_pszName,  other.m_pszName) == 0
        && m_NumInputRegs  == other.m_NumInputRegs
        && m_NumOutputRegs == other.m_NumOutputRegs
        && m_bTypedUavs    == other.m_bTypedUavs
        && m_NumUAVRegs    == other.m_NumUAVRegs;
}

bool ShaderModel::IsValid() const {
  DXASSERT(IsPS() || IsVS() || IsGS() || IsHS() || IsDS() || IsCS() ||
               IsLib() || m_Kind == Kind::Invalid,
           "invalid shader model");
  return m_Kind != Kind::Invalid;
}

bool ShaderModel::IsValidForDxil() const {
  if (!IsValid())
    return false;
  switch (m_Major) {
    case 6: {
      switch (m_Minor) {
      case 0:
      case 1:
      case 2:
        return true;
      }
    }
    break;
  }
  return false;
}

const ShaderModel *ShaderModel::Get(unsigned Idx) {
  DXASSERT_NOMSG(Idx < kNumShaderModels - 1);
  if (Idx < kNumShaderModels - 1)
    return &ms_ShaderModels[Idx];
  else
    return GetInvalid();
}

const ShaderModel *ShaderModel::Get(Kind Kind, unsigned Major, unsigned Minor) {
  // Linear search. Replaced by binary search if necessary.
  for (unsigned i = 0; i < kNumShaderModels; i++) {
    const ShaderModel *pSM = &ms_ShaderModels[i];
    if (pSM->m_Kind == Kind && pSM->m_Major == Major && pSM->m_Minor == Minor)
      return pSM;
  }

  return GetInvalid();
}

const ShaderModel *ShaderModel::GetByName(const char *pszName) {
  // [ps|vs|gs|hs|ds|cs]_[major]_[minor]
  Kind Kind;
  switch (pszName[0]) {
  case 'p':   Kind = Kind::Pixel;     break;
  case 'v':   Kind = Kind::Vertex;    break;
  case 'g':   Kind = Kind::Geometry;  break;
  case 'h':   Kind = Kind::Hull;      break;
  case 'd':   Kind = Kind::Domain;    break;
  case 'c':   Kind = Kind::Compute;   break;
  case 'l':   Kind = Kind::Library;   break;
  default:    return GetInvalid();
  }
  unsigned Idx = 3;
  if (Kind != Kind::Library) {
    if (pszName[1] != 's' || pszName[2] != '_')
      return GetInvalid();
  } else {
    if (pszName[1] != 'i' || pszName[2] != 'b' || pszName[3] != '_')
      return GetInvalid();
    Idx = 4;
  }

  unsigned Major;
  switch (pszName[Idx++]) {
  case '4': Major = 4;  break;
  case '5': Major = 5;  break;
  case '6': Major = 6;  break;
  default:  return GetInvalid();
  }
  if (pszName[Idx++] != '_')
    return GetInvalid();

  unsigned Minor;
  switch (pszName[Idx++]) {
    case '0': Minor = 0;  break;
    case '1': Minor = 1;  break;
    case '2':
      if (Major == 6) {
        Minor = 2;
        break;
      }
      else return GetInvalid();
    default:  return GetInvalid();
  }
  if (pszName[Idx++] != 0)
    return GetInvalid();

  return Get(Kind, Major, Minor);
}

void ShaderModel::GetDxilVersion(unsigned &DxilMajor, unsigned &DxilMinor) const {
  DXASSERT(IsValidForDxil(), "invalid shader model");
  DxilMajor = 1;
  switch (m_Minor) {
  case 0:
    DxilMinor = 0;
    break;
  case 1:
    DxilMinor = 1;
    break;
  case 2:
    DxilMinor = 2;
    break;
  default:
    DXASSERT(0, "IsValidForDxil() should have caught this.");
    break;
  }
}

void ShaderModel::GetMinValidatorVersion(unsigned &ValMajor, unsigned &ValMinor) const {
  DXASSERT(IsValidForDxil(), "invalid shader model");
  ValMajor = 1;
  switch (m_Minor) {
  case 0:
    ValMinor = 0;
    break;
  case 1:
    ValMinor = 1;
    break;
  case 2:
    ValMinor = 2;
    break;
  default:
    DXASSERT(0, "IsValidForDxil() should have caught this.");
    break;
  }
}

static const char *ShaderModelKindNames[] = {
    "ps", "vs", "gs", "hs", "ds", "cs", "lib", "invalid",
};

std::string ShaderModel::GetKindName() const {
  return GetKindName(m_Kind);
}

std::string ShaderModel::GetKindName(Kind kind) {
  return std::string(ShaderModelKindNames[static_cast<unsigned int>(kind)]);
}

const ShaderModel *ShaderModel::GetInvalid() {
  return &ms_ShaderModels[kNumShaderModels - 1];
}

typedef ShaderModel SM;
typedef Semantic SE;
const ShaderModel ShaderModel::ms_ShaderModels[kNumShaderModels] = {
  //                                  IR  OR   UAV?   TyUAV? UAV base
  SM(Kind::Compute,  4, 0, "cs_4_0",  0,  0,   true,  false, 1),
  SM(Kind::Compute,  4, 1, "cs_4_1",  0,  0,   true,  false, 1),
  SM(Kind::Compute,  5, 0, "cs_5_0",  0,  0,   true,  true,  64),
  SM(Kind::Compute,  5, 1, "cs_5_1",  0,  0,   true,  true,  UINT_MAX),
  SM(Kind::Compute,  6, 0, "cs_6_0",  0,  0,   true,  true,  UINT_MAX),
  SM(Kind::Compute,  6, 1, "cs_6_1",  0,  0,   true,  true,  UINT_MAX),
  SM(Kind::Compute,  6, 2, "cs_6_2",  0,  0,   true,  true,  UINT_MAX),

  SM(Kind::Domain,   5, 0, "ds_5_0",  32, 32,  true,  true,  64),
  SM(Kind::Domain,   5, 1, "ds_5_1",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Domain,   6, 0, "ds_6_0",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Domain,   6, 1, "ds_6_1",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Domain,   6, 2, "ds_6_2",  32, 32,  true,  true,  UINT_MAX),

  SM(Kind::Geometry, 4, 0, "gs_4_0",  16, 32,  false, false, 0),
  SM(Kind::Geometry, 4, 1, "gs_4_1",  32, 32,  false, false, 0),
  SM(Kind::Geometry, 5, 0, "gs_5_0",  32, 32,  true,  true,  64),
  SM(Kind::Geometry, 5, 1, "gs_5_1",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Geometry, 6, 0, "gs_6_0",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Geometry, 6, 1, "gs_6_1",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Geometry, 6, 2, "gs_6_2",  32, 32,  true,  true,  UINT_MAX),

  SM(Kind::Hull,     5, 0, "hs_5_0",  32, 32,  true,  true,  64),
  SM(Kind::Hull,     5, 1, "hs_5_1",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Hull,     6, 0, "hs_6_0",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Hull,     6, 1, "hs_6_1",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Hull,     6, 2, "hs_6_2",  32, 32,  true,  true,  UINT_MAX),

  SM(Kind::Pixel,    4, 0, "ps_4_0",  32, 8,   false, false, 0),
  SM(Kind::Pixel,    4, 1, "ps_4_1",  32, 8,   false, false, 0),
  SM(Kind::Pixel,    5, 0, "ps_5_0",  32, 8,   true,  true,  64),
  SM(Kind::Pixel,    5, 1, "ps_5_1",  32, 8,   true,  true,  UINT_MAX),
  SM(Kind::Pixel,    6, 0, "ps_6_0",  32, 8,   true,  true,  UINT_MAX),
  SM(Kind::Pixel,    6, 1, "ps_6_1",  32, 8,   true,  true,  UINT_MAX),
  SM(Kind::Pixel,    6, 2, "ps_6_2",  32, 8,   true,  true,  UINT_MAX),

  SM(Kind::Vertex,   4, 0, "vs_4_0",  16, 16,  false, false, 0),
  SM(Kind::Vertex,   4, 1, "vs_4_1",  32, 32,  false, false, 0),
  SM(Kind::Vertex,   5, 0, "vs_5_0",  32, 32,  true,  true,  64),
  SM(Kind::Vertex,   5, 1, "vs_5_1",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Vertex,   6, 0, "vs_6_0",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Vertex,   6, 1, "vs_6_1",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Vertex,   6, 2, "vs_6_2",  32, 32,  true,  true,  UINT_MAX),

  SM(Kind::Library,  6, 1, "lib_6_1",  32, 32,  true,  true,  UINT_MAX),
  SM(Kind::Library,  6, 2, "lib_6_2",  32, 32,  true,  true,  UINT_MAX),

  SM(Kind::Invalid,  0, 0, "invalid", 0,  0,   false, false, 0),
};

} // namespace hlsl
