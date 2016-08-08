#include "CompiledResolver.h"

#include <string>

#include <llvm/IR/Attributes.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include "SimpleObjectCache.h"
#include "SimpleOrcJit.h"
#include "Utils.h"

void llvm::ObjectCache::anchor() {}

CompiledResolver::CompiledResolver(const DecisionTree_t &tree,
                                   int dataSetFeatures, int functionDepth,
                                   int switchDepth)
    : Builder(Ctx), DecisionTree(tree) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  assert(isPowerOf2(DecisionTree.size() + 1));
  uint8_t treeDepth = Log2(DecisionTree.size() + 1);

  assert(dataSetFeatures > 0);
  assert(functionDepth > 0 && functionDepth <= treeDepth);
  assert(switchDepth > 0 && switchDepth <= MaxSwitchLevels);

  // current restrictions:
  assert(treeDepth % functionDepth == 0);
  assert(functionDepth % switchDepth == 0);

  std::string cachedTreeFile = makeTreeFileName(treeDepth, dataSetFeatures);
  std::string cachedObjFile =
      makeObjFileName(treeDepth, dataSetFeatures, functionDepth, switchDepth);

  bool inCache = isFileInCache(cachedTreeFile) && isFileInCache(cachedObjFile);

  TheModule = makeModule(cachedObjFile, Ctx);
  TheCompiler = makeCompiler();

  CompiledEvaluators =
      (inCache) ? loadEvaluators(functionDepth, cachedObjFile)
                : compileEvaluators(functionDepth, switchDepth, cachedObjFile);
}

CompiledResolver::~CompiledResolver() { llvm::llvm_shutdown(); }

std::unique_ptr<llvm::Module>
CompiledResolver::makeModule(std::string name, llvm::LLVMContext &ctx) {
  auto M = std::make_unique<llvm::Module>("file:" + name, ctx);

  auto *targetMachine = llvm::EngineBuilder().selectTarget();
  M->setDataLayout(targetMachine->createDataLayout());

  return M;
}

std::unique_ptr<SimpleOrcJit> CompiledResolver::makeCompiler() {
  auto targetMachine = llvm::EngineBuilder().selectTarget();
  auto cache = std::make_unique<SimpleObjectCache>();

  return std::make_unique<SimpleOrcJit>(*targetMachine, std::move(cache));
}

uint64_t CompiledResolver::getNumCompiledEvaluators(uint8_t nodeLevelsPerFunction) {
  uint64_t expectedEvaluators = 0;

  uint8_t treeDepth = Log2(DecisionTree.size() + 1);
  for (uint8_t level = 0; level < treeDepth; level += nodeLevelsPerFunction)
    expectedEvaluators += PowerOf2(level);

  return expectedEvaluators;
}

CompiledResolver::SubtreeEvals_t
CompiledResolver::loadEvaluators(uint8_t nodeLevelsPerFunction,
                                 std::string objFileName) {
  uint64_t evaluators = getNumCompiledEvaluators(nodeLevelsPerFunction);

  printf("Loading %llu evaluators for %lu nodes from file %s\n\n", evaluators,
         DecisionTree.size(), objFileName.c_str());
  fflush(stdout);

  // load module from cache
  TheCompiler->loadModuleFromCache(std::move(TheModule));

  return collectEvaluatorFunctions(nodeLevelsPerFunction, "nodeEvaluator_");
}

