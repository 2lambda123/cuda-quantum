/****************************************************************-*- C++ -*-****
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/
#include "distributed_capi.h"
#include <iostream>
#include <pybind11/embed.h>
namespace py = pybind11;

namespace {

/// @brief Reference to the pybind11 scoped interpreter
thread_local static std::unique_ptr<py::scoped_interpreter> interp;

static bool mpi4pyFound = false;

bool initCalledByThis = false;
py::object convertType(DataType dataType) {
  auto mpiMod = py::module::import("mpi4py.MPI");
  switch (dataType) {
  case FLOAT_32:
    return mpiMod.attr("FLOAT");
  case FLOAT_64:
    return mpiMod.attr("DOUBLE");
  }
  __builtin_unreachable();
}

std::size_t getDataSize(DataType dataType) {
  switch (dataType) {
  case FLOAT_32:
    return sizeof(float);
  case FLOAT_64:
    return sizeof(double);
  }
  __builtin_unreachable();
}

// MPI_Op convertType(ReduceOp opType) {
//   switch (opType) {
//   case SUM:
//     return MPI_SUM;
//   case PROD:
//     return MPI_PROD;
//   }
//   __builtin_unreachable();
// }

py::object unpackMpiCommunicator(const cudaqDistributedCommunicator_t *comm) {
  try {
    auto mpiComm = py::module::import("mpi4py.MPI.Comm");
    return mpiComm.attr("fromhandle")(comm->commPtr);
  } catch (...) {
    throw std::runtime_error(
        "Invalid distributed communicator encountered in CUDAQ mpi4py plugin.");
  }
}
} // namespace
extern "C" {

int mpi_initialize(int32_t *argc, char ***argv) {
  try {
    auto mpiMod = py::module::import("mpi4py.MPI");
    if (mpiMod.attr("Is_initialized")().cast<bool>())
      return 0;

    mpiMod.attr("Init")();
    initCalledByThis = true;
    return 0;
  } catch (...) {
    return 1;
  }
}

int mpi_finalize() {
  if (!initCalledByThis)
    return 0;
  try {
    auto mpiMod = py::module::import("mpi4py.MPI");
    if (mpiMod.attr("Is_finalized")().cast<bool>())
      return 0;

    mpiMod.attr("Finalize")();
    return 0;
  } catch (...) {
    return 1;
  }
}

int mpi_initialized(int32_t *flag) {
  try {
    auto mpiMod = py::module::import("mpi4py.MPI");
    if (mpiMod.attr("Is_initialized")().cast<bool>())
      *flag = 1;
    else
      *flag = 0;

    return 0;
  } catch (...) {
    return 1;
  }
}

int mpi_finalized(int32_t *flag) {
  try {
    auto mpiMod = py::module::import("mpi4py.MPI");
    if (mpiMod.attr("Is_finalized")().cast<bool>())
      *flag = 1;
    else
      *flag = 0;

    return 0;
  } catch (...) {
    return 1;
  }
}

int mpi_getNumRanks(const cudaqDistributedCommunicator_t *comm, int32_t *size) {
  auto pyComm = unpackMpiCommunicator(comm);
  try {
    *size = pyComm.attr("Get_size")().cast<int>();
    return 0;
  } catch (...) {
    return 1;
  }
}

int mpi_getProcRank(const cudaqDistributedCommunicator_t *comm, int32_t *rank) {
  auto pyComm = unpackMpiCommunicator(comm);
  try {
    *rank = pyComm.attr("Get_rank")().cast<int>();
    return 0;
  } catch (...) {
    return 1;
  }
}

int mpi_Barrier(const cudaqDistributedCommunicator_t *comm) {
  auto pyComm = unpackMpiCommunicator(comm);
  try {
    pyComm.attr("Barrier")();
    return 0;
  } catch (...) {
    return 1;
  }
}

int mpi_Bcast(const cudaqDistributedCommunicator_t *comm, void *buffer,
              int32_t count, DataType dataType, int32_t rootRank) {
  py::buffer_info pyBuffer(
      // Pointer to buffer
      buffer,
      // Size of one scalar
      getDataSize(dataType),
      // Python struct-style format descriptor
      py::format_descriptor<double>::format(),
      // Number of dimensions
      1,
      // Buffer dimensions
      {count},
      // Strides (in bytes) for each index
      {sizeof(double)});
  auto pyComm = unpackMpiCommunicator(comm);
  try {
    pyComm.attr("Bcast")(pyBuffer, rootRank);
    return 0;
  } catch (...) {
    return 1;
  }
}
int mpi_Allreduce(const cudaqDistributedCommunicator_t *comm,
                  const void *sendBuffer, void *recvBuffer, int32_t count,
                  DataType dataType, ReduceOp opType) {
  // TODO
  return 0;
}
int mpi_Allgather(const cudaqDistributedCommunicator_t *comm,
                  const void *sendBuffer, void *recvBuffer, int32_t count,
                  DataType dataType) {
  // TODO
  return 0;
}
int mpi_CommDup(const cudaqDistributedCommunicator_t *comm,
                cudaqDistributedCommunicator_t **newDupComm) {
  // TODO
  return 0;
}

int mpi_CommSplit(const cudaqDistributedCommunicator_t *comm, int32_t color,
                  int32_t key, cudaqDistributedCommunicator_t **newSplitComm) {
  // TODO
  return 0;
}

cudaqDistributedCommunicator_t *getMpiCommunicator() {
  try {
    static cudaqDistributedCommunicator_t commWorld;
    auto mpiMod = py::module::import("mpi4py.MPI");
    auto pyCommWorld = mpiMod.attr("COMM_WORLD");
    auto commPtr =
        (void *)(mpiMod.attr("_addressof")(pyCommWorld).cast<int64_t>());
    commWorld.commPtr = commPtr;
    commWorld.commSize =
        mpiMod.attr("_sizeof")(pyCommWorld).cast<std::size_t>();
    return &commWorld;
  } catch (std::exception &e) {
    return nullptr;
  }
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
      mpi_Barrier,
      mpi_Bcast,
      mpi_Allreduce,
      mpi_Allgather,
      mpi_CommDup,
      mpi_CommSplit};
  return &cudaqDistributedInterface;
}
}

__attribute__((constructor)) void dllMain() {
  interp = std::make_unique<py::scoped_interpreter>();
  try {
    py::module::import("mpi4py");
  } catch (std::exception &e) {
    // mpi4py not installed
    mpi4pyFound = false;
  }
  mpi4pyFound = true;
  // Disable auto init
  // https://mpi4py.readthedocs.io/en/stable/mpi4py.html#mpi4py.mpi4py.rc.initialize
  auto mpiRcMod = py::module::import("mpi4py.rc");
  mpiRcMod.attr("initialize") = false;
}