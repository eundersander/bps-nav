#include <v4r/cuda.hpp>
#include <PathFinder.h>
#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <simdjson.h>
#include <zlib.h>

#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <queue>
#include <random>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <pthread.h>

// Spinning will never be beneficial, always takes some time to come back
// around for the next iteration
#define __NO_SPIN
#include <atomic_wait>
contended_t contention[256];

contended_t *__contention(volatile void const *p)
{
    return contention + ((uintptr_t)p & 255);
}
#undef __NO_SPIN

using namespace std;
using namespace v4r;
namespace py = pybind11;

namespace SimulatorConfig {
constexpr float SUCCESS_REWARD = 2.5;
constexpr float SLACK_REWARD = 1e-2;
constexpr float SUCCESS_DISTANCE = 0.2;
constexpr float MAX_STEPS = 500;
constexpr float FORWARD_STEP_SIZE = 0.25;
constexpr float TURN_ANGLE = glm::radians(10.f);
constexpr glm::vec3 UP_VECTOR(0.f, 1.f, 0.f);

constexpr glm::vec3 CAM_FWD_VECTOR =
    glm::vec3(0.f, 0.f, -1.f) * FORWARD_STEP_SIZE;

static const glm::quat LEFT_ROTATION = glm::angleAxis(TURN_ANGLE, UP_VECTOR);

static const glm::quat RIGHT_ROTATION = glm::angleAxis(-TURN_ANGLE, UP_VECTOR);
}

template <typename T>
class Span {
public:
    Span(T *base, size_t num_elems) : ptr_(base), num_elems_(num_elems) {}

    T &operator[](size_t idx) const { return ptr_[idx]; }

    T *begin() { return ptr_; }
    const T *begin() const { return ptr_; }

    T *end() { return ptr_ + num_elems_; }
    const T *end() const { return ptr_ + num_elems_; }

    size_t size() const { return num_elems_; }

private:
    T *ptr_;
    size_t num_elems_;
};

struct Episode {
    glm::vec3 startPosition;
    glm::quat startRotation;
    glm::vec3 goal;
};

struct SceneMetadata {
    uint32_t firstEpisode;
    uint32_t numEpisodes;
    string meshPath;
    string navPath;
};

template <typename T>
class DynArray {
public:
    explicit DynArray(size_t n) : ptr_(allocator<T>().allocate(n)), n_(n) {}

    ~DynArray()
    {
        for (size_t i = 0; i < n_; i++) {
            ptr_[i].~T();
        }
        allocator<T>().deallocate(ptr_, n_);
    }

    T &operator[](size_t idx) { return ptr_[idx]; }
    const T &operator[](size_t idx) const { return ptr_[idx]; }

    T *begin() { return ptr_; }
    T *end() { return ptr_ + n_; }
    const T *begin() const { return ptr_; }
    const T *end() const { return ptr_ + n_; }

private:
    T *ptr_;
    size_t n_;
};

static uint32_t num_cores()
{
    cpu_set_t cpuset;
    pthread_getaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    return CPU_COUNT(&cpuset);
}

static void set_affinity(int target_cpu_idx)
{
    if (target_cpu_idx < 0) return;

    cpu_set_t cpuset;
    pthread_getaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    assert(target_cpu_idx < CPU_COUNT(&cpuset));
    cpu_set_t worker_set;
    CPU_ZERO(&worker_set);

    // This is needed incase there was already cpu masking via
    // a different call to setaffinity or via cgroup (SLURM)
    for (int cpu_idx = 0, cpus_found = 0; cpu_idx < CPU_SETSIZE; ++cpu_idx) {
        if (CPU_ISSET(cpu_idx, &cpuset)) {
            if (cpus_found == target_cpu_idx) {
                CPU_SET(cpu_idx, &worker_set);
                break;
            } else {
                ++cpus_found;
            }
        }
    }

    pthread_setaffinity_np(pthread_self(), sizeof(worker_set), &worker_set);
}

static simdjson::dom::element parseFile(const string &file_path,
                                        size_t num_bytes,
                                        simdjson::dom::parser &parser)
{
    gzFile gz = gzopen(file_path.c_str(), "rb");
    if (gz == nullptr) {
        cerr << "Failed to open " << file_path << endl;
        abort();
    }

    vector<uint8_t> out_data {};

    size_t cur_out_size = num_bytes * 2;
    int cur_decompressed = 0;
    size_t total_decompressed = 0;
    for (; !gzeof(gz) && cur_decompressed >= 0;
         cur_decompressed = gzread(gz, out_data.data() + total_decompressed,
                                   cur_out_size - total_decompressed),
         total_decompressed += cur_decompressed, cur_out_size *= 2) {
        out_data.resize(cur_out_size + simdjson::SIMDJSON_PADDING);
    }

    if (cur_decompressed == -1) {
        cerr << "Failed to read " << file_path << endl;
        abort();
    }

    gzclose(gz);

    return parser.parse(out_data.data(), total_decompressed, false);
}

