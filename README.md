# UHD Test Project - SDR Application for USRP Devices

## Overview

This project implements a Software Defined Radio (SDR) application that enables synchronized transmission and reception of samples using USRP hardware through the UHD (USRP Hardware Driver) library. The application supports simultaneous TX/RX operations with configurable parameters for frequency, gain, sample rate, and channel selection. It includes both a command-line application and an IPC server for remote control.

## Features

- Synchronized transmission and reception
- Multi-channel support
- File-based sample input/output (fc32 format)
- Configurable parameters (frequency, gain, sample rate, etc.)
- Real-time LO lock checking
- Clock synchronization with PPS
- Reference clock support (internal, external, GPSDO)
- Graceful shutdown handling with signal processing
- UsrpTransceiver class for encapsulated USRP operations
- IPC server using ZeroMQ and shared memory for remote control
- Protocol Buffers for message serialization

## Project Structure

### Core Components

- `txrx_sync.cpp` - Main application implementing synchronized TX/RX functionality with file I/O
- `server.cpp` - IPC server for remote control via Python clients using ZeroMQ and shared memory
- `usrp_transceiver.cpp` / `usrp_transceiver.h` - USRP device management and configuration handling
- `utils.cpp` / `utils.h` - Utility functions for file I/O operations
- `usrp_protocol.proto` - Protocol Buffers definition for IPC communication
- `net.sh` - System network buffer configuration script
- `CMakeLists.txt` - Build configuration

## Dependencies

- UHD (USRP Hardware Driver) 4.9.0 or later
- Boost libraries (program_options, thread)
- ZeroMQ (libzmq) for IPC communication
- Protocol Buffers for message serialization
- C++23 compiler or later
- USRP hardware device (e.g., X310, B210)

## Building

```bash
mkdir build
cd build
cmake ..
make
```

This creates two executables:
- `txrx_sync` - Command-line application for direct USRP control
- `txrx_server` - IPC server for remote control via Python clients

## Usage

### Basic Command-Line Usage

```bash
./txrx_sync --tx-files tx_data.fc32 --rx-files rx_data.fc32 --freq 915e6 --rate 1e6
```

### IPC Server Usage

The project includes an IPC server that allows remote control of USRP operations via Python clients:

#### Starting the IPC Server

```bash
./txrx_server --args "addr=192.168.180.2" --port 5555
```

#### Python Client Example

A Python client can communicate with the server using ZeroMQ and shared memory:

```python
import zmq
import numpy as np
from multiprocessing import shared_memory

# Connect to the server
context = zmq.Context()
socket = context.socket(zmq.REQ)
socket.connect("tcp://localhost:5555")

# Prepare configuration
config = {
    "clock_source": "internal",
    "time_source": "internal",
    "spb": 2500,
    "delay": 1.0,
    "rx_samps": 5000000,
    "tx_samps": 5000000,
    "tx_channels": [0],
    "rx_channels": [1],
    "tx_rates": [1e6],
    "rx_rates": [1e6],
    "tx_freqs": [915e6],
    "rx_freqs": [915e6],
    "tx_gains": [10.0],
    "rx_gains": [10.0],
    "tx_ants": ["TX/RX"],
    "rx_ants": ["RX2"]
}

# Create TX data in shared memory
tx_data = np.exp(1j * 2 * np.pi * np.arange(5000000) * 0.1).astype(np.complex64)
tx_data_bytes = tx_data.tobytes()
shm = shared_memory.SharedMemory(create=True, size=len(tx_data_bytes), name="/usrp_tx_shm")
shm.buf[:len(tx_data_bytes)] = tx_data_bytes

# Send EXECUTE command using Protocol Buffers
# (Actual implementation would serialize the protobuf message)

# Cleanup
shm.close()
shm.unlink()
socket.close()
context.term()
```

### Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `--help, -h` | Show this help message | N/A |
| `--args` | USRP device address string | `"addr=192.168.180.2"` |
| `--tx-files` | TX data files (fc32 format) | `"tx_data_fc32.bin"` |
| `--rx-files` | RX data files (fc32 format) | `"rx_data_fc32.bin"` |
| `--tx-ants` | TX antenna selections (one per channel) | `"TX/RX"` |
| `--rx-ants` | RX antenna selections (one per channel) | `"RX2"` |
| `--tx-channels` | TX channels (space separated) | `0` |
| `--rx-channels` | RX channels (space separated) | `1` |
| `--spb` | Samples per buffer | `2500` |
| `--rate` | Sample rate (Hz) for both TX and RX | N/A (sets both tx/rx rates if specified) |
| `--tx-rates` | TX Sample rates (Hz) (one per channel) | `1e6` |
| `--rx-rates` | RX Sample rates (Hz) (one per channel) | `1e6` |
| `--freq` | Center frequency (Hz) for ALL Tx and Rx CHANNELS | N/A (sets both tx/rx freqs if specified) |
| `--tx-freqs` | TX Center frequencies (Hz) (one per channel) | `915e6` |
| `--rx-freqs` | RX Center frequencies (Hz) (one per channel) | `915e6` |
| `--tx-gains` | TX gains (dB) (one per channel) | `10.0` |
| `--rx-gains` | RX gains (dB) (one per channel) | `10.0` |
| `--delay` | Delay before start (seconds) | `1` |
| `--rx_samps` | Number of samples to receive | `5e6` |
| `--clock-source` | Reference: internal, external, gpsdo | `"internal"` |
| `--time-source` | Time Source | `"internal"` |

### Example Usage

#### Basic TX/RX with Files
```bash
./txrx_sync --tx-files tx_data.fc32 --rx-files rx_data.fc32 --freq 915e6 --rate 1e6
```

#### Multiple Channels
```bash
./txrx_sync --tx-channels 0 1 --rx-channels 0 1 --tx-files tx0.fc32 tx1.fc32 --rx-files rx0.fc32 rx1.fc32 --tx-ants TX/RX TX/RX --rx-ants RX2 RX2 --tx-freqs 915e6 925e6 --rx-freqs 915e6 925e6
```

#### Custom Parameters
```bash
./txrx_sync --args "addr=192.168.10.2" --tx-freqs 2.4e9 2.5e9 --rx-freqs 2.4e9 2.5e9 --tx-gains 20 25 --rx-gains 15 18 --tx-rates 5e6 5e6 --rx-rates 5e6 5e6 --tx-files tx1.fc32 tx2.fc32 --rx-files rx1.fc32 rx2.fc32
```

## File Formats

The application uses complex floating-point (fc32) format for input and output files. Each sample is two 32-bit floats (real and imaginary components), stored in binary format.

## Network Configuration

For optimal network performance with USRP devices, run the network buffer configuration script before starting:

```bash
sudo ./net.sh
```

This script increases the network buffer sizes to improve data throughput between the host computer and USRP device.

## Architecture

The project is organized around the `UsrpTransceiver` class which encapsulates USRP device management and operations:

- `UsrpTransceiver` class handles device initialization, configuration validation, and TX/RX operations
- `UsrpConfig` struct centralizes all configuration parameters
- File I/O operations are handled by utility functions in `utils.cpp`
- The IPC server uses Protocol Buffers for message serialization and shared memory for efficient data transfer
- Signal handling for graceful shutdown is implemented with atomic flags

## Troubleshooting

- **Device not found**: Verify USRP address in `--args` parameter and network connectivity
- **Clock reference issues**: Ensure proper cabling for external references if using external/gpsdo options
- **Performance issues**: Run `net.sh` as root to configure optimal network buffer sizes
- **LO unlock errors**: Check antenna connections and verify frequency settings are within device specifications
- **Shared memory errors**: Ensure proper cleanup of shared memory segments between runs

## Development

The project follows these key design principles:
- Thread-safe operations for concurrent TX/RX
- Proper resource cleanup and exception handling
- Consistent parameter validation
- Clear organization of USRP configuration parameters
- Support for multi-channel operations
- Efficient data handling with minimal copying
- Robust error handling and logging

For development, please follow existing code style and maintain thread safety in all operations.