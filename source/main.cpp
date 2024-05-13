/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.hpp"
#include "reshade.hpp"
#include "trace_data.hpp"

extern bool play_frame(trace_data_read &trace_data, reshade::api::command_list *cmd_list, reshade::api::effect_runtime *runtime);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int nCmdShow)
{
	SetEnvironmentVariable(TEXT("RESHADE_DISABLE_LOADING_CHECK"), TEXT("1"));
	SetEnvironmentVariable(TEXT("RESHADE_DISABLE_GRAPHICS_HOOK"), TEXT("1"));

	const HMODULE reshade_module = LoadLibrary(
#ifndef _WIN64
		TEXT("ReShade32.dll")
#else
		TEXT("ReShade64.dll")
#endif
		);
	if (reshade_module == nullptr)
		return 1;
	const auto create_effect_runtime = reinterpret_cast<decltype(&ReShadeCreateEffectRuntime)>(GetProcAddress(reshade_module, "ReShadeCreateEffectRuntime"));
	const auto destroy_effect_runtime = reinterpret_cast<decltype(&ReShadeDestroyEffectRuntime)>(GetProcAddress(reshade_module, "ReShadeDestroyEffectRuntime"));
	const auto update_and_present_effect_runtime = reinterpret_cast<decltype(&ReShadeUpdateAndPresentEffectRuntime)>(GetProcAddress(reshade_module, "ReShadeUpdateAndPresentEffectRuntime"));
	if (create_effect_runtime == nullptr || destroy_effect_runtime == nullptr || update_and_present_effect_runtime == nullptr)
		return 1;

	WNDCLASS wc = { sizeof(wc) };
	wc.hInstance = hInstance;
	wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wc.lpszClassName = TEXT("apitrace");
	wc.lpfnWndProc =
		[](HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
			if (Msg == WM_DESTROY)
				PostQuitMessage(EXIT_SUCCESS);
			return DefWindowProc(hWnd, Msg, wParam, lParam);
		};
	RegisterClass(&wc);

	const HWND window_handle = CreateWindow(wc.lpszClassName, TEXT("apitrace"), WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, hInstance, nullptr);
	if (window_handle == nullptr)
		return 1;
	ShowWindow(window_handle, nCmdShow);

	trace_data_read trace_data(lpCmdLine[0] == '\0' ? "api_trace_log.bin" : lpCmdLine);
	if (!trace_data.is_open())
		return 2;

	if (constexpr uint64_t MAGIC = (uint64_t('A') << 0) | (uint64_t('P') << 8) | (uint64_t('I') << 16) | (uint64_t('T') << 24) | (uint64_t('R') << 32) | (uint64_t('A') << 40) | (uint64_t('C') << 48) | (uint64_t('E') << 56);
		MAGIC != trace_data.read<uint64_t>())
		return 2;

	const auto graphics_api = trace_data.read<reshade::api::device_api>();

	std::unique_ptr<application> app;
	switch (graphics_api)
	{
	case reshade::api::device_api::d3d9:
		app = create_application_d3d9(window_handle);
		break;
	case reshade::api::device_api::d3d11:
		app = create_application_d3d11(window_handle);
		break;
	case reshade::api::device_api::d3d12:
		app = create_application_d3d12(window_handle);
		break;
	case reshade::api::device_api::opengl:
		app = create_application_opengl(window_handle);
		break;
	}

	if (app == nullptr)
		return 1;

	reshade::api::effect_runtime *runtime = nullptr;
	if (!create_effect_runtime(graphics_api, app->get_device(), app->get_command_queue(), app->get_swapchain(), ".\\", &runtime))
		return 1;

	MSG msg = {};
	while (true)
	{
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE) && msg.message != WM_QUIT)
			DispatchMessage(&msg);

		if (msg.message == WM_QUIT)
			break;

		reshade::api::command_list *const cmd_list = runtime->get_command_queue()->get_immediate_command_list();
		cmd_list->barrier(runtime->get_current_back_buffer(), reshade::api::resource_usage::present, reshade::api::resource_usage::render_target);
		play_frame(trace_data, cmd_list, runtime);
		cmd_list->barrier(runtime->get_current_back_buffer(), reshade::api::resource_usage::render_target, reshade::api::resource_usage::present);

		update_and_present_effect_runtime(runtime);

		app->present();
	}

	destroy_effect_runtime(runtime);

	return static_cast<int>(msg.wParam);
}