class Dataset {
public:
    Dataset(const string &dataset_path_name,
            const string &asset_path_name,
            uint32_t num_threads)
        : episodes_(),
          scenes_()
    {
        filesystem::path dataset_name {dataset_path_name};
        constexpr const char *data_suffix = ".json.gz";

        vector<pair<string, size_t>> json_files;
        for (const auto &entry :
             filesystem::directory_iterator(dataset_name)) {
            const string filename = entry.path().string();
            if (string_view(filename).substr(
                    filename.size() - strlen(data_suffix)) == data_suffix) {
                json_files.push_back({
                    filename,
                    filesystem::file_size(entry.path()),
                });
            }
        }

        num_threads =
            min(num_threads, static_cast<uint32_t>(json_files.size()));

        uint32_t files_per_thread = json_files.size() / num_threads;
        uint32_t extra_files =
            json_files.size() - num_threads * files_per_thread;

        vector<thread> loader_threads;
        loader_threads.reserve(num_threads);

        mutex merge_mutex;

        uint32_t thread_file_offset = 0;
        for (uint32_t i = 0; i < num_threads; i++) {
            uint32_t num_files = files_per_thread;
            if (extra_files > 0) {
                num_files++;
                extra_files--;
            }

            loader_threads.emplace_back([this, thread_file_offset, num_files,
                                         &merge_mutex, &asset_path_name,
                                         &json_files]() {
                vector<Episode> episodes;
                vector<SceneMetadata> scenes;

                simdjson::dom::parser parser;

                for (uint32_t file_idx = 0; file_idx < num_files; file_idx++) {
                    uint32_t scene_episode_start = episodes.size();
                    string_view scene_id;

                    const auto &[file_name, num_bytes] =
                        json_files[thread_file_offset + file_idx];

                    auto json = parseFile(file_name, num_bytes, parser);
                    const auto &json_episodes = json["episodes"];

                    for (const auto &json_episode : json_episodes) {
                        auto fill_vec = [](auto &vec, const auto &json_arr) {
                            uint32_t idx = 0;
                            for (double component : json_arr) {
                                vec[idx] = component;
                                idx++;
                            }
                        };

                        glm::vec3 start_pos;
                        fill_vec(start_pos, json_episode["start_position"]);
                        glm::quat start_rot;
                        fill_vec(start_rot, json_episode["start_rotation"]);

                        glm::vec3 goal_pos;
                        fill_vec(goal_pos,
                                 json_episode["goals"].at(0)["position"]);

                        const string_view cur_scene_path =
                            json_episode["scene_id"];

                        if (scene_id.size() == 0) {
                            scene_id = cur_scene_path;
                        }

                        if (scene_id != cur_scene_path) {
                            cerr << "Loading code assumes json file contains "
                                    "data for one scene"
                                 << endl;
                            abort();
                        }

                        episodes.push_back({
                            start_pos,
                            start_rot,
                            goal_pos,
                        });
                    }

                    if (scene_id.size() > 0) {
                        size_t dotpos = scene_id.rfind('.');
                        if (dotpos == string_view::npos) {
                            cerr << "Invalid scene id: " << scene_id << endl;
                            abort();
                        }

                        string bps_suffix =
                            string(scene_id.substr(0, dotpos)) + ".bps";

                        // FIXME is there some more principled way to get this?
                        string navmesh_suffix =
                            string(scene_id.substr(0, dotpos)) + ".navmesh";

                        scenes.push_back({
                            scene_episode_start,
                            static_cast<uint32_t>(episodes.size() -
                                                  scene_episode_start),
                            asset_path_name + "/" + bps_suffix,
                            asset_path_name + "/" + navmesh_suffix,
                        });
                    }
                }

                {
                    lock_guard merge_lock {merge_mutex};
                    uint32_t episode_offset = episodes_.size();

                    for (SceneMetadata &scene : scenes) {
                        scene.firstEpisode += episode_offset;
                        scenes_.push_back(scene);
                    }

                    for (const Episode &episode : episodes) {
                        episodes_.push_back(episode);
                    }
                }
            });

            thread_file_offset += num_files;
        }

        for (uint32_t i = 0; i < num_threads; i++) {
            loader_threads[i].join();
        }
    }

    Span<const Episode> getEpisodes(uint32_t scene_idx) const
    {
        const SceneMetadata &scene = scenes_[scene_idx];
        return Span(&episodes_[scene.firstEpisode], scene.numEpisodes);
    }

    const string_view getScenePath(uint32_t scene_idx) const
    {
        return scenes_[scene_idx].meshPath;
    }

    const string_view getNavmeshPath(uint32_t scene_idx) const
    {
        return scenes_[scene_idx].navPath;
    }

    uint32_t numScenes() const { return scenes_.size(); }

private:
    vector<Episode> episodes_;
    vector<SceneMetadata> scenes_;
};

BatchRendererCUDA makeRenderer(int32_t gpu_id,
                               uint32_t renderer_batch_size,
                               uint32_t num_loaders,
                               const array<uint32_t, 2> &resolution,
                               bool color,
                               bool depth,
                               bool doubleBuffered)
{
    RenderOptions options {};
    if (doubleBuffered) {
        options |= RenderOptions::DoubleBuffered;
    }
    // options |= RenderOptions::CpuSynchronization;

    auto make = [&](auto features) {
        cudaSetDevice(gpu_id);
        size_t free_gpu_mem, total_gpu_mem;
        auto res [[maybe_unused]] = cudaMemGetInfo(&free_gpu_mem, &total_gpu_mem);
        assert(res == cudaSuccess);

        // Leave 1 GiB for non texture stuff. Super scientific.
        free_gpu_mem -= (1 << 30);

        return BatchRendererCUDA(
            {
                gpu_id,       // gpuID
                num_loaders,  // numLoaders
                1,            // numStreams
                renderer_batch_size, resolution[1], resolution[0],
                free_gpu_mem,
                glm::mat4(1, 0, 0, 0, 0, -1.19209e-07, -1, 0, 0, 1,
                          -1.19209e-07, 0, 0, 0, 0,
                          1),  // Habitat coordinate txfm matrix
            },
            features);
    };

    if (color && depth) {
        return make(
            RenderFeatures<Unlit<RenderOutputs::Color | RenderOutputs::Depth,
                                 DataSource::Texture>> {options});
    } else if (color) {
        return make(
            RenderFeatures<Unlit<RenderOutputs::Color, DataSource::Texture>> {
                options});
    } else {
        return make(
            RenderFeatures<Unlit<RenderOutputs::Depth, DataSource::None>> {
                options});
    }
}

template <typename T>
struct FastPromise {
    FastPromise() : value_ {nullptr}, status_ {nullptr} {}

    explicit FastPromise(T *value, atomic_uint32_t *status)
        : value_ {value},
          status_ {status}
    {}

    void set_result(T &&v)
    {
        *value_ = v;
        status_->store(1, memory_order_release);
    }

private:
    T *value_;
    atomic_uint32_t *status_;
};

