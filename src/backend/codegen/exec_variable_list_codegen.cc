//---------------------------------------------------------------------------
//  Greenplum Database
//  Copyright (C) 2016 Pivotal Software, Inc.
//
//  @filename:
//    codegen_utils.cpp
//
//  @doc:
//    Contains different code generators
//
//---------------------------------------------------------------------------


#include <algorithm>
#include <cstdint>
#include <string>

#include "codegen/exec_variable_list_codegen.h"
#include "codegen/slot_getattr_codegen.h"
#include "codegen/utils/clang_compiler.h"
#include "codegen/utils/utility.h"
#include "codegen/utils/gp_codegen_utils.h"
#include "codegen/base_codegen.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Casting.h"

extern "C" {
#include "postgres.h"  // NOLINT(build/include)
#include "utils/elog.h"
#include "access/htup.h"
#include "nodes/execnodes.h"
#include "executor/tuptable.h"
}

using gpcodegen::ExecVariableListCodegen;
using gpcodegen::SlotGetAttrCodegen;

constexpr char ExecVariableListCodegen::kExecVariableListPrefix[];

ExecVariableListCodegen::ExecVariableListCodegen
(
    ExecVariableListFn regular_func_ptr,
    ExecVariableListFn* ptr_to_regular_func_ptr,
    ProjectionInfo* proj_info,
    TupleTableSlot* slot) :
    BaseCodegen(kExecVariableListPrefix,
        regular_func_ptr,
        ptr_to_regular_func_ptr),
    proj_info_(proj_info),
    slot_(slot) {
}


