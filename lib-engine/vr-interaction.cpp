#include "vr-interaction.hpp"
#include "system-transform.hpp"
#include "system-identifier.hpp"
#include "system-render.hpp"
#include "system-collision.hpp"

///////////////////////////////////////////
//   vr_input_processor implementation   //
///////////////////////////////////////////

vr_input_focus vr_input_processor::recompute_focus(const openvr_controller & controller)
{
    const ray controller_ray = ray(controller.t.position, -qzdir(controller.t.orientation));
    const entity_hit_result box_result = env->collision_system->raycast(controller_ray, raycast_type::box);

    if (box_result.r.hit)
    {
        std::cout << "Box hit: " << box_result.e << std::endl;

        // Refine if hit the mesh
        const entity_hit_result mesh_result = env->collision_system->raycast(controller_ray, raycast_type::mesh);
        if (mesh_result.r.hit)
        {
            return { controller_ray, mesh_result };
        }
        else
        {
            // Otherwise hitting the outer bounding box is still considered "in focus"
            return { controller_ray, box_result };
        }
    }
    return { controller_ray, {} };
}

vr_input_processor::vr_input_processor(entity_orchestrator * orch, environment * env, openvr_hmd * hmd) : env(env), hmd(hmd) 
{ 

}

vr_input_processor::~vr_input_processor()
{

}

vr::ETrackedControllerRole vr_input_processor::get_dominant_hand() const 
{ 
    return dominant_hand; 
}

vr_input_focus vr_input_processor::get_focus() const 
{
    return last_focus; 
}

void vr_input_processor::process(const float dt)
{
    //scoped_timer t("vr_input_processor::process()");

    // Generate button events
    for (auto hand : { vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand })
    {
        const openvr_controller controller = hmd->get_controller(hand);
        const vr_input_source_t src = (hand == vr::TrackedControllerRole_LeftHand) ? vr_input_source_t::left_controller : vr_input_source_t::right_controller;;

        for (auto b : controller.buttons)
        {
            if (b.second.pressed)
            {
                const vr_input_focus focus = recompute_focus(controller);
                vr_input_event press = make_event(vr_event_t::press, src, focus, controller);
                env->event_manager->send(press);

                // Swap dominant hand based on last activated trigger button
                if (b.first == vr::EVRButtonId::k_EButton_SteamVR_Trigger)
                {
                    dominant_hand = hand;
                }
            }
            else if (b.second.released)
            {
                const vr_input_focus focus = recompute_focus(controller);
                vr_input_event release = make_event(vr_event_t::release, src, focus, controller);
                env->event_manager->send(release);
            }
        }
    }

    // Generate focus events for the dominant hand. todo: this can be rate-limited
    {
        const openvr_controller controller = hmd->get_controller(dominant_hand);
        const vr_input_source_t src = (dominant_hand == vr::TrackedControllerRole_LeftHand) ? vr_input_source_t::left_controller : vr_input_source_t::right_controller;
        const vr_input_focus active_focus = recompute_focus(controller);

        // New focus, not invalid
        if (active_focus != last_focus && active_focus.result.e != kInvalidEntity)
        {
            vr_input_event focus_gained = make_event(vr_event_t::focus_begin, src, active_focus, controller);
            env->event_manager->send(focus_gained);
            std::cout << "dispatching vr_event_t::focus_begin for entity " << active_focus.result.e << std::endl;
            // todo - focus_end on old entity
        }

        // Last one valid, new one invalid
        if (last_focus.result.e != kInvalidEntity && active_focus.result.e == kInvalidEntity)
        {
            vr_input_event focus_lost = make_event(vr_event_t::focus_end, src, last_focus, controller);
            env->event_manager->send(focus_lost);
            std::cout << "dispatching vr_event_t::focus_end for entity " << last_focus.result.e << std::endl;
        }

        last_focus = active_focus;
    }
}

/////////////////////////////////////////////
//   vr_controller_system implementation   //
/////////////////////////////////////////////

