//#define NOMINMAX

#include "molecule_draw.h"
#include <core/common.h>
#include <core/log.h>
#include <gfx/gl_utils.h>
#include <gfx/immediate_draw_utils.h>
#include <mol/molecule_utils.h>

namespace draw {

static GLuint instanced_quad_vao = 0;
static GLuint instanced_quad_ibo = 0;

static GLuint vbo = 0;
static GLsizeiptr vbo_size = MEGABYTES(4);

void draw_instanced_quads(int num_instances) {
    glBindVertexArray(instanced_quad_vao);
    glDrawElementsInstanced(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, 0, num_instances);
    glBindVertexArray(0);
}

inline void set_vbo_data(const void* data, GLsizeiptr size_in_bytes) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    if (vbo_size > size_in_bytes) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, size_in_bytes, data);
    } else {
        glBufferData(GL_ARRAY_BUFFER, size_in_bytes, data, GL_STREAM_DRAW);
        vbo_size = size_in_bytes;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

namespace vdw {
static GLuint v_shader = 0;
static GLuint g_shader = 0;
static GLuint f_shader = 0;
static GLuint program = 0;
static GLuint vao = 0;

static GLuint buf_position_radius = 0;
static GLuint buf_color = 0;

static GLint attrib_loc_pos_rad = -1;
static GLint attrib_loc_color = -1;

static GLint uniform_loc_view_mat = -1;
static GLint uniform_loc_proj_mat = -1;
static GLint uniform_loc_inv_proj_mat = -1;
static GLint uniform_loc_radius_scale = -1;

static const char* v_shader_src_old = R"(
#version 150 core

uniform mat4 u_view_mat;
uniform mat4 u_proj_mat;
uniform mat4 u_inv_proj_mat;

uniform samplerBuffer u_tex_pos_rad;
uniform samplerBuffer u_tex_color;
uniform samplerBuffer u_tex_picking;

uniform float u_radius_scale = 1.0;

out Fragment {
    flat vec4 color;
    flat vec4 view_sphere;
	flat vec4 picking_color;
    smooth vec4 view_coord;
} out_frag;

vec4 pack_u32(uint data) {
	return vec4(
        (data & uint(0x000000FF)) >> 0,
        (data & uint(0x0000FF00)) >> 8,
        (data & uint(0x00FF0000)) >> 16,
        (data & uint(0xFF000000)) >> 24) / 255.0;
}

// From Inigo Quilez!
void proj_sphere(in vec4 sphere, 
				 in float fle,
				 out vec2 axis_a,
				 out vec2 axis_b,
				 out vec2 center) {
	vec3  o = sphere.xyz;
    float r2 = sphere.w*sphere.w;
	float z2 = o.z*o.z;	
	float l2 = dot(o,o);
	
	// axis
	axis_a = fle*sqrt(-r2*(r2-l2)/((l2-z2)*(r2-z2)*(r2-z2)))*vec2( o.x,o.y);
	axis_b = fle*sqrt(-r2*(r2-l2)/((l2-z2)*(r2-z2)*(r2-l2)))*vec2(-o.y,o.x);
	center = -fle*o.z*o.xy/(z2-r2);
}

void main() {
	int VID = gl_VertexID;
	int IID = gl_InstanceID;
	vec2 uv = vec2(VID / 2, VID % 2) * 2.0 - 1.0; 

	vec4 pos_rad = texelFetch(u_tex_pos_rad, IID);
	vec4 color	 = texelFetch(u_tex_color, IID);
	vec4 picking = pack_u32(uint(IID));

	vec3 pos = pos_rad.xyz;
	float rad = pos_rad.w * u_radius_scale;
	if (color.a == 0) rad = 0;

    vec4 view_coord = u_view_mat * vec4(pos, 1.0);
    float len = length(view_coord.xyz);
    vec3 view_dir = view_coord.xyz / len;

    out_frag.color = color;
    out_frag.view_sphere = vec4(view_coord.xyz, rad);
	out_frag.picking_color = picking;

	// Focal length
	float fle = u_proj_mat[1][1];
	// 1.0 / aspect_ratio
	float inv_ar = u_proj_mat[0][0] / u_proj_mat[1][1];

	vec2 axis_a;
	vec2 axis_b;
	vec2 center;
	proj_sphere(out_frag.view_sphere, fle, axis_a, axis_b, center);

	// Bias depth with radius of sphere to make sure we only write depth values greater than this
	view_coord.z += rad;

	vec2 xy = (center + axis_a * uv.x + axis_b * uv.y) * vec2(inv_ar, 1.0);
	float z = -u_proj_mat[2][2] - u_proj_mat[3][2] / view_coord.z;
	vec4 pc = vec4(xy, z, 1);

	// Compute view_coordinate which is used for ray-sphere intersection in fragment shader
	out_frag.view_coord = u_inv_proj_mat * pc;
	out_frag.view_coord = out_frag.view_coord / out_frag.view_coord.w;

    gl_Position = pc;
}
)";

static const char* v_shader_src = R"(
#version 150 core
#extension GL_ARB_explicit_attrib_location : enable

uniform mat4 u_view_mat;
uniform mat4 u_proj_mat;
uniform mat4 u_inv_proj_mat;

uniform float u_radius_scale = 1.0;

layout (location = 0) in vec4 in_pos_rad;
layout (location = 1) in vec4 in_color;

out VS_GS {
	flat vec4 view_sphere;
    flat vec4 color;
	flat vec4 picking_color;
	flat vec2 axis_a;
	flat vec2 axis_b;
	flat vec2 center;
	flat float inv_aspect_ratio;
	flat float z;
} out_geom;

vec4 pack_u32(uint data) {
	return vec4(
        (data & uint(0x000000FF)) >> 0,
        (data & uint(0x0000FF00)) >> 8,
        (data & uint(0x00FF0000)) >> 16,
        (data & uint(0xFF000000)) >> 24) / 255.0;
}

// From Inigo Quilez!
void proj_sphere(in vec4 sphere, 
				 in float fle,
				 out vec2 axis_a,
				 out vec2 axis_b,
				 out vec2 center) {
	vec3  o = sphere.xyz;
    float r2 = sphere.w*sphere.w;
	float z2 = o.z*o.z;	
	float l2 = dot(o,o);
	
	// axis
	axis_a = fle*sqrt(-r2*(r2-l2)/((l2-z2)*(r2-z2)*(r2-z2)))*vec2( o.x,o.y);
	axis_b = fle*sqrt(-r2*(r2-l2)/((l2-z2)*(r2-z2)*(r2-l2)))*vec2(-o.y,o.x);
	center = -fle*o.z*o.xy/(z2-r2);
}

void main() {
	vec4 pos_rad = in_pos_rad;
	vec4 color = in_color;
	vec4 picking_color = pack_u32(uint(gl_VertexID));

	vec3 pos = pos_rad.xyz;
	float rad = pos_rad.w * u_radius_scale;
    vec4 view_coord = u_view_mat * vec4(pos, 1.0);
	vec4 view_sphere = vec4(view_coord.xyz, rad);

	// Focal length
	float fle = u_proj_mat[1][1];
	// 1.0 / aspect_ratio
	float inv_ar = u_proj_mat[0][0] / u_proj_mat[1][1];
	// Compute view depth (with bias of radius) 
	float z = -u_proj_mat[2][2] - u_proj_mat[3][2] / (view_coord.z + rad);

	vec2 axis_a;
	vec2 axis_b;
	vec2 center;
	proj_sphere(view_sphere, fle, axis_a, axis_b, center);

    out_geom.view_sphere = view_sphere;
    out_geom.color = color;
	out_geom.picking_color = picking_color;
    out_geom.axis_a = axis_a;
    out_geom.axis_b = axis_b;
    out_geom.center = center;
	out_geom.inv_aspect_ratio = inv_ar;
	out_geom.z = z;

    gl_Position = view_coord;
}
)";


