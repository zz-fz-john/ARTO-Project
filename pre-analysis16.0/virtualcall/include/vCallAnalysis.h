#ifndef VCALL_H
#define VCALL_H

#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/PassManager.h"
#include <cassert>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/TypeMetadataUtils.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSummaryIndexYAML.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GlobPattern.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/FunctionAttrs.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/CallPromotionUtils.h"
#include "llvm/Transforms/Utils/Evaluator.h"
#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>

#include "llvm/Passes/PassBuilder.h"

namespace vcall{

    void runVirtualCallAnalysis(llvm::Module &M);

    // A bit vector that keeps track of which bits are used. We use this to
    // pack constant values compactly before and after each virtual table.
    struct AccumBitVector {
        std::vector<uint8_t> Bytes;

        // Bits in BytesUsed[I] are 1 if matching bit in Bytes[I] is used, 0 if not.
        std::vector<uint8_t> BytesUsed;

        std::pair<uint8_t *, uint8_t *> getPtrToData(uint64_t Pos, uint8_t Size) {
            if (Bytes.size() < Pos + Size) {
            Bytes.resize(Pos + Size);
            BytesUsed.resize(Pos + Size);
            }
            return std::make_pair(Bytes.data() + Pos, BytesUsed.data() + Pos);
        }

        // Set little-endian value Val with size Size at bit position Pos,
        // and mark bytes as used.
        void setLE(uint64_t Pos, uint64_t Val, uint8_t Size) {
            assert(Pos % 8 == 0);
            auto DataUsed = getPtrToData(Pos / 8, Size);
            for (unsigned I = 0; I != Size; ++I) {
            DataUsed.first[I] = Val >> (I * 8);
            assert(!DataUsed.second[I]);
            DataUsed.second[I] = 0xff;
            }
        }

        // Set big-endian value Val with size Size at bit position Pos,
        // and mark bytes as used.
        void setBE(uint64_t Pos, uint64_t Val, uint8_t Size) {
            assert(Pos % 8 == 0);
            auto DataUsed = getPtrToData(Pos / 8, Size);
            for (unsigned I = 0; I != Size; ++I) {
            DataUsed.first[Size - I - 1] = Val >> (I * 8);
            assert(!DataUsed.second[Size - I - 1]);
            DataUsed.second[Size - I - 1] = 0xff;
            }
        }

        // Set bit at bit position Pos to b and mark bit as used.
        void setBit(uint64_t Pos, bool b) {
            auto DataUsed = getPtrToData(Pos / 8, 1);
            if (b)
            *DataUsed.first |= 1 << (Pos % 8);
            assert(!(*DataUsed.second & (1 << Pos % 8)));
            *DataUsed.second |= 1 << (Pos % 8);
        }
    };


    // The bits that will be stored before and after a particular vtable.
    struct VTableBits {
        // The vtable global.
        llvm::GlobalVariable *GV;

        // Cache of the vtable's size in bytes.
        uint64_t ObjectSize = 0;

        // The bit vector that will be laid out before the vtable. Note that these
        // bytes are stored in reverse order until the globals are rebuilt. This means
        // that any values in the array must be stored using the opposite endianness
        // from the target.
        AccumBitVector Before;

        // The bit vector that will be laid out after the vtable.
        AccumBitVector After; 
    };

    // Information about a member of a particular type identifier.
    struct TypeMemberInfo {
        // The VTableBits for the vtable.
        VTableBits *Bits;

        // The offset in bytes from the start of the vtable (i.e. the address point).
        uint64_t Offset;

        bool operator<(const TypeMemberInfo &other) const {
            return Bits < other.Bits || (Bits == other.Bits && Offset < other.Offset);
        }
    };

    struct VirtualCallTarget {
        VirtualCallTarget(llvm::Function *Fn, const TypeMemberInfo *TM);

        // The function stored in the vtable.
        llvm::Function *Fn; 

        // A pointer to the type identifier member through which the pointer to Fn is
        // accessed.
        const TypeMemberInfo *TM;

        // When doing virtual constant propagation, this stores the return value for
        // the function when passed the currently considered argument list.
        uint64_t RetVal;

        // Whether the target is big endian.
        bool IsBigEndian;

        // Whether at least one call site to the target was devirtualized.
        bool WasDevirt;

        // The minimum byte offset before the address point. This covers the bytes in
        // the vtable object before the address point (e.g. RTTI, access-to-top,
        // vtables for other base classes) and is equal to the offset from the start
        // of the vtable object to the address point.
        uint64_t minBeforeBytes() const { return TM->Offset; }

        // The minimum byte offset after the address point. This covers the bytes in
        // the vtable object after the address point (e.g. the vtable for the current
        // class and any later base classes) and is equal to the size of the vtable
        // object minus the offset from the start of the vtable object to the address
        // point.
        uint64_t minAfterBytes() const { return TM->Bits->ObjectSize - TM->Offset; }

        // The number of bytes allocated (for the vtable plus the byte array) before
        // the address point.
        uint64_t allocatedBeforeBytes() const {
            return minBeforeBytes() + TM->Bits->Before.Bytes.size();
        }

        // The number of bytes allocated (for the vtable plus the byte array) after
        // the address point.
        uint64_t allocatedAfterBytes() const {
            return minAfterBytes() + TM->Bits->After.Bytes.size();
        }

        // Set the bit at position Pos before the address point to RetVal.
        void setBeforeBit(uint64_t Pos) {
            assert(Pos >= 8 * minBeforeBytes());
            TM->Bits->Before.setBit(Pos - 8 * minBeforeBytes(), RetVal);
        }

