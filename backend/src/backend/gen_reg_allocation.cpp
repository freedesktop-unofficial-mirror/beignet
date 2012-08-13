/* 
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Benjamin Segovia <benjamin.segovia@intel.com>
 */

/**
 * \file gen_reg_allocation.cpp
 * \author Benjamin Segovia <benjamin.segovia@intel.com>
 */

#include "ir/profile.hpp"
#include "ir/function.hpp"
#include "backend/gen_insn_selection.hpp"
#include "backend/gen_reg_allocation.hpp"
#include "backend/program.hpp"
#include <algorithm>
#include <climits>

namespace gbe
{
  // Note that byte vector registers use two bytes per byte (and can be
  // interleaved)
  static const size_t familyVectorSize[] = {2,2,2,4,8};
  static const size_t familyScalarSize[] = {2,1,2,4,8};

  /*! Interval as used in linear scan allocator. Basically, stores the first and
   *  the last instruction where the register is alive
   */
  struct GenRegInterval {
    INLINE GenRegInterval(ir::Register reg) :
      reg(reg), minID(INT_MAX), maxID(-INT_MAX) {}
    ir::Register reg;     //!< (virtual) register of the interval
    int32_t minID, maxID; //!< Starting and ending points
  };

  GenRegAllocator::GenRegAllocator(GenContext &ctx) : ctx(ctx) {}
  GenRegAllocator::~GenRegAllocator(void) {}

#define INSERT_REG(SIMD16, SIMD8, SIMD1) \
  if (ctx.sel->isScalarOrBool(reg) == true) \
    RA.insert(std::make_pair(reg, GenReg::SIMD1(nr, subnr))); \
  else if (simdWidth == 8) \
    RA.insert(std::make_pair(reg, GenReg::SIMD8(nr, subnr))); \
  else if (simdWidth == 16) \
    RA.insert(std::make_pair(reg, GenReg::SIMD16(nr, subnr))); \
  else \
    NOT_SUPPORTED;

  void GenRegAllocator::allocatePayloadReg(gbe_curbe_type value,
                                           ir::Register reg,
                                           uint32_t subValue,
                                           uint32_t subOffset)
  {
    using namespace ir;
    const Kernel *kernel = ctx.getKernel();
    const Function &fn = ctx.getFunction();
    const uint32_t simdWidth = ctx.getSimdWidth();
    const int32_t curbeOffset = kernel->getCurbeOffset(value, subValue);
    if (curbeOffset >= 0) {
      const uint32_t offset = curbeOffset + subOffset;
      const ir::RegisterData data = fn.getRegisterData(reg);
      const ir::RegisterFamily family = data.family;
      const bool isScalar = ctx.isScalarOrBool(reg);
      const uint32_t typeSize = isScalar ? familyScalarSize[family] : familyVectorSize[family];
      const uint32_t nr = (offset + GEN_REG_SIZE) / GEN_REG_SIZE;
      const uint32_t subnr = ((offset + GEN_REG_SIZE) % GEN_REG_SIZE) / typeSize;
      switch (family) {
        case FAMILY_BOOL: INSERT_REG(uw1grf, uw1grf, uw1grf); break;
        case FAMILY_WORD: INSERT_REG(uw16grf, uw8grf, uw1grf); break;
        case FAMILY_BYTE: INSERT_REG(ub16grf, ub8grf, ub1grf); break;
        case FAMILY_DWORD: INSERT_REG(f16grf, f8grf, f1grf); break;
        default: NOT_SUPPORTED;
      }
      this->intervals[reg].minID = 0;
    }
  }

