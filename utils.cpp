#include "utils.h"

namespace fs = std::filesystem;
using std::vector;
using std::string;
using std::complex;
using std::format;

/**
 * Loads data from multiple TX files into a buffer
 *
 * This function reads complex floating-point samples from specified files and loads them
 * into a buffer organized by channel. Each channel has its own vector of complex samples.
 *
 * @param config USRP configuration containing the TX file paths
 * @return A vector of vectors containing the loaded complex samples, one per channel
 */
std::vector<std::vector<complexf>> LoadFileToBuffer(const UsrpConfig &config) {
    // Create buffers for each channel
    std::vector<std::vector<complexf>> buffs(config.tx_channels.size());

    // Load data from input files into buffers
    for (size_t index = 0; index < config.tx_channels.size(); ++index) {
        // Get file size using std::filesystem
        std::uintmax_t file_size = fs::file_size(config.tx_files[index]);

        // Calculate number of complex samples
        std::uintmax_t num_samples = file_size / sizeof(complexf);

        // Resize buffer to accommodate all samples
        buffs[index].resize(num_samples);

        // Open input file and read all samples
        std::ifstream infile(config.tx_files[index], std::ios::binary);

        if (!infile.is_open()) {
            UHD_LOG_ERROR("BUFFER-LOAD", format("Cannot open TX file: {}", config.tx_files[index]));
            throw std::runtime_error("Cannot open TX file: " + config.tx_files[index]);
        }

        // Read all samples from the file
        infile.read(reinterpret_cast<char*>(buffs[index].data()), file_size);

        // Check if read was successful
        if (static_cast<std::uintmax_t>(infile.gcount()) != file_size) {
            UHD_LOG_WARNING("BUFFER-LOAD", format("Incomplete read for TX file: {}", config.tx_files[index]));
        }

        infile.close();
    }

    UHD_LOG_INFO("BUFFER-LOAD", format("Loaded {} channels of TX data", buffs.size()));
    return buffs;
}

/**
 * Writes samples from a buffer to files
 *
 * This function takes complex floating-point samples from a buffer and writes them
 * to specified files. Each channel's samples are written to its corresponding file.
 *
 * @param config USRP configuration containing the RX file paths
 * @param buffs Buffer containing complex samples organized by channel
 */
void WriteBufferToFile(const UsrpConfig &config, const std::vector<std::vector<complexf>> &buffs) {
    // Create output files for each channel
    std::vector<std::shared_ptr<std::ofstream>> outfiles;

    for (const auto &rx_file: config.rx_files) {
        outfiles.push_back(std::make_shared<std::ofstream>(rx_file, std::ofstream::binary));

        if (not outfiles.back()->is_open()) {
            UHD_LOG_ERROR("BUFFER-WRITE", format("Cannot open receive file: {}", rx_file));
            throw std::runtime_error("Cannot open receive file: " + rx_file);
        }
        UHD_LOG_INFO("BUFFER-WRITE", format("Rx channel saving to file: {}", rx_file));
    }

    // Write samples from buffer to files
    for (size_t i = 0; i < outfiles.size(); ++i) {
        if (i < buffs.size()) {
            outfiles[i]->write(reinterpret_cast<const char*>(buffs[i].data()),
                               buffs[i].size() * sizeof(complexf));
        }
    }

    // Close all output files
    for (auto &outfile: outfiles) {
        if (outfile->is_open()) {
            outfile->close();
        }
    }

    UHD_LOG_INFO("BUFFER-WRITE", "Write completed! Files written: " << outfiles.size());
}