static const char* g_shader_src = R"(
#version 150 core

uniform mat4 u_inv_proj_mat;

layout (points) in;
layout (triangle_strip, max_vertices = 4) out;

in VS_GS {
	flat vec4 view_sphere;
    flat vec4 color;
	flat vec4 picking_color;
	flat vec2 axis_a;
	flat vec2 axis_b;
	flat vec2 center;
	flat float inv_aspect_ratio;
	flat float z;
} in_vert[];

out GS_FS {
    flat vec4 color;
    flat vec4 view_sphere;
	flat vec4 picking_color;
    smooth vec4 view_coord;
} out_frag;

void emit_vertex(vec2 uv) {
	vec2 axis_a = in_vert[0].axis_a;
	vec2 axis_b = in_vert[0].axis_b;
	vec2 center = in_vert[0].center;
	float inv_aspect_ratio = in_vert[0].inv_aspect_ratio;
	float z = in_vert[0].z;

	vec2 xy = (center + axis_a * uv.x + axis_b * uv.y) * vec2(inv_aspect_ratio, 1.0);
	vec4 pc = vec4(xy, z, 1);
	vec4 vc = u_inv_proj_mat * pc;

	out_frag.view_coord = vc / vc.w;
	gl_Position = pc;
	EmitVertex();
}

void main()
{
    if (in_vert[0].color.a == 0 || in_vert[0].view_sphere.w == 0) {
        EndPrimitive();
        return;
    }

    out_frag.color = in_vert[0].color;
    out_frag.view_sphere = in_vert[0].view_sphere;
    out_frag.picking_color = in_vert[0].picking_color;

	emit_vertex(vec2(-1,-1));
	emit_vertex(vec2( 1,-1));
	emit_vertex(vec2(-1, 1));
	emit_vertex(vec2( 1, 1));

	EndPrimitive();
}
)";


static const char* f_shader_src = R"(
#version 150 core
#extension GL_ARB_conservative_depth : enable
#extension GL_ARB_explicit_attrib_location : enable

uniform mat4 u_proj_mat;
uniform float u_exposure = 1.0;

in GS_FS {
    flat vec4 color;
    flat vec4 view_sphere;
	flat vec4 picking_color;
    smooth vec4 view_coord;
} in_frag;

#ifdef GL_EXT_conservative_depth
layout (depth_greater) out float gl_FragDepth;
#endif
layout(location = 0) out vec4 out_color_alpha;
layout(location = 1) out vec4 out_f0_smoothness;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_picking_color;

// https://aras-p.info/texts/CompactNormalStorage.html
vec4 encode_normal (vec3 n) {
    float p = sqrt(n.z*8+8);
    return vec4(n.xy/p + 0.5,0,0);
}

void main() {
    vec3 center = in_frag.view_sphere.xyz;
    float radius = in_frag.view_sphere.w;
    vec3 view_dir = -normalize(in_frag.view_coord.xyz);

    vec3 m = -center;
    vec3 d = -view_dir;
    float r = radius;
    float b = dot(m, d);
    float c = dot(m, m) - r*r;
    float discr = b*b - c;
    if (discr < 0.0) discard;
    float t = -b -sqrt(discr);

    vec3 view_hit = d * t;
    vec3 view_normal = (view_hit - center) / radius;

    gl_FragDepth = (-u_proj_mat[2][2] - u_proj_mat[3][2] / view_hit.z) * 0.5 + 0.5;
    out_color_alpha = in_frag.color;
	out_f0_smoothness = vec4(0.04, 0.04, 0.04, 0.0);
	out_normal = encode_normal(view_normal);
	out_picking_color = in_frag.picking_color;
}
)";

