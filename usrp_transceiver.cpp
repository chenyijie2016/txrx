#include "usrp_transceiver.h"

#include <utility>
#include <ranges>
#include <chrono>
#include <filesystem>
// External global flag for signal handling (defined in txrx_sync.cpp)
extern std::atomic<bool> stop_signal_called;

namespace fs = std::filesystem;
namespace stdr = std::ranges;
namespace stdv = std::views;

using std::vector;
using std::string;
using std::complex;
using std::format;
using namespace std::chrono_literals;


UsrpTransceiver::UsrpTransceiver(const std::string &args) {
    usrp = uhd::usrp::multi_usrp::make(args);
    UHD_LOG_INFO("UsrpTransceiver", format("Creating USRP device with args: {}", args));
}



bool UsrpTransceiver::ValidateConfiguration(const UsrpConfig &config) {
    // Check channel validity
    size_t total_tx_channels = usrp->get_tx_num_channels();
    size_t total_rx_channels = usrp->get_rx_num_channels();

    {
        std::ostringstream oss;
        oss << "TX channels: ";
        for (size_t ch: config.tx_channels) oss << ch << " ";
        UHD_LOG_INFO("CHECK", oss.str());
    }

    {
        std::ostringstream oss;
        oss << "RX channels: ";
        for (size_t ch: config.rx_channels) oss << ch << " ";
        UHD_LOG_INFO("CHECK", oss.str());
    }

    if (stdr::any_of(config.tx_channels, [&](const auto &ch) {
        return ch >= total_tx_channels;
    })) {
        UHD_LOG_ERROR("CHECK", "TX channels are not supported");
        return false;
    }

    if (stdr::any_of(config.rx_channels, [&](const auto &ch) {
        return ch >= total_rx_channels;
    })) {
        UHD_LOG_ERROR("CHECK", "RX channels are not supported");
    }
    std::vector<size_t> tx_sizes = {
        config.tx_channels.size(),
        config.tx_files.size(),
        config.tx_ants.size(),
        config.tx_gains.size(),
        config.tx_freqs.size()
    };
    if (stdr::adjacent_find(tx_sizes, std::not_equal_to{}) != tx_sizes.end()) {
        UHD_LOG_ERROR("CHECK", "Tx configurations mismatch!");
        return false;
    }

    std::vector<size_t> rx_sizes = {
        config.rx_channels.size(),
        config.rx_files.size(),
        config.rx_ants.size(),
        config.rx_gains.size(),
        config.rx_freqs.size()
    };
    if (stdr::adjacent_find(rx_sizes, std::not_equal_to{}) != rx_sizes.end()) {
        UHD_LOG_ERROR("CHECK", "Rx configurations mismatch!");
        return false;
    }


    // Verify TX input files exist
    if (not stdr::all_of(config.tx_files, [](const std::string &filename) {
        return fs::exists(filename);
    })) {
        UHD_LOG_ERROR("CHECK", "TX input files do not exist!")
        return false;
    }

    // Verify all TX files have the same size
    std::vector<std::uintmax_t> sizes = stdr::to<std::vector>(
        config.tx_files
        | stdv::transform([](const std::string &f) {
            return fs::file_size(f);
        }));


    if (stdr::adjacent_find(sizes, std::not_equal_to{}) != sizes.end()) {
        UHD_LOG_ERROR("CHECK", "Tx file sizes mismatch")
        return false;
    }
    UHD_LOG_INFO("CHECK", "The input parameters appear to be correct.")
    return true;
}

