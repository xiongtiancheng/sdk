#include "vm/compiler/backend/llvm/ir_translator.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include <unordered_map>

#include "vm/compiler/aot/precompiler.h"
#include "vm/compiler/assembler/object_pool_builder.h"
#include "vm/compiler/backend/il_printer.h"
#include "vm/compiler/backend/llvm/compiler_state.h"
#include "vm/compiler/backend/llvm/initialize_llvm.h"
#include "vm/compiler/backend/llvm/liveness_analysis.h"
#include "vm/compiler/backend/llvm/output.h"
#include "vm/compiler/backend/llvm/stack_map_info.h"
#include "vm/compiler/backend/llvm/target_specific.h"
#include "vm/compiler/runtime_api.h"

namespace dart {
namespace dart_llvm {
namespace {
class AnonImpl;
struct NotMergedPhiDesc {
  BlockEntryInstr* pred;
  int value;
  LValue phi;
};

struct GCRelocateDesc {
  int ssa_index;
  int where;
  GCRelocateDesc(int _ssa_index, int w) : ssa_index(_ssa_index), where(w) {}
};

using GCRelocateDescList = std::vector<GCRelocateDesc>;

enum class ValueType { LLVMValue };

struct ValueDesc {
  ValueType type;
  Definition* def;
  union {
    LValue llvm_value;
    int64_t relative_call_target;
  };
};

using ExceptionBlockLiveInValueMap = std::unordered_map<int, ValueDesc>;

struct ExceptionBlockLiveInEntry {
  ExceptionBlockLiveInEntry(ExceptionBlockLiveInValueMap&& _live_in_value_map,
                            LBasicBlock _from_block);
  ExceptionBlockLiveInValueMap live_in_value_map;
  LBasicBlock from_block;
};

struct IRTranslatorBlockImpl {
  std::vector<NotMergedPhiDesc> not_merged_phis;
  std::unordered_map<int, ValueDesc> values_;
  // values for exception block.
  std::unordered_map<int, ValueDesc> exception_values_;

  LBasicBlock native_bb = nullptr;
  LBasicBlock continuation = nullptr;
  // exception vals
  LValue exception_val = nullptr;
  LValue stacktrace_val = nullptr;
  std::vector<ExceptionBlockLiveInEntry> exception_live_in_entries;

  AnonImpl* anon_impl;
  int try_index = kInvalidTryIndex;

  bool started = false;
  bool ended = false;
  bool need_merge = false;
  bool exception_block = false;

  inline void SetLLVMValue(int ssa_id, LValue value) {
    values_[ssa_id] = {ValueType::LLVMValue, nullptr, {value}};
  }

  inline void SetLLVMValue(Definition* d, LValue value) {
    values_[d->ssa_temp_index()] = {ValueType::LLVMValue, nullptr, {value}};
  }

  inline void SetValue(int nid, const ValueDesc& value) {
    values_[nid] = value;
  }

  LValue GetLLVMValue(int ssa_id);

  LValue GetLLVMValue(Value* v) {
    return GetLLVMValue(v->definition()->ssa_temp_index());
  }

  bool IsDefined(Definition* def) {
    auto found = values_.find(def->ssa_temp_index());
    return found != values_.end();
  }

#if 0
  LValue GetLLVMValue(Definition* d) {
    return GetLLVMValue(d->ssa_temp_index());
  }
#endif

  const ValueDesc& GetValue(int nid) const {
    auto found = values_.find(nid);
    EMASSERT(found != values_.end());
    return found->second;
  }

  const ValueDesc& GetValue(Value* v) const {
    return GetValue(v->definition()->ssa_temp_index());
  }

  inline std::unordered_map<int, ValueDesc>& values() { return values_; }

  inline void StartTranslate() {
    EMASSERT(!started);
    EMASSERT(!ended);
    started = true;
  }

  inline void EndTranslate() {
    EMASSERT(started);
    EMASSERT(!ended);
    ended = true;
  }
};

class AnonImpl {
 public:
  LBasicBlock GetNativeBB(BlockEntryInstr* bb);
  LBasicBlock EnsureNativeBB(BlockEntryInstr* bb);
  IRTranslatorBlockImpl* GetTranslatorBlockImpl(BlockEntryInstr* bb);
  LBasicBlock GetNativeBBContinuation(BlockEntryInstr* bb);
  bool IsBBStartedToTranslate(BlockEntryInstr* bb);
  bool IsBBEndedToTranslate(BlockEntryInstr* bb);
  void StartTranslate(BlockEntryInstr* bb);
  void MergePredecessors(BlockEntryInstr* bb);
  void MergeExceptionVirtualPredecessors(BlockEntryInstr* bb);
  // When called, the current builder position must point to the end of catch block's native bb.
  void MergeExceptionLiveInEntries(const ExceptionBlockLiveInEntry& e,
                                   BlockEntryInstr* catch_block,
                                   IRTranslatorBlockImpl* catch_block_impl);
  bool AllPredecessorStarted(BlockEntryInstr* bb, BlockEntryInstr** ref_pred);
  void End();
  void EndCurrentBlock();
  void ProcessPhiWorkList();
  void BuildPhiAndPushToWorkList(BlockEntryInstr* bb,
                                 BlockEntryInstr* ref_pred);
  // MaterializeDef
  void MaterializeDef(ValueDesc& desc);
  // Phis
  LValue EnsurePhiInput(BlockEntryInstr* pred, int index, LType type);
  LValue EnsurePhiInputAndPosition(BlockEntryInstr* pred,
                                   int index,
                                   LType type);
  // Exception handling
  LBasicBlock EnsureNativeCatchBlock(int try_index);
  IRTranslatorBlockImpl* GetCatchBlockImplInitIfNeeded(
      CatchBlockEntryInstr* instr);
  // Debug & line info.
  void SetDebugLine(Instruction*);
  // Access to memory.
  LValue BuildAccessPointer(LValue base, LValue offset, Representation rep);
  LValue BuildAccessPointer(LValue base, LValue offset, LType type);
  // Calls
  int NextPatchPoint();
  void SubmitStackMap(std::unique_ptr<StackMapInfo> info);
  void PushArgument(LValue v);
  LValue GenerateRuntimeCall(Instruction*,
                             TokenPosition token_pos,
                             intptr_t deopt_id,
                             const RuntimeEntry& entry,
                             intptr_t argument_count,
                             LocationSummary* locs);
  LValue GenerateStaticCall(Instruction*,
                            int ssa_index,
                            const std::vector<LValue>& arguments,
                            intptr_t deopt_id,
                            TokenPosition token_pos,
                            const Function& function,
                            ArgumentsInfo args_info,
                            LocationSummary* locs,
                            const ICData& ic_data_in,
                            Code::EntryKind entry_kind);
  // Types
  LType GetMachineRepresentationType(Representation);
  LValue EnsureBoolean(LValue v);
  LValue EnsureIntPtr(LValue v);
  LValue EnsureInt32(LValue v);
  LValue SmiTag(LValue v);
  LValue SmiUntag(LValue v);
  LValue TstSmi(LValue v);

  // Value helpers for current bb
  LValue GetLLVMValue(int ssa_id);

  LValue GetLLVMValue(Value* v) {
    return GetLLVMValue(v->definition()->ssa_temp_index());
  }
  LValue GetLLVMValue(Definition* d) {
    return GetLLVMValue(d->ssa_temp_index());
  }
  void SetLLVMValue(int ssa_id, LValue v);
  void SetLLVMValue(Definition* d, LValue v);
  void SetLazyValue(Definition*);

  // Load Store
  compiler::ObjectPoolBuilder& object_pool_builder() {
    return *precompiler_->global_object_pool_builder();
  }
  LValue LoadFieldFromOffset(LValue base, int offset, LType type);
  LValue LoadFieldFromOffset(LValue base, int offset);
  LValue LoadFromOffset(LValue base, int offset, LType type);
  LValue LoadObject(const Object& object, bool is_unique = false);

  void StoreToOffset(LValue base, int offset, LValue v);
  void StoreToOffset(LValue base, LValue offset, LValue v);

  // Basic blocks continuation
  LBasicBlock NewBasicBlock(const char* fmt, ...);
  void SetCurrentBlockContinuation(LBasicBlock continuation);

  inline CompilerState& compiler_state() { return *compiler_state_; }
  inline LivenessAnalysis& liveness() { return *liveness_analysis_; }
  inline Output& output() { return *output_; }
  inline IRTranslatorBlockImpl* current_bb_impl() { return current_bb_impl_; }
  inline BlockEntryInstr* current_bb() { return current_bb_; }
  inline FlowGraph* flow_graph() { return flow_graph_; }

  BlockEntryInstr* current_bb_;
  IRTranslatorBlockImpl* current_bb_impl_;
  FlowGraph* flow_graph_;
  Precompiler* precompiler_;
  std::unique_ptr<CompilerState> compiler_state_;
  std::unique_ptr<LivenessAnalysis> liveness_analysis_;
  std::unique_ptr<Output> output_;
  std::unordered_map<BlockEntryInstr*, IRTranslatorBlockImpl> block_map_;
  StackMapInfoMap stack_map_info_map;
  std::vector<BlockEntryInstr*> phi_rebuild_worklist_;
  std::vector<LBasicBlock> catch_blocks_;

  // debug lines
  std::vector<Instruction*> debug_instrs_;
  // Calls
  std::vector<LValue> pushed_arguments_;
  int patch_point_id_ = 0;
  bool visited_function_entry_ = false;
};

class ContinuationResolver {
 protected:
  ContinuationResolver(AnonImpl& impl, int ssa_id);
  ~ContinuationResolver() = default;
  inline AnonImpl& impl() { return impl_; }
  inline Output& output() { return impl().output(); }
  inline BlockEntryInstr* current_bb() { return impl().current_bb(); }
  inline int ssa_id() const { return ssa_id_; }

 private:
  AnonImpl& impl_;
  int ssa_id_;
};

class DiamondContinuationResolver : public ContinuationResolver {
 public:
  DiamondContinuationResolver(AnonImpl& impl, int ssa_id);
  ~DiamondContinuationResolver() = default;
  DiamondContinuationResolver& BuildCmp(std::function<LValue()>);
  DiamondContinuationResolver& BuildLeft(std::function<LValue()>);
  DiamondContinuationResolver& BuildRight(std::function<LValue()>);
  LValue End();

 private:
  LBasicBlock blocks_[3];
  LValue values_[2];
};

class CallResolver : public ContinuationResolver {
 public:
  struct CallResolverParameter {
    Instruction* call_instruction;
    size_t instruction_size;
    std::unique_ptr<CallSiteInfo> callsite_info;
  };
  explicit CallResolver(AnonImpl& impl,
                        int ssa_id,
                        CallResolverParameter& call_resolver_parameter);
  ~CallResolver() = default;
  void SetGParameter(int reg, LValue);
  void AddStackParameter(LValue);
  LValue GetStackParameter(size_t i);
  LValue BuildCall();

 private:
  void GenerateStatePointFunction();
  void ExtractCallInfo();
  void EmitCall();
  void EmitRelocatesIfNeeded();
  void EmitExceptionBlockIfNeeded();
  void EmitPatchPoint();
  bool need_invoke() { return tail_call_ == false && catch_block_ != nullptr; }

  CallResolverParameter& call_resolver_parameter_;

