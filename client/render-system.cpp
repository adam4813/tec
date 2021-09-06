#include "render-system.hpp"

#include <cmath>
#include <thread>

#include <glm/ext.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include <google/protobuf/util/json_util.h>
#include <graphics.pb.h>

#include "components/transforms.hpp"
#include "entity.hpp"
#include "events.hpp"
#include "graphics/animation.hpp"
#include "graphics/lights.hpp"
#include "graphics/material.hpp"
#include "graphics/renderable.hpp"
#include "graphics/shader.hpp"
#include "graphics/texture-object.hpp"
#include "graphics/view.hpp"
#include "multiton.hpp"
#include "proto-load.hpp"
#include "resources/mesh.hpp"
#include "resources/obj.hpp"
#include "resources/pixel-buffer.hpp"

namespace tec {
using PointLightMap = Multiton<eid, PointLight*>;
using DirectionalLightMap = Multiton<eid, DirectionalLight*>;
using RenderableMap = Multiton<eid, Renderable*>;
using AnimationMap = Multiton<eid, Animation*>;
using ScaleMap = Multiton<eid, Scale*>;

void RenderSystem::Startup() {
	_log = spdlog::get("console_log");

	GLenum err = glGetError();
	// If there is an error that means something went wrong when creating the context.
	if (err) {
		_log->debug("[RenderSystem] Something went wrong when creating the context.");
		return;
	}

	// load the list of extensions
	GLint num_exts = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &num_exts);
	std::string ext("");
	for (GLint e = 0; e < num_exts; e++) {
		extensions.emplace(std::string((const char*)glGetStringi(GL_EXTENSIONS, e)));
	}

	if (HasExtension("GL_ARB_clip_control")) {
		_log->debug("[RenderSystem] Using glClipControl.");
		glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
	}
	// Black is the safest clear color since this is a space game.
	glClearColor(0.0, 0.0, 0.0, 0.0);
	// Reversed Z buffering for improved precision (maybe)
	glClearDepth(0.0f);
	glDepthFunc(GL_GREATER);
	std::shared_ptr<OBJ> sphere = OBJ::Create(FilePath::GetAssetPath("/sphere/sphere.obj"));
	if (!sphere) {
		_log->debug("[RenderSystem] Error loading sphere.obj.");
	}
	else {
		this->sphere_vbo.Load(sphere);
	}
	std::shared_ptr<OBJ> quad = OBJ::Create(FilePath::GetAssetPath("/quad/quad.obj"));
	if (!quad) {
		_log->debug("[RenderSystem] Error loading quad.obj.");
	}
	else {
		this->quad_vbo.Load(quad);
	}

	this->inv_view_size.x = 1.0f / static_cast<float>(this->view_size.x);
	this->inv_view_size.y = 1.0f / static_cast<float>(this->view_size.y);
	this->light_gbuffer.AddColorAttachments(this->view_size.x, this->view_size.y);
	this->light_gbuffer.SetDepthAttachment(
			GBuffer::GBUFFER_DEPTH_TYPE::GBUFFER_DEPTH_TYPE_STENCIL, this->view_size.x, this->view_size.y);
	if (!this->light_gbuffer.CheckCompletion()) {
		_log->error("[RenderSystem] Failed to create Light GBuffer.");
	}

	const size_t checker_size = 64;
	auto default_pbuffer = std::make_shared<PixelBuffer>(checker_size, checker_size, 8, ImageColorMode::COLOR_RGBA);
	{
		std::lock_guard lg(default_pbuffer->GetWritelock());
		auto pixels = reinterpret_cast<uint32_t*>(default_pbuffer->GetPtr());
		for (size_t y = 0; y < checker_size; y++) {
			for (size_t x = 0; x < checker_size; x++) {
				uint32_t c = ((x / 8) ^ (y / 8)) & 1; // c is 1 or 0 in an 8x8 checker pattern
				*(pixels++) = 0xff000000 | (c * 0xffffff); // set pixel with full alpha
			}
		}
	}

	PixelBufferMap::Set("default", default_pbuffer);

