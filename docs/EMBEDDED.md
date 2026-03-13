# Embedded Target Support

## Supported Architectures

| Target     | Compiler                     | Profile        | QEMU Machine        |
|-----------|------------------------------|----------------|---------------------|
| Cortex-M4 | `arm-none-eabi-gcc`         | FREESTANDING   | lm3s6965evb         |
| Cortex-M0 | `arm-none-eabi-gcc`         | FREESTANDING   | lm3s6965evb (M3 CPU)|
| RV32IMAC  | `riscv64-unknown-elf-gcc`   | FREESTANDING   | virt                |
| RV64IMAC  | `riscv64-unknown-elf-gcc`   | FREESTANDING   | virt                |

Each architecture also builds with EMBEDDED_ROUTER profile.

## R1 Memory Budgets

Budgets are enforced by `tools/ci/measure_baremetal_footprint.sh`.

### ROM Budget (text + rodata)

| Target     | Budget  | Measured | Utilization |
|-----------|---------|----------|-------------|
| Cortex-M4 | 128 KB  | ~63 KB   | 49%         |
| Cortex-M0 | 128 KB  | ~72 KB   | 56%         |
| RV32IMAC  | 128 KB  | ~78 KB   | 61%         |
| RV64IMAC  | 128 KB  | ~77 KB   | 60%         |

### RAM Budget (data + bss)

| Target     | Budget  | Measured | Utilization |
|-----------|---------|----------|-------------|
| Cortex-M4 | 512 KB  | ~301 KB  | 58%         |
| Cortex-M0 | 512 KB  | ~301 KB  | 58%         |
| RV32IMAC  | 512 KB  | ~301 KB  | 58%         |
| RV64IMAC  | 512 KB  | ~318 KB  | 62%         |

### BSS Breakdown (dominant contributors)

| Symbol           | Size    | Source                  |
|-----------------|---------|------------------------|
| g_entries        | ~20 KB  | Overload catalog        |
| g_trace_ring     | 24 KB   | Trace subsystem         |
| g_replay_ref     | 24 KB   | Replay reference buffer |
| g_event_log      | 16 KB   | Event log ring          |
| Region/task slots| ~144 KB | Runtime state tables    |

All BSS sizes are configurable via compile-time defines
(e.g., `ASX_MAX_REGIONS`, `ASX_TRACE_RING_SIZE`).
For flash-constrained targets, reduce these to fit.

## External Symbol Dependencies

### Compiler Intrinsics (provided by libgcc)

- ARM: `__aeabi_ldivmod`, `__aeabi_uldivmod`, `__aeabi_lmul`, `__aeabi_uidiv`
- RV32: `__divdi3`, `__udivdi3`
- RV64: none (hardware divide)

### C Library (provided by newlib/picolibc)

Minimal libc functions used: `memcpy`, `memset`, `memcmp`, `strcmp`, `strlen`,
`strncmp`, `strstr`, `snprintf`.

Heap functions: `malloc`, `free`, `realloc` — used by `snprintf` and some
diagnostic paths. On bare-metal, newlib/picolibc provides these with a
user-supplied `_sbrk` stub.

### POSIX Functions

None. Zero POSIX dependencies in FREESTANDING and EMBEDDED_ROUTER profiles.

## Build Commands

```bash
# Single target
make cross-baremetal-arm-m4-free

# All 8 targets
make cross-baremetal-all

# Footprint report
tools/ci/measure_baremetal_footprint.sh

# QEMU smoke test
tools/ci/run_qemu_baremetal.sh --build-first

# Linkage validation
tools/ci/validate_baremetal_linkage.sh
```

## Linker Scripts

Located in `tools/embedded/`:

| File           | Target    | Flash Origin | SRAM Origin  |
|---------------|-----------|-------------|-------------|
| cortex-m4.ld  | Cortex-M4 | 0x08000000  | 0x20000000  |
| cortex-m0.ld  | Cortex-M0 | 0x08000000  | 0x20000000  |
| qemu-arm.ld   | QEMU ARM  | 0x00000000  | 0x20000000  |
| riscv32.ld    | RV32      | SRAM-only   | 0x80000000  |
| riscv64.ld    | RV64      | SRAM-only   | 0x80000000  |

Stack size: 4 KB default, configurable via `__stack_size` linker symbol.