  // state in the middle.
  LValue statepoint_function_ = nullptr;
  LType callee_type_ = nullptr;
  LValue call_value_ = nullptr;
  LValue statepoint_value_ = nullptr;
  // exception blocks
  IRTranslatorBlockImpl* catch_block_impl_ = nullptr;
  LBasicBlock continuation_block_ = nullptr;
  CatchBlockEntryInstr* catch_block_ = nullptr;
  BitVector* call_live_out_ = nullptr;
  GCRelocateDescList gc_desc_list_;
  ExceptionBlockLiveInValueMap exception_block_live_in_value_map_;
  std::vector<LValue> parameters_;
  int patchid_ = 0;
  bool tail_call_ = false;
};

ExceptionBlockLiveInEntry::ExceptionBlockLiveInEntry(
    ExceptionBlockLiveInValueMap&& _live_in_value_map,
    LBasicBlock _from_block)
    : live_in_value_map(std::move(_live_in_value_map)),
      from_block(_from_block) {}

LValue IRTranslatorBlockImpl::GetLLVMValue(int ssa_id) {
  auto found = values_.find(ssa_id);
  EMASSERT(found != values_.end());
  EMASSERT(found->second.type == ValueType::LLVMValue);
  if (!found->second.llvm_value) {
    // materialize the llvm_value now!
    anon_impl->MaterializeDef(found->second);
  }
  EMASSERT(found->second.llvm_value);
  return found->second.llvm_value;
}

IRTranslatorBlockImpl* AnonImpl::GetTranslatorBlockImpl(BlockEntryInstr* bb) {
  auto& ref = block_map_[bb];
  return &ref;
}

LBasicBlock AnonImpl::EnsureNativeBB(BlockEntryInstr* bb) {
  IRTranslatorBlockImpl* impl = GetTranslatorBlockImpl(bb);
  if (impl->native_bb) return impl->native_bb;
  char buf[256];
  snprintf(buf, 256, "B%d", static_cast<int>(bb->block_id()));
  LBasicBlock native_bb = output_->appendBasicBlock(buf);
  impl->native_bb = native_bb;
  impl->continuation = native_bb;
  return native_bb;
}

LBasicBlock AnonImpl::GetNativeBB(BlockEntryInstr* bb) {
  return GetTranslatorBlockImpl(bb)->native_bb;
}

LBasicBlock AnonImpl::GetNativeBBContinuation(BlockEntryInstr* bb) {
  return GetTranslatorBlockImpl(bb)->continuation;
}

bool AnonImpl::IsBBStartedToTranslate(BlockEntryInstr* bb) {
  auto impl = GetTranslatorBlockImpl(bb);
  return impl->started;
}

bool AnonImpl::IsBBEndedToTranslate(BlockEntryInstr* bb) {
  auto impl = GetTranslatorBlockImpl(bb);
  return impl->ended;
}

void AnonImpl::StartTranslate(BlockEntryInstr* bb) {
  current_bb_ = bb;
  current_bb_impl_ = GetTranslatorBlockImpl(bb);
  current_bb_impl()->StartTranslate();
  EnsureNativeBB(bb);
  output_->positionToBBEnd(GetNativeBB(bb));
}

void AnonImpl::MergePredecessors(BlockEntryInstr* bb) {
  intptr_t predecessor_count = bb->PredecessorCount();
  if (predecessor_count == 0) return;
  IRTranslatorBlockImpl* block_impl = GetTranslatorBlockImpl(bb);
  EMASSERT(!block_impl->exception_block);
  if (predecessor_count == 1) {
    // Don't use phi if only one predecessor.
    BlockEntryInstr* pred = bb->PredecessorAt(0);
    IRTranslatorBlockImpl* pred_block_impl = GetTranslatorBlockImpl(pred);
    EMASSERT(IsBBStartedToTranslate(pred));
    for (BitVector::Iterator it(liveness().GetLiveInSet(bb)); !it.Done();
         it.Advance()) {
      int live = it.Current();
      auto& value = pred_block_impl->GetValue(live);
      block_impl->SetValue(live, value);
    }
    return;
  }
  BlockEntryInstr* ref_pred = nullptr;
  if (!AllPredecessorStarted(bb, &ref_pred)) {
    EMASSERT(!!ref_pred);
    BuildPhiAndPushToWorkList(bb, ref_pred);
    return;
  }
  // Use phi.
  for (BitVector::Iterator it(liveness().GetLiveInSet(bb)); !it.Done();
       it.Advance()) {
    int live = it.Current();
    auto& value = GetTranslatorBlockImpl(ref_pred)->GetValue(live);
    if (value.type != ValueType::LLVMValue) {
      block_impl->SetValue(live, value);
      continue;
    }
    if (value.def != nullptr) {
      ValueDesc new_value_desc = value;
      // reset the constant.
      new_value_desc.llvm_value = nullptr;
      block_impl->SetValue(live, new_value_desc);
      continue;
    }
    LValue ref_value = value.llvm_value;
    LType ref_type = typeOf(ref_value);
    if (ref_type != output().tagged_type()) {
      // FIXME: Should add EMASSERT that all values are the same.
      block_impl->SetLLVMValue(live, ref_value);
      continue;
    }
    LValue phi = output().buildPhi(ref_type);
    for (intptr_t i = 0; i < predecessor_count; ++i) {
      BlockEntryInstr* pred = bb->PredecessorAt(i);
      LValue value = GetTranslatorBlockImpl(pred)->GetLLVMValue(live);
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &native, 1);
    }
    block_impl->SetLLVMValue(live, phi);
  }
}

void AnonImpl::MergeExceptionVirtualPredecessors(BlockEntryInstr* bb) {
  IRTranslatorBlockImpl* block_impl = current_bb_impl();
  LValue landing_pad = output().buildLandingPad();
  for (auto& e : block_impl->exception_live_in_entries) {
    MergeExceptionLiveInEntries(e, current_bb(), block_impl);
  }
  block_impl->exception_live_in_entries.clear();
  block_impl->exception_val =
      output().buildCall(output().repo().gcExceptionIntrinsic(), landing_pad);
  block_impl->stacktrace_val = output().buildCall(
      output().repo().gcExceptionDataIntrinsic(), landing_pad);
}

void AnonImpl::MergeExceptionLiveInEntries(
    const ExceptionBlockLiveInEntry& e,
    BlockEntryInstr* catch_block,
    IRTranslatorBlockImpl* catch_block_impl) {
  auto& values = catch_block_impl->values();
  auto MergeNotExist = [&](int ssa_id, const ValueDesc& value_desc_incoming) {
    if (value_desc_incoming.type != ValueType::LLVMValue ||
        value_desc_incoming.def) {
      values.emplace(ssa_id, value_desc_incoming);
    } else {
      LValue phi = output().buildPhi(typeOf(value_desc_incoming.llvm_value));
      addIncoming(phi, &value_desc_incoming.llvm_value, &e.from_block, 1);
      ValueDesc value_desc{ValueType::LLVMValue, nullptr, {phi}};
      values.emplace(ssa_id, value_desc);
    }
  };
  auto MergeExist = [&](int ssa_id, ValueDesc& value_desc,
                        const ValueDesc& value_desc_incoming) {
    if (value_desc_incoming.type != ValueType::LLVMValue ||
        value_desc_incoming.def) {
      EMASSERT(value_desc_incoming.def == value_desc.def);
      EMASSERT(value_desc_incoming.type == value_desc.type);
    } else {
      LValue phi = value_desc.llvm_value;
      EMASSERT(typeOf(phi) == typeOf(value_desc_incoming.llvm_value));
      addIncoming(phi, &value_desc_incoming.llvm_value, &e.from_block, 1);
    }
  };
  for (BitVector::Iterator it(liveness().GetLiveInSet(catch_block)); !it.Done();
       it.Advance()) {
    int live_ssa_index = it.Current();
    auto found_incoming = e.live_in_value_map.find(live_ssa_index);
    EMASSERT(found_incoming != e.live_in_value_map.end());
    auto& value_desc_incoming = found_incoming->second;
    auto found = values.find(live_ssa_index);
    if (found == values.end()) {
      MergeNotExist(live_ssa_index, value_desc_incoming);
    } else {
      MergeExist(live_ssa_index, found->second, value_desc_incoming);
    }
  }
}

bool AnonImpl::AllPredecessorStarted(BlockEntryInstr* bb,
                                     BlockEntryInstr** ref_pred) {
  bool ret_value = true;
  intptr_t predecessor_count = bb->PredecessorCount();
  for (intptr_t i = 0; i < predecessor_count; ++i) {
    BlockEntryInstr* pred = bb->PredecessorAt(i);
    if (IsBBStartedToTranslate(pred)) {
      if (!*ref_pred) *ref_pred = pred;
    } else {
      ret_value = false;
    }
  }
  return ret_value;
}

void AnonImpl::BuildPhiAndPushToWorkList(BlockEntryInstr* bb,
                                         BlockEntryInstr* ref_pred) {
  IRTranslatorBlockImpl* block_impl = GetTranslatorBlockImpl(bb);
  IRTranslatorBlockImpl* ref_pred_impl = GetTranslatorBlockImpl(ref_pred);
  for (BitVector::Iterator it(liveness().GetLiveInSet(bb)); !it.Done();
       it.Advance()) {
    int live = it.Current();
    const ValueDesc& value_desc = ref_pred_impl->GetValue(live);
    if (value_desc.type != ValueType::LLVMValue) {
      block_impl->SetValue(live, value_desc);
      continue;
    }
    if (value_desc.def != nullptr) {
      ValueDesc new_value_desc = value_desc;
      // reset the constant.
      new_value_desc.llvm_value = nullptr;
      block_impl->SetValue(live, new_value_desc);
      continue;
    }
    LValue ref_value = value_desc.llvm_value;
    LType ref_type = typeOf(ref_value);
    if (ref_type != output().tagged_type()) {
      block_impl->SetLLVMValue(live, ref_value);
      continue;
    }
    LValue phi = output().buildPhi(ref_type);
    block_impl->SetLLVMValue(live, phi);
    intptr_t predecessor_count = bb->PredecessorCount();
    for (intptr_t i = 0; i < predecessor_count; ++i) {
      BlockEntryInstr* pred = bb->PredecessorAt(i);
      if (!IsBBStartedToTranslate(pred)) {
        block_impl->not_merged_phis.emplace_back();
        NotMergedPhiDesc& not_merged_phi = block_impl->not_merged_phis.back();
        not_merged_phi.phi = phi;
        not_merged_phi.value = live;
        not_merged_phi.pred = pred;
        continue;
      }
      LValue value = GetTranslatorBlockImpl(pred)->GetLLVMValue(live);
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &native, 1);
    }
  }
  phi_rebuild_worklist_.push_back(bb);
}

void AnonImpl::End() {
  EMASSERT(!!current_bb_);
  EndCurrentBlock();
  ProcessPhiWorkList();
  output().positionToBBEnd(output().prologue());
  output().buildBr(GetNativeBB(flow_graph_->graph_entry()));
  output().finalize();
}

void AnonImpl::EndCurrentBlock() {
  GetTranslatorBlockImpl(current_bb_)->EndTranslate();
  current_bb_ = nullptr;
  current_bb_impl_ = nullptr;
  pushed_arguments_.clear();
}

void AnonImpl::ProcessPhiWorkList() {
  for (BlockEntryInstr* bb : phi_rebuild_worklist_) {
    auto impl = GetTranslatorBlockImpl(bb);
    for (auto& e : impl->not_merged_phis) {
      BlockEntryInstr* pred = e.pred;
      EMASSERT(IsBBStartedToTranslate(pred));
      LValue value = EnsurePhiInput(pred, e.value, typeOf(e.phi));
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(e.phi, &value, &native, 1);
    }
    impl->not_merged_phis.clear();
  }
  phi_rebuild_worklist_.clear();
}

LValue AnonImpl::EnsurePhiInput(BlockEntryInstr* pred, int index, LType type) {
  LValue val = GetTranslatorBlockImpl(pred)->GetLLVMValue(index);
  LType value_type = typeOf(val);
  if (value_type == type) return val;
  LValue terminator =
      LLVMGetBasicBlockTerminator(GetNativeBBContinuation(pred));
  if ((value_type == output().repo().intPtr) &&
      (type == output().tagged_type())) {
    output().positionBefore(terminator);
    LValue ret_val =
        output().buildCast(LLVMIntToPtr, val, output().tagged_type());
    return ret_val;
  }
  LLVMTypeKind value_type_kind = LLVMGetTypeKind(value_type);
  if ((LLVMPointerTypeKind == value_type_kind) &&
      (type == output().repo().intPtr)) {
    output().positionBefore(terminator);
    LValue ret_val =
        output().buildCast(LLVMPtrToInt, val, output().repo().intPtr);
    return ret_val;
  }
  if ((value_type == output().repo().boolean) &&
      (type == output().repo().intPtr)) {
    output().positionBefore(terminator);
    LValue ret_val = output().buildCast(LLVMZExt, val, output().repo().intPtr);
    return ret_val;
  }
  LLVMTypeKind type_kind = LLVMGetTypeKind(type);
  if ((LLVMIntegerTypeKind == value_type_kind) &&
      (value_type_kind == type_kind)) {
    // handle both integer
    output().positionBefore(terminator);
    LValue ret_val;
    if (LLVMGetIntTypeWidth(value_type) > LLVMGetIntTypeWidth(type)) {
      ret_val = output().buildCast(LLVMTrunc, val, type);
    } else {
      ret_val = output().buildCast(LLVMZExt, val, type);
    }
    return ret_val;
  }
  __builtin_trap();
}

LValue AnonImpl::EnsurePhiInputAndPosition(BlockEntryInstr* pred,
                                           int index,
                                           LType type) {
  LValue value = EnsurePhiInput(pred, index, type);
  output().positionToBBEnd(GetNativeBB(current_bb_));
  return value;
}

void AnonImpl::MaterializeDef(ValueDesc& desc) {
  EMASSERT(desc.def != nullptr);
  Definition* def = desc.def;
  LValue v;
  if (UnboxedConstantInstr* unboxed = def->AsUnboxedConstant()) {
    const Object& object = unboxed->value();
    if (object.IsDouble()) {
      const Double& n = Double::Cast(object);
      v = output().constDouble(n.value());
    } else if (object.IsSmi()) {
      v = output().constTagged(compiler::target::ToRawSmi(object));
    } else {
      LLVMLOGE(
          "MaterializeDef: UnboxedConstantInstr failed to interpret: unknown "
          "type: %s",
          object.ToCString());
      UNREACHABLE();
    }
  } else if (ConstantInstr* constant_object = def->AsConstant()) {
    v = LoadObject(constant_object->value());
  } else {
    LLVMLOGE("MaterializeDef: unknown def: %s\n", def->ToCString());
    UNREACHABLE();
  }
  desc.llvm_value = v;
}

LBasicBlock AnonImpl::EnsureNativeCatchBlock(int try_index) {
  char buf[256];
  LBasicBlock native_bb;
  if ((native_bb = catch_blocks_[try_index]) == nullptr) {
    snprintf(buf, 256, "B_catch_block_%d", try_index);
    native_bb = output().appendBasicBlock(buf);
    catch_blocks_[try_index] = native_bb;
  }
  return native_bb;
}

IRTranslatorBlockImpl* AnonImpl::GetCatchBlockImplInitIfNeeded(
    CatchBlockEntryInstr* instr) {
  IRTranslatorBlockImpl* block_impl = GetTranslatorBlockImpl(instr);
  if (block_impl->native_bb) return block_impl;
  LBasicBlock native_bb = EnsureNativeCatchBlock(instr->catch_try_index());
  block_impl->native_bb = native_bb;
  block_impl->exception_block = true;
  return block_impl;
}

void AnonImpl::SetDebugLine(Instruction* instr) {
  debug_instrs_.emplace_back(instr);
  output().setDebugInfo(debug_instrs_.size(), nullptr);
}

LValue AnonImpl::BuildAccessPointer(LValue base,
                                    LValue offset,
                                    Representation rep) {
  return BuildAccessPointer(base, offset,
                            pointerType(GetMachineRepresentationType(rep)));
}

LValue AnonImpl::BuildAccessPointer(LValue base, LValue offset, LType type) {
  LLVMTypeKind kind = LLVMGetTypeKind(typeOf(base));
  if (kind == LLVMIntegerTypeKind) {
    base = output().buildCast(LLVMIntToPtr, base, output().repo().ref8);
  }
  // For ElementOffsetFromIndex ignores BitcastTaggedToWord.
  if (typeOf(offset) == output().tagged_type()) {
    UNREACHABLE();
    // offset = output().buildCast(LLVMPtrToInt, offset, output().repo().intPtr);
  }
  LValue pointer = output().buildGEPWithByteOffset(base, offset, type);
  return pointer;
}

int AnonImpl::NextPatchPoint() {
  return patch_point_id_++;
}

void AnonImpl::SubmitStackMap(std::unique_ptr<StackMapInfo> info) {
  int patchid = info->patchid();
  auto where = stack_map_info_map.emplace(patchid, std::move(info));
  EMASSERT(where.second && "Submit overlapped patch id");
}

void AnonImpl::PushArgument(LValue v) {
  pushed_arguments_.emplace_back(v);
}

LValue AnonImpl::GenerateRuntimeCall(Instruction* instr,
                                     TokenPosition token_pos,
                                     intptr_t deopt_id,
                                     const RuntimeEntry& runtime_entry,
                                     intptr_t argument_count,
                                     LocationSummary* locs) {
  EMASSERT(runtime_entry.is_leaf());
  LValue code_object =
      LoadFromOffset(output().thread(),
                     compiler::target::Thread::OffsetFromThread(&runtime_entry),
                     pointerType(output().tagged_type()));
  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  callsite_info->set_type(CallSiteInfo::CallTargetType::kCodeObject);
  callsite_info->set_token_pos(token_pos);
  callsite_info->set_deopt_id(deopt_id);
  callsite_info->set_locs(locs);
  callsite_info->set_stack_parameter_count(argument_count);
  CallResolver::CallResolverParameter param = {instr, Instr::kInstrSize,
                                               std::move(callsite_info)};
  CallResolver resolver(*this, -1, param);
  resolver.SetGParameter(static_cast<int>(CODE_REG), code_object);
  // add stack parameters.
  EMASSERT(static_cast<intptr_t>(pushed_arguments_.size()) >= argument_count);
  for (intptr_t i = 0; i < argument_count; ++i) {
    LValue argument = pushed_arguments_.back();
    pushed_arguments_.pop_back();
    resolver.AddStackParameter(argument);
  }
  return resolver.BuildCall();
}

LValue AnonImpl::GenerateStaticCall(Instruction* instr,
                                    int ssa_index,
                                    const std::vector<LValue>& arguments,
                                    intptr_t deopt_id,
                                    TokenPosition token_pos,
                                    const Function& function,
                                    ArgumentsInfo args_info,
                                    LocationSummary* locs,
                                    const ICData& ic_data_in,
                                    Code::EntryKind entry_kind) {
  const ICData& ic_data = ICData::ZoneHandle(ic_data_in.Original());
  const Array& arguments_descriptor = Array::ZoneHandle(
      flow_graph()->zone(), ic_data.IsNull() ? args_info.ToArgumentsDescriptor()
                                             : ic_data.arguments_descriptor());
  LValue argument_desc_val = LLVMGetUndef(output().tagged_type());
  if (function.HasOptionalParameters() || function.IsGeneric()) {
    argument_desc_val = LoadObject(arguments_descriptor);
  } else {
    if (!(FLAG_precompiled_mode && FLAG_use_bare_instructions)) {
      argument_desc_val = output().constTagged(0);
    }
  }
  size_t argument_count = args_info.count_with_type_args;
  EMASSERT(argument_count == arguments.size());

  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  callsite_info->set_type(CallSiteInfo::CallTargetType::kCodeRelative);
  callsite_info->set_token_pos(token_pos);
  callsite_info->set_deopt_id(deopt_id);
  callsite_info->set_locs(locs);
  callsite_info->set_stack_parameter_count(argument_count);
  callsite_info->set_target(&function);
  callsite_info->set_entry_kind(entry_kind);
  CallResolver::CallResolverParameter param = {instr, Instr::kInstrSize,
                                               std::move(callsite_info)};
  CallResolver resolver(*this, ssa_index, param);
  resolver.SetGParameter(static_cast<int>(ARGS_DESC_REG), argument_desc_val);
  // setup stack parameters
  for (size_t i = argument_count - 1; i >= 0; --i) {
    LValue param = arguments[i];
    resolver.AddStackParameter(param);
  }
  return resolver.BuildCall();
}

LType AnonImpl::GetMachineRepresentationType(Representation rep) {
  LType type;
  switch (rep) {
    case kTagged:
      type = output().tagged_type();
      break;
    case kUnboxedDouble:
      type = output().repo().doubleType;
      break;
    case kUnboxedFloat:
      type = output().repo().floatType;
      break;
    case kUnboxedInt32:
    case kUnboxedUint32:
      type = output().repo().int32;
      break;
    case kUnboxedInt64:
      type = output().repo().int64;
      break;
    default:
      UNREACHABLE();
  }
  return type;
}

LValue AnonImpl::EnsureBoolean(LValue v) {
  LType type = typeOf(v);
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  if (kind == LLVMPointerTypeKind)
    v = output().buildCast(LLVMPtrToInt, v, output().repo().intPtr);
  type = typeOf(v);
  if (LLVMGetIntTypeWidth(type) == 1) return v;
  v = output().buildICmp(LLVMIntNE, v, output().repo().intPtrZero);
  return v;
}

LValue AnonImpl::EnsureIntPtr(LValue v) {
  LType type = typeOf(v);
  if (type == output().repo().intPtr) return v;
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  if (kind == LLVMPointerTypeKind)
    return output().buildCast(LLVMZExt, v, output().repo().intPtr);
  return v;
}

LValue AnonImpl::EnsureInt32(LValue v) {
  LType type = typeOf(v);
  if (type == output().repo().int32) return v;
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  if (kind == LLVMPointerTypeKind) {
    return output().buildCast(LLVMPtrToInt, v, output().repo().int32);
  }
  if (type == output().repo().boolean) {
    return output().buildCast(LLVMZExt, v, output().repo().int32);
  }
  EMASSERT(type == output().repo().int32);
  return v;
}

LValue AnonImpl::SmiTag(LValue v) {
  EMASSERT(typeOf(v) == output().repo().intPtr);
  v = output().buildShl(v, output().constIntPtr(kSmiTagSize));
  return output().buildCast(LLVMIntToPtr, v, output().tagged_type());
}

LValue AnonImpl::SmiUntag(LValue v) {
  EMASSERT(typeOf(v) == output().tagged_type());
  v = output().buildCast(LLVMPtrToInt, v, output().repo().intPtr);
  return output().buildShr(v, output().constIntPtr(kSmiTagSize));
}

LValue AnonImpl::TstSmi(LValue v) {
  EMASSERT(typeOf(v) == output().tagged_type());
  LValue address = output().buildCast(LLVMPtrToInt, v, output().repo().intPtr);
  LValue and_value =
      output().buildAnd(address, output().constIntPtr(kSmiTagMask));
  return output().buildICmp(LLVMIntNE, and_value,
                            output().constIntPtr(kSmiTagMask));
}

LValue AnonImpl::GetLLVMValue(int ssa_id) {
  return current_bb_impl()->GetLLVMValue(ssa_id);
}

void AnonImpl::SetLLVMValue(int ssa_id, LValue val) {
  current_bb_impl()->SetLLVMValue(ssa_id, val);
}

void AnonImpl::SetLLVMValue(Definition* d, LValue val) {
  current_bb_impl()->SetLLVMValue(d, val);
}

void AnonImpl::SetLazyValue(Definition* d) {
  ValueDesc desc = {ValueType::LLVMValue, d, {nullptr}};
  current_bb_impl()->SetValue(d->ssa_temp_index(), desc);
}

LValue AnonImpl::LoadFieldFromOffset(LValue base, int offset, LType type) {
  LValue gep = output().buildGEPWithByteOffset(
      base, output().constIntPtr(offset - kHeapObjectTag), type);
  return output().buildLoad(gep);
}

LValue AnonImpl::LoadFieldFromOffset(LValue base, int offset) {
  return LoadFieldFromOffset(base, offset, pointerType(output().tagged_type()));
}

LValue AnonImpl::LoadFromOffset(LValue base, int offset, LType type) {
  LValue gep =
      output().buildGEPWithByteOffset(base, output().constIntPtr(offset), type);
  return output().buildLoad(gep);
}

LValue AnonImpl::LoadObject(const Object& object, bool is_unique) {
  intptr_t offset = 0;
  if (compiler::target::CanLoadFromThread(object, &offset)) {
    // Load common VM constants from the thread. This works also in places where
    // no constant pool is set up (e.g. intrinsic code).
    LValue gep = output().buildGEPWithByteOffset(
        output().thread(), output().constIntPtr(offset),
        pointerType(output().tagged_type()));
    return output().buildLoad(gep);
  } else if (compiler::target::IsSmi(object)) {
    // Relocation doesn't apply to Smis.
    return output().constIntPtr(
        static_cast<intptr_t>(compiler::target::ToRawSmi(object)));
  } else {
    // Make sure that class CallPattern is able to decode this load from the
    // object pool.
    EMASSERT(FLAG_use_bare_instructions);
    const auto index = is_unique ? object_pool_builder().AddObject(object)
                                 : object_pool_builder().FindObject(object);
    const int32_t offset = compiler::target::ObjectPool::element_offset(index);
    LValue gep = output().buildGEPWithByteOffset(
        output().pp(), output().constIntPtr(offset - kHeapObjectTag),
        pointerType(output().tagged_type()));
    return output().buildLoad(gep);
  }
}

void AnonImpl::StoreToOffset(LValue base, int offset, LValue v) {
  StoreToOffset(base, output().constIntPtr(offset), v);
}

void AnonImpl::StoreToOffset(LValue base, LValue offset, LValue v) {
  LValue gep =
      output().buildGEPWithByteOffset(base, offset, pointerType(typeOf(v)));
  output().buildStore(v, gep);
}

LBasicBlock AnonImpl::NewBasicBlock(const char* fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char buf[256];
  vsnprintf(buf, 256, fmt, va);
  va_end(va);
  LBasicBlock bb = output().appendBasicBlock(buf);
  return bb;
}

void AnonImpl::SetCurrentBlockContinuation(LBasicBlock continuation) {
  current_bb_impl()->continuation = continuation;
}

ContinuationResolver::ContinuationResolver(AnonImpl& impl, int ssa_id)
    : impl_(impl), ssa_id_(ssa_id) {}

DiamondContinuationResolver::DiamondContinuationResolver(AnonImpl& _impl,
                                                         int ssa_id)
    : ContinuationResolver(_impl, ssa_id) {
  blocks_[0] = impl().NewBasicBlock("left_for_%d", ssa_id);
  blocks_[1] = impl().NewBasicBlock("right_for_%d", ssa_id);
  blocks_[2] = impl().NewBasicBlock("continuation_for_%d", ssa_id);
}

DiamondContinuationResolver& DiamondContinuationResolver::BuildCmp(
    std::function<LValue()> f) {
  LValue cmp_value = f();
  output().buildCondBr(cmp_value, blocks_[0], blocks_[1]);
  return *this;
}

DiamondContinuationResolver& DiamondContinuationResolver::BuildLeft(
    std::function<LValue()> f) {
  output().positionToBBEnd(blocks_[0]);
  values_[0] = f();
  output().buildBr(blocks_[2]);
  return *this;
}

DiamondContinuationResolver& DiamondContinuationResolver::BuildRight(
    std::function<LValue()> f) {
  output().positionToBBEnd(blocks_[1]);
  values_[1] = f();
  output().buildBr(blocks_[2]);
  return *this;
}

LValue DiamondContinuationResolver::End() {
  output().positionToBBEnd(blocks_[2]);
  LValue phi = output().buildPhi(typeOf(values_[0]));
  addIncoming(phi, values_, blocks_, 2);
  impl().SetCurrentBlockContinuation(blocks_[2]);
  return phi;
}

CallResolver::CallResolver(
    AnonImpl& impl,
    int ssa_id,
    CallResolver::CallResolverParameter& call_resolver_parameter)
    : ContinuationResolver(impl, ssa_id),
      call_resolver_parameter_(call_resolver_parameter),
      parameters_(kV8CCRegisterParameterCount,
                  LLVMGetUndef(output().tagged_type())) {
  // init parameters_.
  parameters_[static_cast<int>(PP)] = output().pp();
  parameters_[static_cast<int>(THR)] = LLVMGetUndef(output().repo().ref8);
  parameters_[static_cast<int>(FP)] = LLVMGetUndef(output().repo().ref8);
}

void CallResolver::SetGParameter(int reg, LValue val) {
  EMASSERT(reg < kV8CCRegisterParameterCount);
  EMASSERT(reg >= 0);
  parameters_[reg] = val;
}

void CallResolver::AddStackParameter(LValue v) {
  EMASSERT(typeOf(v) == output().tagged_type());
  parameters_.emplace_back(v);
  EMASSERT(parameters_.size() - kV8CCRegisterParameterCount <=
           call_resolver_parameter_.callsite_info->stack_parameter_count());
}

LValue CallResolver::GetStackParameter(size_t i) {
  i += kV8CCRegisterParameterCount;
  EMASSERT(parameters_.size() > i);
  return parameters_[i];
}

LValue CallResolver::BuildCall() {
  EMASSERT(parameters_.size() - kV8CCRegisterParameterCount ==
           call_resolver_parameter_.callsite_info->stack_parameter_count());
  EMASSERT(call_resolver_parameter_.callsite_info->type() !=
               CallSiteInfo::CallTargetType::kCodeObject ||
           !LLVMIsUndef(parameters_[static_cast<int>(CODE_REG)]));
  ExtractCallInfo();
  GenerateStatePointFunction();
  EmitCall();
  EmitExceptionBlockIfNeeded();

  // reset to continuation.
  if (need_invoke()) {
    output().positionToBBEnd(continuation_block_);
    impl().SetCurrentBlockContinuation(continuation_block_);
  }
  EmitRelocatesIfNeeded();
  EmitPatchPoint();
  return call_value_;
}

void CallResolver::ExtractCallInfo() {
  call_live_out_ =
      impl().liveness().GetCallOutAt(call_resolver_parameter_.call_instruction);
  if (impl().current_bb()->try_index() != kInvalidTryIndex) {
    catch_block_ = impl().flow_graph()->graph_entry()->GetCatchEntry(
        impl().current_bb()->try_index());
  }
  if (call_resolver_parameter_.call_instruction->IsTailCall()) {
    tail_call_ = true;
  }
}

void CallResolver::GenerateStatePointFunction() {
  LType return_type = output().tagged_type();
  std::vector<LType> operand_value_types;
  for (LValue param : parameters_) {
    operand_value_types.emplace_back(typeOf(param));
  }
  LType callee_function_type =
      functionType(return_type, operand_value_types.data(),
                   operand_value_types.size(), NotVariadic);
  callee_type_ = pointerType(callee_function_type);

  statepoint_function_ = output().getStatePointFunction(callee_type_);
}

void CallResolver::EmitCall() {
  std::vector<LValue> statepoint_operands;
  patchid_ = impl().NextPatchPoint();
  statepoint_operands.push_back(output().constInt64(patchid_));
  statepoint_operands.push_back(
      output().constInt32(call_resolver_parameter_.instruction_size));
  statepoint_operands.push_back(constNull(callee_type_));
  statepoint_operands.push_back(
      output().constInt32(parameters_.size()));           // # call params
  statepoint_operands.push_back(output().constInt32(0));  // flags
  for (LValue parameter : parameters_)
    statepoint_operands.push_back(parameter);
  statepoint_operands.push_back(output().constInt32(0));  // # transition args
  statepoint_operands.push_back(output().constInt32(0));  // # deopt arguments
  // normal block
  if (!tail_call_) {
    for (BitVector::Iterator it(call_live_out_); !it.Done(); it.Advance()) {
      int live_ssa_index = it.Current();
      ValueDesc desc = impl().current_bb_impl()->GetValue(live_ssa_index);
      if (desc.type != ValueType::LLVMValue) continue;
      if (desc.def != nullptr) {
        // constant values need to rematerialize after call.
        desc.llvm_value = nullptr;
        impl().current_bb_impl()->SetValue(live_ssa_index, desc);
        continue;
      }
      gc_desc_list_.emplace_back(live_ssa_index, statepoint_operands.size());
      statepoint_operands.emplace_back(desc.llvm_value);
    }
  }

  if (need_invoke()) {
    for (BitVector::Iterator it(impl().liveness().GetLiveInSet(catch_block_));
         !it.Done(); it.Advance()) {
      int live_ssa_index = it.Current();
      ValueDesc desc = impl().current_bb_impl()->GetValue(live_ssa_index);
      if (desc.def != nullptr) {
        // constant value save directly, and need to rematerialize.
        desc.llvm_value = nullptr;
      }
      exception_block_live_in_value_map_.emplace(live_ssa_index, desc);
    }
  }

  if (!need_invoke()) {
    statepoint_value_ =
        output().buildCall(statepoint_function_, statepoint_operands.data(),
                           statepoint_operands.size());
    if (tail_call_) output().buildReturnForTailCall();
  } else {
    catch_block_impl_ = impl().GetCatchBlockImplInitIfNeeded(catch_block_);
    continuation_block_ =
        impl().NewBasicBlock("continuation_block_for_%d", ssa_id());
    statepoint_value_ =
        output().buildInvoke(statepoint_function_, statepoint_operands.data(),
                             statepoint_operands.size(), continuation_block_,
                             catch_block_impl_->native_bb);
  }
}

void CallResolver::EmitRelocatesIfNeeded() {
  if (tail_call_) return;
  for (auto& gc_relocate : gc_desc_list_) {
    LValue relocated = output().buildCall(
        output().repo().gcRelocateIntrinsic(), statepoint_value_,
        output().constInt32(gc_relocate.where),
        output().constInt32(gc_relocate.where));
    impl().current_bb_impl()->SetLLVMValue(gc_relocate.ssa_index, relocated);
  }

  LType return_type = output().tagged_type();
  LValue intrinsic = output().getGCResultFunction(return_type);
  call_value_ = output().buildCall(intrinsic, statepoint_value_);
}

void CallResolver::EmitPatchPoint() {
  std::unique_ptr<CallSiteInfo> callsite_info(
      std::move(call_resolver_parameter_.callsite_info));
  callsite_info->set_is_tailcall(tail_call_);
  callsite_info->set_patchid(patchid_);
  callsite_info->set_try_index(impl().current_bb()->try_index());
  impl().SubmitStackMap(std::move(callsite_info));
}

void CallResolver::EmitExceptionBlockIfNeeded() {
  if (!need_invoke()) return;
  if (!impl().IsBBStartedToTranslate(catch_block_)) {
    catch_block_impl_->exception_live_in_entries.emplace_back(
        std::move(exception_block_live_in_value_map_),
        impl().current_bb_impl()->continuation);
  } else {
    ExceptionBlockLiveInEntry e = {
        std::move(exception_block_live_in_value_map_),
        impl().current_bb_impl()->continuation};
    output().positionToBBEnd(catch_block_impl_->native_bb);
    impl().MergeExceptionLiveInEntries(e, catch_block_, catch_block_impl_);
  }
}
}  // namespace

