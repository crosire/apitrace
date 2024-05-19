/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "trace_data.hpp"
#include <reshade.hpp>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <shared_mutex>

using namespace reshade::api;

static std::unordered_map<uint64_t, sampler> s_samplers;
static std::unordered_map<uint64_t, resource> s_resources;
static std::unordered_map<uint64_t, resource_view> s_resource_views;
static std::unordered_map<uint64_t, pipeline> s_pipelines;
static std::unordered_map<uint64_t, pipeline_layout> s_pipeline_layouts;
static std::unordered_map<uint64_t, descriptor_table> s_descriptor_tables;

static void play_init_swapchain(trace_data_read &trace_data, effect_runtime *runtime)
{
	device *const device = runtime->get_device();

	const auto buffer_count = trace_data.read<uint32_t>();

	for (uint32_t i = 0; i < buffer_count; ++i)
	{
		const auto handle = trace_data.read<resource>().handle;

		s_resources[handle] = runtime->get_back_buffer(i < runtime->get_back_buffer_count() ? i : 0);
		if (device->get_api() == device_api::d3d9 || device->get_api() == device_api::opengl)
			s_resource_views[handle] = { s_resources[handle].handle };
	}
}
static void play_destroy_swapchain(trace_data_read &trace_data, effect_runtime *runtime)
{
	device *const device = runtime->get_device();

	const auto buffer_count = trace_data.read<uint32_t>();

	for (uint32_t i = 0; i < buffer_count; ++i)
	{
		const auto handle = trace_data.read<resource>().handle;

		s_resources[handle] = {};
		if (device->get_api() == device_api::d3d9 || device->get_api() == device_api::opengl)
			s_resource_views[handle] = {};
	}
}

static void play_init_sampler(trace_data_read &trace_data, device *device)
{
	const auto desc = trace_data.read<sampler_desc>();
	const auto handle = trace_data.read<sampler>().handle;

	if (!device->create_sampler(desc, &s_samplers[handle]))
		assert(false);
}
static void play_destroy_sampler(trace_data_read &trace_data, device *device)
{
	const auto handle = trace_data.read<sampler>().handle;

	device->destroy_sampler(s_samplers[handle]);
	s_samplers[handle] = {};
}

static void play_init_resource(trace_data_read &trace_data, device *device)
{
	auto desc = trace_data.read<resource_desc>();
	const auto initial_state = trace_data.read<resource_usage>();
	const auto handle = trace_data.read<resource>().handle;

	const auto subresources = trace_data.read<uint32_t>();

	std::vector<std::vector<uint8_t>> data(subresources);
	std::vector<subresource_data> initial_data(subresources);

	if (desc.type == resource_type::buffer)
	{
		if (subresources != 0)
		{
			data[0].resize(static_cast<size_t>(desc.buffer.size));
			trace_data.read(data[0].data(), data[0].size());

			initial_data[0].data = data[0].data();
		}
	}
	else
	{
		if (device->get_api() == device_api::opengl && desc.texture.levels == 0)
			desc.texture.levels = 1;

		const uint32_t levels = desc.texture.levels;
		const uint32_t layers = (desc.type != resource_type::texture_3d) ? desc.texture.depth_or_layers : 1;

		for (uint32_t layer = 0; layer < layers; ++layer)
		{
			for (uint32_t level = 0; level < levels; ++level)
			{
				const uint32_t subresource = layer * levels + level;
				if (subresource >= subresources)
					break;

				subresource_data subresource_data = {};
				subresource_data.row_pitch = trace_data.read<uint32_t>();
				subresource_data.slice_pitch = trace_data.read<uint32_t>();

				data[subresource].resize(static_cast<size_t>(trace_data.read<uint64_t>()));
				trace_data.read(data[subresource].data(), data[subresource].size());

				subresource_data.data = data[subresource].data();
				initial_data[subresource] = subresource_data;
			}
		}
	}

	if (device->get_api() == device_api::opengl && (handle >> 40) == 0x8218 /* GL_FRAMEBUFFER_DEFAULT */)
	{
		s_resources[handle].handle = handle;
		return;
	}

	if (s_resources.find(handle) != s_resources.end())
		device->destroy_resource(s_resources[handle]);

	if (!device->create_resource(desc, initial_data.data(), initial_state, &s_resources[handle]))
		assert(false);
}
static void play_destroy_resource(trace_data_read &trace_data, device *device)
{
	const auto handle = trace_data.read<resource>().handle;

	if (device->get_api() == device_api::opengl && (handle >> 40) == 0x8218 /* GL_FRAMEBUFFER_DEFAULT */)
		return;

	device->destroy_resource(s_resources[handle]);
	s_resources[handle] = {};
}

