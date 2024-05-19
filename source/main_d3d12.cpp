/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.hpp"
#include "com_ptr.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>

struct application_d3d12 : public application
{
	application_d3d12(HMODULE dxgi_module, HMODULE d3d12_module, com_ptr<ID3D12Device> device, com_ptr<ID3D12CommandQueue> command_queue, com_ptr<IDXGISwapChain1> swapchain) :
		_dxgi_module(dxgi_module),
		_d3d12_module(d3d12_module),
		_device(std::move(device)),
		_command_queue(std::move(command_queue)),
		_swapchain(std::move(swapchain))
	{
	}
	~application_d3d12()
	{
		_swapchain.reset();
		_command_queue.reset();
		_device.reset();

		FreeLibrary(_d3d12_module);
		FreeLibrary(_dxgi_module);
	}

	auto get_device() -> void * final
	{
		return _device.get();
	}
	auto get_command_queue() -> void * final
	{
		return _command_queue.get();
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
	HMODULE _d3d12_module = nullptr;

	com_ptr<ID3D12Device> _device;
	com_ptr<ID3D12CommandQueue> _command_queue;
	com_ptr<IDXGISwapChain1> _swapchain;
};

std::unique_ptr<application> create_application_d3d12(HWND window_handle)
{
	const HMODULE dxgi_module = LoadLibrary(TEXT("dxgi.dll"));
	if (dxgi_module == nullptr)
		return nullptr;
	const HMODULE d3d12_module = LoadLibrary(TEXT("d3d12.dll"));
	if (d3d12_module == nullptr)
		return nullptr;

	const auto create_dxgi = reinterpret_cast<decltype(&CreateDXGIFactory2)>(GetProcAddress(dxgi_module, "CreateDXGIFactory2"));
	if (create_dxgi == nullptr)
		return nullptr;
	const auto create_d3d12 = reinterpret_cast<decltype(&D3D12CreateDevice)>(GetProcAddress(d3d12_module, "D3D12CreateDevice"));
	if (create_d3d12 == nullptr)
		return nullptr;

	// Initialize Direct3D 12
	com_ptr<IDXGIFactory2> dxgi_factory;
	if (FAILED(create_dxgi(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&dxgi_factory))))
		return nullptr;

	com_ptr<ID3D12Device> device;
	if (FAILED(create_d3d12(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
		return nullptr;

	D3D12_COMMAND_QUEUE_DESC command_queue_desc = {};
	command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	com_ptr<ID3D12CommandQueue> command_queue;
	if (FAILED(device->CreateCommandQueue(&command_queue_desc, IID_PPV_ARGS(&command_queue))))
		return nullptr;

	DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
	swapchain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapchain_desc.SampleDesc = { 1, 0 };
	swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapchain_desc.BufferCount = 3;
	swapchain_desc.Scaling = DXGI_SCALING_STRETCH;
	swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	com_ptr<IDXGISwapChain1> swapchain;
	if (FAILED(dxgi_factory->CreateSwapChainForHwnd(command_queue.get(), window_handle, &swapchain_desc, nullptr, nullptr, &swapchain)))
		return nullptr;

	return std::make_unique<application_d3d12>(dxgi_module, d3d12_module, std::move(device), std::move(command_queue), std::move(swapchain));
}