static void initialize() {
    constexpr int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];

    v_shader = glCreateShader(GL_VERTEX_SHADER);
	g_shader = glCreateShader(GL_GEOMETRY_SHADER);
    f_shader = glCreateShader(GL_FRAGMENT_SHADER);

    glShaderSource(v_shader, 1, &v_shader_src, 0);
	glShaderSource(g_shader, 1, &g_shader_src, 0);
    glShaderSource(f_shader, 1, &f_shader_src, 0);

    glCompileShader(v_shader);
    if (gl::get_shader_compile_error(buffer, BUFFER_SIZE, v_shader)) {
        LOG_ERROR("Compiling vdw vertex shader:\n%s\n", buffer);
    }

	glCompileShader(g_shader);
	if (gl::get_shader_compile_error(buffer, BUFFER_SIZE, g_shader)) {
		LOG_ERROR("Compiling vdw geometry shader:\n%s\n", buffer);
	}

    glCompileShader(f_shader);
    if (gl::get_shader_compile_error(buffer, BUFFER_SIZE, f_shader)) {
        LOG_ERROR("Compiling vdw fragment shader:\n%s\n", buffer);
    }

    program = glCreateProgram();
    glAttachShader(program, v_shader);
	glAttachShader(program, g_shader);
    glAttachShader(program, f_shader);
    glLinkProgram(program);
    if (gl::get_program_link_error(buffer, BUFFER_SIZE, program)) {
        LOG_ERROR("Linking vdw program:\n%s\n", buffer);
    }

    glDetachShader(program, v_shader);
	glDetachShader(program, g_shader);
    glDetachShader(program, f_shader);

    glDeleteShader(v_shader);
	glDeleteShader(g_shader);
    glDeleteShader(f_shader);

	if (!vao) glGenVertexArrays(1, &vao);

    if (!buf_position_radius) glGenBuffers(1, &buf_position_radius);
    if (!buf_color) glGenBuffers(1, &buf_color);

	attrib_loc_pos_rad = glGetAttribLocation(program, "in_pos_rad");
	attrib_loc_color = glGetAttribLocation(program, "in_color");

    uniform_loc_view_mat = glGetUniformLocation(program, "u_view_mat");
    uniform_loc_proj_mat = glGetUniformLocation(program, "u_proj_mat");
    uniform_loc_inv_proj_mat = glGetUniformLocation(program, "u_inv_proj_mat");
    uniform_loc_radius_scale = glGetUniformLocation(program, "u_radius_scale");
}

static void shutdown() {
    if (program) glDeleteProgram(program);

    if (buf_position_radius) glDeleteBuffers(1, &buf_position_radius);
    if (buf_color) glDeleteBuffers(1, &buf_color);
}

}  // namespace vdw

namespace licorice {
struct Vertex {
    vec3 position;
    uint32 color;
};

static GLuint vao = 0;
static GLuint ibo = 0;

static GLuint v_shader = 0;
static GLuint g_shader = 0;
static GLuint f_shader = 0;
static GLuint program = 0;

static GLint attrib_loc_position = -1;
static GLint attrib_loc_color = -1;
static GLint uniform_loc_view_mat = -1;
static GLint uniform_loc_proj_mat = -1;
static GLint uniform_loc_radius = -1;

static const char* v_shader_src = R"(
#version 150 core

uniform mat4 u_view_mat;

in vec3	 v_position;
in vec4  v_color;

out Vertex {
    flat vec4 color;
	flat uint picking_id;
} out_vert;

void main() {
    gl_Position = u_view_mat * vec4(v_position, 1.0);
    out_vert.color = v_color;
	out_vert.picking_id = uint(gl_VertexID);
}
)";

static const char* g_shader_src = R"(
#version 150 core

uniform mat4 u_proj_mat;
uniform float u_radius = 1.0;

layout (lines) in;
layout (triangle_strip, max_vertices = 24) out;

in Vertex {
    flat vec4 color;
	flat uint picking_id;
} in_vert[];

out Fragment {
    flat vec4 color[2];
	flat vec4 picking_color[2];
    smooth vec3 view_pos;

	flat vec4  capsule_center_radius;
	flat vec4  capsule_axis_length;
} out_frag;

vec4 pack_u32(uint data) {
	return vec4(
        (data & uint(0x000000FF)) >> 0,
        (data & uint(0x0000FF00)) >> 8,
        (data & uint(0x00FF0000)) >> 16,
        (data & uint(0xFF000000)) >> 24) / 255.0;
}

vec4 prismoid[8];

void emit_vertex(int a){
    out_frag.view_pos = prismoid[a].xyz;
	gl_Position = u_proj_mat * prismoid[a];
    EmitVertex();
}

void emit(int a, int b, int c, int d)
{
    emit_vertex(a);
    emit_vertex(b);
    emit_vertex(c);
    emit_vertex(d);
    EndPrimitive(); 
}

vec3 get_ortho_vec(vec3 v, vec3 A, vec3 B){
    if(abs(1-dot(v,A))>0.001){
        return normalize(cross(v,A));
    }else{
        return normalize(cross(v,B));
    }
}