template <typename T>
struct FastFuture {
    FastFuture() : value_ {new T}, status_ {new atomic_uint32_t}
    {
        status_->store(0, memory_order_relaxed);
    }

    FastFuture(FastFuture &&f) : value_ {nullptr}, status_ {nullptr}
    {
        std::swap(value_, f.value_);
        std::swap(status_, f.status_);
    }

    FastFuture &operator=(FastFuture &&f)
    {
        delete value_;
        delete status_;
        value_ = nullptr;
        status_ = nullptr;

        std::swap(value_, f.value_);
        std::swap(status_, f.status_);
        return *this;
    }

    bool valid() const { return status_->load(memory_order_relaxed) != 2; }
    bool isReady() const { return status_->load(memory_order_relaxed) == 1; }

    T get()
    {
        atomic_thread_fence(memory_order_acquire);
        status_->store(2, memory_order_relaxed);
        return move(*value_);
    }

    FastFuture(FastFuture &) = delete;
    FastFuture &operator=(FastFuture &) = delete;

    ~FastFuture()
    {
        delete value_;
        delete status_;
    }

    FastPromise<T> promise() { return FastPromise<T> {value_, status_}; }

private:
    T *value_;
    atomic_uint32_t *status_;
};

struct BackgroundSceneLoader {
    explicit BackgroundSceneLoader(AssetLoader &loader, int core_idx = -1)
        : loader_ {loader},
          loader_mutex_ {},
          loader_cv_ {},
          loader_exit_ {false},
          loader_requests_ {},
          loader_thread_ {[&]() { loaderLoop(core_idx); }} {};

    ~BackgroundSceneLoader()
    {
        {
            lock_guard<mutex> cv_lock(loader_mutex_);
            loader_exit_ = true;
        }
        loader_cv_.notify_one();
        loader_thread_.join();
    }

    shared_ptr<Scene> loadScene(string_view scene_path)
    {
        return loader_.loadScene(scene_path);
    }

    FastFuture<shared_ptr<Scene>> asyncLoadScene(string_view scene_path)
    {
        FastFuture<shared_ptr<Scene>> loader_future;

        {
            lock_guard<mutex> wait_lock(loader_mutex_);

            loader_requests_.emplace(scene_path, loader_future.promise());
        }
        loader_cv_.notify_one();

        return loader_future;
    }

private:
    void loaderLoop(int core_idx)
    {
        nice(19);

        set_affinity(core_idx);
        auto lastTime = std::chrono::system_clock::now();

        while (true) {
            string scene_path;
            FastPromise<shared_ptr<Scene>> loader_promise;
            {
                unique_lock<mutex> wait_lock(loader_mutex_);
                while (loader_requests_.size() == 0) {
                    if (loader_exit_) {
                        return;
                    }

                    loader_cv_.wait(wait_lock);
                }

                scene_path = move(loader_requests_.front().first);
                loader_promise = move(loader_requests_.front().second);
                loader_requests_.pop();
            }

            auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::system_clock::now() - lastTime)
                             .count();
            if (delta < RATE_LIMIT) usleep(RATE_LIMIT - delta);

            lastTime = std::chrono::system_clock::now();

            auto scene = loader_.loadScene(scene_path);
            loader_promise.set_result(move(scene));
        }
    };

    const uint32_t RATE_LIMIT = 1000000;
    AssetLoader &loader_;
    mutex loader_mutex_;
    condition_variable loader_cv_;
    bool loader_exit_;
    queue<pair<string, FastPromise<shared_ptr<Scene>>>> loader_requests_;
    thread loader_thread_;
};

struct StepInfo {
    float success;
    float spl;
    float distanceToGoal;
};

struct ResultPointers {
    float *reward;
    uint8_t *mask;
    StepInfo *info;
    glm::vec2 *polar;
};

static inline float computeGeoDist(esp::nav::ShortestPath &test_path,
                                   const esp::nav::NavMeshPoint &start,
                                   const esp::nav::NavMeshPoint &end,
                                   esp::nav::PathFinder &pathfinder)
{
    test_path.requestedStart = start;
    test_path.requestedEnd = end;

    pathfinder.findPath(test_path);
    return test_path.geodesicDistance;
}

class Simulator {
public:
    Simulator(Span<const Episode> episodes,
              Environment &render_env,
              ResultPointers ptrs)
        : episodes_(episodes),
          render_env_(&render_env),
          outputs_(ptrs),
          position_(),
          rotation_(),
          goal_(),
          navmeshPosition_(),
          navmeshGoal_(),
          initial_distance_to_goal_(),
          prev_distance_to_goal_(),
          cumulative_travel_distance_(),
          step_()
    {}

    void reset(esp::nav::PathFinder &pathfinder, mt19937 &rgen)
    {
        step_ = 1;

        std::uniform_int_distribution<uint64_t> episode_dist(
            0, episodes_.size() - 1);
        const Episode &episode = episodes_[episode_dist(rgen)];
        position_ = episode.startPosition;
        rotation_ = episode.startRotation;
        goal_ = episode.goal;
        navmeshGoal_ = pathfinder.snapPoint(
            Eigen::Map<const esp::vec3f>(glm::value_ptr(episode.goal)));
        navmeshPosition_ = pathfinder.snapPoint(
            Eigen::Map<const esp::vec3f>(glm::value_ptr(position_)));

        cumulative_travel_distance_ = 0;
        initial_distance_to_goal_ = computeGeoDist(
            test_path_, navmeshPosition_, navmeshGoal_, pathfinder);
        prev_distance_to_goal_ = initial_distance_to_goal_;

        updateObservationState();
    }

