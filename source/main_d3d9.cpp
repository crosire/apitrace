/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.hpp"
#include "com_ptr.hpp"
#include <d3d9.h>

struct application_d3d9 : public application
{
	application_d3d9(HMODULE d3d9_module, com_ptr<IDirect3DDevice9> device) :
		_d3d9_module(d3d9_module),
		_device(std::move(device))
	{
		_device->GetSwapChain(0, &_swapchain);
		_device->BeginScene();
	}
	~application_d3d9()
	{
		_device->EndScene();

		_swapchain.reset();
		_device.reset();

		FreeLibrary(_d3d9_module);
	}

	auto get_device() -> void * final
	{
		return _device.get();
	}
	auto get_command_queue() -> void * final
	{
		return _device.get();
	}
	auto get_swapchain() -> void * final
	{
		return _swapchain.get();
	}

	void present() final
	{
		_device->EndScene();
		_device->Present(nullptr, nullptr, nullptr, nullptr);
		_device->BeginScene();
	}

	HMODULE _d3d9_module = nullptr;

	com_ptr<IDirect3DDevice9> _device;
	com_ptr<IDirect3DSwapChain9> _swapchain;
};

std::unique_ptr<application> create_application_d3d9(HWND window_handle, unsigned int samples)
{
	const HMODULE d3d9_module = LoadLibrary(TEXT("d3d9.dll"));
	if (d3d9_module == nullptr)
		return nullptr;

	const auto create_d3d9 = reinterpret_cast<decltype(&Direct3DCreate9)>(GetProcAddress(d3d9_module, "Direct3DCreate9"));
	if (create_d3d9 == nullptr)
		return nullptr;

	com_ptr<IDirect3D9> d3d = create_d3d9(D3D_SDK_VERSION);
	if (d3d == nullptr)
		return nullptr;

	D3DPRESENT_PARAMETERS pp = {};
	pp.MultiSampleType = samples > 1 ? static_cast<D3DMULTISAMPLE_TYPE>(samples) : D3DMULTISAMPLE_NONE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.hDeviceWindow = window_handle;
	pp.Windowed = true;
	pp.Flags = 0;
	pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

	com_ptr<IDirect3DDevice9> device;
	if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window_handle, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device)))
		return nullptr;

	return std::make_unique<application_d3d9>(d3d9_module, std::move(device));
}
