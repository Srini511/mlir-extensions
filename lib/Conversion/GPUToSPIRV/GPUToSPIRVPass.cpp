//===- GPUToSPIRVPass.cpp -  --------------*- C++ -*-===//
//
// Copyright 2022 Intel Corporation
// Part of the IMEX Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file extends upstream GPUToSPIRV Pass that converts GPU ops to SPIR-V
/// by adding more conversion patterns like SCF, math and control flow. This
/// pass only converts gpu.func ops inside gpu.module op.
///
//===----------------------------------------------------------------------===//
#include "imex/Conversion/GPUToSPIRV/GPUToSPIRVPass.h"

#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/BuiltinTypes.h"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/Debug.h>
#include <mlir/Conversion/ArithToSPIRV/ArithToSPIRV.h>
#include <mlir/Conversion/ControlFlowToSPIRV/ControlFlowToSPIRV.h>
#include <mlir/Conversion/FuncToSPIRV/FuncToSPIRV.h>
#include <mlir/Conversion/GPUToSPIRV/GPUToSPIRV.h>
#include <mlir/Conversion/IndexToSPIRV/IndexToSPIRV.h>
#include <mlir/Conversion/MathToSPIRV/MathToSPIRV.h>
#include <mlir/Conversion/MemRefToSPIRV/MemRefToSPIRV.h>
#include <mlir/Conversion/SCFToSPIRV/SCFToSPIRV.h>
#include <mlir/Conversion/VectorToSPIRV/VectorToSPIRV.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/GPU/IR/GPUDialect.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVDialect.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVOps.h>
#include <mlir/Dialect/SPIRV/IR/SPIRVTypes.h>
#include <mlir/Dialect/SPIRV/Transforms/SPIRVConversion.h>
#include <mlir/Dialect/XeGPU/IR/XeGPU.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Matchers.h>
#include <mlir/Support/LLVM.h>
#include <mlir/Transforms/DialectConversion.h>

#include <mlir/Pass/Pass.h>

namespace imex {
#define GEN_PASS_DEF_CONVERTGPUXTOSPIRV
#include "imex/Conversion/Passes.h.inc"
} // namespace imex