    bool step(int64_t raw_action, esp::nav::PathFinder &pathfinder)
    {
        SimAction action {raw_action};
        step_++;
        bool done = step_ >= SimulatorConfig::MAX_STEPS;
        float reward = -SimulatorConfig::SLACK_REWARD;

        float distance_to_goal = 0;
        float success = 0;
        float spl = 0;

        if (action == SimAction::Stop) {
            done = true;
            distance_to_goal = computeGeoDist(test_path_, navmeshGoal_,
                                              navmeshPosition_, pathfinder);
            success =
                float(distance_to_goal < SimulatorConfig::SUCCESS_DISTANCE);
            spl = success * initial_distance_to_goal_ /
                  max(initial_distance_to_goal_, cumulative_travel_distance_),
            reward += SimulatorConfig::SUCCESS_REWARD * spl;
        } else {
            glm::vec3 prev_position = position_;

            bool position_updated = handleMovement(action, pathfinder);
            updateObservationState();

            if (position_updated) {
                distance_to_goal = computeGeoDist(
                    test_path_, navmeshGoal_, navmeshPosition_, pathfinder);
                reward += prev_distance_to_goal_ - distance_to_goal;

                cumulative_travel_distance_ +=
                    glm::length(position_ - prev_position);

                prev_distance_to_goal_ = distance_to_goal;
            } else {
                distance_to_goal = prev_distance_to_goal_;
            }
        }

        StepInfo info {
            success,
            spl,
            distance_to_goal,
        };

        *outputs_.reward = reward;
        *outputs_.mask = done ? 0 : 1;
        *outputs_.info = info;

        return done;
    }

private:
    enum class SimAction : int64_t {
        Stop = 0,
        MoveForward = 1,
        TurnLeft = 2,
        TurnRight = 3
    };

    inline void updateObservationState()
    {
        // Update renderer view matrix (World -> Camera)
        glm::mat3 rot = glm::mat3_cast(rotation_);
        glm::mat3 transposed_rot = glm::transpose(rot);
        glm::mat4 new_view(transposed_rot);

        glm::vec3 eye_pos = position_ + SimulatorConfig::UP_VECTOR * 1.25f;

        glm::vec4 translate(transposed_rot * -eye_pos, 1.f);
        new_view[3] = translate;

        render_env_->setCameraView(new_view);

        // Write out polar coordinates
        glm::vec3 to_goal = goal_ - position_;
        glm::vec3 to_goal_view = transposed_rot * to_goal;

        auto cartesianToPolar = [](float x, float y) {
            float rho = glm::length(glm::vec2(x, y));
            float phi = atan2f(y, x);

            return glm::vec2(rho, -phi);
        };

        *outputs_.polar = cartesianToPolar(-to_goal_view.z, to_goal_view.x);
    }

    // Returns true when position updated
    inline bool handleMovement(SimAction action,
                               esp::nav::PathFinder &pathfinder)
    {
        switch (action) {
            case SimAction::MoveForward: {
                glm::vec3 delta =
                    glm::rotate(rotation_, SimulatorConfig::CAM_FWD_VECTOR);
                glm::vec3 new_pos = position_ + delta;

                navmeshPosition_ = pathfinder.tryStep(
                    navmeshPosition_,
                    Eigen::Map<const esp::vec3f>(glm::value_ptr(new_pos)));

                position_ = glm::make_vec3(navmeshPosition_.xyz.data());
                return true;
            }
            case SimAction::TurnLeft: {
                rotation_ = rotation_ * SimulatorConfig::LEFT_ROTATION;
                return false;
            }
            case SimAction::TurnRight: {
                rotation_ = rotation_ * SimulatorConfig::RIGHT_ROTATION;
                return false;
            }
            default: {
                cerr << "Unknown action: " << static_cast<int64_t>(action)
                     << endl;
                abort();
            }
        }
    }

    Span<const Episode> episodes_;
    Environment *render_env_;
    ResultPointers outputs_;

    glm::vec3 position_;
    glm::quat rotation_;
    glm::vec3 goal_;

    esp::nav::NavMeshPoint navmeshPosition_;
    esp::nav::NavMeshPoint navmeshGoal_;
    esp::nav::ShortestPath test_path_;

    float initial_distance_to_goal_;
    float prev_distance_to_goal_;
    float cumulative_travel_distance_;
    uint32_t step_;
};

template <typename T>
static py::array_t<T> makeFlatNumpyArray(const vector<T> &vec)
{
    // Final py::none argument stops the data from being copied
    // Technically unsafe, because the python array can persist
    // after the array is deallocated by C++, but the expectation
    // is the RolloutGenerator will live for the whole computation.
    return py::array_t<T>({vec.size()}, {sizeof(T)}, vec.data(), py::none());
}

class EnvironmentGroup;
class SceneTracker;

class ThreadEnvironment {
public:
    ThreadEnvironment(ThreadEnvironment &&) = default;

private:
    ThreadEnvironment(uint32_t idx, Simulator &sim, SceneTracker &scene)
        : idx_(idx),
          sim_(&sim),
          scene_(&scene)
    {}

    uint32_t idx_;
    Simulator *sim_;
    SceneTracker *scene_;

    friend class EnvironmentGroup;
};

static uint32_t computeNumLoaderCores(uint32_t num_active_scenes, bool color)
{
    if (!color)
        return max(num_cores() - 1,
                   1u);  // If possible, leave 1 core for pytorch

    return min(max(num_active_scenes, 1u), num_cores() / 2);
}
static uint32_t computeNumWorkers(int num_desired_workers,
                                  uint32_t num_active_scenes,
                                  bool color)
{
    assert(num_desired_workers != 0);
    if (num_desired_workers != -1) return num_desired_workers;

    uint32_t num_workers = num_cores() - 1;

    (void)num_active_scenes;
    (void)color;
#if 0
    if (color) {
        num_workers -=
            min(computeNumLoaderCores(num_active_scenes, color), num_workers);
    }
#endif

    return num_workers;
}

class SceneSwapper {
public:
    SceneSwapper(AssetLoader &&loader,
                 int background_loader_core_idx,
                 Dataset &dataset,
                 uint32_t &active_scene,
                 std::vector<uint32_t> &inactive_scenes,
                 uint32_t envs_per_scene,
                 mt19937 &rgen)
        : renderer_loader_ {move(loader)},
          num_scene_loads_ {0},
          next_scene_future_ {},
          next_scene_ {},
          loader_ {renderer_loader_, background_loader_core_idx},
          dataset_ {dataset},
          active_scene_ {active_scene},
          inactive_scenes_ {inactive_scenes},
          envs_per_scene_ {envs_per_scene},
          rgen_ {rgen}
    {}