struct IRTranslator::Impl : public AnonImpl {};

IRTranslator::IRTranslator(FlowGraph* flow_graph, Precompiler* precompiler)
    : FlowGraphVisitor(flow_graph->reverse_postorder()) {
  // dynamic function has multiple entry points
  // which LLVM does not suppport.
  InitLLVM();
  impl_.reset(new Impl);
  impl().flow_graph_ = flow_graph;
  impl().precompiler_ = precompiler;
  impl().compiler_state_.reset(new CompilerState(
      String::Handle(flow_graph->zone(),
                     flow_graph->parsed_function().function().UserVisibleName())
          .ToCString()));
  impl().liveness_analysis_.reset(new LivenessAnalysis(flow_graph));
  impl().output_.reset(new Output(impl().compiler_state()));
  // init parameter desc
  RegisterParameterDesc parameter_desc;
  LType tagged_type = output().repo().tagged_type;
  for (int i = flow_graph->num_direct_parameters(); i > 0; --i) {
    parameter_desc.emplace_back(-i, tagged_type);
  }
  // init output().
  output().initializeBuild(parameter_desc);
}

IRTranslator::~IRTranslator() {}

void IRTranslator::Translate() {
#if 0
  impl().liveness().Analyze();
  VisitBlocks();
#endif

  // FlowGraphPrinter::PrintGraph("IRTranslator", impl().flow_graph_);
}

