// Bounded-buffer CPU model and a narrow GLSL->C++ execution bridge for the robust buffer access checks.
//
// Storage blocks are backed by bounded arrays with sentinel memory on both sides. Every actual
// read/write is recorded as an event (reads via implicit conversion, writes via assignment) and the
// model raises a hard violation if an out-of-range index ever reaches the backing store. Sentinel
// integrity is compared byte-wise so the same code works for scalars and vectors.
#ifndef ROBUST_CPU_MODEL_HPP
#define ROBUST_CPU_MODEL_HPP

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace robust
{
using uint = uint32_t;

struct AccessEvent
{
	std::string array;
	uint32_t index = 0;
	bool is_write = false;
	AccessEvent() = default;
	AccessEvent(std::string array_, uint32_t index_, bool is_write_)
	    : array(std::move(array_)), index(index_), is_write(is_write_)
	{
	}
	bool operator==(const AccessEvent &other) const
	{
		return array == other.array && index == other.index && is_write == other.is_write;
	}
};

struct ModelRuntime
{
	std::vector<AccessEvent> events;
	bool violation = false;
	std::string violation_text;

	void fail(const std::string &what)
	{
		violation = true;
		if (violation_text.empty())
			violation_text = what;
	}
	void reset()
	{
		events.clear();
		violation = false;
		violation_text.clear();
	}
};

// Minimal GLSL-compatible vector with component access and subscript.
template <typename T, int N>
struct Vec
{
	union
	{
		T c[N];
		struct
		{
			T x, y, z, w;
		};
	};
	Vec() : c{}
	{
	}
	Vec(const Vec &other)
	{
		for (int i = 0; i < N; i++)
			c[i] = other.c[i];
	}
	Vec &operator=(const Vec &other)
	{
		for (int i = 0; i < N; i++)
			c[i] = other.c[i];
		return *this;
	}
	explicit Vec(T v)
	{
		for (int i = 0; i < N; i++)
			c[i] = v;
	}
	T &operator[](uint32_t i)
	{
		return c[i];
	}
	const T &operator[](uint32_t i) const
	{
		return c[i];
	}
};

template <typename T>
inline bool bytes_equal(const T &a, const T &b)
{
	return std::memcmp(&a, &b, sizeof(T)) == 0;
}

// Reference to one scalar cell. Logs a read when implicitly converted, a write when assigned.
template <typename T>
struct ScalarRef
{
	ModelRuntime *rt;
	std::string name;
	uint32_t index;
	T *cell;

	ScalarRef &operator=(const T &value)
	{
		rt->events.push_back(AccessEvent(name, index, true));
		*cell = value;
		return *this;
	}
	ScalarRef &operator=(const ScalarRef &other)
	{
		T value = T(other); // force a read of the source first
		rt->events.push_back(AccessEvent(name, index, true));
		*cell = value;
		return *this;
	}
	operator T() const
	{
		rt->events.push_back(AccessEvent(name, index, false));
		return *cell;
	}
};

// Reference to one vector cell. Whole-vector reads/writes plus component subscripting.
template <typename T, int N>
struct VectorRef
{
	ModelRuntime *rt;
	std::string name;
	uint32_t index;
	Vec<T, N> *cell;

	// Whole-vector write.
	VectorRef &operator=(const Vec<T, N> &value)
	{
		rt->events.push_back(AccessEvent(name, index, true));
		*cell = value;
		return *this;
	}
	// Whole-vector read.
	operator Vec<T, N>() const
	{
		rt->events.push_back(AccessEvent(name, index, false));
		return *cell;
	}
	// Component access; each component inherits the element index of the enclosing array.
	ScalarRef<T> operator[](uint32_t component)
	{
		(void)component;
		return ScalarRef<T>{rt, name, index, &(*cell)[0] + component};
	}
};

// Sentinel-marked, bounds-checked runtime array of scalars. Out-of-range access trips a violation and
// hands back a dummy cell instead of touching neighboring memory.
template <typename T>
struct BoundedArray
{
	std::string name;
	ModelRuntime *rt = nullptr;
	std::vector<T> before;
	std::vector<T> data;
	std::vector<T> after;
	T dummy{};
	static const uint32_t SENTINEL = 4u;

	void bind(ModelRuntime *runtime, const std::string &array_name, uint32_t length)
	{
		rt = runtime;
		name = array_name;
		before.assign(SENTINEL, make_sentinel(0xDEADBEEFu));
		after.assign(SENTINEL, make_sentinel(0xBEEFCAFEu));
		data.assign(length, T{});
	}

	static T make_sentinel(uint32_t pattern)
	{
		T value{};
		std::memset(&value, 0, sizeof(T));
		size_t count = sizeof(T) / sizeof(uint32_t);
		auto *words = reinterpret_cast<uint32_t *>(&value);
		for (size_t i = 0; i < count; i++)
			words[i] = pattern;
		return value;
	}

	uint32_t length() const
	{
		return uint32_t(data.size());
	}

	bool in_range(uint32_t i) const
	{
		return i < data.size();
	}

	void note_violation(uint32_t i) const
	{
		std::ostringstream oss;
		oss << "out-of-range access on " << name << " index " << i << " length " << data.size();
		rt->fail(oss.str());
	}

	ScalarRef<T> operator[](uint32_t i)
	{
		if (!in_range(i))
		{
			note_violation(i);
			return ScalarRef<T>{rt, name, i, &dummy};
		}
		return ScalarRef<T>{rt, name, i, &data[i]};
	}

	bool sentinels_intact() const
	{
		for (const T &v : before)
			if (!bytes_equal(v, make_sentinel(0xDEADBEEFu)))
				return false;
		for (const T &v : after)
			if (!bytes_equal(v, make_sentinel(0xBEEFCAFEu)))
				return false;
		return true;
	}
};

// Runtime array of structs (the nested-struct scenario). Subscripting returns a raw reference to the
// element so its fixed-array members remain directly addressable; the outer index is validated here,
// while each inner FixedArray enforces and logs its own component accesses.
template <typename T>
struct StructBoundedArray
{
	std::string name;
	ModelRuntime *rt = nullptr;
	std::vector<T> before;
	std::vector<T> data;
	std::vector<T> after;
	T dummy{};
	static const uint32_t SENTINEL = 4u;

	void bind(ModelRuntime *runtime, const std::string &array_name, uint32_t length)
	{
		rt = runtime;
		name = array_name;
		before.assign(SENTINEL, T{});
		after.assign(SENTINEL, T{});
		data.assign(length, T{});
	}

	uint32_t length() const
	{
		return uint32_t(data.size());
	}

	T &operator[](uint32_t i)
	{
		if (i >= data.size())
		{
			std::ostringstream oss;
			oss << "out-of-range outer access on " << name << " index " << i << " length " << data.size();
			rt->fail(oss.str());
			return dummy;
		}
		// Only validate the outer index here; scalar read/write events are recorded by the inner
		// FixedArray members so the event log maps one-to-one onto actual scalar memory accesses.
		return data[i];
	}

	bool sentinels_intact() const
	{
		for (const T &v : before)
			for (const auto &m : v.inner.data)
				if (m != 0)
					return false;
		for (const T &v : after)
			for (const auto &m : v.inner.data)
				if (m != 0)
					return false;
		return true;
	}
};

// Specialization for vector elements so subscripting returns a component reference.
template <typename T, int N>
struct BoundedArray<Vec<T, N>>
{
	std::string name;
	ModelRuntime *rt = nullptr;
	std::vector<Vec<T, N>> before;
	std::vector<Vec<T, N>> data;
	std::vector<Vec<T, N>> after;
	Vec<T, N> dummy{};
	static const uint32_t SENTINEL = 4u;

	void bind(ModelRuntime *runtime, const std::string &array_name, uint32_t length)
	{
		rt = runtime;
		name = array_name;
		before.assign(SENTINEL, Vec<T, N>{});
		after.assign(SENTINEL, Vec<T, N>{});
		data.assign(length, Vec<T, N>{});
		for (auto &v : before)
			std::memset(&v, 0xAA, sizeof(v));
		for (auto &v : after)
			std::memset(&v, 0xBB, sizeof(v));
	}

	uint32_t length() const
	{
		return uint32_t(data.size());
	}

	void note_violation(uint32_t i, bool write) const
	{
		std::ostringstream oss;
		oss << "out-of-range " << (write ? "write" : "read") << " on " << name << " index " << i
		    << " length " << data.size();
		rt->fail(oss.str());
	}

	VectorRef<T, N> operator[](uint32_t i)
	{
		if (i >= data.size())
		{
			note_violation(i, true);
			return VectorRef<T, N>{rt, name, i, &dummy};
		}
		return VectorRef<T, N>{rt, name, i, &data[i]};
	}

	bool sentinels_intact() const
	{
		for (const auto &v : before)
		{
			const char *p = reinterpret_cast<const char *>(&v);
			for (size_t k = 0; k < sizeof(v); k++)
				if (p[k] != char(0xAA))
					return false;
		}
		for (const auto &v : after)
		{
			const char *p = reinterpret_cast<const char *>(&v);
			for (size_t k = 0; k < sizeof(v); k++)
				if (p[k] != char(0xBB))
					return false;
		}
		return true;
	}
};

// Fixed-length inner arrays (e.g. a struct member) with the same event/violation semantics.
template <typename T, uint32_t L>
struct FixedArray
{
	std::string name;
	ModelRuntime *rt = nullptr;
	T data[L]{};

	void bind(ModelRuntime *runtime, const std::string &array_name, uint32_t)
	{
		rt = runtime;
		name = array_name;
		for (uint32_t i = 0; i < L; i++)
			data[i] = T{};
	}
	uint32_t length() const
	{
		return L;
	}
	ScalarRef<T> operator[](uint32_t i)
	{
		if (i >= L)
		{
			std::ostringstream oss;
			oss << "out-of-range access on fixed " << name << " index " << i;
			rt->fail(oss.str());
			static T dummy{};
			return ScalarRef<T>{rt, name, i, &dummy};
		}
		return ScalarRef<T>{rt, name, i, &data[i]};
	}
};

using vec2 = Vec<float, 2>;
using vec3 = Vec<float, 3>;
using vec4 = Vec<float, 4>;
using ivec2 = Vec<int32_t, 2>;
using ivec3 = Vec<int32_t, 3>;
using ivec4 = Vec<int32_t, 4>;
using uvec2 = Vec<uint32_t, 2>;
using uvec3 = Vec<uint32_t, 3>;
using uvec4 = Vec<uint32_t, 4>;
} // namespace robust

#endif
