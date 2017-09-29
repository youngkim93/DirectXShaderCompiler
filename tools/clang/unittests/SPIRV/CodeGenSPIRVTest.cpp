//===- unittests/SPIRV/CodeGenSPIRVTest.cpp ---- Run CodeGenSPIRV tests ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "FileTestFixture.h"
#include "WholeFileTestFixture.h"

namespace {
using clang::spirv::FileTest;
using clang::spirv::WholeFileTest;

// === Whole output tests ===

TEST_F(WholeFileTest, EmptyVoidMain) {
  runWholeFileTest("empty-void-main.hlsl2spv", /*generateHeader*/ true);
}

TEST_F(WholeFileTest, PassThruPixelShader) {
  runWholeFileTest("passthru-ps.hlsl2spv", /*generateHeader*/ true);
}

TEST_F(WholeFileTest, PassThruVertexShader) {
  runWholeFileTest("passthru-vs.hlsl2spv", /*generateHeader*/ true);
}

TEST_F(WholeFileTest, PassThruComputeShader) {
  runWholeFileTest("passthru-cs.hlsl2spv", /*generateHeader*/ true);
}

TEST_F(WholeFileTest, ConstantPixelShader) {
  runWholeFileTest("constant-ps.hlsl2spv", /*generateHeader*/ true);
}

// === Partial output tests ===

// For types
TEST_F(FileTest, ScalarTypes) { runFileTest("type.scalar.hlsl"); }
TEST_F(FileTest, VectorTypes) { runFileTest("type.vector.hlsl"); }
TEST_F(FileTest, MatrixTypes) { runFileTest("type.matrix.hlsl"); }
TEST_F(FileTest, StructTypes) { runFileTest("type.struct.hlsl"); }
TEST_F(FileTest, ArrayTypes) { runFileTest("type.array.hlsl"); }
TEST_F(FileTest, TypedefTypes) { runFileTest("type.typedef.hlsl"); }
TEST_F(FileTest, SamplerTypes) { runFileTest("type.sampler.hlsl"); }
TEST_F(FileTest, TextureTypes) { runFileTest("type.texture.hlsl"); }
TEST_F(FileTest, RWTextureTypes) { runFileTest("type.rwtexture.hlsl"); }
TEST_F(FileTest, BufferType) { runFileTest("type.buffer.hlsl"); }
TEST_F(FileTest, CBufferType) { runFileTest("type.cbuffer.hlsl"); }
TEST_F(FileTest, ConstantBufferType) {
  runFileTest("type.constant-buffer.hlsl");
}
TEST_F(FileTest, StructuredBufferType) {
  runFileTest("type.structured-buffer.hlsl");
}
TEST_F(FileTest, AppendStructuredBufferType) {
  runFileTest("type.append-structured-buffer.hlsl");
}
TEST_F(FileTest, ConsumeStructuredBufferType) {
  runFileTest("type.consume-structured-buffer.hlsl");
}
TEST_F(FileTest, ByteAddressBufferTypes) {
  runFileTest("type.byte-address-buffer.hlsl");
}

// For constants
TEST_F(FileTest, ScalarConstants) { runFileTest("constant.scalar.hlsl"); }
TEST_F(FileTest, VectorConstants) { runFileTest("constant.vector.hlsl"); }
TEST_F(FileTest, MatrixConstants) { runFileTest("constant.matrix.hlsl"); }
TEST_F(FileTest, StructConstants) { runFileTest("constant.struct.hlsl"); }
TEST_F(FileTest, ArrayConstants) { runFileTest("constant.array.hlsl"); }

// For variables
TEST_F(FileTest, VarInitScalarVector) { runFileTest("var.init.hlsl"); }
TEST_F(FileTest, VarInitMatrixMxN) { runFileTest("var.init.matrix.mxn.hlsl"); }
TEST_F(FileTest, VarInitMatrixMx1) { runFileTest("var.init.matrix.mx1.hlsl"); }
TEST_F(FileTest, VarInitMatrix1xN) { runFileTest("var.init.matrix.1xn.hlsl"); }
TEST_F(FileTest, VarInitMatrix1x1) { runFileTest("var.init.matrix.1x1.hlsl"); }
TEST_F(FileTest, VarInitStruct) { runFileTest("var.init.struct.hlsl"); }
TEST_F(FileTest, VarInitArray) { runFileTest("var.init.array.hlsl"); }
TEST_F(FileTest, StaticVar) { runFileTest("var.static.hlsl"); }

// For prefix/postfix increment/decrement
TEST_F(FileTest, UnaryOpPrefixIncrement) {
  runFileTest("unary-op.prefix-inc.hlsl");
}
TEST_F(FileTest, UnaryOpPrefixIncrementMatrix) {
  runFileTest("unary-op.prefix-inc.matrix.hlsl");
}
TEST_F(FileTest, UnaryOpPrefixDecrement) {
  runFileTest("unary-op.prefix-dec.hlsl");
}
TEST_F(FileTest, UnaryOpPrefixDecrementMatrix) {
  runFileTest("unary-op.prefix-dec.matrix.hlsl");
}
TEST_F(FileTest, UnaryOpPostfixIncrement) {
  runFileTest("unary-op.postfix-inc.hlsl");
}
TEST_F(FileTest, UnaryOpPostfixIncrementMatrix) {
  runFileTest("unary-op.postfix-inc.matrix.hlsl");
}
TEST_F(FileTest, UnaryOpPostfixDecrement) {
  runFileTest("unary-op.postfix-dec.hlsl");
}
TEST_F(FileTest, UnaryOpPostfixDecrementMatrix) {
  runFileTest("unary-op.postfix-dec.matrix.hlsl");
}

// For unary operators
TEST_F(FileTest, UnaryOpPlus) { runFileTest("unary-op.plus.hlsl"); }
TEST_F(FileTest, UnaryOpMinus) { runFileTest("unary-op.minus.hlsl"); }
TEST_F(FileTest, UnaryOpLogicalNot) {
  runFileTest("unary-op.logical-not.hlsl");
}

// For assignments
TEST_F(FileTest, BinaryOpAssign) { runFileTest("binary-op.assign.hlsl"); }
TEST_F(FileTest, BinaryOpAssignComposite) {
  runFileTest("binary-op.assign.composite.hlsl");
}

// For comma binary operator
TEST_F(FileTest, BinaryOpComma) { runFileTest("binary-op.comma.hlsl"); }

// For arithmetic binary operators
TEST_F(FileTest, BinaryOpScalarArithmetic) {
  runFileTest("binary-op.arithmetic.scalar.hlsl");
}
TEST_F(FileTest, BinaryOpVectorArithmetic) {
  runFileTest("binary-op.arithmetic.vector.hlsl");
}
TEST_F(FileTest, BinaryOpMatrixArithmetic) {
  runFileTest("binary-op.arithmetic.matrix.hlsl");
}
TEST_F(FileTest, BinaryOpMixedArithmetic) {
  runFileTest("binary-op.arithmetic.mixed.hlsl");
}

// For arithmetic assignments
TEST_F(FileTest, BinaryOpScalarArithAssign) {
  runFileTest("binary-op.arith-assign.scalar.hlsl");
}
TEST_F(FileTest, BinaryOpVectorArithAssign) {
  runFileTest("binary-op.arith-assign.vector.hlsl");
}
TEST_F(FileTest, BinaryOpMatrixArithAssign) {
  runFileTest("binary-op.arith-assign.matrix.hlsl");
}
TEST_F(FileTest, BinaryOpMixedArithAssign) {
  runFileTest("binary-op.arith-assign.mixed.hlsl");
}

// For bitwise binary operators
TEST_F(FileTest, BinaryOpScalarBitwise) {
  runFileTest("binary-op.bitwise.scalar.hlsl");
}
TEST_F(FileTest, BinaryOpVectorBitwise) {
  runFileTest("binary-op.bitwise.vector.hlsl");
}

// For bitwise assignments
TEST_F(FileTest, BinaryOpScalarBitwiseAssign) {
  runFileTest("binary-op.bitwise-assign.scalar.hlsl");
}
TEST_F(FileTest, BinaryOpVectorBitwiseAssign) {
  runFileTest("binary-op.bitwise-assign.vector.hlsl");
}

// For comparison operators
TEST_F(FileTest, BinaryOpScalarComparison) {
  runFileTest("binary-op.comparison.scalar.hlsl");
}
TEST_F(FileTest, BinaryOpVectorComparison) {
  runFileTest("binary-op.comparison.vector.hlsl");
}

// For logical binary operators
TEST_F(FileTest, BinaryOpLogicalAnd) {
  runFileTest("binary-op.logical-and.hlsl");
}
TEST_F(FileTest, BinaryOpLogicalOr) {
  runFileTest("binary-op.logical-or.hlsl");
}

// For ternary operators
TEST_F(FileTest, TernaryOpConditionalOp) {
  runFileTest("ternary-op.cond-op.hlsl");
}

// For vector accessing/swizzling operators
TEST_F(FileTest, OpVectorSwizzle) { runFileTest("op.vector.swizzle.hlsl"); }
TEST_F(FileTest, OpVectorSize1Swizzle) {
  runFileTest("op.vector.swizzle.size1.hlsl");
}
TEST_F(FileTest, OpVectorAccess) { runFileTest("op.vector.access.hlsl"); }

// For matrix accessing/swizzling operators
TEST_F(FileTest, OpMatrixAccessMxN) {
  runFileTest("op.matrix.access.mxn.hlsl");
}
TEST_F(FileTest, OpMatrixAccessMx1) {
  runFileTest("op.matrix.access.mx1.hlsl");
}
TEST_F(FileTest, OpMatrixAccess1xN) {
  runFileTest("op.matrix.access.1xn.hlsl");
}
TEST_F(FileTest, OpMatrixAccess1x1) {
  runFileTest("op.matrix.access.1x1.hlsl");
}

// For struct & array accessing operator
TEST_F(FileTest, OpStructAccess) { runFileTest("op.struct.access.hlsl"); }
TEST_F(FileTest, OpArrayAccess) { runFileTest("op.array.access.hlsl"); }

// For buffer accessing operator
TEST_F(FileTest, OpBufferAccess) { runFileTest("op.buffer.access.hlsl"); }
TEST_F(FileTest, OpCBufferAccess) { runFileTest("op.cbuffer.access.hlsl"); }
TEST_F(FileTest, OpConstantBufferAccess) {
  runFileTest("op.constant-buffer.access.hlsl");
}
TEST_F(FileTest, OpStructuredBufferAccess) {
  runFileTest("op.structured-buffer.access.hlsl");
}
TEST_F(FileTest, OpRWStructuredBufferAccess) {
  runFileTest("op.rw-structured-buffer.access.hlsl");
}

// For Texture/RWTexture accessing operator (operator[])
TEST_F(FileTest, OpTextureAccess) { runFileTest("op.texture.access.hlsl"); }
TEST_F(FileTest, OpRWTextureAccess) { runFileTest("op.rwtexture.access.hlsl"); }

// For Texture.mips[][] operator
TEST_F(FileTest, OpTextureMipsAccess) {
  runFileTest("op.texture.mips-access.hlsl");
}
// For Texture2MD(Array).sample[][] operator
TEST_F(FileTest, OpTextureSampleAccess) {
  runFileTest("op.texture.sample-access.hlsl");
}

// For casting
TEST_F(FileTest, CastNoOp) { runFileTest("cast.no-op.hlsl"); }
TEST_F(FileTest, CastImplicit2Bool) { runFileTest("cast.2bool.implicit.hlsl"); }
TEST_F(FileTest, CastExplicit2Bool) { runFileTest("cast.2bool.explicit.hlsl"); }
TEST_F(FileTest, CastImplicit2SInt) { runFileTest("cast.2sint.implicit.hlsl"); }
TEST_F(FileTest, CastExplicit2SInt) { runFileTest("cast.2sint.explicit.hlsl"); }
TEST_F(FileTest, CastImplicit2UInt) { runFileTest("cast.2uint.implicit.hlsl"); }
TEST_F(FileTest, CastExplicit2UInt) { runFileTest("cast.2uint.explicit.hlsl"); }
TEST_F(FileTest, CastImplicit2FP) { runFileTest("cast.2fp.implicit.hlsl"); }
TEST_F(FileTest, CastExplicit2FP) { runFileTest("cast.2fp.explicit.hlsl"); }

// For vector/matrix splatting and trunction
TEST_F(FileTest, CastTruncateVector) { runFileTest("cast.vector.trunc.hlsl"); }
TEST_F(FileTest, CastSplatVector) { runFileTest("cast.vector.splat.hlsl"); }
TEST_F(FileTest, CastSplatMatrix) { runFileTest("cast.matrix.splat.hlsl"); }

// For if statements
TEST_F(FileTest, IfStmtPlainAssign) { runFileTest("if-stmt.plain.hlsl"); }
TEST_F(FileTest, IfStmtNestedIfStmt) { runFileTest("if-stmt.nested.hlsl"); }
TEST_F(FileTest, IfStmtConstCondition) {
  runFileTest("if-stmt.const-cond.hlsl");
}

// For switch statements
TEST_F(FileTest, SwitchStmtUsingOpSwitch) {
  runFileTest("switch-stmt.opswitch.hlsl");
}
TEST_F(FileTest, SwitchStmtUsingIfStmt) {
  runFileTest("switch-stmt.ifstmt.hlsl");
}

// For for statements
TEST_F(FileTest, ForStmtPlainAssign) { runFileTest("for-stmt.plain.hlsl"); }
TEST_F(FileTest, ForStmtNestedForStmt) { runFileTest("for-stmt.nested.hlsl"); }
TEST_F(FileTest, ForStmtContinue) { runFileTest("for-stmt.continue.hlsl"); }
TEST_F(FileTest, ForStmtBreak) { runFileTest("for-stmt.break.hlsl"); }

// For while statements
TEST_F(FileTest, WhileStmtPlain) { runFileTest("while-stmt.plain.hlsl"); }
TEST_F(FileTest, WhileStmtNested) { runFileTest("while-stmt.nested.hlsl"); }
TEST_F(FileTest, WhileStmtContinue) { runFileTest("while-stmt.continue.hlsl"); }
TEST_F(FileTest, WhileStmtBreak) { runFileTest("while-stmt.break.hlsl"); }

// For do statements
TEST_F(FileTest, DoStmtPlain) { runFileTest("do-stmt.plain.hlsl"); }
TEST_F(FileTest, DoStmtNested) { runFileTest("do-stmt.nested.hlsl"); }
TEST_F(FileTest, DoStmtContinue) { runFileTest("do-stmt.continue.hlsl"); }
TEST_F(FileTest, DoStmtBreak) { runFileTest("do-stmt.break.hlsl"); }

// For break statements (mix of breaks in loops and switch)
TEST_F(FileTest, BreakStmtMixed) { runFileTest("break-stmt.mixed.hlsl"); }

// For discard statement
TEST_F(FileTest, Discard) { runFileTest("cf.discard.hlsl"); }

// For early returns
TEST_F(FileTest, EarlyReturn) { runFileTest("cf.return.early.hlsl"); }
TEST_F(FileTest, EarlyReturnFloat4) {
  runFileTest("cf.return.early.float4.hlsl");
}

// For returning a structure
TEST_F(FileTest, ReturnStruct) { runFileTest("cf.return.struct.hlsl"); }

// For control flows
TEST_F(FileTest, ControlFlowNestedIfForStmt) { runFileTest("cf.if.for.hlsl"); }
TEST_F(FileTest, ControlFlowLogicalAnd) { runFileTest("cf.logical-and.hlsl"); }
TEST_F(FileTest, ControlFlowLogicalOr) { runFileTest("cf.logical-or.hlsl"); }
TEST_F(FileTest, ControlFlowConditionalOp) { runFileTest("cf.cond-op.hlsl"); }

// For functions
TEST_F(FileTest, FunctionCall) { runFileTest("fn.call.hlsl"); }
TEST_F(FileTest, FunctionInOutParam) { runFileTest("fn.param.inout.hlsl"); }

// For struct methods
TEST_F(FileTest, StructMethodCallNormal) {
  runFileTest("method.struct.method.hlsl");
}

// For semantics
TEST_F(FileTest, SemanticPositionVS) {
  runFileTest("semantic.position.vs.hlsl");
}
TEST_F(FileTest, SemanticPositionPS) {
  runFileTest("semantic.position.ps.hlsl");
}
TEST_F(FileTest, SemanticVertexIDVS) {
  runFileTest("semantic.vertex-id.vs.hlsl");
}
TEST_F(FileTest, SemanticInstanceIDVS) {
  runFileTest("semantic.instance-id.vs.hlsl");
}
TEST_F(FileTest, SemanticInstanceIDPS) {
  runFileTest("semantic.instance-id.ps.hlsl");
}
TEST_F(FileTest, SemanticTargetPS) { runFileTest("semantic.target.ps.hlsl"); }
TEST_F(FileTest, SemanticDepthPS) { runFileTest("semantic.depth.ps.hlsl"); }
TEST_F(FileTest, SemanticDepthGreaterEqualPS) {
  runFileTest("semantic.depth-greater-equal.ps.hlsl");
}
TEST_F(FileTest, SemanticDepthLessEqualPS) {
  runFileTest("semantic.depth-less-equal.ps.hlsl");
}
TEST_F(FileTest, SemanticIsFrontFacePS) {
  runFileTest("semantic.is-front-face.ps.hlsl");
}
TEST_F(FileTest, SemanticArbitrary) { runFileTest("semantic.arbitrary.hlsl"); }
TEST_F(FileTest, SemanticArbitraryDeclLocation) {
  runFileTest("semantic.arbitrary.location.decl.hlsl");
}
TEST_F(FileTest, SemanticArbitraryAlphaLocation) {
  runFileTest("semantic.arbitrary.location.alpha.hlsl");
}
TEST_F(FileTest, SemanticDuplication) {
  runFileTest("semantic.duplication.hlsl", /*expectSuccess*/ false);
}
TEST_F(FileTest, SemanticDispatchThreadId) {
  runFileTest("semantic.dispatch-thread-id.cs.hlsl");
}
TEST_F(FileTest, SemanticGroupID) {
  runFileTest("semantic.group-id.cs.hlsl");
}
TEST_F(FileTest, SemanticGroupThreadID) {
  runFileTest("semantic.group-thread-id.cs.hlsl");
}
TEST_F(FileTest, SemanticGroupIndex) {
  runFileTest("semantic.group-index.cs.hlsl");
}

// For texture methods
TEST_F(FileTest, TextureSample) { runFileTest("texture.sample.hlsl"); }
TEST_F(FileTest, TextureArraySample) {
  runFileTest("texture.array.sample.hlsl");
}
TEST_F(FileTest, TextureLoad) { runFileTest("texture.load.hlsl"); }
TEST_F(FileTest, TextureArrayLoad) { runFileTest("texture.array.load.hlsl"); }
TEST_F(FileTest, TextureGetDimensions) {
  runFileTest("texture.get-dimensions.hlsl");
}
TEST_F(FileTest, TextureCalculateLevelOfDetail) {
  runFileTest("texture.calculate-lod.hlsl");
}
TEST_F(FileTest, TextureGather) { runFileTest("texture.gather.hlsl"); }
TEST_F(FileTest, TextureArrayGather) {
  runFileTest("texture.array.gather.hlsl");
}
TEST_F(FileTest, TextureSampleLevel) {
  runFileTest("texture.sample-level.hlsl");
}
TEST_F(FileTest, TextureArraySampleLevel) {
  runFileTest("texture.array.sample-level.hlsl");
}
TEST_F(FileTest, TextureSampleBias) { runFileTest("texture.sample-bias.hlsl"); }
TEST_F(FileTest, TextureArraySampleBias) {
  runFileTest("texture.array.sample-bias.hlsl");
}
TEST_F(FileTest, TextureSampleGrad) { runFileTest("texture.sample-grad.hlsl"); }
TEST_F(FileTest, TextureArraySampleGrad) {
  runFileTest("texture.array.sample-grad.hlsl");
}

TEST_F(FileTest, StructuredBufferLoad) {
  runFileTest("method.structured-buffer.load.hlsl");
}
TEST_F(FileTest, StructuredBufferGetDimensions) {
  runFileTest("method.structured-buffer.get-dimensions.hlsl");
}
TEST_F(FileTest, AppendStructuredBufferAppend) {
  runFileTest("method.append-structured-buffer.append.hlsl");
}
TEST_F(FileTest, AppendStructuredBufferGetDimensions) {
  runFileTest("method.append-structured-buffer.get-dimensions.hlsl");
}
TEST_F(FileTest, ConsumeStructuredBufferConsume) {
  runFileTest("method.consume-structured-buffer.consume.hlsl");
}
TEST_F(FileTest, ConsumeStructuredBufferGetDimensions) {
  runFileTest("method.consume-structured-buffer.get-dimensions.hlsl");
}

// For ByteAddressBuffer methods
TEST_F(FileTest, ByteAddressBufferLoad) {
  runFileTest("method.byte-address-buffer.load.hlsl");
}
TEST_F(FileTest, ByteAddressBufferStore) {
  runFileTest("method.byte-address-buffer.store.hlsl");
}
TEST_F(FileTest, ByteAddressBufferGetDimensions) {
  runFileTest("method.byte-address-buffer.get-dimensions.hlsl");
}

// For Buffer/RWBuffer methods
TEST_F(FileTest, BufferLoad) { runFileTest("buffer.load.hlsl"); }
TEST_F(FileTest, BufferWrite) { runFileTest("buffer.write.hlsl"); }
TEST_F(FileTest, BufferGetDimensions) {
  runFileTest("buffer.get-dimensions.hlsl");
}

// For RWTexture methods
TEST_F(FileTest, RWTextureLoad) { runFileTest("rwtexture.load.hlsl"); }
TEST_F(FileTest, RWTextureWrite) { runFileTest("rwtexture.write.hlsl"); }
TEST_F(FileTest, RWTextureGetDimensions) {
  runFileTest("rwtexture.get-dimensions.hlsl");
}

// For intrinsic functions
TEST_F(FileTest, IntrinsicsCountBits) {
  runFileTest("intrinsics.countbits.hlsl");
}
TEST_F(FileTest, IntrinsicsDot) { runFileTest("intrinsics.dot.hlsl"); }
TEST_F(FileTest, IntrinsicsMul) { runFileTest("intrinsics.mul.hlsl"); }
TEST_F(FileTest, IntrinsicsAll) { runFileTest("intrinsics.all.hlsl"); }
TEST_F(FileTest, IntrinsicsAny) { runFileTest("intrinsics.any.hlsl"); }
TEST_F(FileTest, IntrinsicsAsfloat) { runFileTest("intrinsics.asfloat.hlsl"); }
TEST_F(FileTest, IntrinsicsAsint) { runFileTest("intrinsics.asint.hlsl"); }
TEST_F(FileTest, IntrinsicsAsuint) { runFileTest("intrinsics.asuint.hlsl"); }
TEST_F(FileTest, IntrinsicsRound) { runFileTest("intrinsics.round.hlsl"); }
TEST_F(FileTest, IntrinsicsAbs) { runFileTest("intrinsics.abs.hlsl"); }
TEST_F(FileTest, IntrinsicsCross) { runFileTest("intrinsics.cross.hlsl"); }
TEST_F(FileTest, IntrinsicsCeil) { runFileTest("intrinsics.ceil.hlsl"); }
TEST_F(FileTest, IntrinsicsClamp) { runFileTest("intrinsics.clamp.hlsl"); }
TEST_F(FileTest, IntrinsicsClip) { runFileTest("intrinsics.clip.hlsl"); }
TEST_F(FileTest, IntrinsicsDegrees) { runFileTest("intrinsics.degrees.hlsl"); }
TEST_F(FileTest, IntrinsicsDistance) {
  runFileTest("intrinsics.distance.hlsl");
}
TEST_F(FileTest, IntrinsicsRadians) { runFileTest("intrinsics.radians.hlsl"); }
TEST_F(FileTest, IntrinsicsDeterminant) {
  runFileTest("intrinsics.determinant.hlsl");
}
TEST_F(FileTest, IntrinsicsExp) { runFileTest("intrinsics.exp.hlsl"); }
TEST_F(FileTest, IntrinsicsExp2) { runFileTest("intrinsics.exp2.hlsl"); }
TEST_F(FileTest, IntrinsicsFirstBitHigh) {
  runFileTest("intrinsics.firstbithigh.hlsl");
}
TEST_F(FileTest, IntrinsicsFirstBitLow) {
  runFileTest("intrinsics.firstbitlow.hlsl");
}
TEST_F(FileTest, IntrinsicsFloor) { runFileTest("intrinsics.floor.hlsl"); }
TEST_F(FileTest, IntrinsicsFma) { runFileTest("intrinsics.fma.hlsl"); }
TEST_F(FileTest, IntrinsicsFmod) { runFileTest("intrinsics.fmod.hlsl"); }
TEST_F(FileTest, IntrinsicsFrac) { runFileTest("intrinsics.frac.hlsl"); }
TEST_F(FileTest, IntrinsicsFrexp) { runFileTest("intrinsics.frexp.hlsl"); }
TEST_F(FileTest, IntrinsicsIsFinite) {
  runFileTest("intrinsics.isfinite.hlsl");
}
TEST_F(FileTest, IntrinsicsIsInf) { runFileTest("intrinsics.isinf.hlsl"); }
TEST_F(FileTest, IntrinsicsIsNan) { runFileTest("intrinsics.isnan.hlsl"); }
TEST_F(FileTest, IntrinsicsLength) { runFileTest("intrinsics.length.hlsl"); }
TEST_F(FileTest, IntrinsicsLdexp) { runFileTest("intrinsics.ldexp.hlsl"); }
TEST_F(FileTest, IntrinsicsLerp) { runFileTest("intrinsics.lerp.hlsl"); }
TEST_F(FileTest, IntrinsicsLog) { runFileTest("intrinsics.log.hlsl"); }
TEST_F(FileTest, IntrinsicsLog10) { runFileTest("intrinsics.log10.hlsl"); }
TEST_F(FileTest, IntrinsicsLog2) { runFileTest("intrinsics.log2.hlsl"); }
TEST_F(FileTest, IntrinsicsMin) { runFileTest("intrinsics.min.hlsl"); }
TEST_F(FileTest, IntrinsicsModf) { runFileTest("intrinsics.modf.hlsl"); }
TEST_F(FileTest, IntrinsicsMad) { runFileTest("intrinsics.mad.hlsl"); }
TEST_F(FileTest, IntrinsicsMax) { runFileTest("intrinsics.max.hlsl"); }
TEST_F(FileTest, IntrinsicsNormalize) {
  runFileTest("intrinsics.normalize.hlsl");
}
TEST_F(FileTest, IntrinsicsPow) { runFileTest("intrinsics.pow.hlsl"); }
TEST_F(FileTest, IntrinsicsRsqrt) { runFileTest("intrinsics.rsqrt.hlsl"); }
TEST_F(FileTest, IntrinsicsFloatSign) {
  runFileTest("intrinsics.floatsign.hlsl");
}
TEST_F(FileTest, IntrinsicsIntSign) { runFileTest("intrinsics.intsign.hlsl"); }
TEST_F(FileTest, IntrinsicsReflect) { runFileTest("intrinsics.reflect.hlsl"); }
TEST_F(FileTest, IntrinsicsRefract) { runFileTest("intrinsics.refract.hlsl"); }
TEST_F(FileTest, IntrinsicsReverseBits) {
  runFileTest("intrinsics.reversebits.hlsl");
}
TEST_F(FileTest, IntrinsicsSaturate) {
  runFileTest("intrinsics.saturate.hlsl");
}
TEST_F(FileTest, IntrinsicsSmoothStep) {
  runFileTest("intrinsics.smoothstep.hlsl");
}
TEST_F(FileTest, IntrinsicsStep) { runFileTest("intrinsics.step.hlsl"); }
TEST_F(FileTest, IntrinsicsSqrt) { runFileTest("intrinsics.sqrt.hlsl"); }
TEST_F(FileTest, IntrinsicsTranspose) {
  runFileTest("intrinsics.transpose.hlsl");
}
TEST_F(FileTest, IntrinsicsTrunc) { runFileTest("intrinsics.trunc.hlsl"); }

// For intrinsic trigonometric functions
TEST_F(FileTest, IntrinsicsSin) { runFileTest("intrinsics.sin.hlsl"); }
TEST_F(FileTest, IntrinsicsCos) { runFileTest("intrinsics.cos.hlsl"); }
TEST_F(FileTest, IntrinsicsSinCos) { runFileTest("intrinsics.sincos.hlsl"); }
TEST_F(FileTest, IntrinsicsTan) { runFileTest("intrinsics.tan.hlsl"); }
TEST_F(FileTest, IntrinsicsSinh) { runFileTest("intrinsics.sinh.hlsl"); }
TEST_F(FileTest, IntrinsicsCosh) { runFileTest("intrinsics.cosh.hlsl"); }
TEST_F(FileTest, IntrinsicsTanh) { runFileTest("intrinsics.tanh.hlsl"); }
TEST_F(FileTest, IntrinsicsAsin) { runFileTest("intrinsics.asin.hlsl"); }
TEST_F(FileTest, IntrinsicsAcos) { runFileTest("intrinsics.acos.hlsl"); }
TEST_F(FileTest, IntrinsicsAtan) { runFileTest("intrinsics.atan.hlsl"); }
TEST_F(FileTest, IntrinsicsAtan2) { runFileTest("intrinsics.atan2.hlsl"); }

// For attributes
TEST_F(FileTest, AttributeNumThreads) {
  runFileTest("attribute.numthreads.hlsl");
}
TEST_F(FileTest, AttributeMissingNumThreads) {
  runFileTest("attribute.numthreads.missing.hlsl");
}

// Vulkan/SPIR-V specific
TEST_F(FileTest, SpirvStorageClass) { runFileTest("spirv.storage-class.hlsl"); }

TEST_F(FileTest, SpirvEntryFunctionWrapper) {
  runFileTest("spirv.entry-function.wrapper.hlsl");
}
TEST_F(FileTest, SpirvEntryFunctionInOut) {
  runFileTest("spirv.entry-function.inout.hlsl");
}

TEST_F(FileTest, VulkanLocation) { runFileTest("vk.location.hlsl"); }
TEST_F(FileTest, VulkanLocationInputExplicitOutputImplicit) {
  runFileTest("vk.location.exp-in.hlsl");
}
TEST_F(FileTest, VulkanLocationInputImplicitOutputExplicit) {
  runFileTest("vk.location.exp-out.hlsl");
}
TEST_F(FileTest, VulkanLocationTooLarge) {
  runFileTest("vk.location.large.hlsl", /*expectSuccess*/ false);
}
TEST_F(FileTest, VulkanLocationReassigned) {
  runFileTest("vk.location.reassign.hlsl", /*expectSuccess*/ false);
}
TEST_F(FileTest, VulkanLocationPartiallyAssigned) {
  runFileTest("vk.location.mixed.hlsl", /*expectSuccess*/ false);
}

TEST_F(FileTest, SpirvInterpolation) {
  runFileTest("spirv.interpolation.hlsl");
}
TEST_F(FileTest, SpirvInterpolationError) {
  runFileTest("spirv.interpolation.error.hlsl", /*expectSuccess*/ false);
}

TEST_F(FileTest, VulkanExplicitBinding) {
  // Resource binding from [[vk::binding()]]
  runFileTest("vk.binding.explicit.hlsl");
}
TEST_F(FileTest, VulkanImplicitBinding) {
  // Resource binding from neither [[vk::binding()]] or :register()
  runFileTest("vk.binding.implicit.hlsl");
}
TEST_F(FileTest, VulkanRegisterBinding) {
  // Resource binding from :register()
  runFileTest("vk.binding.register.hlsl");
}
TEST_F(FileTest, VulkanExplicitBindingReassigned) {
  runFileTest("vk.binding.explicit.error.hlsl", /*expectSuccess*/ false);
}
TEST_F(FileTest, VulkanLayoutCBufferStd140) {
  runFileTest("vk.layout.cbuffer.std140.hlsl");
}
TEST_F(FileTest, VulkanLayoutCBufferNestedStd140) {
  runFileTest("vk.layout.cbuffer.nested.std140.hlsl");
}
TEST_F(FileTest, VulkanLayoutSBufferStd430) {
  runFileTest("vk.layout.sbuffer.std430.hlsl");
}
TEST_F(FileTest, VulkanLayoutSBufferNestedStd430) {
  runFileTest("vk.layout.sbuffer.nested.std430.hlsl");
}
TEST_F(FileTest, VulkanLayoutAppendSBufferStd430) {
  runFileTest("vk.layout.asbuffer.std430.hlsl");
}
TEST_F(FileTest, VulkanLayoutConsumeSBufferStd430) {
  runFileTest("vk.layout.csbuffer.std430.hlsl");
}

} // namespace