vr_controller_system::vr_controller_system(entity_orchestrator * orch, environment * env, openvr_hmd * hmd, vr_input_processor * processor)
    : env(env), hmd(hmd), processor(processor)
{
    // fixme - the min/max teleportation bounds in world space are defined by this bounding box. 
    arc_pointer.xz_plane_bounds = aabb_3d({ -24.f, -0.01f, -24.f }, { +24.f, +0.01f, +24.f });

    // Setup the pointer entity (which is re-used between laser/arc styles)
    pointer = env->track_entity(orch->create_entity());
    env->identifier_system->create(pointer, "vr-pointer");
    env->xform_system->create(pointer, transform(float3(0, 0, 0)), { 1.f, 1.f, 1.f });
    env->render_system->create(pointer, material_component(pointer, material_handle(material_library::kDefaultMaterialId)));
    pointer_mesh_component = env->render_system->create(pointer, polymer::mesh_component(pointer, gpu_mesh_handle("vr-pointer")));

    // Setup left controller
    left_controller = env->track_entity(orch->create_entity());
    env->identifier_system->create(left_controller, "openvr-left-controller");
    env->xform_system->create(left_controller, transform(float3(0, 0, 0)), { 1.f, 1.f, 1.f });
    env->render_system->create(left_controller, material_component(left_controller, material_handle(material_library::kDefaultMaterialId)));
    env->render_system->create(left_controller, mesh_component(left_controller));

    // Setup right controller
    right_controller = env->track_entity(orch->create_entity());
    env->identifier_system->create(right_controller, "openvr-right-controller");
    env->xform_system->create(right_controller, transform(float3(0, 0, 0)), { 1.f, 1.f, 1.f });
    env->render_system->create(right_controller, material_component(right_controller, material_handle(material_library::kDefaultMaterialId)));
    env->render_system->create(right_controller, mesh_component(right_controller));

    // Setup render models for controllers when they are loaded
    hmd->controller_render_data_callback([this, env](cached_controller_render_data & data)
    {
        // We will get this callback for each controller, but we only need to handle it once for both.
        if (need_controller_render_data == true)
        {
            need_controller_render_data = false;

            // Create new gpu mesh from the openvr geometry
            create_handle_for_asset("controller-mesh", make_mesh_from_geometry(data.mesh));

            // Re-lookup components since they were std::move'd above
            auto lmc = env->render_system->get_mesh_component(left_controller);
            assert(lmc != nullptr);

            auto rmc = env->render_system->get_mesh_component(right_controller);
            assert(rmc != nullptr);

            // Set the handles
            lmc->mesh = gpu_mesh_handle("controller-mesh");
            rmc->mesh = gpu_mesh_handle("controller-mesh");
        }
    });

    input_handler_connection = env->event_manager->connect(this, [this](const vr_input_event & event)
    {
        handle_event(event);
    });
}

vr_controller_system::~vr_controller_system()
{
    // todo - input_handler_connection disconnect
}

std::vector<entity> vr_controller_system::get_renderables() const
{
    if (style != controller_render_style_t::invisible && should_draw_pointer) return { pointer, left_controller, right_controller };
    return { left_controller, right_controller };
}

void vr_controller_system::handle_event(const vr_input_event & event)
{
    // can this entity be pointed at? (list)

    // Draw laser on focus
    if (event.type == vr_event_t::focus_begin)
    {
        set_visual_style(controller_render_style_t::laser);
        should_draw_pointer = true;
    }
    else if (event.type == vr_event_t::focus_end)
    {
        set_visual_style(controller_render_style_t::invisible);
        should_draw_pointer = false;
    }
}

