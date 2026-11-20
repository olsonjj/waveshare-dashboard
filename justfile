# Solar dashboard tasks. The host service is Rust (server/), the board
# firmware is PlatformIO (firmware/). See README.md for the architecture.

# Run the push server (Ctrl-C to stop). Requires server/config.toml.
run:
    cd server && cargo run

# Compile-check + build the server (debug).
build:
    cd server && cargo build

# Release-build the server.
build-release:
    cd server && cargo build --release

# Rust unit + integration tests (bucket accumulator, contract, Envoy
# parser, SQLite store, socket pusher).
test:
    cd server && cargo test

# Build the board firmware.
fw-build:
    cd firmware && pio run

# Build + flash the board firmware over the USB-C port.
fw-flash:
    cd firmware && pio run -t upload

# Serial console (921600 baud).
fw-monitor:
    cd firmware && pio device monitor
