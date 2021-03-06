//===---- include/gcinfo/gcinfo.h -------------------------------*- C++ -*-===//
//
// LLILC
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license.
// See LICENSE file in the project root for full license information.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief GCInfo Generator for LLILC
///
//===----------------------------------------------------------------------===//

#ifndef GCINFO_H
#define GCINFO_H

#include "gcinfoencoder.h"
#include "jitpch.h"
#include "LLILCJit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include <sstream>

class GcInfoAllocator;
class GcInfoEncoder;

// Allocation type information for a stack allocation.
//
// While some of the following information is deducible
// from the Alloca instruction (ex: GcPointer/Aggregate)
// we cache the information in the flag bits instead of
// walking through the type every time.

enum AllocaFlags {
  None = 0x0,
  GcPointer = 0x1,
  GcAggregate = 0x2,
  GcValue = GcPointer | GcAggregate,
  Pinned = 0x4,
  GsCookie = 0x8,
  SecurityObject = 0x10,
  GenericsContext = 0x20
};

// Per stack allocation GcInfo
//
// This Data-structure records the frame-offsets for certain special
// allocations in each function generated by LLILC. This includes:
//  -> Special Symbols: GsCookie, SecurityObject, GenericsContext
//                      which must be identified to the runtime.
//  -> Pinned pointers: Whose location must be reported to the runtime.
//  -> GC Aggregates:   Location of all aggregates containing GC pointers
//                      allocated on the stack
//  -> GC Pointers:     GC Pointers allocated on the stack.
//
// Gc Pointers and GC aggregates are collectively referred to as GC Values.
//
// The stackmap generated via Statepoints only handles SSA values,
// and therefore does not track pointers within stack allocated GC Values.
//
// To circumvent this problem, we report all pointers within stack allocated
// GC Values as untracked.
// These stack allocations are known to be live throughout the function,
// because the reader marks them as frame-escaped.

struct AllocaInfo {
  int32_t Offset;
  AllocaFlags Flags; // Combination of AllocaFlags, to be precise.
  const char *getAllocTypeString() const;
  bool isGcPointer() const { return ((Flags & AllocaFlags::GcPointer) != 0); }
  bool isGcAggregate() const {
    return ((Flags & AllocaFlags::GcAggregate) != 0);
  }
  bool isGcValue() const { return ((Flags & (AllocaFlags::GcValue)) != 0); }
  bool isPinned() const { return ((Flags & AllocaFlags::Pinned) != 0); }
};

/// \brief Per Function GcInfo
/// Allocation map for stack allocated GC Values
/// and special symbols.

class GcFuncInfo {
public:
  GcFuncInfo(const llvm::Function *F);

  void recordGcAlloca(const llvm::AllocaInst *Alloca);
  void recordPinned(const llvm::AllocaInst *Alloca);
  void recordSecurityObject(const llvm::AllocaInst *Alloca);
  void recordGsCookie(const llvm::AllocaInst *Alloca, uint32_t ValidRangeStart,
                      uint32_t ValidRangeEnd);
  void recordGenericsContext(const llvm::AllocaInst *Alloca,
                             GENERIC_CONTEXTPARAM_TYPE ParamType);

  bool hasRecord(const llvm::AllocaInst *Alloca) {
    return (AllocaMap.find(Alloca) != AllocaMap.end());
  }

  void getEscapingLocations(llvm::SmallVector<llvm::Value *, 4> &EscapingLocs);

  // Function for which GcInfo is recorded
  const llvm::Function *Function;

  // Alloca Instruction to AllocaInfo Map, for:
  // a) All stack allocated GC Values
  // b) Certain special allocations like Generics Context Parameter,
  //    which are not of GC-type.
  llvm::ValueMap<const llvm::AllocaInst *, AllocaInfo> AllocaMap;

  // Additional information for special slots
  uint32_t GsCkValidRangeStart;
  uint32_t GsCkValidRangeEnd;
  GENERIC_CONTEXTPARAM_TYPE GenericsContextParamType;

private:
  // Record a Stack Allocation in the FuncInfo, with appropriate
  // Flags based on Type of allocation.
  void recordAlloca(const llvm::AllocaInst *Alloca);

  // Mark additional annotations on a recorded GC Value
  void markGcAlloca(const llvm::AllocaInst *Alloca, AllocaFlags Flags);

  // Mark additional annotations on a recorded non-GC-Value
  void markNonGcAlloca(const llvm::AllocaInst *Alloca, AllocaFlags Flags);
};

/// \brief Per Module / Jit Invocation GcInfo
// GcFuncInfo Map for all functions in a Module.

