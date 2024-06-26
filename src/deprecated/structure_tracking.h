#pragma once

#include <core/types.h>
#include <core/bitfield.h>
#include <core/vector_types.h>

struct MoleculeDynamic;

namespace structure_tracking {
typedef u32 ID;

void initialize();
void shutdown();

ID create_structure();
bool remove_structure(ID structure_id);
void clear_structures();

struct TrackingData {
    i64 count = 0;
    struct {
        vec3* com = nullptr;
        struct {
            quat* absolute = nullptr;
            quat* relative = nullptr;
            quat* hybrid = nullptr;
        } rotation;
    } transform;
    struct {
        mat3* vectors = nullptr;
        vec3* values = nullptr;
    } eigen;

    struct {
        int atom_count = 0;
        float* x = nullptr;
        float* y = nullptr;
        float* z = nullptr;
    } average_structure;

    mat3 simulation_box_aligned_pca = {};
    mat3 pca = {};
};

/*
vec3 compute_eigen_values(const float* RESTRICT x, const float* RESTRICT y, const float* RESTRICT z, const float* RESTRICT mass, int64 count);
*/
mat3 compute_rotation(const float* x0, const float* y0, const float* z0, const float* x1, const float* y1, const float* z1,
                      const float* mass, i64 count, const vec3& com0, const vec3& com1);

bool compute_trajectory_transform_data(ID structure_id, const MoleculeDynamic& dynamic, Bitfield atom_mask, i64 mask_offset = 0);

void transform_to_internal_frame(MoleculeDynamic& dynamic, Bitfield atom_mask, i64 mask_offset);

const TrackingData* get_tracking_data(ID structure_id);

/*
const Array<const vec3> get_com(ID structure_id);
const Array<const quat> get_rot_absolute(ID structure_id);
const Array<const quat> get_rot_relative(ID structure_id);
const Array<const quat> get_rot_hybrid(ID structure_id);
*/
/*
const Array<const mat3> get_eigen_vectors(ID structure_id);
const Array<const vec3> get_eigen_values(ID structure_id);
*/

// const Array<const SupportFrame> get_support_frames(ID structure_id);

}  // namespace structure_tracking