static void play_init_resource_view(trace_data_read &trace_data, device *device)
{
	const auto resource_handle = trace_data.read<resource_view>().handle;
	const auto usage_type = trace_data.read<resource_usage>();
	const auto desc = trace_data.read<resource_view_desc>();
	const auto handle = trace_data.read<resource_view>().handle;

	if (device->get_api() == device_api::opengl && (handle >> 40) == 0x8218 /* GL_FRAMEBUFFER_DEFAULT */)
	{
		s_resource_views[handle].handle = handle;
		return;
	}

	if (s_resource_views.find(handle) != s_resource_views.end())
		device->destroy_resource_view(s_resource_views[handle]);

	if (!device->create_resource_view(s_resources[resource_handle], usage_type, desc, &s_resource_views[handle]))
		assert(false);
}
static void play_destroy_resource_view(trace_data_read &trace_data, device *device)
{
	const auto handle = trace_data.read<resource_view>().handle;

	device->destroy_resource_view(s_resource_views[handle]);
	s_resource_views[handle] = {};
}

static void play_init_pipeline(trace_data_read &trace_data, device *device)
{
	const auto layout = trace_data.read<pipeline_layout>().handle;
	const auto subobject_count = trace_data.read<uint32_t>();

	std::vector<pipeline_subobject> subobjects(subobject_count);
	shader_desc shader_descs[6];
	std::vector<char> shader_code[6];
	std::vector<char> entry_points[6];
	std::vector<input_element> input_layout;
	std::vector<std::vector<char>> input_layout_semantics;
	blend_desc blend_state;
	rasterizer_desc rasterizer_state;
	depth_stencil_desc depth_stencil_state;

	for (uint32_t i = 0; i < subobject_count; ++i)
	{
		subobjects[i].type = trace_data.read<pipeline_subobject_type>();

		switch (subobjects[i].type)
		{
		case pipeline_subobject_type::vertex_shader:
		case pipeline_subobject_type::hull_shader:
		case pipeline_subobject_type::domain_shader:
		case pipeline_subobject_type::geometry_shader:
		case pipeline_subobject_type::pixel_shader:
		case pipeline_subobject_type::compute_shader:
		{
			const auto shader_type_index = static_cast<size_t>(subobjects[i].type) - static_cast<size_t>(pipeline_subobject_type::vertex_shader);

			const auto desc = static_cast<shader_desc *>(&shader_descs[shader_type_index]);
			std::vector<char> &code = shader_code[shader_type_index];
			std::vector<char> &entry_point = entry_points[shader_type_index];

			const auto code_size = trace_data.read<uint64_t>();
			code.resize(static_cast<size_t>(code_size));
			trace_data.read(code.data(), static_cast<size_t>(code_size));

			const auto entry_point_length = trace_data.read<uint32_t>();
			entry_point.resize(entry_point_length + 1);
			trace_data.read(entry_point.data(), entry_point_length);

			desc->code = code.data();
			desc->code_size = static_cast<size_t>(code_size);
			desc->entry_point = entry_point_length ? entry_point.data() : nullptr;

			subobjects[i].count = 1;
			subobjects[i].data = desc;
			break;
		}
		case pipeline_subobject_type::input_layout:
		{
			const auto count = trace_data.read<uint32_t>();

			input_layout.resize(count);
			input_layout_semantics.resize(count);

			for (uint32_t k = 0; k < count; ++k)
			{
				input_layout[k].location = trace_data.read<uint32_t>();

				const auto semantic_length = trace_data.read<uint32_t>();
				input_layout_semantics[k].resize(semantic_length + 1);
				trace_data.read(input_layout_semantics[k].data(), semantic_length);

				input_layout[k].semantic_index = trace_data.read<uint32_t>();
				input_layout[k].format = trace_data.read<format>();
				input_layout[k].buffer_binding = trace_data.read<uint32_t>();
				input_layout[k].offset = trace_data.read<uint32_t>();
				input_layout[k].stride = trace_data.read<uint32_t>();
				input_layout[k].instance_step_rate = trace_data.read<uint32_t>();

				input_layout[k].semantic = semantic_length ? input_layout_semantics[k].data() : nullptr;
			}

			subobjects[i].count = count;
			subobjects[i].data = input_layout.data();
			break;
		}
		case pipeline_subobject_type::blend_state:
		{
			blend_state = trace_data.read<blend_desc>();

			subobjects[i].count = 1;
			subobjects[i].data = &blend_state;
			break;
		}
		case pipeline_subobject_type::rasterizer_state:
		{
			rasterizer_state = trace_data.read<rasterizer_desc>();

			subobjects[i].count = 1;
			subobjects[i].data = &rasterizer_state;
			break;
		}
		case pipeline_subobject_type::depth_stencil_state:
		{
			depth_stencil_state = trace_data.read<depth_stencil_desc>();

			subobjects[i].count = 1;
			subobjects[i].data = &depth_stencil_state;
			break;
		}
		case pipeline_subobject_type::stream_output_state:
		case pipeline_subobject_type::primitive_topology:
		case pipeline_subobject_type::depth_stencil_format:
		case pipeline_subobject_type::render_target_formats:
		case pipeline_subobject_type::sample_mask:
		case pipeline_subobject_type::sample_count:
		case pipeline_subobject_type::viewport_count:
		case pipeline_subobject_type::dynamic_pipeline_states:
		case pipeline_subobject_type::max_vertex_count:
			break;
		}
	}


	const auto handle = trace_data.read<pipeline>().handle;

	if (s_pipelines.find(handle) != s_pipelines.end())
		device->destroy_pipeline(s_pipelines[handle]);

	if (!device->create_pipeline(s_pipeline_layouts[layout], subobject_count, subobjects.data(), &s_pipelines[handle]))
		assert(false);
}
static void play_destroy_pipeline(trace_data_read &trace_data, device *device)
{
	const auto handle = trace_data.read<pipeline>().handle;

	device->destroy_pipeline(s_pipelines[handle]);
	s_pipelines[handle] = {};
}

