/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "trace_data.hpp"
#include <reshade.hpp>
#include <vector>
#include <algorithm>
#include <shared_mutex>

using namespace reshade::api;

struct __declspec(uuid("589E9521-a7c5-4e07-9c64-1175b0cf3ab4")) device_data : trace_data_write
{
	static inline unsigned int index = 0;

	device_data(device_api graphics_api) : trace_data_write(("api_trace_log" + (++index > 1 ? "_" + std::to_string(index) : "") + ".bin").c_str())
	{
		constexpr uint64_t MAGIC = (uint64_t('A') << 0) | (uint64_t('P') << 8) | (uint64_t('I') << 16) | (uint64_t('T') << 24) | (uint64_t('R') << 32) | (uint64_t('A') << 40) | (uint64_t('C') << 48) | (uint64_t('E') << 56);
		write(MAGIC);
		write(graphics_api);
	}
};

static std::shared_mutex s_mutex;

struct mapping
{
	resource resource;
	uint64_t offset;
	uint64_t size;
	uint32_t subresource;
	bool has_box;
	subresource_box box;
	map_access access;
	subresource_data data;
	uint64_t ref = 1;
};
static std::vector<mapping> mappings;

static inline uint64_t calc_texture_size(const resource_desc &desc, uint32_t subresource, const subresource_data &data, const subresource_box *box = nullptr)
{
	const uint32_t level = (desc.texture.levels != 0) ? subresource % desc.texture.levels : subresource;

	switch (desc.type)
	{
	case resource_type::texture_1d:
		return format_row_pitch(desc.texture.format,
			box != nullptr ? box->width() : std::max(desc.texture.width >> level, 1u));
	case resource_type::texture_2d:
		assert(data.row_pitch != 0);
		return format_slice_pitch(desc.texture.format, data.row_pitch,
			box != nullptr ? box->height() : std::max(desc.texture.height >> level, 1u));
	case resource_type::texture_3d:
		assert(data.slice_pitch != 0);
		return data.slice_pitch * (box != nullptr ? box->depth() : desc.texture.depth_or_layers);
	default:
		return 0;
	}
}

static void on_init_device(device *device)
{
	device->create_private_data<device_data>(device->get_api());
}
static void on_destroy_device(device *device)
{
	device->destroy_private_data<device_data>();
}

static void on_init_command_list(command_list *cmd_list)
{
}
static void on_destroy_command_list(command_list *cmd_list)
{
}

static void on_init_swapchain(swapchain *swapchain)
{
	device *const device = swapchain->get_device();

	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::init_swapchain);
	const uint32_t buffer_count = swapchain->get_back_buffer_count();
	trace_data.write(buffer_count);
	for (uint32_t i = 0; i < buffer_count; ++i)
		trace_data.write(swapchain->get_back_buffer(i));
}
static void on_destroy_swapchain(swapchain *swapchain)
{
	device *const device = swapchain->get_device();

	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::destroy_swapchain);
	const uint32_t buffer_count = swapchain->get_back_buffer_count();
	trace_data.write(buffer_count);
	for (uint32_t i = 0; i < buffer_count; ++i)
		trace_data.write(swapchain->get_back_buffer(i));
}

static void on_init_sampler(device *device, const sampler_desc &desc, sampler handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::init_sampler);
	trace_data.write(desc);
	trace_data.write(handle);
}
static void on_destroy_sampler(device *device, sampler handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::destroy_sampler);
	trace_data.write(handle);
}

static void on_init_resource(device *device, const resource_desc &desc, const subresource_data *initial_data, resource_usage initial_state, resource handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::init_resource);
	trace_data.write(desc);
	trace_data.write(initial_state);
	trace_data.write(handle);

	if (desc.type == resource_type::buffer)
	{
		const uint32_t subresources = (initial_data != nullptr) ? 1 : 0;
		trace_data.write(subresources);

		if (subresources)
		{
			const subresource_data &subresource_data = *initial_data;

			trace_data.write(subresource_data.data, static_cast<size_t>(desc.buffer.size));
		}
	}
	else
	{
		const uint32_t levels = (desc.texture.levels != 0) ? desc.texture.levels : 1;
		const uint32_t layers = (desc.type != resource_type::texture_3d) ? desc.texture.depth_or_layers : 1;

		const uint32_t subresources = (initial_data != nullptr) ? levels * layers : 0;
		trace_data.write(subresources);

		for (uint32_t layer = 0; layer < layers; ++layer)
		{
			for (uint32_t level = 0; level < levels; ++level)
			{
				const uint32_t subresource = layer * levels + level;
				if (subresource >= subresources)
					break;

				const subresource_data &subresource_data = initial_data[subresource];
				trace_data.write(subresource_data.row_pitch);
				trace_data.write(subresource_data.slice_pitch);

				const uint64_t size = calc_texture_size(desc, subresource, subresource_data);
				trace_data.write(size);
				trace_data.write(subresource_data.data, static_cast<size_t>(size));
			}
		}
	}
}
static void on_destroy_resource(device *device, resource handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::destroy_resource);
	trace_data.write(handle);
}

