# txrx - UHD synchronized TX/RX samples

## Overview

txrx is a C++23 Software Defined Radio (SDR) application that performs synchronized transmission and reception of complex samples using USRP hardware via the UHD (USRP Hardware Driver) library. It provides both a command-line utility and an IPC server for remote control, enabling multi-channel configurations with configurable rates, gains, and frequencies.

## Features

- Synchronized transmission and reception
- Multi-channel support with per-channel configuration
- File-based sample input/output (fc32 format)
- Real-time LO lock checking
- Clock synchronization with PPS
- Reference clock support (internal, external, GPSDO)
- Graceful shutdown handling with signal processing
- IPC server using ZeroMQ, shared memory, and Protocol Buffers

## Repository layout

- `txrx_sync.cpp` - Command-line application for synchronized TX/RX with file I/O
- `server.cpp` - IPC server for remote control using ZeroMQ and shared memory
- `usrp_transceiver.cpp` / `usrp_transceiver.h` - USRP device management and configuration
- `utils.cpp` / `utils.h` - Utility functions for file I/O
- `usrp_protocol.proto` - Protocol Buffers definition for IPC communication
- `net.sh` - Network buffer configuration helper
- `CMakeLists.txt` - Build configuration

## Requirements

- UHD (USRP Hardware Driver) 4.9.0 or later
- C++23 compiler (GCC 12+, Clang 15+, or Apple Clang with C++23 support)
- CMake 3.12 or later
- Boost libraries: `program_options`, `thread`
- ZeroMQ (`libzmq`)
- Protocol Buffers compiler and libraries
- USRP hardware device (e.g., X310, B210)

## Build

Once dependencies are installed, the standard build flow is:

```bash
cmake -S . -B build
cmake --build build
```

This produces two executables:

- `txrx_sync` - Command-line application for direct USRP control
- `txrx_server` - IPC server for remote control via Python clients

### Apple Silicon (macOS arm64)

1. Install command line tools:

   ```bash
   xcode-select --install
   ```

2. Install dependencies with Homebrew:

   ```bash
   brew install cmake uhd boost zmq protobuf pkg-config
   ```

3. Configure and build:

   ```bash
   cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix)"
   cmake --build build
   ```

If CMake cannot locate UHD, set `UHD_DIR` or expand `CMAKE_PREFIX_PATH` to the UHD install prefix (for example, `-DCMAKE_PREFIX_PATH="$(brew --prefix uhd)"`).

### Ubuntu (Linux)

1. Install dependencies:

   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential cmake pkg-config \
     libuhd-dev libboost-program-options-dev libboost-thread-dev \
     libzmq3-dev protobuf-compiler libprotobuf-dev
   ```

2. Configure and build:

   ```bash
   cmake -S . -B build
   cmake --build build
   ```

If your distribution ships an older UHD version, install UHD 4.9+ from Ettus Research packages and rerun CMake with `-DUHD_DIR=/path/to/UHDConfig.cmake`.

## Usage

### Basic command-line usage

```bash
./txrx_sync --tx-files tx_data.fc32 --rx-files rx_data.fc32 --freq 915e6 --rate 1e6
```

### IPC server usage

The IPC server allows remote control of USRP operations via Python clients.

#### Starting the IPC server

```bash
./txrx_server --args "addr=192.168.180.2" --port 5555
```

#### Python client example

A Python client can communicate with the server using ZeroMQ and shared memory:

```python
import zmq
import numpy as np
import matplotlib.pyplot as plt
import posix_ipc
import mmap
from usrp_config import UsrpConfig
import usrp_protocol_pb2 as pb
TX_SHM_NAME = "/usrp_tx_shm"