void vr_controller_system::process(const float dt)
{
    // update left/right controller positions
    const transform lct = hmd->get_controller(vr::TrackedControllerRole_LeftHand).t;
    env->xform_system->set_local_transform(left_controller, lct);
    const transform rct = hmd->get_controller(vr::TrackedControllerRole_RightHand).t;
    env->xform_system->set_local_transform(right_controller, rct);

    // touchpad state used for teleportation
    const std::vector<input_button_state> touchpad_button_states = {
        hmd->get_controller(vr::TrackedControllerRole_LeftHand).buttons[vr::k_EButton_SteamVR_Touchpad],
        hmd->get_controller(vr::TrackedControllerRole_RightHand).buttons[vr::k_EButton_SteamVR_Touchpad]
    };

    if (should_draw_pointer)
    {
        const vr_input_focus focus = processor->get_focus();
        if (focus.result.e != kInvalidEntity)
        {
            const float hit_distance = focus.result.r.distance;
            auto & m = pointer_mesh_component->mesh.get();
            m = make_mesh_from_geometry(make_plane(0.010f, hit_distance, 24, 24), GL_STREAM_DRAW);

            if (auto * tc = env->xform_system->get_local_transform(pointer))
            {
                // The mesh is in local space so we massage it through a transform 
                auto t = hmd->get_controller(processor->get_dominant_hand()).t;
                t = t * transform(make_rotation_quat_axis_angle({ 1, 0, 0 }, (float)POLYMER_PI / 2.f)); // coordinate
                t = t * transform(float4(0, 0, 0, 1), float3(0, -(hit_distance * 0.5f), 0)); // translation
                env->xform_system->set_local_transform(pointer, t);
            }
        }
    }

    for (int i = 0; i < touchpad_button_states.size(); ++i)
    {
        // Draw arc on touchpad down
        if (touchpad_button_states[i].down)
        {
            const transform t = hmd->get_controller(vr::ETrackedControllerRole(i + 1)).t;
            arc_pointer.position = t.position;
            arc_pointer.forward = -qzdir(t.orientation);

            if (make_pointer_arc(arc_pointer, arc_curve))
            {
                set_visual_style(controller_render_style_t::arc);
                should_draw_pointer = true;

                auto & m = pointer_mesh_component->mesh.get();
                m = make_mesh_from_geometry(make_parabolic_geometry(arc_curve, arc_pointer.forward, 0.1f, arc_pointer.lineThickness), GL_STREAM_DRAW);
                target_location = arc_curve.back(); // world-space hit point
                if (auto * tc = env->xform_system->get_local_transform(pointer))
                {
                    // the arc mesh is constructed in world space, so we reset its transform
                    env->xform_system->set_local_transform(pointer, {});
                }
            }
        }
        // Teleport on touchpad up
        else if (touchpad_button_states[i].released)
        {
            set_visual_style(controller_render_style_t::invisible);
            should_draw_pointer = false;

            // Target location is on the xz plane because of a linecast, so we re-add the current height of the player
            target_location.y = hmd->get_hmd_pose().position.y;
            const transform target_pose = { hmd->get_hmd_pose().orientation, target_location };

            hmd->set_world_pose({}); // reset world pose
            const transform hmd_pose = hmd->get_hmd_pose(); // hmd_pose is now in the HMD's own coordinate system
            hmd->set_world_pose(target_pose * hmd_pose.inverse()); // set the new world pose

            vr_teleport_event teleport_event;
            teleport_event.world_position = target_pose.position;
            teleport_event.timestamp = system_time_ns();
            env->event_manager->send(teleport_event);
        }
    }
}

/////////////////////////////////////////
//   vr_imgui_surface implementation   //
/////////////////////////////////////////

vr_imgui_surface::vr_imgui_surface(entity_orchestrator * orch, environment * env, openvr_hmd * hmd, vr_input_processor * processor, const uint2 size, GLFWwindow * window)
    : env(env), hmd(hmd), processor(processor), imgui_surface(size, window)
{
    // Setup the billboard entity
    auto mesh = make_fullscreen_quad_ndc_geom();
    for (auto & v : mesh.vertices) { v *= 0.15f; }

    create_handle_for_asset("imgui-billboard", make_mesh_from_geometry(mesh)); // gpu mesh
    create_handle_for_asset("imgui-billboard", std::move(mesh)); // cpu mesh

    imgui_billboard = env->track_entity(orch->create_entity());
    env->identifier_system->create(imgui_billboard, "imgui-billboard");
    env->xform_system->create(imgui_billboard, transform(float3(0, 0, 0)), { 1.f, 1.f, 1.f });
    env->render_system->create(imgui_billboard, material_component(imgui_billboard, material_handle("imgui")));
    env->render_system->create(imgui_billboard, mesh_component(imgui_billboard, gpu_mesh_handle("imgui-billboard")));
    env->collision_system->create(imgui_billboard, geometry_component(imgui_billboard, cpu_mesh_handle("imgui-billboard")));

    imgui_material = std::make_shared<polymer_fx_material>();
    imgui_material->shader = shader_handle("textured");
    env->mat_library->create_material("imgui", imgui_material);

    input_handler_connection = env->event_manager->connect(this, [this](const vr_input_event & event)
    {
        handle_event(event);
    });
}