CompiledResolver::SubtreeEvals_t
CompiledResolver::compileEvaluators(uint8_t nodeLevelsPerFunction,
                                    uint8_t nodeLevelsPerSwitch,
                                    std::string objFileName) {
  uint64_t evaluators = getNumCompiledEvaluators(nodeLevelsPerFunction);

  printf("Generating %llu evaluators for %lu nodes and cache it in file %s",
         evaluators, DecisionTree.size(), objFileName.c_str());

  std::string nameStub = "nodeEvaluator_";

  {
    printf("\nComposing...");
    fflush(stdout);

    using namespace std::chrono;
    auto start = high_resolution_clock::now();

    emitSubtreeEvaluators(nodeLevelsPerFunction, nodeLevelsPerSwitch, nameStub);

    auto end = high_resolution_clock::now();
    auto dur = duration_cast<seconds>(end - start);

    printf(" took %lld seconds", dur.count());
    fflush(stdout);
  }

#ifndef NDEBUG
  llvm::outs() << "\n\nWe just constructed this LLVM module:\n\n";
  llvm::outs() << *TheModule.get() << "\n\n";
#endif

  // submit module for jit compilation
  {
    printf("\nCompiling...");
    fflush(stdout);

    using namespace std::chrono;
    auto start = high_resolution_clock::now();

    TheCompiler->submitModule(std::move(TheModule));

    auto end = high_resolution_clock::now();
    auto dur = duration_cast<seconds>(end - start);

    printf(" took %lld seconds", dur.count());
    fflush(stdout);
  }

  printf("\nCollecting...");
  fflush(stdout);

  return collectEvaluatorFunctions(nodeLevelsPerFunction, nameStub);
}

CompiledResolver::SubtreeEvals_t
CompiledResolver::collectEvaluatorFunctions(int nodeLevelsPerFunction,
                                            std::string functionNameStub) {
  SubtreeEvals_t evaluators;
  int treeDepth = Log2(DecisionTree.size() + 1);

  for (int level = 0; level < treeDepth; level += nodeLevelsPerFunction) {
    uint64_t firstNodeIdxOnLevel = TreeNodes(level);
    uint64_t numNodesOnLevel = PowerOf2(level);

    for (uint64_t offset = 0; offset < numNodesOnLevel; offset++) {
      uint64_t nodeIdx = firstNodeIdxOnLevel + offset;

      std::string name = functionNameStub + std::to_string(nodeIdx);
      evaluators[nodeIdx] = TheCompiler->getFnPtr<SubtreeEvaluator_f>(name);
    }
  }

  assert(evaluators.size() == getNumCompiledEvaluators(nodeLevelsPerFunction));
  return evaluators;
}

void CompiledResolver::emitSubtreeEvaluators(uint8_t subtreeLevels,
                                             uint8_t switchLevels,
                                             const std::string &nameStub) {
  uint8_t treeDepth = Log2(DecisionTree.size() + 1);

  for (uint8_t level = 0; level < treeDepth; level += subtreeLevels) {
    uint64_t firstIdxOnLevel = TreeNodes(level);
    uint64_t firstIdxOnNextLevel = TreeNodes(level + 1);

    for (uint64_t nodeIdx = firstIdxOnLevel; nodeIdx < firstIdxOnNextLevel;
         nodeIdx++) {
      const TreeNode &node = DecisionTree.at(nodeIdx);

      std::string name = nameStub + std::to_string(nodeIdx);
      llvm::Function *evalFn = emitFunctionDeclaration(move(name));
      llvm::Value *dataSetPtr = &*evalFn->arg_begin();

      Builder.SetInsertPoint(llvm::BasicBlock::Create(Ctx, "entry", evalFn));
      auto *entryBB = Builder.GetInsertBlock();

      int numNestedSwitches = subtreeLevels / switchLevels - 1;
      llvm::Value *nextNodeIdx = emitSubtreeSwitchesRecursively(
          nodeIdx, switchLevels, evalFn, entryBB, dataSetPtr,
          (uint8_t)numNestedSwitches);

      Builder.CreateRet(nextNodeIdx);
      verifyFunction(*evalFn);
    }
  }
}

llvm::Function *CompiledResolver::emitFunctionDeclaration(std::string name) {
  using namespace llvm;

  auto returnTy = Type::getInt64Ty(Ctx);
  auto argTy = Type::getFloatTy(Ctx)->getPointerTo();
  auto signature = FunctionType::get(returnTy, {argTy}, false);
  auto linkage = Function::ExternalLinkage;

  Function *evalFn =
      Function::Create(signature, linkage, name, TheModule.get());

  evalFn->setName(name);

  AttributeSet attributeSet;
  evalFn->setAttributes(attributeSet.addAttribute(
      Ctx, AttributeSet::FunctionIndex, "target-features", "+avx"));

  return evalFn;
}