void main()
{
    if (in_vert[0].color.a == 0 || in_vert[1].color.a == 0) {
        EndPrimitive();
        return;
    }

    // Compute orientation vectors for the two connecting faces:
    vec3 p0 = gl_in[0].gl_Position.xyz;
    vec3 p1 = gl_in[1].gl_Position.xyz;
	float r = u_radius;
	float l = distance(p0, p1);
	vec3 a = (p1 - p0) / l;
	vec3 c = (p0 + p1) * 0.5;

	out_frag.color[0] = in_vert[0].color;
	out_frag.color[1] = in_vert[1].color;

    out_frag.picking_color[0] = pack_u32(in_vert[0].picking_id);
    out_frag.picking_color[1] = pack_u32(in_vert[1].picking_id);

	out_frag.capsule_center_radius = vec4(c, r);
	out_frag.capsule_axis_length = vec4(a, l);

    // Extend end points to properly fit the sphere caps
    p0 -= a * r;
    p1 += a * r;

	vec3 B = vec3(0,0,1);
	vec3 A = vec3(1,0,0);
    vec3 o = get_ortho_vec(a,A,B);

    // Declare scratch variables for basis vectors:
    vec3 i,j,k;

    // Compute vertices of prismoid:
    j = a; i = o; k = normalize(cross(i, j)); i = normalize(cross(k, j)); ; i *= r; k *= r;
    prismoid[0] = vec4(p0 + i + k, 1);
    prismoid[1] = vec4(p0 + i - k, 1);
    prismoid[2] = vec4(p0 - i - k, 1);
    prismoid[3] = vec4(p0 - i + k, 1);
    prismoid[4] = vec4(p1 + i + k, 1);
    prismoid[5] = vec4(p1 + i - k, 1);
    prismoid[6] = vec4(p1 - i - k, 1);
    prismoid[7] = vec4(p1 - i + k, 1);

    // Emit the six faces of the prismoid:
    emit(0,1,3,2); emit(5,4,6,7);
    emit(4,5,0,1); emit(3,2,7,6);
    emit(0,3,4,7); emit(2,1,6,5);
}
)";

static const char* f_shader_src = R"(
#version 150 core
#extension GL_ARB_conservative_depth : enable
#extension GL_ARB_explicit_attrib_location : enable

uniform mat4 u_proj_mat;
uniform float u_exposure = 1.0;
uniform float u_radius = 1.0;

in Fragment {
    flat vec4 color[2];
	flat vec4 picking_color[2];
    smooth vec3 view_pos;

	flat vec4  capsule_center_radius;
	flat vec4  capsule_axis_length;
} in_frag;

#ifdef GL_EXT_conservative_depth
layout (depth_greater) out float gl_FragDepth;
#endif
layout(location = 0) out vec4 out_color_alpha;
layout(location = 1) out vec4 out_f0_smoothness;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_picking_id;

// Source from Ingo Quilez (https://www.shadertoy.com/view/Xt3SzX)
float intersect_capsule(in vec3 ro, in vec3 rd, in vec3 cc, in vec3 ca, float cr,
                      float ch, out vec3 normal, out int side)  // cc center, ca orientation axis, cr radius, ch height
{
    vec3 oc = ro - cc;
    ch *= 0.5;

    float card = dot(ca, rd);
    float caoc = dot(ca, oc);

    float a = 1.0 - card * card;
    float b = dot(oc, rd) - caoc * card;
    float c = dot(oc, oc) - caoc * caoc - cr * cr;
    float h = b * b - a * c;
    if (h < 0.0) return -1.0;
    float t = (-b - sqrt(h)) / a;

    float y = caoc + t * card;
    side = int(y > 0);

    // body
    if (abs(y) < ch) {
        normal = normalize(oc + t * rd - ca * y);
        return t;
    }

    // caps
    float sy = sign(y);
    oc = ro - (cc + sy * ca * ch);
    b = dot(rd, oc);
    c = dot(oc, oc) - cr * cr;
    h = b * b - c;
    if (h > 0.0) {
        t = -b - sqrt(h);
        normal = normalize(oc + rd * t);
        return t;
    }

    return -1.0;
}

vec4 encode_normal (vec3 n) {
    float p = sqrt(n.z*8+8);
    return vec4(n.xy/p + 0.5,0,0);
}

void main() {
    vec3 ro = vec3(0);
    vec3 rd = normalize(in_frag.view_pos);
	vec3 cc = in_frag.capsule_center_radius.xyz;
	float cr = in_frag.capsule_center_radius.w;
	vec3 ca = in_frag.capsule_axis_length.xyz;
    float ch = in_frag.capsule_axis_length.w;

    vec3 normal;
    int side;
    float t = intersect_capsule(ro, rd, cc, ca, cr, ch, normal, side);
    if (t < 0.0) {
        discard;
        return;
    }

    vec3 pos = ro + t*rd;
    vec4 color = in_frag.color[side];
	vec4 picking_color = in_frag.picking_color[side];

    vec4 coord = vec4(0, 0, pos.z, 1);
    coord = u_proj_mat * coord;
    coord = coord / coord.w;

    gl_FragDepth = coord.z * 0.5 + 0.5;
    out_color_alpha = color;
	out_f0_smoothness = vec4(0.04, 0.04, 0.04, 0.0);
	out_normal = encode_normal(normal);
	out_picking_id = picking_color;
}
)";

