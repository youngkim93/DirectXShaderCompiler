///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// DxilAddPixelHitInstrumentation.cpp                                        //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// Provides a pass to add instrumentation to determine pixel hit count and   //
// cost. Used by PIX.                                                        //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "dxc/HLSL/DxilGenerationPass.h"
#include "dxc/HLSL/DxilOperations.h"
#include "dxc/HLSL/DxilSignatureElement.h"
#include "dxc/HLSL/DxilModule.h"
#include "dxc/Support/Global.h"
#include "dxc/HLSL/DxilTypeSystem.h"
#include "dxc/HLSL/DxilConstants.h"
#include "dxc/HLSL/DxilInstructions.h"
#include "dxc/HLSL/DxilSpanAllocator.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/Local.h"
#include <memory>
#include <unordered_set>
#include <array>

using namespace llvm;
using namespace hlsl;

class DxilAddPixelHitInstrumentation : public ModulePass {

  bool ForceEarlyZ = false;
  bool AddPixelCost = false;
  int RTWidth = 1024;
  int NumPixels = 128;

public:
  static char ID; // Pass identification, replacement for typeid
  explicit DxilAddPixelHitInstrumentation() : ModulePass(ID) {}
  const char *getPassName() const override { return "DXIL Constant Color Mod"; }
  void applyOptions(PassOptions O) override;
  bool runOnModule(Module &M) override;
};

void DxilAddPixelHitInstrumentation::applyOptions(PassOptions O)
{
  for (const auto & option : O)
  {
    if (0 == option.first.compare("force-early-z"))
    {
      ForceEarlyZ = atoi(option.second.data()) != 0;
    }
    else if (0 == option.first.compare("rt-width"))
    {
      RTWidth = atoi(option.second.data());
    }
    else if (0 == option.first.compare("num-pixels"))
    {
      NumPixels = atoi(option.second.data());
    }
    else if (0 == option.first.compare("add-pixel-cost"))
    {
      AddPixelCost = atoi(option.second.data()) != 0;
    }
  }
}

