//===--------- command_buffer.cpp - HIP Adapter ---------------------------===//
//
// Copyright (C) 2023 Intel Corporation
//
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "command_buffer.hpp"

#include <hip/hip_runtime.h>

#include "common.hpp"
#include "enqueue.hpp"
#include "event.hpp"
#include "kernel.hpp"
#include "memory.hpp"
#include "queue.hpp"

#include <cstring>

namespace {
ur_result_t
commandBufferReleaseInternal(ur_exp_command_buffer_handle_t CommandBuffer) {
  if (CommandBuffer->decrementInternalReferenceCount() != 0) {
    return UR_RESULT_SUCCESS;
  }

  delete CommandBuffer;
  return UR_RESULT_SUCCESS;
}

ur_result_t
commandHandleReleaseInternal(ur_exp_command_buffer_command_handle_t Command) {
  if (Command->decrementInternalReferenceCount() != 0) {
    return UR_RESULT_SUCCESS;
  }

  // Decrement parent command-buffer internal ref count
  commandBufferReleaseInternal(Command->CommandBuffer);

  delete Command;
  return UR_RESULT_SUCCESS;
}
} // end anonymous namespace

ur_exp_command_buffer_handle_t_::ur_exp_command_buffer_handle_t_(
    ur_context_handle_t hContext, ur_device_handle_t hDevice, bool IsUpdatable)
    : Context(hContext), Device(hDevice),
      IsUpdatable(IsUpdatable), HIPGraph{nullptr}, HIPGraphExec{nullptr},
      RefCountInternal{1}, RefCountExternal{1} {
  urContextRetain(hContext);
  urDeviceRetain(hDevice);
}

/// The ur_exp_command_buffer_handle_t_ destructor releases
/// all the memory objects allocated for command_buffer managment
ur_exp_command_buffer_handle_t_::~ur_exp_command_buffer_handle_t_() {
  // Release the memory allocated to the Context stored in the command_buffer
  UR_TRACE(urContextRelease(Context));

  // Release the device
  UR_TRACE(urDeviceRelease(Device));

  // Release the memory allocated to the HIPGraph
  UR_CHECK_ERROR(hipGraphDestroy(HIPGraph));

  // Release the memory allocated to the HIPGraphExec
  if (HIPGraphExec) {
    UR_CHECK_ERROR(hipGraphExecDestroy(HIPGraphExec));
  }
}

ur_exp_command_buffer_command_handle_t_::
    ur_exp_command_buffer_command_handle_t_(
        ur_exp_command_buffer_handle_t CommandBuffer, ur_kernel_handle_t Kernel,
        std::shared_ptr<hipGraphNode_t> &&Node, hipKernelNodeParams Params,
        uint32_t WorkDim, const size_t *GlobalWorkOffsetPtr,
        const size_t *GlobalWorkSizePtr, const size_t *LocalWorkSizePtr)
    : CommandBuffer(CommandBuffer), Kernel(Kernel), Node(std::move(Node)),
      Params(Params), WorkDim(WorkDim), RefCountInternal(1),
      RefCountExternal(1) {
  CommandBuffer->incrementInternalReferenceCount();

  const size_t CopySize = sizeof(size_t) * WorkDim;
  std::memcpy(GlobalWorkOffset, GlobalWorkOffsetPtr, CopySize);
  std::memcpy(GlobalWorkSize, GlobalWorkSizePtr, CopySize);
  // Local work size may be nullptr
  if (LocalWorkSizePtr) {
    std::memcpy(LocalWorkSize, LocalWorkSizePtr, CopySize);
  } else {
    std::memset(LocalWorkSize, 0, sizeof(size_t) * 3);
  }

  if (WorkDim < 3) {
    const size_t ZeroSize = sizeof(size_t) * (3 - WorkDim);
    std::memset(GlobalWorkOffset + WorkDim, 0, ZeroSize);
    std::memset(GlobalWorkSize + WorkDim, 0, ZeroSize);
  }
}

/// Helper function for finding the HIP Nodes associated with the commands in a
/// command-buffer, each event is pointed to by a sync-point in the wait list.
///
/// @param[in] CommandBuffer to lookup the events from.
/// @param[in] NumSyncPointsInWaitList Length of \p SyncPointWaitList.
/// @param[in] SyncPointWaitList List of sync points in \p CommandBuffer
/// to find the events for.
/// @param[out] HipNodesList Return parameter for the HIP Nodes associated with
/// each sync-point in \p SyncPointWaitList.
///
/// @return UR_RESULT_SUCCESS or an error code on failure
static ur_result_t getNodesFromSyncPoints(
    const ur_exp_command_buffer_handle_t &CommandBuffer,
    size_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    std::vector<hipGraphNode_t> &HIPNodesList) {
  // Map of ur_exp_command_buffer_sync_point_t to ur_event_handle_t defining
  // the event associated with each sync-point
  auto SyncPoints = CommandBuffer->SyncPoints;

  // For each sync-point add associated HIP graph node to the return list.
  for (size_t i = 0; i < NumSyncPointsInWaitList; i++) {
    if (auto NodeHandle = SyncPoints.find(SyncPointWaitList[i]);
        NodeHandle != SyncPoints.end()) {
      HIPNodesList.push_back(*NodeHandle->second.get());
    } else {
      return UR_RESULT_ERROR_INVALID_VALUE;
    }
  }
  return UR_RESULT_SUCCESS;
}

