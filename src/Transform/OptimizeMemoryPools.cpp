//===-- OptimizeMemoryPools.cpp - Optimize Memory Pools -------------------===//
//
// Copyright 2019-2020 The IBM Research Authors.
//
// =============================================================================
//
// For certain cases the number of individual memory allocations required for
// all internal tensors is large and needs to be mitigated. This pass optimizes
// the internal MemRef static and dynamic memory pools emitted by the
// BundleMemoryPool pass.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/SetVector.h"

#include "src/Conversion/ONNXToKrnl/ONNXToKrnlCommon.hpp"
#include "src/Dialect/Krnl/KrnlOps.hpp"
#include "src/Pass/Passes.hpp"

using namespace mlir;
namespace {

/// Get the AllocOp of the current GetRef.
AllocOp getAllocOfGetRef(KrnlGetRefOp *getRef) {
  auto parentBlock = getRef->getOperation()->getBlock();

  AllocOp alloc = nullptr;
  parentBlock->walk([&alloc, getRef](AllocOp op) {
    auto getRefAlloc = getRef->getOperands()[0];
    if (op.getResult() == getRefAlloc)
      alloc = op;
  });

  return alloc;
}

/// Get the number of GetRef ops associated with this AllocOp.
int64_t getAllocGetRefNum(AllocOp *allocOp) {
  auto parentBlock = allocOp->getOperation()->getBlock();

  int64_t numGetRefs = 0;
  parentBlock->walk([&numGetRefs, allocOp](KrnlGetRefOp op) {
    auto result = allocOp->getResult();
    if (op.getOperands()[0] == result)
      numGetRefs++;
  });

  return numGetRefs;
}

/// Get the total size in bytes used by the getref operations associated
/// with a given memory pool.
int64_t getAllocGetRefTotalSize(AllocOp *allocOp) {
  auto parentBlock = allocOp->getOperation()->getBlock();

  int64_t totalSize = 0;
  parentBlock->walk([&totalSize, allocOp](KrnlGetRefOp op) {
    auto result = allocOp->getResult();
    if (op.getOperands()[0] == result)
      totalSize += getMemRefSizeInBytes(op.getResult());
  });

  return totalSize;
}

// Check if this value is an argument of one of the blocks nested
// around it.
bool isBlockArgument(KrnlGetRefOp firstGetRef, Value operand) {
  // Parent operation of the current block.
  Operation *parentBlockOp;
  Block *currentBlock = firstGetRef.getOperation()->getBlock();

  do {
    // Check the arguments of the current block.
    for (auto arg : currentBlock->getArguments())
      if (operand == arg)
        return true;

    parentBlockOp = currentBlock->getParentOp();
    currentBlock = parentBlockOp->getBlock();

  } while (!llvm::dyn_cast_or_null<FuncOp>(parentBlockOp));

  return false;
}

/// Returns a list of operations in the current block that use the getref.
std::vector<Operation *> getGetRefStores(KrnlGetRefOp *getRef) {
  auto parentBlock = getRef->getOperation()->getBlock();
  std::vector<Operation *> stores;

  parentBlock->walk([&stores, getRef](StoreOp op) {
    for (const auto &operand : op.getOperands())
      if (operand == getRef->getResult())
        stores.emplace_back(op);
  });

  parentBlock->walk([&stores, getRef](AffineStoreOp op) {
    for (const auto &operand : op.getOperands())
      if (operand == getRef->getResult())
        stores.emplace_back(op);
  });

  // The list contains at least one use.
  return stores;
}

/// Returns a list of krnl.getref operations in the current block
/// that use the memory pool.
std::vector<KrnlGetRefOp> getAllGetRefsForAlloc(AllocOp *allocOp) {
  auto parentBlock = allocOp->getOperation()->getBlock();
  std::vector<KrnlGetRefOp> getRefs;

  parentBlock->walk([&getRefs, allocOp](KrnlGetRefOp op) {
    if (op.getOperands()[0] == allocOp->getResult())
      getRefs.emplace_back(op);
  });

  // The list contains at least one use.
  return getRefs;
}

bool getRefUsesAreDisjoint(
    KrnlGetRefOp firstGetRef, KrnlGetRefOp secondGetRef) {
  // Return variable.
  bool refsUseIsDisjoint = true;

  // Compute all the stores into the second getref.
  std::vector<Operation *> allStores = getGetRefStores(&secondGetRef);

  // For each store, analyze the list of dependendent operations that
  // contributes to the computation of the value being stored. The leaf
  // values are going to be represented by: load operations and constants.
  for (const auto &store : allStores) {
    // Initialize work queue data structure.
    std::vector<Value> operandList;
    operandList.emplace_back(store->getOperands()[0]);

    // Construct the list of Values on which the current AllocOp depends on.
    llvm::SetVector<Operation *> dependentOps;
    while (operandList.size() > 0) {
      Value currentElement = operandList[0];
      Operation *definingOperation = currentElement.getDefiningOp();

      // If this value has not been seen before, process it.
      if (dependentOps.count(definingOperation) == 0) {
        // Add value to dependent values list.
        dependentOps.insert(definingOperation);

        if (llvm::dyn_cast<AffineLoadOp>(definingOperation) ||
            llvm::dyn_cast<LoadOp>(definingOperation)) {
          // Check that the MemRef operand of this store operation is
          // not the firstGetRef.
          Value memRefOperand = definingOperation->getOperands()[0];
          if (!isBlockArgument(firstGetRef, memRefOperand)) {
            Operation *operandDefinition = memRefOperand.getDefiningOp();
            KrnlGetRefOp storeGetRefOperand =
                llvm::dyn_cast<KrnlGetRefOp>(operandDefinition);

            if (storeGetRefOperand && firstGetRef == storeGetRefOperand) {
              refsUseIsDisjoint = false;
            }
          }
        } else {
          // Add operands to work queue.
          for (const auto &operand : definingOperation->getOperands())
            if (!isBlockArgument(firstGetRef, operand))
              operandList.emplace_back(operand);
        }
      }

      // Erase first element from work queue.
      operandList.erase(operandList.begin());
    }

    // Exit if use is not disjoint.
    if (!refsUseIsDisjoint)
      break;
  }

  return refsUseIsDisjoint;
}

bool getRefUsesAreMutuallyDisjoint(
    KrnlGetRefOp firstGetRef, KrnlGetRefOp secondGetRef) {
  return getRefUsesAreDisjoint(firstGetRef, secondGetRef) &&
    getRefUsesAreDisjoint(secondGetRef, firstGetRef);
}

//===----------------------------------------------------------------------===//
// Rewrite patterns.
//===----------------------------------------------------------------------===//

class KrnlOptimizeStaticMemoryPools : public OpRewritePattern<KrnlGetRefOp> {
public:
  using OpRewritePattern<KrnlGetRefOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(
      KrnlGetRefOp firstGetRef, PatternRewriter &rewriter) const override {
    auto memRefType = convertToMemRefType(firstGetRef.getResult().getType());
    auto memRefShape = memRefType.getShape();

    // Only handle krnl.getref ops that return a constant shaped MemRef.
    if (!hasAllConstantDimensions(memRefType))
      return failure();

    // Retrieve the AllocOp that this GetRef uses.
    auto staticMemPool = getAllocOfGetRef(&firstGetRef);

    // Ensure that the alloc obtained above is static memory pool.
    auto memPoolType =
        convertToMemRefType(staticMemPool.getResult().getType());
    auto memPoolShape = memPoolType.getShape();

    // Static memory pool type must be byte.
    if (getMemRefEltSizeInBytes(memPoolType) != 1)
      return failure();

    // Rank of the static memory pool must be 1.
    if (memPoolShape.size() != 1)
      return failure();

    // Determine if the static memory pool is bundled i.e. participates in more
    // than one getRef.
    if (getAllocGetRefNum(&staticMemPool) < 2)
      return failure();

    // Get parent block.
    Block *parentBlock = firstGetRef.getOperation()->getBlock();

    // Get a GetRef, other than the current one, that uses the same static
    // memory pool.
    KrnlGetRefOp secondGetRef = nullptr;
    for (auto &op :
        llvm::make_range(parentBlock->begin(), std::prev(parentBlock->end()))) {
      KrnlGetRefOp candidate = llvm::dyn_cast_or_null<KrnlGetRefOp>(&op);

      // The second krnl.getref properties:
      // - must be valid;
      // - cannot be the same krnl.getref as the first;
      // - must use the same static memory pool as the first krnl.getref;
      // - the result must have the same memory footprint as the first.
      if (candidate && candidate != firstGetRef &&
          getAllocOfGetRef(&candidate) == staticMemPool &&
          getMemRefSizeInBytes(firstGetRef.getResult()) ==
          getMemRefSizeInBytes(candidate.getResult())) {
        secondGetRef = candidate;
      }
    }

    // If no secondGetRef was found, pattern matching failed.
    if (!secondGetRef)
      return failure();

    // A suitable candidate has been found. The next step is to check that
    // the usage of the candidate getref is disjoint from the usage of the
    // first getref. This means that for any store to the secondGetRef, the
    // value stored does not involve a load from the firstGetRef.
    bool refsUseIsDisjoint =
        getRefUsesAreMutuallyDisjoint(firstGetRef, secondGetRef);

    if (!refsUseIsDisjoint)
      return failure();

    // A suitable replacement has been found, perform replacement, replace
    // second getref with first getref.
    rewriter.replaceOp(secondGetRef, firstGetRef.getResult());

    return success();
  }
};

class KrnlCompactStaticMemoryPools : public OpRewritePattern<AllocOp> {
public:
  using OpRewritePattern<AllocOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(
      AllocOp allocOp, PatternRewriter &rewriter) const override {
    auto loc = allocOp.getLoc();

    auto memPoolType = convertToMemRefType(allocOp.getResult().getType());
    auto memPoolShape = memPoolType.getShape();

    // Only handle alloc ops that return a constant shaped MemRef.
    if (!hasAllConstantDimensions(memPoolType))
      return failure();

    // Static memory pool type must be byte.
    if (getMemRefEltSizeInBytes(memPoolType) != 1)
      return failure();

    // Rank of the static memory pool must be 1.
    if (memPoolShape.size() != 1)
      return failure();

    // This is a memory pool if it is used by at least one getref.
    if (getAllocGetRefNum(&allocOp) < 1)
      return failure();

    // Compute size of all krnl.getref operations that use this memory pool.
    int64_t usedMemory = getAllocGetRefTotalSize(&allocOp);

    assert(usedMemory <= memPoolShape[0] &&
        "Used memory exceeds allocated memory.");

    // Check if changes to the memory pool are required.
    if (memPoolShape[0] == usedMemory)
      return failure();

    // Changes are required, memory pool needs to be compacted.
    std::vector<KrnlGetRefOp> allGetRefs = getAllGetRefsForAlloc(&allocOp);

    // Compute the shape of the new static memory pool.
    SmallVector<int64_t, 1> newStaticMemPoolShape;
    newStaticMemPoolShape.emplace_back(usedMemory);
    auto newStaticMemPoolType =
        MemRefType::get(newStaticMemPoolShape, rewriter.getIntegerType(8));

    // We need to emit a new alloc of smaller size.
    AllocOp newStaticMemPool = rewriter.create<AllocOp>(
        loc, newStaticMemPoolType);
    newStaticMemPool.getOperation()->moveBefore(allocOp);

    // Each krnl.getref using the alloc needs to be re-emitted with the new
    // static memory pool and the new offset.
    int64_t currentOffset = 0;
    for (auto getRefOp : allGetRefs) {
      // Emit the current offset inside the static memory pool.
      auto newOffset = rewriter.create<ConstantOp>(
          loc, rewriter.getIntegerAttr(
                 rewriter.getIntegerType(64), currentOffset));

      // Create a new krnl.getref using the new memory pool and new offset.
      auto newGetRefOp = rewriter.create<KrnlGetRefOp>(loc,
          getRefOp.getResult().getType(), newStaticMemPool,
          newOffset);
      newGetRefOp.getOperation()->moveBefore(getRefOp);

      // Update offset.
      currentOffset += getMemRefSizeInBytes(getRefOp.getResult());

      // Replace old krnl.getref with the new one.
      rewriter.replaceOp(getRefOp, newGetRefOp.getResult());
    }

    rewriter.replaceOp(allocOp, newStaticMemPool.getResult());

    return success();
  }
};

/*!
 *  Function pass that optimizes memory pools.
 */
class KrnlOptimizeMemoryPoolsPass
    : public PassWrapper<KrnlOptimizeMemoryPoolsPass, FunctionPass> {
public:
  void runOnFunction() override {
    auto function = getFunction();

    ConversionTarget target(getContext());
    OwningRewritePatternList patterns;
    patterns.insert<KrnlOptimizeStaticMemoryPools, KrnlCompactStaticMemoryPools>(
        &getContext());

    applyPatternsAndFoldGreedily(function, patterns);
  }
};
} // namespace

std::unique_ptr<Pass> mlir::createKrnlOptimizeMemoryPoolsPass() {
  return std::make_unique<KrnlOptimizeMemoryPoolsPass>();
}