static void on_init_resource_view(device *device, resource resource, resource_usage usage_type, const resource_view_desc &desc, resource_view handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::init_resource_view);
	trace_data.write(resource);
	trace_data.write(usage_type);
	trace_data.write(desc);
	trace_data.write(handle);
}
static void on_destroy_resource_view(device *device, resource_view handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::destroy_resource_view);
	trace_data.write(handle);
}

static void on_init_pipeline(device *device, pipeline_layout layout, uint32_t subobject_count, const pipeline_subobject *subobjects, pipeline handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::init_pipeline);
	trace_data.write(layout);
	trace_data.write(subobject_count);

	for (uint32_t i = 0; i < subobject_count; ++i)
	{
		trace_data.write(subobjects[i].type);

		switch (subobjects[i].type)
		{
		case pipeline_subobject_type::vertex_shader:
		case pipeline_subobject_type::hull_shader:
		case pipeline_subobject_type::domain_shader:
		case pipeline_subobject_type::geometry_shader:
		case pipeline_subobject_type::pixel_shader:
		case pipeline_subobject_type::compute_shader:
		{
			assert(subobjects[i].count == 1);
			const auto desc = static_cast<const shader_desc *>(subobjects[i].data);

			const uint64_t code_size = desc->code_size;
			trace_data.write(code_size);
			trace_data.write(desc->code, static_cast<size_t>(code_size));

			const uint32_t entry_point_length = desc->entry_point != nullptr ? static_cast<uint32_t>(strlen(desc->entry_point)) : 0;
			trace_data.write(entry_point_length);
			trace_data.write(desc->entry_point, entry_point_length);
			break;
		}
		case pipeline_subobject_type::input_layout:
		{
			trace_data.write(subobjects[i].count);
			const auto desc = static_cast<const input_element *>(subobjects[i].data);

			for (uint32_t k = 0; k < subobjects[i].count; ++k)
			{
				trace_data.write(desc[k].location);

				const uint32_t semantic_length = desc[k].semantic != nullptr ? static_cast<uint32_t>(strlen(desc[k].semantic)) : 0;
				trace_data.write(semantic_length);
				trace_data.write(desc[k].semantic, semantic_length);
				trace_data.write(desc[k].semantic_index);

				trace_data.write(desc[k].format);
				trace_data.write(desc[k].buffer_binding);
				trace_data.write(desc[k].offset);
				trace_data.write(desc[k].stride);
				trace_data.write(desc[k].instance_step_rate);
			}
			break;
		}
		case pipeline_subobject_type::blend_state:
		{
			assert(subobjects[i].count == 1);
			const auto desc = static_cast<const blend_desc *>(subobjects[i].data);

			trace_data.write(*desc);
			break;
		}
		case pipeline_subobject_type::rasterizer_state:
		{
			assert(subobjects[i].count == 1);
			const auto desc = static_cast<const rasterizer_desc *>(subobjects[i].data);

			trace_data.write(*desc);
			break;
		}
		case pipeline_subobject_type::depth_stencil_state:
		{
			assert(subobjects[i].count == 1);
			const auto desc = static_cast<const depth_stencil_desc *>(subobjects[i].data);

			trace_data.write(*desc);
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

	trace_data.write(handle.handle);
}
static void on_destroy_pipeline(device *device, pipeline handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::destroy_pipeline);
	trace_data.write(handle);
}

static void on_init_pipeline_layout(device *device, uint32_t param_count, const pipeline_layout_param *params, pipeline_layout handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::init_pipeline_layout);
	trace_data.write(param_count);
	for (uint32_t i = 0; i < param_count; ++i)
	{
		trace_data.write(params[i].type);

		switch (params[i].type)
		{
		case pipeline_layout_param_type::push_constants:
			trace_data.write(params[i].push_constants);
			break;
		case pipeline_layout_param_type::push_descriptors:
			trace_data.write(params[i].push_descriptors);
			break;
		case pipeline_layout_param_type::descriptor_table:
		case pipeline_layout_param_type::push_descriptors_with_ranges:
			trace_data.write(params[i].descriptor_table.count);
			for (uint32_t k = 0; k < params[i].descriptor_table.count; ++k)
				trace_data.write(params[i].descriptor_table.ranges[k]);
			break;
		case pipeline_layout_param_type::descriptor_table_with_static_samplers:
		case pipeline_layout_param_type::push_descriptors_with_static_samplers:
			trace_data.write(params[i].descriptor_table_with_static_samplers.count);
			for (uint32_t k = 0; k < params[i].descriptor_table_with_static_samplers.count; ++k)
				trace_data.write(params[i].descriptor_table_with_static_samplers.ranges[k]);
			break;
		}
	}

	trace_data.write(handle);
}
static void on_destroy_pipeline_layout(device *device, pipeline_layout handle)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::destroy_pipeline_layout);
	trace_data.write(handle);
}

