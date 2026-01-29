# AES67 macOS Driver

## Building the Project

### Prerequisites

Before building the AES67 driver, you need to have the following prerequisites installed:

1. **Xcode Command Line Tools**:
   ```bash
   xcode-select --install
   ```

2. **CMake** (version 3.20 or higher):
   ```bash
   brew install cmake
   ```

3. **ASPL Library**: This project depends on the ASPL (Audio Server Plug-in Library). You can either:
   - Install it system-wide using Homebrew:
     ```bash
     brew install libaspl
     ```
   - Or include it as a submodule in the `external/libASPL` directory.

### Cloning the Repository

To properly clone the repository with all submodules:

```bash
git clone --recursive https://github.com/maxajbarlow/AES67_macos_Driver.git
```

If you've already cloned the repo without submodules:

```bash
git submodule update --init --recursive
```

### Building

#### Using CMake (Recommended)

1. Create a build directory:
   ```bash
   mkdir build
   cd build
   ```

2. Configure the project:
   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Release
   ```

3. Build the project:
   ```bash
   cmake --build . --config Release
   ```

4. Install (optional):
   ```bash
   sudo cmake --build . --target install
   ```

#### Alternative Build Method

You can also use the provided Makefile:

```bash
make
```

### Submodule Management

The project uses the following submodules:
- `external/libASPL` - Audio Server Plug-in Library
- `NetworkEngine/PTP/vendor/ptpd` - Precision Time Protocol daemon (embedded)

To update submodules:
```bash
git submodule update --remote
```

## Architecture

The AES67 macOS driver consists of several key components:

### Core Components
- **Driver Module**: The AudioServerPlugIn module that integrates with macOS Core Audio
- **Network Engine**: Handles AES67 stream reception/transmission
- **PTP Synchronization**: Implements IEEE 1588-2008 Precision Time Protocol
- **Jitter Buffer**: Absorbs network jitter for smooth audio playback
- **Adaptive Resampling**: Corrects for clock drift between devices
- **Manager Application**: SwiftUI application for configuring streams

### PTP Integration
The driver embeds ptpd (Precision Time Protocol daemon) to achieve accurate synchronization. The embedded ptpd:
- Does not adjust the system clock
- Updates shared state with timing information
- Operates in threaded mode within the coreaudiod process space
- Uses software timestamping (does not require hardware support)

### Audio Processing Pipeline
1. AES67 streams are received via RTP over UDP
2. Packets are ordered and buffered in the jitter buffer
3. PTP synchronization ensures proper timing
4. Adaptive resampling corrects for clock differences
5. Audio is delivered to Core Audio

## Development

### Adding New Features
When adding new features:
1. Follow the existing code structure and naming conventions
2. Write unit tests for new functionality
3. Update documentation as needed
4. Ensure cross-platform compatibility where applicable

### Testing
Unit tests are located in the `Tests/` directory. Run them with:
```bash
make test
```

Or using CTest:
```bash
cd build
ctest
```

## Known Issues

- When running on virtualized environments, PTP synchronization may be less accurate due to virtual network overhead
- High network packet loss may affect audio quality despite jitter buffer and concealment algorithms
- The driver requires exclusive access to the network interface for optimal performance

## Troubleshooting

If you encounter issues during build or installation:

1. Ensure all dependencies are properly installed
2. Check that you have the required permissions for installation
3. Review the build logs for specific error messages
4. Consult the documentation or file an issue on GitHub

## Contributing

We welcome contributions to the AES67 macOS driver project. Please read our contribution guidelines before submitting pull requests.