Output& IRTranslator::output() {
  return impl().output();
}

void IRTranslator::VisitBlockEntry(BlockEntryInstr* block) {
  impl().SetDebugLine(block);
  impl().StartTranslate(block);
  IRTranslatorBlockImpl* block_impl = impl().current_bb_impl();
  block_impl->try_index = block->try_index();
  if (!block_impl->exception_block)
    impl().MergePredecessors(block);
  else
    impl().MergeExceptionVirtualPredecessors(block);
}

void IRTranslator::VisitBlockEntryWithInitialDefs(
    BlockEntryWithInitialDefs* block) {
  VisitBlockEntry(block);
  for (Definition* def : *block->initial_definitions()) {
    def->Accept(this);
  }
}

void IRTranslator::VisitGraphEntry(GraphEntryInstr* instr) {
  VisitBlockEntryWithInitialDefs(instr);
}

void IRTranslator::VisitJoinEntry(JoinEntryInstr* instr) {
  VisitBlockEntry(instr);
  for (PhiInstr* phi : *instr->phis()) {
    VisitPhi(phi);
  }
}

void IRTranslator::VisitTargetEntry(TargetEntryInstr* instr) {
  VisitBlockEntry(instr);
}

void IRTranslator::VisitFunctionEntry(FunctionEntryInstr* instr) {
  EMASSERT(!impl().visited_function_entry_);
  impl().visited_function_entry_ = true;
  LBasicBlock bb = impl().EnsureNativeBB(instr);
  output().buildBr(bb);
  VisitBlockEntryWithInitialDefs(instr);
}