static bool on_copy_descriptor_tables(device *device, uint32_t count, const descriptor_table_copy *copies)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::copy_descriptor_tables);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
		trace_data.write(copies[i]);

	return false;
}
static bool on_update_descriptor_tables(device *device, uint32_t count, const descriptor_table_update *updates)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::update_descriptor_tables);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		const descriptor_table_update &update = updates[i];

		trace_data.write(update.table);
		trace_data.write(update.binding);
		trace_data.write(update.array_offset);
		trace_data.write(update.count);
		trace_data.write(update.type);

		switch (update.type)
		{
		case descriptor_type::sampler:
		case descriptor_type::shader_resource_view:
		case descriptor_type::unordered_access_view:
		case descriptor_type::shader_storage_buffer:
			trace_data.write(static_cast<const uint64_t *>(update.descriptors) + i * 1, sizeof(uint64_t) * 1);
			break;
		case descriptor_type::sampler_with_resource_view:
			trace_data.write(static_cast<const uint64_t *>(update.descriptors) + i * 2, sizeof(uint64_t) * 2);
			break;
		case descriptor_type::constant_buffer:
			trace_data.write(static_cast<const uint64_t *>(update.descriptors) + i * 3, sizeof(uint64_t) * 3);
			break;
		}
	}

	return false;
}

