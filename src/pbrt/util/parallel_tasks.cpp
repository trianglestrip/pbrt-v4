#include <pbrt/util/parallel_tasks.h>

#include <taskflow/taskflow.hpp>

namespace pbrt {

void RunParallelTasks(std::vector<std::function<void()>> tasks) {
    if (tasks.empty())
        return;
    if (tasks.size() == 1) {
        // Avoid spinning up a thread pool for a single item.
        tasks.front()();
        return;
    }

    tf::Executor executor;
    tf::Taskflow taskflow;
    for (auto &fn : tasks)
        taskflow.emplace([&fn]() { fn(); });
    executor.run(taskflow).wait();
}

}  // namespace pbrt
