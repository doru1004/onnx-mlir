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
  SmallVector<KrnlGetRefOp, 4> seenGetRefs;
  parentBlock->walk([&totalSize, &seenGetRefs, allocOp](KrnlGetRefOp op) {
    // Check that the krnl.getref operation has not already been counted.
    // We must make sure we count the memory footprint of getref operations
    // sharing a slot only once.
    for (auto getRef : seenGetRefs)
      if (op.offset() == getRef.offset())
        return;

    // Footprint has not been counter yet. Add it to totalSize.
    auto result = allocOp->getResult();
    if (op.getOperands()[0] == result)
      totalSize += getMemRefSizeInBytes(op.getResult());

    // Act krnl.getref operation as seen.
    seenGetRefs.emplace_back(op);
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

/// Returns a list of distinct krnl.getref operations in the current
/// block that use the memory pool.
SmallVector<KrnlGetRefOp, 4> getAllDistinctGetRefsForAlloc(AllocOp *allocOp) {
  auto parentBlock = allocOp->getOperation()->getBlock();
  SmallVector<KrnlGetRefOp, 4> getRefs;

  parentBlock->walk([&getRefs, allocOp](KrnlGetRefOp op) {
    // If a getRef with the same memory pool and offset has
    // already been added, skip it.
    for (auto getRef : getRefs)
      if (op.mempool() == getRef.mempool() && op.offset() == op.offset())
        return;

    if (op.getOperands()[0] == allocOp->getResult())
      getRefs.emplace_back(op);
  });

  // The list contains at least one use.
  return getRefs;
}

/// Returns a list of krnl.getref operations in the current block
/// that share the same offset and memory pool.
SmallVector<KrnlGetRefOp, 4> getAllGetRefWithSameOffset(KrnlGetRefOp *getRef) {
  auto parentBlock = getRef->getOperation()->getBlock();
  SmallVector<KrnlGetRefOp, 4> sameOffsetGetRefs;

  parentBlock->walk([&sameOffsetGetRefs, getRef](KrnlGetRefOp op) {
    if (op.mempool() == getRef->mempool() && op.offset() == getRef->offset())
      sameOffsetGetRefs.emplace_back(op);
  });

  // The list contains at least one entry, the input krnl.getref.
  return sameOffsetGetRefs;
}

bool getRefUsesAreDisjoint(
    SmallVector<KrnlGetRefOp, 4> firstGetRefList, KrnlGetRefOp secondGetRef) {
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
    while (operandList.size() > 0 && refsUseIsDisjoint) {
      Value currentElement = operandList[0];
      Operation *definingOperation = currentElement.getDefiningOp();

      // If this value has not been seen before, process it.
      if (definingOperation && dependentOps.count(definingOperation) == 0) {
        // Add value to dependent values list.
        dependentOps.insert(definingOperation);

        if (llvm::dyn_cast<AffineLoadOp>(definingOperation) ||
            llvm::dyn_cast<LoadOp>(definingOperation)) {
          // Check that the MemRef operand of this load operation is
          // not in the firstGetRefList.
          Value loadOperand = definingOperation->getOperands()[0];
          if (!isBlockArgument(secondGetRef, loadOperand)) {
            Operation *loadOperandDefinition = loadOperand.getDefiningOp();
            KrnlGetRefOp loadGetRefOperand =
                llvm::dyn_cast<KrnlGetRefOp>(loadOperandDefinition);

            // If the load operand is valid, compare it with all the entries
            // in the firstGetRefList. If it matches any one of them then the
            // secondGetRef cannot share the same memory pool slot with the
            // rest of the getref operations in the firstGetRefList.
            if (loadGetRefOperand)
              for (auto firstGetRef : firstGetRefList)
                if (firstGetRef == loadGetRefOperand) {
                  refsUseIsDisjoint = false;
                  break;
                }
          }
        } else {
          // Add operands to work queue.
          for (const auto &operand : definingOperation->getOperands())
            if (!isBlockArgument(secondGetRef, operand))
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

bool getRefUsesAreMutuallyDisjoint(SmallVector<KrnlGetRefOp, 4> firstGetRefList,
    SmallVector<KrnlGetRefOp, 4> secondGetRefList) {
  for (auto getRef : secondGetRefList) {
    if (!getRefUsesAreDisjoint(firstGetRefList, getRef)) {
      return false;
    }
  }

  for (auto getRef : firstGetRefList) {
    if (!getRefUsesAreDisjoint(secondGetRefList, getRef)) {
      return false;
    }
  }
  return true;
}

bool isLoad(Operation *op) {
  return llvm::dyn_cast_or_null<LoadOp>(op) ||
         llvm::dyn_cast_or_null<AffineLoadOp>(op);
}

bool isStore(Operation *op) {
  return llvm::dyn_cast_or_null<StoreOp>(op) ||
         llvm::dyn_cast_or_null<AffineStoreOp>(op);
}

bool isLoadStoreForGetRef(KrnlGetRefOp getRef, Operation *op) {
  return (isLoad(op) && getRef.getResult() == op->getOperands()[0]) ||
         (isStore(op) && getRef.getResult() == op->getOperands()[1]);
}

/// Get top block.
Block *getTopBlock(Operation *op) {
  // Get current block as the first top block candidate.
  Block *topBlock = op->getBlock();
  Operation *parentBlockOp = topBlock->getParentOp();

  while (!llvm::dyn_cast_or_null<FuncOp>(parentBlockOp)) {
    topBlock = parentBlockOp->getBlock();
    parentBlockOp = topBlock->getParentOp();
  }

  return topBlock;
}

Operation *getLiveRangeLastOp(KrnlGetRefOp getRef) {
  Block *topBlock = getTopBlock(getRef.getOperation());

  Operation *lastLoadStore = nullptr;
  topBlock->walk([&lastLoadStore, getRef](Operation *op) {
    // If op is a Laod/Store, of any kind then assign it to lastLoadStore.
    if (isLoadStoreForGetRef(getRef, op))
      lastLoadStore = op;
  });

  printf("Last Load/Store: \n");
  lastLoadStore->dump();

  return lastLoadStore;
}

Operation *getLiveRangeFirstOp(KrnlGetRefOp getRef) {
  Block *topBlock = getTopBlock(getRef.getOperation());

  Operation *firstLoadStore = nullptr;
  topBlock->walk([&firstLoadStore, getRef](Operation *op) {
    // If op is a Laod/Store, of any kind then assign it to lastLoadStore.
    if (!firstLoadStore && isLoadStoreForGetRef(getRef, op))
      firstLoadStore = op;
  });

  return firstLoadStore;
}

bool operationInLiveRange(
    Operation *operation, std::vector<Operation *> liveRangeOpList) {
  for (auto &op : liveRangeOpList) {
    if (op == operation)
      return true;
  }
  return false;
}

std::vector<Operation *> getLiveRange(KrnlGetRefOp getRef) {
  std::vector<Operation *> operations;

  auto topBlock = getTopBlock(getRef.getOperation());

  // Determine last load/store from getRef.
  Operation *lastLoadStore = getLiveRangeLastOp(getRef);

  printf("Operations in Live Range: \n");
  bool operationInLiveRange = false;
  topBlock->walk([&operations, &operationInLiveRange, lastLoadStore, getRef](
                     Operation *op) {
    // If op is a Laod/Store, of any kind, then assign it to lastLoadStore.
    if (isLoadStoreForGetRef(getRef, op) && !operationInLiveRange)
      operationInLiveRange = true;

    if (operationInLiveRange) {
      // op->dump();
      operations.emplace_back(op);
    }

    if (op == lastLoadStore)
      operationInLiveRange = false;
  });
  printf(" ===== END OF LIVE RANGE ===== \n");

  return operations;
}

/// This function returns true if `beforeOp` is visited before `op` in a
/// traversal of the provided block.
bool opBeforeOp(Block *block, Operation *beforeOp, Operation *afterOp) {
  bool beforeOpIsBefore = true;
  bool beforeOpFound = false;
  block->walk(
      [&beforeOpIsBefore, &beforeOpFound, beforeOp, afterOp](Operation *op) {
        if (op == beforeOp)
          beforeOpFound = true;
        else if (op == afterOp && !beforeOpFound)
          beforeOpIsBefore = false;
      });
  return beforeOpIsBefore;
}

/// The live range is contained between firstOp and lastOp.
bool liveRangeIsContained(Operation *firstOp, Operation *lastOp,
    std::vector<Operation *> liveRangeOpList) {
  Operation *liveRangeFirstOp = liveRangeOpList[0];
  assert(liveRangeOpList.size() > 0 &&
         "Live range empty but must have at least one element.");
  Operation *liveRangeLastOp = liveRangeOpList[liveRangeOpList.size() - 1];

  Block *topLevelBlock = getTopBlock(firstOp);

  return opBeforeOp(topLevelBlock, firstOp, liveRangeFirstOp) &&
         opBeforeOp(topLevelBlock, liveRangeLastOp, lastOp);
}

bool opInTopLevelBlock(Operation *op) {
  Block *currentBlock = op->getBlock();

  // If the parent operation of the current block is a FuncOp then
  // this operation is in the top-level block.
  return llvm::dyn_cast_or_null<FuncOp>(currentBlock->getParentOp());
}

Operation *getOutermostLoop(Operation *op) {
  Operation *outermostLoop = nullptr;

  // Get current block.
  Block *currentBlock = op->getBlock();

  // Current block must exist.
  assert(currentBlock && "Operation not in a block.");

  // Compute parent operation of the current block. Every block has
  // a parent operation.
  Operation *parentBlockOp = currentBlock->getParentOp();

  // This loop will handle the following case:
  //
  // func() {
  //   if {
  //     krnl.iterate {  <--- Outermost loop.
  //       krnl.iterate {
  //         if {
  //           ... op ...
  //         }
  //       }
  //     }
  //   }
  // }
  //
  while (!llvm::dyn_cast_or_null<FuncOp>(parentBlockOp)) {
    if (llvm::dyn_cast_or_null<KrnlIterateOp>(parentBlockOp))
      outermostLoop = parentBlockOp;
    parentBlockOp = parentBlockOp->getBlock()->getParentOp();
  }

  return outermostLoop;
}

bool checkOuterLoopsMatch(Operation *op1, Operation *op2) {
  // Check if the outer loops of the two operations match.
  // If one of the operations is not part of a loop (i.e. the returned
  // operation of the getOutermostLoop is nullptr) then return false.
  Operation *outerLoop1 = getOutermostLoop(op1);

  if (!outerLoop1)
    return false;

  Operation *outerLoop2 = getOutermostLoop(op2);

  if (!outerLoop2)
    return false;

  // If both outer loops are valid, check if they match.
  printf("Outer Loops match = %d\n", outerLoop1 == outerLoop2);
  return outerLoop1 == outerLoop2;
}

bool liveRangesInSameLoopNest(Operation *firstOp, Operation *lastOp,
    std::vector<Operation *> liveRangeOpList) {
  // If any of the firstOp or lastOp are in the top level block of the
  // function, then they cannot share a loop nest with the last or first
  // operation in the live range respectively.
  bool firstOpInTopLevelBlock = opInTopLevelBlock(firstOp);
  bool lastOpInTopLevelBlock = opInTopLevelBlock(firstOp);

  printf("firstOpInTopLevelBlock = %d\n", firstOpInTopLevelBlock);
  printf("lastOpInTopLevelBlock = %d\n", lastOpInTopLevelBlock);

  // If both firstOp and lastOp are in the top level block then they cannot
  // share a loop nest with the live range.
  if (firstOpInTopLevelBlock && lastOpInTopLevelBlock) {
    printf("CASE 1: firstOp and lastOp are both in top-level block\n");
    return false;
  }

  // Repeat checks for first/last operation in live range.
  Operation *liveRangeFirstOp = liveRangeOpList[0];
  assert(liveRangeOpList.size() > 0 &&
         "Live range empty but must have at least one element.");
  Operation *liveRangeLastOp = liveRangeOpList[liveRangeOpList.size() - 1];

  bool firstLROpInTopLevelBlock = opInTopLevelBlock(liveRangeFirstOp);
  bool lastLROpInTopLevelBlock = opInTopLevelBlock(liveRangeLastOp);

  printf("firstLROpInTopLevelBlock = %d\n", firstLROpInTopLevelBlock);
  printf("lastLROpInTopLevelBlock = %d\n", lastLROpInTopLevelBlock);

  // If both live range extremities are in the top level block then they cannot
  // share a loop nest with the other live range.
  if (firstLROpInTopLevelBlock && lastLROpInTopLevelBlock) {
    printf("CASE 2: LR first and last op are both in top-level block\n");
    return false;
  }

  // If neither of the lastOp or liveRangeFirstOp are in the top block then
  // check if the outermost loops that contain them are the same. If they are
  // the same then they share the same loop nest, return true.
  if (!lastOpInTopLevelBlock && !firstLROpInTopLevelBlock &&
      checkOuterLoopsMatch(lastOp, liveRangeFirstOp)) {
    printf("CASE 3: top extremities in same loop nest!\n");
    return true;
  }

  // Now check the other pair of extremities. If they are in the same loop nest
  // return true.
  if (!firstOpInTopLevelBlock && !lastLROpInTopLevelBlock &&
      checkOuterLoopsMatch(firstOp, liveRangeLastOp)) {
    printf("CASE 4: bottom extremities in same loop nest!\n");
    return true;
  }

  // If none of the cases above were met then:
  // 1. at least one of the extremities in each pair is at top-block level.
  // or
  // 2. extremities are in sub-blocks but they do not share a loop nest.
  // In either case the intersection check must return false.
  printf("CASE 5: otherwise\n");
  return false;
}

bool checkLiveRangesIntersect(SmallVector<KrnlGetRefOp, 4> firstGetRefList,
    SmallVector<KrnlGetRefOp, 4> secondGetRefList) {
  // Check that the live range of each individual element in secondGetRefList
  // is independent from the individual live ranges of the elements
  // of the firstGetRefList.
  for (auto firstGetRef : firstGetRefList) {
    // Fetch the full live range for the first set of getref operations.
    std::vector<Operation *> liveRangeOpList = getLiveRange(firstGetRef);

    for (auto secondGetRef : secondGetRefList) {
      printf(" == Comparing live ranges of:\n");
      firstGetRef.dump();
      secondGetRef.dump();

      // Get first and last ops for the live range of the second set of
      // getref operations.
      Operation *firstOp = getLiveRangeFirstOp(secondGetRef);
      Operation *lastOp = getLiveRangeLastOp(secondGetRef);

      // Check if either the first or last ops in the second live range are part
      // of the first live range.
      bool firstOpInLiveRange = operationInLiveRange(firstOp, liveRangeOpList);
      bool lastOpInLiveRange = operationInLiveRange(lastOp, liveRangeOpList);

      printf("firstOpInLiveRange = %d\n", firstOpInLiveRange);
      printf("lastOpInLiveRange = %d\n", lastOpInLiveRange);
      if (firstOpInLiveRange || lastOpInLiveRange)
        return true;

      // Since firstOp and lastOp are not part of the live range, check whether
      // the live range is fully contained between firstOp and lastOp. If it is
      // return true.
      if (liveRangeIsContained(firstOp, lastOp, liveRangeOpList))
        return true;

      // Up to this point, the checks we have done allow for ranges to be
      // considered disjoint even when their extremities are part of the same
      // loop nest. This means we have to perform an additional check: if the
      // extremities of the two live ranges share the same loop nest determiend
      // by `krnl.iterate` ops. If they do then the live ranges intersect.
      if (liveRangesInSameLoopNest(firstOp, lastOp, liveRangeOpList))
        return true;
    }
  }

  // If all getRef live ranges are independent then no intersection exists.
  return false;
}

//===----------------------------------------------------------------------===//
// Rewrite patterns.
//===----------------------------------------------------------------------===//

class KrnlOptimizeStaticMemoryPools : public OpRewritePattern<KrnlGetRefOp> {
public:
  using OpRewritePattern<KrnlGetRefOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(
      KrnlGetRefOp firstGetRef, PatternRewriter &rewriter) const override {
    auto loc = firstGetRef.getLoc();
    auto memRefType = convertToMemRefType(firstGetRef.getResult().getType());
    auto memRefShape = memRefType.getShape();

    // Only handle krnl.getref ops that return a constant shaped MemRef.
    if (!hasAllConstantDimensions(memRefType))
      return failure();

    // Retrieve the AllocOp that this GetRef uses.
    auto staticMemPool = getAllocOfGetRef(&firstGetRef);

    // Ensure that the alloc obtained above is static memory pool.
    auto memPoolType = convertToMemRefType(staticMemPool.getResult().getType());
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

    // If this is not the top block fail.
    if (!llvm::dyn_cast_or_null<FuncOp>(parentBlock->getParentOp()))
      return failure();

    // Get a GetRef, other than the current one, that uses the same static
    // memory pool.
    SmallVector<KrnlGetRefOp, 4> getRefCandidates;
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
        getRefCandidates.emplace_back(candidate);
      }
    }

    // If no candidate was found, pattern matching failed.
    if (getRefCandidates.size() < 1)
      return failure();

    SmallVector<KrnlGetRefOp, 4> validSlotReusers;
    for (auto secondGetRef : getRefCandidates) {
      // Check that the current candidate has not already been added as a valid
      // slot reuser.
      bool isSlotReuser = false;
      for (auto slotReuser : validSlotReusers) {
        if (slotReuser == secondGetRef) {
          isSlotReuser = true;
          break;
        }
      }
      if (isSlotReuser)
        continue;

      // If the second getref has the same offset as the first then the rewrite
      // rule has already been applied to this getref so there is no work to do.
      if (firstGetRef.offset() == secondGetRef.offset())
        continue;

      // Both first and second getRef ops may have already been processed by
      // this rewrite rule. There could be several krnl.getref with the same
      // offset as firstGetRef and several krnl.getRef with the same offset as
      // secondGetRef. In general we have to be able to handle this case.
      SmallVector<KrnlGetRefOp, 4> firstGetRefList =
          getAllGetRefWithSameOffset(&firstGetRef);
      SmallVector<KrnlGetRefOp, 4> secondGetRefList =
          getAllGetRefWithSameOffset(&secondGetRef);

      // Add all the currently discovered krnl.getref reusers that have not yet
      // been actually processed but are now known to be valid reusers of the
      // same slot. This is done for the purpose of checking validity of the
      // other remaining candidates which have to consider that there is now
      // an additional getref that uses the same slot.
      for (auto validUnemittedReuser : validSlotReusers)
        firstGetRefList.emplace_back(validUnemittedReuser);

      // Check that the usage of the candidate getrefs is disjoint from the
      // usage of any of the first getrefs. This means that for any store to a
      // getref in secondGetRefList, the value stored does not involve a load
      // from a getref in firstGetRefList (and vice-versa).
      bool refsUseIsDisjoint =
          getRefUsesAreMutuallyDisjoint(firstGetRefList, secondGetRefList);

      if (!refsUseIsDisjoint)
        continue;

      printf("Found a possible match:\n");
      firstGetRef.dump();
      secondGetRef.dump();

      // Check live ranges don't intersect.
      // Live range, chain of instructions between the first and last
      // load/store from/to any krnl.getref in a given list.
      bool liveRangesIntersect =
          checkLiveRangesIntersect(firstGetRefList, secondGetRefList);

      printf("Live ranges intersect =====> %d\n", liveRangesIntersect);
      if (liveRangesIntersect)
        continue;

      for (auto secondGetRef : secondGetRefList)
        validSlotReusers.emplace_back(secondGetRef);
      printf("=======> CANDIDATES CAN SHARE THE SLOT!! <=======\n");
    }

    // No valid slot reuse getRefs have been identified.
    if (validSlotReusers.size() == 0)
      return failure();

    // A suitable slot can be reused. Convert all secondGetRefList entries to
    // use the same slot in the memory pool as all the firstGetRefList entries.
    for (auto secondGetRef : validSlotReusers) {
      auto newGetRefOp =
          rewriter.create<KrnlGetRefOp>(loc, secondGetRef.getResult().getType(),
              staticMemPool, firstGetRef.offset());
      newGetRefOp.getOperation()->moveBefore(secondGetRef);
      rewriter.replaceOp(secondGetRef, newGetRefOp.getResult());
    }

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

    // Get parent block.
    Block *parentBlock = allocOp.getOperation()->getBlock();

    // If this is not the top block, fail.
    if (!llvm::dyn_cast_or_null<FuncOp>(parentBlock->getParentOp()))
      return failure();

    // Compute size of all krnl.getref operations that use this memory pool.
    int64_t usedMemory = getAllocGetRefTotalSize(&allocOp);

    assert(usedMemory <= memPoolShape[0] &&
           "Used memory exceeds allocated memory.");

    // Check if changes to the memory pool are required.
    if (memPoolShape[0] == usedMemory)
      return failure();

    // Compute the shape of the new static memory pool.
    SmallVector<int64_t, 1> newStaticMemPoolShape;
    newStaticMemPoolShape.emplace_back(usedMemory);
    auto newStaticMemPoolType =
        MemRefType::get(newStaticMemPoolShape, rewriter.getIntegerType(8));

    // We need to emit a new alloc of smaller size.
    AllocOp newStaticMemPool =
        rewriter.create<AllocOp>(loc, newStaticMemPoolType);
    newStaticMemPool.getOperation()->moveBefore(allocOp);

    // Changes are required, memory pool needs to be compacted.
    SmallVector<KrnlGetRefOp, 4> distinctGetRefs =
        getAllDistinctGetRefsForAlloc(&allocOp);

    // Each krnl.getref using the alloc needs to be re-emitted with the new
    // static memory pool and the new offset.
    int64_t currentOffset = 0;
    std::map<KrnlGetRefOp, KrnlGetRefOp> oldToNewGetRef;
    for (auto getRefOp : distinctGetRefs) {
      // Emit the current offset inside the static memory pool.
      auto newOffset = rewriter.create<ConstantOp>(loc,
          rewriter.getIntegerAttr(rewriter.getIntegerType(64), currentOffset));

      // Size of current getref.
      int64_t currentGetRefSize = getMemRefSizeInBytes(getRefOp.getResult());

      // Get all getRefs which share the same memory slot.
      SmallVector<KrnlGetRefOp, 4> sameSlotGetRefs =
          getAllGetRefWithSameOffset(&getRefOp);

      // Replace each one with a getref using the new offset in the compacted
      // memory pool.
      for (auto oldGetRef : sameSlotGetRefs) {
        // Create a new krnl.getref using the new memory pool and new offset.
        auto newGetRefOp = rewriter.create<KrnlGetRefOp>(
            loc, oldGetRef.getResult().getType(), newStaticMemPool, newOffset);
        newGetRefOp.getOperation()->moveBefore(oldGetRef);

        oldToNewGetRef.insert(
            std::pair<KrnlGetRefOp, KrnlGetRefOp>(oldGetRef, newGetRefOp));
      }

      // Update offset.
      currentOffset += currentGetRefSize;
    }

    for (auto getRefPair : oldToNewGetRef)
      rewriter.replaceOp(getRefPair.first, getRefPair.second.getResult());

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
    patterns.insert<KrnlOptimizeStaticMemoryPools>(&getContext());
    patterns.insert<KrnlCompactStaticMemoryPools>(&getContext());

    applyPatternsAndFoldGreedily(function, patterns);
  }
};
} // namespace

std::unique_ptr<Pass> mlir::createKrnlOptimizeMemoryPoolsPass() {
  return std::make_unique<KrnlOptimizeMemoryPoolsPass>();
}