static void on_map_buffer_region(device *device, resource resource, uint64_t offset, uint64_t size, map_access access, void **data)
{
	if (UINT64_MAX == size)
		size = device->get_resource_desc(resource).buffer.size;

	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::map_buffer_region);
	trace_data.write(resource);
	trace_data.write(offset);
	trace_data.write(size);
	trace_data.write(access);

	mappings.push_back({ resource, offset, size, 0, false, subresource_box {}, access, subresource_data { *data } });
}
static void on_unmap_buffer_region(device *device, resource resource)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::unmap_buffer_region);
	trace_data.write(resource);

	const auto mapping_it = std::find_if(mappings.begin(), mappings.end(), [resource](const auto &mapping) { return mapping.resource == resource; });
	assert(mapping_it != mappings.end());
	const mapping &mapping = *mapping_it;

	trace_data.write(mapping.offset);
	trace_data.write(mapping.size);
	trace_data.write(mapping.access);

	if (mapping.access != map_access::read_only)
	{
		assert(mapping.size <= std::numeric_limits<size_t>::max());
		trace_data.write(mapping.data.data, static_cast<size_t>(mapping.size));
	}

	mappings.erase(mapping_it);
}
static void on_map_texture_region(device *device, resource resource, uint32_t subresource, const subresource_box *box, map_access access, subresource_data *data)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::map_texture_region);
	trace_data.write(resource);
	trace_data.write(subresource);
	const bool has_box = box != nullptr;
	trace_data.write(has_box);
	if (has_box)
		trace_data.write(*box);
	trace_data.write(access);

	const auto mapping_it = std::find_if(mappings.begin(), mappings.end(), [resource, subresource](const auto &mapping) { return mapping.resource == resource && mapping.subresource == subresource; });
	if (mapping_it != mappings.end() && mapping_it->data.data == data->data)
	{
		mapping_it->ref++;
	}
	else
	{
		mappings.push_back({ resource, 0, 0, subresource, has_box, has_box ? *box : subresource_box {}, access, *data });
	}
}
static void on_unmap_texture_region(device *device, resource resource, uint32_t subresource)
{
	const auto desc = device->get_resource_desc(resource);

	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::unmap_texture_region);
	trace_data.write(resource);
	trace_data.write(subresource);

	const auto mapping_it = std::find_if(mappings.rbegin(), mappings.rend(), [resource, subresource](const auto &mapping) { return mapping.resource == resource && mapping.subresource == subresource; });
	assert(mapping_it != mappings.rend());
	const mapping &mapping = *mapping_it;

	trace_data.write(mapping.has_box);
	if (mapping.has_box)
		trace_data.write(mapping.box);
	trace_data.write(mapping.access);

	if (mapping.access != map_access::read_only)
	{
		const uint64_t size = calc_texture_size(desc, subresource, mapping.data, mapping.has_box ? &mapping.box : nullptr);
		trace_data.write(size);
		assert(size <= std::numeric_limits<size_t>::max());
		trace_data.write(mapping.data.data, static_cast<size_t>(size));
	}

	if (--mapping_it->ref == 0)
		mappings.erase((mapping_it + 1).base());

}
static bool on_update_buffer_region(device *device, const void *data, resource resource, uint64_t offset, uint64_t size)
{
	if (UINT64_MAX == size)
		size = device->get_resource_desc(resource).buffer.size;

	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::update_buffer_region);
	trace_data.write(resource);
	trace_data.write(offset);
	trace_data.write(size);

	assert(size <= std::numeric_limits<size_t>::max());
	trace_data.write(data, static_cast<size_t>(size));

	return false;
}
static bool on_update_texture_region(device *device, const subresource_data &data, resource resource, uint32_t subresource, const subresource_box *box)
{
	const auto desc = device->get_resource_desc(resource);

	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::update_texture_region);
	trace_data.write(resource);
	trace_data.write(subresource);
	const bool has_box = box != nullptr;
	trace_data.write(has_box);
	if (has_box)
		trace_data.write(*box);

	trace_data.write(data.row_pitch);
	trace_data.write(data.slice_pitch);

	const uint64_t size = calc_texture_size(desc, subresource, data, box);
	trace_data.write(size);
	assert(size <= std::numeric_limits<size_t>::max());
	trace_data.write(data.data, static_cast<size_t>(size));

	return false;
}

static void on_barrier(command_list *cmd_list, uint32_t count, const resource *resources, const resource_usage *old_states, const resource_usage *new_states)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::barrier);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		trace_data.write(resources[i]);
		trace_data.write(old_states[i]);
		trace_data.write(new_states[i]);
	}
}

static void on_begin_render_pass(command_list *cmd_list, uint32_t count, const render_pass_render_target_desc *rts, const render_pass_depth_stencil_desc *ds)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::begin_render_pass);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		trace_data.write(rts[i]);
	}
	const bool has_ds = ds != nullptr;
	trace_data.write(has_ds);
	if (has_ds)
		trace_data.write(*ds);
}
static void on_end_render_pass(command_list *cmd_list)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::end_render_pass);
}
static void on_bind_render_targets_and_depth_stencil(command_list *cmd_list, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::bind_render_targets_and_depth_stencil);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
		trace_data.write(rtvs[i]);
	trace_data.write(dsv);
}

