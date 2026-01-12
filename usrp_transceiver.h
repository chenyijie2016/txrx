#pragma once

#include <uhd/usrp/multi_usrp.hpp>
#include <vector>
#include <string>
#include <complex>
using complexf = std::complex<float>;

struct UsrpConfig {
    std::string clock_source, time_source;
    std::vector<size_t> tx_channels, rx_channels;
    size_t spb; // Samples per buffer
    double rate, rx_bw, tx_bw, delay, freq;
    size_t nsamps{0}; // Number of samples to receive, 0 means until TX complete
    std::vector<double> tx_rates, rx_rates;
    std::vector<std::string> tx_files, rx_files;
    std::vector<double> tx_freqs, rx_freqs;
    std::vector<double> tx_gains, rx_gains;
    std::vector<std::string> tx_ants, rx_ants;
};

class UsrpTransceiver {
private:
    uhd::usrp::multi_usrp::sptr usrp;
    UsrpConfig _config{};

public:
    uhd::time_spec_t start_time;

    explicit UsrpTransceiver(const std::string &args);

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
     * @param spb Samples per buffer - number of samples to process in each iteration
     * @param start_time Time specification for when transmission should begin
     */
    void TransmitFromBuffer(std::vector<std::vector<complexf> > &buffs);

    /**
     * Receives samples from USRP to a buffer using a streaming approach
     *
     * @param spb Samples per buffer - number of samples to process in each iteration
     * @param start_time Time specification for when reception should begin
     * @param num_samps_to_recv Total number of samples to receive before stopping
     * @return A vector of vectors containing the received complex samples, one per channel
     */
    std::vector<std::vector<complexf> > ReceiveToBuffer();

    // Getter methods
    uhd::usrp::multi_usrp::sptr &get_usrp() { return usrp; }
    const UsrpConfig &get_config() const { return _config; }
};
