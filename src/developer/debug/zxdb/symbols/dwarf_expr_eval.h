// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_EXPR_EVAL_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_EXPR_EVAL_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "lib/fit/function.h"
#include "src/developer/debug/shared/register_id.h"
#include "src/developer/debug/zxdb/common/data_extractor.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/common/int128_t.h"
#include "src/developer/debug/zxdb/common/tagged_data_builder.h"
#include "src/developer/debug/zxdb/symbols/arch.h"
#include "src/developer/debug/zxdb/symbols/dwarf_expr.h"
#include "src/developer/debug/zxdb/symbols/symbol_context.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/ref_ptr.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

class SymbolDataProvider;

// This class evaluates DWARF expressions. These expressions are used to encode the locations of
// variables and a few other nontrivial lookups.
//
// This class is complicated by supporting asynchronous interactions with the debugged program. This
// means that accessing register and memory data (which may be required to evaluate the expression)
// may be asynchronous.
//
//  eval_ = std::make_unique<DwarfExprEval>();
//  eval_.eval(..., [](DwarfExprEval* eval, const Err& err) {
//    if (err.has_error()) {
//      // Handle error.
//    } else {
//      ... use eval->GetResult() ...
//    }
//  });
class DwarfExprEval {
 public:
  // Type of completion from a call. Async completion will happen in a callback
  // in the future.
  enum class Completion { kSync, kAsync };

  // A DWARF expression can compute either the address of the desired object in the debugged
  // programs address space, or it can compute the actual value of the object (because it may not
  // exist in memory).
  enum class ResultType {
    // The return value from GetResult() is a pointer to the result in memory. The caller will need
    // to know the size and type of this result from the context.
    kPointer,

    // The return value from GetResult() is the resulting value itself. Most results will need
    // to be truncated to the correct size (the caller needs to know the size and type from the
    // context).
    kValue,

    // The result is stored in a data block returned by result_data(). It can be any size. Do not
    // call GetResult() as the stack normally has no data on it in this case.
    kData
  };

  enum class StringOutput {
    kNone,     // Don't do string output.
    kLiteral,  // Outputs exact DWARF opcodes and values.
    kPretty,   // Decodes values and register names.
  };

  // The DWARF spec says the stack entry "can represent a value of any supported base type of the
  // target machine". We need to support x87 long doubles (80 bits) and XMM registers (128 bits).
  // Generally the XMM registers used for floating point use only the low 64 bits and long doubles
  // are very uncommon, but using 128 bits here covers the edge cases better. The ARM "v" registers
  // (128 bits) are similar.
  //
  // The YMM (256 bit) and ZMM (512 bit) x64 reigisters aren't currently representable in DWARF
  // expressions so larger numbers are unnecessary.
  using StackEntry = uint128_t;
  using SignedStackEntry = int128_t;

  using CompletionCallback = fit::callback<void(DwarfExprEval* eval, const Err& err)>;

  DwarfExprEval();
  ~DwarfExprEval();

  // Pushes a value on the stack. Call before Eval() for the cases where an expression requires
  // some intitial state.
  void Push(StackEntry value);

  // Clears any existing values in the stack.
  void Clear() { stack_.clear(); }

  // A complete expression has finished executing but may or may not have had an error. A successful
  // expression indicates execution is complete and there is a valid result to read.
  bool is_complete() const { return is_complete_; }
  bool is_success() const { return is_success_; }

  // Valid when is_success(), this indicates how to interpret the value from GetResult().
  ResultType GetResultType() const;

  // Valid when is_success() and type() == kPointer/kValue. Returns the result of evaluating the
  // expression. The meaning will be dependent on the context of the expression being evaluated.
  // Most results will be smaller than this in which case they will use only the low bits.
  StackEntry GetResult() const;

  // Destructively returns the generated data buffer. Valid when is_success() and type() == kData.
  TaggedData TakeResultData();

  // When the result is computed, this will indicate if the result is directly from a register,
  // and if it is, which one. If the current result was the result of some computation and has no
  // direct register source, it will be RegisterID::kUnknown.
  debug::RegisterID current_register_id() const { return current_register_id_; }

  // When the result is computed, this will indicate whether it's from a constant source (encoded in
  // the DWARF expression) or is the result of reading some memory or registers.
  bool result_is_constant() const { return result_is_constant_; }