	std::shared_ptr<TextureObject> default_texture = std::make_shared<TextureObject>(default_pbuffer);
	TextureMap::Set("default", default_texture);

	this->SetupDefaultShaders();
	_log->info("[RenderSystem] Startup complete.");
}

void RenderSystem::SetViewportSize(unsigned int width, unsigned int height) {
	auto viewport = glm::max(glm::uvec2(1), glm::uvec2(width, height));
	this->view_size = viewport;
	this->inv_view_size = 1.0f / glm::vec2(viewport);
	float aspect_ratio = static_cast<float>(viewport.x) / static_cast<float>(viewport.y);
	if ((aspect_ratio < 1.0f) || std::isnan(aspect_ratio)) {
		aspect_ratio = 4.0f / 3.0f;
	}

	this->projection = glm::perspective(glm::radians(45.0f), aspect_ratio, 0.1f, 10000.0f);
	// convert the projection to reverse depth with infinite far plane
	this->projection[2][2] = 0.0f;
	this->projection[3][2] = 0.1f;
	this->light_gbuffer.ResizeColorAttachments(viewport.x, viewport.y);
	this->light_gbuffer.ResizeDepthAttachment(viewport.x, viewport.y);
	glViewport(0, 0, viewport.x, viewport.y);
}

void RenderSystem::Update(const double delta) {
	ProcessCommandQueue();
	EventQueue<WindowResizedEvent>::ProcessEventQueue();
	EventQueue<EntityCreated>::ProcessEventQueue();
	EventQueue<EntityDestroyed>::ProcessEventQueue();

	GLenum err;
	err = glGetError();
	if (err) {
		_log->debug("[GL] Preframe error {}", err);
	}
	UpdateRenderList(delta);
	this->light_gbuffer.StartFrame();

	GeometryPass();

	this->light_gbuffer.BeginLightPass();
	glEnable(GL_STENCIL_TEST);
	BeginPointLightPass();
	glDisable(GL_STENCIL_TEST);
	DirectionalLightPass();

	FinalPass();
	// RenderGbuffer();
	err = glGetError();
	if (err) {
		_log->debug("[GL] Postframe error {}", err);
	}
}

void RenderSystem::GeometryPass() {
	this->light_gbuffer.BeginGeometryPass();

	glm::mat4 camera_matrix(1.0f);
	{
		View* view = this->current_view;
		if (view) {
			camera_matrix = view->view_matrix;
		}
	}

	std::shared_ptr<Shader> def_shader = ShaderMap::Get("deferred");
	def_shader->Use();
	glUniformMatrix4fv(def_shader->GetUniformLocation("view"), 1, GL_FALSE, glm::value_ptr(camera_matrix));
	glUniformMatrix4fv(def_shader->GetUniformLocation("projection"), 1, GL_FALSE, glm::value_ptr(this->projection));
	GLint animatrix_loc = def_shader->GetUniformLocation("animation_matrix");
	GLint animated_loc = def_shader->GetUniformLocation("animated");
	GLint model_index = def_shader->GetUniformLocation("model");
	for (auto shader_list : this->render_item_list) {
		// Check if we need to use a specific shader and set it up.
		if (shader_list.first) {
			def_shader->UnUse();
			shader_list.first->Use();
			glUniformMatrix4fv(
					shader_list.first->GetUniformLocation("view"), 1, GL_FALSE, glm::value_ptr(camera_matrix));
			glUniformMatrix4fv(
					shader_list.first->GetUniformLocation("projection"), 1, GL_FALSE, glm::value_ptr(this->projection));
			animatrix_loc = shader_list.first->GetUniformLocation("animation_matrix");
			animated_loc = shader_list.first->GetUniformLocation("animated");
			model_index = shader_list.first->GetUniformLocation("model");
		}
		for (auto render_item : shader_list.second) {
			glBindVertexArray(render_item->vbo->GetVAO());
			glUniform1i(animated_loc, 0);
			if (render_item->animated) {
				glUniform1i(animated_loc, 1);
				auto& animmatricies = render_item->animation->bone_matrices;
				glUniformMatrix4fv(
						animatrix_loc,
						static_cast<GLsizei>(animmatricies.size()),
						GL_FALSE,
						glm::value_ptr(animmatricies[0]));
			}
			for (VertexGroup& vertex_group : render_item->vertex_groups) {
				glPolygonMode(GL_FRONT_AND_BACK, vertex_group.material->GetPolygonMode());
				vertex_group.material->Activate();
				glUniformMatrix4fv(model_index, 1, GL_FALSE, glm::value_ptr(render_item->model_matrix));
				glDrawElements(
						vertex_group.material->GetDrawElementsMode(),
						static_cast<GLsizei>(vertex_group.index_count),
						GL_UNSIGNED_INT,
						(GLvoid*)(vertex_group.starting_offset * sizeof(GLuint)));
				vertex_group.material->Deactivate();
			}
		}
		// If we used a special shader set things back to the deferred shader.
		if (shader_list.first) {
			shader_list.first->UnUse();
			def_shader->Use();
			animatrix_loc = def_shader->GetUniformLocation("animation_matrix");
			animated_loc = def_shader->GetUniformLocation("animated");
			model_index = def_shader->GetUniformLocation("model");
		}
	}
	def_shader->UnUse();
	glBindVertexArray(0);
	this->light_gbuffer.EndGeometryPass();
}

