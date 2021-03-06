# Copyright (C) Microsoft Corporation. All rights reserved.
# This file is distributed under the University of Illinois Open Source License. See LICENSE.TXT for details.
add_llvm_library(LLVMHLSL
  ComputeViewIdState.cpp
  ControlDependence.cpp
  DxilAddPixelHitInstrumentation.cpp
  DxilCBuffer.cpp
  DxilCompType.cpp
  DxilCondenseResources.cpp
  DxilContainer.cpp
  DxilContainerAssembler.cpp
  DxilContainerReflection.cpp
  DxilDebugInstrumentation.cpp
  DxilEliminateOutputDynamicIndexing.cpp
  DxilExpandTrigIntrinsics.cpp
  DxilForceEarlyZ.cpp
  DxilGenerationPass.cpp
  DxilInterpolationMode.cpp
  DxilLegalizeSampleOffsetPass.cpp
  DxilLinker.cpp
  DxilMetadataHelper.cpp
  DxilModule.cpp
  DxilOperations.cpp
  DxilOutputColorBecomesConstant.cpp
  DxilPreparePasses.cpp
  DxilRemoveDiscards.cpp
  DxilReduceMSAAToSingleSample.cpp
  DxilPreserveAllOutputs.cpp
  DxilResource.cpp
  DxilResourceBase.cpp
  DxilRootSignature.cpp
  DxilSampler.cpp
  DxilSemantic.cpp
  DxilShaderModel.cpp
  DxilSignature.cpp
  DxilSignatureElement.cpp
  DxilTargetLowering.cpp
  DxilTargetTransformInfo.cpp
  DxilTypeSystem.cpp
  DxilUtil.cpp
  DxilValidation.cpp
  DxcOptimizer.cpp
  HLMatrixLowerPass.cpp
  HLModule.cpp
  HLOperations.cpp
  HLOperationLower.cpp
  HLOperationLowerExtension.cpp
  HLResource.cpp
  HLSignatureLower.cpp
  PauseResumePasses.cpp
  ReducibilityAnalysis.cpp
  WaveSensitivityAnalysis.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/IR
)

add_dependencies(LLVMHLSL intrinsics_gen)