llvm::Value *
CompiledResolver::emitNodeLoad(const TreeNode &node,
                               llvm::Value *dataSetPtr) {
  llvm::Value *dataSetFeaturePtr =
      Builder.CreateConstGEP1_32(dataSetPtr, node.DataSetFeatureIdx);

  return Builder.CreateLoad(dataSetFeaturePtr);
}

llvm::Value *
CompiledResolver::emitNodeCompare(const TreeNode &node,
                                  llvm::Value *dataSetFeatureVal) {
  llvm::Constant *bias = llvm::ConstantFP::get(dataSetFeatureVal->getType(), node.Bias);

  return Builder.CreateFCmpOGT(dataSetFeatureVal, bias);
}

llvm::Value *CompiledResolver::emitSubtreeSwitchesRecursively(
    uint64_t subtreeRootIdx, uint8_t subtreeLevels, llvm::Function *function,
    llvm::BasicBlock *switchBB, llvm::Value *dataSetPtr,
    uint8_t remainingNestedSwitches) {
  using namespace llvm;
  Type *nodeIdxTy = Type::getInt64Ty(Ctx);
  auto numNodes = TreeNodes<uint8_t>(subtreeLevels);

  Value *conditionVector =
      emitComputeConditionVector(dataSetPtr, subtreeRootIdx, subtreeLevels);

  std::string returnBBLabel =
      "switch" + std::to_string(subtreeRootIdx) + "_return";
  auto *returnBB = BasicBlock::Create(Ctx, returnBBLabel, function);

  std::string defaultBBLabel =
      "switch" + std::to_string(subtreeRootIdx) + "_default";
  auto *defaultBB = BasicBlock::Create(Ctx, defaultBBLabel, function);

  std::string evalResultLabel =
      "switch_" + std::to_string(subtreeRootIdx) + "_value";
  Value *evalResult = Builder.CreateAlloca(nodeIdxTy, nullptr, evalResultLabel);
  Builder.CreateStore(ConstantInt::get(nodeIdxTy, 0), evalResult);

  auto *switchInst = Builder.CreateSwitch(conditionVector, defaultBB,
                                          PowerOf2<uint32_t>(numNodes - 1));

  std::vector<SubtreePathsMap_t> subtreeChildNodePaths =
      buildSubtreeChildNodePaths(subtreeRootIdx, subtreeLevels);

  std::unordered_map<uint64_t, BasicBlock *> nodeTargetBBs;

  for (const auto &childNodeInfo : subtreeChildNodePaths) {
    uint64_t childNodeIdx = childNodeInfo.first;

    nodeTargetBBs[childNodeIdx] = emitSubtreeSwitchTargetAndRecurse(
        childNodeIdx, subtreeLevels, function, dataSetPtr,
        remainingNestedSwitches, returnBB, evalResult);
  }

  BasicBlock *lastSwitchTargetBB = switchBB;
  for (const auto &childNodeInfo : subtreeChildNodePaths) {
    BasicBlock *nodeTargetBB = nodeTargetBBs[childNodeInfo.first];
    const PathBitsMap_t &pathBitsMap = childNodeInfo.second;

    emitSubtreeSwitchCaseLabels(switchBB, switchInst, nodeTargetBB, numNodes,
                                pathBitsMap);

    lastSwitchTargetBB = nodeTargetBB;
  }

  defaultBB->moveAfter(lastSwitchTargetBB);
  returnBB->moveAfter(defaultBB);

  Builder.SetInsertPoint(defaultBB);
  Builder.CreateBr(returnBB);

  Builder.SetInsertPoint(returnBB);
  return Builder.CreateLoad(evalResult);
}