  void GenRegAllocator::createGenReg(const GenRegInterval &interval) {
    using namespace ir;
    const ir::Register reg = interval.reg;
    const uint32_t simdWidth = ctx.getSimdWidth();
    if (RA.contains(reg) == true) return; // already allocated
    GBE_ASSERT(ctx.isScalarReg(reg) == false);
    const bool isScalar = ctx.sel->isScalarOrBool(reg);
    const RegisterData regData = ctx.sel->getRegisterData(reg);
    const RegisterFamily family = regData.family;
    const uint32_t typeSize = isScalar ? familyScalarSize[family] : familyVectorSize[family];
    const uint32_t regSize = simdWidth*typeSize;
    uint32_t grfOffset;
    while ((grfOffset = ctx.allocate(regSize, regSize)) == 0) {
      IF_DEBUG(const bool success =) this->expire(interval);
      GBE_ASSERTM(success, "Register allocation failed");
    }
    if (grfOffset != 0) {
      const uint32_t nr = grfOffset / GEN_REG_SIZE;
      const uint32_t subnr = (grfOffset % GEN_REG_SIZE) / typeSize;
      switch (family) {
        case FAMILY_BOOL: INSERT_REG(uw1grf, uw1grf, uw1grf); break;
        case FAMILY_WORD: INSERT_REG(uw16grf, uw8grf, uw1grf); break;
        case FAMILY_BYTE: INSERT_REG(ub16grf, ub8grf, ub1grf); break;
        case FAMILY_DWORD: INSERT_REG(f16grf, f8grf, f1grf); break;
        default: NOT_SUPPORTED;
      }
    } else
      GBE_ASSERTM(false, "Unable to register allocate");
  }

#undef INSERT_REG

  bool GenRegAllocator::isAllocated(const SelectionVector *vector) const {
    const ir::Register first = vector->reg[0].reg;
    const auto it = vectorMap.find(first);

    // If the first register is not allocated we are done
    if (it == vectorMap.end())
      return false;

    // If there are more left registers than in the found vector, there are
    // still registers to allocate
    const SelectionVector *other = it->second.first;
    const uint32_t otherFirst = it->second.second;
    const uint32_t leftNum = other->regNum - otherFirst;
    if (leftNum < vector->regNum)
      return false;

    // Now check that all the registers in the already allocated vector match
    // the current vector
    for (uint32_t regID = 1; regID < vector->regNum; ++regID) {
       const ir::Register from = vector->reg[regID].reg;
       const ir::Register to = other->reg[regID + otherFirst].reg;
       if (from != to)
         return false;
    }
    return true;
  }

  void GenRegAllocator::coalesce(Selection &selection, SelectionVector *vector) {
    for (uint32_t regID = 0; regID < vector->regNum; ++regID) {
      const ir::Register reg = vector->reg[regID].reg;
      const auto it = this->vectorMap.find(reg);
      // case 1: the register is not already in a vector, so it can stay in this
      // vector. Note that local IDs are *non-scalar* special registers but will
      // require a MOV anyway since pre-allocated in the CURBE
      if (it == vectorMap.end() &&
          ctx.sel->isScalarOrBool(reg) == false &&
          ctx.isSpecialReg(reg) == false)
      {
        const VectorLocation location = std::make_pair(vector, regID);
        this->vectorMap.insert(std::make_pair(reg, location));
      }
      // case 2: the register is already in another vector, so we need to move
      // it to a temporary register.
      // TODO: we can do better than that if we analyze the liveness of the
      // already allocated registers in the vector.  If there is no inteference
      // and the order is maintained, we can reuse the previous vector and avoid
      // the MOVs
      else {
        ir::Register tmp;
        if (vector->isSrc)
          tmp = selection.replaceSrc(vector->insn, regID);
        else
          tmp = selection.replaceDst(vector->insn, regID);
        const VectorLocation location = std::make_pair(vector, regID);
        this->vectorMap.insert(std::make_pair(tmp, location));
      }
    }
  }

  /*! Will sort vector in decreasing order */
  inline bool cmp(const SelectionVector *v0, const SelectionVector *v1) {
    return v0->regNum > v1->regNum;
  }

  void GenRegAllocator::allocateVector(Selection &selection) {
    const uint32_t vectorNum = selection.getVectorNum();
    this->vectors.resize(vectorNum);

    // First we find and store all vectors
    uint32_t vectorID = 0;
    selection.foreach([&](SelectionBlock &block) {
      SelectionVector *v = block.vector;
      while (v) {
        GBE_ASSERT(vectorID < vectorNum);
        this->vectors[vectorID++] = v;
        v = v->next;
      }
    });
    GBE_ASSERT(vectorID == vectorNum);

    // Heuristic (really simple...): sort them by the number of registers they
    // contain
    std::sort(this->vectors.begin(), this->vectors.end(), cmp);

    // Insert MOVs when this is required
    for (vectorID = 0; vectorID < vectorNum; ++vectorID) {
      SelectionVector *vector = this->vectors[vectorID];
      if (this->isAllocated(vector))
        continue;
      this->coalesce(selection, vector);
    }
  }