    SceneSwapper() = delete;
    SceneSwapper(const SceneSwapper &) = delete;

    bool canSwapScene() const
    {
        return next_scene_ == nullptr && !next_scene_future_.valid();
    }

    void startSceneSwap()
    {
        assert(canSwapScene());
        if (inactive_scenes_.size() > 0) {
            uniform_int_distribution<uint32_t> scene_selector(
                0, inactive_scenes_.size() - 1);

            uint32_t new_scene_position = scene_selector(rgen_);

            swap(inactive_scenes_[new_scene_position], active_scene_);

            auto scene_path = dataset_.getScenePath(active_scene_);

            next_scene_future_ = loader_.asyncLoadScene(scene_path);
        }
    }

    void preStep()
    {
        if (next_scene_future_.isReady()) {
            next_scene_ = next_scene_future_.get();
            num_scene_loads_.store(envs_per_scene_, memory_order_relaxed);
        }
    }

    bool postStep()
    {
        if (next_scene_ != nullptr &&
            num_scene_loads_.load(memory_order_relaxed) == 0) {
            next_scene_ = nullptr;
            startSceneSwap();
            return true;
        }

        return false;
    }

    void oneLoaded() { num_scene_loads_.fetch_sub(1, memory_order_relaxed); }

    BackgroundSceneLoader &getLoader() { return loader_; }
    const shared_ptr<Scene> &getNextScene() const { return next_scene_; }
    atomic_uint32_t &getNumSceneLoads() { return num_scene_loads_; }

private:
    AssetLoader renderer_loader_;

    // Futures need to be destroyed before AssetLoader
    atomic_uint32_t num_scene_loads_;
    FastFuture<shared_ptr<Scene>> next_scene_future_;
    shared_ptr<Scene> next_scene_;

    BackgroundSceneLoader loader_;

    Dataset &dataset_;

    uint32_t &active_scene_;
    vector<uint32_t> &inactive_scenes_;
    uint32_t envs_per_scene_;

    mt19937 &rgen_;
};

class SceneTracker {
public:
    SceneTracker(const uint32_t *src_ptr, SceneSwapper *swapper)
        : src_ptr_(src_ptr),
          cur_(*src_ptr_),
          swapper_(swapper)
    {}

    bool isConsistent() const { return *src_ptr_ == cur_; }

    void update() { cur_ = *src_ptr_; }

    uint32_t curScene() const { return cur_; }

    SceneSwapper &getSwapper() { return *swapper_; }
    const SceneSwapper &getSwapper() const { return *swapper_; }

private:
    const uint32_t *src_ptr_;
    uint32_t cur_;

    SceneSwapper *swapper_;
};

class EnvironmentGroup {
public:
    EnvironmentGroup(CommandStreamCUDA &strm,
                     BackgroundSceneLoader &loader,
                     const Dataset &dataset,
                     uint32_t envs_per_scene,
                     const Span<const uint32_t> &initial_scene_indices,
                     const Span<SceneSwapper> &scene_swappers)
        : cmd_strm_(strm),
          dataset_(dataset),
          render_envs_(),
          sim_states_(),
          env_scenes_(),
          rewards_(envs_per_scene * initial_scene_indices.size()),
          masks_(rewards_.size()),
          infos_(rewards_.size()),
          polars_(rewards_.size())
    {
        render_envs_.reserve(rewards_.size());
        sim_states_.reserve(rewards_.size());
        env_scenes_.reserve(rewards_.size());

        // Take address of scene_idx so all envs can know when
        // the corresponding active scene is updated by a swap
        for (uint32_t i = 0; i < initial_scene_indices.size(); i++) {
            const uint32_t &scene_idx = initial_scene_indices[i];
            SceneSwapper &scene_swapper = scene_swappers[i];

            auto scene_path = dataset_.getScenePath(scene_idx);
            auto scene = loader.loadScene(scene_path);

            auto scene_episodes = dataset_.getEpisodes(scene_idx);

            for (uint32_t env_idx = 0; env_idx < envs_per_scene; env_idx++) {
                render_envs_.emplace_back(
                    strm.makeEnvironment(scene, 90, 0.1, 1000));
                sim_states_.emplace_back(scene_episodes, render_envs_.back(),
                                         getPointers(sim_states_.size()));
                env_scenes_.emplace_back(&scene_idx, &scene_swapper);
            }
        }
    }

    std::tuple<float, float> sceneStats()
    {
        std::unordered_map<uint32_t, uint32_t> counts_per_scene;
        for (auto &tracker : env_scenes_) {
            auto p = counts_per_scene.emplace(tracker.curScene(), 0);
            ++(p.first->second);
        }

        const float num_scenes = counts_per_scene.size();
        float avg_count = 0;
        for (auto &p : counts_per_scene) avg_count += p.second;
        avg_count /= num_scenes;

        return {num_scenes, avg_count};
    }

    void render() { cmd_strm_.render(render_envs_); }

    py::array_t<float> getRewards() const
    {
        return makeFlatNumpyArray(rewards_);
    }

    py::array_t<uint8_t> getMasks() const
    {
        return makeFlatNumpyArray(masks_);
    }

    py::array_t<StepInfo> getInfos() const
    {
        return makeFlatNumpyArray(infos_);
    }

    py::array_t<float> getPolars() const
    {
        return py::array_t<float>({polars_.size(), 2ul},
                                  {sizeof(float) * 2, sizeof(float)},
                                  &polars_[0].x, py::none());
    }

    ThreadEnvironment makeThreadEnv(uint32_t env_idx)
    {
        return ThreadEnvironment(env_idx, sim_states_[env_idx],
                                 env_scenes_[env_idx]);
    }

    inline bool step(const ThreadEnvironment &env,
                     vector<esp::nav::PathFinder> &pathfinders,
                     int64_t action)
    {
        return env.sim_->step(action, pathfinders[env.scene_->curScene()]);
    }