static void initialize() {
    constexpr int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];

    v_shader = glCreateShader(GL_VERTEX_SHADER);
    g_shader = glCreateShader(GL_GEOMETRY_SHADER);
    f_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v_shader, 1, &v_shader_src, 0);
    glShaderSource(g_shader, 1, &g_shader_src, 0);
    glShaderSource(f_shader, 1, &f_shader_src, 0);

    glCompileShader(v_shader);
    if (gl::get_shader_compile_error(buffer, BUFFER_SIZE, v_shader)) {
        LOG_ERROR("Compiling licorice vertex shader:\n%s\n", buffer);
    }
    glCompileShader(g_shader);
    if (gl::get_shader_compile_error(buffer, BUFFER_SIZE, g_shader)) {
        LOG_ERROR("Compiling licorice geometry shader:\n%s\n", buffer);
    }
    glCompileShader(f_shader);
    if (gl::get_shader_compile_error(buffer, BUFFER_SIZE, f_shader)) {
        LOG_ERROR("Compiling licorice fragment shader:\n%s\n", buffer);
    }

    program = glCreateProgram();
    glAttachShader(program, v_shader);
    glAttachShader(program, g_shader);
    glAttachShader(program, f_shader);
    glLinkProgram(program);
    if (gl::get_program_link_error(buffer, BUFFER_SIZE, program)) {
        LOG_ERROR("Linking licorice program:\n%s\n", buffer);
    }

    glDetachShader(program, v_shader);
    glDetachShader(program, g_shader);
    glDetachShader(program, f_shader);

    glDeleteShader(v_shader);
    glDeleteShader(g_shader);
    glDeleteShader(f_shader);

    attrib_loc_position = glGetAttribLocation(program, "v_position");
    attrib_loc_color = glGetAttribLocation(program, "v_color");
    uniform_loc_view_mat = glGetUniformLocation(program, "u_view_mat");
    uniform_loc_proj_mat = glGetUniformLocation(program, "u_proj_mat");
    uniform_loc_radius = glGetUniformLocation(program, "u_radius");

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glEnableVertexAttribArray(attrib_loc_position);
    glVertexAttribPointer(attrib_loc_position, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const GLvoid*)0);

    glEnableVertexAttribArray(attrib_loc_color);
    glVertexAttribPointer(attrib_loc_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (const GLvoid*)12);

    glBindVertexArray(0);

    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, MEGABYTES(4), nullptr, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void shutdown() {
    if (vao) glDeleteVertexArrays(1, &vao);
    if (program) glDeleteProgram(program);
    if (ibo) glDeleteBuffers(1, &ibo);
}

}  // namespace licorice

namespace ribbons {

struct Vertex {
    vec3 position;
    vec3 tangent;
    vec3 normal;
    unsigned int color = 0xffffffff;
    unsigned int picking_id = 0xffffffff;
};

static GLuint vao = 0;
static GLuint program = 0;

static GLint attrib_loc_position = -1;
static GLint attrib_loc_tangent = -1;
static GLint attrib_loc_normal = -1;
static GLint attrib_loc_color = -1;
static GLint attrib_loc_picking = -1;

static GLint uniform_loc_view_mat = -1;
static GLint uniform_loc_proj_mat = -1;
static GLint uniform_loc_scale_x = -1;
static GLint uniform_loc_scale_y = -1;

static const char* v_shader_src = R"(
#version 150 core
in vec3 v_position;
in vec3 v_tangent;
in vec3 v_normal;
in vec4 v_color;
in uint v_picking_id;

out Vertex {
    vec3 tangent;
    vec3 normal;
    vec4 color;
    vec4 picking_color;
} out_vert;

vec4 pack_u32(uint data) {
    return vec4(
        (data & uint(0x000000FF)) >> 0,
        (data & uint(0x0000FF00)) >> 8,
        (data & uint(0x00FF0000)) >> 16,
        (data & uint(0xFF000000)) >> 24) / 255.0;
}
 
void main() {
    out_vert.tangent = v_tangent;
    out_vert.normal = v_normal;
    out_vert.color = v_color;
    out_vert.picking_color = pack_u32(v_picking_id);
    gl_Position = vec4(v_position, 1);
} 
)";

static const char* g_shader_src = R"(
#version 150 core

#define CIRCLE_RES 8

layout(lines) in;
layout(triangle_strip, max_vertices = 56) out;

uniform mat4 u_view_mat;
uniform mat4 u_proj_mat;
uniform float u_scale_x = 1.0;
uniform float u_scale_y = 0.1;

in Vertex {
    vec3 tangent;
    vec3 normal;
    vec4 color;
    vec4 picking_color;
} in_vert[];

out Fragment {
    smooth vec4 color;
    smooth vec3 view_normal;
    flat vec4 picking_color;
} out_frag;

void emit(mat4 mat, int input_idx, vec4 v, vec3 n) {
    vec4 view_coord = u_view_mat * mat * v;
    mat3 norm_mat = inverse(transpose(mat3(u_view_mat) * mat3(mat)));
    out_frag.view_normal = normalize(norm_mat * n);
    out_frag.color = in_vert[input_idx].color;
    out_frag.picking_color = in_vert[input_idx].picking_color;
    gl_Position = u_proj_mat * view_coord;
    EmitVertex();
}

mat4 compute_mat(vec3 tangent, vec3 normal, vec3 translation) {
    vec3 binormal = normalize(cross(tangent, normal));
    return mat4(vec4(binormal, 0), vec4(normal, 0), vec4(tangent, 0), vec4(translation, 1));
}

