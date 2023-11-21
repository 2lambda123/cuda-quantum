/****************************************************************-*- C++ -*-****
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/
#include "distributed_capi.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mpi.h>
#include <stdexcept>

namespace {
bool initCalledByThis = false;
MPI_Datatype convertType(DataType dataType) {
  switch (dataType) {
  case INT_8:
    return MPI_INT8_T;
  case INT_16:
    return MPI_INT16_T;
  case INT_32:
    return MPI_INT32_T;
  case INT_64:
    return MPI_INT64_T;
  case FLOAT_32:
    return MPI_FLOAT;
  case FLOAT_64:
    return MPI_DOUBLE;
  case FLOAT_COMPLEX:
    return MPI_C_FLOAT_COMPLEX;
  case DOUBLE_COMPLEX:
    return MPI_C_DOUBLE_COMPLEX;
  }
  __builtin_unreachable();
}

MPI_Datatype convertTypeMinLoc(DataType dataType) {
  switch (dataType) {
  case FLOAT_32:
    return MPI_FLOAT_INT;
  case FLOAT_64:
    return MPI_DOUBLE_INT;
  default:
    throw std::runtime_error("Unsupported MINLOC data type");
  }
  __builtin_unreachable();
}

MPI_Op convertType(ReduceOp opType) {
  switch (opType) {
  case SUM:
    return MPI_SUM;
  case PROD:
    return MPI_PROD;
  case MIN:
    return MPI_MIN;
  case MIN_LOC:
    return MPI_MINLOC;
  }
  __builtin_unreachable();
}

MPI_Comm unpackMpiCommunicator(const cudaqDistributedCommunicator_t *comm) {
  if (comm->commPtr == NULL)
    return MPI_COMM_NULL;
  if (sizeof(MPI_Comm) != comm->commSize) {
    printf("#FATAL: MPI_Comm object has unexpected size!\n");
    exit(EXIT_FAILURE);
  }
  return *((MPI_Comm *)(comm->commPtr));
}
} // namespace
extern "C" {

int mpi_initialize(int32_t *argc, char ***argv) {
  int flag = 0;
  int res = MPI_Initialized(&flag);
  if (res != MPI_SUCCESS)
    return res;
  // This has been initialized, nothing to do.
  if (flag)
    return MPI_SUCCESS;
  initCalledByThis = true;
  return MPI_Init(argc, argv);
}

int mpi_finalize() {
  if (!initCalledByThis)
    return MPI_SUCCESS;
  return MPI_Finalize();
}

int mpi_initialized(int32_t *flag) { return MPI_Initialized(flag); }
int mpi_finalized(int32_t *flag) { return MPI_Finalized(flag); }

int mpi_getNumRanks(const cudaqDistributedCommunicator_t *comm, int32_t *size) {
  return MPI_Comm_size(unpackMpiCommunicator(comm), size);
}

int mpi_getProcRank(const cudaqDistributedCommunicator_t *comm, int32_t *rank) {
  return MPI_Comm_rank(unpackMpiCommunicator(comm), rank);
}

int mpi_getCommSizeShared(const cudaqDistributedCommunicator_t *comm,
                          int32_t *numRanks) {
  *numRanks = 0;
  MPI_Info info;
  MPI_Info_create(&info);
  MPI_Info_set(info, "mpi_hw_resource_type", "mpi_shared_memory");
  int procRank = -1;
  int mpiErr = MPI_Comm_rank(unpackMpiCommunicator(comm), &procRank);
  if (mpiErr == MPI_SUCCESS) {
    MPI_Comm localComm;
    mpiErr =
        MPI_Comm_split_type(unpackMpiCommunicator(comm), MPI_COMM_TYPE_SHARED,
                            procRank, info, &localComm);
    if (mpiErr == MPI_SUCCESS) {
      int nranks = 0;
      mpiErr = MPI_Comm_size(localComm, &nranks);
      *numRanks = nranks;
      MPI_Comm_free(&localComm);
    }
  }
  return mpiErr;
}

int mpi_Barrier(const cudaqDistributedCommunicator_t *comm) {
  return MPI_Barrier(unpackMpiCommunicator(comm));
}
int mpi_Bcast(const cudaqDistributedCommunicator_t *comm, void *buffer,
              int32_t count, DataType dataType, int32_t rootRank) {
  return MPI_Bcast(buffer, count, convertType(dataType), rootRank,
                   unpackMpiCommunicator(comm));
}
int mpi_Allreduce(const cudaqDistributedCommunicator_t *comm,
                  const void *sendBuffer, void *recvBuffer, int32_t count,
                  DataType dataType, ReduceOp opType) {
  if (opType == MIN_LOC) {
    return MPI_Allreduce(sendBuffer, recvBuffer, count,
                         convertTypeMinLoc(dataType), convertType(opType),
                         unpackMpiCommunicator(comm));
  } else {
    return MPI_Allreduce(sendBuffer, recvBuffer, count, convertType(dataType),
                         convertType(opType), unpackMpiCommunicator(comm));
  }
}

int mpi_AllreduceInplace(const cudaqDistributedCommunicator_t *comm,
                         void *recvBuffer, int32_t count, DataType dataType,
                         ReduceOp opType) {
  return MPI_Allreduce(MPI_IN_PLACE, recvBuffer, count, convertType(dataType),
                       convertType(opType), unpackMpiCommunicator(comm));
}

int mpi_Allgather(const cudaqDistributedCommunicator_t *comm,
                  const void *sendBuffer, void *recvBuffer, int32_t count,
                  DataType dataType) {
  return MPI_Allgather(sendBuffer, count, convertType(dataType), recvBuffer,
                       count, convertType(dataType),
                       unpackMpiCommunicator(comm));
}
int mpi_CommDup(const cudaqDistributedCommunicator_t *comm,
                cudaqDistributedCommunicator_t **newDupComm) {
  // Use std::deque to make sure pointers to elements are valid.
  static std::deque<std::pair<MPI_Comm, cudaqDistributedCommunicator_t>>
      dup_comms;
  dup_comms.emplace_back(std::pair<MPI_Comm, cudaqDistributedCommunicator_t>());
  auto &[dupComm, newComm] = dup_comms.back();
  auto status = MPI_Comm_dup(unpackMpiCommunicator(comm), &dupComm);
  newComm.commPtr = &dupComm;
  newComm.commSize = sizeof(dupComm);
  *newDupComm = &newComm;
  return status;
}

int mpi_CommSplit(const cudaqDistributedCommunicator_t *comm, int32_t color,
                  int32_t key, cudaqDistributedCommunicator_t **newSplitComm) {

  // Use std::deque to make sure pointers to elements are valid.
  static std::deque<std::pair<MPI_Comm, cudaqDistributedCommunicator_t>>
      split_comms;
  split_comms.emplace_back(
      std::pair<MPI_Comm, cudaqDistributedCommunicator_t>());
  auto &[splitComm, newComm] = split_comms.back();
  auto status =
      MPI_Comm_split(unpackMpiCommunicator(comm), color, key, &splitComm);
  newComm.commPtr = &splitComm;
  newComm.commSize = sizeof(splitComm);
  *newSplitComm = &newComm;
  return status;
}

cudaqDistributedCommunicator_t *getMpiCommunicator() {
  static MPI_Comm pluginComm = MPI_COMM_WORLD;
  static cudaqDistributedCommunicator_t commWorld{&pluginComm,
                                                  sizeof(pluginComm)};
  return &commWorld;
}

cudaqDistributedInterface_t *getDistributedInterface() {
  static cudaqDistributedInterface_t cudaqDistributedInterface{
      CUDAQ_DISTRIBUTED_INTERFACE_VERSION,
      mpi_initialize,
      mpi_finalize,
      mpi_initialized,
      mpi_finalized,
      mpi_getNumRanks,
      mpi_getProcRank,
      mpi_getCommSizeShared,
      mpi_Barrier,
      mpi_Bcast,
      mpi_Allreduce,
      mpi_AllreduceInplace,
      mpi_Allgather,
      mpi_CommDup,
      mpi_CommSplit};
  return &cudaqDistributedInterface;
}
}