def txrx_ipc(usrp_config: UsrpConfig, tx_buffer: np.ndarray) -> np.ndarray:
    shm_tx = None
    mm_tx = None

    try:
        posix_ipc.unlink_shared_memory(TX_SHM_NAME)
    except posix_ipc.ExistentialError:
        pass

    try:
        shm_tx = posix_ipc.SharedMemory(TX_SHM_NAME, flags=posix_ipc.O_CREX, size=tx_buffer.nbytes)
        mm_tx = mmap.mmap(shm_tx.fd, tx_buffer.nbytes)
        mm_tx.write(tx_buffer.tobytes())

        ctx = zmq.Context()
        sock = ctx.socket(zmq.REQ)
        sock.connect("tcp://localhost:5555")

        request = pb.Request()
        request.cmd = pb.EXECUTE
        request.tx_shm_name = TX_SHM_NAME

        c = request.config
        c.clock_source = str(usrp_config.clock_source)
        c.time_source = str(usrp_config.time_source)
        c.spb = int(usrp_config.spb)
        c.delay = float(usrp_config.delay)
        c.rx_samps = int(usrp_config.rx_samps)
        c.tx_samps = len(tx_buffer) // len(usrp_config.tx_channels)  # 发射总样本数
        # 显式转换类型
        c.tx_channels.extend([int(x) for x in usrp_config.tx_channels])
        c.rx_channels.extend([int(x) for x in usrp_config.rx_channels])
        c.tx_rates.extend([float(x) for x in usrp_config.tx_rates])
        c.rx_rates.extend([float(x) for x in usrp_config.rx_rates])
        c.tx_freqs.extend([float(x) for x in usrp_config.tx_freqs])
        c.rx_freqs.extend([float(x) for x in usrp_config.rx_freqs])
        c.tx_gains.extend([float(x) for x in usrp_config.tx_gains])
        c.rx_gains.extend([float(x) for x in usrp_config.rx_gains])
        c.tx_ants.extend([str(x) for x in usrp_config.tx_ants])
        c.rx_ants.extend([str(x) for x in usrp_config.rx_ants])

        print("Sending Protobuf Tx request...")
        # 发送二进制序列化数据
        sock.send(request.SerializeToString())

        # --- 3. 处理 RX 数据 ---
        raw_res = sock.recv()
        response = pb.Response()
        response.ParseFromString(raw_res) # 解析二进制响应

        mm_tx.close()
        shm_tx.close_fd()
        posix_ipc.unlink_shared_memory(TX_SHM_NAME)

        if response.status == pb.SUCCESS:
            n_ch = response.num_rx_ch
            n_samps = response.rx_nsamps_per_ch
            rx_name = response.rx_shm_name # C++ 也应该返回带 / 的名字

            # 映射 RX 内存
            shm_rx = posix_ipc.SharedMemory(rx_name)

            # 计算总字节数
            total_rx_bytes = n_ch * n_samps * np.dtype(np.complex64).itemsize
            with mmap.mmap(shm_rx.fd, total_rx_bytes, prot=mmap.PROT_READ) as mm_rx:
                # 从内存流构造 numpy 数组并深拷贝
                rx_data = np.frombuffer(mm_rx, dtype=np.complex64).copy()
                rx_data = rx_data.reshape(n_ch, n_samps)

            print(f"Received {n_ch} channels, {n_samps} samples each.")

            # 告知 C++ 释放内存
            release_req = pb.Request()
            release_req.cmd = pb.RELEASE
            sock.send(release_req.SerializeToString())

            # 接收确认
            raw_release_res = sock.recv()
            release_res = pb.Response()
            release_res.ParseFromString(raw_release_res)

            shm_rx.close_fd()

            return rx_data
        else:
            print(f"Error: {response.msg}")
            return None
    except Exception as e:
        print(f"IPC Error: {e}")
        if mm_tx: mm_tx.close()
        if shm_tx: shm_tx.close_fd()
        return None
```

### Command line options

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
| `--tx-rates` | TX sample rates (Hz) (one per channel) | `1e6` |
| `--rx-rates` | RX sample rates (Hz) (one per channel) | `1e6` |
| `--freq` | Center frequency (Hz) for all TX and RX channels (sets both tx/rx freqs when provided) | N/A (use `--tx-freqs`/`--rx-freqs`) |
| `--tx-freqs` | TX center frequencies (Hz) (one per channel) | `915e6` |
| `--rx-freqs` | RX center frequencies (Hz) (one per channel) | `915e6` |
| `--tx-gains` | TX gains (dB) (one per channel) | `10.0` |
| `--rx-gains` | RX gains (dB) (one per channel) | `10.0` |
| `--delay` | Delay before start (seconds) | `1` |
| `--rx_samps` | Number of samples to receive | `5e6` |
| `--clock-source` | Reference: internal, external, gpsdo | `"internal"` |
| `--time-source` | Time Source | `"internal"` |

### Example usage

#### Basic TX/RX with files
```bash
./txrx_sync --tx-files tx_data.fc32 --rx-files rx_data.fc32 --freq 915e6 --rate 1e6
```

#### Multiple channels
```bash
./txrx_sync --tx-channels 0 1 --rx-channels 0 1 --tx-files tx0.fc32 tx1.fc32 --rx-files rx0.fc32 rx1.fc32 --tx-ants TX/RX TX/RX --rx-ants RX2 RX2 --tx-freqs 915e6 925e6 --rx-freqs 915e6 925e6
```

#### Custom parameters
```bash
./txrx_sync --args "addr=192.168.10.2" --tx-freqs 2.4e9 2.5e9 --rx-freqs 2.4e9 2.5e9 --tx-gains 20 25 --rx-gains 15 18 --tx-rates 5e6 5e6 --rx-rates 5e6 5e6 --tx-files tx1.fc32 tx2.fc32 --rx-files rx1.fc32 rx2.fc32
```

## File formats

The application uses complex floating-point (fc32) format for input and output files. Each sample is two 32-bit floats (real and imaginary components), stored in binary format.

## Network configuration

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