llvm::BasicBlock *CompiledResolver::emitSubtreeSwitchTargetAndRecurse(
    uint64_t nodeIdx, uint8_t subtreeLevels, llvm::Function *function,
    llvm::Value *dataSetPtr, uint8_t remainingNestedSwitches,
    llvm::BasicBlock *returnBB, llvm::Value *evalResultPtr) {
  std::string nodeBBLabel = "n" + std::to_string(nodeIdx);
  llvm::BasicBlock *nodeBB = llvm::BasicBlock::Create(Ctx, nodeBBLabel, function);
  Builder.SetInsertPoint(nodeBB);

  if (remainingNestedSwitches == 0) {
    auto *nodeIdxTy = llvm::Type::getInt64Ty(Ctx);
    auto *nodeIdxVal = llvm::ConstantInt::get(nodeIdxTy, nodeIdx);

    Builder.CreateStore(nodeIdxVal, evalResultPtr);
  }
  else {
    llvm::Value *subSwitchResult = emitSubtreeSwitchesRecursively(
        nodeIdx, subtreeLevels, function, nodeBB, dataSetPtr,
        remainingNestedSwitches - 1);

    Builder.CreateStore(subSwitchResult, evalResultPtr);
  }

  Builder.CreateBr(returnBB);
  return nodeBB;
}

void CompiledResolver::emitSubtreeSwitchCaseLabels(
    llvm::BasicBlock *switchBB, llvm::SwitchInst *switchInst,
    llvm::BasicBlock *nodeBB,  uint8_t subtreeNodes,
    const CompiledResolver::PathBitsMap_t &pathBitsMap) {
  uint32_t conditionVectorTemplate =
      buildFixedBitsConditionVectorTemplate(pathBitsMap);

  std::vector<uint32_t> canonicalVariants =
      buildCanonicalConditionVectorVariants(
          subtreeNodes, conditionVectorTemplate, pathBitsMap);

  Builder.SetInsertPoint(switchBB);
  llvm::IntegerType *switchTy = llvm::IntegerType::get(Ctx, subtreeNodes + 1);

  for (uint32_t variant : canonicalVariants) {
    llvm::ConstantInt *caseVal = llvm::ConstantInt::get(switchTy, variant);
    switchInst->addCase(caseVal, nodeBB);
  }
}

llvm::Value *CompiledResolver::emitComputeConditionVector(
    llvm::Value *dataSetPtr, uint64_t subtreeRootIdx, uint8_t subtreeLevels) {
  using namespace llvm;

  uint8_t numNodes = TreeNodes<uint8_t>(subtreeLevels);
  uint8_t avxPackSize = numNodes + 1;

  std::vector<uint64_t> nodeIdxs =
      collectSubtreeNodeIdxs(subtreeRootIdx, numNodes);

  Value *dataSetValues = emitCollectDataSetValues(nodeIdxs, dataSetPtr);
  Value *treeNodeValues = emitDefineTreeNodeValues(nodeIdxs);

  Value *avxCmpResults =
      emitComputeCompareAvx(dataSetValues, treeNodeValues, avxPackSize);

  Value *bitShiftMasks =
      emitDefineBitShiftMaskValues(numNodes);

  Value *avxBitShiftResults =
      emitComputeBitShiftsAvx(avxCmpResults, bitShiftMasks, avxPackSize);

  return emitComputeHorizontalOrAvx(avxBitShiftResults, avxPackSize);
}

llvm::Value *CompiledResolver::emitCollectDataSetValues(
    const std::vector<uint64_t> &nodeIdxs, llvm::Value *dataSetPtr) {
  using namespace llvm;

  Type *int8Ty = Type::getInt8Ty(Ctx);
  Type *floatTy = Type::getFloatTy(Ctx);

  size_t numNodes = nodeIdxs.size();
  Constant *numNodeAllocs = ConstantInt::get(int8Ty, numNodes + 1);

  Value *featureValues = Builder.Insert(
      new AllocaInst(floatTy, numNodeAllocs, 32), "featureValues");

  for (uint8_t bitOffset = 0; bitOffset < numNodes; bitOffset++) {
    const TreeNode &node = DecisionTree.at(nodeIdxs.at(bitOffset));
    Value *dataSetFeatureVal = emitNodeLoad(node, dataSetPtr);

    Builder.CreateStore(
        dataSetFeatureVal,
        Builder.CreateConstGEP1_32(featureValues, bitOffset));
  }

  return featureValues;
}

