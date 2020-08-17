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

static const glm::quat LEFT_ROTATION = glm::angleAxis(-TURN_ANGLE, UP_VECTOR);

static const glm::quat RIGHT_ROTATION = glm::angleAxis(TURN_ANGLE, UP_VECTOR);
}

template <typename T>
class Span {
public:
    Span(T *base, size_t num_elems) : ptr_(base), num_elems_(num_elems) {}

    T &operator[](size_t idx) { return ptr_[idx]; }
    const T &operator[](size_t idx) const { return ptr_[idx]; }

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

                        // FIXME is there some more principled way to get this?
                        string navmesh_suffix =
                            string(scene_id.substr(0, dotpos)) + ".navmesh";

                        scenes.push_back({
                            scene_episode_start,
                            static_cast<uint32_t>(episodes.size() -
                                                  scene_episode_start),
                            asset_path_name + "/" + string(scene_id),
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
                               const array<uint32_t, 2> &resolution,
                               bool color,
                               bool depth,
                               bool doubleBuffered)
{
    RenderOptions options {};
    if (doubleBuffered) {
        options |= RenderOptions::DoubleBuffered;
    }

    auto make = [&](auto features) {
        return BatchRendererCUDA(
            {
                gpu_id,  // gpuID
                1,       // numLoaders
                1,       // numStreams
                renderer_batch_size, resolution[1], resolution[0],
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

template <typename R>
static bool isReady(const future<R> &f)
{
    return f.wait_for(chrono::seconds(0)) == future_status::ready;
}

struct BackgroundSceneLoader {
    explicit BackgroundSceneLoader(AssetLoader &&loader)
        : loader_ {move(loader)},
          loader_mutex_ {},
          loader_cv_ {},
          loader_exit_ {false},
          loader_requests_ {},
          loader_thread_ {[&]() { loaderLoop(); }} {};

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
        auto fut = asyncLoadScene(scene_path);
        fut.wait();
        return fut.get();
    }

    future<shared_ptr<Scene>> asyncLoadScene(string_view scene_path)
    {
        future<shared_ptr<Scene>> loader_future;

        {
            lock_guard<mutex> wait_lock(loader_mutex_);

            promise<shared_ptr<Scene>> loader_promise;
            loader_future = loader_promise.get_future();

            loader_requests_.emplace(scene_path, move(loader_promise));
        }
        loader_cv_.notify_one();

        return loader_future;
    }

private:
    void loaderLoop()
    {
        while (true) {
            string scene_path;
            promise<shared_ptr<Scene>> loader_promise;
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

            auto scene = loader_.loadScene(scene_path);
            loader_promise.set_value(move(scene));
        }
    };

    AssetLoader loader_;

    mutex loader_mutex_;
    condition_variable loader_cv_;
    bool loader_exit_;
    queue<pair<string, promise<shared_ptr<Scene>>>> loader_requests_;
    thread loader_thread_;
};

struct StepInfo {
    float success;
    float spl;
    float distanceToGoal;
};

struct ResultPointers {
    float *reward;
    float *mask;
    StepInfo *info;
    glm::vec2 *polar;
};

class Simulator {
public:
    Simulator(Span<const Episode> episodes,
              string_view navmesh_path,
              Environment &render_env,
              ResultPointers ptrs,
              mt19937 &rgen)
        : episodes_(episodes),
          cur_episode_(0),
          episode_order_(),
          render_env_(&render_env),
          outputs_(ptrs),
          pathfinder_(make_unique<esp::nav::PathFinder>()),
          position_(),
          rotation_(),
          goal_(),
          navmeshPosition_(),
          navmeshGoal_(),
          initial_distance_to_goal_(),
          prev_distance_to_goal_(),
          cumulative_travel_distance_(),
          step_()
    {
        episode_order_.reserve(episodes_.size());
        for (uint32_t i = 0; i < episodes_.size(); i++) {
            episode_order_.push_back(i);
        }

        shuffle(episode_order_.begin(), episode_order_.end(), rgen);

        bool navmesh_success = pathfinder_->loadNavMesh(string(navmesh_path));
        if (!navmesh_success) {
            cerr << "Failed to load navmesh: " << navmesh_path << endl;
            abort();
        }
    }

    void reset()
    {
        step_ = 1;

        const Episode &episode = episodes_[episode_order_[cur_episode_]];
        position_ = episode.startPosition;
        rotation_ = episode.startRotation;
        goal_ = episode.goal;
        navmeshGoal_ = pathfinder_->snapPoint(
            Eigen::Map<const esp::vec3f>(glm::value_ptr(episode.goal)));
        navmeshPosition_ = pathfinder_->snapPoint(
            Eigen::Map<const esp::vec3f>(glm::value_ptr(position_)));

        cumulative_travel_distance_ = 0;
        initial_distance_to_goal_ = geoDist(navmeshPosition_, navmeshGoal_);
        prev_distance_to_goal_ = initial_distance_to_goal_;

        updateObservationState();

        cur_episode_++;
    }

    bool step(int64_t raw_action)
    {
        SimAction action {raw_action};
        step_++;
        bool done = step_ >= SimulatorConfig::MAX_STEPS;
        float reward = -SimulatorConfig::SLACK_REWARD;

        float success = 0;
        float distance_to_goal = 0;

        if (action == SimAction::Stop) {
            done = true;
            distance_to_goal = geoDist(navmeshGoal_, navmeshPosition_);
            success =
                float(distance_to_goal <= SimulatorConfig::SUCCESS_DISTANCE);
            reward += success * SimulatorConfig::SUCCESS_REWARD;
        } else {
            glm::vec3 prev_position = position_;

            handleMovement(action);
            updateObservationState();

            distance_to_goal = geoDist(navmeshGoal_, navmeshPosition_);
            reward += prev_distance_to_goal_ - distance_to_goal;
            cumulative_travel_distance_ +=
                glm::length(position_ - prev_position);

            prev_distance_to_goal_ = distance_to_goal;
        }

        StepInfo info {
            success,
            success * initial_distance_to_goal_ /
                max(initial_distance_to_goal_, cumulative_travel_distance_),
            distance_to_goal,
        };

        *outputs_.reward = reward;
        if (done) {
            *outputs_.mask = 0.f;
        } else {
            *outputs_.mask = 1.f;
        }
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

    inline float geoDist(const esp::nav::NavMeshPoint &start,
                         const esp::nav::NavMeshPoint &end)
    {
        esp::nav::ShortestPath test_path;

        test_path.requestedStart = start;
        test_path.requestedEnd = end;

        pathfinder_->findPath(test_path);
        return test_path.geodesicDistance;
    }

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

    inline void handleMovement(SimAction action)
    {
        switch (action) {
            case SimAction::MoveForward: {
                glm::vec3 delta =
                    glm::rotate(rotation_, SimulatorConfig::CAM_FWD_VECTOR);
                glm::vec3 new_pos = position_ + delta;

                navmeshPosition_ = pathfinder_->tryStep(
                    navmeshPosition_,
                    Eigen::Map<const esp::vec3f>(glm::value_ptr(new_pos)));

                position_ = glm::make_vec3(navmeshPosition_.xyz.data());
                break;
            }
            case SimAction::TurnLeft: {
                rotation_ = SimulatorConfig::LEFT_ROTATION * rotation_;
                break;
            }
            case SimAction::TurnRight: {
                rotation_ = SimulatorConfig::RIGHT_ROTATION * rotation_;
                break;
            }
            default: {
                cerr << "Unknown action: " << static_cast<int64_t>(action)
                     << endl;
                abort();
            }
        }
    }

    Span<const Episode> episodes_;
    uint32_t cur_episode_;
    vector<uint32_t> episode_order_;
    Environment *render_env_;
    ResultPointers outputs_;
    unique_ptr<esp::nav::PathFinder> pathfinder_;

    glm::vec3 position_;
    glm::quat rotation_;
    glm::vec3 goal_;

    esp::nav::NavMeshPoint navmeshPosition_;
    esp::nav::NavMeshPoint navmeshGoal_;

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

class EnvironmentGroup {
public:
    EnvironmentGroup(CommandStreamCUDA &strm,
                     BackgroundSceneLoader &loader,
                     const Dataset &dataset,
                     mt19937 &rgen,
                     uint32_t envs_per_scene,
                     const Span<uint32_t> &initial_scene_indices)
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
        for (const uint32_t &scene_idx : initial_scene_indices) {
            auto scene_path = dataset_.getScenePath(scene_idx);
            auto scene = loader.loadScene(scene_path);

            auto scene_episodes = dataset_.getEpisodes(scene_idx);
            auto scene_navmesh = dataset_.getNavmeshPath(scene_idx);

            for (uint32_t env_idx = 0; env_idx < envs_per_scene; env_idx++) {
                render_envs_.emplace_back(
                    strm.makeEnvironment(scene, 90, 0.1, 1000));
                sim_states_.emplace_back(
                    scene_episodes, scene_navmesh, render_envs_.back(),
                    getPointers(sim_states_.size()), rgen);
                env_scenes_.emplace_back(&scene_idx);
            }
        }
    }

    void render() { cmd_strm_.render(render_envs_); }

    Simulator &getSimulator(uint32_t idx) { return sim_states_[idx]; }

    Environment &getEnvironment(uint32_t idx) { return render_envs_[idx]; }

    py::array_t<float> getRewards() const
    {
        return makeFlatNumpyArray(rewards_);
    }

    py::array_t<float> getMasks() const { return makeFlatNumpyArray(masks_); }

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

    bool swapReady(uint32_t env_idx) const
    {
        const auto &scene_tracker = env_scenes_[env_idx];
        return !scene_tracker.isConsistent();
    }

    void swapScene(uint32_t env_idx,
                   const shared_ptr<Scene> &scene_data,
                   std::mt19937 &rgen)
    {
        render_envs_[env_idx] =
            cmd_strm_.makeEnvironment(scene_data, 90, 0.01, 1000);

        auto &scene_tracker = env_scenes_[env_idx];
        scene_tracker.update();
        uint32_t scene_idx = scene_tracker.curScene();

        auto scene_episodes = dataset_.getEpisodes(scene_idx);
        auto scene_navmesh = dataset_.getNavmeshPath(scene_idx);

        sim_states_[env_idx] =
            Simulator(scene_episodes, scene_navmesh, render_envs_[env_idx],
                      getPointers(env_idx), rgen);
    }

private:
    class SceneTracker {
    public:
        SceneTracker(const uint32_t *src_ptr)
            : src_ptr_(src_ptr),
              cur_(*src_ptr_)
        {}

        bool isConsistent() const { return *src_ptr_ == cur_; }

        void update() { cur_ = *src_ptr_; }

        uint32_t curScene() const { return cur_; }

    private:
        const uint32_t *src_ptr_;
        uint32_t cur_;
    };

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
    vector<float> masks_;
    vector<StepInfo> infos_;
    vector<glm::vec2> polars_;
};

static uint32_t computeNumWorkers(int num_desired_workers)
{
    assert(num_desired_workers != 0);
    return num_desired_workers == -1 ?
               max(static_cast<int64_t>(thread::hardware_concurrency()) - 1,
                   1l) :
               num_desired_workers;
}

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
                     uint64_t seed)
        : RolloutGenerator(dataset_path,
                           asset_path,
                           num_environments,
                           num_active_scenes,
                           computeNumWorkers(num_workers),
                           gpu_id,
                           render_resolution,
                           color,
                           depth,
                           double_buffered ? 2u : 1u,
                           seed)
    {}

    ~RolloutGenerator()
    {
        exit_ = true;
        pthread_barrier_wait(&start_barrier_);

        for (auto &t : worker_threads_) {
            t.join();
        }

        pthread_barrier_destroy(&start_barrier_);
        pthread_barrier_destroy(&finish_barrier_);
    }

    bool canSwapScene() const
    {
        return next_scene_ == nullptr && !next_scene_future_.valid();
    }

    void triggerSwapScene(uint32_t active_scene_idx)
    {
        assert(canSwapScene());
        assert(active_scene_idx < active_scenes_.size());

        uniform_int_distribution<uint32_t> scene_selector(
            0, inactive_scenes_.size() - 1);

        uint32_t new_scene_position = scene_selector(rgen_);

        swap(inactive_scenes_[new_scene_position],
             active_scenes_[active_scene_idx]);

        uint32_t scene_idx = active_scenes_[active_scene_idx];

        auto scene_path = dataset_.getScenePath(scene_idx);

        next_scene_future_ = loader_.asyncLoadScene(scene_path);
    }

    void reset(uint32_t group_idx)
    {
        simulateAndRender(group_idx, true, nullptr);
    }

    // action_ptr is a uint64_t to match torch.tensor.data_ptr()
    void step(uint32_t group_idx,
              const py::array_t<int64_t, py::array::c_style> &actions)
    {
        if (next_scene_future_.valid() && isReady(next_scene_future_)) {
            next_scene_ = next_scene_future_.get();
            num_scene_loads_.store(envs_per_scene_, memory_order_relaxed);
        }

        auto action_raw = actions.unchecked<1>();

        simulateAndRender(group_idx, false, action_raw.data(0));

        if (next_scene_ != nullptr &&
            num_scene_loads_.load(memory_order_relaxed) == 0) {
            next_scene_ = nullptr;
        }
    }

    py::array_t<float> getRewards(uint32_t group_idx) const
    {
        return groups_[group_idx].getRewards();
    }

    py::array_t<float> getMasks(uint32_t group_idx) const
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
                     uint64_t seed)
        : dataset_(dataset_path, asset_path, num_workers),
          renderer_(makeRenderer(gpu_id,
                                 num_environments / num_groups,
                                 render_resolution,
                                 color,
                                 depth,
                                 num_groups == 2)),
          cmd_strm_(renderer_.makeCommandStream()),
          loader_(renderer_.makeLoader()),
          num_scene_loads_(0),
          next_scene_future_(),
          next_scene_(),
          envs_per_scene_(num_environments / num_active_scenes),
          rgen_(seed),
          active_scenes_(),
          inactive_scenes_(),
          groups_(),
          worker_threads_(),
          start_barrier_(),
          finish_barrier_(),
          active_group_(),
          active_actions_(nullptr),
          sim_reset_(false),
          exit_(false)
    {
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

        uint32_t envs_per_group = num_environments / num_groups;
        uint32_t scenes_per_group = num_active_scenes / num_groups;

        for (uint32_t i = 0; i < num_groups; i++) {
            groups_.emplace_back(
                cmd_strm_, loader_, dataset_, rgen_, envs_per_scene_,
                Span(&active_scenes_[i * scenes_per_group], scenes_per_group));
        }

        pthread_barrier_init(&start_barrier_, nullptr, num_workers + 1);
        pthread_barrier_init(&finish_barrier_, nullptr, num_workers + 1);

        uint32_t envs_per_thread = envs_per_group / num_workers;
        uint32_t extra_files = envs_per_group - envs_per_thread * num_workers;

        uint32_t thread_env_offset = 0;
        for (uint32_t thread_idx = 0; thread_idx < num_workers; thread_idx++) {
            uint32_t thread_num_envs = envs_per_thread;
            if (extra_files > 0) {
                thread_num_envs++;
                extra_files--;
            }

            worker_threads_.emplace_back(
                [this, thread_env_offset, thread_num_envs, seed]() {
                    simulationWorker(thread_env_offset, thread_num_envs,
                                     seed + thread_env_offset);
                });

            thread_env_offset += thread_num_envs;
        }
    }

    void simulateAndRender(uint32_t active_group,
                           bool trigger_reset,
                           const int64_t *action_ptr)
    {
        active_group_ = active_group;
        active_actions_ = action_ptr;
        sim_reset_ = trigger_reset;

        // My guess is that the barrier wait does this
        // implicitly but it doesn't seem to be defined anywhere
        atomic_thread_fence(memory_order_release);
        pthread_barrier_wait(&start_barrier_);
        pthread_barrier_wait(&finish_barrier_);
        atomic_thread_fence(memory_order_acquire);

        groups_[active_group].render();
    }

    void simulationWorker(uint32_t first_env_idx,
                          uint32_t num_envs,
                          uint64_t seed)
    {
        uint32_t end_env_idx = first_env_idx + num_envs;
        mt19937 rgen {seed};

        while (true) {
            pthread_barrier_wait(&start_barrier_);
            if (exit_) {
                return;
            }

            const bool trigger_reset = sim_reset_;

            EnvironmentGroup &group = groups_[active_group_];
            for (uint32_t env_idx = first_env_idx; env_idx < end_env_idx;
                 env_idx++) {
                Simulator &sim = group.getSimulator(env_idx);

                if (trigger_reset) {
                    sim.reset();
                } else {
                    bool done = sim.step(active_actions_[env_idx]);

                    if (done && next_scene_ != nullptr &&
                        group.swapReady(env_idx)) {
                        group.swapScene(env_idx, next_scene_, rgen);
                        num_scene_loads_.fetch_sub(1, memory_order_relaxed);
                    }

                    if (done) {
                        sim.reset();
                    }
                }
            }

            pthread_barrier_wait(&finish_barrier_);
        }
    }

    Dataset dataset_;
    BatchRendererCUDA renderer_;
    CommandStreamCUDA cmd_strm_;
    BackgroundSceneLoader loader_;
    atomic_uint32_t num_scene_loads_;
    future<shared_ptr<Scene>> next_scene_future_;
    shared_ptr<Scene> next_scene_;
    uint32_t envs_per_scene_;

    mt19937 rgen_;
    vector<uint32_t> active_scenes_;
    vector<uint32_t> inactive_scenes_;
    vector<EnvironmentGroup> groups_;

    vector<thread> worker_threads_;
    pthread_barrier_t start_barrier_;
    pthread_barrier_t finish_barrier_;
    uint32_t active_group_;
    const int64_t *active_actions_;
    bool sim_reset_;
    bool exit_;
};

PYBIND11_MODULE(ddppo_fastrollout, m)
{
    PYBIND11_NUMPY_DTYPE(StepInfo, success, spl, distanceToGoal);
    py::class_<RolloutGenerator>(m, "RolloutGenerator")
        .def(py::init<const string &, const string &, uint32_t, uint32_t, int,
                      int, const array<uint32_t, 2> &, bool, bool, bool,
                      uint64_t>())
        .def("step", &RolloutGenerator::step)
        .def("reset", &RolloutGenerator::reset)
        .def("rgba", &RolloutGenerator::getColorMemory)
        .def("depth", &RolloutGenerator::getDepthMemory)
        .def("get_cuda_semaphore", &RolloutGenerator::getCUDASemaphore)
        .def("get_rewards", &RolloutGenerator::getRewards)
        .def("get_masks", &RolloutGenerator::getMasks)
        .def("get_infos", &RolloutGenerator::getInfos)
        .def("get_polars", &RolloutGenerator::getPolars)
        .def("can_swap_scene", &RolloutGenerator::canSwapScene)
        .def("trigger_swap_scene", &RolloutGenerator::triggerSwapScene);
}