void RenderSystem::BeginPointLightPass() {
	glm::mat4 camera_matrix(1.0f);
	{
		View* view = this->current_view;
		if (view) {
			camera_matrix = view->view_matrix;
		}
	}

	std::shared_ptr<Shader> def_pl_shader = ShaderMap::Get("deferred_pointlight");
	def_pl_shader->Use();
	glUniformMatrix4fv(def_pl_shader->GetUniformLocation("view"), 1, GL_FALSE, glm::value_ptr(camera_matrix));
	glUniformMatrix4fv(def_pl_shader->GetUniformLocation("projection"), 1, GL_FALSE, glm::value_ptr(this->projection));
	glUniform1i(
			def_pl_shader->GetUniformLocation("gPositionMap"),
			static_cast<GLint>(GBuffer::GBUFFER_TEXTURE_TYPE::GBUFFER_TEXTURE_TYPE_POSITION));
	glUniform1i(
			def_pl_shader->GetUniformLocation("gNormalMap"),
			static_cast<GLint>(GBuffer::GBUFFER_TEXTURE_TYPE::GBUFFER_TEXTURE_TYPE_NORMAL));
	glUniform1i(
			def_pl_shader->GetUniformLocation("gColorMap"),
			static_cast<GLint>(GBuffer::GBUFFER_TEXTURE_TYPE::GBUFFER_TEXTURE_TYPE_DIFFUSE));
	glUniform2f(def_pl_shader->GetUniformLocation("gScreenSize"), this->inv_view_size.x, this->inv_view_size.y);
	GLint model_index = def_pl_shader->GetUniformLocation("model");
	GLint Color_index = def_pl_shader->GetUniformLocation("gPointLight.Base.Color");
	GLint AmbientIntensity_index = def_pl_shader->GetUniformLocation("gPointLight.Base.AmbientIntensity");
	GLint DiffuseIntensity_index = def_pl_shader->GetUniformLocation("gPointLight.Base.DiffuseIntensity");
	GLint Atten_Constant_index = def_pl_shader->GetUniformLocation("gPointLight.Atten.Constant");
	GLint Atten_Linear_index = def_pl_shader->GetUniformLocation("gPointLight.Atten.Linear");
	GLint Atten_Exp_index = def_pl_shader->GetUniformLocation("gPointLight.Atten.Exp");

	glBindVertexArray(this->sphere_vbo.GetVAO());

	auto index_count{static_cast<GLsizei>(this->sphere_vbo.GetVertexGroup(0)->index_count)};

	this->light_gbuffer.BeginLightPass();
	this->light_gbuffer.BeginPointLightPass();
	def_pl_shader->Use();

	for (auto itr = PointLightMap::Begin(); itr != PointLightMap::End(); ++itr) {
		eid entity_id = itr->first;
		PointLight* light = itr->second;

		glm::vec3 position;
		glm::quat orientation;
		if (Multiton<eid, Position*>::Has(entity_id)) {
			position = Multiton<eid, Position*>::Get(entity_id)->value;
		}
		if (Multiton<eid, Orientation*>::Has(entity_id)) {
			orientation = Multiton<eid, Orientation*>::Get(entity_id)->value;
		}

		light->UpdateBoundingRadius();
		glm::mat4 transform_matrix =
				glm::scale(glm::translate(glm::mat4(1.0), position), glm::vec3(light->bounding_radius));

		glUniformMatrix4fv(model_index, 1, GL_FALSE, glm::value_ptr(transform_matrix));
		glUniform3f(Color_index, light->color.x, light->color.y, light->color.z);
		glUniform1f(AmbientIntensity_index, light->ambient_intensity);
		glUniform1f(DiffuseIntensity_index, light->diffuse_intensity);
		glUniform1f(Atten_Constant_index, light->Attenuation.constant);
		glUniform1f(Atten_Linear_index, light->Attenuation.linear);
		glUniform1f(Atten_Exp_index, light->Attenuation.exponential);
		glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, nullptr);
	}
	// these would go in the loop for stencil lights
	this->light_gbuffer.EndPointLightPass();
	def_pl_shader->UnUse();

	glBindVertexArray(0);
}