static void on_bind_pipeline(command_list *cmd_list, pipeline_stage type, pipeline pipeline)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::bind_pipeline);
	trace_data.write(type);
	trace_data.write(pipeline);
}
static void on_bind_pipeline_states(command_list *cmd_list, uint32_t count, const dynamic_state *states, const uint32_t *values)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::bind_pipeline_states);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		trace_data.write(states[i]);
		trace_data.write(values[i]);
	}
}
static void on_bind_viewports(command_list *cmd_list, uint32_t first, uint32_t count, const viewport *viewports)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::bind_viewports);
	trace_data.write(first);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
		trace_data.write(viewports[i]);
}
static void on_bind_scissor_rects(command_list *cmd_list, uint32_t first, uint32_t count, const rect *rects)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::bind_scissor_rects);
	trace_data.write(first);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
		trace_data.write(rects[i]);
}
static void on_push_constants(command_list *cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param_index, uint32_t first, uint32_t count, const void *values)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::push_constants);
	trace_data.write(stages);
	trace_data.write(layout);
	trace_data.write(param_index);
	trace_data.write(first);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
		trace_data.write(&static_cast<const uint32_t *>(values)[i], 4);
}
static void on_push_descriptors(command_list *cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param_index, const descriptor_table_update &update)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::push_descriptors);
	trace_data.write(stages);
	trace_data.write(layout);
	trace_data.write(param_index);
	trace_data.write(update.binding);
	trace_data.write(update.array_offset);
	trace_data.write(update.count);
	trace_data.write(update.type);
	for (uint32_t i = 0; i < update.count; ++i)
	{
		switch (update.type)
		{
		case descriptor_type::sampler:
		case descriptor_type::shader_resource_view:
		case descriptor_type::unordered_access_view:
		case descriptor_type::shader_storage_buffer:
			trace_data.write(static_cast<const uint64_t *>(update.descriptors) + i * 1, sizeof(uint64_t) * 1);
			break;
		case descriptor_type::sampler_with_resource_view:
			trace_data.write(static_cast<const uint64_t *>(update.descriptors) + i * 2, sizeof(uint64_t) * 2);
			break;
		case descriptor_type::constant_buffer:
			trace_data.write(static_cast<const uint64_t *>(update.descriptors) + i * 3, sizeof(uint64_t) * 3);
			break;
		}
	}
}
static void on_bind_descriptor_tables(command_list *cmd_list, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count, const descriptor_table *tables)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::bind_descriptor_tables);
	trace_data.write(stages);
	trace_data.write(layout);
	trace_data.write(first);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
		trace_data.write(tables[i]);
}
static void on_bind_index_buffer(command_list *cmd_list, resource buffer, uint64_t offset, uint32_t index_size)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::bind_index_buffer);
	trace_data.write(buffer);
	trace_data.write(offset);
	trace_data.write(index_size);
}
static void on_bind_vertex_buffers(command_list *cmd_list, uint32_t first, uint32_t count, const resource *buffers, const uint64_t *offsets, const uint32_t *strides)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::bind_vertex_buffers);
	trace_data.write(first);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		trace_data.write(buffers[i]);
		trace_data.write(offsets[i]);
		trace_data.write(strides != nullptr ? strides[i] : 0u);
	}
}
static void on_bind_stream_output_buffers(command_list *cmd_list, uint32_t first, uint32_t count, const resource *buffers, const uint64_t *offsets, const uint64_t *max_sizes, const resource *counter_buffers, const uint64_t *counter_offsets)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::bind_stream_output_buffers);
	trace_data.write(first);
	trace_data.write(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		trace_data.write(buffers[i]);
		trace_data.write(offsets[i]);
		trace_data.write(max_sizes != nullptr ? max_sizes[i] : 0u);
		trace_data.write(counter_buffers != nullptr ? counter_buffers[i] : resource { 0 });
		trace_data.write(counter_offsets != nullptr ? counter_offsets[i] : 0u);
	}
}

static bool on_draw(command_list *cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::draw);
	trace_data.write(vertex_count);
	trace_data.write(instance_count);
	trace_data.write(first_vertex);
	trace_data.write(first_instance);

	return false;
}
static bool on_draw_indexed(command_list *cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::draw_indexed);
	trace_data.write(index_count);
	trace_data.write(instance_count);
	trace_data.write(first_index);
	trace_data.write(vertex_offset);
	trace_data.write(first_instance);

	return false;
}
static bool on_dispatch(command_list *cmd_list, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::dispatch);
	trace_data.write(group_count_x);
	trace_data.write(group_count_y);
	trace_data.write(group_count_z);

	return false;
}
static bool on_draw_or_dispatch_indirect(command_list *cmd_list, indirect_command type, resource buffer, uint64_t offset, uint32_t draw_count, uint32_t stride)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::draw_or_dispatch_indirect);
	trace_data.write(type);
	trace_data.write(buffer);
	trace_data.write(offset);
	trace_data.write(draw_count);
	trace_data.write(stride);

	return false;
}

