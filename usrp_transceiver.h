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
    double delay;
    size_t rx_samps{0};
    size_t tx_samps{0};
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
    /**
     * Applies the USRP TuneRequest to the device
     */
    void ApplyTuneRequest(const UsrpConfig &config);


    void ApplyTimeSync(const UsrpConfig &config);

public:
    uhd::time_spec_t start_time;

    explicit UsrpTransceiver(const std::string &args);

    ~UsrpTransceiver() = default;

    /**
     * Validates the USRP configuration
     * @return true if configuration is valid, false otherwise
     */
    bool ValidateConfiguration(const UsrpConfig &config,const bool require_file=true);

    /**
     * Applies the USRP configuration to the device
     */
    void ApplyConfiguration(const UsrpConfig &config);


    /**
     * Transmits samples from a buffer to USRP using a streaming approach
     *
     * @param buffs Buffer containing complex samples organized by channel
     */
    void TransmitFromBuffer(std::vector<std::vector<complexf>> &buffs,std::atomic<bool> &stop_signal);

    /**
     * Receives samples from USRP to a buffer using a streaming approach
     *
     * @return A vector of vectors containing the received complex samples, one per channel
     */
    std::vector<std::vector<complexf>> ReceiveToBuffer(std::atomic<bool> &stop_signal);
};