// Helper function for enqueuing memory fills
static ur_result_t enqueueCommandBufferFillHelper(
    ur_exp_command_buffer_handle_t CommandBuffer, void *DstDevice,
    const hipMemoryType DstType, const void *Pattern, size_t PatternSize,
    size_t Size, uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *SyncPoint) {
  std::vector<hipGraphNode_t> DepsList;

  {
    ur_result_t Result = UR_RESULT_SUCCESS;
    UR_CALL(getNodesFromSyncPoints(CommandBuffer, NumSyncPointsInWaitList,
                                   SyncPointWaitList, DepsList),
            Result);

    if (Result != UR_RESULT_SUCCESS) {
      return Result;
    }
  }

  try {
    const size_t N = Size / PatternSize;
    auto DstPtr = DstType == hipMemoryTypeDevice
                      ? *static_cast<hipDeviceptr_t *>(DstDevice)
                      : DstDevice;

    if ((PatternSize == 1) || (PatternSize == 2) || (PatternSize == 4)) {
      // Create a new node
      hipGraphNode_t GraphNode;
      hipMemsetParams NodeParams = {};
      NodeParams.dst = DstPtr;
      NodeParams.elementSize = PatternSize;
      NodeParams.height = N;
      NodeParams.pitch = PatternSize;
      NodeParams.width = 1;

      // pattern size in bytes
      switch (PatternSize) {
      case 1: {
        auto Value = *static_cast<const uint8_t *>(Pattern);
        NodeParams.value = Value;
        break;
      }
      case 2: {
        auto Value = *static_cast<const uint16_t *>(Pattern);
        NodeParams.value = Value;
        break;
      }
      case 4: {
        auto Value = *static_cast<const uint32_t *>(Pattern);
        NodeParams.value = Value;
        break;
      }
      }

      UR_CHECK_ERROR(hipGraphAddMemsetNode(&GraphNode, CommandBuffer->HIPGraph,
                                           DepsList.data(), DepsList.size(),
                                           &NodeParams));

      // Get sync point and register the node with it.
      *SyncPoint = CommandBuffer->addSyncPoint(
          std::make_shared<hipGraphNode_t>(GraphNode));

    } else {
      // HIP has no memset functions that allow setting values more than 4
      // bytes. UR API lets you pass an arbitrary "pattern" to the buffer
      // fill, which can be more than 4 bytes. We must break up the pattern
      // into 1 byte values, and set the buffer using multiple strided calls.
      // This means that one hipGraphAddMemsetNode call is made for every 1
      // bytes in the pattern.

      size_t NumberOfSteps = PatternSize / sizeof(uint8_t);

      // Shared pointer that will point to the last node created
      std::shared_ptr<hipGraphNode_t> GraphNodePtr;

      // Create a new node
      hipGraphNode_t GraphNodeFirst;
      // Update NodeParam
      hipMemsetParams NodeParamsStepFirst = {};
      NodeParamsStepFirst.dst = DstPtr;
      NodeParamsStepFirst.elementSize = 4;
      NodeParamsStepFirst.height = Size / sizeof(uint32_t);
      NodeParamsStepFirst.pitch = 4;
      NodeParamsStepFirst.value = *(static_cast<const uint32_t *>(Pattern));
      NodeParamsStepFirst.width = 1;

      UR_CHECK_ERROR(hipGraphAddMemsetNode(
          &GraphNodeFirst, CommandBuffer->HIPGraph, DepsList.data(),
          DepsList.size(), &NodeParamsStepFirst));

      // Get sync point and register the node with it.
      *SyncPoint = CommandBuffer->addSyncPoint(
          std::make_shared<hipGraphNode_t>(GraphNodeFirst));

      DepsList.clear();
      DepsList.push_back(GraphNodeFirst);

      // we walk up the pattern in 1-byte steps, and add Memset node for each
      // 1-byte chunk of the pattern.
      for (auto Step = 4u; Step < NumberOfSteps; ++Step) {
        // take 1 bytes of the pattern
        auto Value = *(static_cast<const uint8_t *>(Pattern) + Step);

        // offset the pointer to the part of the buffer we want to write to
        auto OffsetPtr = reinterpret_cast<void *>(
            reinterpret_cast<uint8_t *>(DstPtr) + (Step * sizeof(uint8_t)));

        // Create a new node
        hipGraphNode_t GraphNode;
        // Update NodeParam
        hipMemsetParams NodeParamsStep = {};
        NodeParamsStep.dst = reinterpret_cast<void *>(OffsetPtr);
        NodeParamsStep.elementSize = sizeof(uint8_t);
        NodeParamsStep.height = Size / NumberOfSteps;
        NodeParamsStep.pitch = NumberOfSteps * sizeof(uint8_t);
        NodeParamsStep.value = Value;
        NodeParamsStep.width = 1;

        UR_CHECK_ERROR(hipGraphAddMemsetNode(
            &GraphNode, CommandBuffer->HIPGraph, DepsList.data(),
            DepsList.size(), &NodeParamsStep));

        GraphNodePtr = std::make_shared<hipGraphNode_t>(GraphNode);
        // Get sync point and register the node with it.
        *SyncPoint = CommandBuffer->addSyncPoint(GraphNodePtr);

        DepsList.clear();
        DepsList.push_back(*GraphNodePtr.get());
      }
    }
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferCreateExp(
    ur_context_handle_t hContext, ur_device_handle_t hDevice,
    const ur_exp_command_buffer_desc_t *pCommandBufferDesc,
    ur_exp_command_buffer_handle_t *phCommandBuffer) {
  const bool IsUpdatable =
      pCommandBufferDesc ? pCommandBufferDesc->isUpdatable : false;

  try {
    *phCommandBuffer =
        new ur_exp_command_buffer_handle_t_(hContext, hDevice, IsUpdatable);
  } catch (const std::bad_alloc &) {
    return UR_RESULT_ERROR_OUT_OF_HOST_MEMORY;
  } catch (...) {
    return UR_RESULT_ERROR_UNKNOWN;
  }

  try {
    UR_CHECK_ERROR(hipGraphCreate(&(*phCommandBuffer)->HIPGraph, 0));
  } catch (...) {
    return UR_RESULT_ERROR_OUT_OF_RESOURCES;
  }

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL
urCommandBufferRetainExp(ur_exp_command_buffer_handle_t hCommandBuffer) {
  hCommandBuffer->incrementInternalReferenceCount();
  hCommandBuffer->incrementExternalReferenceCount();
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL
urCommandBufferReleaseExp(ur_exp_command_buffer_handle_t hCommandBuffer) {
  if (hCommandBuffer->decrementExternalReferenceCount() == 0) {
    // External ref count has reached zero, internal release of created
    // commands.
    for (auto Command : hCommandBuffer->CommandHandles) {
      commandHandleReleaseInternal(Command);
    }
  }

  return commandBufferReleaseInternal(hCommandBuffer);
}

UR_APIEXPORT ur_result_t UR_APICALL
urCommandBufferFinalizeExp(ur_exp_command_buffer_handle_t hCommandBuffer) {
  try {
    const unsigned long long flags = 0;
    UR_CHECK_ERROR(hipGraphInstantiateWithFlags(
        &hCommandBuffer->HIPGraphExec, hCommandBuffer->HIPGraph, flags));
  } catch (...) {
    return UR_RESULT_ERROR_UNKNOWN;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendKernelLaunchExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_kernel_handle_t hKernel,
    uint32_t workDim, const size_t *pGlobalWorkOffset,
    const size_t *pGlobalWorkSize, const size_t *pLocalWorkSize,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint,
    ur_exp_command_buffer_command_handle_t *phCommand) {
  // Preconditions
  UR_ASSERT(hCommandBuffer->Context == hKernel->getContext(),
            UR_RESULT_ERROR_INVALID_KERNEL);
  UR_ASSERT(workDim > 0, UR_RESULT_ERROR_INVALID_WORK_DIMENSION);
  UR_ASSERT(workDim < 4, UR_RESULT_ERROR_INVALID_WORK_DIMENSION);
  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);

  hipGraphNode_t GraphNode;
  std::vector<hipGraphNode_t> DepsList;

  ur_result_t Result = UR_RESULT_SUCCESS;
  UR_CALL(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                 pSyncPointWaitList, DepsList),
          Result);

  if (Result != UR_RESULT_SUCCESS) {
    return Result;
  }

  if (*pGlobalWorkSize == 0) {
    try {
      // Create an empty node if the kernel workload size is zero
      UR_CHECK_ERROR(hipGraphAddEmptyNode(&GraphNode, hCommandBuffer->HIPGraph,
                                          DepsList.data(), DepsList.size()));

      // Get sync point and register the node with it.
      *pSyncPoint = hCommandBuffer->addSyncPoint(
          std::make_shared<hipGraphNode_t>(GraphNode));
    } catch (ur_result_t Err) {
      return Err;
    }
    return UR_RESULT_SUCCESS;
  }

  // Set the number of threads per block to the number of threads per warp
  // by default unless user has provided a better number
  size_t ThreadsPerBlock[3] = {64u, 1u, 1u};
  size_t BlocksPerGrid[3] = {1u, 1u, 1u};

  uint32_t LocalSize = hKernel->getLocalSize();
  hipFunction_t HIPFunc = hKernel->get();
  UR_CALL(setKernelParams(hCommandBuffer->Device, workDim, pGlobalWorkOffset,
                          pGlobalWorkSize, pLocalWorkSize, hKernel, HIPFunc,
                          ThreadsPerBlock, BlocksPerGrid),
          Result);
  if (Result != UR_RESULT_SUCCESS) {
    return Result;
  }

  try {
    // Set node param structure with the kernel related data
    auto &ArgIndices = hKernel->getArgIndices();
    hipKernelNodeParams NodeParams;
    NodeParams.func = HIPFunc;
    NodeParams.gridDim.x = BlocksPerGrid[0];
    NodeParams.gridDim.y = BlocksPerGrid[1];
    NodeParams.gridDim.z = BlocksPerGrid[2];
    NodeParams.blockDim.x = ThreadsPerBlock[0];
    NodeParams.blockDim.y = ThreadsPerBlock[1];
    NodeParams.blockDim.z = ThreadsPerBlock[2];
    NodeParams.sharedMemBytes = LocalSize;
    NodeParams.kernelParams = const_cast<void **>(ArgIndices.data());
    NodeParams.extra = nullptr;

    // Create and add an new kernel node to the HIP graph
    UR_CHECK_ERROR(hipGraphAddKernelNode(&GraphNode, hCommandBuffer->HIPGraph,
                                         DepsList.data(), DepsList.size(),
                                         &NodeParams));

    if (LocalSize != 0)
      hKernel->clearLocalSize();

    // Get sync point and register the node with it.
    auto NodeSP = std::make_shared<hipGraphNode_t>(GraphNode);
    if (pSyncPoint) {
      *pSyncPoint = hCommandBuffer->addSyncPoint(NodeSP);
    }

    auto NewCommand = new ur_exp_command_buffer_command_handle_t_{
        hCommandBuffer, hKernel,           std::move(NodeSP), NodeParams,
        workDim,        pGlobalWorkOffset, pGlobalWorkSize,   pLocalWorkSize};

    NewCommand->incrementInternalReferenceCount();
    hCommandBuffer->CommandHandles.push_back(NewCommand);

    if (phCommand) {
      *phCommand = NewCommand;
    }

  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMMemcpyExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, void *pDst, const void *pSrc,
    size_t size, uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  hipGraphNode_t GraphNode;
  std::vector<hipGraphNode_t> DepsList;

  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);

  {
    ur_result_t Result = UR_RESULT_SUCCESS;
    UR_CALL(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                   pSyncPointWaitList, DepsList),
            Result);

    if (Result != UR_RESULT_SUCCESS) {
      return Result;
    }
  }

  try {
    UR_CHECK_ERROR(hipGraphAddMemcpyNode1D(
        &GraphNode, hCommandBuffer->HIPGraph, DepsList.data(), DepsList.size(),
        pDst, pSrc, size, hipMemcpyHostToHost));

    // Get sync point and register the node with it.
    *pSyncPoint = hCommandBuffer->addSyncPoint(
        std::make_shared<hipGraphNode_t>(GraphNode));
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferCopyExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hSrcMem,
    ur_mem_handle_t hDstMem, size_t srcOffset, size_t dstOffset, size_t size,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  hipGraphNode_t GraphNode;
  std::vector<hipGraphNode_t> DepsList;

  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);
  UR_ASSERT(size + dstOffset <= std::get<BufferMem>(hDstMem->Mem).getSize(),
            UR_RESULT_ERROR_INVALID_SIZE);
  UR_ASSERT(size + srcOffset <= std::get<BufferMem>(hSrcMem->Mem).getSize(),
            UR_RESULT_ERROR_INVALID_SIZE);

  {
    ur_result_t Result = UR_RESULT_SUCCESS;
    UR_CALL(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                   pSyncPointWaitList, DepsList),
            Result);

    if (Result != UR_RESULT_SUCCESS) {
      return Result;
    }
  }

  try {
    auto Src = std::get<BufferMem>(hSrcMem->Mem)
                   .getPtrWithOffset(hCommandBuffer->Device, srcOffset);
    auto Dst = std::get<BufferMem>(hDstMem->Mem)
                   .getPtrWithOffset(hCommandBuffer->Device, dstOffset);

    UR_CHECK_ERROR(hipGraphAddMemcpyNode1D(
        &GraphNode, hCommandBuffer->HIPGraph, DepsList.data(), DepsList.size(),
        Dst, Src, size, hipMemcpyDeviceToDevice));

    // Get sync point and register the node with it.
    *pSyncPoint = hCommandBuffer->addSyncPoint(
        std::make_shared<hipGraphNode_t>(GraphNode));
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferCopyRectExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hSrcMem,
    ur_mem_handle_t hDstMem, ur_rect_offset_t srcOrigin,
    ur_rect_offset_t dstOrigin, ur_rect_region_t region, size_t srcRowPitch,
    size_t srcSlicePitch, size_t dstRowPitch, size_t dstSlicePitch,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  hipGraphNode_t GraphNode;
  std::vector<hipGraphNode_t> DepsList;

  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);

  {
    ur_result_t Result = UR_RESULT_SUCCESS;
    UR_CALL(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                   pSyncPointWaitList, DepsList),
            Result);

    if (Result != UR_RESULT_SUCCESS) {
      return Result;
    }
  }

  try {
    auto SrcPtr =
        std::get<BufferMem>(hSrcMem->Mem).getPtr(hCommandBuffer->Device);
    auto DstPtr =
        std::get<BufferMem>(hDstMem->Mem).getPtr(hCommandBuffer->Device);
    hipMemcpy3DParms NodeParams = {};

    setCopyRectParams(region, SrcPtr, hipMemoryTypeDevice, srcOrigin,
                      srcRowPitch, srcSlicePitch, DstPtr, hipMemoryTypeDevice,
                      dstOrigin, dstRowPitch, dstSlicePitch, NodeParams);

    UR_CHECK_ERROR(hipGraphAddMemcpyNode(&GraphNode, hCommandBuffer->HIPGraph,
                                         DepsList.data(), DepsList.size(),
                                         &NodeParams));

    // Get sync point and register the node with it.
    *pSyncPoint = hCommandBuffer->addSyncPoint(
        std::make_shared<hipGraphNode_t>(GraphNode));
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT
ur_result_t UR_APICALL urCommandBufferAppendMemBufferWriteExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hBuffer,
    size_t offset, size_t size, const void *pSrc,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  hipGraphNode_t GraphNode;
  std::vector<hipGraphNode_t> DepsList;

  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);

  {
    ur_result_t Result = UR_RESULT_SUCCESS;
    UR_CALL(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                   pSyncPointWaitList, DepsList),
            Result);

    if (Result != UR_RESULT_SUCCESS) {
      return Result;
    }
  }

  try {
    auto Dst = std::get<BufferMem>(hBuffer->Mem)
                   .getPtrWithOffset(hCommandBuffer->Device, offset);

    UR_CHECK_ERROR(hipGraphAddMemcpyNode1D(
        &GraphNode, hCommandBuffer->HIPGraph, DepsList.data(), DepsList.size(),
        Dst, pSrc, size, hipMemcpyHostToDevice));

    // Get sync point and register the node with it.
    *pSyncPoint = hCommandBuffer->addSyncPoint(
        std::make_shared<hipGraphNode_t>(GraphNode));
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT
ur_result_t UR_APICALL urCommandBufferAppendMemBufferReadExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hBuffer,
    size_t offset, size_t size, void *pDst, uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  hipGraphNode_t GraphNode;
  std::vector<hipGraphNode_t> DepsList;

  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);

  {
    ur_result_t Result = UR_RESULT_SUCCESS;
    UR_CALL(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                   pSyncPointWaitList, DepsList),
            Result);

    if (Result != UR_RESULT_SUCCESS) {
      return Result;
    }
  }

  try {
    auto Src = std::get<BufferMem>(hBuffer->Mem)
                   .getPtrWithOffset(hCommandBuffer->Device, offset);

    UR_CHECK_ERROR(hipGraphAddMemcpyNode1D(
        &GraphNode, hCommandBuffer->HIPGraph, DepsList.data(), DepsList.size(),
        pDst, Src, size, hipMemcpyDeviceToHost));

    // Get sync point and register the node with it.
    *pSyncPoint = hCommandBuffer->addSyncPoint(
        std::make_shared<hipGraphNode_t>(GraphNode));
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT
ur_result_t UR_APICALL urCommandBufferAppendMemBufferWriteRectExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hBuffer,
    ur_rect_offset_t bufferOffset, ur_rect_offset_t hostOffset,
    ur_rect_region_t region, size_t bufferRowPitch, size_t bufferSlicePitch,
    size_t hostRowPitch, size_t hostSlicePitch, void *pSrc,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  hipGraphNode_t GraphNode;
  std::vector<hipGraphNode_t> DepsList;

  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);

  {
    ur_result_t Result = UR_RESULT_SUCCESS;
    UR_CALL(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                   pSyncPointWaitList, DepsList),
            Result);

    if (Result != UR_RESULT_SUCCESS) {
      return Result;
    }
  }

  try {
    auto DstPtr =
        std::get<BufferMem>(hBuffer->Mem).getPtr(hCommandBuffer->Device);
    hipMemcpy3DParms NodeParams = {};

    setCopyRectParams(region, pSrc, hipMemoryTypeHost, hostOffset, hostRowPitch,
                      hostSlicePitch, DstPtr, hipMemoryTypeDevice, bufferOffset,
                      bufferRowPitch, bufferSlicePitch, NodeParams);

    UR_CHECK_ERROR(hipGraphAddMemcpyNode(&GraphNode, hCommandBuffer->HIPGraph,
                                         DepsList.data(), DepsList.size(),
                                         &NodeParams));

    // Get sync point and register the node with it.
    *pSyncPoint = hCommandBuffer->addSyncPoint(
        std::make_shared<hipGraphNode_t>(GraphNode));
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT
ur_result_t UR_APICALL urCommandBufferAppendMemBufferReadRectExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hBuffer,
    ur_rect_offset_t bufferOffset, ur_rect_offset_t hostOffset,
    ur_rect_region_t region, size_t bufferRowPitch, size_t bufferSlicePitch,
    size_t hostRowPitch, size_t hostSlicePitch, void *pDst,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  hipGraphNode_t GraphNode;
  std::vector<hipGraphNode_t> DepsList;

  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);

  {
    ur_result_t Result = UR_RESULT_SUCCESS;
    UR_CALL(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                   pSyncPointWaitList, DepsList),
            Result);

    if (Result != UR_RESULT_SUCCESS) {
      return Result;
    }
  }

  try {
    auto SrcPtr =
        std::get<BufferMem>(hBuffer->Mem).getPtr(hCommandBuffer->Device);
    hipMemcpy3DParms NodeParams = {};

    setCopyRectParams(region, SrcPtr, hipMemoryTypeDevice, bufferOffset,
                      bufferRowPitch, bufferSlicePitch, pDst, hipMemoryTypeHost,
                      hostOffset, hostRowPitch, hostSlicePitch, NodeParams);

    UR_CHECK_ERROR(hipGraphAddMemcpyNode(&GraphNode, hCommandBuffer->HIPGraph,
                                         DepsList.data(), DepsList.size(),
                                         &NodeParams));

    // Get sync point and register the node with it.
    *pSyncPoint = hCommandBuffer->addSyncPoint(
        std::make_shared<hipGraphNode_t>(GraphNode));
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMPrefetchExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, const void * /* Mem */,
    size_t /*Size*/, ur_usm_migration_flags_t /*Flags*/,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  // Prefetch cmd is not supported by Hip Graph.
  // We implement it as an empty node to enforce dependencies.
  hipGraphNode_t GraphNode;
  std::vector<hipGraphNode_t> DepsList;

  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);

  {
    ur_result_t Result = UR_RESULT_SUCCESS;
    UR_CALL(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                   pSyncPointWaitList, DepsList),
            Result);

    if (Result != UR_RESULT_SUCCESS) {
      return Result;
    }
  }

  try {
    // Create an empty node if the kernel workload size is zero
    UR_CHECK_ERROR(hipGraphAddEmptyNode(&GraphNode, hCommandBuffer->HIPGraph,
                                        DepsList.data(), DepsList.size()));

    // Get sync point and register the node with it.
    *pSyncPoint = hCommandBuffer->addSyncPoint(
        std::make_shared<hipGraphNode_t>(GraphNode));

    setErrorMessage("Prefetch hint ignored and replaced with empty node as "
                    "prefetch is not supported by HIP Graph backend",
                    UR_RESULT_SUCCESS);
    return UR_RESULT_ERROR_ADAPTER_SPECIFIC;
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMAdviseExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, const void * /* Mem */,
    size_t /*Size*/, ur_usm_advice_flags_t /*Advice*/,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  // Mem-Advise cmd is not supported by Hip Graph.
  // We implement it as an empty node to enforce dependencies.
  hipGraphNode_t GraphNode;
  std::vector<hipGraphNode_t> DepsList;

  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);

  {
    ur_result_t Result = UR_RESULT_SUCCESS;
    UR_CALL(getNodesFromSyncPoints(hCommandBuffer, numSyncPointsInWaitList,
                                   pSyncPointWaitList, DepsList),
            Result);

    if (Result != UR_RESULT_SUCCESS) {
      return Result;
    }
  }

  try {
    // Create an empty node if the kernel workload size is zero
    UR_CHECK_ERROR(hipGraphAddEmptyNode(&GraphNode, hCommandBuffer->HIPGraph,
                                        DepsList.data(), DepsList.size()));

    // Get sync point and register the node with it.
    *pSyncPoint = hCommandBuffer->addSyncPoint(
        std::make_shared<hipGraphNode_t>(GraphNode));

    setErrorMessage("Memory advice ignored and replaced with empty node as "
                    "memory advice is not supported by HIP Graph backend",
                    UR_RESULT_SUCCESS);
    return UR_RESULT_ERROR_ADAPTER_SPECIFIC;
  } catch (ur_result_t Err) {
    return Err;
  }
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferFillExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_mem_handle_t hBuffer,
    const void *pPattern, size_t patternSize, size_t offset, size_t size,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {
  auto ArgsAreMultiplesOfPatternSize =
      (offset % patternSize == 0) || (size % patternSize == 0);

  auto PatternIsValid = (pPattern != nullptr);

  auto PatternSizeIsValid = ((patternSize & (patternSize - 1)) == 0) &&
                            (patternSize > 0); // is a positive power of two
  UR_ASSERT(ArgsAreMultiplesOfPatternSize && PatternIsValid &&
                PatternSizeIsValid,
            UR_RESULT_ERROR_INVALID_SIZE);
  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);

  auto DstDevice = std::get<BufferMem>(hBuffer->Mem)
                       .getPtrWithOffset(hCommandBuffer->Device, offset);

  return enqueueCommandBufferFillHelper(
      hCommandBuffer, &DstDevice, hipMemoryTypeDevice, pPattern, patternSize,
      size, numSyncPointsInWaitList, pSyncPointWaitList, pSyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMFillExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, void *pPtr,
    const void *pPattern, size_t patternSize, size_t size,
    uint32_t numSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *pSyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *pSyncPoint) {

  auto PatternIsValid = (pPattern != nullptr);

  auto PatternSizeIsValid = ((patternSize & (patternSize - 1)) == 0) &&
                            (patternSize > 0); // is a positive power of two

  UR_ASSERT(!(pSyncPointWaitList == NULL && numSyncPointsInWaitList > 0),
            UR_RESULT_ERROR_INVALID_EVENT_WAIT_LIST);
  UR_ASSERT(PatternIsValid && PatternSizeIsValid, UR_RESULT_ERROR_INVALID_SIZE);
  return enqueueCommandBufferFillHelper(
      hCommandBuffer, pPtr, hipMemoryTypeUnified, pPattern, patternSize, size,
      numSyncPointsInWaitList, pSyncPointWaitList, pSyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferEnqueueExp(
    ur_exp_command_buffer_handle_t hCommandBuffer, ur_queue_handle_t hQueue,
    uint32_t numEventsInWaitList, const ur_event_handle_t *phEventWaitList,
    ur_event_handle_t *phEvent) {
  ur_result_t Result = UR_RESULT_SUCCESS;

  try {
    std::unique_ptr<ur_event_handle_t_> RetImplEvent{nullptr};
    ScopedContext Active(hQueue->getDevice());
    uint32_t StreamToken;
    ur_stream_quard Guard;
    hipStream_t HIPStream = hQueue->getNextComputeStream(
        numEventsInWaitList, phEventWaitList, Guard, &StreamToken);

    if ((Result = enqueueEventsWait(hQueue, HIPStream, numEventsInWaitList,
                                    phEventWaitList)) != UR_RESULT_SUCCESS) {
      return Result;
    }

    if (phEvent) {
      RetImplEvent = std::unique_ptr<ur_event_handle_t_>(
          ur_event_handle_t_::makeNative(UR_COMMAND_COMMAND_BUFFER_ENQUEUE_EXP,
                                         hQueue, HIPStream, StreamToken));
      UR_CHECK_ERROR(RetImplEvent->start());
    }

    // Launch graph
    UR_CHECK_ERROR(hipGraphLaunch(hCommandBuffer->HIPGraphExec, HIPStream));

    if (phEvent) {
      UR_CHECK_ERROR(RetImplEvent->record());
      *phEvent = RetImplEvent.release();
    }
  } catch (ur_result_t Err) {
    Result = Err;
  }

  return Result;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferRetainCommandExp(
    ur_exp_command_buffer_command_handle_t hCommand) {
  hCommand->incrementExternalReferenceCount();
  hCommand->incrementInternalReferenceCount();
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferReleaseCommandExp(
    ur_exp_command_buffer_command_handle_t hCommand) {
  hCommand->decrementExternalReferenceCount();
  return commandHandleReleaseInternal(hCommand);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferUpdateKernelLaunchExp(
    ur_exp_command_buffer_command_handle_t hCommand,
    const ur_exp_command_buffer_update_kernel_launch_desc_t
        *pUpdateKernelLaunch) {
  // Update requires command-buffer to be finalized
  ur_exp_command_buffer_handle_t CommandBuffer = hCommand->CommandBuffer;
  if (!CommandBuffer->HIPGraphExec) {
    return UR_RESULT_ERROR_INVALID_OPERATION;
  }

  // Update requires command-buffer to be created with update enabled
  if (!CommandBuffer->IsUpdatable) {
    return UR_RESULT_ERROR_INVALID_OPERATION;
  }

  // Kernel corresponding to the command to update
  ur_kernel_handle_t Kernel = hCommand->Kernel;
  ur_device_handle_t Device = CommandBuffer->Device;

  // Update pointer arguments to the kernel
  uint32_t NumPointerArgs = pUpdateKernelLaunch->numNewPointerArgs;
  const ur_exp_command_buffer_update_pointer_arg_desc_t *ArgPointerList =
      pUpdateKernelLaunch->pNewPointerArgList;
  for (uint32_t i = 0; i < NumPointerArgs; i++) {
    const auto &PointerArgDesc = ArgPointerList[i];
    uint32_t ArgIndex = PointerArgDesc.argIndex;
    const void *ArgValue = PointerArgDesc.pNewPointerArg;

    ur_result_t Result = UR_RESULT_SUCCESS;
    try {
      Kernel->setKernelArg(ArgIndex, sizeof(ArgValue), ArgValue);
    } catch (ur_result_t Err) {
      Result = Err;
      return Result;
    }
  }

  // Update memobj arguments to the kernel
  uint32_t NumMemobjArgs = pUpdateKernelLaunch->numNewMemObjArgs;
  const ur_exp_command_buffer_update_memobj_arg_desc_t *ArgMemobjList =
      pUpdateKernelLaunch->pNewMemObjArgList;
  for (uint32_t i = 0; i < NumMemobjArgs; i++) {
    const auto &MemobjArgDesc = ArgMemobjList[i];
    uint32_t ArgIndex = MemobjArgDesc.argIndex;
    ur_mem_handle_t ArgValue = MemobjArgDesc.hNewMemObjArg;

    ur_result_t Result = UR_RESULT_SUCCESS;
    try {
      if (ArgValue == nullptr) {
        Kernel->setKernelArg(ArgIndex, 0, nullptr);
      } else {
        void *HIPPtr = std::get<BufferMem>(ArgValue->Mem).getVoid(Device);
        Kernel->setKernelArg(ArgIndex, sizeof(void *), (void *)&HIPPtr);
      }
    } catch (ur_result_t Err) {
      Result = Err;
      return Result;
    }
  }

  // Update value arguments to the kernel
  uint32_t NumValueArgs = pUpdateKernelLaunch->numNewValueArgs;
  const ur_exp_command_buffer_update_value_arg_desc_t *ArgValueList =
      pUpdateKernelLaunch->pNewValueArgList;
  for (uint32_t i = 0; i < NumValueArgs; i++) {
    const auto &ValueArgDesc = ArgValueList[i];
    uint32_t ArgIndex = ValueArgDesc.argIndex;
    size_t ArgSize = ValueArgDesc.argSize;
    const void *ArgValue = ValueArgDesc.pNewValueArg;

    ur_result_t Result = UR_RESULT_SUCCESS;

    try {
      Kernel->setKernelArg(ArgIndex, ArgSize, ArgValue);
    } catch (ur_result_t Err) {
      Result = Err;
      return Result;
    }
  }

  // Set the updated ND range
  const uint32_t NewWorkDim = pUpdateKernelLaunch->newWorkDim;
  if (NewWorkDim != 0) {
    UR_ASSERT(NewWorkDim > 0, UR_RESULT_ERROR_INVALID_WORK_DIMENSION);
    UR_ASSERT(NewWorkDim < 4, UR_RESULT_ERROR_INVALID_WORK_DIMENSION);
    hCommand->WorkDim = NewWorkDim;
  }

  if (pUpdateKernelLaunch->pNewGlobalWorkOffset) {
    hCommand->setGlobalOffset(pUpdateKernelLaunch->pNewGlobalWorkOffset);
  }

  if (pUpdateKernelLaunch->pNewGlobalWorkSize) {
    hCommand->setGlobalSize(pUpdateKernelLaunch->pNewGlobalWorkSize);
  }

  if (pUpdateKernelLaunch->pNewLocalWorkSize) {
    hCommand->setLocalSize(pUpdateKernelLaunch->pNewLocalWorkSize);
  }

  size_t *GlobalWorkOffset = hCommand->GlobalWorkOffset;
  size_t *GlobalWorkSize = hCommand->GlobalWorkSize;

  const bool ProvidedLocalSize = hCommand->LocalWorkSize[0] != 0 ||
                                 hCommand->LocalWorkSize[1] != 0 ||
                                 hCommand->LocalWorkSize[2] != 0;
  // If no worksize is provided make sure we pass nullptr to setKernelParams so
  // it can guess the local work size.
  size_t *LocalWorkSize = ProvidedLocalSize ? hCommand->LocalWorkSize : nullptr;
  uint32_t WorkDim = hCommand->WorkDim;

  // Set the number of threads per block to the number of threads per warp
  // by default unless user has provided a better number
  size_t ThreadsPerBlock[3] = {32u, 1u, 1u};
  size_t BlocksPerGrid[3] = {1u, 1u, 1u};
  hipFunction_t HIPFunc = Kernel->get();
  auto Result = setKernelParams(Device, WorkDim, GlobalWorkOffset,
                                GlobalWorkSize, LocalWorkSize, Kernel, HIPFunc,
                                ThreadsPerBlock, BlocksPerGrid);
  if (Result != UR_RESULT_SUCCESS) {
    return Result;
  }

  hipKernelNodeParams &Params = hCommand->Params;

  Params.func = HIPFunc;
  Params.gridDim.x = BlocksPerGrid[0];
  Params.gridDim.y = BlocksPerGrid[1];
  Params.gridDim.z = BlocksPerGrid[2];
  Params.blockDim.x = ThreadsPerBlock[0];
  Params.blockDim.y = ThreadsPerBlock[1];
  Params.blockDim.z = ThreadsPerBlock[2];
  Params.sharedMemBytes = Kernel->getLocalSize();
  Params.kernelParams = const_cast<void **>(Kernel->getArgIndices().data());

  hipGraphNode_t Node = *(hCommand->Node);
  hipGraphExec_t HipGraphExec = CommandBuffer->HIPGraphExec;
  UR_CHECK_ERROR(hipGraphExecKernelNodeSetParams(HipGraphExec, Node, &Params));
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferGetInfoExp(
    ur_exp_command_buffer_handle_t hCommandBuffer,
    ur_exp_command_buffer_info_t propName, size_t propSize, void *pPropValue,
    size_t *pPropSizeRet) {
  UrReturnHelper ReturnValue(propSize, pPropValue, pPropSizeRet);

  switch (propName) {
  case UR_EXP_COMMAND_BUFFER_INFO_REFERENCE_COUNT:
    return ReturnValue(hCommandBuffer->getExternalReferenceCount());
  default:
    assert(!"Command-buffer info request not implemented");
  }

  return UR_RESULT_ERROR_INVALID_ENUMERATION;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferCommandGetInfoExp(
    ur_exp_command_buffer_command_handle_t hCommand,
    ur_exp_command_buffer_command_info_t propName, size_t propSize,
    void *pPropValue, size_t *pPropSizeRet) {
  UrReturnHelper ReturnValue(propSize, pPropValue, pPropSizeRet);

  switch (propName) {
  case UR_EXP_COMMAND_BUFFER_COMMAND_INFO_REFERENCE_COUNT:
    return ReturnValue(hCommand->getExternalReferenceCount());
  default:
    assert(!"Command-buffer command info request not implemented");
  }

  return UR_RESULT_ERROR_INVALID_ENUMERATION;
}