namespace imex {

/// Pass to lower GPU Dialect to SPIR-V. The pass only converts the gpu.func ops
/// inside gpu.module ops. i.e., the function that are referenced in
/// gpu.launch_func ops. For each such function
///
/// 1) Create a spirv::ModuleOp, and clone the function into spirv::ModuleOp
/// (the original function is still needed by the gpu::LaunchKernelOp, so cannot
/// replace it).
///
/// 2) Lower the body of the spirv::ModuleOp.
class GPUXToSPIRVPass : public impl::ConvertGPUXToSPIRVBase<GPUXToSPIRVPass> {
public:
  explicit GPUXToSPIRVPass(bool mapMemorySpace)
      : mapMemorySpace(mapMemorySpace) {}
  void runOnOperation() override;

private:
  bool mapMemorySpace;
};

class PrintfOpPattern : public mlir::OpConversionPattern<mlir::gpu::PrintfOp> {
public:
  using mlir::OpConversionPattern<mlir::gpu::PrintfOp>::OpConversionPattern;
  mlir::LogicalResult
  matchAndRewrite(mlir::gpu::PrintfOp gpuPrintfOp, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    auto loc = gpuPrintfOp.getLoc();

    auto funcOp = rewriter.getBlock()
                      ->getParent()
                      ->getParentOfType<mlir::spirv::FuncOp>();

    auto moduleOp = funcOp->getParentOfType<mlir::spirv::ModuleOp>();

    const char formatStringPrefix[] = "printfMsg";
    unsigned stringNumber = 0;
    mlir::SmallString<16> globalVarName;
    mlir::spirv::GlobalVariableOp globalVar;

    // formulate spirv global variable name
    do {
      globalVarName.clear();
      (formatStringPrefix + llvm::Twine(stringNumber++))
          .toStringRef(globalVarName);
    } while (moduleOp.lookupSymbol(globalVarName));

    auto i8Type = rewriter.getI8Type();
    auto i32Type = rewriter.getI32Type();

    unsigned scNum = 0;
    auto createSpecConstant = [&](unsigned value) {
      auto attr = rewriter.getI8IntegerAttr(value);
      mlir::SmallString<16> specCstName;
      (llvm::Twine(globalVarName) + "_sc" + llvm::Twine(scNum++))
          .toStringRef(specCstName);

      return rewriter.create<mlir::spirv::SpecConstantOp>(
          loc, rewriter.getStringAttr(specCstName), attr);
    };

    // define GlobalVarOp with printf format string using SpecConstants
    // and make composite of SpecConstants
    {
      mlir::Operation *parent =
          mlir::SymbolTable::getNearestSymbolTable(gpuPrintfOp->getParentOp());

      mlir::ConversionPatternRewriter::InsertionGuard guard(rewriter);

      mlir::Block &entryBlock = *parent->getRegion(0).begin();
      rewriter.setInsertionPointToStart(
          &entryBlock); // insertion point at module level

      // Create Constituents with SpecConstant to construct
      // SpecConstantCompositeOp
      llvm::SmallString<20> formatString(gpuPrintfOp.getFormat());
      formatString.push_back('\0'); // Null terminate for C
      mlir::SmallVector<mlir::Attribute, 4> constituents;
      for (auto c : formatString) {
        auto cSpecConstantOp = createSpecConstant(c);
        constituents.push_back(mlir::SymbolRefAttr::get(cSpecConstantOp));
      }

      // Create specialization constant composite defined via spirv.SpecConstant
      size_t contentSize = constituents.size();
      auto globalType = mlir::spirv::ArrayType::get(i8Type, contentSize);
      mlir::spirv::SpecConstantCompositeOp specCstComposite;
      mlir::SmallString<16> specCstCompositeName;
      (llvm::Twine(globalVarName) + "_scc").toStringRef(specCstCompositeName);
      specCstComposite = rewriter.create<mlir::spirv::SpecConstantCompositeOp>(
          loc, mlir::TypeAttr::get(globalType),
          rewriter.getStringAttr(specCstCompositeName),
          rewriter.getArrayAttr(constituents));

      // Define GlobalVariable initialized from Constant Composite
      globalVar = rewriter.create<mlir::spirv::GlobalVariableOp>(
          loc,
          mlir::spirv::PointerType::get(
              globalType, mlir::spirv::StorageClass::UniformConstant),
          globalVarName, mlir::FlatSymbolRefAttr::get(specCstComposite));
      globalVar->setAttr("Constant", rewriter.getUnitAttr());
    }

    // Get SSA value of Global variable
    mlir::Value globalPtr =
        rewriter.create<mlir::spirv::AddressOfOp>(loc, globalVar);

    mlir::Value fmtStr = rewriter.create<mlir::spirv::BitcastOp>(
        loc,
        mlir::spirv::PointerType::get(
            i8Type, mlir::spirv::StorageClass::UniformConstant),
        globalPtr);

    // Get printf arguments
    auto argsRange = adaptor.getArgs();
    mlir::SmallVector<mlir::Value, 4> printfArgs;
    printfArgs.reserve(argsRange.size() + 1);
    printfArgs.append(argsRange.begin(), argsRange.end());

    rewriter.create<mlir::spirv::CLPrintfOp>(loc, i32Type, fmtStr, printfArgs);

    rewriter.eraseOp(gpuPrintfOp);

    return mlir::success();
  }
};

void populateGPUPrintfToSPIRVPatterns(mlir::SPIRVTypeConverter &typeConverter,
                                      mlir::RewritePatternSet &patterns) {

  patterns.add<PrintfOpPattern>(typeConverter, patterns.getContext());
}

// This op:
//   vector.create_mask %maskVal : vector<vWidth x i1>
// is lowered to:
//   if maskVal < 0
//     mask = 0
//   else if maskVal < vWidth
//     mask = (1 << maskVal) - 1
//   else
//     mask = all ones
class VectorMaskConversionPattern final
    : public mlir::OpConversionPattern<mlir::vector::CreateMaskOp> {
public:
  using OpConversionPattern<mlir::vector::CreateMaskOp>::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::vector::CreateMaskOp vMaskOp, OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    mlir::VectorType vTy = vMaskOp.getVectorType();
    if (vTy.getRank() != 1)
      return mlir::failure();

    auto vWidth = vTy.getNumElements();
    auto vWidthConst = rewriter.create<mlir::arith::ConstantOp>(
        vMaskOp.getLoc(), rewriter.getI32IntegerAttr(vWidth));
    auto maskVal = adaptor.getOperands()[0];
    maskVal = rewriter.create<mlir::arith::TruncIOp>(
        vMaskOp.getLoc(), rewriter.getI32Type(), maskVal);

    // maskVal < vWidth
    auto cmp = rewriter.create<mlir::arith::CmpIOp>(
        vMaskOp.getLoc(), mlir::arith::CmpIPredicate::slt, maskVal,
        vWidthConst);
    auto one = rewriter.create<mlir::arith::ConstantOp>(
        vMaskOp.getLoc(), rewriter.getI32Type(), rewriter.getI32IntegerAttr(1));
    auto shift = rewriter.create<mlir::spirv::ShiftLeftLogicalOp>(
        vMaskOp.getLoc(), one, maskVal);
    auto mask1 =
        rewriter.create<mlir::arith::SubIOp>(vMaskOp.getLoc(), shift, one);
    auto mask2 = rewriter.create<mlir::arith::ConstantOp>(
        vMaskOp.getLoc(), rewriter.getI32Type(),
        rewriter.getI32IntegerAttr(0xFFFFFFFF));
    mlir::Value sel = rewriter.create<mlir::arith::SelectOp>(vMaskOp.getLoc(),
                                                             cmp, mask1, mask2);

    // maskVal < 0
    auto zero = rewriter.create<mlir::arith::ConstantOp>(
        vMaskOp.getLoc(), rewriter.getI32Type(), rewriter.getI32IntegerAttr(0));
    auto cmp2 = rewriter.create<mlir::arith::CmpIOp>(
        vMaskOp.getLoc(), mlir::arith::CmpIPredicate::slt, maskVal, zero);
    sel = rewriter.create<mlir::arith::SelectOp>(vMaskOp.getLoc(), cmp2, zero,
                                                 sel);

    sel = rewriter.create<mlir::arith::TruncIOp>(
        vMaskOp.getLoc(), rewriter.getIntegerType(vWidth), sel);
    auto res = rewriter.create<mlir::spirv::BitcastOp>(
        vMaskOp.getLoc(), mlir::VectorType::get({vWidth}, rewriter.getI1Type()),
        sel);
    vMaskOp->replaceAllUsesWith(res);
    rewriter.eraseOp(vMaskOp);
    return mlir::success();
  }
};