    inline void reset(const ThreadEnvironment &env,
                      vector<esp::nav::PathFinder> &pathfinders,
                      mt19937 &rgen)
    {
        env.sim_->reset(pathfinders[env.scene_->curScene()], rgen);
    }

    bool swapReady(const ThreadEnvironment &env) const
    {
        const auto &scene_tracker = env_scenes_[env.idx_];
        return scene_tracker.getSwapper().getNextScene() != nullptr &&
               !scene_tracker.isConsistent();
    }

    void swapScene(ThreadEnvironment &env)
    {
        auto &scene_tracker = env_scenes_[env.idx_];
        SceneSwapper &swapper = scene_tracker.getSwapper();
        shared_ptr<Scene> scene_data = swapper.getNextScene();

        render_envs_[env.idx_] =
            cmd_strm_.makeEnvironment(move(scene_data), 90, 0.01, 1000);

        scene_tracker.update();
        uint32_t scene_idx = scene_tracker.curScene();

        auto scene_episodes = dataset_.getEpisodes(scene_idx);

        sim_states_[env.idx_] = Simulator(
            scene_episodes, render_envs_[env.idx_], getPointers(env.idx_));

        swapper.oneLoaded();
    }

private:
    ResultPointers getPointers(uint32_t idx)
    {
        return ResultPointers {
            &rewards_[idx],
            &masks_[idx],
            &infos_[idx],
            &polars_[idx],
        };
    };

    CommandStreamCUDA &cmd_strm_;
    const Dataset &dataset_;
    vector<Environment> render_envs_;
    vector<Simulator> sim_states_;
    vector<SceneTracker> env_scenes_;
    vector<float> rewards_;
    vector<uint8_t> masks_;
    vector<StepInfo> infos_;
    vector<glm::vec2> polars_;
};

class RolloutGenerator {
public:
    RolloutGenerator(const string &dataset_path,
                     const string &asset_path,
                     uint32_t num_environments,
                     uint32_t num_active_scenes,
                     int num_workers,
                     int gpu_id,
                     const array<uint32_t, 2> &render_resolution,
                     bool color,
                     bool depth,
                     bool double_buffered,
                     uint64_t seed,
                     bool should_set_affinity = true)
        : RolloutGenerator(
              dataset_path,
              asset_path,
              num_environments,
              num_active_scenes,
              computeNumWorkers(num_workers, num_active_scenes, color),
              gpu_id,
              render_resolution,
              color,
              depth,
              double_buffered ? 2u : 1u,
              seed,
              should_set_affinity)
    {}

    ~RolloutGenerator()
    {
        exit_ = true;

        atomic_thread_fence(memory_order_release);
        start_atomic_.fetch_xor(1, memory_order_release);
        atomic_notify_all(&start_atomic_);

        for (auto &t : worker_threads_) {
            t.join();
        }

        pthread_barrier_destroy(&ready_barrier_);
    }

    void reset(uint32_t group_idx)
    {
        simulateAndRender(group_idx, true, nullptr);
    }

    void stepStart(uint32_t group_idx,
                   const py::array_t<int64_t, py::array::c_style> &actions)
    {
        for (auto &swapper : scene_swappers_) swapper.preStep();

        auto action_raw = actions.unchecked<1>();

        simulateStart(group_idx, false, action_raw.data(0));

        num_steps_taken_ += actions.shape(0);
    }

    void stepEnd(uint32_t group_idx)
    {
        simulateEnd(group_idx);
        for (auto &swapper : scene_swappers_)
            num_scenes_swapped_ += swapper.postStep() ? 1 : 0;
    };

    void render(uint32_t group_idx) { groups_[group_idx].render(); }

    // action_ptr is a uint64_t to match torch.tensor.data_ptr()
    void step(uint32_t group_idx,
              const py::array_t<int64_t, py::array::c_style> &actions)
    {
        stepStart(group_idx, actions);
        stepEnd(group_idx);
        render(group_idx);
    }

    std::tuple<float, float, float> swapStats()
    {
        auto groupStats = groups_[0].sceneStats();
        for (uint32_t i = 1; i < groups_.size(); ++i) {
            auto otherStats = groups_[i].sceneStats();
            std::get<0>(groupStats) += std::get<0>(otherStats);
            std::get<1>(groupStats) += std::get<1>(otherStats);
        }

        return {static_cast<double>(num_scenes_swapped_) /
                    static_cast<double>(num_steps_taken_) * 100.0,
                std::get<0>(groupStats), std::get<1>(groupStats)};
    }

    py::array_t<float> getRewards(uint32_t group_idx) const
    {
        return groups_[group_idx].getRewards();
    }

    py::array_t<uint8_t> getMasks(uint32_t group_idx) const
    {
        return groups_[group_idx].getMasks();
    }

    py::array_t<StepInfo> getInfos(uint32_t group_idx) const
    {
        return groups_[group_idx].getInfos();
    }

    py::array_t<float> getPolars(uint32_t group_idx) const
    {
        return groups_[group_idx].getPolars();
    }

    py::capsule getColorMemory(const uint32_t groupIdx) const
    {
        return py::capsule(cmd_strm_.getColorDevicePtr(groupIdx));
    }

    py::capsule getDepthMemory(const uint32_t groupIdx) const
    {
        return py::capsule(cmd_strm_.getDepthDevicePtr(groupIdx));
    }

    py::capsule getCUDASemaphore(const uint32_t groupIdx) const
    {
        return py::capsule(cmd_strm_.getCudaSemaphore(groupIdx));
    }

    void waitForFrame(const uint32_t groupIdx)
    {
        cmd_strm_.waitForFrame(groupIdx);
    }

