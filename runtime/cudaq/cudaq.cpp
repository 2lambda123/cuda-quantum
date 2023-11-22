/*******************************************************************************
 * Copyright (c) 2022 - 2023 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include "cudaq.h"
#define LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING 1

#include "common/Logger.h"
#ifdef CUDAQ_HAS_CUDA
#include "cuda_runtime_api.h"
#endif
#include "cudaq/platform.h"
#include "cudaq/utils/registry.h"
#include "distributed/mpi_plugin.h"
#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <regex>
#include <signal.h>
#include <string>
#include <vector>
namespace nvqir {
void tearDownBeforeMPIFinalize();
void setRandomSeed(std::size_t);
} // namespace nvqir

namespace cudaq::mpi {
cudaq::MPIPlugin *getMpiPlugin(bool unsafe) {
  // Locate and load the MPI comm plugin.
  // Rationale: we don't want to explicitly link `libcudaq.so` against any
  // specific MPI implementation for compatibility. Rather, MPI functionalities
  // are encapsulated inside a runtime-loadable plugin.
  static std::unique_ptr<cudaq::MPIPlugin> g_plugin;
  if (!g_plugin) {
    const char *mpiLibPath = std::getenv("CUDAQ_MPI_COMM_LIB");
    // Search priority
    // (1) Environment variable take precedence
    // (2) Built-in comm plugin (e.g., docker container or build from source
    // with MPI) This will throw if it cannot find an appropriate MPI comm
    // plugin. i.e., users are calling cudaq::mpi APIs while no MPI support can
    // be found.
    // (3) mpi4py-based wrapper
    if (mpiLibPath) {
      // The user has set the environment variable.
      cudaq::info("Load MPI comm plugin from CUDAQ_MPI_COMM_LIB environment "
                  "variable at '{}'",
                  mpiLibPath);
      g_plugin = std::make_unique<cudaq::MPIPlugin>(mpiLibPath);
    } else {
      // Try locate it in the install directory
      std::filesystem::path cudaqLibPath{cudaq::getCUDAQLibraryPath()};
      const auto pluginsPath = cudaqLibPath.parent_path() / "plugins";
#if defined(__APPLE__) && defined(__MACH__)
      const std::string libSuffix = "dylib";
#else
      const std::string libSuffix = "so";
#endif
      const auto pluginLibFile =
          pluginsPath / fmt::format("libcudaq-comm-plugin.{}", libSuffix);
      const auto pyPluginLibFile =
          pluginsPath / fmt::format("libcudaq-py-comm-plugin.{}", libSuffix);
      if (std::filesystem::exists(pluginLibFile)) {
        cudaq::info("Load builtin MPI comm plugin from  at '{}'",
                    pluginLibFile.c_str());
        g_plugin = std::make_unique<cudaq::MPIPlugin>(pluginLibFile.c_str());
      } else if (std::filesystem::exists(pyPluginLibFile)) {
        cudaq::info("Try loading mpi4py MPI comm plugin from  at '{}'",
                    pyPluginLibFile.c_str());
        g_plugin = std::make_unique<cudaq::MPIPlugin>(pyPluginLibFile.c_str());
        if (!g_plugin->isValid()) {
          cudaq::info("Failed to load mpi4py MPI comm plugin (mpi4py is not "
                      "available).");
          // Don't use it since mpi4py is not available.
          g_plugin.reset();
        }
      }
    }
  }
  if (!g_plugin) {
    if (unsafe)
      return nullptr;
    else
      throw std::runtime_error(
          "No MPI support can be found when attempted to use cudaq::mpi APIs. "
          "Please refer to the documentation for instructions to activate MPI "
          "support.");
  }

  return g_plugin.get();
};

void initialize() {
  // int argc{0};
  // char **argv = nullptr;
  // initialize(argc, argv);
  auto *commPlugin = getMpiPlugin();
  commPlugin->initialize();
}

void initialize(int argc, char **argv) {
  auto *commPlugin = getMpiPlugin();
  commPlugin->initialize(argc, argv);
  const auto pid = commPlugin->rank();
  const auto np = commPlugin->num_ranks();
  if (pid == 0)
    cudaq::info("MPI Initialized, nRanks = {}", np);
}

int rank() {
  auto *commPlugin = getMpiPlugin();
  return commPlugin->rank();
}

int num_ranks() {
  auto *commPlugin = getMpiPlugin();
  return commPlugin->num_ranks();
}

bool is_initialized() {
  // Allow to probe is_initialized even without MPI support
  auto *commPlugin = getMpiPlugin(true);
  if (!commPlugin)
    return false;

  return commPlugin->is_initialized();
}

namespace details {

#define CUDAQ_ALL_REDUCE_IMPL(TYPE, BINARY, REDUCE_OP)                         \
  TYPE allReduce(const TYPE &local, const BINARY<TYPE> &) {                    \
    static_assert(std::is_floating_point<TYPE>::value,                         \
                  "all_reduce argument must be a floating point number");      \
    std::vector<double> result(1);                                             \
    std::vector<double> localVec{static_cast<double>(local)};                  \
    auto *commPlugin = getMpiPlugin();                                         \
    commPlugin->all_reduce(result, localVec, REDUCE_OP);                       \
    return static_cast<TYPE>(result.front());                                  \
  }

CUDAQ_ALL_REDUCE_IMPL(float, std::plus, SUM)
CUDAQ_ALL_REDUCE_IMPL(float, std::multiplies, PROD)

CUDAQ_ALL_REDUCE_IMPL(double, std::plus, SUM)
CUDAQ_ALL_REDUCE_IMPL(double, std::multiplies, PROD)

} // namespace details

void all_gather(std::vector<double> &global, const std::vector<double> &local) {
  auto *commPlugin = getMpiPlugin();
  commPlugin->all_gather(global, local);
}

void broadcast(std::vector<double> &data, int rootRank) {
  auto *commPlugin = getMpiPlugin();
  commPlugin->broadcast(data, rootRank);
}

void finalize() {
  if (rank() == 0)
    cudaq::info("Finalizing MPI.");

  // Inform the simulator that we are
  // about to run MPI Finalize
  nvqir::tearDownBeforeMPIFinalize();
  auto *commPlugin = getMpiPlugin();
  if (!commPlugin->is_finalized())
    commPlugin->finalize();
}

} // namespace cudaq::mpi

namespace cudaq::__internal__ {
std::map<std::string, std::string> runtime_registered_mlir;
std::string demangle_kernel(const char *name) {
  return quantum_platform::demangle(name);
}
bool globalFalse = false;
} // namespace cudaq::__internal__

//===----------------------------------------------------------------------===//
// Registry that maps device code keys to strings of device code. The map is
// created at program startup and can be used to find code to be
// compiled/executed at runtime.
//===----------------------------------------------------------------------===//

static std::vector<std::pair<std::string, std::string>> quakeRegistry;

void cudaq::registry::deviceCodeHolderAdd(const char *key, const char *code) {
  quakeRegistry.emplace_back(key, code);
}

//===----------------------------------------------------------------------===//
// Registry of all kernels that have been generated. The vector of kernels is
// created at program startup time. This list can be consulted by the runtime to
// determine if a particular kernel has been processed for kernel execution,
// including adding the trampoline to call the runtime to launch the kernel.
//===----------------------------------------------------------------------===//

static std::vector<std::string> kernelRegistry;

static std::map<std::string, cudaq::KernelArgsCreator> argsCreators;
static std::map<std::string, std::string> lambdaNames;

void cudaq::registry::cudaqRegisterKernelName(const char *kernelName) {
  kernelRegistry.emplace_back(kernelName);
}

void cudaq::registry::cudaqRegisterArgsCreator(const char *name,
                                               char *rawFunctor) {
  argsCreators.insert(
      {std::string(name), reinterpret_cast<KernelArgsCreator>(rawFunctor)});
}

void cudaq::registry::cudaqRegisterLambdaName(const char *name,
                                              const char *value) {
  lambdaNames.insert({std::string(name), std::string(value)});
}

bool cudaq::__internal__::isKernelGenerated(const std::string &kernelName) {
  for (auto regName : kernelRegistry)
    if (kernelName == regName)
      return true;
  return false;
}

bool cudaq::__internal__::isLibraryMode(const std::string &kernelname) {
  return !isKernelGenerated(kernelname);
}

//===----------------------------------------------------------------------===//

namespace nvqir {
void setRandomSeed(std::size_t);
}

namespace cudaq {

void set_target_backend(const char *backend) {
  std::string backendName(backend);
  auto &platform = cudaq::get_platform();
  platform.setTargetBackend(backendName);
}

KernelArgsCreator getArgsCreator(const std::string &kernelName) {
  return argsCreators[kernelName];
}

std::string get_quake_by_name(const std::string &kernelName,
                              bool throwException) {
  // A prefix name has a '.' before the C++ mangled name suffix.
  auto kernelNamePrefix = kernelName + '.';

  // Find the quake code
  std::optional<std::string> result;
  for (auto [k, v] : quakeRegistry) {
    if (k == kernelName) {
      // Exact match. Return the code.
      return v;
    }
    if (k.starts_with(kernelNamePrefix)) {
      // Prefix match. Record it and make sure that it is a unique prefix.
      if (result.has_value()) {
        if (throwException)
          throw std::runtime_error("Quake code for '" + kernelName +
                                   "' has multiple matches.\n");
      } else {
        result = v;
      }
    }
  }
  if (result.has_value())
    return *result;
  auto msg = "Quake code not found for '" + kernelName + "'.\n";
  if (throwException)
    throw std::runtime_error(msg);
  return {};
}

std::string get_quake_by_name(const std::string &kernelName) {
  return get_quake_by_name(kernelName, true);
}

bool kernelHasConditionalFeedback(const std::string &kernelName) {
  auto quakeCode = get_quake_by_name(kernelName, false);
  return !quakeCode.empty() &&
         quakeCode.find("qubitMeasurementFeedback = true") != std::string::npos;
}

// Ignore warnings about deprecations in platform.set_shots and
// platform.clear_shots because the functions that are using them here
// (cudaq::set_shots and cudaq::clear_shots are also deprecated and will be
// removed at the same time.)
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
void set_shots(const std::size_t nShots) {
  auto &platform = cudaq::get_platform();
  platform.set_shots(nShots);
}
void clear_shots(const std::size_t nShots) {
  auto &platform = cudaq::get_platform();
  platform.clear_shots();
}
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

void set_noise(const cudaq::noise_model &model) {
  auto &platform = cudaq::get_platform();
  platform.set_noise(&model);
}

void unset_noise() {
  auto &platform = cudaq::get_platform();
  platform.set_noise(nullptr);
}

thread_local static std::size_t cudaq_random_seed = 0;

/// @brief Note: a seed value of 0 will cause broadcast operations to use
/// std::random_device (or something similar) as a seed for the PRNGs, so this
/// will not be repeatable for those operations.
void set_random_seed(std::size_t seed) {
  cudaq_random_seed = seed;
  nvqir::setRandomSeed(seed);
}

std::size_t get_random_seed() { return cudaq_random_seed; }

int num_available_gpus() {
  int nDevices = 0;
#ifdef CUDAQ_HAS_CUDA
  cudaGetDeviceCount(&nDevices);
#endif
  return nDevices;
}

namespace __internal__ {
void cudaqCtrlCHandler(int signal) {
  printf(" CTRL-C caught in cudaq runtime.\n");
  std::exit(1);
}

__attribute__((constructor)) void startSigIntHandler() {
  struct sigaction sigIntHandler;
  sigIntHandler.sa_handler = cudaqCtrlCHandler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;
  sigaction(SIGINT, &sigIntHandler, NULL);
}
} // namespace __internal__

} // namespace cudaq

namespace cudaq::support {
extern "C" {
void __nvqpp_initializer_list_to_vector_bool(std::vector<bool> &result,
                                             char *initList, std::size_t size) {
  // result is a sret return value. Make sure it is default initialized. Takes
  // advantage of default empty vector being all 0s.
  std::memset(&result, 0, sizeof(result));
  // Allocate space.
  result.reserve(size);
  // Copy in the initialization list data.
  char *p = initList;
  for (std::size_t i = 0; i < size; ++i, ++p)
    result.push_back(static_cast<bool>(*p));
  // Free the initialization list, which was stack allocated.
  free(initList);
}
}
} // namespace cudaq::support
