#pragma once

#include <core/common.h>
#include <core/types.h>
#include <core/log.h>
#include <core/volume.h>
#include <core/array_types.h>
#include <core/vector_types.h>
#include <core/math_utils.h>
#include <core/string_utils.h>
#include <mol/trajectory_utils.h>

//#include <initializer_list>

struct MoleculeDynamic;
struct MoleculeStructure;
struct MoleculeTrajectory;
struct Volume;

namespace stats {

typedef uint64 ID;
constexpr ID INVALID_ID = 0;

struct Histogram {
    Array<float> bins = {};
    Range value_range = {};
    Range bin_range = {};
    int32 num_samples = 0;
};

struct Structure {
    int32 beg_idx;
    int32 end_idx;
};

enum AggregationStrategy { COM, NONE };

struct StructureData {
    DynamicArray<Structure> structures{};
    AggregationStrategy strategy = NONE;
};

struct InstanceData {
    InstanceData(int32 size) { data = allocate_array<float>(size); }
    ~InstanceData() { free_array(&data); }

    Array<float> data;
};

/*
typedef int PropertyFlags;
enum PropertyFlags_ {
    PropertyFlags_Valid             = BIT(1);
    PropertyFlags_Periodic          = BIT(2);
    PropertyFlags_ShowOverlay       = BIT(3);
    PropertyFlags_ShowTimeline      = BIT(4);
    PropertyFlags_ShowDistribution  = BIT(5);
    PropertyFlags_ShowVolume        = BIT(5);
    PropertyFlags_DataDirty         = BIT(6);
    PropertyFlags_FilterDirty       = BIT(7);
}
*/

struct Property {
    StringBuffer<32> name_buf{};
    StringBuffer<256> args_buf{};
    StringBuffer<32> unit_buf{};
    StringBuffer<256> error_msg_buf{};

    bool valid = false;
    bool periodic = false;

    bool enable_visualization = false;
    bool enable_timeline = true;
    bool enable_distribution = true;
    bool enable_volume = true;

    bool data_dirty = false;
    bool filter_dirty = false;

    Range filter{0, 0};
    Range total_data_range{0, 0};
    Range avg_data_range{0, 0};
    DynamicArray<float> filter_fraction{};
    DynamicArray<float> avg_data{};
    DynamicArray<float> std_dev_data{};
    Histogram full_histogram{};
    Histogram filt_histogram{};

