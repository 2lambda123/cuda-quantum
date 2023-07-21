/*******************************************************************************
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include "LoopAnalysis.h"
#include "PassDetails.h"
#include "cudaq/Optimizer/Transforms/Passes.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/Passes.h"

namespace cudaq::opt {
#define GEN_PASS_DEF_LOOPUNROLL
#include "cudaq/Optimizer/Transforms/Passes.h.inc"
} // namespace cudaq::opt

#define DEBUG_TYPE "cc-loop-unroll"

using namespace mlir;

inline std::pair<Block *, Block *> findCloneRange(Block *first, Block *last) {
  return {first->getNextNode(), last->getPrevNode()};
}

static std::size_t
unrollLoopByValue(cudaq::cc::LoopOp loop,
                  const cudaq::opt::LoopComponents &components) {
  auto c = components.compareValue.getDefiningOp<arith::ConstantOp>();
  return cast<IntegerAttr>(c.getValue()).getInt();
}

static std::size_t unrollLoopByValue(cudaq::cc::LoopOp loop) {
  auto components = cudaq::opt::getLoopComponents(loop);
  return unrollLoopByValue(loop, *components);
}

static bool exceedsThresholdValue(cudaq::cc::LoopOp loop,
                                  std::size_t threshold) {
  auto upperBound = unrollLoopByValue(loop);
  return upperBound >= threshold;
}

namespace {

/// We fully unroll a counted loop (so marked with the counted attribute) as
/// long as the number of iterations is constant and that constant is less than
/// the threshold value.
///
/// Assumptions are made that the counted loop has a particular structural
/// layout as is consistent with the factory producing the counted loop.
///
/// After this pass, all loops marked counted will be unrolled or marked
/// invariant. An invariant loop means the loop must execute exactly some
/// specific number of times, even if that number is only known at runtime.
struct UnrollCountedLoop : public OpRewritePattern<cudaq::cc::LoopOp> {
  explicit UnrollCountedLoop(MLIRContext *ctx, std::size_t t, bool sf,
                             unsigned &p)
      : OpRewritePattern(ctx), threshold(t), signalFailure(sf), progress(p) {}

  LogicalResult matchAndRewrite(cudaq::cc::LoopOp loop,
                                PatternRewriter &rewriter) const override {
    // When the signalFailure flag is set, all loops are matched since that flag
    // requires that all LoopOp operations be rewritten. Despite the setting of
    // this flag, it may not be possible to fully unroll every LoopOp anyway.
    // Check for cases that are clearly not going to be unrolled.
    if (!cudaq::opt::isaCountedLoop(loop)) {
      if (signalFailure)
        loop.emitOpError("not a simple counted loop");
      return failure();
    }
    if (exceedsThresholdValue(loop, threshold)) {
      if (signalFailure)
        loop.emitOpError("loop bounds exceed iteration threshold");
      return failure();
    }

    // At this point, we're ready to unroll the loop and replace it with a
    // sequence of blocks. Each block will receive a block argument that is the
    // iteration number. The original cc.loop will be replaced by a constant,
    // the total number of iterations.
    // TODO: Allow the threading of other block arguments to the result.
    auto components = cudaq::opt::getLoopComponents(loop);
    assert(components && "counted loop must have components");
    auto unrollBy = unrollLoopByValue(loop, *components);
    if (components->isClosedIntervalForm())
      ++unrollBy;
    Type inductionTy = loop.getOperands()[components->induction].getType();
    LLVM_DEBUG(llvm::dbgs()
               << "unrolling loop by " << unrollBy << " iterations\n");
    auto loc = loop.getLoc();
    // Split the basic block in which this cc.loop appears.
    auto *insBlock = rewriter.getInsertionBlock();
    auto insPos = rewriter.getInsertionPoint();
    auto *endBlock = rewriter.splitBlock(insBlock, insPos);
    rewriter.setInsertionPointToEnd(insBlock);
    Value iterCount = getIntegerConstant(loc, inductionTy, 0, rewriter);
    SmallVector<Location> locsRange(loop.getNumResults(), loc);
    auto &bodyRegion = loop.getBodyRegion();
    SmallVector<Value> iterationOpers = loop.getOperands();
    // Make a constant number of copies of the body.
    for (std::size_t i = 0u; i < unrollBy; ++i) {
      rewriter.cloneRegionBefore(bodyRegion, endBlock);
      auto [cloneFront, cloneBack] = findCloneRange(insBlock, endBlock);
      auto termOpers = cloneBack->getTerminator()->getOperands();
      rewriter.eraseOp(cloneBack->getTerminator());
      rewriter.setInsertionPointToEnd(cloneBack);
      // Append the next iteration number.
      Value nextIterCount =
          getIntegerConstant(loc, inductionTy, i + 1, rewriter);
      rewriter.setInsertionPointToEnd(insBlock);
      // Propagate the previous iteration number into the new block.
      // FIXME: need to thread all exit blocks. Also the step and while blocks
      // may have side-effects that should be considered here.
      iterationOpers[components->induction] = iterCount;
      rewriter.create<cf::BranchOp>(loc, cloneFront, iterationOpers);
      iterationOpers = termOpers;
      iterCount = nextIterCount;
      insBlock = cloneBack;
    }
    rewriter.setInsertionPointToEnd(insBlock);
    auto total = getIntegerConstant(loc, inductionTy, unrollBy, rewriter);
    iterationOpers[components->induction] = total;
    rewriter.replaceOp(loop, iterationOpers);
    [[maybe_unused]] auto lastBranch =
        rewriter.create<cf::BranchOp>(loc, endBlock);

    LLVM_DEBUG(llvm::dbgs() << "after unrolling a loop:\n";
               lastBranch->getParentOfType<func::FuncOp>().dump());
    progress++;
    return success();
  }

  static Value getIntegerConstant(Location loc, Type ty, std::int64_t val,
                                  PatternRewriter &rewriter) {
    auto attr = rewriter.getIntegerAttr(ty, val);
    return rewriter.create<arith::ConstantOp>(loc, ty, attr);
  }

  std::size_t threshold;
  bool signalFailure;
  unsigned &progress;
};

/// The loop unrolling pass will fully unroll a `cc::LoopOp` when the loop is
/// known to always execute a constant number of iterations. That is, the loop
/// is a counted loop. (A threshold value can be used to bound the legal range
/// of iterations. The default is 50.)
class LoopUnrollPass : public cudaq::opt::impl::LoopUnrollBase<LoopUnrollPass> {
public:
  using LoopUnrollBase::LoopUnrollBase;

  void runOnOperation() override {
    auto *ctx = &getContext();
    auto *op = getOperation();
    auto numLoops = countLoopOps(op);
    unsigned progress = 0;
    if (numLoops) {
      PassManager pm(ctx);
      pm.addPass(createCanonicalizerPass());
      RewritePatternSet patterns(ctx);
      patterns.insert<UnrollCountedLoop>(ctx, threshold,
                                         /*signalFailure=*/false, progress);
      FrozenRewritePatternSet frozen(std::move(patterns));
      // Iterate over the loops until a fixed-point is reached. Some loops can
      // only be unrolled if other loops are unrolled first and the constants
      // iteratively propagated.
      do {
        progress = 0;
        (void)applyPatternsAndFoldGreedily(op, frozen);
        if (failed(pm.run(op)))
          break;
      } while (loopsWereUnrolled(op, numLoops, progress));
    }
    if (numLoops && signalFailure) {
      RewritePatternSet patterns(ctx);
      patterns.insert<UnrollCountedLoop>(ctx, threshold, signalFailure,
                                         progress);
      (void)applyPatternsAndFoldGreedily(op, std::move(patterns));
      emitError(UnknownLoc::get(ctx), "did not unroll loops");
      signalPassFailure();
    }
  }

  static bool loopsWereUnrolled(Operation *op, unsigned &numLoops,
                                unsigned progress) {
    auto oldNumLoops = numLoops;
    numLoops = numLoops >= progress ? numLoops - progress : 0;
    return oldNumLoops > numLoops;
  }

  static unsigned countLoopOps(Operation *op) {
    unsigned result = 0;
    op->walk([&](cudaq::cc::LoopOp loop) { result++; });
    LLVM_DEBUG(llvm::dbgs() << "Total number of loops: " << result << '\n');
    return result;
  }
};
} // namespace

void cudaq::opt::createUnrollingPipeline(OpPassManager &pm, unsigned threshold,
                                         bool signalFailure) {
  pm.addPass(createCanonicalizerPass());
  pm.addNestedPass<func::FuncOp>(createClassicalMemToReg());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createLoopNormalize());
  pm.addPass(createCanonicalizerPass());
  LoopUnrollOptions luo{threshold, signalFailure};
  pm.addPass(createLoopUnroll(luo));
}

void cudaq::opt::registerUnrollingPipeline() {
  PassPipelineRegistration<>(
      "unrolling-pipeline",
      "Fully unroll loops that can be completely unrolled.",
      addUnrollingPipeline);
}