static bool on_copy_resource(command_list *cmd_list, resource src, resource dst)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::copy_resource);
	trace_data.write(src);
	trace_data.write(dst);

	return false;
}
static bool on_copy_buffer_region(command_list *cmd_list, resource src, uint64_t src_offset, resource dst, uint64_t dst_offset, uint64_t size)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::copy_buffer_region);
	trace_data.write(src);
	trace_data.write(src_offset);
	trace_data.write(dst);
	trace_data.write(dst_offset);
	trace_data.write(size);

	return false;
}
static bool on_copy_buffer_to_texture(command_list *cmd_list, resource src, uint64_t src_offset, uint32_t row_length, uint32_t slice_height, resource dst, uint32_t dst_subresource, const subresource_box *dst_box)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::copy_buffer_to_texture);
	trace_data.write(src);
	trace_data.write(src_offset);
	trace_data.write(row_length);
	trace_data.write(slice_height);
	trace_data.write(dst);
	trace_data.write(dst_subresource);
	const bool has_dst_box = dst_box != nullptr;
	trace_data.write(has_dst_box);
	if (has_dst_box)
		trace_data.write(*dst_box);

	return false;
}
static bool on_copy_texture_region(command_list *cmd_list, resource src, uint32_t src_subresource, const subresource_box *src_box, resource dst, uint32_t dst_subresource, const subresource_box *dst_box, filter_mode filter)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::copy_texture_region);
	trace_data.write(src);
	trace_data.write(src_subresource);
	const bool has_src_box = src_box != nullptr;
	trace_data.write(has_src_box);
	if (has_src_box)
		trace_data.write(*src_box);
	trace_data.write(dst);
	trace_data.write(dst_subresource);
	const bool has_dst_box = dst_box != nullptr;
	trace_data.write(has_dst_box);
	if (has_dst_box)
		trace_data.write(*dst_box);
	trace_data.write(filter);

	return false;
}
static bool on_copy_texture_to_buffer(command_list *cmd_list, resource src, uint32_t src_subresource, const subresource_box *src_box, resource dst, uint64_t dst_offset, uint32_t row_length, uint32_t slice_height)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::copy_texture_to_buffer);
	trace_data.write(src);
	trace_data.write(src_subresource);
	const bool has_src_box = src_box != nullptr;
	trace_data.write(has_src_box);
	if (has_src_box)
		trace_data.write(*src_box);
	trace_data.write(dst);
	trace_data.write(dst_offset);
	trace_data.write(row_length);
	trace_data.write(slice_height);

	return false;
}
static bool on_resolve_texture_region(command_list *cmd_list, resource src, uint32_t src_subresource, const subresource_box *src_box, resource dst, uint32_t dst_subresource, int32_t dst_x, int32_t dst_y, int32_t dst_z, format format)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::resolve_texture_region);
	trace_data.write(src);
	trace_data.write(src_subresource);
	const bool has_src_box = src_box != nullptr;
	trace_data.write(has_src_box);
	if (has_src_box)
		trace_data.write(*src_box);
	trace_data.write(dst);
	trace_data.write(dst_subresource);
	trace_data.write(dst_x);
	trace_data.write(dst_y);
	trace_data.write(dst_z);
	trace_data.write(format);

	return false;
}

static bool on_clear_depth_stencil_view(command_list *cmd_list, resource_view dsv, const float *depth, const uint8_t *stencil, uint32_t, const rect *)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::clear_depth_stencil_view);
	trace_data.write(dsv);
	trace_data.write(depth != nullptr ? *depth : 0.0f);
	trace_data.write(stencil != nullptr ? *stencil : uint8_t(0));

	return false;
}
static bool on_clear_render_target_view(command_list *cmd_list, resource_view rtv, const float color[4], uint32_t, const rect *)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::clear_render_target_view);
	trace_data.write(rtv);
	trace_data.write(color, sizeof(float) * 4);

	return false;
}
static bool on_clear_unordered_access_view_uint(command_list *cmd_list, resource_view uav, const uint32_t values[4], uint32_t, const rect *)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::clear_unordered_access_view_uint);
	trace_data.write(uav);
	trace_data.write(values, sizeof(uint32_t) * 4);

	return false;
}
static bool on_clear_unordered_access_view_float(command_list *cmd_list, resource_view uav, const float values[4], uint32_t, const rect *)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::clear_unordered_access_view_float);
	trace_data.write(uav);
	trace_data.write(values, sizeof(float) * 4);

	return false;
}

static bool on_generate_mipmaps(command_list *cmd_list, resource_view srv)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = cmd_list->get_device()->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::generate_mipmaps);
	trace_data.write(srv);

	return false;
}

static bool on_begin_query(command_list *cmd_list, query_heap heap, query_type type, uint32_t index)
{
	return false;
}
static bool on_end_query(command_list *cmd_list, query_heap heap, query_type type, uint32_t index)
{
	return false;
}
static bool on_copy_query_heap_results(command_list *cmd_list, query_heap heap, query_type type, uint32_t first, uint32_t count, resource dest, uint64_t dest_offset, uint32_t stride)
{
	return false;
}