bool DxilAddPixelHitInstrumentation::runOnModule(Module &M)
{
  // This pass adds instrumentation for pixel hit counting and pixel cost.

  DxilModule &DM = M.GetOrCreateDxilModule();
  LLVMContext & Ctx = M.getContext();
  OP *HlslOP = DM.GetOP();

  // ForceEarlyZ is incompatible with the discard function (the Z has to be tested/written, and may be written before the shader even runs)
  if (ForceEarlyZ)
  {
    DM.m_ShaderFlags.SetForceEarlyDepthStencil(true);
  }
  
  hlsl::DxilSignature & InputSignature = DM.GetInputSignature();

  auto & InputElements = InputSignature.GetElements();

  unsigned SV_Position_ID;

  auto SV_Position = std::find_if(InputElements.begin(), InputElements.end(), [](const std::unique_ptr<DxilSignatureElement> & Element) {
    return Element->GetSemantic()->GetKind() == hlsl::DXIL::SemanticKind::Position; });

  // SV_Position, if present, has to have full mask, so we needn't worry 
  // about the shader having selected components that don't include x or y.
  // If not present, we add it.
  if ( SV_Position == InputElements.end() ) {
    auto SVPosition = std::make_unique<DxilSignatureElement>(DXIL::SigPointKind::PSIn);
    SVPosition->Initialize("Position", hlsl::CompType::getF32(), hlsl::DXIL::InterpolationMode::Linear, !DM.m_ShaderFlags.GetUseNativeLowPrecision(), 1, 4, 0, 0);
    SVPosition->AppendSemanticIndex(0);
    SVPosition->SetSigPointKind(DXIL::SigPointKind::PSIn);
    SVPosition->SetKind(hlsl::DXIL::SemanticKind::Position);

    auto index = InputSignature.AppendElement(std::move(SVPosition));
    SV_Position_ID = InputElements[index]->GetID();
  }
  else {
    SV_Position_ID = SV_Position->get()->GetID();
  }

  auto EntryPointFunction = DM.GetEntryFunction();

  auto & EntryBlock = EntryPointFunction->getEntryBlock();
  bool HaveInsertedUAV = false;

  CallInst *HandleForUAV;

  // todo: is it a reasonable assumption that there will be a "Ret" in the entry block, and that these are the only
  // points from which the shader can exit (except for a pixel-kill?)
  auto & Instructions = EntryBlock.getInstList();
  auto It = Instructions.begin();
  while(It != Instructions.end()) {
    auto ThisInstruction = It++;
    LlvmInst_Ret Ret(ThisInstruction);
    if (Ret) {
      // Check that there is at least one instruction preceding the Ret (no need to instrument it if there isn't)
      if (ThisInstruction->getPrevNode() != nullptr) {

        // Start adding instructions right before the Ret:
        IRBuilder<> Builder(ThisInstruction);

        if (!HaveInsertedUAV) {

          // Set up a UAV with structure of a single int
          SmallVector<llvm::Type*, 1> Elements{ Type::getInt32Ty(Ctx) };
          llvm::StructType *UAVStructTy = llvm::StructType::create(Elements, "PIX_CountUAV_Type");
          std::unique_ptr<DxilResource> pUAV = llvm::make_unique<DxilResource>();
          pUAV->SetGlobalName("PIX_CountUAVName");
          pUAV->SetGlobalSymbol(UndefValue::get(UAVStructTy->getPointerTo()));
          pUAV->SetID(0);
          pUAV->SetSpaceID((unsigned int)-2); // This is the reserved-for-tools register space
          pUAV->SetSampleCount(1);
          pUAV->SetGloballyCoherent(false);
          pUAV->SetHasCounter(false);
          pUAV->SetCompType(CompType::getI32());
          pUAV->SetLowerBound(0);
          pUAV->SetRangeSize(1);
          pUAV->SetKind(DXIL::ResourceKind::StructuredBuffer);
          pUAV->SetElementStride(4);

          ID = DM.AddUAV(std::move(pUAV));

          // Create handle for the newly-added UAV
          Function* CreateHandleOpFunc = HlslOP->GetOpFunc(DXIL::OpCode::CreateHandle, Type::getVoidTy(Ctx));
          Constant* CreateHandleOpcodeArg = HlslOP->GetU32Const((unsigned)DXIL::OpCode::CreateHandle);
          Constant* UAVVArg = HlslOP->GetI8Const(static_cast<std::underlying_type<DxilResourceBase::Class>::type>(DXIL::ResourceClass::UAV));
          Constant* MetaDataArg = HlslOP->GetU32Const(ID); // position of the metadata record in the corresponding metadata list
          Constant* IndexArg = HlslOP->GetU32Const(0); // 
          Constant* FalseArg = HlslOP->GetI1Const(0); // non-uniform resource index: false
          HandleForUAV = Builder.CreateCall(CreateHandleOpFunc,
            { CreateHandleOpcodeArg, UAVVArg, MetaDataArg, IndexArg, FalseArg }, "PIX_CountUAV_Handle");

          DM.ReEmitDxilResources();

          HaveInsertedUAV = true;
        }

        // ------------------------------------------------------------------------------------------------------------
        // Generate instructions to increment (by one) a UAV value corresponding to the pixel currently being rendered
        // ------------------------------------------------------------------------------------------------------------

        // Useful constants
        Constant* Zero32Arg = HlslOP->GetU32Const(0);
        Constant* Zero8Arg = HlslOP->GetI8Const(0);
        Constant* One32Arg = HlslOP->GetU32Const(1);
        Constant* One8Arg = HlslOP->GetI8Const(1);
        UndefValue* UndefArg = UndefValue::get(Type::getInt32Ty(Ctx));
        Constant* NumPixelsArg = HlslOP->GetU32Const(NumPixels);
        Constant* NumPixelsMinusOneArg = HlslOP->GetU32Const(NumPixels-1);

        // Step 1: Convert SV_POSITION to UINT          
        Value * XAsInt;
        Value * YAsInt;
        {
          auto LoadInputOpFunc = HlslOP->GetOpFunc(DXIL::OpCode::LoadInput, Type::getFloatTy(Ctx));
          Constant* LoadInputOpcode = HlslOP->GetU32Const((unsigned)DXIL::OpCode::LoadInput);
          Constant*  SV_Pos_ID = HlslOP->GetU32Const(SV_Position_ID);
          auto XPos = Builder.CreateCall(LoadInputOpFunc,
          { LoadInputOpcode, SV_Pos_ID, Zero32Arg /*row*/, Zero8Arg /*column*/, UndefArg }, "XPos");
          auto YPos = Builder.CreateCall(LoadInputOpFunc,
          { LoadInputOpcode, SV_Pos_ID, Zero32Arg /*row*/, One8Arg /*column*/, UndefArg }, "YPos");

          XAsInt = Builder.CreateCast(Instruction::CastOps::FPToUI, XPos, Type::getInt32Ty(Ctx), "XIndex");
          YAsInt = Builder.CreateCast(Instruction::CastOps::FPToUI, YPos, Type::getInt32Ty(Ctx), "YIndex");
        }

        // Step 2: Calculate pixel index
        Value * ClampedIndex;
        {
          Constant* RTWidthArg = HlslOP->GetI32Const(RTWidth);
          auto YOffset = Builder.CreateMul(YAsInt, RTWidthArg);
          auto Index = Builder.CreateAdd(XAsInt, YOffset);

          // Step 3: Clamp to size of UAV to prevent TDR if something goes wrong
          auto CompareToLimit = Builder.CreateICmpUGT(Index, NumPixelsMinusOneArg);
          ClampedIndex = Builder.CreateSelect(CompareToLimit, NumPixelsMinusOneArg, Index, "Clamped");
        }

        // Insert the UAV increment instruction:
        Function* AtomicOpFunc = HlslOP->GetOpFunc(OP::OpCode::AtomicBinOp, Type::getInt32Ty(Ctx));
        Constant* AtomicBinOpcode = HlslOP->GetU32Const((unsigned)OP::OpCode::AtomicBinOp);
        Constant* AtomicAdd = HlslOP->GetU32Const((unsigned)DXIL::AtomicBinOpCode::Add);
        {
          (void)Builder.CreateCall(AtomicOpFunc, {
            AtomicBinOpcode,// i32, ; opcode
            HandleForUAV,   // %dx.types.Handle, ; resource handle
            AtomicAdd,      // i32, ; binary operation code : EXCHANGE, IADD, AND, OR, XOR, IMIN, IMAX, UMIN, UMAX
            ClampedIndex,   // i32, ; coordinate c0: index in elements
            Zero32Arg,      // i32, ; coordinate c1: byte offset into element
            Zero32Arg,      // i32, ; coordinate c2 (unused)
            One32Arg        // i32); increment value
          }, "UAVIncResult");
        }

        if (AddPixelCost) {
          // ------------------------------------------------------------------------------------------------------------
          // Generate instructions to increment a value corresponding to the current pixel in the second half of the UAV, 
          // by an amount proportional to the estimated average cost of each pixel in the current draw call.
          // ------------------------------------------------------------------------------------------------------------

          // Step 1: Retrieve weight value from UAV; it will be placed after the range we're writing to
          Value * Weight;
          {
            Function* LoadWeight = HlslOP->GetOpFunc(OP::OpCode::BufferLoad, Type::getInt32Ty(Ctx));
            Constant* LoadWeightOpcode = HlslOP->GetU32Const((unsigned)DXIL::OpCode::BufferLoad);
            Constant* OffsetIntoUAV = HlslOP->GetU32Const(NumPixels * 2);
            auto WeightStruct = Builder.CreateCall(LoadWeight, {
              LoadWeightOpcode, // i32 opcode
              HandleForUAV,     // %dx.types.Handle, ; resource handle
              OffsetIntoUAV,    // i32 c0: index in elements into UAV
              Zero32Arg         // i32 c1: byte offset into struct
            }, "WeightStruct");
            Weight = Builder.CreateExtractValue(WeightStruct, static_cast<uint64_t>(0LL), "Weight");
          }

          // Step 2: Update write position ("Index") to second half of the UAV 
          auto OffsetIndex = Builder.CreateAdd(ClampedIndex, NumPixelsArg);

          // Step 3: Increment UAV value by the weight
          (void)Builder.CreateCall(AtomicOpFunc,{
            AtomicBinOpcode,          // i32, ; opcode
            HandleForUAV,   // %dx.types.Handle, ; resource handle
            AtomicAdd,      // i32, ; binary operation code : EXCHANGE, IADD, AND, OR, XOR, IMIN, IMAX, UMIN, UMAX
            OffsetIndex,    // i32, ; coordinate c0: index in elements
            Zero32Arg,      // i32, ; coordinate c1: byte offset into element
            Zero32Arg,      // i32, ; coordinate c2 (unused)
            Weight          // i32); increment value
          }, "UAVIncResult2");
        }
      }
    }
  }

  bool Modified = false;

  return Modified;
}

char DxilAddPixelHitInstrumentation::ID = 0;

ModulePass *llvm::createDxilAddPixelHitInstrumentationPass() {
  return new DxilAddPixelHitInstrumentation();
}

INITIALIZE_PASS(DxilAddPixelHitInstrumentation, "hlsl-dxil-add-pixel-hit-instrmentation", "DXIL Count completed PS invocations and costs", false, false)