// This pattern converts vector.from_elements op to SPIR-V CompositeInsertOp
class VectorFromElementsConversionPattern final
    : public mlir::OpConversionPattern<mlir::vector::FromElementsOp> {
public:
  using OpConversionPattern<mlir::vector::FromElementsOp>::OpConversionPattern;

  mlir::LogicalResult
  matchAndRewrite(mlir::vector::FromElementsOp fromElementsOp,
                  OpAdaptor adaptor,
                  mlir::ConversionPatternRewriter &rewriter) const override {
    mlir::VectorType vecTy = fromElementsOp.getType();
    if (vecTy.getRank() > 1)
      return rewriter.notifyMatchFailure(fromElementsOp,
                                         "rank > 1 vectors are not supported");

    mlir::Type spirvVecTy = getTypeConverter()->convertType(vecTy);
    if (!spirvVecTy)
      return mlir::failure();

    // if the vector is just constructed from one element
    if (mlir::isa<mlir::spirv::ScalarType>(spirvVecTy)) {
      rewriter.replaceOp(fromElementsOp, adaptor.getElements()[0]);
      return mlir::success();
    }

    auto loc = fromElementsOp.getLoc();
    mlir::Value result = rewriter.create<mlir::spirv::UndefOp>(loc, spirvVecTy);
    for (auto [idx, val] : llvm::enumerate(adaptor.getElements())) {
      result = rewriter.create<mlir::spirv::CompositeInsertOp>(loc, val, result,
                                                               idx);
    }
    rewriter.replaceOp(fromElementsOp, result);
    return mlir::success();
  }
};

