#include <atomic>
#include <boost/algorithm/string.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/program_options.hpp>
#include <csignal>
#include <format>
#include <future>
#include <nlohmann/json.hpp>
#include <uhd/convert.hpp>
#include <uhd/exception.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <vector>
#include <zmq.hpp>
#include "usrp_transceiver.h"
#include "utils.h"

namespace po = boost::program_options;
namespace stdr = std::ranges;
using json = nlohmann::json;

using std::format;
using std::string;
using std::vector;

// External global flag for signal handling (defined in workers.cpp)
std::atomic<bool> stop_signal_called{false};

/**
 * Signal handler for graceful shutdown
 * Sets the stop_signal_called flag to true when SIGINT is received
 */
void sig_int_handler(int) {
    stop_signal_called = true;
    UHD_LOG_INFO("SIGNAL", "SIGINT received, stopping...")
}

int UHD_SAFE_MAIN(int argc, char *argv[]) {
    UsrpConfig config{};
    string args;
    uint16_t port;

    po::options_description desc("Command line options");
    auto option = desc.add_options();
    option("help,h", "Show this help message");
    option("port", po::value<uint16_t>(&port)->default_value(5555), "zmq port number");
    option("args", po::value<string>(&args)->default_value("addr=192.168.180.2"), "USRP device args");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.contains("help")) {
            std::cout << desc << std::endl;
            return EXIT_SUCCESS;
        }

        po::notify(vm);
    } catch (const std::exception &e) {
        UHD_LOG_ERROR("MAIN", "Error parsing command line: " << e.what())
        std::cerr << desc << std::endl;
        return EXIT_FAILURE;
    }
    // Register signal handler for graceful shutdown
    UHD_LOG_INFO("SYSTEM", "Registering signal handler")
    std::signal(SIGINT, &sig_int_handler);

    // Create UsrpTransceiver instance
    UsrpTransceiver transceiver(args);
    UHD_LOG_TRACE("SYSTEM", "Starting server--Ctrl^C to stop")

    zmq::context_t ctx{1};
    zmq::socket_t sock{ctx, zmq::socket_type::rep};

    sock.bind(format("tcp://*:{}", port).c_str());

    while (not stop_signal_called) {
        // TODO

    }


    return EXIT_SUCCESS;
}