llvm::Value *CompiledResolver::emitDefineTreeNodeValues(
    const std::vector<uint64_t> &nodeIdxs) {
  using namespace llvm;

  Type *int8Ty = Type::getInt8Ty(Ctx);
  Type *floatTy = Type::getFloatTy(Ctx);

  size_t numNodes = nodeIdxs.size();
  Constant *numNodeAllocs = ConstantInt::get(int8Ty, numNodes + 1);

  Value *compareValues = Builder.Insert(
      new AllocaInst(floatTy, numNodeAllocs, 32), "compareValues");

  for (uint8_t bitOffset = 0; bitOffset < numNodes; bitOffset++) {
    const TreeNode &node = DecisionTree.at(nodeIdxs[bitOffset]);

    Builder.CreateStore(
        ConstantFP::get(floatTy, node.Bias),
        Builder.CreateConstGEP1_32(compareValues, bitOffset));
  }

  return compareValues;
}

llvm::Value *CompiledResolver::emitDefineBitShiftMaskValues(
    uint64_t numNodes) {
  using namespace llvm;

  Type *int8Ty = Type::getInt8Ty(Ctx);
  Type *int32Ty = Type::getInt32Ty(Ctx);

  assert(isPowerOf2(numNodes + 1));
  Constant *numNodeAllocs = ConstantInt::get(int8Ty, numNodes + 1);

  Value *bitShiftValues = Builder.Insert(
      new AllocaInst(int32Ty, numNodeAllocs, 32), "bitShiftValues");

  for (uint8_t i = 0; i < numNodes; i++) {
    Builder.CreateStore(
        ConstantInt::get(int32Ty, PowerOf2(i)),
        Builder.CreateConstGEP1_32(bitShiftValues, numNodes - 1 - i));
  }

  // AND with 0 in unused last item
  Builder.CreateStore(
      ConstantInt::get(int32Ty, 0),
      Builder.CreateConstGEP1_32(bitShiftValues, numNodes));

  return bitShiftValues;
}

llvm::Value *CompiledResolver::emitComputeCompareAvx(
    llvm::Value *lhs, llvm::Value *rhs, uint8_t items) {
  using namespace llvm;

  Type *floatTy = Type::getFloatTy(Ctx);
  Type *avxCmpOpTy = Type::getInt8Ty(Ctx);
  Type *avxPackedFloatsTy = VectorType::get(floatTy, items);
  Type *avxPackedFloatsPtrTy = avxPackedFloatsTy->getPointerTo();

  Value *lhsAvxPtr = Builder.CreateBitCast(lhs, avxPackedFloatsPtrTy);
  Value *rhsAvxPtr = Builder.CreateBitCast(rhs, avxPackedFloatsPtrTy);

  Function *avxCmpFn = Intrinsic::getDeclaration(
      TheModule.get(), Intrinsic::x86_avx_cmp_ps_256);

  ArrayRef<Value*> avxCmpArgs {
      Builder.CreateAlignedLoad(lhsAvxPtr, 32),
      Builder.CreateAlignedLoad(rhsAvxPtr, 32),
      ConstantInt::get(avxCmpOpTy, 14) // _CMP_GT_OS
  };

  return Builder.CreateCall(avxCmpFn, avxCmpArgs);
}

llvm::Value *CompiledResolver::emitComputeBitShiftsAvx(
    llvm::Value *avxPackedCmpResults, llvm::Value *bitShiftValues, uint8_t items) {
  using namespace llvm;

  Type *intTy = Type::getInt32Ty(Ctx);
  Type *avx8IntsTy = VectorType::get(intTy, items);

  Value* avxBitShiftIntsPtr =
      Builder.CreateBitCast(bitShiftValues, avx8IntsTy->getPointerTo());

  Value* avxBitShiftInts =
      Builder.CreateAlignedLoad(avxBitShiftIntsPtr, 32);

  Value* avxCmpResInts =
      Builder.CreateBitCast(avxPackedCmpResults, avx8IntsTy);

  return Builder.CreateAnd(avxCmpResInts, avxBitShiftInts);
}