vr_imgui_surface::~vr_imgui_surface()
{

}

void vr_imgui_surface::handle_event(const vr_input_event & event)
{
    if (event.type == vr_event_t::focus_begin && event.focus.result.e == imgui_billboard)
    {
        focused = true;
    }
    else if (event.type == vr_event_t::focus_end && event.focus.result.e == imgui_billboard)
    {
        focused = false;
    }
}

void vr_imgui_surface::set_surface_transform(const transform & t)
{
    // Update surface/billboard position
    if (auto * tc = env->xform_system->get_local_transform(imgui_billboard))
    {
        env->xform_system->set_local_transform(imgui_billboard, t);
    }
}

void vr_imgui_surface::process(const float dt)
{
    if (focused)
    {
        const vr_input_focus focus = processor->get_focus();
        const uint2 imgui_framebuffer_size = get_size();
        const float2 pixel_coord = { (1 - focus.result.r.uv.x) * imgui_framebuffer_size.x, focus.result.r.uv.y * imgui_framebuffer_size.y };

        const bool trigger_state = hmd->get_controller(processor->get_dominant_hand()).buttons[vr::EVRButtonId::k_EButton_SteamVR_Trigger].down;
        app_input_event controller_event;
        controller_event.type = app_input_event::MOUSE;
        controller_event.action = trigger_state;
        controller_event.value = { 0, 0 };
        controller_event.cursor = pixel_coord;
        imgui->update_input(controller_event);
    }

    imgui_material->use();
    auto & imgui_shader = imgui_material->compiled_shader->shader;
    imgui_shader.texture("s_texture", 0, get_render_texture(), GL_TEXTURE_2D);
    imgui_shader.unbind();
}

std::vector<entity> vr_imgui_surface::get_renderables() const
{
    return { imgui_billboard };
}

/////////////////////////////////
//   vr_gizmo implementation   //
/////////////////////////////////