  template <bool sortStartingPoint>
  inline bool cmp(const GenRegInterval *i0, const GenRegInterval *i1) {
    return sortStartingPoint ? i0->minID < i1->minID : i0->maxID < i1->maxID;
  }

  bool GenRegAllocator::expire(const GenRegInterval &limit) {
    while (this->expiringID != ending.size()) {
      const GenRegInterval *toExpire = this->ending[this->expiringID];
      const ir::Register reg = toExpire->reg;
      if (toExpire->minID >= limit.maxID)
        return false;
      auto it = RA.find(reg);
      GBE_ASSERT(it != RA.end());

      // Case 1 - it does not belong to a vector. Just remove it
      if (vectorMap.contains(reg) == false) {
        const uint32_t offset = it->second.nr * GEN_REG_SIZE + it->second.subnr;
        ctx.deallocate(offset);
        this->expiringID++;
        return true;
      // Case 2 - check that the vector has not been already removed. If not,
      // since we equaled the intervals of all registers in the vector, we just
      // remove the complete vector
      } else {
        SelectionVector *vector = vectorMap.find(reg)->second.first;
        if (expired.contains(vector)) {
          this->expiringID++;
          continue;
        } else {
          const ir::Register first = vector->reg[0].reg;
          auto it = RA.find(first);
          GBE_ASSERT(it != RA.end());
          const uint32_t offset = it->second.nr * GEN_REG_SIZE + it->second.subnr;
          ctx.deallocate(offset);
          expired.insert(vector);
          this->expiringID++;
          return true;
        }
      }
    }

    // We were not able to expire anything
    return false;
  }