static void play_init_pipeline_layout(trace_data_read &trace_data, device *device)
{
	const auto param_count = trace_data.read<uint32_t>();

	std::vector<pipeline_layout_param> params(param_count);
	std::vector<std::vector<descriptor_range>> ranges(param_count);
	std::vector<std::vector<descriptor_range_with_static_samplers>> ranges_with_static_samplers(param_count);

	for (uint32_t i = 0; i < param_count; ++i)
	{
		params[i].type = trace_data.read<pipeline_layout_param_type>();

		switch (params[i].type)
		{
		case pipeline_layout_param_type::push_constants:
			params[i].push_constants = trace_data.read<decltype(params[i].push_constants)>();
			break;
		case pipeline_layout_param_type::push_descriptors:
			params[i].push_descriptors = trace_data.read<decltype(params[i].push_descriptors)>();
			break;
		case pipeline_layout_param_type::descriptor_table:
		case pipeline_layout_param_type::push_descriptors_with_ranges:
			params[i].descriptor_table.count = trace_data.read<uint32_t>();
			ranges[i].resize(params[i].descriptor_table.count);
			for (uint32_t k = 0; k < params[i].descriptor_table.count; ++k)
				ranges[i][k] = trace_data.read<descriptor_range>();
			params[i].descriptor_table.ranges = ranges[i].data();
			break;
		case pipeline_layout_param_type::descriptor_table_with_static_samplers:
		case pipeline_layout_param_type::push_descriptors_with_static_samplers:
			params[i].descriptor_table_with_static_samplers.count = trace_data.read<uint32_t>();
			ranges_with_static_samplers[i].resize(params[i].descriptor_table_with_static_samplers.count);
			for (uint32_t k = 0; k < params[i].descriptor_table_with_static_samplers.count; ++k)
				ranges_with_static_samplers[i][k] = trace_data.read<descriptor_range_with_static_samplers>();
			params[i].descriptor_table_with_static_samplers.ranges = ranges_with_static_samplers[i].data();
			break;
		}
	}

	const auto handle = trace_data.read<pipeline_layout>().handle;

	if (!device->create_pipeline_layout(param_count, params.data(), &s_pipeline_layouts[handle]))
		assert(false);
}
static void play_destroy_pipeline_layout(trace_data_read &trace_data, device *device)
{
	const auto handle = trace_data.read<pipeline_layout>().handle;

	device->destroy_pipeline_layout(s_pipeline_layouts[handle]);
	s_pipeline_layouts[handle] = {};
}

static void play_copy_descriptor_sets(trace_data_read &trace_data, device *device)
{
	const auto count = trace_data.read<uint32_t>();

	std::vector<descriptor_table_copy> copies(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		descriptor_table_copy &copy = copies[i];

		copy = trace_data.read<descriptor_table_copy>();

		// TODO: Create these tables somehow
		copy.source_table = s_descriptor_tables[copy.source_table.handle];
		copy.dest_table = s_descriptor_tables[copy.dest_table.handle];
	}

	device->copy_descriptor_tables(count, copies.data());
}
static void play_update_descriptor_sets(trace_data_read &trace_data, device *device)
{
	const auto count = trace_data.read<uint32_t>();

	std::vector<descriptor_table_update> updates(count);
	std::vector<std::vector<uint64_t>> descriptors(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		descriptor_table_update &update = updates[i];

		const auto table_handle = trace_data.read<descriptor_table>().handle;

		// TODO: Create this table somehow
		update.table = s_descriptor_tables[table_handle];
		update.binding = trace_data.read<uint32_t>();
		update.array_offset = trace_data.read<uint32_t>();
		update.count = trace_data.read<uint32_t>();
		update.type = trace_data.read<descriptor_type>();

		uint64_t handle[3] = {};
		descriptors[i].resize(update.count * 3);

		for (uint32_t k = 0; k < update.count; ++k)
		{
			switch (update.type)
			{
			case descriptor_type::sampler:
				trace_data.read(handle, sizeof(*handle));
				descriptors[i][k] = s_samplers[handle[0]].handle;
				break;
			case descriptor_type::shader_resource_view:
			case descriptor_type::unordered_access_view:
			case descriptor_type::shader_storage_buffer:
				trace_data.read(handle, sizeof(*handle));
				descriptors[i][k] = s_resource_views[handle[0]].handle;
				break;
			case descriptor_type::sampler_with_resource_view:
				trace_data.read(handle, sizeof(*handle) * 2);
				descriptors[i][k * 2 + 0] = s_samplers[handle[0]].handle;
				descriptors[i][k * 2 + 1] = s_resource_views[handle[1]].handle;
				break;
			case descriptor_type::constant_buffer:
				trace_data.read(handle, sizeof(*handle) * 3);
				descriptors[i][k * 3 + 0] = s_resources[handle[0]].handle;
				descriptors[i][k * 3 + 1] = handle[1];
				descriptors[i][k * 3 + 2] = handle[2];
				break;
			}
		}

		update.descriptors = descriptors.data();
	}

	device->update_descriptor_tables(count, updates.data());
}

