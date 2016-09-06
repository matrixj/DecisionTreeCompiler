#include <gtest/gtest.h>

#include "test/TestDecisionTree.h"

#include "test/TestCGEvaluationPath.h"
#include "test/TestCGEvaluationPathsBuilder.h"
#include "test/TestCGConditionVectorVariationsBuilder.h"

#include "test/TestSingleCodegenL1.h"

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