void main() {
    if (in_vert[0].color.a == 0) {
        EndPrimitive();
        return;
    }

    vec3 pos[2];
    pos[0] = gl_in[0].gl_Position.xyz;
    pos[1] = gl_in[1].gl_Position.xyz;

    vec3 t[2];
    t[0] = normalize(in_vert[0].tangent);
    t[1] = normalize(in_vert[1].tangent);

    vec3 n[2];
    n[0] = normalize(in_vert[0].normal);
    n[1] = normalize(in_vert[1].normal);

    mat4 mat[2];
    mat[0] = compute_mat(t[0], n[0], pos[0]);
    mat[1] = compute_mat(t[1], n[1], pos[1]);

	// BOTTOM
	emit(mat[0], 0, vec4( 1 * u_scale_x, -1 * u_scale_y, 0, 1), vec3(0,-1,0));
	emit(mat[0], 0, vec4(-1 * u_scale_x, -1 * u_scale_y, 0, 1), vec3(0,-1,0));
	emit(mat[1], 1, vec4( 1 * u_scale_x, -1 * u_scale_y, 0, 1), vec3(0,-1,0));
	emit(mat[1], 1, vec4(-1 * u_scale_x, -1 * u_scale_y, 0, 1), vec3(0,-1,0));
	EndPrimitive();

	// TOP
	emit(mat[0], 0, vec4(-1 * u_scale_x, 1 * u_scale_y, 0, 1), vec3(0,1,0));
	emit(mat[0], 0, vec4( 1 * u_scale_x, 1 * u_scale_y, 0, 1), vec3(0,1,0));
	emit(mat[1], 1, vec4(-1 * u_scale_x, 1 * u_scale_y, 0, 1), vec3(0,1,0));
	emit(mat[1], 1, vec4( 1 * u_scale_x, 1 * u_scale_y, 0, 1), vec3(0,1,0));
	EndPrimitive();

	// LEFT
	emit(mat[0], 0, vec4(-1 * u_scale_x, -1 * u_scale_y, 0, 1), vec3(-1,0,0));
	emit(mat[0], 0, vec4(-1 * u_scale_x,  1 * u_scale_y, 0, 1), vec3(-1,0,0));
	emit(mat[1], 1, vec4(-1 * u_scale_x, -1 * u_scale_y, 0, 1), vec3(-1,0,0));
	emit(mat[1], 1, vec4(-1 * u_scale_x,  1 * u_scale_y, 0, 1), vec3(-1,0,0));
	EndPrimitive();

	// RIGHT
	emit(mat[0], 0, vec4(1 * u_scale_x,  1 * u_scale_y, 0, 1), vec3(1,0,0));
	emit(mat[0], 0, vec4(1 * u_scale_x, -1 * u_scale_y, 0, 1), vec3(1,0,0));
	emit(mat[1], 1, vec4(1 * u_scale_x,  1 * u_scale_y, 0, 1), vec3(1,0,0));
	emit(mat[1], 1, vec4(1 * u_scale_x, -1 * u_scale_y, 0, 1), vec3(1,0,0));
    EndPrimitive();
}
)";

static const char* f_shader_src = R"(
#version 150 core
#extension GL_ARB_explicit_attrib_location : enable

in Fragment {
    smooth vec4 color;
    smooth vec3 view_normal;
	flat vec4 picking_color;
} in_frag;

layout(location = 0) out vec4 out_color_alpha;
layout(location = 1) out vec4 out_f0_smoothness;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_picking_id;

vec4 encode_normal (vec3 n) {
    float p = sqrt(n.z*8+8);
    return vec4(n.xy/p + 0.5,0,0);
}

void main() {
    out_color_alpha = in_frag.color;
	out_f0_smoothness = vec4(0.04, 0.04, 0.04, 0.0);
	out_normal = encode_normal(in_frag.view_normal);
    out_picking_id = in_frag.picking_color;
}
)";

void intitialize() {
    constexpr int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];

    GLuint v_shader = glCreateShader(GL_VERTEX_SHADER);
    GLuint g_shader = glCreateShader(GL_GEOMETRY_SHADER);
    GLuint f_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v_shader, 1, &v_shader_src, 0);
    glShaderSource(g_shader, 1, &g_shader_src, 0);
    glShaderSource(f_shader, 1, &f_shader_src, 0);

    glCompileShader(v_shader);
    if (gl::get_shader_compile_error(buffer, BUFFER_SIZE, v_shader)) {
        LOG_ERROR("Compiling ribbons vertex shader:\n%s\n", buffer);
    }
    glCompileShader(g_shader);
    if (gl::get_shader_compile_error(buffer, BUFFER_SIZE, g_shader)) {
        LOG_ERROR("Compiling ribbons geometry shader:\n%s\n", buffer);
    }
    glCompileShader(f_shader);
    if (gl::get_shader_compile_error(buffer, BUFFER_SIZE, f_shader)) {
        LOG_ERROR("Compiling ribbons fragment shader:\n%s\n", buffer);
    }

    program = glCreateProgram();
    glAttachShader(program, v_shader);
    glAttachShader(program, g_shader);
    glAttachShader(program, f_shader);
    glLinkProgram(program);
    if (gl::get_program_link_error(buffer, BUFFER_SIZE, program)) {
        LOG_ERROR("Linking ribbons program:\n%s\n", buffer);
    }

    glDetachShader(program, v_shader);
    glDetachShader(program, g_shader);
    glDetachShader(program, f_shader);

    glDeleteShader(v_shader);
    glDeleteShader(g_shader);
    glDeleteShader(f_shader);

    attrib_loc_position = glGetAttribLocation(program, "v_position");
    attrib_loc_tangent = glGetAttribLocation(program, "v_tangent");
    attrib_loc_normal = glGetAttribLocation(program, "v_normal");
    attrib_loc_color = glGetAttribLocation(program, "v_color");
    attrib_loc_picking = glGetAttribLocation(program, "v_picking_id");

    uniform_loc_view_mat = glGetUniformLocation(program, "u_view_mat");
    uniform_loc_proj_mat = glGetUniformLocation(program, "u_proj_mat");
    uniform_loc_scale_x = glGetUniformLocation(program, "u_scale_x");
    uniform_loc_scale_y = glGetUniformLocation(program, "u_scale_y");

    if (!vao) {
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);

        glEnableVertexAttribArray(attrib_loc_position);
        glVertexAttribPointer(attrib_loc_position, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const GLvoid*)0);

        glEnableVertexAttribArray(attrib_loc_tangent);
        glVertexAttribPointer(attrib_loc_tangent, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const GLvoid*)12);

        glEnableVertexAttribArray(attrib_loc_normal);
        glVertexAttribPointer(attrib_loc_normal, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const GLvoid*)24);

        glEnableVertexAttribArray(attrib_loc_color);
        glVertexAttribPointer(attrib_loc_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (const GLvoid*)36);

        glEnableVertexAttribArray(attrib_loc_picking);
        glVertexAttribIPointer(attrib_loc_picking, 1, GL_UNSIGNED_INT, sizeof(Vertex), (const GLvoid*)40);

        glBindVertexArray(0);
    }
}