static void play_map_buffer_region(trace_data_read &trace_data, device *device)
{
	trace_data.read<resource>();
	trace_data.read<uint64_t>();
	trace_data.read<uint64_t>();
	trace_data.read<map_access>();
}
static void play_unmap_buffer_region(trace_data_read &trace_data, device *device)
{
	const auto handle = trace_data.read<resource>().handle;
	const auto offset = trace_data.read<uint64_t>();
	const auto size = trace_data.read<uint64_t>();
	const auto access = trace_data.read<map_access>();

	if (access != map_access::read_only)
	{
		std::vector<uint8_t> data(static_cast<size_t>(size));
		trace_data.read(data.data(), static_cast<size_t>(size));

		if (s_resources[handle] == 0)
			return;

		void *mapped_data = nullptr;
		if (device->map_buffer_region(s_resources[handle], offset, size, access, &mapped_data))
		{
			std::memcpy(mapped_data, data.data(), data.size());
			device->unmap_buffer_region(s_resources[handle]);
		}
	}
}
static void play_map_texture_region(trace_data_read &trace_data, device *device)
{
	trace_data.read<resource>();
	trace_data.read<uint32_t>();
	const bool has_box = trace_data.read<bool>();
	has_box ? trace_data.read<subresource_box>() : subresource_box {};
	trace_data.read<map_access>();
}
static void play_unmap_texture_region(trace_data_read &trace_data, device *device)
{
	const auto handle = trace_data.read<resource>().handle;
	const auto subresource = trace_data.read<uint32_t>();
	const bool has_box = trace_data.read<bool>();
	const auto box = has_box ? trace_data.read<subresource_box>() : subresource_box {};
	const auto access = trace_data.read<map_access>();

	if (access != map_access::read_only)
	{
		std::vector<uint8_t> data(static_cast<size_t>(trace_data.read<uint64_t>()));
		trace_data.read(data.data(), data.size());

		if (s_resources[handle] == 0)
			return;

		subresource_data mapped_data = {};
		if (device->map_texture_region(s_resources[handle], subresource, has_box ? &box : nullptr, access, &mapped_data))
		{
			std::memcpy(mapped_data.data, data.data(), data.size());
			device->unmap_texture_region(s_resources[handle], subresource);
		}
	}
}
static void play_update_buffer_region(trace_data_read &trace_data, device *device)
{
	const auto handle = trace_data.read<resource>().handle;
	const auto offset = trace_data.read<uint64_t>();
	const auto size = trace_data.read<uint64_t>();

	std::vector<uint8_t> data(static_cast<size_t>(size));
	trace_data.read(data.data(), data.size());

	if (data.empty() || s_resources[handle] == 0)
		return;

	device->update_buffer_region(data.data(), s_resources[handle], offset, size);
}
static void play_update_texture_region(trace_data_read &trace_data, device *device)
{
	const auto handle = trace_data.read<resource>().handle;
	const auto subresource = trace_data.read<uint32_t>();
	const bool has_box = trace_data.read<bool>();
	const auto box = has_box ? trace_data.read<subresource_box>() : subresource_box {};

	subresource_data subresource_data = {};
	subresource_data.row_pitch = trace_data.read<uint32_t>();
	subresource_data.slice_pitch = trace_data.read<uint32_t>();

	std::vector<uint8_t> data(static_cast<size_t>(trace_data.read<uint64_t>()));
	trace_data.read(data.data(), data.size());

	if (data.empty() || s_resources[handle] == 0)
		return;

	subresource_data.data = data.data();

	device->update_texture_region(subresource_data, s_resources[handle], subresource, has_box ? &box : nullptr);
}

static void play_barrier(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto count = trace_data.read<uint32_t>();

	std::vector<resource> resources(count);
	std::vector<resource_usage> old_states(count);
	std::vector<resource_usage> new_states(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		const auto handle = trace_data.read<resource>().handle;

		resources[i] = s_resources[handle];
		old_states[i] = trace_data.read<resource_usage>();
		new_states[i] = trace_data.read<resource_usage>();
	}

	cmd_list->barrier(count, resources.data(), old_states.data(), new_states.data());
}