llvm::Value *CompiledResolver::emitComputeHorizontalOrAvx(
    llvm::Value *avxPackedInts, uint8_t items) {
  using namespace llvm;

  Type *intTy = Type::getInt32Ty(Ctx);
  Type *avx8IntsTy = VectorType::get(intTy, items);
  Type *avx4IntsTy = VectorType::get(intTy, items / 2);

  Value *no4Ints = UndefValue::get(avx4IntsTy);
  Value *no8Ints = UndefValue::get(avx8IntsTy);

  Value *avxOr_04_15_26_37 = Builder.CreateOr(
      Builder.CreateShuffleVector(avxPackedInts, no8Ints, {0, 1, 2, 3}),
      Builder.CreateShuffleVector(avxPackedInts, no8Ints, {4, 5, 6, 7}));

  Value* avxOr_0145_15_2367_37 = Builder.CreateOr(
      Builder.CreateShuffleVector(avxOr_04_15_26_37, no4Ints, {1, 1, 3, 3}),
      avxOr_04_15_26_37);

  return Builder.CreateOr(
      Builder.CreateExtractElement(avxOr_0145_15_2367_37, 0ull),
      Builder.CreateExtractElement(avxOr_0145_15_2367_37, 2ull));
}

std::vector<uint64_t> CompiledResolver::collectSubtreeNodeIdxs(
    uint64_t subtreeRootIdx, uint8_t subtreeNodes) {
  std::vector<uint64_t> nodeIdxs;
  nodeIdxs.reserve(subtreeNodes);

  int subtreeRootIdxLevel = Log2(subtreeRootIdx + 1);
  uint64_t firstIdxOnRootLevel = TreeNodes(subtreeRootIdxLevel);
  uint64_t subtreeRootIdxOffset = subtreeRootIdx - firstIdxOnRootLevel;

  for (uint8_t i = 0; i < subtreeNodes; i++) {
    int nodeLevelInSubtree = Log2(i + 1);

    uint64_t firstIdxOnNodeLevel =
        TreeNodes(subtreeRootIdxLevel + nodeLevelInSubtree);

    uint64_t numSubtreeNodesOnLevel = PowerOf2(nodeLevelInSubtree);
    uint64_t firstSubtreeIdxOnNodeLevel =
        firstIdxOnNodeLevel + subtreeRootIdxOffset * numSubtreeNodesOnLevel;

    uint32_t nodeOffsetInSubtreeLevel = i - (PowerOf2<uint32_t>(nodeLevelInSubtree) - 1);
    nodeIdxs.push_back(firstSubtreeIdxOnNodeLevel + nodeOffsetInSubtreeLevel);
  }

  return nodeIdxs;
}

std::vector<CompiledResolver::SubtreePathsMap_t>
CompiledResolver::buildSubtreeChildNodePaths(
    uint64_t subtreeRootIdx, uint8_t subtreeLevels) {
  auto subtreeNodes = TreeNodes<uint8_t>(subtreeLevels);
  auto subtreeChildNodes = PowerOf2<uint8_t>(subtreeLevels);

  std::vector<uint64_t> subtreeNodeIdxs =
      collectSubtreeNodeIdxs(subtreeRootIdx, subtreeNodes);

  std::unordered_map<uint64_t, uint8_t> bitOffsets;
  bitOffsets.reserve(subtreeNodes);

  for (uint8_t i = 0; i < subtreeNodes; i++)
    bitOffsets[subtreeNodeIdxs[i]] = i;

  std::vector<SubtreePathsMap_t> results;
  results.reserve(subtreeChildNodes);

  buildSubtreeChildNodePathsRecursively(subtreeRootIdx, subtreeLevels,
                                        bitOffsets, results);

  assert(results.size() == subtreeChildNodes);
  return results;
}

