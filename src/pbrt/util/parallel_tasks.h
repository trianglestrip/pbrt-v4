#ifndef PBRT_UTIL_PARALLEL_TASKS_H
#define PBRT_UTIL_PARALLEL_TASKS_H

#include <functional>
#include <vector>

namespace pbrt {

// Runs the given closures concurrently on a work-stealing thread pool
// (Taskflow-backed) and blocks until all have completed. Implemented in a
// CUDA-free translation unit so nvcc never has to parse the Taskflow headers
// (cudafe++ crashes on them).
void RunParallelTasks(std::vector<std::function<void()>> tasks);

}  // namespace pbrt

#endif
