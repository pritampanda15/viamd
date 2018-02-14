#include <imgui.h>
#include <core/platform.h>
#include <core/gl.h>
#include <core/types.h>
#include <core/math_utils.h>
#include <core/camera.h>
#include <core/camera_utils.h>
#include <mol/molecule.h>
#include <mol/trajectory.h>
#include <mol/trajectory_utils.h>
#include <mol/molecule_utils.h>
#include <mol/pdb_utils.h>
#include <mol/gro_utils.h>
#include <gfx/immediate_draw_utils.h>
#include <gfx/postprocessing_utils.h>

#include <stdio.h>

struct MainFramebuffer {
	GLuint id = 0;
	GLuint tex_depth = 0;
	GLuint tex_color = 0;
	GLuint tex_picking = 0;
	int width = 0;
	int height = 0;
};

struct ApplicationData {
    platform::Window* main_window;

    Camera camera;
    TrackballController controller;

    // Perhaps move these into a struct
    MoleculeStructure* mol_struct;
    DynamicArray<float> atom_radii;
    DynamicArray<uint32> atom_colors;
    Trajectory* trajectory;

	// Framebuffer
	MainFramebuffer fbo;

    bool use_ssao = true;
};

void draw_main_menu(ApplicationData* data);
void init_main_framebuffer(MainFramebuffer* fbo, int width, int height);
void destroy_main_framebuffer(MainFramebuffer* fbo);

void reset_view(ApplicationData* data);

int main(int, char**) {
	ApplicationData data;

    auto gro_res = load_gro_from_file(PROJECT_SOURCE_DIR "/data/shaoqi/md-nowater.gro");
    data.mol_struct = &gro_res.gro;
	Trajectory traj = read_and_allocate_trajectory(PROJECT_SOURCE_DIR "/data/shaoqi/md-centered.xtc");

    data.camera.position = vec3(0, 0, 100);
	int display_w = 1920;
	int display_h = 1080;

    platform::initialize();
    data.main_window = platform::create_window(display_w, display_h, "VIAMD");

    platform::set_vsync(false);

    //auto pdb_res = load_pdb_from_file(PROJECT_SOURCE_DIR "/data/5ulj.pdb");
    //data.mol_struct = &pdb_res.pdb;
    data.atom_radii = molecule::compute_atom_radii(data.mol_struct->atom_elements);
    data.atom_colors = molecule::compute_atom_colors(*data.mol_struct, ColorMapping::CPK);

    immediate::initialize();
    postprocessing::initialize(display_w, display_h);
    molecule::draw::initialize();
	init_main_framebuffer(&data.fbo, display_w, display_h);

    // Setup style
    ImGui::StyleColorsClassic();

    bool show_demo_window = false;
    vec4 clear_color = vec4(0.2, 0.2, 0.4, 1);

    // Main loop
    while (!(platform::window_should_close(data.main_window))) {
        platform::update();
        platform::InputState* input = platform::get_input_state();
		platform::get_framebuffer_size(data.main_window, &display_w, &display_h);
		float dt = (float)platform::get_delta_time();

		if (data.fbo.width != display_w || data.fbo.height != display_h) {
			init_main_framebuffer(&data.fbo, display_w, display_h);
			postprocessing::initialize(display_w, display_h);
		}

		ImGui::Begin("Input");
		ImGui::Text("MouseVel: %g, %g", input->mouse_velocity.x, input->mouse_velocity.y);
		ImGui::Text("Camera Pos: %g, %g, %g", data.camera.position.x, data.camera.position.y, data.camera.position.z);
		ImGui::Text("MOUSE_BUTTONS [%i, %i, %i, %i, %i]", input->mouse_down[0], input->mouse_down[1], input->mouse_down[2], input->mouse_down[3], input->mouse_down[4]);
        if (ImGui::Button("Reset View")) {
            reset_view(&data);
        }
		ImGui::End();

		if (!ImGui::GetIO().WantCaptureMouse) {
            data.controller.input.rotate_button = input->mouse_down[0];
            data.controller.input.pan_button = input->mouse_down[1];
            data.controller.input.dolly_button = input->mouse_down[2];
            data.controller.input.prev_mouse_ndc = input->prev_mouse_ndc_coords;
            data.controller.input.curr_mouse_ndc = input->mouse_ndc_coords;
            data.controller.input.dolly_delta = input->mouse_scroll.y;
			data.controller.update();
			data.camera.position = data.controller.position;
			data.camera.orientation = data.controller.orientation;
		}

        // MAIN MENU BAR
        draw_main_menu(&data);

        // 3. Show the ImGui demo window. Most of the sample code is in ImGui::ShowDemoWindow().
        if (show_demo_window) {
            ImGui::SetNextWindowPos(ImVec2(650, 20),
                                    ImGuiCond_FirstUseEver);  // Normally user code doesn't need/want to call this because positions are saved in .ini
                                                              // file anyway. Here we just want to make the demo initial state a bit more friendly!
            ImGui::ShowDemoWindow(&show_demo_window);
        }

		mat4 model_mat = mat4(1);
		mat4 view_mat = compute_world_to_view_matrix(data.camera);
		mat4 proj_mat = compute_perspective_projection_matrix(data.camera, display_w, display_h);

        // Rendering
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, data.fbo.id);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);

        molecule::draw::draw_vdw(data.mol_struct->atom_positions, data.atom_radii, data.atom_colors, model_mat, view_mat, proj_mat);
		
        if (data.use_ssao) {
            postprocessing::apply_ssao(data.fbo.tex_depth, proj_mat, 1.5f, 3.f);
        }

        // Activate backbuffer
        glDisable(GL_DEPTH_TEST);
		glDepthFunc(GL_ALWAYS);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        // Apply tone mapping
        postprocessing::apply_tonemapping(data.fbo.tex_color);

        // Render Imgui
        ImGui::Render();

        // Swap buffers
        platform::swap_buffers(data.main_window);
    }

    platform::shutdown();

    return 0;
}