void CompiledResolver::buildSubtreeChildNodePathsRecursively(
    uint64_t nodeIdx, uint8_t remainingLevels,
    const std::unordered_map<uint64_t, uint8_t> &bitOffsets,
    std::vector<SubtreePathsMap_t> &result) {
  if (remainingLevels == 0) {
    // children of subtree leaf nodes create empty path maps
    std::unordered_map<uint8_t, bool> pathBitsMap;
    result.push_back({nodeIdx, pathBitsMap});
  } else {
    // subtree nodes add their offsets to their leafs' child path maps
    const TreeNode &node = DecisionTree.at(nodeIdx);
    uint8_t thisBitOffset = bitOffsets.at(nodeIdx);
    uint8_t numChildLeafPaths = PowerOf2<uint8_t>(remainingLevels);
    uint8_t numChildsPerCond = numChildLeafPaths / 2;

    buildSubtreeChildNodePathsRecursively(node.TrueChildNodeIdx,
                                          remainingLevels - 1,
                                          bitOffsets, result);

    for (uint8_t i = 0; i < numChildsPerCond; i++) {
      result[result.size() - i - 1].second[thisBitOffset] = true;
    }

    buildSubtreeChildNodePathsRecursively(node.FalseChildNodeIdx,
                                          remainingLevels - 1,
                                          bitOffsets, result);

    for (uint8_t i = 0; i < numChildsPerCond; i++) {
      result[result.size() - i - 1].second[thisBitOffset] = false;
    }
  }
}

uint32_t CompiledResolver::buildFixedBitsConditionVectorTemplate(
    const PathBitsMap_t &subtreePaths) {
  uint32_t fixedBitsVector = 0;
  for (const auto &mapEntry : subtreePaths) {
    uint32_t bit = mapEntry.second ? 1 : 0;
    uint32_t vectorBit = bit << mapEntry.first;
    fixedBitsVector |= vectorBit;
  }

  return fixedBitsVector;
}

std::vector<uint32_t> CompiledResolver::buildCanonicalConditionVectorVariants(
    uint8_t numNodes, uint32_t fixedBitsTemplate,
    const PathBitsMap_t &subtreePaths) {
  std::vector<uint8_t> variableBitOffsets;
  for (uint8_t bitOffset = 0; bitOffset < numNodes; bitOffset++) {
    if (subtreePaths.find(bitOffset) == subtreePaths.end())
      variableBitOffsets.push_back(bitOffset);
  }

  if (variableBitOffsets.empty()) {
    return {fixedBitsTemplate};
  } else {
    std::vector<uint32_t> result;

    auto expectedVariants = PowerOf2<uint32_t>(variableBitOffsets.size());
    result.reserve(expectedVariants);

    buildCanonicalConditionVectorVariantsRecursively(
        fixedBitsTemplate, variableBitOffsets, 0, result);

    assert(result.size() == expectedVariants);
    return result;
  }
}

void CompiledResolver::buildCanonicalConditionVectorVariantsRecursively(
    uint32_t conditionVector, const std::vector<uint8_t> &variableBitOffsets,
    uint8_t bitToVaryIdx, std::vector<uint32_t> &result) {
  if (bitToVaryIdx < variableBitOffsets.size()) {
    uint8_t bitToVary = variableBitOffsets.at(bitToVaryIdx);
    uint32_t vectorTrueBit = 1u << bitToVary;

    // bit must still be in default zero state
    assert((conditionVector & ~vectorTrueBit) == conditionVector);
    uint8_t nextBitIdx = bitToVaryIdx + 1;

    // true variation
    buildCanonicalConditionVectorVariantsRecursively(
        conditionVector | vectorTrueBit, variableBitOffsets, nextBitIdx,
        result);

    // false variation
    buildCanonicalConditionVectorVariantsRecursively(
        conditionVector, variableBitOffsets, nextBitIdx, result);
  } else {
    result.push_back(conditionVector);
  }
}

uint64_t CompiledResolver::run(const DecisionTree_t &tree,
                               const DataSet_t &dataSet) {
  assert(&tree == &DecisionTree);

  uint64_t idx = 0;
  uint64_t firstResultIdx = DecisionTree.size();
  const float *data = dataSet.data();

  while (idx < firstResultIdx) {
    SubtreeEvaluator_f *compiledEvaluator = CompiledEvaluators.at(idx);
    idx = compiledEvaluator(data);
  }

  return idx;
}