void RenderSystem::DirectionalLightPass() {
	this->light_gbuffer.BeginDirLightPass();
	std::shared_ptr<Shader> def_dl_shader = ShaderMap::Get("deferred_dirlight");
	def_dl_shader->Use();

	glm::mat4 camera_matrix{1.f};
	{
		View* view = this->current_view;
		if (view) {
			camera_matrix = view->view_matrix;
		}
	}
	glUniform1i(
			def_dl_shader->GetUniformLocation("gPositionMap"),
			static_cast<GLint>(GBuffer::GBUFFER_TEXTURE_TYPE::GBUFFER_TEXTURE_TYPE_POSITION));
	glUniform1i(
			def_dl_shader->GetUniformLocation("gNormalMap"),
			static_cast<GLint>(GBuffer::GBUFFER_TEXTURE_TYPE::GBUFFER_TEXTURE_TYPE_NORMAL));
	glUniform1i(
			def_dl_shader->GetUniformLocation("gColorMap"),
			static_cast<GLint>(GBuffer::GBUFFER_TEXTURE_TYPE::GBUFFER_TEXTURE_TYPE_DIFFUSE));
	glUniform2f(def_dl_shader->GetUniformLocation("gScreenSize"), this->inv_view_size.x, this->inv_view_size.y);
	glUniform3f(def_dl_shader->GetUniformLocation("gEyeWorldPos"), 0, 0, 0);
	GLint Color_index = def_dl_shader->GetUniformLocation("gDirectionalLight.Base.Color");
	GLint AmbientIntensity_index = def_dl_shader->GetUniformLocation("gDirectionalLight.Base.AmbientIntensity");
	GLint DiffuseIntensity_index = def_dl_shader->GetUniformLocation("gDirectionalLight.Base.DiffuseIntensity");
	GLint direction_index = def_dl_shader->GetUniformLocation("gDirectionalLight.Direction");

	glBindVertexArray(this->quad_vbo.GetVAO());

	auto index_count{static_cast<GLsizei>(this->quad_vbo.GetVertexGroup(0)->index_count)};

	for (auto itr = DirectionalLightMap::Begin(); itr != DirectionalLightMap::End(); ++itr) {
		DirectionalLight* light = itr->second;

		glUniform3f(Color_index, light->color.x, light->color.y, light->color.z);
		glUniform1f(AmbientIntensity_index, light->ambient_intensity);
		glUniform1f(DiffuseIntensity_index, light->diffuse_intensity);
		glUniform3f(direction_index, light->direction.x, light->direction.y, light->direction.z);

		glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, nullptr);
	}
	def_dl_shader->UnUse();
	glBindVertexArray(0);
}