void draw_random_triangles(const mat4& mvp) {
	math::set_rnd_seed(0);
	for (int i = 0; i < 500; i++) {
		vec3 v0 = vec3(math::rnd(), math::rnd(), math::rnd()) * 50.f - 50.f;
		vec3 v1 = vec3(math::rnd(), math::rnd(), math::rnd()) * 50.f - 50.f;
		vec3 v2 = vec3(math::rnd(), math::rnd(), math::rnd()) * 50.f - 50.f;
		immediate::draw_triangle(&v0[0], &v1[0], &v2[0], immediate::COLOR_RED);
	}
	immediate::flush(&mvp[0][0]);
}

void draw_main_menu(ApplicationData* data) {
    ASSERT(data);
    bool new_clicked = false;
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New", "CTRL+N")) new_clicked = true;

            if (ImGui::MenuItem("Open", "CTRL+O")) {
            }
            if (ImGui::BeginMenu("Open Recent")) {
                ImGui::MenuItem("fish_hat.c");
                ImGui::MenuItem("fish_hat.inl");
                ImGui::MenuItem("fish_hat.h");
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Save", "CTRL+S")) {
            }
            if (ImGui::MenuItem("Save As..")) {
            }
            if (ImGui::MenuItem("Export", "CTRL+E")) {
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "ALT+F4")) {
                platform::set_window_should_close(data->main_window, true);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "CTRL+Z")) {
            }
            if (ImGui::MenuItem("Redo", "CTRL+Y", false, false)) {
            }  // Disabled item
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "CTRL+X")) {
            }
            if (ImGui::MenuItem("Copy", "CTRL+C")) {
            }
            if (ImGui::MenuItem("Paste", "CTRL+V")) {
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Visuals")) {
            ImGui::Checkbox("SSAO", &data->use_ssao);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (new_clicked) ImGui::OpenPopup("Warning New");
    if (ImGui::BeginPopupModal("Warning New")) {
        ImGui::Text("By creating a new workspace you will loose any unsaved progress.");
        ImGui::Text("Are you sure?");
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void reset_view(ApplicationData* data) {
    ASSERT(data);
    ASSERT(data->mol_struct);

    vec3 min_box, max_box;
    molecule::compute_bounding_box(&min_box, &max_box, data->mol_struct->atom_positions);
    vec3 size = max_box - min_box;
    vec3 cent = (min_box + max_box) * 0.5f;
    printf("min_box: %g %g %g\n", min_box.x, min_box.y, min_box.z);
    printf("max_box: %g %g %g\n", max_box.x, max_box.y, max_box.z);

    data->controller.look_at(cent, cent + size * 2.f);
    data->camera.position = data->controller.position;
    data->camera.orientation = data->controller.orientation;
}

void init_main_framebuffer(MainFramebuffer* fbo, int width, int height) {
	ASSERT(fbo);

	bool attach_textures = false;
	if (!fbo->id) {
		glGenFramebuffers(1, &fbo->id);
		attach_textures = true;
	}
	if (!fbo->tex_depth)
		glGenTextures(1, &fbo->tex_depth);
	if (!fbo->tex_color)
		glGenTextures(1, &fbo->tex_color);
	if (!fbo->tex_picking)
		glGenTextures(1, &fbo->tex_picking);

	glBindTexture(GL_TEXTURE_2D, fbo->tex_depth);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindTexture(GL_TEXTURE_2D, fbo->tex_color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindTexture(GL_TEXTURE_2D, fbo->tex_picking);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindTexture(GL_TEXTURE_2D, 0);

	fbo->width = width;
	fbo->height = height;

	if (attach_textures) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo->id);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, fbo->tex_depth, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->tex_color, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, fbo->tex_picking, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
}

void destroy_main_framebuffer(MainFramebuffer* fbo) {
	ASSERT(fbo);
	if (fbo->id) glDeleteFramebuffers(1, &fbo->id);
	if (fbo->tex_depth) glDeleteTextures(1, &fbo->tex_depth);
	if (fbo->tex_color) glDeleteTextures(1, &fbo->tex_color);
	if (fbo->tex_picking) glDeleteTextures(1, &fbo->tex_picking);
}