static void play_begin_render_pass(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto count = trace_data.read<uint32_t>();

	std::vector<render_pass_render_target_desc> rts(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		rts[i] = trace_data.read<render_pass_render_target_desc>();
		rts[i].view = s_resource_views[rts[i].view.handle];
	}

	const bool has_ds = trace_data.read<bool>();

	render_pass_depth_stencil_desc ds;
	if (has_ds)
	{
		ds = trace_data.read<render_pass_depth_stencil_desc>();
		ds.view = s_resource_views[ds.view.handle];
	}

	cmd_list->begin_render_pass(count, rts.data(), has_ds ? &ds : nullptr);
}
static void play_end_render_pass(trace_data_read &trace_data, command_list *cmd_list)
{
	cmd_list->end_render_pass();
}
static void play_bind_render_targets_and_depth_stencil(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto count = trace_data.read<uint32_t>();

	std::vector<resource_view> rtvs(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		const auto rtv_handle = trace_data.read<resource_view>().handle;

		rtvs[i] = s_resource_views[rtv_handle];
	}

	const auto dsv_handle = trace_data.read<resource_view>().handle;

	cmd_list->bind_render_targets_and_depth_stencil(count, rtvs.data(), s_resource_views[dsv_handle]);
}

static void play_bind_pipeline(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto stages = trace_data.read<pipeline_stage>();
	const auto handle = trace_data.read<pipeline>().handle;

	cmd_list->bind_pipeline(stages, s_pipelines[handle]);
}
static void play_bind_pipeline_states(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto count = trace_data.read<uint32_t>();

	std::vector<dynamic_state> states(count);
	std::vector<uint32_t> values(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		states[i] = trace_data.read<dynamic_state>();
		values[i] = trace_data.read<uint32_t>();
	}

	cmd_list->bind_pipeline_states(count, states.data(), values.data());
}
static void play_bind_viewports(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto first = trace_data.read<uint32_t>();
	const auto count = trace_data.read<uint32_t>();

	std::vector<viewport> viewports(count);

	for (uint32_t i = 0; i < count; ++i)
		viewports[i] = trace_data.read<viewport>();

	cmd_list->bind_viewports(first, count, viewports.data());
}
static void play_bind_scissor_rects(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto first = trace_data.read<uint32_t>();
	const auto count = trace_data.read<uint32_t>();

	std::vector<rect> rects(count);

	for (uint32_t i = 0; i < count; ++i)
		rects[i] = trace_data.read<rect>();

	cmd_list->bind_scissor_rects(first, count, rects.data());
}
static void play_push_constants(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto stages = trace_data.read<shader_stage>();
	const auto layout = trace_data.read<pipeline_layout>().handle;
	const auto param = trace_data.read<uint32_t>();

	const auto first = trace_data.read<uint32_t>();
	const auto count = trace_data.read<uint32_t>();

	std::vector<uint32_t> values(count);

	for (uint32_t i = 0; i < count; ++i)
		values[i] = trace_data.read<uint32_t>();

	cmd_list->push_constants(stages, s_pipeline_layouts[layout], param, first, count, values.data());
}
static void play_push_descriptors(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto stages = trace_data.read<shader_stage>();
	const auto layout = trace_data.read<pipeline_layout>().handle;
	const auto param = trace_data.read<uint32_t>();

	descriptor_table_update update = {};
	update.binding = trace_data.read<uint32_t>();
	update.array_offset = trace_data.read<uint32_t>();
	update.count = trace_data.read<uint32_t>();
	update.type = trace_data.read<descriptor_type>();

	uint64_t handle[3] = {};
	std::vector<uint64_t> descriptors(update.count * 3);

	for (uint32_t i = 0; i < update.count; ++i)
	{
		switch (update.type)
		{
		case descriptor_type::sampler:
			trace_data.read(handle, sizeof(*handle));
			descriptors[i] = s_samplers[handle[0]].handle;
			break;
		case descriptor_type::shader_resource_view:
		case descriptor_type::unordered_access_view:
		case descriptor_type::shader_storage_buffer:
			trace_data.read(handle, sizeof(*handle));
			descriptors[i] = s_resource_views[handle[0]].handle;
			break;
		case descriptor_type::sampler_with_resource_view:
			trace_data.read(handle, sizeof(*handle) * 2);
			descriptors[i * 2 + 0] = s_samplers[handle[0]].handle;
			descriptors[i * 2 + 1] = s_resource_views[handle[1]].handle;
			break;
		case descriptor_type::constant_buffer:
			trace_data.read(handle, sizeof(*handle) * 3);
			descriptors[i * 3 + 0] = s_resources[handle[0]].handle;
			descriptors[i * 3 + 1] = handle[1];
			descriptors[i * 3 + 2] = handle[2];
			break;
		}
	}

	update.descriptors = descriptors.data();

	cmd_list->push_descriptors(stages, s_pipeline_layouts[layout], param, update);
}
static void play_bind_descriptor_tables(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto stages = trace_data.read<shader_stage>();
	const auto layout = trace_data.read<pipeline_layout>().handle;
	const auto first = trace_data.read<uint32_t>();
	const auto count = trace_data.read<uint32_t>();

	std::vector<descriptor_table> tables(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		const auto set = trace_data.read<descriptor_table>().handle;

		tables[i] = s_descriptor_tables[set];
	}

	cmd_list->bind_descriptor_tables(stages, s_pipeline_layouts[layout], first, count, tables.data());
}
static void play_bind_index_buffer(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto handle = trace_data.read<resource>().handle;
	const auto offset = trace_data.read<uint64_t>();
	const auto index_size = trace_data.read<uint32_t>();

	cmd_list->bind_index_buffer(s_resources[handle], offset, index_size);
}
static void play_bind_vertex_buffers(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto first = trace_data.read<uint32_t>();
	const auto count = trace_data.read<uint32_t>();

	std::vector<resource> buffers(count);
	std::vector<uint64_t> offsets(count);
	std::vector<uint32_t> strides(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		const auto handle = trace_data.read<resource>().handle;

		buffers[i] = s_resources[handle];
		offsets[i] = trace_data.read<uint64_t>();
		strides[i] = trace_data.read<uint32_t>();
	}

	cmd_list->bind_vertex_buffers(first, count, buffers.data(), offsets.data(), strides.data());
}
static void play_bind_stream_output_buffers(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto first = trace_data.read<uint32_t>();
	const auto count = trace_data.read<uint32_t>();

	std::vector<resource> buffers(count);
	std::vector<uint64_t> offsets(count);
	std::vector<uint64_t> max_sizes(count);
	std::vector<resource> counter_buffers(count);
	std::vector<uint64_t> counter_offsets(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		const auto handle = trace_data.read<resource>().handle;

		buffers[i] = s_resources[handle];
		offsets[i] = trace_data.read<uint64_t>();
		max_sizes[i] = trace_data.read<uint64_t>();

		const auto counter_handle = trace_data.read<resource>().handle;

		counter_buffers[i] = s_resources[counter_handle];
		counter_offsets[i] = trace_data.read<uint64_t>();
	}

	cmd_list->bind_stream_output_buffers(first, count, buffers.data(), offsets.data(), max_sizes.data(), counter_buffers.data(), counter_offsets.data());
}