    Array<InstanceData> instance_data{};
    Array<StructureData> structure_data{};
    DynamicArray<Property*> dependencies{};
};

// This is to use as base colors for overlaying graphics
struct VisualizationStyle {
    static const int32 NUM_COLORS = 4;
    uint32 point_colors[NUM_COLORS] = {0xffe3cea6, 0xffb4781f, 0xff8adfb2, 0xff2ca033};
    uint32 line_color = 0x55cccccc;
};

// Helper functions
void set_error_message(const char* fmt, ...);

void init_instance_data(Array<InstanceData>* instance_data, int32 num_instances, int32 num_frames);
void free_instance_data(Array<InstanceData>* instance_data);

void init_structure_data(Array<StructureData>* structure_data, int32 count);
bool sync_structure_data_length(Array<StructureData> data);
bool extract_args_structures(Array<StructureData> data, Array<CString> arg, const MoleculeStructure& structure);

Array<const vec3> extract_positions(Structure structure, Array<const vec3> atom_positions);

template <typename Callback>
void for_each_filtered_property_structure_in_frame(Property* prop, int32 frame_idx, Callback cb) {
    ASSERT(prop);
    for (int32 inst_idx = 0; inst_idx < prop->instance_data.count; inst_idx++) {
        const float v = prop->instance_data[inst_idx].data[frame_idx];
        if (prop->filter.x <= v && v <= prop->filter.y) {
            for (const auto& s_data : prop->structure_data) {
                for (const auto& s : s_data.structures) {
                    cb(s);
                }
            }
        }
    }
}

typedef bool (*PropertyComputeFunc)(Property* prop, const Array<CString> args, const MoleculeDynamic& dynamic);
typedef bool (*PropertyVisualizeFunc)(const Property& prop, const MoleculeDynamic& dynamic);

// HISTOGRAM
void init_histogram(Histogram* hist, int32 num_bins);
void free_histogram(Histogram* hist);

void compute_histogram(Histogram* hist, Array<const float> data);
void compute_histogram(Histogram* hist, Array<const float> data, Range filter);

void clear_histogram(Histogram* hist);
void normalize_histogram(Histogram* hist);

// STATS
void initialize();
void shutdown();

void async_update(const MoleculeDynamic& dynamic, Range frame_filter = {0, 0}, void (*on_finished)(void*) = nullptr, void* usr_data = nullptr);

// Async functionality
// void lock_thread_mutex();
// void unlock_thread_mutex();
bool thread_running();
void signal_stop();
void signal_stop_and_wait();
void signal_start();
float fraction_done();

// bool compute_stats(const MoleculeDynamic& dynamic);
void visualize(const MoleculeDynamic& dynamic);

const Volume& get_density_volume();
VisualizationStyle* get_style();

// void compute_property(Property* prop, const MoleculeDynamic& dynamic);
// void compute_property_histograms(Property* prop);
// void compute_property_histograms(Property* prop, Range frame_filter);

bool register_property_command(CString cmd_keyword, PropertyComputeFunc compute_func, PropertyVisualizeFunc visualize_func);

Array<CString> get_property_commands();
Array<CString> get_structure_commands();
Array<CString> get_property_names();

// PROPERTY
Property* create_property(CString name = {}, CString args = {});
void remove_property(Property* prop);
void remove_all_properties();
void move_property_up(Property* prop);
void move_property_down(Property* prop);

// Keep property, but remove the generated data
void clear_property(Property* prop);
void clear_all_properties();

void set_all_property_flags(bool data_dirty, bool filter_dirty);

Array<Property*> get_properties();
Property* find_property(CString name);

// DENSITY VOLUME
void compute_density_volume(Volume* vol, const mat4& world_to_volume, const MoleculeTrajectory& traj, Range frame_range);

template <typename WorldToVolumeFunc>
void compute_density_volume_with_basis(Volume* vol, const MoleculeTrajectory& traj, Range frame_range, WorldToVolumeFunc func) {
    ASSERT(vol);
    if (vol->dim.x == 0 || vol->dim.y == 0 || vol->dim.z == 0) {
        LOG_ERROR("One or more volume dimension are zero...");
        return;
    }

    auto properties = get_properties();
    if (properties.count == 0) return;
    float num_frames = (float)properties.front()->avg_data.count;

    if (frame_range.x == 0 && frame_range.y == 0) {
        frame_range.y = num_frames;
    }

    frame_range.x = math::clamp(frame_range.x, 0.f, num_frames);
    frame_range.y = math::clamp(frame_range.y, 0.f, num_frames);

    clear_volume(vol);

    for (auto prop : get_properties()) {
        if (!prop->enable_volume) continue;
        for (int32 frame_idx = (int32)frame_range.x; frame_idx < (int32)frame_range.y; frame_idx++) {
            const Array<const vec3> atom_positions = get_trajectory_positions(traj, frame_idx);
            const mat4 world_to_volume_matrix = func(frame_idx);
            for_each_filtered_property_structure_in_frame(prop, frame_idx,
                                                          [vol, &atom_positions, &world_to_volume_matrix](const Structure& s) {
                                                              for (int32 i = s.beg_idx; i < s.end_idx; i++) {
                                                                  const vec4 tc = math::fract(world_to_volume_matrix * vec4(atom_positions[i], 1));
                                                                  // if (tc.x < 0.f || 1.f < tc.x) continue;
                                                                  // if (tc.y < 0.f || 1.f < tc.y) continue;
                                                                  // if (tc.z < 0.f || 1.f < tc.z) continue;
                                                                  const ivec3 c = vec3(tc) * (vec3)vol->dim;
                                                                  const int32 voxel_idx = c.z * vol->dim.x * vol->dim.y + c.y * vol->dim.x + c.x;
                                                                  vol->voxel_data[voxel_idx]++;
                                                                  vol->voxel_range.y = math::max(vol->voxel_range.y, vol->voxel_data[voxel_idx]);
                                                              }
                                                          });
        }
    }
}

}  // namespace stats