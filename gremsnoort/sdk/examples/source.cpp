// sdk
#include <gremsnoort/sdk/forward/unit.hpp>
#include <gremsnoort/sdk/forward/program_options.hpp>

// conan
#include <boost/lockfree/queue.hpp>

struct options_t {
	std::size_t producers_count = 4;
	std::size_t consumers_count = 4;
	std::size_t messages_count = 1000;
	std::size_t repetitions_count = 1;
	std::size_t sleep_on_send = 0;
};

template<gremsnoort::sdk::Queue Q>
class unit_impl final : public gremsnoort::sdk::unit_t<Q, gremsnoort::sdk::unit::mode_e::blocking_push> {
	using base_t = gremsnoort::sdk::unit_t<Q, gremsnoort::sdk::unit::mode_e::blocking_push>;

	virtual auto on_start(const std::size_t index) noexcept -> void final {
		std::printf("%s\n", std::format("{} :: #{} :: STARTED +++", gremsnoort::sdk::get_tid(), index).data());
	}
	virtual auto on_stop(const std::size_t index) noexcept -> void final {
		std::printf("%s\n", std::format("{} :: #{} :: STOPPED ---", gremsnoort::sdk::get_tid(), index).data());
	}
	virtual auto on_exception(const std::size_t index, std::exception& ex) noexcept -> void final {
		std::printf("%s\n", std::format("{} :: #{} :: EXCEPTION CAUGHT: {}", gremsnoort::sdk::get_tid(), index, ex.what()).data());
	}
	virtual auto on_idle(const std::size_t index) -> void final {

	}
	virtual auto on_data(const std::size_t index, typename Q::value_type& data) -> void final {
		std::printf("%s\n", std::format("{} :: #{} :: Data `{}`", gremsnoort::sdk::get_tid(), index, data).data());
	}

public:
	explicit unit_impl(const std::size_t scale_factor, const std::size_t source_sz = 1024)
		: base_t(scale_factor, source_sz) {}
};

template<gremsnoort::sdk::Queue Q>
auto run(const options_t& opts) {
	auto unit = unit_impl<Q>(opts.consumers_count, 1024);
	unit.start();

	auto producer = [&]() {
		for (std::size_t i = 0; i < opts.messages_count; ++i) {
			if (opts.sleep_on_send > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(opts.sleep_on_send)); // @NOTE in ms !!!
			}
			auto r = unit.produce(i);
			assert(r);
		}
	};

	std::vector<std::thread> producers;
	for (std::size_t i = 0; i < opts.producers_count; ++i) {
		producers.emplace_back(producer);
	}
	for (auto& p : producers) {
		p.join();
	}

	unit.stop();
	unit.join();
}

int main(int argc, char* argv[]) {

	gremsnoort::sdk::program_options_t desc(std::string(__FILE__), std::string("source_example"));
	static constexpr auto
		help_o = "help",
		prod_o = "p", prod_descr = "Producers count",
		cons_o = "c", cons_descr = "Consumers count",
		msgc_o = "m", msgc_descr = "Message count per producer",
		reps_o = "r", reps_descr = "Repetitions count",
		sleep_o = "s", sleep_descr = "Sleep on send per producer, in microseconds";

	desc.add_options()
		(help_o, "produce help message")
		(prod_o, prod_descr, cxxopts::value<std::size_t>())
		(cons_o, cons_descr, cxxopts::value<std::size_t>())
		(msgc_o, msgc_descr, cxxopts::value<std::size_t>())
		(reps_o, reps_descr, cxxopts::value<std::size_t>())
		(sleep_o, sleep_descr, cxxopts::value<std::size_t>())
		;

	auto result = desc.parse(argc, argv);

	if (result.count(help_o)) {
		std::cout << desc.help() << "\n";
		return 1;
	}

	options_t opts;

	desc.retrieve_opt(opts.producers_count, prod_o, prod_descr, result, false);
	desc.retrieve_opt(opts.consumers_count, cons_o, cons_descr, result, false);
	desc.retrieve_opt(opts.messages_count, msgc_o, msgc_descr, result, false);
	desc.retrieve_opt(opts.repetitions_count, reps_o, reps_descr, result, false);
	desc.retrieve_opt(opts.sleep_on_send, sleep_o, sleep_descr, result, false);

	if (opts.producers_count <= 0 || opts.consumers_count <= 0 || opts.messages_count <= 0) {
		return 0;
	}

	using type_t = int64_t;
	//using queue_t = gremsnoort::lockfree::queue_traits<type_t>::queue_t;
	using queue_t = ::boost::lockfree::queue<type_t>;

	for (std::size_t i = 0; i < opts.repetitions_count; ++i) {
		run<queue_t>(opts);
	}

	return 0;
}
