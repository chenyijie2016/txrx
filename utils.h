#pragma once
#include <uhd/usrp/multi_usrp.hpp>
#include <complex>
#include <csignal>
#include <vector>
#include "usrp_transceiver.h"
// Complex floating-point type for samples (fc32 format)
using complexf = std::complex<float>;


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
