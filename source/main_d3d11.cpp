/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.hpp"
#include "com_ptr.hpp"
#include <d3d11.h>

struct application_d3d11 : public application
{
	application_d3d11(HMODULE dxgi_module, HMODULE d3d11_module, com_ptr<ID3D11Device> device, com_ptr<ID3D11DeviceContext> immediate_context, com_ptr<IDXGISwapChain> swapchain) :
		_dxgi_module(dxgi_module),
		_d3d11_module(d3d11_module),
		_device(std::move(device)),
		_immediate_context(std::move(immediate_context)),
		_swapchain(std::move(swapchain))
	{
	}
	~application_d3d11()
	{
		_swapchain.reset();
		_immediate_context.reset();
		_device.reset();

		FreeLibrary(_d3d11_module);
		FreeLibrary(_dxgi_module);
	}

	auto get_device() -> void * final
	{
		return _device.get();
	}
	auto get_command_queue() -> void * final
	{
		return _immediate_context.get();
	}
	auto get_swapchain() -> void * final
	{
		return _swapchain.get();
	}

	void present() final
	{
		_swapchain->Present(1, 0);
	}

	HMODULE _dxgi_module = nullptr;
	HMODULE _d3d11_module = nullptr;

	com_ptr<ID3D11Device> _device;
	com_ptr<ID3D11DeviceContext> _immediate_context;
	com_ptr<IDXGISwapChain> _swapchain;
};

std::unique_ptr<application> create_application_d3d11(HWND window_handle, unsigned int samples)
{
	const HMODULE dxgi_module = LoadLibrary(TEXT("dxgi.dll"));
	if (dxgi_module == nullptr)
		return nullptr;
	const HMODULE d3d11_module = LoadLibrary(TEXT("d3d11.dll"));
	if (d3d11_module == nullptr)
		return nullptr;

	const auto create_d3d11 = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(GetProcAddress(d3d11_module, "D3D11CreateDeviceAndSwapChain"));
	if (create_d3d11 == nullptr)
		return nullptr;

	DXGI_SWAP_CHAIN_DESC desc = {};
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc = { samples, 0u };
	desc.BufferUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 1;
	desc.OutputWindow = window_handle;
	desc.Windowed = true;
	desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	com_ptr<ID3D11Device> device;
	com_ptr<ID3D11DeviceContext> immediate_context;
	com_ptr<IDXGISwapChain> swapchain;
	if (FAILED(create_d3d11(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapchain, &device, nullptr, &immediate_context)))
		return nullptr;

	return std::make_unique<application_d3d11>(dxgi_module, d3d11_module, std::move(device), std::move(immediate_context), std::move(swapchain));
}