  // Evaluates the expression using the current stack. If the stack needs initial setup, callers
  // should call Push() first, or Clear() if there might be unwanted data.
  //
  // This will take a reference to the SymbolDataProvider until the computation is complete.
  //
  // The symbol context is used to evaluate relative addresses. It should be the context associated
  // with the module that this expression is from. Normally this will be retrieved from the
  // symbol that generated the dwarf expression (see DwarfExpr::source()).
  //
  // The return value will indicate if the request completed synchronously. In synchronous
  // completion the callback will have been called reentrantly from within the stack of this
  // function. This does not indicate success as it could succeed or fail both synchronously and
  // asynchronously.
  //
  // This class must not be deleted from within the completion callback.
  Completion Eval(fxl::RefPtr<SymbolDataProvider> data_provider,
                  const SymbolContext& symbol_context, DwarfExpr expr, CompletionCallback cb);

  // Converts the given DWARF expression to a string. The result values on this class won't be
  // set since the expression won't actually be evaluated.
  //
  // The data_provider is required to get the current architecture for pretty-printing register
  // names. To disable this, pass the default SymbolDataProvider implementation.
  //
  // When "pretty" mode is enabled, operations will be simplified and platform register names will
  // be substituted.
  std::string ToString(fxl::RefPtr<SymbolDataProvider> data_provider,
                       const SymbolContext& symbol_context, DwarfExpr expr, bool pretty);

 private:
  void SetUp(fxl::RefPtr<SymbolDataProvider> data_provider, const SymbolContext& symbol_context,
             DwarfExpr expr, CompletionCallback cb);

  // Evaluates the next phases of the expression until an asynchronous operation is required.
  // Returns the value of |is_complete| because |this| could be deleted by the time this method
  // returns.
  bool ContinueEval();

  // Evaluates a single operation.
  Completion EvalOneOp();

  // Adds a register's contents + an offset to the stack. Use 0 for the offset to get the raw
  // register value.
  Completion PushRegisterWithOffset(int dwarf_register_number, int128_t offset);

  // These read constant data from the current index in the stream. The size of the data is in
  // byte_size, and the result will be extended to a stack entry according to the type.
  //
  // They return true if the value was read, false if there wasn't enough data (they will issue the
  // error internally, the calling code should just return on failure).
  bool ReadSigned(int byte_size, SignedStackEntry* output);
  bool ReadUnsigned(int byte_size, StackEntry* output);

  // Reads a signed or unsigned LEB constant from the stream. They return true if the value was
  // read, false if there wasn't enough data (they will issue the error internally, the calling code
  // should just return on failure).
  bool ReadLEBSigned(SignedStackEntry* output);
  bool ReadLEBUnsigned(StackEntry* output);

  // Schedules an asynchronous memory read. If there is any failure, including short reads, this
  // will report it and fail evaluation.
  //
  // If the correct amount of memory is read, it will issue the callback with the data and then
  // continue evaluation.
  void ReadMemory(TargetPointer address, uint32_t byte_size,
                  fit::callback<void(DwarfExprEval* eval, std::vector<uint8_t> value)> on_success);

  // Reports the given error. Always returns "kSync" so the caller can do "return ReportError()"
  // which would be the common case.
  Completion ReportError(const std::string& msg);
  Completion ReportError(const Err& err);
  void ReportStackUnderflow();
  void ReportUnimplementedOpcode(uint8_t op);

  // Executes the given unary operation with the top stack entry as the parameter and pushes the
  // result.
  Completion OpUnary(StackEntry (*op)(StackEntry), const char* op_name);

  // Executes the given binary operation by popping the top two stack entries as parameters (the
  // first is the next-to-top, the second is the top) and pushing the result on the stack.
  Completion OpBinary(StackEntry (*op)(StackEntry, StackEntry), const char* op_name);

  // Implements DW_OP_addrx and DW_OP_constx (corresponding to the given result types). The type
  // of the result on the stack will be set to the given result type, and kPointer result types
  // will be relocated according to the module's address offset.
  Completion OpAddrBase(ResultType result_type, const char* op_name);

