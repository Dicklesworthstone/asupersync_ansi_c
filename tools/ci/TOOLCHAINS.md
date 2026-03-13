# Cross-Compilation Toolchains

## Bare-Metal ARM (arm-none-eabi-gcc)

**Package**: `gcc-arm-none-eabi` + `libnewlib-arm-none-eabi`
**Install**: `sudo apt-get install -y gcc-arm-none-eabi libnewlib-arm-none-eabi`
**Profile**: `FREESTANDING`

Targets Cortex-M microcontrollers with no OS. Uses newlib/newlib-nano for
minimal C library support.

### Verified targets

| CPU          | Flags                         | Status |
|-------------|-------------------------------|--------|
| Cortex-M0   | `-mcpu=cortex-m0 -mthumb`    | OK     |
| Cortex-M3   | `-mcpu=cortex-m3 -mthumb`    | OK     |
| Cortex-M4   | `-mcpu=cortex-m4 -mthumb`    | OK     |
| Cortex-M7   | `-mcpu=cortex-m7 -mthumb`    | OK     |

### Newlib-nano

Available via `--specs=nano.specs`. Provides smaller printf/scanf
implementations suitable for flash-constrained targets.

### Link specs

Use `--specs=nosys.specs` for bare-metal (no OS syscalls).

---

## Linux-musl ARM (OpenWrt SDK)

**Package**: OpenWrt SDK downloads
**Profile**: `EMBEDDED_ROUTER`

Targets Linux-based embedded systems (routers, gateways) running musl libc.

| Toolchain                                | Architecture | Status |
|-----------------------------------------|-------------|--------|
| `armv7-openwrt-linux-muslgnueabi-gcc`   | ARMv7       | OK     |
| `aarch64-openwrt-linux-musl-gcc`        | AArch64     | OK     |

---

## Bare-Metal RISC-V (riscv64-unknown-elf-gcc)

**Package**: `gcc-riscv64-unknown-elf` + `picolibc-riscv64-unknown-elf`
**Install**: `sudo apt-get install -y gcc-riscv64-unknown-elf picolibc-riscv64-unknown-elf`
**Profile**: `FREESTANDING`

Targets RISC-V microcontrollers with no OS. Uses picolibc for minimal
C library support.

### Verified targets

| ISA        | Flags                                  | Status |
|-----------|----------------------------------------|--------|
| RV32IMAC  | `-march=rv32imac -mabi=ilp32`          | OK     |
| RV64IMAC  | `-march=rv64imac -mabi=lp64`           | OK     |

### Link specs

Use `--specs=picolibc.specs` for bare-metal with picolibc.

---

## Native Host

**Package**: System `gcc`
**Profile**: `CORE` (default)

Standard host compilation for development and testing.