void RenderSystem::FinalPass() {
	this->light_gbuffer.FinalPass();

	glBlitFramebuffer(
			0,
			0,
			this->view_size.x,
			this->view_size.y,
			0,
			0,
			this->view_size.x,
			this->view_size.y,
			GL_COLOR_BUFFER_BIT,
			GL_LINEAR);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderSystem::RenderGbuffer() {
	this->light_gbuffer.BindForRendering();
	glDisable(GL_BLEND);
	glActiveTexture(GL_TEXTURE0 + 3);
	glBindSampler(GL_TEXTURE_2D, 0);
	glBindTexture(GL_TEXTURE_2D, this->light_gbuffer.GetDepthTexture());
	glDrawBuffer(GL_BACK);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	glBindVertexArray(this->quad_vbo.GetVAO());

	std::shared_ptr<Shader> def_db_shader = ShaderMap::Get("deferred_debug");
	def_db_shader->Use();

	glUniform1i(
			def_db_shader->GetUniformLocation("gPositionMap"),
			static_cast<GLint>(GBuffer::GBUFFER_TEXTURE_TYPE::GBUFFER_TEXTURE_TYPE_POSITION));
	glUniform1i(
			def_db_shader->GetUniformLocation("gNormalMap"),
			static_cast<GLint>(GBuffer::GBUFFER_TEXTURE_TYPE::GBUFFER_TEXTURE_TYPE_NORMAL));
	glUniform1i(
			def_db_shader->GetUniformLocation("gColorMap"),
			static_cast<GLint>(GBuffer::GBUFFER_TEXTURE_TYPE::GBUFFER_TEXTURE_TYPE_DIFFUSE));
	glUniform1i(def_db_shader->GetUniformLocation("gDepthMap"), static_cast<GLint>(3));
	glUniform2f(def_db_shader->GetUniformLocation("gScreenSize"), this->inv_view_size.x, this->inv_view_size.y);

	auto index_count{static_cast<GLsizei>(this->quad_vbo.GetVertexGroup(0)->index_count)};
	glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, nullptr);

	def_db_shader->UnUse();
	glBindVertexArray(0);
}

void RenderSystem::SetupDefaultShaders() {
	tec::gfx::ShaderList shader_list;
	auto core_fname = FilePath::GetAssetPath("shaders/core.json");
	auto core_json = LoadAsString(core_fname);
	auto status = google::protobuf::util::JsonStringToMessage(core_json, &shader_list);
	if (!status.ok()) {
		_log->error("[RenderSystem] loading shader list: {} failed: {}", core_fname.toString(), status.ToString());
		return;
	}
	for (auto& shader_def : shader_list.shaders()) {
		Shader::CreateFromDef(shader_def);
	}
	auto debug_fill = Material::Create("material_debug");
	debug_fill->SetPolygonMode(GL_LINE);
	debug_fill->SetDrawElementsMode(GL_LINES);
}

void RenderSystem::On(std::shared_ptr<WindowResizedEvent> data) {
	SetViewportSize(
			data->new_width > 0 ? static_cast<unsigned int>(data->new_width) : 0,
			data->new_height > 0 ? static_cast<unsigned int>(data->new_height) : 0);
}

void RenderSystem::On(std::shared_ptr<EntityDestroyed> data) { RenderableMap::Remove(data->entity_id); }

void RenderSystem::On(std::shared_ptr<EntityCreated> data) {
	eid entity_id = data->entity.id();
	for (int i = 0; i < data->entity.components_size(); ++i) {
		const proto::Component& comp = data->entity.components(i);
		switch (comp.component_case()) {
		case proto::Component::kRenderable:
		{
			Renderable* renderable = new Renderable();
			renderable->In(comp);
			RenderableMap::Set(entity_id, renderable);
			break;
		}
		case proto::Component::kPointLight:
		{
			PointLight* point_light = new PointLight();
			point_light->In(comp);
			PointLightMap::Set(entity_id, point_light);
			break;
		}
		case proto::Component::kDirectionalLight:
		{
			DirectionalLight* dir_light = new DirectionalLight();
			dir_light->In(comp);
			DirectionalLightMap::Set(entity_id, dir_light);
			break;
		}
		case proto::Component::kAnimation:
		{
			Animation* animation = new Animation();
			animation->In(comp);
			AnimationMap::Set(entity_id, animation);
			break;
		}
		case proto::Component::kScale:
		{
			Scale* scale = new Scale();
			scale->In(comp);
			ScaleMap::Set(entity_id, scale);
			break;
		}
		default: break;
		}
	}
}