void populateVectorToSPIRVPatterns(mlir::SPIRVTypeConverter &typeConverter,
                                   mlir::RewritePatternSet &patterns) {
  patterns
      .add<VectorFromElementsConversionPattern, VectorMaskConversionPattern>(
          typeConverter, patterns.getContext());
}

static bool isGenericVectorTy(mlir::Type type) {
  if (mlir::isa<mlir::spirv::ScalarType>(type))
    return true;
  auto vecSize = mlir::dyn_cast<mlir::VectorType>(type).getNumElements();
  return vecSize == 2 || vecSize == 3 || vecSize == 4 || vecSize == 8 ||
         vecSize == 16;
}

void GPUXToSPIRVPass::runOnOperation() {
  mlir::MLIRContext *context = &getContext();
  mlir::ModuleOp module = getOperation();

  llvm::SmallVector<mlir::Operation *, 1> gpuModules;
  mlir::OpBuilder builder(context);
  module.walk([&](mlir::gpu::GPUModuleOp moduleOp) {
    // For each kernel module (should be only 1 for now, but that is not a
    // requirement here), clone the module for conversion because the
    // gpu.launch function still needs the kernel module.
    builder.setInsertionPoint(moduleOp.getOperation());
    gpuModules.push_back(builder.clone(*moduleOp.getOperation()));
  });

  for (auto gpuModule : gpuModules) {

    // Map MemRef memory space to SPIR-V storage class first if requested.
    if (mapMemorySpace) {
      std::unique_ptr<mlir::ConversionTarget> target =
          mlir::spirv::getMemorySpaceToStorageClassTarget(*context);
      mlir::spirv::MemorySpaceToStorageClassMap memorySpaceMap =
          mlir::spirv::mapMemorySpaceToOpenCLStorageClass;
      mlir::spirv::MemorySpaceToStorageClassConverter converter(memorySpaceMap);

      mlir::RewritePatternSet patterns(context);
      mlir::spirv::convertMemRefTypesAndAttrs(gpuModule, converter);

      if (failed(applyFullConversion(gpuModule, *target, std::move(patterns))))
        return signalPassFailure();
    }

    auto targetAttr = mlir::spirv::lookupTargetEnvOrDefault(gpuModule);
    std::unique_ptr<mlir::ConversionTarget> target =
        mlir::SPIRVConversionTarget::get(targetAttr);

    mlir::RewritePatternSet patterns(context);
    mlir::SPIRVConversionOptions options;
    options.use64bitIndex = true;

    mlir::SPIRVTypeConverter typeConverter(targetAttr, options);

    /// Walk gpu.func and collect root ops for these two special patterns
    /// 1. Pattern to convert arith.truncf (f32 -> bf16) followed by
    ///    arith.bitcast (bf16 -> i16)
    ///    into a SPIR-V convert op.
    /// 2. Pattern to convert arith.bitcast (i16 -> bf16) followed by
    ///    arith.extf (bf16 -> f32)
    ///    into a SPIR-V convert op.
    /// And convert the patterns into spirv bf16<->f32 conversion ops.
    mlir::OpBuilder builder(gpuModule);
    llvm::SmallVector<mlir::Operation *, 16> eraseOps;
    gpuModule->walk([&](mlir::gpu::GPUFuncOp fop) {
      fop->walk([&](mlir::arith::BitcastOp bop) {
        if (auto vecTy = llvm::dyn_cast<mlir::VectorType>(bop.getType())) {
          if (vecTy.getElementType().isInteger(16)) {
            mlir::arith::TruncFOp inputOp =
                llvm::dyn_cast<mlir::arith::TruncFOp>(
                    bop.getOperand().getDefiningOp());
            if (inputOp) {
              if (auto inTy =
                      llvm::dyn_cast<mlir::VectorType>(inputOp.getType())) {
                if (inTy.getElementType().isBF16()) {
                  if (auto truncfInTy = llvm::dyn_cast<mlir::VectorType>(
                          inputOp.getOperand().getType())) {
                    if (truncfInTy.getElementType().isF32()) {
                      builder.setInsertionPoint(inputOp);
                      auto widen =
                          builder.create<mlir::spirv::INTELConvertFToBF16Op>(
                              inputOp.getLoc(),
                              mlir::VectorType::get(truncfInTy.getShape(),
                                                    builder.getI16Type()),
                              inputOp.getOperand());
                      bop->getResult(0).replaceAllUsesWith(widen);
                      eraseOps.push_back(bop);
                      eraseOps.push_back(inputOp);
                    }
                  }
                }
              }
            }
          }
        } else if (bop.getType().isInteger(16)) {
          mlir::arith::TruncFOp inputOp = llvm::dyn_cast<mlir::arith::TruncFOp>(
              bop.getOperand().getDefiningOp());
          if (inputOp) {
            if (inputOp.getType().isBF16() &&
                inputOp.getOperand().getType().isF32()) {
              builder.setInsertionPoint(inputOp);
              auto narrow = builder.create<mlir::spirv::INTELConvertFToBF16Op>(
                  inputOp.getLoc(), builder.getI16Type(), inputOp.getOperand());
              bop->getResult(0).replaceAllUsesWith(narrow);
              eraseOps.push_back(bop);
              eraseOps.push_back(inputOp);
            }
          }
        }
      });
      fop->walk([&](mlir::arith::ExtFOp eop) {
        if (auto vecTy = llvm::dyn_cast<mlir::VectorType>(eop.getType())) {
          if (vecTy.getElementType().isF32()) {
            mlir::arith::BitcastOp inputOp =
                llvm::dyn_cast<mlir::arith::BitcastOp>(
                    eop.getOperand().getDefiningOp());
            if (inputOp) {
              if (auto inTy =
                      llvm::dyn_cast<mlir::VectorType>(inputOp.getType())) {
                if (inTy.getElementType().isBF16()) {
                  if (auto bcastInTy = llvm::dyn_cast<mlir::VectorType>(
                          inputOp.getOperand().getType())) {
                    if (bcastInTy.getElementType().isInteger(16)) {
                      builder.setInsertionPoint(inputOp);
                      auto widen =
                          builder.create<mlir::spirv::INTELConvertBF16ToFOp>(
                              inputOp.getLoc(),
                              mlir::VectorType::get(bcastInTy.getShape(),
                                                    builder.getF32Type()),
                              inputOp.getOperand());
                      eop->getResult(0).replaceAllUsesWith(widen);
                      eraseOps.push_back(eop);
                      eraseOps.push_back(inputOp);
                    }
                  }
                }
              }
            }
          }
        } else if (eop.getType().isF32()) {
          mlir::arith::BitcastOp inputOp =
              llvm::dyn_cast<mlir::arith::BitcastOp>(
                  eop.getOperand().getDefiningOp());
          if (inputOp) {
            if (inputOp.getType().isBF16() &&
                inputOp.getOperand().getType().isInteger(16)) {
              builder.setInsertionPoint(inputOp);
              auto widen = builder.create<mlir::spirv::INTELConvertBF16ToFOp>(
                  inputOp.getLoc(), builder.getF32Type(), inputOp.getOperand());
              eop->getResult(0).replaceAllUsesWith(widen);
              eraseOps.push_back(eop);
              eraseOps.push_back(inputOp);
            }
          }
        }
      });
    });
    target->addDynamicallyLegalOp<mlir::spirv::INTELConvertBF16ToFOp>(
        [](mlir::spirv::INTELConvertBF16ToFOp) { return true; });
    target->addDynamicallyLegalOp<mlir::spirv::INTELConvertFToBF16Op>(
        [](mlir::spirv::INTELConvertFToBF16Op) { return true; });
    for (auto eraseOp : eraseOps) {
      eraseOp->erase();
    }

    // SPIR-V elementwise arith/math ops require special handling if the operate
    // on large vectors. We dynamically legalize these ops based on the vector
    // size they consume.
    // FIXME: this is not an exhaustive list of arith/math ops that need special
    // handling.
    target->addDynamicallyLegalOp<mlir::spirv::CLExpOp>(
        [&](mlir::spirv::CLExpOp op) {
          return isGenericVectorTy(op.getType());
        });
    target->addDynamicallyLegalOp<mlir::spirv::CLFMaxOp>(
        [&](mlir::spirv::CLFMaxOp op) {
          return isGenericVectorTy(op.getType());
        });

    // Upstream SPIRVTypeConverter does not add conversion for
    // UnrankedMemRefType.
    // Conversion logic is the same as ranked dynamic memref type for OpenCL
    // Kernel. unranked memref type is converted to a spirv pointer type with
    // converted spirv scalar element type and spirv storage class.
    // Only scalar element type is currently supported.
    // Also vulkan should be handled differently but out of scope since this
    // conversion pass is for lowering to OpenCL spirv kernel only.
    typeConverter.addConversion(
        [&](mlir::UnrankedMemRefType type) -> std::optional<mlir::Type> {
          auto attr = mlir::dyn_cast_or_null<mlir::spirv::StorageClassAttr>(
              type.getMemorySpace());
          if (!attr)
            return nullptr;
          mlir::spirv::StorageClass storageClass = attr.getValue();

          mlir::Type elementType = type.getElementType();
          auto scalarType =
              mlir::dyn_cast<mlir::spirv::ScalarType>(elementType);
          if (!scalarType)
            return nullptr;
          mlir::Type arrayElemType = typeConverter.convertType(scalarType);
          return mlir::spirv::PointerType::get(arrayElemType, storageClass);
        });

    //------- Upstream Conversion------------
    mlir::populateGPUToSPIRVPatterns(typeConverter, patterns);
    mlir::arith::populateArithToSPIRVPatterns(typeConverter, patterns);
    mlir::populateBuiltinFuncToSPIRVPatterns(typeConverter, patterns);
    mlir::populateVectorToSPIRVPatterns(typeConverter, patterns);
    mlir::populateMathToSPIRVPatterns(typeConverter, patterns);
    mlir::index::populateIndexToSPIRVPatterns(typeConverter, patterns);
    mlir::populateMemRefToSPIRVPatterns(typeConverter, patterns);
    mlir::populateFuncToSPIRVPatterns(typeConverter, patterns);
    // ---------------------------------------

    // IMEX GPUToSPIRV extension
    mlir::ScfToSPIRVContext scfToSpirvCtx;
    mlir::populateSCFToSPIRVPatterns(typeConverter, scfToSpirvCtx, patterns);
    mlir::cf::populateControlFlowToSPIRVPatterns(typeConverter, patterns);
    mlir::populateMathToSPIRVPatterns(typeConverter, patterns);
    imex::populateGPUPrintfToSPIRVPatterns(typeConverter, patterns);
    imex::populateVectorToSPIRVPatterns(typeConverter, patterns);

    if (failed(applyFullConversion(gpuModule, *target, std::move(patterns))))
      return signalPassFailure();
  }
}

std::unique_ptr<::mlir::OperationPass<::mlir::ModuleOp>>
createConvertGPUXToSPIRVPass(bool mapMemorySpace) {
  return std::make_unique<GPUXToSPIRVPass>(mapMemorySpace);
}
} // namespace imex