  void GenRegAllocator::allocate(Selection &selection) {
    using namespace ir;
    const Kernel *kernel = ctx.getKernel();
    const Function &fn = ctx.getFunction();
    const uint32_t simdWidth = ctx.getSimdWidth();
    GBE_ASSERT(fn.getProfile() == PROFILE_OCL);

    // Allocate all the vectors first since they need to be contiguous
    this->allocateVector(selection);

    // Now start the linear scan allocation
    for (uint32_t regID = 0; regID < ctx.sel->regNum(); ++regID)
      this->intervals.push_back(ir::Register(regID));

    // Allocate the special registers (only those which are actually used)
    allocatePayloadReg(GBE_CURBE_LOCAL_ID_X, ocl::lid0);
    allocatePayloadReg(GBE_CURBE_LOCAL_ID_Y, ocl::lid1);
    allocatePayloadReg(GBE_CURBE_LOCAL_ID_Z, ocl::lid2);
    allocatePayloadReg(GBE_CURBE_LOCAL_SIZE_X, ocl::lsize0);
    allocatePayloadReg(GBE_CURBE_LOCAL_SIZE_Y, ocl::lsize1);
    allocatePayloadReg(GBE_CURBE_LOCAL_SIZE_Z, ocl::lsize2);
    allocatePayloadReg(GBE_CURBE_GLOBAL_SIZE_X, ocl::gsize0);
    allocatePayloadReg(GBE_CURBE_GLOBAL_SIZE_Y, ocl::gsize1);
    allocatePayloadReg(GBE_CURBE_GLOBAL_SIZE_Z, ocl::gsize2);
    allocatePayloadReg(GBE_CURBE_GLOBAL_OFFSET_X, ocl::goffset0);
    allocatePayloadReg(GBE_CURBE_GLOBAL_OFFSET_Y, ocl::goffset1);
    allocatePayloadReg(GBE_CURBE_GLOBAL_OFFSET_Z, ocl::goffset2);
    allocatePayloadReg(GBE_CURBE_GROUP_NUM_X, ocl::numgroup0);
    allocatePayloadReg(GBE_CURBE_GROUP_NUM_Y, ocl::numgroup1);
    allocatePayloadReg(GBE_CURBE_GROUP_NUM_Z, ocl::numgroup2);
    allocatePayloadReg(GBE_CURBE_STACK_POINTER, ocl::stackptr);

    // Group IDs are always allocated by the hardware in r0
    RA.insert(std::make_pair(ocl::groupid0, GenReg::f1grf(0, 1)));
    RA.insert(std::make_pair(ocl::groupid1, GenReg::f1grf(0, 6)));
    RA.insert(std::make_pair(ocl::groupid2, GenReg::f1grf(0, 7)));

    // block IP used to handle the mask in SW is always allocated
    int32_t blockIPOffset = GEN_REG_SIZE + kernel->getCurbeOffset(GBE_CURBE_BLOCK_IP,0);
    GBE_ASSERT(blockIPOffset >= 0 && blockIPOffset % GEN_REG_SIZE == 0);
    blockIPOffset /= GEN_REG_SIZE;
    if (simdWidth == 8)
      RA.insert(std::make_pair(ocl::blockip, GenReg::uw8grf(blockIPOffset, 0)));
    else if (simdWidth == 16)
      RA.insert(std::make_pair(ocl::blockip, GenReg::uw16grf(blockIPOffset, 0)));
    else
      NOT_SUPPORTED;
    this->intervals[ocl::blockip].minID = 0;

    // Allocate all (non-structure) argument parameters
    const uint32_t argNum = fn.argNum();
    for (uint32_t argID = 0; argID < argNum; ++argID) {
      const FunctionArgument &arg = fn.getArg(argID);
      GBE_ASSERT(arg.type == FunctionArgument::GLOBAL_POINTER ||
                 arg.type == FunctionArgument::CONSTANT_POINTER ||
                 arg.type == FunctionArgument::VALUE ||
                 arg.type == FunctionArgument::STRUCTURE);
      allocatePayloadReg(GBE_CURBE_KERNEL_ARGUMENT, arg.reg, argID);
    }

    // Allocate all pushed registers (i.e. structure kernel arguments)
    const Function::PushMap &pushMap = fn.getPushMap();
    for (const auto &pushed : pushMap) {
      const uint32_t argID = pushed.second.argID;
      const uint32_t subOffset = pushed.second.offset;
      const Register reg = pushed.second.getRegister();
      allocatePayloadReg(GBE_CURBE_KERNEL_ARGUMENT, reg, argID, subOffset);
    }

    // Compute the intervals
    int32_t insnID = 0;
    selection.foreach([&](const SelectionBlock &block) {
      int32_t lastID = insnID;
      // Update the intervals of each used register. Note that we do not
      // register allocate R0, so we skip all sub-registers in r0
      block.foreach([&](const SelectionInstruction &insn) {
        const uint32_t srcNum = insn.srcNum, dstNum = insn.dstNum;
        for (uint32_t srcID = 0; srcID < srcNum; ++srcID) {
          const SelectionReg &selReg = insn.src[srcID];
          const ir::Register reg = selReg.reg;
          if (selReg.file != GEN_GENERAL_REGISTER_FILE ||
              reg == ir::ocl::groupid0 ||
              reg == ir::ocl::groupid1 ||
              reg == ir::ocl::groupid2)
            continue;
          this->intervals[reg].minID = min(this->intervals[reg].minID, insnID);
          this->intervals[reg].maxID = max(this->intervals[reg].maxID, insnID);
        }
        for (uint32_t dstID = 0; dstID < dstNum; ++dstID) {
          const SelectionReg &selReg = insn.dst[dstID];
          const ir::Register reg = selReg.reg;
          if (selReg.file != GEN_GENERAL_REGISTER_FILE ||
              reg == ir::ocl::groupid0 ||
              reg == ir::ocl::groupid1 ||
              reg == ir::ocl::groupid2)
            continue;
          this->intervals[reg].minID = min(this->intervals[reg].minID, insnID);
          this->intervals[reg].maxID = max(this->intervals[reg].maxID, insnID);
        }
        lastID = insnID;
        insnID++;
      });

      // All registers alive at the end of the block must have their intervals
      // updated as well
      const ir::BasicBlock *bb = block.bb;
      const ir::Liveness::LiveOut &liveOut = ctx.getLiveOut(bb);
      for (auto reg : liveOut) {
        this->intervals[reg].minID = min(this->intervals[reg].minID, lastID);
        this->intervals[reg].maxID = max(this->intervals[reg].maxID, lastID);
      }
    });

    // Extend the liveness of the registers that belong to vectors. Actually,
    // this is way too brutal, we should instead maintain a list of allocated
    // intervals to handle vector registers independently while doing the linear
    // scan (or anything else)
    for (auto vector : this->vectors) {
      const uint32_t regNum = vector->regNum;
      const ir::Register first = vector->reg[0].reg;
      int32_t minID = this->intervals[first].minID;
      int32_t maxID = this->intervals[first].maxID;
      for (uint32_t regID = 1; regID < regNum; ++regID) {
        const ir::Register reg = vector->reg[regID].reg;
        minID = min(minID, this->intervals[reg].minID);
        maxID = max(maxID, this->intervals[reg].maxID);
      }
      for (uint32_t regID = 0; regID < regNum; ++regID) {
        const ir::Register reg = vector->reg[regID].reg;
        this->intervals[reg].minID = minID;
        this->intervals[reg].maxID = maxID;
      }
    }

    // Sort both intervals in starting point and ending point increasing orders
    uint32_t regNum = ctx.sel->regNum();
    this->starting.resize(regNum);
    this->ending.resize(regNum);
    for (uint32_t regID = 0; regID < regNum; ++regID)
      this->starting[regID] = this->ending[regID] = &intervals[regID];
    std::sort(this->starting.begin(), this->starting.end(), cmp<true>);
    std::sort(this->ending.begin(), this->ending.end(), cmp<false>);

    // Remove the registers that were not allocated
    this->expiringID = 0;
    while (this->expiringID < regNum) {
      const GenRegInterval *interval = ending[this->expiringID];
      if (interval->maxID == -INT_MAX)
        this->expiringID++;
      else
        break;
    }

    // Perform the linear scan allocator
    for (uint32_t startID = 0; startID < regNum; ++startID) {
      const GenRegInterval &interval = *this->starting[startID];
      const ir::Register reg = interval.reg;
      if (interval.maxID == -INT_MAX)
        continue; // Unused register
      if (RA.contains(reg))
        continue; // already allocated

      // Case 1: the register belongs to a vector, allocate all the registers in
      // one piece
      auto it = vectorMap.find(reg);
      if (it != vectorMap.end()) {
        const SelectionVector *vector = it->second.first;
        const uint32_t simdWidth = ctx.getSimdWidth();
        const uint32_t alignment = simdWidth * sizeof(uint32_t);
        const uint32_t size = vector->regNum * alignment;
        uint32_t grfOffset;
        while ((grfOffset = ctx.allocate(size, alignment)) == 0) {
          IF_DEBUG(const bool success =) this->expire(interval);
          GBE_ASSERTM(success, "Register allocation failed");
        }
        //GBE_ASSERTM(grfOffset != 0, "Unable to register allocate");
        for (uint32_t regID = 0; regID < vector->regNum; ++regID, grfOffset += alignment) {
          const ir::Register reg = vector->reg[regID].reg;
          const uint32_t nr = grfOffset / GEN_REG_SIZE;
          const uint32_t subnr = (grfOffset % GEN_REG_SIZE) / sizeof(uint32_t);
          GBE_ASSERT(RA.contains(reg) == false);
          if (simdWidth == 16)
            RA.insert(std::make_pair(reg, GenReg::f16grf(nr, subnr)));
          else if (simdWidth == 8)
            RA.insert(std::make_pair(reg, GenReg::f8grf(nr, subnr)));
          else
            NOT_SUPPORTED;
        }
      }
      // Case 2: This is a regular scalar register, allocate it alone
      else
        this->createGenReg(interval);
    }
  }

  INLINE void setGenReg(GenReg &dst, const SelectionReg &src) {
    dst.type = src.type;
    dst.file = src.file;
    dst.negation = src.negation;
    dst.absolute = src.absolute;
    dst.vstride = src.vstride;
    dst.width = src.width;
    dst.hstride = src.hstride;
    dst.address_mode = GEN_ADDRESS_DIRECT;
    dst.dw1.ud = src.immediate.ud;
  }

  GenReg GenRegAllocator::genReg(const SelectionReg &reg) {
    // Right now, only GRF are allocated (TODO bool) ...
    if (reg.file == GEN_GENERAL_REGISTER_FILE) {
      GBE_ASSERT(RA.contains(reg.reg) != false);
      GenReg dst = RA.find(reg.reg)->second;
      setGenReg(dst, reg);
      if (reg.quarter != 0)
        dst = GenReg::Qn(dst, reg.quarter);
      return dst;
    }
    // Other registers are already physical registers
    else {
      GenReg dst;
      setGenReg(dst, reg);
      dst.nr = reg.nr;
      dst.subnr = reg.subnr;
      return dst;
    }
  }

} /* namespace gbe */