class GcInfo {
public:
  static const uint32_t UnmanagedAddressSpace = 0;
  static const uint32_t ManagedAddressSpace = 1;
  static const int32_t InvalidPointerOffset = -1;

  static bool isGcPointer(const llvm::Type *Type);
  static bool isGcAggregate(const llvm::Type *Type);
  static bool isGcType(const llvm::Type *Type) {
    return isGcPointer(Type) || isGcAggregate(Type);
  }
  static bool isUnmanagedPointer(const llvm::Type *Type) {
    return Type->isPointerTy() && !isGcPointer(Type);
  }
  static bool isGcValue(const llvm::Value *Value) {
    return isGcType(Value->getType());
  }
  static bool isGcAllocation(const llvm::Value *Value);
  static bool isGcFunction(const llvm::Function *F);
  static bool isFPBasedFunction(const llvm::Function *F);

  static void getGcPointers(llvm::StructType *StructTy,
                            const llvm::DataLayout &DataLayout,
                            llvm::SmallVector<uint32_t, 4> &GcPtrOffsets);

  GcFuncInfo *newGcInfo(const llvm::Function *F);
  GcFuncInfo *getGcInfo(const llvm::Function *F);

  llvm::ValueMap<const llvm::Function *, GcFuncInfo *> GcInfoMap;
};

/// \brief This is the translator from LLVM's GC StackMaps
///  to CoreCLR's GcInfo encoding.
class GcInfoEmitter {
public:
  /// Construct a GCInfo object
  /// \param JitCtx Context record for the method's jit request.
  /// \param StackMapData A pointer to the .llvm_stackmaps section
  ///        loaded in memory
  /// \param Allocator The allocator to be used by GcInfo encoder
  GcInfoEmitter(LLILCJitContext *JitCtx, uint8_t *StackMapData,
                GcInfoAllocator *Allocator);

  /// Emit GC Info to the EE using GcInfoEncoder.
  void emitGCInfo();

  /// Destructor -- delete allocated memory
  ~GcInfoEmitter();

private:
  void emitGCInfo(const GcFuncInfo *GcFuncInfo);
  void encodeHeader(const GcFuncInfo *GcFuncInfo);
  void encodeTrackedPointers(const GcFuncInfo *GcFuncInfo);
  void encodeUntrackedPointers(const GcFuncInfo *GcFuncInfo);
  void encodeGcAggregate(const llvm::AllocaInst *Alloca,
                         const AllocaInfo &AllocaInfo);
  void finalizeEncoding();
  void emitEncoding();

  bool needsGCInfo(const llvm::Function *F);
  bool needsPointerReporting(const llvm::Function *F);

  bool hasSlot(int32_t Offset) { return SlotMap.find(Offset) != SlotMap.end(); }
  bool isTrackedSlot(GcSlotId SlotID);
  GcSlotId getSlot(int32_t Offset, GcSlotFlags Flags);
  GcSlotId getTrackedSlot(int32_t Offset);
  GcSlotId getUntrackedSlot(int32_t Offset, bool IsPinned = false,
                            bool IsObjectRef = false);

  const LLILCJitContext *JitContext;
  const uint8_t *LLVMStackMapData;
  GcInfoEncoder Encoder;

  // Offset to SlotID Map
  // Currently, the base pointer for all slots is the current function's SP.
  // If this changes, we need to change SlotMap
  //   from {Offset -> SlotID} mapping
  //   to {(base, offset) -> SlotID) mapping.
  //
  // The current encoding requires all slots of the same type
  // (tracked, untracked, pinned) to be allocated contiguously.
  // The groups of same-typed slots can be allocated
  // in any mutual order.
  // Methods like isTrackedSlot() depend on this property.
  // If this property doesn't hold, SlotMap should be changed:
  //   from Offset -> SlotID map
  //   to   Offset -> {SlotId, SlotFlags, SpBase} map

  llvm::DenseMap<int32_t, uint32_t> SlotMap;
  GcSlotId FirstTrackedSlot;
  size_t NumTrackedSlots;

#if !defined(NDEBUG)
  bool EmitLogs;
  std::ostringstream SlotStream;
  std::ostringstream LiveStream;
#endif // !NDEBUG

#if defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
  size_t NumCallSites;
  unsigned *CallSites;
  BYTE *CallSiteSizes;
#endif // defined(PARTIALLY_INTERRUPTIBLE_GC_SUPPORTED)
};

/// \brief MachineFunctionPass to record frame information
/// for special allocations in GcFuncInfo
class GcInfoRecorder : public llvm::MachineFunctionPass {
public:
  explicit GcInfoRecorder() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(llvm::MachineFunction &MF) override;

private:
  static char ID;
};

#endif // GCINFO_H
