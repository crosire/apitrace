/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdio>
#include <cassert>

struct trace_data
{
	trace_data()
	{
	}
	~trace_data()
	{
		if (s != nullptr)
			fclose(s);
	}

	bool is_open() const { return s != nullptr; }

	FILE *s = nullptr;
};

struct trace_data_read : trace_data
{
	explicit trace_data_read(const char *filename)
	{
		fopen_s(&s, filename, "rb");
	}

	template <typename T>
	T read()
	{
		T value;
		read(&value, sizeof(T));
		return value;
	}
	bool read(void *data, size_t size)
	{
		size_t read = fread(data, 1, size, s);
		assert(read == size || read == 0);
		return read == size;
	}
};


struct trace_data_write : trace_data
{
	explicit trace_data_write(const char *filename)
	{
		fopen_s(&s, filename, "wb");

		assert(is_open());
	}

	template <typename T>
	void write(T &&value)
	{
		write(&value, sizeof(T));
	}
	void write(const void *data, size_t size)
	{
#if 0
		auto p = static_cast<const unsigned char *>(data);

		while (size != 0)
		{
			const size_t chunk = std::min(size, size_t(512));

			size_t written = fwrite(p, 1, chunk, s);
			assert(written == chunk);

			p += chunk;
			size -= chunk;
		}
#else
		size_t written = fwrite(data, 1, size, s);
		assert(written == size);
#endif
	}
};
