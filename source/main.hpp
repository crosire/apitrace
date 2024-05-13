/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <memory>
#include <Windows.h>

class application
{
public:
	virtual ~application() {}

	virtual auto get_device() -> void * = 0;
	virtual auto get_command_queue() -> void * = 0;
	virtual auto get_swapchain() -> void * = 0;

	virtual void present() = 0;
};

std::unique_ptr<application> create_application_d3d9(HWND window_handle, unsigned int samples = 1);
std::unique_ptr<application> create_application_d3d11(HWND window_handle, unsigned int samples = 1);
std::unique_ptr<application> create_application_d3d12(HWND window_handle);
std::unique_ptr<application> create_application_opengl(HWND window_handle, unsigned int samples = 1);