static void play_draw(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto vertex_count = trace_data.read<uint32_t>();
	const auto instance_count = trace_data.read<uint32_t>();
	const auto first_vertex = trace_data.read<uint32_t>();
	const auto first_instance = trace_data.read<uint32_t>();

	cmd_list->draw(vertex_count, instance_count, first_vertex, first_instance);
}
static void play_draw_indexed(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto index_count = trace_data.read<uint32_t>();
	const auto instance_count = trace_data.read<uint32_t>();
	const auto first_index = trace_data.read<uint32_t>();
	const auto vertex_offset = trace_data.read<int32_t>();
	const auto first_instance = trace_data.read<uint32_t>();

	cmd_list->draw_indexed(index_count, instance_count, first_index, vertex_offset, first_instance);
}
static void play_dispatch(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto group_count_x = trace_data.read<uint32_t>();
	const auto group_count_y = trace_data.read<uint32_t>();
	const auto group_count_z = trace_data.read<uint32_t>();

	cmd_list->dispatch(group_count_x, group_count_y, group_count_z);
}
static void play_draw_or_dispatch_indirect(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto type = trace_data.read<indirect_command>();
	const auto handle = trace_data.read<resource>().handle;
	const auto offset = trace_data.read<uint64_t>();
	const auto draw_count = trace_data.read<uint32_t>();
	const auto stride = trace_data.read<uint32_t>();

	cmd_list->draw_or_dispatch_indirect(type, s_resources[handle], offset, draw_count, stride);
}

static void play_copy_resource(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto src_handle = trace_data.read<resource>().handle;
	const auto dst_handle = trace_data.read<resource>().handle;

	cmd_list->copy_resource(s_resources[src_handle], s_resources[dst_handle]);
}
static void play_copy_buffer_region(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto src_handle = trace_data.read<resource>().handle;
	const auto src_offset = trace_data.read<uint64_t>();
	const auto dst_handle = trace_data.read<resource>().handle;
	const auto dst_offset = trace_data.read<uint64_t>();
	const auto size = trace_data.read<uint64_t>();

	cmd_list->copy_buffer_region(s_resources[src_handle], src_offset, s_resources[dst_handle], dst_offset, size);
}
static void play_copy_buffer_to_texture(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto src_handle = trace_data.read<resource>().handle;
	const auto src_offset = trace_data.read<uint64_t>();
	const auto row_length = trace_data.read<uint32_t>();
	const auto slice_height = trace_data.read<uint32_t>();
	const auto dst_handle = trace_data.read<resource>().handle;
	const auto dst_subresource = trace_data.read<uint32_t>();
	const bool has_dst_box = trace_data.read<bool>();
	const auto dst_box = has_dst_box ? trace_data.read<subresource_box>() : subresource_box {};

	cmd_list->copy_buffer_to_texture(s_resources[src_handle], src_offset, row_length, slice_height, s_resources[dst_handle], dst_subresource, has_dst_box ? &dst_box : nullptr);
}
static void play_copy_texture_region(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto src_handle = trace_data.read<resource>().handle;
	const auto src_subresource = trace_data.read<uint32_t>();
	const bool has_src_box = trace_data.read<bool>();
	const auto src_box = has_src_box ? trace_data.read<subresource_box>() : subresource_box {};
	const auto dst_handle = trace_data.read<resource>().handle;
	const auto dst_subresource = trace_data.read<uint32_t>();
	const bool has_dst_box = trace_data.read<bool>();
	const auto dst_box = has_dst_box ? trace_data.read<subresource_box>() : subresource_box {};
	const auto filter = trace_data.read<filter_mode>();

	cmd_list->copy_texture_region(s_resources[src_handle], src_subresource, has_src_box ? &src_box : nullptr, s_resources[dst_handle], dst_subresource, has_dst_box ? &dst_box : nullptr, filter);
}
static void play_copy_texture_to_buffer(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto src_handle = trace_data.read<resource>().handle;
	const auto src_subresource = trace_data.read<uint32_t>();
	const bool has_src_box = trace_data.read<bool>();
	const auto src_box = has_src_box ? trace_data.read<subresource_box>() : subresource_box {};
	const auto dst_handle = trace_data.read<resource>().handle;
	const auto dst_offset = trace_data.read<uint64_t>();
	const auto row_length = trace_data.read<uint32_t>();
	const auto slice_height = trace_data.read<uint32_t>();

	cmd_list->copy_texture_to_buffer(s_resources[src_handle], src_subresource, has_src_box ? &src_box : nullptr, s_resources[dst_handle], dst_offset, row_length, slice_height);
}
static void play_resolve_texture_region(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto src_handle = trace_data.read<resource>().handle;
	const auto src_subresource = trace_data.read<uint32_t>();
	const bool has_src_box = trace_data.read<bool>();
	const auto src_box = has_src_box ? trace_data.read<subresource_box>() : subresource_box {};
	const auto dst_handle = trace_data.read<resource>().handle;
	const auto dst_subresource = trace_data.read<uint32_t>();
	const auto dst_x = trace_data.read<int32_t>();
	const auto dst_y = trace_data.read<int32_t>();
	const auto dst_z = trace_data.read<int32_t>();
	const auto resolve_format = trace_data.read<format>();

	cmd_list->resolve_texture_region(s_resources[src_handle], src_subresource, has_src_box ? &src_box : nullptr, s_resources[dst_handle], dst_subresource, dst_x, dst_y, dst_z, resolve_format);
}