void UsrpTransceiver::ApplyConfiguration(const UsrpConfig &config) {
    this->_config = config;

    UHD_LOG_INFO("CONFIG", format("====== Configuring Tx ======"))
    for (const auto &[index, ch]: stdv::enumerate(config.tx_channels)) {
        UHD_LOG_INFO("CONFIG", format("====== Tx Channel {}", ch))

        // gain
        usrp->set_tx_gain(config.tx_gains[index], ch);
        UHD_LOG_INFO("CONFIG", format("Gain: {:.2f} dB", usrp->get_tx_gain(ch)))

        // ant
        usrp->set_tx_antenna(config.tx_ants[index], ch);
        UHD_LOG_INFO("CONFIG", format("Ant : {}", usrp->get_tx_antenna(ch)));

        // rate
        usrp->set_tx_rate(config.tx_rates[index], ch);
        UHD_LOG_INFO("CONFIG", std::format("Rate: {:.3f} Msps", usrp->get_tx_rate(ch)/1e6));
    }
    UHD_LOG_INFO("CONFIG", format("============================"))
    UHD_LOG_INFO("CONFIG", format("====== Configuring Rx ======"))
    // Configure RX channels
    for (const auto &[index, ch]: stdv::enumerate(config.rx_channels)) {
        UHD_LOG_INFO("CONFIG", format("====== Rx Channel {}", ch))

        usrp->set_rx_gain(config.rx_gains[index], ch);
        UHD_LOG_INFO("CONFIG", format("Gain: {:.1f} dB", usrp->get_rx_gain(ch)))


        usrp->set_rx_antenna(config.rx_ants[index], ch);
        UHD_LOG_INFO("CONFIG", format("Ant : {}", usrp->get_rx_antenna(ch)))

        // rate
        usrp->set_rx_rate(config.rx_rates[index], ch);
        UHD_LOG_INFO("CONFIG", std::format("Rate: {:.3f} Msps", usrp->get_rx_rate(ch)/1e6));
    }
    UHD_LOG_INFO("CONFIG", format("============================"))
    // Configure clock reference
    UHD_LOG_INFO("CONFIG", format("Setting clock reference to: {}", config.clock_source));
    usrp->set_clock_source(config.clock_source);

    // Configure time reference
    if (config.clock_source == "external" || config.clock_source == "gpsdo") {
        UHD_LOG_INFO("CONFIG", std::format("Setting time reference to: {}", config.clock_source));
        usrp->set_time_source("external");
    } else {
        usrp->set_time_source("internal");
    }

    // Wait for PPS sync and set time
    UHD_LOG_INFO("CONFIG", "Waiting for PPS sync and setting time...");
    // usrp->set_time_unknown_pps(uhd::time_spec_t(0.0));

    const uhd::time_spec_t last_pps_time = usrp->get_time_last_pps();
    while (last_pps_time == usrp->get_time_last_pps()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // This command will be processed fairly soon after the last PPS edge:
    usrp->set_time_next_pps(uhd::time_spec_t(0.0));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    // Display current USRP time
    UHD_LOG_INFO("CONFIG", std::format("Current USRP time: {:.6f} seconds", usrp->get_time_now().get_real_secs()));

    // Sync tx and tx
    UHD_LOG_INFO("CONFIG", "Start Sync tune Request for Tx and Rx")
    usrp->set_command_time(uhd::time_spec_t(0.3),
                           uhd::usrp::multi_usrp::ALL_MBOARDS);
    for (const auto &[ch, freq_]: stdv::zip(config.tx_channels, config.tx_freqs)) {
        uhd::tune_request_t tune_req(freq_);
        tune_req.args = uhd::device_addr_t("mode_n=integer");
        usrp->set_tx_freq(tune_req, ch);
    }
    for (const auto &[ch, freq_]: stdv::zip(config.rx_channels, config.rx_freqs)) {
        uhd::tune_request_t tune_req(freq_);
        tune_req.args = uhd::device_addr_t("mode_n=integer");
        usrp->set_rx_freq(tune_req, ch);
    }

    usrp->clear_command_time(uhd::usrp::multi_usrp::ALL_MBOARDS);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    for (const size_t ch: config.tx_channels) {
        UHD_LOG_INFO("CONFIG", std::format("Tx channel {} freq set to {:.3f} MHz", ch, usrp->get_tx_freq(ch)/1e6));
    }
    for (const size_t ch: config.rx_channels) {
        UHD_LOG_INFO("CONFIG", std::format("Rx channel {} freq set to {:.3f} MHz", ch, usrp->get_rx_freq(ch)/1e6));
    }


    // Check LO lock status
    UHD_LOG_INFO("SYSTEM", "Checking LO lock status...");

    // Check TX LO lock
    for (size_t ch: config.tx_channels) {
        if (auto sensor_names = usrp->get_tx_sensor_names(ch);
            std::ranges::find(sensor_names, "lo_locked") != sensor_names.end()) {
            auto lo_locked = usrp->get_tx_sensor("lo_locked", ch);
            UHD_LOG_INFO("SYSTEM", format("Checking Tx(ch={}): {}",ch, lo_locked.to_pp_string()));
            UHD_ASSERT_THROW(lo_locked.to_bool());
        }
    }

    // Check RX LO lock
    for (size_t ch: config.rx_channels) {
        if (auto sensor_names = usrp->get_rx_sensor_names(ch);
            std::ranges::find(sensor_names, "lo_locked") != sensor_names.end()) {
            auto lo_locked = usrp->get_rx_sensor("lo_locked", ch);
            UHD_LOG_INFO("SYSTEM", format("Checking Rx(ch={}): {}",ch, lo_locked.to_pp_string()));
            UHD_ASSERT_THROW(lo_locked.to_bool());
        }
    }
    // Check Ref lock
    UHD_LOG_INFO("SYSTEM", "Checking REF lock status...");
    size_t num_mboards = usrp->get_num_mboards();
    if (config.clock_source == "external") {
        for (auto mboard: stdv::iota(static_cast<size_t>(0), num_mboards)) {
            if (auto sensor_names = usrp->get_mboard_sensor_names(mboard);
                std::ranges::find(sensor_names, "ref_locked") != sensor_names.end()) {
                auto ref_locked = usrp->get_mboard_sensor("ref_locked", mboard);
                UHD_LOG_INFO("SYSTEM", format("Checking mboard(={}): {}", mboard, ref_locked.to_pp_string()));
                UHD_ASSERT_THROW(ref_locked.to_bool());
            }
        }
    }
    if (config.clock_source == "mimo") {
        for (auto mboard: stdv::iota(static_cast<size_t>(0), num_mboards)) {
            if (auto sensor_names = usrp->get_mboard_sensor_names(mboard);
                std::ranges::find(sensor_names, "mimo_locked") != sensor_names.end()) {
                auto ref_locked = usrp->get_mboard_sensor("mimo_locked", mboard);
                UHD_LOG_INFO("SYSTEM", format("Checking mboard(={}): {}", mboard, ref_locked.to_pp_string()));
                UHD_ASSERT_THROW(ref_locked.to_bool());
            }
        }
    }

    start_time = usrp->get_time_now() + uhd::time_spec_t(config.delay);
    UHD_LOG_INFO(
        "SYSTEM",
        std::format("Start time: {:.3f} seconds in the future (absolute time: {:.6f})", config.delay, start_time.
            get_real_secs()));
}

void UsrpTransceiver::TransmitFromBuffer(std::vector<std::vector<complexf>> &buffs) {
    // Create TX stream
    UHD_LOG_TRACE("STREAM", "Creating TX stream");
    uhd::stream_args_t tx_stream_args("fc32", "sc16");
    tx_stream_args.channels = _config.tx_channels;
    uhd::tx_streamer::sptr tx_stream = usrp->get_tx_stream(tx_stream_args);

    // Initialize TX metadata
    uhd::tx_metadata_t md;
    md.start_of_burst = false;
    md.end_of_burst = false;
    md.has_time_spec = true;
    md.time_spec = start_time;
    double timeout = 5;
    bool first_packet = true;

    // Create buffer pointers for transmission
    std::vector<complexf *> buff_ptrs;
    for (auto &buff: buffs) {
        buff_ptrs.push_back(buff.data());
    }

    size_t num_samps_transmitted = 0;

    // Track buffer state
    size_t current_sample_idx = 0; // Current position in the buffer
    size_t total_samples = buffs.empty() ? 0 : buffs[0].size(); // Total samples to transmit

    UHD_LOG_INFO("TX-BUFFER", format("Starting transmission from buffer with {} samples per channel", total_samples))

    while (!stop_signal_called && current_sample_idx < total_samples) {
        /* ---------- Send samples from buffer ---------- */
        size_t samps_to_send = std::min(_config.spb, total_samples - current_sample_idx);

        std::vector<complexf *> offset_ptrs(tx_stream->get_num_channels());
        for (size_t ch = 0; ch < tx_stream->get_num_channels(); ++ch) {
            offset_ptrs[ch] = buffs[ch].data() + current_sample_idx;
        }

        size_t samps_sent = tx_stream->send(
            offset_ptrs,
            samps_to_send,
            md,
            timeout
        );

        if (samps_sent == 0) {
            UHD_LOG_WARNING("TX-BUFFER",
                            format("send() returned 0 samples [{}/{}]", current_sample_idx, total_samples));
            continue;
        }
        num_samps_transmitted += samps_sent;
        current_sample_idx += samps_sent;
        first_packet = false;
        timeout = 0.1;
    }

    // Finalize transmission
    md.end_of_burst = true;
    tx_stream->send("", 0, md);

    UHD_LOG_INFO("TX-BUFFER", "Transmit completed! Samples sent: " << num_samps_transmitted);
}

std::vector<std::vector<complexf>> UsrpTransceiver::ReceiveToBuffer() {
    // Create RX stream
    UHD_LOG_TRACE("STREAM", "Creating RX stream");
    uhd::stream_args_t rx_stream_args("fc32", "sc16");
    rx_stream_args.channels = _config.rx_channels;
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(rx_stream_args);


    // Create buffers for each channel
    std::vector<std::vector<complexf>> buffs(rx_stream->get_num_channels(), std::vector<complexf>(_config.nsamps));
    std::vector<complexf *> buff_ptrs;
    for (auto &buff: buffs) {
        buff_ptrs.push_back(buff.data());
    }

    // Initialize reception parameters
    bool first_packet = true;
    double timeout = 5;
    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    stream_cmd.num_samps = _config.nsamps;
    stream_cmd.stream_now = false;
    stream_cmd.time_spec = start_time;

    UHD_LOG_INFO("RX-BUFFER", format("Starting reception, will receive {} samples", _config.nsamps))
    UHD_LOG_DEBUG("RX-BUFFER", format("Reception start time: {:.3f} seconds", start_time.get_real_secs()))

    // Issue stream command to start reception
    rx_stream->issue_stream_cmd(stream_cmd);

    uhd::rx_metadata_t md;
    size_t num_samps_received = 0;

    // Main reception loop
    while (not stop_signal_called && (num_samps_received < _config.nsamps)) {
        std::vector<complexf *> offset_ptrs(rx_stream->get_num_channels());
        for (size_t ch = 0; ch < rx_stream->get_num_channels(); ++ch) {
            offset_ptrs[ch] = buffs[ch].data() + num_samps_received;
        }


        size_t num_rx_samps = rx_stream->recv(offset_ptrs, _config.spb, md, timeout);

        if (first_packet) {
            timeout = 0.1; // Reduce timeout after first packet
            first_packet = false;
        }

        // Handle different error conditions
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            UHD_LOG_WARNING("RX-BUFFER", "RX channel received timeout.");
            continue;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
            UHD_LOG_WARNING("RX-BUFFER", "RX channel received overflow.");
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            UHD_LOG_ERROR("RX-BUFFER", "RX channel received error: " << md.strerror());
            throw std::runtime_error("Receive error: " + md.strerror());
        }

        num_samps_received += num_rx_samps;
    }

    UHD_LOG_INFO("RX-BUFFER", "Receive completed! Samples received: " << num_samps_received);

    // Resize buffers to actual received sample count
    for (auto &buff: buffs) {
        buff.resize(num_samps_received);
    }

    return buffs;
}