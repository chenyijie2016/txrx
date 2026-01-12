#pragma once

#include <atomic>
#include <complex>
#include <string>
#include <uhd/usrp/multi_usrp.hpp>
#include <vector>
using complexf = std::complex<float>;

struct UsrpConfig {
    std::string clock_source, time_source;
    std::vector<size_t> tx_channels, rx_channels;
    size_t spb; // Samples per buffer
    double rx_bw, tx_bw, delay;
    size_t nsamps{0};
    std::vector<double> tx_rates, rx_rates;
    std::vector<std::string> tx_files, rx_files;
    std::vector<double> tx_freqs, rx_freqs;
    std::vector<double> tx_gains, rx_gains;
    std::vector<std::string> tx_ants, rx_ants;
};

class UsrpTransceiver {
private:
    uhd::usrp::multi_usrp::sptr usrp;
    UsrpConfig usrp_config{};
    std::atomic<bool> &stop_signal; // Pointer to the stop signal flag
    /**
     * Applies the USRP TuneRequest to the device
     */
    void ApplyTuneRequest(const UsrpConfig &config);
    void ApplyTimeSync(const UsrpConfig &config);
public:
    uhd::time_spec_t start_time;

    explicit UsrpTransceiver(const std::string &args, std::atomic<bool> &stop_signal);

    ~UsrpTransceiver() = default;

    /**
     * Validates the USRP configuration
     * @return true if configuration is valid, false otherwise
     */
    bool ValidateConfiguration(const UsrpConfig &config);

    /**
     * Applies the USRP configuration to the device
     */
    void ApplyConfiguration(const UsrpConfig &config);


    /**
     * Transmits samples from a buffer to USRP using a streaming approach
     *
     * @param buffs Buffer containing complex samples organized by channel
     */
    void TransmitFromBuffer(std::vector<std::vector<complexf>> &buffs);

    /**
     * Receives samples from USRP to a buffer using a streaming approach
     *
     * @return A vector of vectors containing the received complex samples, one per channel
     */
    std::vector<std::vector<complexf>> ReceiveToBuffer();

    // Getter methods
    // uhd::usrp::multi_usrp::sptr &get_usrp() { return usrp; }
    // [[nodiscard]] const UsrpConfig &get_config() const { return usrp_config; }
};
