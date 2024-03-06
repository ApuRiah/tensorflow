/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/mlir/tensorflow/analysis/resource_dataflow.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "mlir/Analysis/DataFlow/SparseAnalysis.h"  // from @llvm-project
#include "mlir/Analysis/DataFlowFramework.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/SymbolTable.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/analysis/tf_dataflow.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_saved_model.h"

#define DEBUG_TYPE "resource-dataflow"

namespace mlir {
namespace TF {

namespace {
constexpr char kCompositeDevice[] = "tf._composite_device";
}  // namespace

ResourceConstructingOps::ResourceConstructingOps(Operation *op) {
  if (op) ops.insert(op);
}

ResourceConstructingOps ResourceConstructingOps::EntryState(
    MLIRContext *context) {
  return ResourceConstructingOps();
}
ResourceConstructingOps ResourceConstructingOps::EntryState(Value value) {
  if (auto barg = value.dyn_cast<BlockArgument>()) {
    if (func::FuncOp func =
            dyn_cast<func::FuncOp>(barg.getOwner()->getParentOp())) {
      SymbolTable symbol_table(func->getParentOfType<ModuleOp>());
      auto global_tensor = tf_saved_model::LookupBoundInputOfType<
          tf_saved_model::GlobalTensorOp>(func, barg.getArgNumber(),
                                          symbol_table);
      ResourceConstructingOps result(global_tensor);
      if (func.getArgAttr(barg.getArgNumber(), kCompositeDevice)) {
        result.is_on_composite_device = true;
      }
      return result;
    }
  } else if (auto vh = dyn_cast<TF::VarHandleOp>(value.getDefiningOp())) {
    return ResourceConstructingOps(vh);
  } else if (auto it = dyn_cast<TF::IteratorOp>(value.getDefiningOp())) {
    return ResourceConstructingOps(it);
  }
  return ResourceConstructingOps();
}

ResourceConstructingOps ResourceConstructingOps::join(
    const ResourceConstructingOps &lhs, const ResourceConstructingOps &rhs) {
  // Take union of both sets of possible GlobalTensorOp values that can be
  // referenced here.
  ResourceConstructingOps ret;
  ret.ops.insert(lhs.ops.begin(), lhs.ops.end());
  ret.ops.insert(rhs.ops.begin(), rhs.ops.end());
  ret.is_on_composite_device =
      lhs.is_on_composite_device || rhs.is_on_composite_device;
  return ret;
}

void ResourceConstructingOps::print(raw_ostream &os) const {
  llvm::interleaveComma(ops, os << "[");
  if (is_on_composite_device) {
    os << " COMPOSITE";
  }
  os << "]";
}

class ResourceDataflowAnalysis
    : public TensorflowDataflowAnalysis<ResourceConstructingOps> {
 public:
  using TensorflowDataflowAnalysis<
      ResourceConstructingOps>::TensorflowDataflowAnalysis;
  void visitOperation(Operation *op, ArrayRef<const StateT *> operands,
                      ArrayRef<StateT *> results) override;
  ~ResourceDataflowAnalysis() override = default;
};

void ResourceDataflowAnalysis::visitOperation(Operation *op,
                                              ArrayRef<const StateT *> operands,
                                              ArrayRef<StateT *> results) {
  LLVM_DEBUG(llvm::dbgs() << "ResAn: Visiting operation: " << *op << "\n");

  if (ForwardThroughTFOperation(op, operands, results)) return;

  setAllToEntryStates(results);
}

void LoadResourceDataflowAnalysis(DataFlowSolver &solver) {
  solver.load<ResourceDataflowAnalysis>();
}

}  // namespace TF
}  // namespace mlir