vr_gizmo::vr_gizmo(entity_orchestrator * orch, environment * env, openvr_hmd * hmd, vr_input_processor * processor)
    : env(env), hmd(hmd), processor(processor)
{
    auto unlit_material = std::make_shared<polymer_fx_material>();
    unlit_material->shader = shader_handle("unlit-vertex-color");
    env->mat_library->create_material("unlit-vertex-color-material", unlit_material);

    gizmo_entity = env->track_entity(orch->create_entity());
    env->identifier_system->create(gizmo_entity, "gizmo-renderable");
    env->xform_system->create(gizmo_entity, transform(float3(0, 0, 0)), { 1.f, 1.f, 1.f });
    env->render_system->create(gizmo_entity, material_component(gizmo_entity, material_handle("unlit-vertex-color-material")));
    env->render_system->create(gizmo_entity, mesh_component(gizmo_entity));
    env->collision_system->create(gizmo_entity, geometry_component(gizmo_entity));

    // tinygizmo uses a callback to pass its world-space mesh back to users. The callback is triggered by
    // the process(...) function below. 
    gizmo_ctx.render = [this, env](const tinygizmo::geometry_mesh & r)
    {
        const std::vector<tinygizmo::geometry_vertex> & verts = r.vertices;
        const std::vector<linalg::aliases::uint3> & tris = reinterpret_cast<const std::vector<linalg::aliases::uint3> &>(r.triangles);

        // For rendering
        if (auto * mc = env->render_system->get_mesh_component(gizmo_entity))
        {
            auto & gizmo_gpu_mesh = mc->mesh.get();

            // Upload new gizmo mesh data to the gpu
            gizmo_gpu_mesh.set_vertices(verts, GL_DYNAMIC_DRAW);
            gizmo_gpu_mesh.set_attribute(0, 3, GL_FLOAT, GL_FALSE, sizeof(tinygizmo::geometry_vertex), (GLvoid*)offsetof(tinygizmo::geometry_vertex, position));
            gizmo_gpu_mesh.set_attribute(1, 3, GL_FLOAT, GL_FALSE, sizeof(tinygizmo::geometry_vertex), (GLvoid*)offsetof(tinygizmo::geometry_vertex, normal));
            gizmo_gpu_mesh.set_attribute(2, 3, GL_FLOAT, GL_FALSE, sizeof(tinygizmo::geometry_vertex), (GLvoid*)offsetof(tinygizmo::geometry_vertex, color));
            gizmo_gpu_mesh.set_elements(tris, GL_DYNAMIC_DRAW);
        }

        // For focus/defocus and pointing
        if (auto * gc = env->collision_system->get_component(gizmo_entity))
        {
            if (verts.size() != transient_gizmo_geom.vertices.size()) transient_gizmo_geom.vertices.resize(verts.size());
            if (tris.size() != transient_gizmo_geom.faces.size()) transient_gizmo_geom.faces.resize(tris.size());

            // Verts are packed in a struct
            for (int i = 0; i < verts.size(); ++i)
            {
                auto & v = verts[i];
                transient_gizmo_geom.vertices[i] = {v.position.x, v.position.y, v.position.z};
            }

            // Faces can be memcpy'd directly
            std::memcpy(transient_gizmo_geom.faces.data(), tris.data(), tris.size() * sizeof(uint3));

            auto & gizmo_cpu_mesh = gc->geom.get();
            gizmo_cpu_mesh = transient_gizmo_geom;
        }
    };

    input_handler_connection = env->event_manager->connect(this, [this](const vr_input_event & event)
    {
        handle_event(event);
    });
}

vr_gizmo::~vr_gizmo()
{
    // fixme - input_handler_connection must be disconnected
}

void vr_gizmo::handle_event(const vr_input_event & event)
{
    if (event.type == vr_event_t::focus_begin && event.focus.result.e == gizmo_entity)
    {
        focused = true;
    }
    else if (event.type == vr_event_t::focus_end && event.focus.result.e == gizmo_entity)
    {
        focused = false;
    }
}

void vr_gizmo::process(const float dt)
{
    const auto view = view_data(0, hmd->get_eye_pose(vr::Hmd_Eye::Eye_Left), hmd->get_proj_matrix(vr::Hmd_Eye::Eye_Left, 0.075f, 64.f));
    const auto vfov = vfov_from_projection(view.projectionMatrix);

    gizmo_state.cam.near_clip = view.nearClip;
    gizmo_state.cam.far_clip = view.farClip;
    gizmo_state.cam.yfov = vfov;
    gizmo_state.cam.position = minalg::float3(view.pose.position.x, view.pose.position.y, view.pose.position.z);
    gizmo_state.cam.orientation = minalg::float4(view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z, view.pose.orientation.w);
    
    if (focused)
    {   
        const vr_input_focus focus = processor->get_focus();
        gizmo_state.ray_origin = minalg::float3(focus.r.origin.x, focus.r.origin.y, focus.r.origin.z);
        gizmo_state.ray_direction = minalg::float3(focus.r.direction.x, focus.r.direction.y, focus.r.direction.z);
        gizmo_state.mouse_left = hmd->get_controller(processor->get_dominant_hand()).buttons[vr::EVRButtonId::k_EButton_SteamVR_Trigger].down;
    }

    // Update
    gizmo_ctx.update(gizmo_state);

    // Draw gizmo @ transform
    tinygizmo::transform_gizmo("vr-gizmo", gizmo_ctx, xform);

    // Trigger render callback
    gizmo_ctx.draw();
}

std::vector<entity> vr_gizmo::get_renderables() const
{
    return { gizmo_entity };
}