  // Operations. On call, the data extractor will read at the byte following the opcode, and on
  // return it will point to the next instruction (any parameters will be consumed).
  //
  // Some functions handle more than one opcode. In these cases, the opcode name for string output
  // is passed in as op_name.
  Completion OpAddr();
  Completion OpBitPiece();
  Completion OpBra();
  Completion OpBreg(uint8_t op);
  Completion OpCFA();
  Completion OpDeref(uint32_t byte_size, const char* op_name, bool string_include_size);
  Completion OpDerefSize();
  Completion OpDiv();
  Completion OpDrop();
  Completion OpDup();
  Completion OpEntryValue(const char* op_name);
  Completion OpFbreg();
  Completion OpImplicitPointer(const char* op_name);
  Completion OpImplicitValue();
  Completion OpRegx();
  Completion OpBregx();
  Completion OpMod();
  Completion OpOver();
  Completion OpPick();
  Completion OpPiece();
  Completion OpPlusUconst();
  Completion OpPushSigned(int byte_count, const char* op_name);
  Completion OpPushUnsigned(int byte_count, const char* op_name);
  Completion OpPushLEBSigned();
  Completion OpPushLEBUnsigned();
  Completion OpRot();
  Completion OpSkip();
  Completion OpStackValue();
  Completion OpSwap();
  Completion OpTlsAddr(const char* op_name);

  // Adjusts the instruction offset by the given amount, handling out-of-bounds as appropriate. This
  // is the backend for jumps and branches.
  void Skip(int128_t amount);

  // Returns true if generating a string rather than evaluating an expression.
  bool is_string_output() const { return string_output_mode_ != StringOutput::kNone; }

  // Returns a user-readable name for the current architecture's given DWARF register. For
  // stringinfying DWARF expressions.
  std::string GetRegisterName(int reg_number) const;

  // Append an operation to the description in is_string_output() mode (will assert if used outside
  // of this mode).
  //
  // When in kPretty output mode, the second parameter will be used if present. Otherwise the first
  // parameter will be used. The first output should be the actual DWARF operatiors, while the
  // second one can have another level of decode.
  //
  // Always returns "sync" completion so it can be used like "return AppendString(...)" from the
  // opcode handlers.
  Completion AppendString(const std::string& op_output,
                          const std::string& nice_output = std::string());

  fxl::RefPtr<SymbolDataProvider> data_provider_;
  SymbolContext symbol_context_;

  // The expression. See also data_extractor_ which points into here.
  DwarfExpr expr_;

  // Determines if a string describing the expression is being generated instead of evaluating
  // the expression. See is_string_output() and AppendString().
  StringOutput string_output_mode_ = StringOutput::kNone;
  std::string string_output_;  // Result when string_output_mode_ != kNone;

  CompletionCallback completion_callback_;  // Null in string printing mode (it's synchronous).
  bool in_completion_callback_ = false;     // To check for lifetime errors.

  DataExtractor data_extractor_;

  // The result type. Normally expressions compute pointers unless explicitly tagged as a value.
  // This tracks the current "simple" expression result type. For "composite" operations that
  // use one or more DW_OP_[bit_]piece there will be nonempty result_data_ rather than writing
  // "kData" here.
  //
  // This needs to be separate because there can be multiple simple expressions independent of the
  // result_data_ in the composite case. So this value will never be "kData".
  ResultType result_type_ = ResultType::kPointer;

  // Indicates that execution is complete. When this is true, the callback will have been issued. A
  // complete expression could have stopped on error or success (see is_success_).
  bool is_complete_ = false;

  // Indicates that the expression is complete and that there is a result value.
  bool is_success_ = false;

  std::vector<StackEntry> stack_;

  // Tracks the result when generating composite descriptions via DW_OP_[bit_]piece. A nonempty
  // contents indicates that the final result is of type "kData" (see result_type_ for more).
  //
  // TODO(bug 39630) we will need to track source information (memory address or register ID) for
  // each subrange in this block to support writing to the generated object.
  TaggedDataBuilder result_data_;

  // Set when a register value is pushed on the stack and cleared when anything else happens. This
  // allows the user of the expression to determine if the result of the expression is directly from
  // a register (say, to support writing to that value in the future).
  debug::RegisterID current_register_id_ = debug::RegisterID::kUnknown;

  // Tracks whether the current expression uses only constant data. Any operations that read memory
  // or registers should clear this.
  bool result_is_constant_ = true;

  // The nested evaluator for executing DW_OP_entry_value expressions.
  std::unique_ptr<DwarfExprEval> nested_eval_;

  fxl::WeakPtrFactory<DwarfExprEval> weak_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DwarfExprEval);
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_EXPR_EVAL_H_