    void printRendererStats() const {
        Statistics renderer_stats = renderer_.getStatistics();

        cout << "Renderer Statistics -- "
             << "Input Setup: " << renderer_stats.inputSetup << " "
             << "Command Record: " << renderer_stats.commandRecord << " "
             << "Command Submission: " << renderer_stats.renderSubmit << endl;
    }

private:
    RolloutGenerator(const string &dataset_path,
                     const string &asset_path,
                     uint32_t num_environments,
                     uint32_t num_active_scenes,
                     uint32_t num_workers,
                     int gpu_id,
                     const array<uint32_t, 2> &render_resolution,
                     bool color,
                     bool depth,
                     uint32_t num_groups,
                     uint64_t seed,
                     bool should_set_affinity)
        : dataset_(dataset_path, asset_path, num_workers),
          renderer_(makeRenderer(gpu_id,
                                 num_environments / num_groups,
                                 num_active_scenes,
                                 render_resolution,
                                 color,
                                 depth,
                                 num_groups == 2)),
          cmd_strm_(renderer_.makeCommandStream()),
          envs_per_scene_(num_environments / num_active_scenes),
          envs_per_group_(num_environments / num_groups),
          active_scenes_(),
          inactive_scenes_(),
          rgen_(seed),
          scene_swappers_(num_active_scenes),
          groups_(),
          thread_envs_(),
          main_thread_pathfinders_(),
          wait_target_(1 + num_workers),
          worker_threads_(),
          ready_barrier_(),
          start_atomic_(),
          workers_finished_(1 + num_workers),
          next_env_queue_(0),
          active_group_(),
          active_actions_(nullptr),
          sim_reset_(false),
          exit_(false)
    {
        if ((num_environments % num_active_scenes) != 0) {
            cerr << "Num environments is not a multiple of the number of "
                    "active scenes"
                 << std::endl;
            abort();
        }

        groups_.reserve(num_groups);
        worker_threads_.reserve(num_workers);

        active_scenes_.reserve(num_active_scenes);
        inactive_scenes_.reserve(dataset_.numScenes() - num_active_scenes);

        assert(dataset_.numScenes() > num_active_scenes);

        uniform_real_distribution<> selection_distribution(0.f, 1.f);
        uint32_t scene_idx;
        for (scene_idx = 0; scene_idx < dataset_.numScenes() &&
                            active_scenes_.size() < num_active_scenes;
             scene_idx++) {
            float weight = selection_distribution(rgen_);
            if (weight * float(dataset_.numScenes() - scene_idx) <
                float(num_active_scenes - active_scenes_.size())) {
                active_scenes_.push_back(scene_idx);
            } else {
                inactive_scenes_.push_back(scene_idx);
            }
        }

        for (; scene_idx < dataset_.numScenes(); scene_idx++) {
            inactive_scenes_.push_back(scene_idx);
        }

        assert(num_environments % num_groups == 0);
        assert(num_environments % num_active_scenes == 0);
        assert(num_active_scenes % num_groups == 0);

        uint32_t num_scene_loader_cores =
            computeNumLoaderCores(num_active_scenes, color);

        uint32_t num_worker_cores = num_cores() - 1;

#if 0
        if (color) {
            if (num_scene_loader_cores > num_worker_cores) {
                num_scene_loader_cores = num_worker_cores;
                num_worker_cores = 0;
            } else {
                num_worker_cores -= num_scene_loader_cores;
            }
        }
#endif

        for (uint32_t i = 0; i < num_active_scenes; ++i) {
            int core_idx = -1;
            if (should_set_affinity) {
                if (false && color) {
                    // For RGB, map scene loader cores to after all worker
                    // cores
                    core_idx =
                        1 + num_worker_cores + (i % num_scene_loader_cores);
                } else {
                    // For depth, map them to the end of the range so they
                    // don't overlap with the pytorch thread (this thread /
                    // core 0)
                    core_idx = num_cores() - 1 - (i % num_scene_loader_cores);
                }
            }

            new (&scene_swappers_[i]) SceneSwapper(
                renderer_.makeLoader(), core_idx, dataset_, active_scenes_[i],
                inactive_scenes_, envs_per_scene_, rgen_);
        }

        uint32_t scenes_per_group = num_active_scenes / num_groups;
        thread_envs_.reserve(num_environments);

        for (uint32_t i = 0; i < num_groups; i++) {
            groups_.emplace_back(
                cmd_strm_, scene_swappers_[0].getLoader(), dataset_,
                envs_per_scene_,
                Span<const uint32_t>(&active_scenes_[i * scenes_per_group],
                                     scenes_per_group),
                Span(&scene_swappers_[i * scenes_per_group],
                     scenes_per_group));
            for (uint32_t env_idx = 0; env_idx < envs_per_group_; env_idx++) {
                thread_envs_.emplace_back(groups_[i].makeThreadEnv(env_idx));
            }
        }

        for (auto &swapper : scene_swappers_) swapper.startSceneSwap();

        pthread_barrier_init(&ready_barrier_, nullptr, num_workers + 1);

        for (uint32_t thread_idx = 0; thread_idx < num_workers; thread_idx++) {
            int core_idx = should_set_affinity ?
                               (1 + (thread_idx % num_worker_cores)) :
                               -1;

            worker_threads_.emplace_back([this, seed, thread_idx, core_idx]() {
                simulationWorker(seed + 1 + thread_idx, core_idx);
            });
        }

        // This main thread is the first "worker thread", implicitly has
        // affinity 0, don't want to set as could be inherited by pytorch
        // stuff later.
        set_affinity(should_set_affinity ? 0 : -1);

        main_thread_pathfinders_ = initPathfinders();

        // Wait for all threads to reach the start of the their work loop.
        pthread_barrier_wait(&ready_barrier_);
    }