void shutdown() {
    if (vao) glDeleteVertexArrays(1, &vao);
}

}  // namespace ribbons

void initialize() {
    if (!instanced_quad_ibo) {
        const unsigned char data[4] = {0, 1, 2, 3};
        glGenBuffers(1, &instanced_quad_ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, instanced_quad_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, 4, data, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    if (!instanced_quad_vao) {
        glGenVertexArrays(1, &instanced_quad_vao);
        glBindVertexArray(instanced_quad_vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, instanced_quad_ibo);
        glBindVertexArray(0);
    }

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vbo_size, nullptr, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    vdw::initialize();
    licorice::initialize();
    ribbons::intitialize();
}

void shutdown() {
    if (instanced_quad_vao) glDeleteVertexArrays(1, &instanced_quad_vao);
    if (instanced_quad_ibo) glDeleteBuffers(1, &instanced_quad_ibo);
    if (vbo) glDeleteBuffers(1, &vbo);

    vdw::shutdown();
    licorice::shutdown();
    ribbons::shutdown();
}

void draw_vdw(GLuint atom_position_radius_buffer, GLuint atom_color_buffer, int32 atom_count, const mat4& view_mat, const mat4& proj_mat,
              float radius_scale) {
    mat4 inv_proj_mat = math::inverse(proj_mat);

	glBindVertexArray(vdw::vao);
	glBindBuffer(GL_ARRAY_BUFFER, atom_position_radius_buffer);
	glVertexAttribPointer(vdw::attrib_loc_pos_rad, 4, GL_FLOAT, GL_FALSE, sizeof(vec4), (const GLvoid*)0);
	glEnableVertexAttribArray(vdw::attrib_loc_pos_rad);
	glBindBuffer(GL_ARRAY_BUFFER, atom_color_buffer);
	glVertexAttribPointer(vdw::attrib_loc_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(uint32), (const GLvoid*)0);
	glEnableVertexAttribArray(vdw::attrib_loc_color);

    glUseProgram(vdw::program);

    // Uniforms
    glUniform1f(vdw::uniform_loc_radius_scale, radius_scale);
    glUniformMatrix4fv(vdw::uniform_loc_view_mat, 1, GL_FALSE, &view_mat[0][0]);
    glUniformMatrix4fv(vdw::uniform_loc_proj_mat, 1, GL_FALSE, &proj_mat[0][0]);
    glUniformMatrix4fv(vdw::uniform_loc_inv_proj_mat, 1, GL_FALSE, &inv_proj_mat[0][0]);
	
	glDrawArrays(GL_POINTS, 0, atom_count);

	glBindVertexArray(0);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void draw_licorice(GLuint atom_position_buffer, GLuint atom_color_buffer, GLuint bond_buffer, int32 bond_count, const mat4& view_mat, const mat4& proj_mat, float radius_scale) {
    glBindVertexArray(licorice::vao);
    glBindBuffer(GL_ARRAY_BUFFER, atom_position_buffer);
    glEnableVertexAttribArray(licorice::attrib_loc_position);
    glVertexAttribPointer(licorice::attrib_loc_position, 3, GL_FLOAT, GL_FALSE, sizeof(vec4), (const GLvoid*)0);
    glBindBuffer(GL_ARRAY_BUFFER, atom_color_buffer);
    glEnableVertexAttribArray(licorice::attrib_loc_color);
    glVertexAttribPointer(licorice::attrib_loc_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(unsigned int), (const GLvoid*)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bond_buffer);

    glUseProgram(licorice::program);
    glUniformMatrix4fv(licorice::uniform_loc_view_mat, 1, GL_FALSE, &view_mat[0][0]);
    glUniformMatrix4fv(licorice::uniform_loc_proj_mat, 1, GL_FALSE, &proj_mat[0][0]);
    glUniform1f(licorice::uniform_loc_radius, 0.25f * radius_scale);
    
    glDrawElements(GL_LINES, bond_count * 2, GL_UNSIGNED_INT, (const void*)0);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void draw_licorice(Array<const vec3> atom_positions, Array<const Bond> atom_bonds, Array<const uint32> atom_colors, const mat4& view_mat,
                   const mat4& proj_mat, float radii_scale) {
    ASSERT(atom_positions.count == atom_colors.count);

    const auto num_bytes = atom_positions.count * sizeof(licorice::Vertex);
    licorice::Vertex* data = (licorice::Vertex*)TMP_MALLOC(num_bytes);

    for (int64_t i = 0; i < atom_positions.count; i++) {
        data[i].position = atom_positions[i];
        data[i].color = atom_colors[i];
    }

    draw::set_vbo_data(data, atom_positions.count * sizeof(licorice::Vertex));
    TMP_FREE(data);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, licorice::ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, atom_bonds.count * sizeof(Bond), atom_bonds.data, GL_STREAM_DRAW);

    //glEnable(GL_DEPTH_TEST);

    glBindVertexArray(licorice::vao);
    glUseProgram(licorice::program);
    glUniformMatrix4fv(licorice::uniform_loc_view_mat, 1, GL_FALSE, &view_mat[0][0]);
    glUniformMatrix4fv(licorice::uniform_loc_proj_mat, 1, GL_FALSE, &proj_mat[0][0]);
    glUniform1f(licorice::uniform_loc_radius, 0.25f * radii_scale);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, licorice::ibo);
    glDrawElements(GL_LINES, (GLsizei)atom_bonds.count * 2, GL_UNSIGNED_INT, (const void*)0);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    //glDisable(GL_DEPTH_TEST);
}

void draw_ribbons(Array<const BackboneSegment> backbone_segments, Array<const Chain> chains, Array<const vec3> atom_positions,
                  Array<const uint32> atom_colors, const mat4& view_mat, const mat4& proj_mat, int num_subdivisions, float tension, float width_scale,
                  float thickness_scale) {
    if (backbone_segments.count == 0) return;
    if (chains.count == 0) return;
    if (atom_positions.count == 0) return;
    if (atom_colors.count == 0) return;

    struct DrawInfo {
        int32 offset;
        int32 count;
    };

    DynamicArray<DrawInfo> draw_data;
    DynamicArray<BackboneSegment> visible_segments;
    DynamicArray<SplineSegment> spline_segments;
    DynamicArray<ribbons::Vertex> vertices;

    int32 offset = 0;
    for (const auto& c : chains) {
        visible_segments.clear();
        for (int i = c.beg_res_idx; i < c.end_res_idx; i++) {
            int ca_idx = backbone_segments[i].ca_idx;
            if (ca_idx == -1 || (atom_colors[ca_idx] & 0xff000000) == 0)
                break;
            else {
                visible_segments.push_back(backbone_segments[i]);
            }
        }
        // Only do this if all segments within a chain was visible
        if (visible_segments.size() == (c.end_res_idx - c.beg_res_idx)) {
            auto splines = compute_spline(atom_positions, atom_colors, visible_segments, num_subdivisions, tension);
            // spline_segments.append(splines);
            for (const auto& s : splines) {
                vertices.push_back({s.position, s.tangent, s.normal, s.color, s.index});
            }
            draw_data.push_back({offset, (int32)splines.count});
            offset += (int32)splines.count;
        }
    }

    set_vbo_data(vertices.data, vertices.size_in_bytes());

    // glEnable(GL_DEPTH_TEST);

    glBindVertexArray(ribbons::vao);
    glUseProgram(ribbons::program);
    glUniformMatrix4fv(ribbons::uniform_loc_view_mat, 1, GL_FALSE, &view_mat[0][0]);
    glUniformMatrix4fv(ribbons::uniform_loc_proj_mat, 1, GL_FALSE, &proj_mat[0][0]);
    glUniform1f(ribbons::uniform_loc_scale_x, 0.5f * width_scale);
    glUniform1f(ribbons::uniform_loc_scale_y, 0.1f * thickness_scale);
    for (const auto& di : draw_data) {
        glDrawArrays(GL_LINE_STRIP, di.offset, di.count);
    }
    glUseProgram(0);
    glBindVertexArray(0);

    // glDisable(GL_DEPTH_TEST);
}

void draw_backbone(Array<const BackboneSegment> backbone_segments, Array<const vec3> atom_positions, const mat4& view_mat, const mat4& proj_mat) {
    immediate::set_view_matrix(view_mat);
    immediate::set_proj_matrix(proj_mat);

    immediate::set_material(immediate::MATERIAL_ROUGH_BLACK);
    for (const auto& seg : backbone_segments) {
        if (seg.ca_idx > -1) immediate::draw_point(atom_positions[seg.ca_idx]);
        if (seg.c_idx > -1) immediate::draw_point(atom_positions[seg.c_idx]);
        if (seg.o_idx > -1) immediate::draw_point(atom_positions[seg.o_idx]);
    }

    immediate::set_material(immediate::MATERIAL_ROUGH_WHITE);
    for (int i = 1; i < backbone_segments.count; i++) {
        if (backbone_segments[i].ca_idx > -1 && backbone_segments[i - 1].ca_idx > -1)
            immediate::draw_line(atom_positions[backbone_segments[i - 1].ca_idx], atom_positions[backbone_segments[i].ca_idx]);
    }

    immediate::flush();
}

void draw_spline(Array<const SplineSegment> spline, const mat4& view_mat, const mat4& proj_mat) {
    immediate::set_view_matrix(view_mat);
    immediate::set_proj_matrix(proj_mat);

    for (const auto& seg : spline) {
        mat4 basis = mat4(vec4(seg.position + seg.binormal, 0), vec4(seg.position + seg.normal, 0), vec4(seg.position + seg.tangent, 0),
                          vec4(seg.position, 1));
        immediate::draw_basis(basis);
    }

    immediate::set_material(immediate::MATERIAL_ROUGH_WHITE);
    for (int64 i = 1; i < spline.count; i++) {
        immediate::draw_line(spline[i - 1].position, spline[i].position);
    }

    immediate::flush();
}

}  // namespace draw