static void play_clear_depth_stencil_view(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto dsv_handle = trace_data.read<resource_view>().handle;
	const bool has_depth = trace_data.read<bool>();
	const auto depth = has_depth ? trace_data.read<float>() : 0.0f;
	const bool has_stencil = trace_data.read<bool>();
	const auto stencil = has_stencil ? trace_data.read<uint8_t>() : uint8_t(0);

	cmd_list->clear_depth_stencil_view(s_resource_views[dsv_handle], has_depth ? &depth : nullptr, has_stencil ? &stencil : nullptr);
}
static void play_clear_render_target_view(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto rtv_handle = trace_data.read<resource_view>().handle;
	float color[4] = {};
	trace_data.read(color, sizeof(color));

	cmd_list->clear_render_target_view(s_resource_views[rtv_handle], color);
}
static void play_clear_unordered_access_view_uint(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto uav_handle = trace_data.read<resource_view>().handle;
	uint32_t values[4] = {};
	trace_data.read(values, sizeof(values));

	cmd_list->clear_unordered_access_view_uint(s_resource_views[uav_handle], values);
}
static void play_clear_unordered_access_view_float(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto uav_handle = trace_data.read<resource_view>().handle;
	float values[4] = {};
	trace_data.read(values, sizeof(values));

	cmd_list->clear_unordered_access_view_float(s_resource_views[uav_handle], values);
}

static void play_generate_mipmaps(trace_data_read &trace_data, command_list *cmd_list)
{
	const auto srv_handle = trace_data.read<resource_view>().handle;

	cmd_list->generate_mipmaps(s_resource_views[srv_handle]);
}

