#pragma once
#include <uhd/usrp/multi_usrp.hpp>
#include <format>
#include <complex>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <future>
#include <ranges>

// Complex floating-point type for samples (fc32 format)
using complexf = std::complex<float>;

struct UsrpConfig {
    std::string args;
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

/**
 * Loads data from multiple TX files into a buffer
 *
 * This function reads complex floating-point samples from specified files and loads them
 * into a buffer organized by channel. Each channel has its own vector of complex samples.
 *
 * @param config USRP configuration containing the TX file paths
 * @return A vector of vectors containing the loaded complex samples, one per channel
 */
std::vector<std::vector<complexf>> LoadFileToBuffer(const UsrpConfig &config);

/**
 * Writes samples from a buffer to files
 *
 * This function takes complex floating-point samples from a buffer and writes them
 * to specified files. Each channel's samples are written to its corresponding file.
 *
 * @param config USRP configuration containing the RX file paths
 * @param buffs Buffer containing complex samples organized by channel
 */
void WriteBufferToFile(const UsrpConfig &config, const std::vector<std::vector<complexf>> &buffs);