void IRTranslator::VisitNativeEntry(NativeEntryInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitOsrEntry(OsrEntryInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitIndirectEntry(IndirectEntryInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCatchBlockEntry(CatchBlockEntryInstr* instr) {
  IRTranslatorBlockImpl* block_impl =
      impl().GetCatchBlockImplInitIfNeeded(instr);
  output().positionToBBEnd(block_impl->native_bb);
  VisitBlockEntryWithInitialDefs(instr);
}

void IRTranslator::VisitPhi(PhiInstr* instr) {
  impl().SetDebugLine(instr);
  LType phi_type = impl().GetMachineRepresentationType(instr->representation());

  LValue phi = output().buildPhi(phi_type);
  bool should_add_to_tf_phi_worklist = false;
  intptr_t predecessor_count = impl().current_bb_->PredecessorCount();
  IRTranslatorBlockImpl* block_impl = impl().current_bb_impl();

  for (intptr_t i = 0; i < predecessor_count; ++i) {
    BlockEntryInstr* pred = impl().current_bb()->PredecessorAt(i);
    int ssa_value = instr->InputAt(i)->definition()->ssa_temp_index();
    if (impl().IsBBStartedToTranslate(pred)) {
      LValue value =
          impl().EnsurePhiInputAndPosition(pred, ssa_value, phi_type);
      LBasicBlock llvmbb = impl().GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &llvmbb, 1);
    } else {
      should_add_to_tf_phi_worklist = true;
      block_impl->not_merged_phis.emplace_back();
      auto& not_merged_phi = block_impl->not_merged_phis.back();
      not_merged_phi.phi = phi;
      not_merged_phi.pred = pred;
      not_merged_phi.value = ssa_value;
    }
  }
  if (should_add_to_tf_phi_worklist)
    impl().phi_rebuild_worklist_.push_back(impl().current_bb_);
  block_impl->SetLLVMValue(instr->ssa_temp_index(), phi);
}

void IRTranslator::VisitRedefinition(RedefinitionInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitParameter(ParameterInstr* instr) {
  int param_index = instr->index();
  // only stack parameters
  output().parameter(kV8CCRegisterParameterCount +
                     output().stack_parameter_count() - param_index - 1);
}

void IRTranslator::VisitNativeParameter(NativeParameterInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadIndexedUnsafe(LoadIndexedUnsafeInstr* instr) {
  EMASSERT(instr->RequiredInputRepresentation(0) == kTagged);  // It is a Smi.
  impl().SetDebugLine(instr);
  LValue index_smi = impl().GetLLVMValue(instr->index());
  EMASSERT(instr->base_reg() == FP);
  LValue offset = output().buildShl(index_smi, output().constInt32(1));
  offset = output().buildAdd(offset, output().constIntPtr(instr->offset()));
  LValue access =
      impl().BuildAccessPointer(output().fp(), offset, instr->representation());
  LValue value = output().buildLoad(access);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitStoreIndexedUnsafe(StoreIndexedUnsafeInstr* instr) {
  ASSERT(instr->RequiredInputRepresentation(
             StoreIndexedUnsafeInstr::kIndexPos) == kTagged);  // It is a Smi.
  impl().SetDebugLine(instr);
  LValue index_smi = impl().GetLLVMValue(instr->index());
  EMASSERT(instr->base_reg() == FP);
  LValue offset = output().buildShl(index_smi, output().constInt32(1));
  offset = output().buildAdd(offset, output().constIntPtr(instr->offset()));
  LValue gep =
      impl().BuildAccessPointer(output().fp(), offset, instr->representation());

  LValue value = impl().GetLLVMValue(instr->value());
  output().buildStore(value, gep);
}

void IRTranslator::VisitTailCall(TailCallInstr* instr) {
  impl().SetDebugLine(instr);
  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  callsite_info->set_type(CallSiteInfo::CallTargetType::kCodeObject);
  CallResolver::CallResolverParameter param = {instr, Instr::kInstrSize,
                                               std::move(callsite_info)};
  CallResolver resolver(impl(), -1, param);
  LValue code_object = impl().LoadObject(instr->code());
  resolver.SetGParameter(static_cast<int>(CODE_REG), code_object);
  // add register parameter.
  EMASSERT(impl().GetLLVMValue(instr->InputAt(0)) == output().args_desc());
  resolver.SetGParameter(static_cast<int>(ARGS_DESC_REG), output().args_desc());
  resolver.BuildCall();
}

void IRTranslator::VisitParallelMove(ParallelMoveInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitPushArgument(PushArgumentInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  impl().PushArgument(value);
}

void IRTranslator::VisitReturn(ReturnInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  output().buildRet(value);
}

void IRTranslator::VisitNativeReturn(NativeReturnInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitThrow(ThrowInstr* instr) {
  impl().SetDebugLine(instr);
  impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                             kThrowRuntimeEntry, 1, instr->locs());
  output().buildCall(output().repo().trapIntrinsic());
}

void IRTranslator::VisitReThrow(ReThrowInstr* instr) {
  impl().SetDebugLine(instr);
  impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                             kReThrowRuntimeEntry, 2, instr->locs());
  output().buildCall(output().repo().trapIntrinsic());
}

void IRTranslator::VisitStop(StopInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGoto(GotoInstr* instr) {
  impl().SetDebugLine(instr);
  JoinEntryInstr* successor = instr->successor();
  LBasicBlock bb = impl().EnsureNativeBB(successor);
  output().buildBr(bb);
  impl().EndCurrentBlock();
}

void IRTranslator::VisitIndirectGoto(IndirectGotoInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBranch(BranchInstr* instr) {
  impl().SetDebugLine(instr);
  TargetEntryInstr* true_successor = instr->true_successor();
  TargetEntryInstr* false_successor = instr->false_successor();
  impl().EnsureNativeBB(true_successor);
  impl().EnsureNativeBB(false_successor);
  LValue cmp_val = impl().GetLLVMValue(instr->comparison());
  cmp_val = impl().EnsureBoolean(cmp_val);
  output().buildCondBr(cmp_val, impl().GetNativeBB(true_successor),
                       impl().GetNativeBB(false_successor));
  impl().EndCurrentBlock();
}

// FIXME: implement assertions.
void IRTranslator::VisitAssertAssignable(AssertAssignableInstr* instr) {}

void IRTranslator::VisitAssertSubtype(AssertSubtypeInstr* instr) {}

void IRTranslator::VisitAssertBoolean(AssertBooleanInstr* instr) {}

void IRTranslator::VisitSpecialParameter(SpecialParameterInstr* instr) {
  LValue val = nullptr;
  auto kind = instr->kind();
  if (kind == SpecialParameterInstr::kArgDescriptor) {
    val = output().args_desc();
  } else {
    EMASSERT(impl().current_bb_impl()->exception_block);
    switch (kind) {
      case SpecialParameterInstr::kException:
        val = impl().current_bb_impl()->exception_val;
        break;
      case SpecialParameterInstr::kStackTrace:
        val = impl().current_bb_impl()->stacktrace_val;
        break;
      default:
        UNREACHABLE();
    }
  }
  impl().SetLLVMValue(instr, val);
}

void IRTranslator::VisitClosureCall(ClosureCallInstr* instr) {
  impl().SetDebugLine(instr);
  const intptr_t argument_count =
      instr->ArgumentCount();  // Includes type args.
  LValue function = impl().GetLLVMValue(instr->InputAt(0));
  LValue code_object = impl().LoadFieldFromOffset(
      function, compiler::target::Function::code_offset());

  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  callsite_info->set_type(CallSiteInfo::CallTargetType::kCodeObject);
  callsite_info->set_token_pos(instr->token_pos());
  callsite_info->set_deopt_id(instr->deopt_id());
  callsite_info->set_locs(instr->locs());
  callsite_info->set_stack_parameter_count(argument_count);
  CallResolver::CallResolverParameter param = {instr, Instr::kInstrSize,
                                               std::move(callsite_info)};
  CallResolver resolver(impl(), instr->ssa_temp_index(), param);
  const Array& arguments_descriptor = Array::ZoneHandle(
      impl().flow_graph()->zone(), instr->GetArgumentsDescriptor());
  LValue argument_descriptor_obj = impl().LoadObject(arguments_descriptor);
  resolver.SetGParameter(ARGS_DESC_REG, argument_descriptor_obj);
  resolver.SetGParameter(CODE_REG, code_object);
  resolver.SetGParameter(kICReg, output().constTagged(0));
  for (intptr_t i = argument_count - 1; i >= 0; --i) {
    LValue param = impl().GetLLVMValue(instr->PushArgumentAt(i)->value());
    resolver.AddStackParameter(param);
  }
  LValue result = resolver.BuildCall();
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitFfiCall(FfiCallInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInstanceCall(InstanceCallInstr* instr) {
  impl().SetDebugLine(instr);
  EMASSERT(instr->ic_data() != NULL);
  EMASSERT((FLAG_precompiled_mode && FLAG_use_bare_instructions));
  Zone* zone = impl().flow_graph()->zone();
  ICData& ic_data = ICData::ZoneHandle(zone, instr->ic_data()->raw());
  ic_data = ic_data.AsUnaryClassChecks();

  const intptr_t argument_count =
      instr->ArgumentCount();  // Includes type args.

  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  // use kReg type.
  callsite_info->set_type(CallSiteInfo::CallTargetType::kReg);
  callsite_info->set_reg(kInstanceCallTargetReg);
  callsite_info->set_token_pos(instr->token_pos());
  callsite_info->set_deopt_id(instr->deopt_id());
  callsite_info->set_locs(instr->locs());
  callsite_info->set_stack_parameter_count(argument_count);
  CallResolver::CallResolverParameter param = {instr, Instr::kInstrSize,
                                               std::move(callsite_info)};
  CallResolver resolver(impl(), instr->ssa_temp_index(), param);

  const Code& initial_stub = StubCode::UnlinkedCall();
  const UnlinkedCall& data =
      UnlinkedCall::ZoneHandle(zone, ic_data.AsUnlinkedCall());
  LValue data_val = impl().LoadObject(data, true);
  LValue initial_stub_val = impl().LoadObject(initial_stub, true);
  resolver.SetGParameter(kICReg, data_val);
  resolver.SetGParameter(kInstanceCallTargetReg, initial_stub_val);
  for (intptr_t i = argument_count - 1; i >= 0; --i) {
    LValue param = impl().GetLLVMValue(instr->PushArgumentAt(i)->value());
    resolver.AddStackParameter(param);
  }
  LValue receiver =
      resolver.GetStackParameter((ic_data.CountWithoutTypeArgs() - 1));
  resolver.SetGParameter(kReceiverReg, receiver);
#if 0
  // unknown effect.
  __ LoadFromOffset(
      kWord, R0, SP,
      (ic_data.CountWithoutTypeArgs() - 1) * compiler::target::kWordSize);
#endif

  LValue result = resolver.BuildCall();
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitPolymorphicInstanceCall(
    PolymorphicInstanceCallInstr* instr) {
#if 0
  impl().SetDebugLine(instr);
  ArgumentsInfo args_info(instr->instance_call()->type_args_len(),
                          instr->instance_call()->ArgumentCount(),
                          instr->instance_call()->argument_names());
  const Array& arguments_descriptor =
      Array::ZoneHandle(impl().flow_graph()->zone(), args_info.ToArgumentsDescriptor());
  const CallTargets& targets = instr->targets();

  static const int kNoCase = -1;
  int smi_case = kNoCase;
  int which_case_to_skip = kNoCase;

  const int length = targets.length();
  ASSERT(length > 0);
  int non_smi_length = length;

  // Find out if one of the classes in one of the cases is the Smi class. We
  // will be handling that specially.
  for (int i = 0; i < length; i++) {
    const intptr_t start = targets[i].cid_start;
    if (start > kSmiCid) continue;
    const intptr_t end = targets[i].cid_end;
    if (end >= kSmiCid) {
      smi_case = i;
      if (start == kSmiCid && end == kSmiCid) {
        // If this case has only the Smi class then we won't need to emit it at
        // all later.
        which_case_to_skip = i;
        non_smi_length--;
      }
      break;
    }
  }

  if (smi_case != kNoCase) {
    compiler::Label after_smi_test;
    // If the call is complete and there are no other possible receiver
    // classes - then receiver can only be a smi value and we don't need
    // to check if it is a smi.
    if (!(complete && non_smi_length == 0)) {
      EmitTestAndCallSmiBranch(non_smi_length == 0 ? failed : &after_smi_test,
                               /* jump_if_smi= */ false);
    }

    // Do not use the code from the function, but let the code be patched so
    // that we can record the outgoing edges to other code.
    const Function& function = *targets.TargetAt(smi_case)->target;
    GenerateStaticDartCall(deopt_id, token_index, RawPcDescriptors::kOther,
                           locs, function, entry_kind);
    __ Drop(args_info.count_with_type_args);
    if (match_found != NULL) {
      __ Jump(match_found);
    }
    __ Bind(&after_smi_test);
  } else {
    if (!complete) {
      // Smi is not a valid class.
      EmitTestAndCallSmiBranch(failed, /* jump_if_smi = */ true);
    }
  }

  if (non_smi_length == 0) {
    // If non_smi_length is 0 then only a Smi check was needed; the Smi check
    // above will fail if there was only one check and receiver is not Smi.
    return;
  }

  bool add_megamorphic_call = false;
  int bias = 0;

  // Value is not Smi.
  EmitTestAndCallLoadCid(EmitTestCidRegister());

  int last_check = which_case_to_skip == length - 1 ? length - 2 : length - 1;

  for (intptr_t i = 0; i < length; i++) {
    if (i == which_case_to_skip) continue;
    const bool is_last_check = (i == last_check);
    const int count = targets.TargetAt(i)->count;
    if (!is_last_check && !complete && count < (total_ic_calls >> 5)) {
      // This case is hit too rarely to be worth writing class-id checks inline
      // for.  Note that we can't do this for calls with only one target because
      // the type propagator may have made use of that and expects a deopt if
      // a new class is seen at this calls site.  See IsMonomorphic.
      add_megamorphic_call = true;
      break;
    }
    compiler::Label next_test;
    if (!complete || !is_last_check) {
      bias = EmitTestAndCallCheckCid(assembler(),
                                     is_last_check ? failed : &next_test,
                                     EmitTestCidRegister(), targets[i], bias,
                                     /*jump_on_miss =*/true);
    }
    // Do not use the code from the function, but let the code be patched so
    // that we can record the outgoing edges to other code.
    const Function& function = *targets.TargetAt(i)->target;
    GenerateStaticDartCall(deopt_id, token_index, RawPcDescriptors::kOther,
                           locs, function, entry_kind);
    __ Drop(args_info.count_with_type_args);
    if (!is_last_check || add_megamorphic_call) {
      __ Jump(match_found);
    }
    __ Bind(&next_test);
  }
  if (add_megamorphic_call) {
    int try_index = kInvalidTryIndex;
    EmitMegamorphicInstanceCall(function_name, arguments_descriptor, deopt_id,
                                token_index, locs, try_index);
  }
#else
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
#endif
}

void IRTranslator::VisitStaticCall(StaticCallInstr* instr) {
  impl().SetDebugLine(instr);
  Zone* zone = impl().flow_graph()->zone();
  ICData& ic_data = ICData::ZoneHandle(zone, instr->ic_data()->raw());
  ArgumentsInfo args_info(instr->type_args_len(), instr->ArgumentCount(),
                          instr->argument_names());
  std::vector<LValue> arguments;
  for (intptr_t i = 0; i < args_info.count_with_type_args; ++i) {
    arguments.emplace_back(impl().GetLLVMValue(instr->PushArgumentAt(i)));
  }
  LValue result = impl().GenerateStaticCall(
      instr, instr->ssa_temp_index(), arguments, instr->deopt_id(),
      instr->token_pos(), instr->function(), args_info, instr->locs(), ic_data,
      instr->entry_kind());
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitLoadLocal(LoadLocalInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDropTemps(DropTempsInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitMakeTemp(MakeTempInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStoreLocal(StoreLocalInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStrictCompare(StrictCompareInstr* instr) {
  impl().SetDebugLine(instr);
  if (instr->needs_number_check()) {
    // FIXME: do it after call supports
    UNREACHABLE();
  }
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  LValue result = output().buildICmp(
      instr->kind() == Token::kEQ_STRICT ? LLVMIntEQ : LLVMIntNE, left, right);
  impl().SetLLVMValue(instr, result);
}

static LLVMIntPredicate TokenKindToSmiCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ:
      return LLVMIntEQ;
    case Token::kNE:
      return LLVMIntNE;
    case Token::kLT:
      return LLVMIntSLT;
    case Token::kGT:
      return LLVMIntSGT;
    case Token::kLTE:
      return LLVMIntSLE;
    case Token::kGTE:
      return LLVMIntSGE;
    default:
      UNREACHABLE();
  }
}

static LLVMRealPredicate TokenKindToDoubleCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ:
      return LLVMRealOEQ;
    case Token::kNE:
      return LLVMRealONE;
    case Token::kLT:
      return LLVMRealOLT;
    case Token::kGT:
      return LLVMRealOGT;
    case Token::kLTE:
      return LLVMRealOLE;
    case Token::kGTE:
      return LLVMRealOGE;
    default:
      UNREACHABLE();
  }
}

void IRTranslator::VisitEqualityCompare(EqualityCompareInstr* instr) {
  impl().SetDebugLine(instr);
  LValue cmp_value;
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  if (instr->operation_cid() == kSmiCid) {
    EMASSERT(typeOf(left) == output().tagged_type());
    EMASSERT(typeOf(right) == output().tagged_type());
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()),
                           impl().SmiUntag(left), impl().SmiUntag(right));
  } else if (instr->operation_cid() == kMintCid) {
    EMASSERT(typeOf(left) == output().repo().intPtr);
    EMASSERT(typeOf(right) == output().repo().intPtr);
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()), left, right);
  } else {
    EMASSERT(instr->operation_cid() == kDoubleCid);
    EMASSERT(typeOf(left) == output().repo().doubleType);
    EMASSERT(typeOf(right) == output().repo().doubleType);
    cmp_value = output().buildFCmp(TokenKindToDoubleCondition(instr->kind()),
                                   left, right);
  }
  impl().SetLLVMValue(instr, cmp_value);
}

void IRTranslator::VisitRelationalOp(RelationalOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue cmp_value;
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  if (instr->operation_cid() == kSmiCid) {
    EMASSERT(typeOf(left) == output().tagged_type());
    EMASSERT(typeOf(right) == output().tagged_type());
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()),
                           impl().SmiUntag(left), impl().SmiUntag(right));
  } else if (instr->operation_cid() == kMintCid) {
    EMASSERT(typeOf(left) == output().repo().intPtr);
    EMASSERT(typeOf(right) == output().repo().intPtr);
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()), left, right);
  } else {
    EMASSERT(instr->operation_cid() == kDoubleCid);
    EMASSERT(typeOf(left) == output().repo().doubleType);
    EMASSERT(typeOf(right) == output().repo().doubleType);
    cmp_value = output().buildFCmp(TokenKindToDoubleCondition(instr->kind()),
                                   left, right);
  }
  impl().SetLLVMValue(instr, cmp_value);
}

void IRTranslator::VisitNativeCall(NativeCallInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDebugStepCheck(DebugStepCheckInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadIndexed(LoadIndexedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue array = impl().GetLLVMValue(instr->array());
  LValue index = impl().GetLLVMValue(instr->index());

  const intptr_t shift =
      Utils::ShiftForPowerOfTwo(instr->index_scale()) - kSmiTagShift;
  int32_t offset =
      instr->IsExternal()
          ? 0
          : (compiler::target::Instance::DataOffsetFor(instr->class_id()) -
             kHeapObjectTag);
  if (shift < 0) {
    EMASSERT(shift == -1);
    index = output().buildSar(index, output().constIntPtr(1));
  } else {
    index = output().buildShl(index, output().constIntPtr(shift));
  }
  LValue offset_value = output().buildAdd(output().constIntPtr(offset), index);
  LValue gep =
      impl().BuildAccessPointer(array, offset_value, instr->representation());
  LValue val = output().buildLoad(gep);
  impl().SetLLVMValue(instr, val);
}

void IRTranslator::VisitLoadCodeUnits(LoadCodeUnitsInstr* instr) {
  impl().SetDebugLine(instr);
  LValue array = impl().GetLLVMValue(instr->array());
  LValue index = impl().GetLLVMValue(instr->index());
  const intptr_t shift =
      Utils::ShiftForPowerOfTwo(instr->index_scale()) - kSmiTagShift;
  int32_t offset =
      instr->IsExternal()
          ? 0
          : (compiler::target::Instance::DataOffsetFor(instr->class_id()) -
             kHeapObjectTag);
  if (shift < 0) {
    EMASSERT(shift == -1);
    index = output().buildSar(index, output().constIntPtr(1));
  } else {
    index = output().buildShl(index, output().constIntPtr(shift));
  }
  LValue offset_value = output().buildAdd(output().constIntPtr(offset), index);
  LValue value;
#if !defined(TARGET_ARCH_IS_64_BIT)
  if (instr->representation() == kUnboxedInt64) {
    LValue gep = impl().BuildAccessPointer(array, offset_value, kUnboxedUint32);
    LValue value_32 = output().buildLoad(gep);
    value = output().buildCast(LLVMZExt, value_32, output().repo().int64);
  }
#endif
  if (instr->representation() == kTagged) {
    EMASSERT(instr->representation() == kTagged);
    LType type;
    switch (instr->class_id()) {
      case kOneByteStringCid:
      case kExternalOneByteStringCid:
        switch (instr->element_count()) {
          case 1:
            type = output().repo().int8;
            break;
          case 2:
            type = output().repo().int16;
            break;
          case 4:
            type = output().repo().int32;
            break;
          default:
            UNREACHABLE();
        }
        break;
      case kTwoByteStringCid:
      case kExternalTwoByteStringCid:
        switch (instr->element_count()) {
          case 1:
            type = output().repo().int16;
            break;
          case 2:
            type = output().repo().int32;
            break;
          default:
            UNREACHABLE();
        }
        break;
      default:
        UNREACHABLE();
        break;
    }
    LValue gep =
        impl().BuildAccessPointer(array, offset_value, pointerType(type));
    LValue int_value = impl().EnsureIntPtr(output().buildLoad(gep));

    if (instr->can_pack_into_smi()) {
      value = impl().SmiTag(int_value);
    } else {
      // FIXME: implement after call support
      UNREACHABLE();
    }
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitStoreIndexed(StoreIndexedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue array = impl().GetLLVMValue(instr->array());
  LValue index = impl().GetLLVMValue(instr->index());
  LValue value = impl().GetLLVMValue(instr->value());
  const intptr_t shift =
      Utils::ShiftForPowerOfTwo(instr->index_scale()) - kSmiTagShift;
  int32_t offset =
      instr->IsExternal()
          ? 0
          : (compiler::target::Instance::DataOffsetFor(instr->class_id()) -
             kHeapObjectTag);
  if (shift < 0) {
    EMASSERT(shift == -1);
    index = output().buildSar(index, output().constIntPtr(1));
  } else {
    index = output().buildShl(index, output().constIntPtr(shift));
  }
  LValue offset_value = output().buildAdd(output().constIntPtr(offset), index);
  switch (instr->class_id()) {
    case kArrayCid:
      if (instr->ShouldEmitStoreBarrier()) {
        // FIXME: implment after call support
        UNREACHABLE();
      } else {
        LValue gep = impl().BuildAccessPointer(
            array, offset_value, pointerType(output().tagged_type()));
        output().buildStore(value, gep);
      }
      break;
    case kTypedDataInt8ArrayCid:
    case kTypedDataUint8ArrayCid:
    case kExternalTypedDataUint8ArrayCid:
    case kOneByteStringCid: {
      EMASSERT(instr->RequiredInputRepresentation(2) == kUnboxedIntPtr);
      LValue value_to_store =
          output().buildCast(LLVMTrunc, value, output().repo().int8);
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref8);
      output().buildStore(value_to_store, gep);
      break;
    }
    case kTypedDataUint8ClampedArrayCid:
    case kExternalTypedDataUint8ClampedArrayCid: {
      EMASSERT(instr->RequiredInputRepresentation(2) == kUnboxedIntPtr);
      value = impl().EnsureIntPtr(value);
      LValue constant = output().constIntPtr(0xFF);
      LValue cmp_1 = output().buildICmp(LLVMIntSLE, value, constant);
      LValue cmp_2 = output().buildICmp(LLVMIntULE, value, constant);
      LValue value_select =
          output().buildSelect(cmp_1, output().constIntPtr(0), constant);
      value = output().buildSelect(cmp_2, value, value_select);
      LValue value_to_store =
          output().buildCast(LLVMTrunc, value, output().repo().int8);
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref8);
      output().buildStore(value_to_store, gep);
      break;
    }
    case kTypedDataInt16ArrayCid:
    case kTypedDataUint16ArrayCid: {
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref16);
      if (typeOf(value) != output().repo().int16)
        value = output().buildCast(LLVMTrunc, value, output().repo().int16);
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataInt32ArrayCid:
    case kTypedDataUint32ArrayCid: {
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref32);
      if (typeOf(value) != output().repo().int32)
        value = output().buildCast(LLVMTrunc, value, output().repo().int32);
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataInt64ArrayCid:
    case kTypedDataUint64ArrayCid: {
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref64);
      EMASSERT(typeOf(value) == output().repo().int64)
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataFloat32ArrayCid: {
      LValue gep = impl().BuildAccessPointer(array, offset_value,
                                             output().repo().refFloat);
      EMASSERT(typeOf(value) == output().repo().floatType)
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataFloat64ArrayCid: {
      LValue gep = impl().BuildAccessPointer(array, offset_value,
                                             output().repo().refDouble);
      EMASSERT(typeOf(value) == output().repo().doubleType)
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataFloat64x2ArrayCid:
    case kTypedDataInt32x4ArrayCid:
    case kTypedDataFloat32x4ArrayCid: {
      UNREACHABLE();
      break;
    }
    default:
      UNREACHABLE();
  }
}

void IRTranslator::VisitStoreInstanceField(StoreInstanceFieldInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInitInstanceField(InitInstanceFieldInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInitStaticField(InitStaticFieldInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadStaticField(LoadStaticFieldInstr* instr) {
  impl().SetDebugLine(instr);
  LValue field = impl().GetLLVMValue(instr->field_value());
  LValue v = impl().LoadFieldFromOffset(
      field, compiler::target::Field::static_value_offset());
  impl().SetLLVMValue(instr, v);
}

void IRTranslator::VisitStoreStaticField(StoreStaticFieldInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBooleanNegate(BooleanNegateInstr* instr) {
  impl().SetDebugLine(instr);

  LValue true_or_false = impl().GetLLVMValue(instr->value());
  LValue true_value = impl().LoadObject(Bool::True());
  LValue false_value = impl().LoadObject(Bool::False());
  LValue cmp_value = output().buildICmp(LLVMIntEQ, true_value, true_or_false);
  LValue value = output().buildSelect(cmp_value, false_value, true_value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitInstanceOf(InstanceOfInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCreateArray(CreateArrayInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitAllocateObject(AllocateObjectInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadField(LoadFieldInstr* instr) {
  impl().SetDebugLine(instr);
  LValue instance = impl().GetLLVMValue(instr->instance());
  LValue boxed =
      impl().LoadFieldFromOffset(instance, instr->slot().offset_in_bytes());
  if (instr->IsUnboxedLoad()) {
    const intptr_t cid = instr->slot().field().UnboxedFieldCid();
    LValue value;
    switch (cid) {
      case kDoubleCid:
        value = impl().LoadFieldFromOffset(
            boxed, compiler::target::Double::value_offset(),
            output().repo().refDouble);
        break;
      case kFloat32x4Cid:
        EMASSERT("kFloat32x4Cid not supported" && false);
        UNREACHABLE();
      case kFloat64x2Cid:
        EMASSERT("kFloat64x2Cid not supported" && false);
        UNREACHABLE();
      default:
        UNREACHABLE();
    }
    impl().SetLLVMValue(instr, value);
    return;
  }
  if (instr->IsPotentialUnboxedLoad()) {
    // FIXME: need for continuation resolver
    // support after call implementation.
    UNREACHABLE();
  }
  impl().SetLLVMValue(instr, boxed);
}

void IRTranslator::VisitLoadUntagged(LoadUntaggedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue object = impl().GetLLVMValue(instr->object());
  LValue value;
  if (instr->object()->definition()->representation() == kUntagged) {
    value =
        impl().LoadFromOffset(object, instr->offset(), output().repo().refPtr);
  } else {
    value = impl().LoadFieldFromOffset(object, instr->offset());
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitStoreUntagged(StoreUntaggedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue object = impl().GetLLVMValue(instr->object());
  LValue value = impl().GetLLVMValue(instr->value());
  impl().StoreToOffset(object, instr->offset_from_tagged(), value);
}

void IRTranslator::VisitLoadClassId(LoadClassIdInstr* instr) {
  impl().SetDebugLine(instr);
  const AbstractType& value_type = *instr->object()->Type()->ToAbstractType();
  LValue object = impl().GetLLVMValue(instr->object());
  LValue value;
  const intptr_t class_id_offset =
      compiler::target::Object::tags_offset() +
      compiler::target::RawObject::kClassIdTagPos / kBitsPerByte;
  if (CompileType::Smi().IsAssignableTo(value_type) ||
      value_type.IsTypeParameter()) {
    DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
    diamond.BuildCmp([&]() { return impl().TstSmi(object); })
        .BuildLeft([&]() { return output().constInt16(kSmiCid); })
        .BuildRight([&]() {
          return impl().LoadFieldFromOffset(object, class_id_offset,
                                            pointerType(output().repo().int16));
        });

    value = diamond.End();
  } else {
    value = impl().LoadFromOffset(object, class_id_offset - kHeapObjectTag,
                                  pointerType(output().repo().int16));
  }
  value = impl().SmiTag(value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitInstantiateType(InstantiateTypeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInstantiateTypeArguments(
    InstantiateTypeArgumentsInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitAllocateContext(AllocateContextInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitAllocateUninitializedContext(
    AllocateUninitializedContextInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCloneContext(CloneContextInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinarySmiOp(BinarySmiOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().SmiUntag(impl().GetLLVMValue(instr->left()));
  LValue right = impl().SmiUntag(impl().GetLLVMValue(instr->right()));
  LValue value;
  switch (instr->op_kind()) {
    case Token::kSHL:
      value = output().buildShl(left, right);
      break;
    case Token::kADD:
      value = output().buildAdd(left, right);
      break;
    case Token::kMUL:
      value = output().buildMul(left, right);
      break;
    case Token::kTRUNCDIV:
      value = output().buildSDiv(left, right);
      break;
    case Token::kBIT_AND:
      value = output().buildAnd(left, right);
      break;
    case Token::kBIT_OR:
      value = output().buildOr(left, right);
      break;
    case Token::kBIT_XOR:
      value = output().buildXor(left, right);
      break;
    case Token::kSHR:
      value = output().buildShr(left, right);
      break;
    default:
      UNREACHABLE();
      break;
  }
  value = impl().SmiTag(value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitCheckedSmiComparison(CheckedSmiComparisonInstr* instr) {
  impl().SetDebugLine(instr);
  // FIXME: implment slow path code after call supported.
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  LValue cmp_value =
      output().buildICmp(TokenKindToSmiCondition(instr->kind()),
                         impl().SmiUntag(left), impl().SmiUntag(right));
  impl().SetLLVMValue(instr, cmp_value);
}

void IRTranslator::VisitCheckedSmiOp(CheckedSmiOpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryInt32Op(BinaryInt32OpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().EnsureInt32(impl().GetLLVMValue(instr->left()));
  LValue right = impl().EnsureInt32(impl().GetLLVMValue(instr->right()));
  LValue value;
  switch (instr->op_kind()) {
    case Token::kSHL:
      value = output().buildShl(left, right);
      break;
    case Token::kADD:
      value = output().buildAdd(left, right);
      break;
    case Token::kMUL:
      value = output().buildMul(left, right);
      break;
    case Token::kTRUNCDIV:
      value = output().buildSDiv(left, right);
      break;
    case Token::kBIT_AND:
      value = output().buildAnd(left, right);
      break;
    case Token::kBIT_OR:
      value = output().buildOr(left, right);
      break;
    case Token::kBIT_XOR:
      value = output().buildXor(left, right);
      break;
    case Token::kSHR:
      value = output().buildShr(left, right);
      break;
    default:
      UNREACHABLE();
      break;
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitUnarySmiOp(UnarySmiOpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnaryDoubleOp(UnaryDoubleOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue v = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(v) == output().repo().doubleType);
  impl().SetLLVMValue(instr, output().buildFNeg(v));
}

void IRTranslator::VisitCheckStackOverflow(CheckStackOverflowInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitSmiToDouble(SmiToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue v = impl().GetLLVMValue(instr->value());
  LValue num = impl().SmiUntag(v);
  LValue dnum = output().buildCast(LLVMSIToFP, num, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitInt32ToDouble(Int32ToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue num = impl().EnsureInt32(impl().GetLLVMValue(instr->value()));
  LValue dnum = output().buildCast(LLVMSIToFP, num, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitInt64ToDouble(Int64ToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue num = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(num) == output().repo().int64);
  LValue dnum = output().buildCast(LLVMSIToFP, num, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitDoubleToInteger(DoubleToIntegerInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToSmi(DoubleToSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToDouble(DoubleToDoubleInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToFloat(DoubleToFloatInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitFloatToDouble(FloatToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue fnum = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(fnum) == output().repo().floatType);
  LValue dnum = output().buildCast(LLVMFPExt, fnum, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitCheckClass(CheckClassInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckClassId(CheckClassIdInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckSmi(CheckSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckNull(CheckNullInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckCondition(CheckConditionInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitConstant(ConstantInstr* instr) {
  impl().SetLazyValue(instr);
}

void IRTranslator::VisitUnboxedConstant(UnboxedConstantInstr* instr) {
  impl().SetLazyValue(instr);
}

void IRTranslator::VisitCheckEitherNonSmi(CheckEitherNonSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryDoubleOp(BinaryDoubleOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  EMASSERT(typeOf(left) == output().repo().doubleType);
  EMASSERT(typeOf(right) == output().repo().doubleType);
  LValue value;
  switch (instr->op_kind()) {
    case Token::kADD:
      value = output().buildFAdd(left, right);
      break;
    case Token::kSUB:
      value = output().buildFSub(left, right);
      break;
    case Token::kMUL:
      value = output().buildFMul(left, right);
      break;
    case Token::kDIV:
      value = output().buildFDiv(left, right);
      break;
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitDoubleTestOp(DoubleTestOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(value) == output().repo().doubleType);
  LValue cmp_value;
  if (instr->op_kind() == MethodRecognizer::kDouble_getIsNaN) {
    cmp_value = output().buildFCmp(LLVMRealUNO, value, value);
  } else {
    ASSERT(instr->op_kind() == MethodRecognizer::kDouble_getIsInfinite);
    value = output().buildCall(output().repo().doubleAbsIntrinsic(), value);
    cmp_value =
        output().buildFCmp(LLVMRealOEQ, value, output().constDouble(INFINITY));
  }
  const bool is_negated = instr->kind() != Token::kEQ;
  if (is_negated) {
    cmp_value =
        output().buildICmp(LLVMIntEQ, cmp_value, output().repo().booleanFalse);
  }
  impl().SetLLVMValue(instr, cmp_value);
}

void IRTranslator::VisitMathUnary(MathUnaryInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(value) == output().repo().doubleType);
  if (instr->kind() == MathUnaryInstr::kSqrt) {
    value = output().buildCall(output().repo().doubleSqrtIntrinsic(), value);
  } else if (instr->kind() == MathUnaryInstr::kDoubleSquare) {
    value = output().buildFMul(value, value);
  } else {
    UNREACHABLE();
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitMathMinMax(MathMinMaxInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  LValue value;
  LValue cmp_value;
  const intptr_t is_min = (instr->op_kind() == MethodRecognizer::kMathMin);
  if (instr->result_cid() == kDoubleCid) {
    EMASSERT(typeOf(left) == output().repo().doubleType);
    EMASSERT(typeOf(right) == output().repo().doubleType);
    cmp_value = output().buildFCmp(LLVMRealOGT, left, right);
  } else {
    cmp_value = output().buildICmp(LLVMIntSGT, left, right);
  }
  if (is_min)
    value = output().buildSelect(cmp_value, right, left);
  else
    value = output().buildSelect(cmp_value, left, right);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitBox(BoxInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnbox(UnboxInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  LValue result;
  auto EmitLoadFromBox = [&]() {
    switch (instr->representation()) {
      case kUnboxedInt64: {
        LValue gep = output().buildGEPWithByteOffset(
            value,
            Boxing::ValueOffset(instr->representation()) - kHeapObjectTag,
            pointerType(output().repo().int64));
        return output().buildLoad(gep);
      }

      case kUnboxedDouble: {
        LValue gep = output().buildGEPWithByteOffset(
            value,
            Boxing::ValueOffset(instr->representation()) - kHeapObjectTag,
            pointerType(output().repo().doubleType));
        return output().buildLoad(gep);
      }

      case kUnboxedFloat: {
        LValue gep = output().buildGEPWithByteOffset(
            value,
            Boxing::ValueOffset(instr->representation()) - kHeapObjectTag,
            pointerType(output().repo().doubleType));
        LValue result = output().buildLoad(gep);
        return output().buildCast(LLVMFPTrunc, result,
                                  output().repo().floatType);
      }

      case kUnboxedFloat32x4:
      case kUnboxedFloat64x2:
      case kUnboxedInt32x4: {
        UNREACHABLE();
        break;
      }

      default:
        UNREACHABLE();
        break;
    }
  };

  auto EmitLoadInt32FromBoxOrSmi = [&]() {
    DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
    diamond.BuildCmp([&]() { return impl().TstSmi(value); })
        .BuildLeft([&]() { return impl().SmiUntag(value); })
        .BuildRight([&]() {
          return impl().LoadFieldFromOffset(
              value, compiler::target::Mint::value_offset());
        });
    return diamond.End();
  };
  auto EmitSmiConversion = [&]() {
    LValue result;
    switch (instr->representation()) {
      case kUnboxedInt64: {
        result = impl().SmiUntag(value);
        result = output().buildCast(LLVMSExt, result, output().repo().int64);
        break;
      }

      case kUnboxedDouble: {
        result = impl().SmiUntag(value);
        result =
            output().buildCast(LLVMSIToFP, result, output().repo().doubleType);
        break;
      }

      default:
        UNREACHABLE();
        break;
    }
    return result;
  };
  auto EmitLoadInt64FromBoxOrSmi = [&]() {
    DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
    diamond.BuildCmp([&]() { return impl().TstSmi(value); })
        .BuildLeft([&]() {
          LValue result = impl().SmiUntag(value);
          return output().buildCast(LLVMSExt, result, output().repo().int64);
        })
        .BuildRight([&]() { return EmitLoadFromBox(); });
    return diamond.End();
  };
  if (instr->speculative_mode() == UnboxInstr::kNotSpeculative) {
    switch (instr->representation()) {
      case kUnboxedDouble:
      case kUnboxedFloat: {
        result = EmitLoadFromBox();
      } break;

      case kUnboxedInt32:
        result = EmitLoadInt32FromBoxOrSmi();
        break;

      case kUnboxedInt64: {
        if (instr->value()->Type()->ToCid() == kSmiCid) {
          // Smi -> int64 conversion is more efficient than
          // handling arbitrary smi/mint.
          result = EmitSmiConversion();
        } else {
          result = EmitLoadInt64FromBoxOrSmi();
        }
        break;
      }
      default:
        UNREACHABLE();
        break;
    }
  } else {
    ASSERT(instr->speculative_mode() == UnboxInstr::kGuardInputs);
    const intptr_t value_cid = instr->value()->Type()->ToCid();
    const intptr_t box_cid = Boxing::BoxCid(instr->representation());

    if (value_cid == box_cid) {
      result = EmitLoadFromBox();
    } else if (instr->CanConvertSmi() && (value_cid == kSmiCid)) {
      result = EmitSmiConversion();
    } else if (instr->representation() == kUnboxedInt32 &&
               instr->value()->Type()->IsInt()) {
      result = EmitLoadInt32FromBoxOrSmi();
    } else if (instr->representation() == kUnboxedInt64 &&
               instr->value()->Type()->IsInt()) {
      result = EmitLoadInt64FromBoxOrSmi();
    } else {
      UNREACHABLE();
    }
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitBoxInt64(BoxInt64Instr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnboxInt64(UnboxInt64Instr* instr) {
  VisitUnbox(instr);
}

void IRTranslator::VisitCaseInsensitiveCompare(
    CaseInsensitiveCompareInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryInt64Op(BinaryInt64OpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  EMASSERT(typeOf(left) == output().repo().int64);
  EMASSERT(typeOf(right) == output().repo().int64);
  LValue result;
  switch (instr->op_kind()) {
    case Token::kBIT_AND: {
      result = output().buildAnd(left, right);
      break;
    }
    case Token::kBIT_OR: {
      result = output().buildOr(left, right);
      break;
    }
    case Token::kBIT_XOR: {
      result = output().buildXor(left, right);
      break;
    }
    case Token::kADD: {
      result = output().buildAdd(left, right);
      break;
    }
    case Token::kSUB: {
      result = output().buildSub(left, right);
      break;
    }
    case Token::kMUL: {
      result = output().buildMul(left, right);
      break;
    }
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitShiftInt64Op(ShiftInt64OpInstr* instr) {
  impl().SetDebugLine(instr);

  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());

  // only support the constant shift now.
  EMASSERT(instr->right()->BindsToConstant());
  LValue result;

  switch (instr->op_kind()) {
    case Token::kSHR: {
      result = output().buildShr(left, right);
      break;
    }
    case Token::kSHL: {
      result = output().buildShl(left, right);
      break;
    }
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitSpeculativeShiftInt64Op(
    SpeculativeShiftInt64OpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnaryInt64Op(UnaryInt64OpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());

  switch (instr->op_kind()) {
    case Token::kBIT_NOT:
      value = output().buildNot(value);
      break;
    case Token::kNEGATE:
      value = output().buildNeg(value);
      break;
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitCheckArrayBound(CheckArrayBoundInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGenericCheckBound(GenericCheckBoundInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitConstraint(ConstraintInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStringToCharCode(StringToCharCodeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitOneByteStringFromCharCode(
    OneByteStringFromCharCodeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStringInterpolate(StringInterpolateInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInvokeMathCFunction(InvokeMathCFunctionInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitTruncDivMod(TruncDivModInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldClass(GuardFieldClassInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldLength(GuardFieldLengthInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldType(GuardFieldTypeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

static bool IsPowerOfTwoKind(intptr_t v1, intptr_t v2) {
  return (Utils::IsPowerOfTwo(v1) && (v2 == 0)) ||
         (Utils::IsPowerOfTwo(v2) && (v1 == 0));
}

void IRTranslator::VisitIfThenElse(IfThenElseInstr* instr) {
  ComparisonInstr* compare = instr->comparison();
  if (!impl().current_bb_impl()->IsDefined(compare)) {
    compare->Accept(this);
  }
  EMASSERT(impl().current_bb_impl()->IsDefined(compare));
  impl().SetDebugLine(instr);
  LValue cmp_value = impl().GetLLVMValue(compare->ssa_temp_index());

  intptr_t true_value = instr->if_true();
  intptr_t false_value = instr->if_false();

  const bool is_power_of_two_kind = IsPowerOfTwoKind(true_value, false_value);

  if (is_power_of_two_kind) {
    if (true_value == 0) {
      // We need to have zero in result on true_condition.
      cmp_value = output().buildICmp(LLVMIntEQ, cmp_value,
                                     output().repo().booleanFalse);
    }
  } else {
    if (true_value == 0) {
      // Swap values so that false_value is zero.
      intptr_t temp = true_value;
      true_value = false_value;
      false_value = temp;
    } else {
      cmp_value = output().buildICmp(LLVMIntEQ, cmp_value,
                                     output().repo().booleanFalse);
    }
  }
  LValue result = output().buildSelect(cmp_value, output().constIntPtr(1),
                                       output().constIntPtr(0));

  if (is_power_of_two_kind) {
    const intptr_t shift =
        Utils::ShiftForPowerOfTwo(Utils::Maximum(true_value, false_value));
    result =
        output().buildShl(result, output().constIntPtr(shift + kSmiTagSize));
  } else {
    result = output().buildSub(result, output().constIntPtr(1));
    const int32_t val = compiler::target::ToRawSmi(true_value) -
                        compiler::target::ToRawSmi(false_value);
    result = output().buildAnd(result, output().constIntPtr(val));
    if (false_value != 0) {
      result = output().buildAdd(
          result,
          output().constIntPtr(compiler::target::ToRawSmi(false_value)));
    }
  }
  result = output().buildCast(LLVMIntToPtr, result, output().tagged_type());
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitMaterializeObject(MaterializeObjectInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitTestSmi(TestSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitTestCids(TestCidsInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitExtractNthOutput(ExtractNthOutputInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryUint32Op(BinaryUint32OpInstr* instr) {
  impl().SetDebugLine(instr);

  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  EMASSERT(typeOf(left) == output().repo().int32);
  EMASSERT(typeOf(right) == output().repo().int32);
  LValue result;
  switch (instr->op_kind()) {
    case Token::kBIT_AND:
      result = output().buildAnd(left, right);
      break;
    case Token::kBIT_OR:
      result = output().buildOr(left, right);
      break;
    case Token::kBIT_XOR:
      result = output().buildXor(left, right);
      break;
    case Token::kADD:
      result = output().buildAdd(left, right);
      break;
    case Token::kSUB:
      result = output().buildSub(left, right);
      break;
    case Token::kMUL:
      result = output().buildMul(left, right);
      break;
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitShiftUint32Op(ShiftUint32OpInstr* instr) {
  impl().SetDebugLine(instr);

  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());

  // only support the constant shift now.
  EMASSERT(instr->right()->BindsToConstant());
  LValue result;

  switch (instr->op_kind()) {
    case Token::kSHR: {
      result = output().buildShr(left, right);
      break;
    }
    case Token::kSHL: {
      result = output().buildShl(left, right);
      break;
    }
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitSpeculativeShiftUint32Op(
    SpeculativeShiftUint32OpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnaryUint32Op(UnaryUint32OpInstr* instr) {
  impl().SetDebugLine(instr);
  EMASSERT(instr->op_kind() == Token::kBIT_NOT);
  LValue value = impl().GetLLVMValue(instr->value());

  value = output().buildNot(value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitBoxUint32(BoxUint32Instr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnboxUint32(UnboxUint32Instr* instr) {
  VisitUnbox(instr);
}

void IRTranslator::VisitBoxInt32(BoxInt32Instr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnboxInt32(UnboxInt32Instr* instr) {
  VisitUnbox(instr);
}

void IRTranslator::VisitIntConverter(IntConverterInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  LValue result;

  if (instr->from() == kUnboxedInt32 && instr->to() == kUnboxedUint32) {
    result = value;
  } else if (instr->from() == kUnboxedUint32 && instr->to() == kUnboxedInt32) {
    result = value;
  } else if (instr->from() == kUnboxedInt64) {
    if (instr->to() == kUnboxedInt32) {
      result = output().buildCast(LLVMTrunc, value, output().repo().int32);
    } else {
      result = output().buildCast(LLVMTrunc, value, output().repo().int32);
    }
  } else if (instr->to() == kUnboxedInt64) {
    if (instr->from() == kUnboxedUint32) {
      result = output().buildCast(LLVMZExt, value, output().repo().int64);
    } else {
      result = output().buildCast(LLVMSExt, value, output().repo().int64);
    }
  } else {
    UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitBitCast(BitCastInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnboxedWidthExtender(UnboxedWidthExtenderInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDeoptimize(DeoptimizeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitSimdOp(SimdOpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
