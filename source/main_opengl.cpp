/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.hpp"

static decltype(&wglCreateContext) wgl_create_context = nullptr;
static decltype(&wglDeleteContext) wgl_delete_context = nullptr;
static decltype(&wglMakeCurrent) wgl_make_current = nullptr;
static decltype(&wglGetProcAddress) wgl_get_proc_address = nullptr;

struct application_opengl : public application
{
	application_opengl(HMODULE opengl_module, HDC hdc, HGLRC hglrc) :
		_opengl_module(opengl_module),
		_hdc(hdc),
		_hglrc(hglrc)
	{
	}
	~application_opengl()
	{
		wgl_make_current(nullptr, nullptr);
		wgl_delete_context(_hglrc);

		FreeLibrary(_opengl_module);
	}

	auto get_device() -> void * final
	{
		return _hglrc;
	}
	auto get_command_queue() -> void * final
	{
		return _hglrc;
	}
	auto get_swapchain() -> void * final
	{
		return _hdc;
	}

	void present() final
	{
		SwapBuffers(_hdc);
	}

	HMODULE _opengl_module = nullptr;

	HDC _hdc = nullptr;
	HGLRC _hglrc = nullptr;
};

std::unique_ptr<application> create_application_opengl(HWND window_handle, unsigned int samples)
{
	const HMODULE opengl_module = LoadLibrary(TEXT("opengl32.dll"));
	if (opengl_module == nullptr)
		return nullptr;

	wgl_create_context = reinterpret_cast<decltype(&wglCreateContext)>(GetProcAddress(opengl_module, "wglCreateContext"));
	wgl_delete_context = reinterpret_cast<decltype(&wglDeleteContext)>(GetProcAddress(opengl_module, "wglDeleteContext"));
	wgl_make_current = reinterpret_cast<decltype(&wglMakeCurrent)>(GetProcAddress(opengl_module, "wglMakeCurrent"));
	wgl_get_proc_address = reinterpret_cast<decltype(&wglGetProcAddress)>(GetProcAddress(opengl_module, "wglGetProcAddress"));

	if (wgl_create_context == nullptr || wgl_delete_context == nullptr || wgl_make_current == nullptr || wgl_get_proc_address == nullptr)
		return nullptr;

	const HWND temp_window_handle = CreateWindow(TEXT("STATIC"), nullptr, WS_POPUP, 0, 0, 0, 0, window_handle, nullptr, GetModuleHandle(nullptr), nullptr);
	if (temp_window_handle == nullptr)
		return nullptr;

	const HDC hdc1 = GetDC(temp_window_handle);
	const HDC hdc2 = GetDC(window_handle);

	PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1 };
	pfd.dwFlags = PFD_DOUBLEBUFFER | PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 24;
	pfd.cAlphaBits = 8;

	int pix_format = ChoosePixelFormat(hdc1, &pfd);
	SetPixelFormat(hdc1, pix_format, &pfd);

	const HGLRC hglrc1 = wgl_create_context(hdc1);
	if (hglrc1 == nullptr)
		return nullptr;

	wgl_make_current(hdc1, hglrc1);

	const auto wgl_choose_pixel_format = reinterpret_cast<BOOL(WINAPI *)(HDC, const int *, const FLOAT *, UINT, int *, UINT *)>(wgl_get_proc_address("wglChoosePixelFormatARB"));
	const auto wgl_create_context_attribs = reinterpret_cast<HGLRC(WINAPI *)(HDC, HGLRC, const int *)>(wgl_get_proc_address("wglCreateContextAttribsARB"));

	if (wgl_choose_pixel_format == nullptr || wgl_create_context_attribs == nullptr)
		return nullptr;

	const int pix_attribs[] = {
		0x2011 /* WGL_DOUBLE_BUFFER_ARB */, 1,
		0x2001 /* WGL_DRAW_TO_WINDOW_ARB */, 1,
		0x2010 /* WGL_SUPPORT_OPENGL_ARB */, 1,
		0x2013 /* WGL_PIXEL_TYPE_ARB */, 0x202B /* WGL_TYPE_RGBA_ARB */,
		0x2014 /* WGL_COLOR_BITS_ARB */, pfd.cColorBits,
		0x201B /* WGL_ALPHA_BITS_ARB */, pfd.cAlphaBits,
		0x2041 /* WGL_SAMPLE_BUFFERS_ARB */, samples > 1 ? 1 : 0,
		0x2042 /* WGL_SAMPLES_ARB */, static_cast<int>(samples),
		0 // Terminate list
	};

	UINT num_formats = 0;
	if (!wgl_choose_pixel_format(hdc2, pix_attribs, nullptr, 1, &pix_format, &num_formats))
		return nullptr;

	SetPixelFormat(hdc2, pix_format, &pfd);

	// Create an OpenGL 4.5 context
	const int attribs[] = {
		0x2091 /* WGL_CONTEXT_MAJOR_VERSION_ARB */, 4,
		0x2092 /* WGL_CONTEXT_MINOR_VERSION_ARB */, 5,
		0x9126 /* WGL_CONTEXT_PROFILE_MASK_ARB  */, 0x2 /* WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB */,
		0 // Terminate list
	};

	const HGLRC hglrc2 = wgl_create_context_attribs(hdc2, nullptr, attribs);
	if (hglrc2 == nullptr)
		return nullptr;

	wgl_make_current(nullptr, nullptr);
	wgl_delete_context(hglrc1);

	DestroyWindow(temp_window_handle);

	wgl_make_current(hdc2, hglrc2);

	return std::make_unique<application_opengl>(opengl_module, hdc2, hglrc2);
}