        // Set the bit at position Pos after the address point to RetVal.
        void setAfterBit(uint64_t Pos) {
            assert(Pos >= 8 * minAfterBytes());
            TM->Bits->After.setBit(Pos - 8 * minAfterBytes(), RetVal);
        }

        // Set the bytes at position Pos before the address point to RetVal.
        // Because the bytes in Before are stored in reverse order, we use the
        // opposite endianness to the target.
        void setBeforeBytes(uint64_t Pos, uint8_t Size) {
            assert(Pos >= 8 * minBeforeBytes());
            if (IsBigEndian)
            TM->Bits->Before.setLE(Pos - 8 * minBeforeBytes(), RetVal, Size);
            else
            TM->Bits->Before.setBE(Pos - 8 * minBeforeBytes(), RetVal, Size);
        }

        // Set the bytes at position Pos after the address point to RetVal.
        void setAfterBytes(uint64_t Pos, uint8_t Size) {
            assert(Pos >= 8 * minAfterBytes());
            if (IsBigEndian)
            TM->Bits->After.setBE(Pos - 8 * minAfterBytes(), RetVal, Size);
            else
            TM->Bits->After.setLE(Pos - 8 * minAfterBytes(), RetVal, Size);
        }
    };

    struct VTableSlot {
        llvm::Metadata *TypeID;
        uint64_t ByteOffset;
    };

    struct VirtualCallSite {
        llvm::Value *VTable = nullptr;
        llvm::CallBase &CB;
    };

    struct CallSiteInfo {
        std::vector<VirtualCallSite> CallSites;
    };

    struct VTableSlotInfo {
        // The set of call sites which do not have all constant integer arguments
        // (excluding "this").
        CallSiteInfo CSInfo;

        // The set of call sites with all constant integer arguments (excluding
        // "this"), grouped by argument list.
        std::map<std::vector<uint64_t>, CallSiteInfo> ConstCSInfo;

        void addCallSite(llvm::Value *VTable, llvm::CallBase &CB);

    private:
        CallSiteInfo &findCallSiteInfo(llvm::CallBase &CB);
    };

    struct TargetModule {
        llvm::Module &M;
        llvm::function_ref<llvm::AAResults &(llvm::Function &)> AARGetter;
        llvm::function_ref<llvm::DominatorTree &(llvm::Function &)> LookupDomTree;

        llvm::IntegerType *Int8Ty;
        llvm::PointerType *Int8PtrTy;
        llvm::IntegerType *Int32Ty;
        llvm::IntegerType *Int64Ty;
        llvm::IntegerType *IntPtrTy;
        /// Sizeless array type, used for imported vtables. This provides a signal
        /// to analyzers that these imports may alias, as they do for example
        /// when multiple unique return values occur in the same vtable.
        llvm::ArrayType *Int8Arr0Ty;

        llvm::function_ref<llvm::OptimizationRemarkEmitter &(llvm::Function *)> OREGetter;

        llvm::MapVector<VTableSlot, VTableSlotInfo> CallSlots;

        TargetModule(llvm::Module &M, llvm::function_ref<llvm::AAResults &(llvm::Function &)> AARGetter,
                    llvm::function_ref<llvm::OptimizationRemarkEmitter &(llvm::Function *)> OREGetter,
                    llvm::function_ref<llvm::DominatorTree &(llvm::Function &)> LookupDomTree)
            : M(M), AARGetter(AARGetter), LookupDomTree(LookupDomTree),
                Int8Ty(llvm::Type::getInt8Ty(M.getContext())),
                Int8PtrTy(llvm::Type::getInt8PtrTy(M.getContext())),
                Int32Ty(llvm::Type::getInt32Ty(M.getContext())),
                Int64Ty(llvm::Type::getInt64Ty(M.getContext())),
                IntPtrTy(M.getDataLayout().getIntPtrType(M.getContext(), 0)),
                Int8Arr0Ty(llvm::ArrayType::get(llvm::Type::getInt8Ty(M.getContext()), 0)),
                OREGetter(OREGetter) {}

        void
        scanTypeTestUsers(llvm::Function *TypeTestFunc,
                            llvm::DenseMap<llvm::Metadata *, std::set<TypeMemberInfo>> &TypeIdMap);

        void buildTypeIdentifierMap(
            std::vector<VTableBits> &Bits,
            llvm::DenseMap<llvm::Metadata *, std::set<TypeMemberInfo>> &TypeIdMap);

        bool
        tryFindVirtualCallTargets(std::vector<VirtualCallTarget> &TargetsForSlot,
                                    const std::set<TypeMemberInfo> &TypeMemberInfos,
                                    uint64_t ByteOffset);

        bool run();
    };


    struct VCallFinderPass : public llvm::PassInfoMixin<VCallFinderPass> {
        VCallFinderPass() {}
        llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &);
    };
}

namespace llvm{
    template <> struct DenseMapInfo<vcall::VTableSlot> {
        static vcall::VTableSlot getEmptyKey() {
            return {DenseMapInfo<Metadata *>::getEmptyKey(),
                    DenseMapInfo<uint64_t>::getEmptyKey()};
        }
        static vcall::VTableSlot getTombstoneKey() {
            return {DenseMapInfo<Metadata *>::getTombstoneKey(),
                    DenseMapInfo<uint64_t>::getTombstoneKey()};
        }
        static unsigned getHashValue(const vcall::VTableSlot &I) {
            return DenseMapInfo<Metadata *>::getHashValue(I.TypeID) ^
                DenseMapInfo<uint64_t>::getHashValue(I.ByteOffset);
        }
        static bool isEqual(const vcall::VTableSlot &LHS,
                            const vcall::VTableSlot &RHS) {
            return LHS.TypeID == RHS.TypeID && LHS.ByteOffset == RHS.ByteOffset;
        }
    };
}

#endif