void RenderSystem::UpdateRenderList(double delta) {
	this->render_item_list.clear();

	if (!this->default_shader) {
		this->default_shader = ShaderMap::Get("debug");
	}

	// Loop through each renderbale and update its model matrix.
	for (auto itr = RenderableMap::Begin(); itr != RenderableMap::End(); ++itr) {
		eid entity_id = itr->first;
		Renderable* renderable = itr->second;
		if (renderable->hidden) {
			continue;
		}
		Entity entity(entity_id);
		auto [_position, _orientation, _scale, _animation] = entity.GetList<Position, Orientation, Scale, Animation>();
		glm::vec3 position = renderable->local_translation;
		if (_position) {
			position += _position->value;
		}
		glm::quat orientation = renderable->local_orientation.value;
		if (_orientation) {
			orientation *= _orientation->value;
		}
		glm::vec3 scale(1.0);
		if (_scale) {
			scale = _scale->value;
		}

		auto mesh = renderable->mesh;
		auto ri = renderable->render_item;
		if (!mesh && ri) {
			renderable->render_item.reset();
			ri.reset();
		}
		if (mesh && (!ri || ri->mesh_at_set != mesh.get())) {
			auto buffer_itr = mesh_buffers.find(mesh);
			std::shared_ptr<VertexBufferObject> buffer;
			if (buffer_itr == mesh_buffers.cend()) {
				buffer = std::make_shared<VertexBufferObject>(vertex::VF_FULL);
				buffer->Load(mesh);
				mesh_buffers[mesh] = buffer;
			}
			else {
				buffer = buffer_itr->second;
			}
			std::size_t group_count = buffer->GetVertexGroupCount();
			if (group_count > 0) {
				if (!ri) {
					ri = std::make_shared<RenderItem>();
				}
				else {
					ri->vertex_groups.clear();
				}
				ri->vbo = buffer;
				ri->vertex_groups.reserve(group_count);
				ri->mesh_at_set = mesh.get();
				for (std::size_t i = 0; i < group_count; ++i) {
					ri->vertex_groups.push_back(*buffer->GetVertexGroup(i));
				}
				renderable->render_item = ri;
			}
			else {
				_log->warn("[RenderSystem] empty mesh on Renderable [{}]", entity_id);
				renderable->render_item.reset();
				ri.reset();
			}
		}

		if (ri) {
			ri->vbo->Update();
			ri->model_matrix =
					glm::scale(glm::translate(glm::mat4(1.0), position) * glm::mat4_cast(orientation), scale);

			if (_animation) {
				const_cast<Animation*>(_animation)->UpdateAnimation(delta);
				if (_animation->bone_matrices.size() > 0) {
					ri->animated = true;
					ri->animation = const_cast<Animation*>(_animation);
				}
			}
			this->render_item_list[renderable->shader].insert(ri.get());
		}
	}

	for (auto itr = Multiton<eid, View*>::Begin(); itr != Multiton<eid, View*>::End(); ++itr) {
		eid entity_id = itr->first;
		View* view = itr->second;

		Entity entity(entity_id);
		auto [_position, _orientation] = entity.GetList<Position, Orientation>();
		glm::vec3 position;
		if (_position) {
			position = _position->value;
		}
		glm::quat orientation;
		if (_orientation) {
			orientation = _orientation->value;
		}

		view->view_matrix = glm::inverse(glm::translate(glm::mat4(1.0), position) * glm::mat4_cast(orientation));
		if (view->active) {
			this->current_view = view;
		}
	}
}
} // namespace tec