    inline bool simulate(vector<esp::nav::PathFinder> &thread_pathfinders,
                         mt19937 &rgen)
    {
        const bool trigger_reset = sim_reset_;
        EnvironmentGroup &group = groups_[active_group_];

        uint32_t next_env;
        while ((next_env = next_env_queue_.fetch_add(
                    1, memory_order_acq_rel)) < envs_per_group_) {
            ThreadEnvironment &env =
                thread_envs_[next_env + active_group_ * envs_per_group_];

            if (trigger_reset) {
                group.reset(env, thread_pathfinders, rgen);
            } else {
                bool done = group.step(env, thread_pathfinders,
                                       active_actions_[next_env]);
                if (done) {
                    if (group.swapReady(env)) {
                        group.swapScene(env);
                    }

                    group.reset(env, thread_pathfinders, rgen);
                }
            }
        }

        // Returns true to a thread when this iteration is done. Used as small
        // optimization to avoid extra load when main thread finishes last.
        // worker_threads_.size() is equal to the value that this needs to
        // be incremented to minus - 1. Conveniently, fetch_add
        // returns the value before the addition.
        return workers_finished_.fetch_add(1, memory_order_acq_rel) ==
               worker_threads_.size();
    }

    void simulateStart(uint32_t active_group,
                       bool trigger_reset,
                       const int64_t *action_ptr)
    {
        if (workers_finished_.load(memory_order_acquire) != wait_target_) {
            cerr << "Not done with previous simulation" << endl;
            abort();
        }

        active_group_ = active_group;
        active_actions_ = action_ptr;
        sim_reset_ = trigger_reset;

        next_env_queue_.store(0, memory_order_relaxed);
        workers_finished_.store(0, memory_order_relaxed);

        atomic_thread_fence(memory_order_release);

        start_atomic_.fetch_xor(1, memory_order_release);
        atomic_notify_all(&start_atomic_);
    }

    void simulateEnd(uint32_t active_group)
    {
        if (workers_finished_.load(memory_order_acquire) == wait_target_) {
            cerr << "Simulation already done" << endl;
            abort();
        }

        if (active_group != active_group_) {
            cerr << "Group to end simulation differs from currently active "
                    "group"
                 << endl;
            abort();
        }

        bool finished = simulate(main_thread_pathfinders_, rgen_);
        if (!finished) {
            while (workers_finished_.load(memory_order_acquire) !=
                   wait_target_) {
                asm volatile("pause" ::: "memory");
            }
        }

        atomic_thread_fence(memory_order_acquire);
    }

    void simulateAndRender(uint32_t active_group,
                           bool trigger_reset,
                           const int64_t *action_ptr)
    {
        simulateStart(active_group, trigger_reset, action_ptr);
        simulateEnd(active_group);

        render(active_group);
    }

    vector<esp::nav::PathFinder> initPathfinders()
    {
        vector<esp::nav::PathFinder> pathfinders(dataset_.numScenes());

        for (uint32_t scene_idx = 0; scene_idx < dataset_.numScenes();
             scene_idx++) {
            auto navmesh_path = dataset_.getNavmeshPath(scene_idx);

            bool navmesh_success =
                pathfinders[scene_idx].loadNavMesh(string(navmesh_path));

            if (!navmesh_success) {
                cerr << "Failed to load navmesh: " << navmesh_path << endl;
                abort();
            }
        }

        return pathfinders;
    }

    void simulationWorker(uint64_t seed, int core_idx)
    {
        set_affinity(core_idx);

        mt19937 rgen(seed);

        vector<esp::nav::PathFinder> thread_pathfinders = initPathfinders();

        pthread_barrier_wait(&ready_barrier_);

        uint32_t wait_val = 0;
        while (true) {
            atomic_wait_explicit(&start_atomic_, wait_val,
                                 memory_order_acquire);
            wait_val ^= 1;

            if (exit_) {
                return;
            }

            simulate(thread_pathfinders, rgen);
        }
    }

    Dataset dataset_;
    BatchRendererCUDA renderer_;
    CommandStreamCUDA cmd_strm_;
    uint32_t envs_per_scene_;
    uint32_t envs_per_group_;
    vector<uint32_t> active_scenes_;
    vector<uint32_t> inactive_scenes_;

    mt19937 rgen_;
    DynArray<SceneSwapper> scene_swappers_;
    vector<EnvironmentGroup> groups_;
    vector<ThreadEnvironment> thread_envs_;
    vector<esp::nav::PathFinder> main_thread_pathfinders_;
    const uint32_t wait_target_;

    vector<thread> worker_threads_;
    pthread_barrier_t ready_barrier_;
    atomic_uint32_t start_atomic_;
    atomic_uint32_t workers_finished_;

    atomic_uint32_t next_env_queue_;
    uint32_t active_group_;
    const int64_t *active_actions_;
    bool sim_reset_;
    bool exit_;

    uint64_t num_steps_taken_ = 0;
    uint64_t num_scenes_swapped_ = 0;
};

PYBIND11_MODULE(ddppo_fastrollout, m)
{
    PYBIND11_NUMPY_DTYPE(StepInfo, success, spl, distanceToGoal);
    py::class_<RolloutGenerator>(m, "RolloutGenerator")
        .def(py::init<const string &, const string &, uint32_t, uint32_t, int,
                      int, const array<uint32_t, 2> &, bool, bool, bool,
                      uint64_t, bool>())
        .def(py::init<const string &, const string &, uint32_t, uint32_t, int,
                      int, const array<uint32_t, 2> &, bool, bool, bool,
                      uint64_t>())
        .def("wait_for_frame", &RolloutGenerator::waitForFrame)
        .def("step", &RolloutGenerator::step)
        .def("step_start", &RolloutGenerator::stepStart)
        .def("step_end", &RolloutGenerator::stepEnd)
        .def("render", &RolloutGenerator::render)
        .def("reset", &RolloutGenerator::reset)
        .def("rgba", &RolloutGenerator::getColorMemory)
        .def("depth", &RolloutGenerator::getDepthMemory)
        .def("get_cuda_semaphore", &RolloutGenerator::getCUDASemaphore)
        .def("get_rewards", &RolloutGenerator::getRewards)
        .def("get_masks", &RolloutGenerator::getMasks)
        .def("get_infos", &RolloutGenerator::getInfos)
        .def("get_polars", &RolloutGenerator::getPolars)
        .def("print_renderer_stats", &RolloutGenerator::printRendererStats)
        .def_property_readonly("swap_stats", &RolloutGenerator::swapStats);
}