static void on_reset_command_list(command_list *cmd_list)
{
}
static void on_execute_command_list(command_queue *queue, command_list *cmd_list)
{
}
static void on_execute_secondary_command_list(command_list *cmd_list, command_list *secondary_cmd_list)
{
}

static void on_present(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
	device *const device = queue->get_device();

	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	auto &trace_data = device->get_private_data<device_data>();
	trace_data.write(reshade::addon_event::present);
}

extern "C" __declspec(dllexport) const char *NAME = "API Trace";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Example add-on that logs the graphics API calls done by the application of the next frame after pressing a keyboard shortcut.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;

		reshade::register_event<reshade::addon_event::init_device>(on_init_device);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
		reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
		reshade::register_event<reshade::addon_event::init_sampler>(on_init_sampler);
		reshade::register_event<reshade::addon_event::destroy_sampler>(on_destroy_sampler);
		reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
		reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
		reshade::register_event<reshade::addon_event::init_resource_view>(on_init_resource_view);
		reshade::register_event<reshade::addon_event::destroy_resource_view>(on_destroy_resource_view);
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::init_pipeline_layout>(on_init_pipeline_layout);
		reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(on_destroy_pipeline_layout);

		reshade::register_event<reshade::addon_event::copy_descriptor_tables>(on_copy_descriptor_tables);
		reshade::register_event<reshade::addon_event::update_descriptor_tables>(on_update_descriptor_tables);

		reshade::register_event<reshade::addon_event::map_buffer_region>(on_map_buffer_region);
		reshade::register_event<reshade::addon_event::unmap_buffer_region>(on_unmap_buffer_region);
		reshade::register_event<reshade::addon_event::map_texture_region>(on_map_texture_region);
		reshade::register_event<reshade::addon_event::unmap_texture_region>(on_unmap_texture_region);
		reshade::register_event<reshade::addon_event::update_buffer_region>(on_update_buffer_region);
		reshade::register_event<reshade::addon_event::update_texture_region>(on_update_texture_region);

		reshade::register_event<reshade::addon_event::barrier>(on_barrier);
		reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
		reshade::register_event<reshade::addon_event::end_render_pass>(on_end_render_pass);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets_and_depth_stencil);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_bind_pipeline_states);
		reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
		reshade::register_event<reshade::addon_event::bind_scissor_rects>(on_bind_scissor_rects);
		reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
		reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
		reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_bind_descriptor_tables);
		reshade::register_event<reshade::addon_event::bind_index_buffer>(on_bind_index_buffer);
		reshade::register_event<reshade::addon_event::bind_vertex_buffers>(on_bind_vertex_buffers);
		reshade::register_event<reshade::addon_event::bind_stream_output_buffers>(on_bind_stream_output_buffers);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
		reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_or_dispatch_indirect);
		reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
		reshade::register_event<reshade::addon_event::copy_buffer_region>(on_copy_buffer_region);
		reshade::register_event<reshade::addon_event::copy_buffer_to_texture>(on_copy_buffer_to_texture);
		reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
		reshade::register_event<reshade::addon_event::copy_texture_to_buffer>(on_copy_texture_to_buffer);
		reshade::register_event<reshade::addon_event::resolve_texture_region>(on_resolve_texture_region);
		reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_depth_stencil_view);
		reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_render_target_view);
		reshade::register_event<reshade::addon_event::clear_unordered_access_view_uint>(on_clear_unordered_access_view_uint);
		reshade::register_event<reshade::addon_event::clear_unordered_access_view_float>(on_clear_unordered_access_view_float);
		reshade::register_event<reshade::addon_event::generate_mipmaps>(on_generate_mipmaps);
		reshade::register_event<reshade::addon_event::begin_query>(on_begin_query);
		reshade::register_event<reshade::addon_event::end_query>(on_end_query);
		reshade::register_event<reshade::addon_event::copy_query_heap_results>(on_copy_query_heap_results);

		reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
		reshade::register_event<reshade::addon_event::execute_command_list>(on_execute_command_list);
		reshade::register_event<reshade::addon_event::execute_secondary_command_list>(on_execute_secondary_command_list);

		reshade::register_event<reshade::addon_event::present>(on_present);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}
