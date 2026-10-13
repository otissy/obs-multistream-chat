// Usage: oms_tests [L2|L3] [name-substring]
#include "oms_test.hpp"

#include <cstring>
#include <exception>

int main(int argc, char **argv)
{
	std::string layer = argc > 1 ? argv[1] : "";
	std::string filter = argc > 2 ? argv[2] : "";
	int n = 0, pass = 0, fail = 0;
	std::vector<oms_test::Case> &all = oms_test::registry();
	for (size_t i = 0; i < all.size(); ++i) {
		const oms_test::Case &c = all[i];
		if (!layer.empty() && layer != "all" && c.layer != layer)
			continue;
		if (!filter.empty()) {
			// '|' separates alternatives: any substring match selects the test
			bool hit = false;
			size_t from = 0;
			while (from <= filter.size() && !hit) {
				size_t bar = filter.find('|', from);
				std::string part = filter.substr(from, bar == std::string::npos ? std::string::npos : bar - from);
				if (!part.empty() && c.name.find(part) != std::string::npos)
					hit = true;
				if (bar == std::string::npos)
					break;
				from = bar + 1;
			}
			if (!hit)
				continue;
		}
		++n;
		oms_test::failures().clear();
		try {
			c.fn();
		} catch (const std::exception &e) {
			oms_test::failures().push_back(std::string("unexpected exception: ") + e.what());
		} catch (...) {
			oms_test::failures().push_back("unexpected non-standard exception");
		}
		if (oms_test::failures().empty()) {
			++pass;
			std::printf("ok %d - %s %s\n", n, c.layer.c_str(), c.name.c_str());
		} else {
			++fail;
			std::printf("not ok %d - %s %s\n", n, c.layer.c_str(), c.name.c_str());
			for (size_t k = 0; k < oms_test::failures().size(); ++k)
				std::printf("# %s\n", oms_test::failures()[k].c_str());
		}
		std::fflush(stdout);
	}
	std::printf("# total %d pass %d fail %d\n", n, pass, fail);
	return fail == 0 && n > 0 ? 0 : 1;
}