bool play_frame(trace_data_read &trace_data, command_list *cmd_list, effect_runtime *runtime)
{
	device *const device = cmd_list->get_device();

	for (reshade::addon_event ev; trace_data.read(&ev, sizeof(ev));)
	{
		switch (ev)
		{
		case reshade::addon_event::init_swapchain:
			play_init_swapchain(trace_data, runtime);
			break;
		case reshade::addon_event::destroy_swapchain:
			play_destroy_swapchain(trace_data, runtime);
			break;

		case reshade::addon_event::init_sampler:
			play_init_sampler(trace_data, device);
			break;
		case reshade::addon_event::destroy_sampler:
			play_destroy_sampler(trace_data, device);
			break;
		case reshade::addon_event::init_resource:
			play_init_resource(trace_data, device);
			break;
		case reshade::addon_event::destroy_resource:
			play_destroy_resource(trace_data, device);
			break;
		case reshade::addon_event::init_resource_view:
			play_init_resource_view(trace_data, device);
			break;
		case reshade::addon_event::destroy_resource_view:
			play_destroy_resource_view(trace_data, device);
			break;

		case reshade::addon_event::map_buffer_region:
			play_map_buffer_region(trace_data, device);
			break;
		case reshade::addon_event::unmap_buffer_region:
			play_unmap_buffer_region(trace_data, device);
			break;
		case reshade::addon_event::map_texture_region:
			play_map_texture_region(trace_data, device);
			break;
		case reshade::addon_event::unmap_texture_region:
			play_unmap_texture_region(trace_data, device);
			break;
		case reshade::addon_event::update_buffer_region:
			play_update_buffer_region(trace_data, device);
			break;
		case reshade::addon_event::update_texture_region:
			play_update_texture_region(trace_data, device);
			break;

		case reshade::addon_event::init_pipeline:
			play_init_pipeline(trace_data, device);
			break;
		case reshade::addon_event::destroy_pipeline:
			play_destroy_pipeline(trace_data, device);
			break;
		case reshade::addon_event::init_pipeline_layout:
			play_init_pipeline_layout(trace_data, device);
			break;
		case reshade::addon_event::destroy_pipeline_layout:
			play_destroy_pipeline_layout(trace_data, device);
			break;

		case reshade::addon_event::copy_descriptor_tables:
			play_copy_descriptor_sets(trace_data, device);
			break;
		case reshade::addon_event::update_descriptor_tables:
			play_update_descriptor_sets(trace_data, device);
			break;

		case reshade::addon_event::init_query_heap:
			break;
		case reshade::addon_event::destroy_query_heap:
			break;
		case reshade::addon_event::get_query_heap_results:
			break;

		case reshade::addon_event::barrier:
			play_barrier(trace_data, cmd_list);
			break;
		case reshade::addon_event::begin_render_pass:
			play_begin_render_pass(trace_data, cmd_list);
			break;
		case reshade::addon_event::end_render_pass:
			play_end_render_pass(trace_data, cmd_list);
			break;
		case reshade::addon_event::bind_render_targets_and_depth_stencil:
			play_bind_render_targets_and_depth_stencil(trace_data, cmd_list);
			break;
		case reshade::addon_event::bind_pipeline:
			play_bind_pipeline(trace_data, cmd_list);
			break;
		case reshade::addon_event::bind_pipeline_states:
			play_bind_pipeline_states(trace_data, cmd_list);
			break;
		case reshade::addon_event::bind_viewports:
			play_bind_viewports(trace_data, cmd_list);
			break;
		case reshade::addon_event::bind_scissor_rects:
			play_bind_scissor_rects(trace_data, cmd_list);
			break;
		case reshade::addon_event::push_constants:
			play_push_constants(trace_data, cmd_list);
			break;
		case reshade::addon_event::push_descriptors:
			play_push_descriptors(trace_data, cmd_list);
			break;
		case reshade::addon_event::bind_descriptor_tables:
			play_bind_descriptor_tables(trace_data, cmd_list);
			break;
		case reshade::addon_event::bind_index_buffer:
			play_bind_index_buffer(trace_data, cmd_list);
			break;
		case reshade::addon_event::bind_vertex_buffers:
			play_bind_vertex_buffers(trace_data, cmd_list);
			break;
		case reshade::addon_event::bind_stream_output_buffers:
			play_bind_stream_output_buffers(trace_data, cmd_list);
			break;
		case reshade::addon_event::draw:
			play_draw(trace_data, cmd_list);
			break;
		case reshade::addon_event::draw_indexed:
			play_draw_indexed(trace_data, cmd_list);
			break;
		case reshade::addon_event::dispatch:
			play_dispatch(trace_data, cmd_list);
			break;
		case reshade::addon_event::draw_or_dispatch_indirect:
			play_draw_or_dispatch_indirect(trace_data, cmd_list);
			break;
		case reshade::addon_event::copy_resource:
			play_copy_resource(trace_data, cmd_list);
			break;
		case reshade::addon_event::copy_buffer_region:
			play_copy_buffer_region(trace_data, cmd_list);
			break;
		case reshade::addon_event::copy_buffer_to_texture:
			play_copy_buffer_to_texture(trace_data, cmd_list);
			break;
		case reshade::addon_event::copy_texture_region:
			play_copy_texture_region(trace_data, cmd_list);
			break;
		case reshade::addon_event::copy_texture_to_buffer:
			play_copy_texture_to_buffer(trace_data, cmd_list);
			break;
		case reshade::addon_event::resolve_texture_region:
			play_resolve_texture_region(trace_data, cmd_list);
			break;
		case reshade::addon_event::clear_depth_stencil_view:
			play_clear_depth_stencil_view(trace_data, cmd_list);
			break;
		case reshade::addon_event::clear_render_target_view:
			play_clear_render_target_view(trace_data, cmd_list);
			break;
		case reshade::addon_event::clear_unordered_access_view_uint:
			play_clear_unordered_access_view_uint(trace_data, cmd_list);
			break;
		case reshade::addon_event::clear_unordered_access_view_float:
			play_clear_unordered_access_view_float(trace_data, cmd_list);
			break;
		case reshade::addon_event::generate_mipmaps:
			play_generate_mipmaps(trace_data, cmd_list);
			break;
		case reshade::addon_event::begin_query:
			break;
		case reshade::addon_event::end_query:
			break;
		case reshade::addon_event::copy_query_heap_results:
			break;

		case reshade::addon_event::reset_command_list:
			break;
		case reshade::addon_event::close_command_list:
			break;
		case reshade::addon_event::execute_command_list:
			break;
		case reshade::addon_event::execute_secondary_command_list:
			break;

		case reshade::addon_event::present:
			return true;

		default:
			assert(false);
			break;
		}
	}

	return false;
}
