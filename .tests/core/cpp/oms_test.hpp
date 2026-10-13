// Minimal test harness (no third-party framework; the project may not download one).
// TEST(L2, name) registers a test under a layer. The runner prints TAP-like lines:
//   ok 12 - L2 json_rejects_trailing_comma
//   not ok 13 - L2 something   (followed by "# file:line message")
#pragma once
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace oms_test {

struct Case {
	std::string layer;
	std::string name;
	std::function<void()> fn;
};

inline std::vector<Case> &registry()
{
	static std::vector<Case> r;
	return r;
}

struct Registrar {
	Registrar(const char *layer, const char *name, std::function<void()> fn)
	{
		Case c;
		c.layer = layer;
		c.name = name;
		c.fn = fn;
		registry().push_back(c);
	}
};

struct Failure {
	std::string message;
};

inline std::vector<std::string> &failures()
{
	static std::vector<std::string> f;
	return f;
}

inline void fail(const char *file, int line, const std::string &msg)
{
	std::ostringstream ss;
	ss << file << ":" << line << " " << msg;
	failures().push_back(ss.str());
}

template<typename A, typename B> inline std::string show_pair(const A &a, const B &b)
{
	std::ostringstream ss;
	ss << "[" << a << "] != [" << b << "]";
	return ss.str();
}

} // namespace oms_test

#define OMS_CAT2(a, b) a##b
#define OMS_CAT(a, b) OMS_CAT2(a, b)

#define TEST(layer, name)                                                                       \
	static void OMS_CAT(test_fn_, name)();                                                  \
	static oms_test::Registrar OMS_CAT(test_reg_, name)(#layer, #name, OMS_CAT(test_fn_, name)); \
	static void OMS_CAT(test_fn_, name)()

#define CHECK(cond)                                                                             \
	do {                                                                                    \
		if (!(cond))                                                                    \
			oms_test::fail(__FILE__, __LINE__, std::string("CHECK failed: ") + #cond); \
	} while (0)

#define CHECK_EQ(a, b)                                                                          \
	do {                                                                                    \
		if (!((a) == (b)))                                                              \
			oms_test::fail(__FILE__, __LINE__,                                      \
				       std::string("CHECK_EQ failed: ") + #a + " vs " + #b + " " + \
					       oms_test::show_pair((a), (b)));                  \
	} while (0)

#define CHECK_TRUE(cond) CHECK(cond)
#define CHECK_FALSE(cond) CHECK(!(cond))

#define CHECK_NEAR(a, b, eps)                                                                   \
	do {                                                                                    \
		double oms_a = (a), oms_b = (b);                                                \
		if (!(oms_a - oms_b <= (eps) && oms_b - oms_a <= (eps)))                        \
			oms_test::fail(__FILE__, __LINE__,                                      \
				       std::string("CHECK_NEAR failed: ") + #a + " vs " + #b + " " + \
					       oms_test::show_pair(oms_a, oms_b));              \
	} while (0)