bool ExecVariableListCodegen::GenerateExecVariableList(
    gpcodegen::GpCodegenUtils* codegen_utils) {
  assert(NULL != codegen_utils);
  static_assert(sizeof(Datum) == sizeof(int64),
      "sizeof(Datum) doesn't match sizeof(int64)");

  if ( NULL == proj_info_->pi_varSlotOffsets ) {
    elog(DEBUG1,
        "Cannot codegen ExecVariableList because varSlotOffsets are null");
    return false;
  }

  // Only do codegen if all the elements in the target list are on the same
  // tuple slot This is an assumption for scan nodes, but we fall back when
  // joins are involved.
  for (int i = list_length(proj_info_->pi_targetlist) - 1; i > 0; i--) {
    if (proj_info_->pi_varSlotOffsets[i] !=
        proj_info_->pi_varSlotOffsets[i-1]) {
      elog(DEBUG1,
          "Cannot codegen ExecVariableList because multiple slots to deform.");
      return false;
    }
  }

  // Find the largest attribute index in projInfo->pi_targetlist
  int max_attr = *std::max_element(
      proj_info_->pi_varNumbers,
      proj_info_->pi_varNumbers + list_length(proj_info_->pi_targetlist));

  // System attribute
  if (max_attr <= 0) {
    elog(DEBUG1, "Cannot generate code for ExecVariableList"
                 "because max_attr is negative (i.e., system attribute).");
    return false;
  } else if (max_attr > slot_->tts_tupleDescriptor->natts) {
    elog(DEBUG1, "Cannot generate code for ExecVariableList"
                 "because max_attr is greater than natts.");
    return false;
  }

  llvm::Function* exec_variable_list_func = CreateFunction<ExecVariableListFn>(
      codegen_utils, GetUniqueFuncName());

  auto irb = codegen_utils->ir_builder();

  // BasicBlocks
  llvm::BasicBlock* entry_block = codegen_utils->CreateBasicBlock(
      "entry", exec_variable_list_func);
  // BasicBlock for checking correct slot
  llvm::BasicBlock* slot_check_block = codegen_utils->CreateBasicBlock(
      "slot_check", exec_variable_list_func);
  // BasicBlock for main.
  llvm::BasicBlock* main_block = codegen_utils->CreateBasicBlock(
      "main", exec_variable_list_func);
  // BasicBlock for final computations.
  llvm::BasicBlock* final_block = codegen_utils->CreateBasicBlock(
      "final", exec_variable_list_func);
  llvm::BasicBlock* fallback_block = codegen_utils->CreateBasicBlock(
      "fallback", exec_variable_list_func);

  // Generation-time constants
  llvm::Value* llvm_max_attr = codegen_utils->GetConstant(max_attr);
  llvm::Value* llvm_slot = codegen_utils->GetConstant(slot_);

  // Function arguments to ExecVariableList
  llvm::Value* llvm_projInfo_arg = ArgumentByPosition(exec_variable_list_func,
                                                      0);
  llvm::Value* llvm_values_arg = ArgumentByPosition(exec_variable_list_func, 1);
  llvm::Value* llvm_isnull_arg = ArgumentByPosition(exec_variable_list_func, 2);


  // Generate slot_getattr for attributes all the way to max_attr
  std::string slot_getattr_func_name =
      "slot_getattr_" + std::to_string(max_attr);
  llvm::Function* slot_getattr_func = nullptr;
  bool ok = SlotGetAttrCodegen::GenerateSlotGetAttr(
      codegen_utils,
      slot_getattr_func_name,
      slot_,
      max_attr,
      &slot_getattr_func);
  if (!ok) {
    elog(DEBUG1, "Cannot generate code for ExecVariableList "
                 "because slot_getattr generation failed!");
    return false;
  }
  assert(nullptr != slot_getattr_func);

  // Entry block
  // -----------
  irb->SetInsertPoint(entry_block);
  irb->CreateBr(slot_check_block);

  // Slot check block
  // ----------------

  irb->SetInsertPoint(slot_check_block);
  llvm::Value* llvm_econtext =
      irb->CreateLoad(codegen_utils->GetPointerToMember(
          llvm_projInfo_arg, &ProjectionInfo::pi_exprContext));

  // We want to fall back when ExecVariableList is called with a slot that's
  // different from the one we generated the function (eg HashJoin). We also
  // assume only 1 slot and that the slot is in a scan node ie from
  // exprContext->ecxt_scantuple or varOffset = 0
  llvm::Value* llvm_slot_arg =
      irb->CreateLoad(codegen_utils->GetPointerToMember(
          llvm_econtext, &ExprContext::ecxt_scantuple));

  irb->CreateCondBr(
      irb->CreateICmpEQ(llvm_slot, llvm_slot_arg),
      main_block /* true */,
      fallback_block /* false */);


  // Main block
  // ----------
  irb->SetInsertPoint(main_block);
  // Allocate a dummy int so that slot_getattr can write isnull out
  llvm::Value* llvm_dummy_isnull =
      irb->CreateAlloca(codegen_utils->GetType<bool>());
  irb->CreateCall(slot_getattr_func, {
      llvm_slot,
      llvm_max_attr,
      llvm_dummy_isnull
  });

  irb->CreateBr(final_block);


  // Final Block
  // -----------
  irb->SetInsertPoint(final_block);
  llvm::Value* llvm_slot_PRIVATE_tts_isnull /* bool* */ =
      irb->CreateLoad(codegen_utils->GetPointerToMember(
          llvm_slot, &TupleTableSlot::PRIVATE_tts_isnull));
  llvm::Value* llvm_slot_PRIVATE_tts_values /* Datum* */ =
      irb->CreateLoad(codegen_utils->GetPointerToMember(
          llvm_slot, &TupleTableSlot::PRIVATE_tts_values));

  // This code from ExecVariableList copies the contents of the isnull & values
  // arrays in the slot to output variable from the function parameters to
  // ExecVariableList
  int  *varNumbers = proj_info_->pi_varNumbers;
  for (int i = list_length(proj_info_->pi_targetlist) - 1; i >= 0; i--) {
    // *isnull = slot->PRIVATE_tts_isnull[attnum-1];
    llvm::Value* llvm_isnull_from_slot_val =
        irb->CreateLoad(irb->CreateInBoundsGEP(
              llvm_slot_PRIVATE_tts_isnull,
              {codegen_utils->GetConstant(varNumbers[i] - 1)}));
    llvm::Value* llvm_isnull_ptr =
        irb->CreateInBoundsGEP(llvm_isnull_arg,
                               {codegen_utils->GetConstant(i)});
    irb->CreateStore(llvm_isnull_from_slot_val, llvm_isnull_ptr);

    // values[i] = slot_getattr(varSlot, varNumber+1, &(isnull[i]));
    llvm::Value* llvm_value_from_slot_val =
        irb->CreateLoad(irb->CreateInBoundsGEP(
              llvm_slot_PRIVATE_tts_values,
              {codegen_utils->GetConstant(varNumbers[i] - 1)}));
    llvm::Value* llvm_values_ptr =
        irb->CreateInBoundsGEP(llvm_values_arg,
                               {codegen_utils->GetConstant(i)});
    irb->CreateStore(llvm_value_from_slot_val, llvm_values_ptr);
  }

  irb->CreateRetVoid();


  // Fall back Block
  // ---------------
  irb->SetInsertPoint(fallback_block);
  codegen_utils->CreateElog(
      DEBUG1,
      "Falling back to regular ExecVariableList");

  codegen_utils->CreateFallback<ExecVariableListFn>(
      codegen_utils->GetOrRegisterExternalFunction(GetRegularFuncPointer()),
      exec_variable_list_func);

  return true;
}




bool ExecVariableListCodegen::GenerateCodeInternal(
    GpCodegenUtils* codegen_utils) {
  bool isGenerated = GenerateExecVariableList(codegen_utils);

  if (isGenerated) {
    elog(DEBUG1, "ExecVariableList was generated successfully!");
    return true;
  } else {
    elog(DEBUG1, "ExecVariableList generation failed!");
    return false;
  }
}
