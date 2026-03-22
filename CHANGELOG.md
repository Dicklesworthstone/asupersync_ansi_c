# Changelog

All notable changes to `asupersync_ansi_c` (`asx`) are documented here.

This project has no tagged releases yet. History is organized by development
phase and date, with representative commit links for each capability area.
Repo: <https://github.com/Dicklesworthstone/asupersync_ansi_c>

---

## Unreleased

**Status as of 2026-03-21:** 223 commits, 431 files, ~127 k lines of C/header
code, 494 public API functions across 28+ subsystems, 2,011 tests across 161
test files. No tags. No GitHub Releases.

---

### 2026-03-21 -- Cancel witness protocol, combinator fixes, web security

#### Added
- Cancel witness lifecycle: create, advance through phases, release
  ([fc0e075](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/fc0e075e5f0e2320dfb357542d3ab04e5d5d7d62))
- Witness protocol wired into the runtime cancellation flow
  ([e19c44b](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/e19c44b0798e4eefda08d3221b4cfb9946c27dcb))
- Umbrella header (`asx.h`) now exposes all subsystem families
  ([69ecc1e](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/69ecc1e43a806aae4d0f3c8cf128d8298a190ccd),
   [d734299](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/d73429953c913b39e672fcb5aab8142889024117))

#### Fixed
- Combinator: disarm deadlines on all exit paths, fix quorum drain, prevent `race_timeout` double-drain
  ([808d1aa](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/808d1aaa977b681fbbfe2bf487fcb15ba88790c5),
   [31e445d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/31e445d64da662f7dc9f668e09a4cb579b0fa383))
- MPSC channel: allow slot reuse after `FULLY_CLOSED`
  ([5454a6d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/5454a6d14c109a5d5d13027b21733e749d49debf))
- TLS: detect dead TCP under TLS; prevent session ID collisions in web layer
  ([b73f2ea](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/b73f2eaf22788b54ab145df859bb759a8d559006))
- Web: constant-time CSRF comparison, proper `Vary` header for CORS
  ([ba2c4cf](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/ba2c4cffb3cc931403c5520d430ed6a73eb56e4b))
- Cancel: replace `asx_cancel_request` walking skeleton with real delegation
  ([8972ad7](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/8972ad7870856be480cf7b3b089f5e9834d9b755))
- App/report: harden argument parsing against integer overflow, validate report inputs
  ([9eb2d9d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/9eb2d9d79691ae87091400cc09572261e62118bc))
- Diagnostic: validate evidence level before recording entries
  ([a54b162](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/a54b1625990fe0ce0075c8fc1a06341afa929876))
- Replay oracle: compare full lab result fields
  ([c9a4737](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/c9a473748d0f0e22143167e929a204647efd3c3c))

---

### 2026-03-20 -- Lab dual-run, crate parity, MPSC generation fix

#### Added
- Lab dual-run scenario support and regression localization enhancements
  ([0f9e21f](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/0f9e21f1942849c22786cb3a77add353fc10b742))
- Crate-level acceptance gate and expanded E2E suite registry
  ([a97f16a](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/a97f16a1ff9e0718dca4750a5b99407916820003))
- Full crate parity achieved -- all beads closed
  ([2ff14d7](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/2ff14d76310330981c4fc79bf146ee0c56105091))

#### Fixed
- MPSC generation counter and remote lease overflow safety
  ([8b5a3fc](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/8b5a3fc49e9be9f973b1494de5e33e82a55c968b))
- E2E: link `server_shutdown` against `libasx.a` instead of individual sources
  ([0218225](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/02182252a5014083fee8f61e973168a4ecfd218e))

---

### 2026-03-19 -- Networking stack: TCP/TLS/WebSocket/QUIC/HTTP/gRPC/web

This date marks the largest single-day expansion of the project, adding the
full networking layer (~10,000+ lines).

#### Added
- Unix-domain sockets, TLS, WebSocket, QUIC, HTTP, and server subsystems
  ([68f854f](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/68f854fcfd85c3ab3ed714f509e2d3d30a8f4d72))
- Web framework: router, middleware, sessions, SSE, multipart, CORS, CSRF, static files
  ([26fc555](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/26fc555aa1d1fda539d4a20cd4b8e1c9b671aa0f))
- gRPC: codec, service, client/server, reflection, health
  ([bbe4d64](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/bbe4d64b85e374c6be6e2e212a427ae3af60dbaf))
- Database client and messaging/broker subsystems
  ([db8d822](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/db8d82271ff515eabf94edf25a9efea9ab029463))
- Distributed runtime: snapshot, assignment, and recovery
  ([356f4bb](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/356f4bb0badeb995aca65ca4ab735c4859cdd4af))
- HTTP router, middleware, sessions, and security infrastructure
  ([0ba5d0d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/0ba5d0d96ffb6c14247db76bb56c5ff954954364))

#### Fixed
- QUIC stream count leak and connection close cleanup
  ([ec7eab3](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/ec7eab3240113b740d925fb3cce7f44bd840f3ed))
- Buffer safety and input validation across all networking modules
  ([d509dfe](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/d509dfec60c0f519d9f5a647170e5ad6692f2c76))
- Remote: handle_id presence check, report rendering
  ([33c7d09](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/33c7d093c6787a09d26c5954c829a207fb57f97b))
- Reactor priority, obligation lifecycle, and type safety in runtime/link/session
  ([a70579c](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/a70579c398b3e590d9536c1fa48b4908d04a6146))

---

### 2026-03-18 -- Remote execution, service middleware, transport, actor supervision

#### Added
- Remote execution, spork orchestration, and transport subsystems
  ([783bb1a](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/783bb1ab778e97e022cd0f57b5bf201d48545c97))
- Retry and circuit-breaker middleware, service extensions
  ([3a38259](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/3a382591a41fff12b5ddf9bb876e928fc3b8e509))
- Encoding/decoding pipelines, RaptorQ stub, migration subsystem, runtime diagnostics
  ([5622941](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/562294174dcfa5d5ca4e29525565c7f3f69d2827))
- Actor supervision example with comprehensive test coverage (+922 lines)
  ([bf6309c](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/bf6309cf69620a88f31b1b13246be5ab85e559a4))
- Major README expansion with architecture and subsystem documentation (+580 lines)
  ([0b9721b](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/0b9721b671c386e22b1a25278745a48495319b6f))

#### Fixed
- Transport: connection side discrimination and error propagation hardening
  ([dbc5024](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/dbc50248e0e6656bf5a4475cb5779ed5d1f4c11f))
- Remote: validate saga step inputs, propagate rejection status codes
  ([942b018](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/942b0186a54d8d50fc01053b46bf2db04e5ff858))

---

### 2026-03-17 -- Deadline monitor, runtime API expansion (+2,000 lines)

#### Added
- Deadline monitor subsystem with core/config/cancel API expansion (+607 lines)
  ([39b39e5](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/39b39e5f5677c2b6ecd05b032ecb68082c1b93be))
- Runtime API scoping and unit tests across subsystems (+931 lines)
  ([5409dac](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/5409dac20a896ae5ded907f4b6ba5f0281885bba))
- Session unit tests and plan vignette
  ([ed649dd](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/ed649ddd584290f5b5af82b4fa8c69c8a9a80de0))
- Cx, evidence_sink, stream, blocking, IO driver subsystem expansion (+203 lines)
  ([a86e2c4](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/a86e2c4ac978d6025b44751528410fdf924ab62d))
- Blocking/IO driver APIs, quickstart example (+157 lines)
  ([b0eabae](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/b0eabaede3482c57336d113d4618c8d77d17322a))

#### Fixed
- Cancel, combinator, and deadline monitor edge cases
  ([9575eb8](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/9575eb82ca61ace528c809272106891979bba839))

---

### 2026-03-16 -- Diagnostics, lifecycle vignettes, config reload

#### Added
- App doctor, console, monitor, and runtime diagnostic APIs (+771 lines)
  ([6708cc2](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/6708cc24cb0bd2fa85b33a690230bd88ffc3ec62))
- Config reload, IO driver, fs, process, and sync subsystem expansion (+197 lines)
  ([66ddd05](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/66ddd057032923e1171040a68344cc3b8f15a228))
- Lifecycle vignette with comprehensive state machine tests (+141 lines)
  ([92e8572](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/92e85725e46cc53e55855a155ec86451b2a752b1))
- Runtime blocking, IO driver, and lifecycle APIs (+179 lines)
  ([4e70210](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/4e70210dd0b82ed4157fd83555ac02c49a447045))
- Plan subsystem unit tests
  ([a9231f3](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/a9231f365a715c70bcef69c25a298b42fe8f7215))

#### Fixed
- Correct fault injection test assertions
  ([1f622e6](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/1f622e63dd013381428bd55c4dd4872c273b2a89))

---

### 2026-03-15 -- Observability, network surface, automotive instrumentation

#### Added
- Evidence, evidence_sink, monitor, and observability subsystems
  ([ef33eda](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/ef33edab29b2fbab3c58561b8e72efe61ee069ae))
- Net API expansion, monitor/tracing_compat tests, observability vignettes
  ([ceb1d11](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/ceb1d11d29bfe6d9b6ddda3e45e29d92fa606d32))
- Network surface E2E tests, IO driver expansion, network vignette
  ([b7716fc](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/b7716fce67ae203e9cebf513543faca22df41417))
- Runtime local storage API, network subsystem expansion
  ([6ca4c01](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/6ca4c011315aa2298ae538c770f6f2f105e4861f))
- Automotive instrument API and network test scenarios
  ([1d0c459](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/1d0c45914a14406d37cde9ecb7f4e542c3ec9a15))
- Runtime builder, config, snapshot, deadline, hooks expansion (+857 lines)
  ([af96d4c](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/af96d4c9ac733b6236095fee93189f4c0a25d55f),
   [8f634df](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/8f634df10f1e2ebb2f9a6b2f0dd74afc270b9448))
- ABI consumer shim expansion and README update
  ([9be1eb9](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/9be1eb95d019b7cb6660d34d7f4a0ae0335feacb))

---

### 2026-03-14 -- Cx context, runtime builder, session/link/obligation subsystems

#### Added
- Cx context API with lifecycle and resource management
  ([bc376bb](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/bc376bbe5d9e4fa2ba652e5a61711e5986b61abb),
   [c7cb026](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/c7cb0264f5fd6399cc09de06d0d57e5482aa9367))
- Runtime builder API for structured configuration
  ([52e9778](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/52e97780155dbfa14b587be2bd748195e8c30c2a))
- Runtime lifecycle API with graceful shutdown and vignette tests
  ([d7767e4](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/d7767e4718e8a6b79e9a338668c9bd5f94bde7a6))
- Link, obligation, record, and session subsystems with lifecycle/vignette tests
  ([9015fa5](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/9015fa5d6ad0bddc1b03bb5ba88fa14ff64757ee))
- Console, testing, and tracing_compat subsystems
  ([12c2e63](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/12c2e631d0d506defe29f272697db852fbcc4ebc))
- Security vignette tests
  ([ebe0062](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/ebe0062407d5762c83e8ff0848cfe0e548a449eb))
- App and status API expansion
  ([066c6e6](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/066c6e6b8683d1d583d9c57b64444dcaec8e8f06))

---

### 2026-03-13 -- Full subsystem scaffolding, embedded/browser support, ASAN tooling

#### Added
- Expand ANSI C runtime with full subsystem scaffolding, fuzz parity, and fixture updates
  ([59f49f7](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/59f49f722a0dcbb1f8122ec02421d3057de6744b))
- fs, plan, process, signal, stream subsystems and security audit module
  ([039cd20](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/039cd20ffe1d3c257eb3f2b4d1c79bdd21f2a2db))
- Embedded platform support, browser boundary layer, and examples
  ([f5a0a70](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/f5a0a70f3f986c5e81ef19824b44342819d5cc20))
- New E2E integration tests and CI test scripts
  ([520874a](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/520874af5dcd9d07c2e496880c6f519f943c9766))

#### Changed
- clang-format/tidy configs, ASAN build script, and CI/embedded tooling
  ([91ad486](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/91ad4862d9f623a3bca0969ba8688116f5a1d997))
- Refine all existing subsystems with API hardening and test updates
  ([ab01fc0](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/ab01fc0f405765ca5298a1b73c1cf7ea7d36e2c9))

---

### 2026-03-12 -- Trace emission, event log/snapshot APIs, Rust-C conformance parity

This date marks the confirmation of full feature parity between the Rust
reference implementation and this ANSI C port.

#### Added
- Runtime trace events from channel, timer, scheduler, and cancellation subsystems
  ([a46c165](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/a46c16582d4a4640350a98080072771ccb8520f2))
- `event_log` and `snapshot` APIs
  ([1221523](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/1221523465f697f98b57c3e6e37d1eb5c3ac3917))
- Unified trace snapshot capture with runtime snapshot API
  ([de215a0](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/de215a0906a646c0834e3681310344f349de804c))
- Feature parity verification report confirming full Rust-C conformance
  ([dc42f6f](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/dc42f6f75112341307dc22cbb2ef1b214fc37b70))
- Trace emission tests for timer wheel set/fire/cancel events
  ([6a1287c](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/6a1287c9127ffe36eef6c7f71e3eff246c8f0640))
- Runtime hooks and quiescence unit tests
  ([dee483f](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/dee483f6023d72d46eb48a0f8e45fb417cb5e83e))
- Exported surface inventory, subsystem evidence matrix, provenance crosswalk
  ([20e0340](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/20e034012a8c3c389a3bd7b5a87ff6afb5c195d1),
   [a2be4bf](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/a2be4bf8cb015818e9f9275989172df13246f9ec))

#### Fixed
- `asx_runtime_reset` now clears all global diagnostic state
  ([515fcfe](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/515fcfe7e86939e1f0cd23306bc0788861bdf967))

---

### 2026-03-02 -- CI gates, endian safety, HFT SLO enforcement

#### Added
- CI gates, canonical fixtures, checkpoint waivers, and status audit
  ([e40db14](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/e40db14f1c283d96228677cdb0b04c79452d34af))
- HFT SLO enforcement, machine-readable traceability export
  ([a645205](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/a645205d4c65e09a0f0e7c645620b97d78d11731))

#### Fixed
- Endian-safe digests, integer overflow/underflow guards, NULL safety
  ([d7db6eb](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/d7db6eb9d699096170ee14ef29aa96fa3ae3ed86))

---

### 2026-03-01 -- Formal verification, ABI contract, advanced runtime research

#### Added
- ABI stability contract and binary-size/cold-start SLO gates
  ([07b1938](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/07b193833509ff496ed1e02201baa06dab1e53be))
- Formal assurance ladder: CBMC harnesses, algebraic proofs, translation validation
  ([bebbbcc](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/bebbbcc192cbc1564ff560357406c28b328d1dfb))
- Memory-model litmus suite and cross-compiler codegen checks
  ([b188f27](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/b188f27a0578f5e61abf6d29d466280c02aca540))
- Certification evidence bundle with completeness checker
  ([96d49e7](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/96d49e7651473895265b035ba5ea90aca2b7b22b))
- Lock-free MPSC queue evaluation with shadow-path equivalence
  ([86eb507](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/86eb5076fe43d82f257dd6a320830a0806c21a4a))
- Virtual-time injection layer for anomaly replay
  ([f722e0e](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/f722e0ef1142acd6748ba6e2f2902f23fcecc7ab))
- Hot-reload config boundaries with field classification
  ([5a25eb5](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/5a25eb5d9330097169644bdf6e1a83a86240c21e))
- Replay-guided regression localizer and deterministic circuit-breaker
  ([94df482](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/94df48221f0fba6ebf2a130aebdc2c87701eb00a))
- Cache-oblivious arena layout evaluation for scheduler hot paths
  ([bb9e121](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/bb9e12188bd968022d685224d3dba8980533504c))
- SOS barrier-certificate bounds for scheduler starvation analysis
  ([4996cbb](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/4996cbb272d67160d5530cb72a52512d9330e0e0))
- Seqlock metadata and EBR reclamation evaluation for parallel profile
  ([974f4ec](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/974f4ec773e43a1af5e816941016b428c74de662))
- Rust-AST zero-drift extractor prototype for transition tables
  ([f6ef19a](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/f6ef19a710e487b6307d7167ab8cd2a489bde224))
- wasm32 determinism oracle for host-coupling detection
  ([94b3c7e](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/94b3c7eca28c5f069628d4d27037a46f71d39ea4))
- Deterministic release artifact builder script
  ([b5e17ce](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/b5e17ce1c5767647404937f5e4986fdc67416d68))
- Ghost check transitions and cancel_phase tracking in parallel scheduler
  ([ff25c32](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/ff25c322d43308c9fc5f3dd00bf0070b3fa39fee))

#### Fixed
- uint32_t overflow in load percentage calculations
  ([ea02058](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/ea0205880a9d0044ed793be9cbd997866659c7b9))
- Runtime safety hardening: cancel attribution, resource underflow, ghost checks, divergence tracking
  ([cd82df4](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/cd82df45a7c47c56ca0dd7a5b0dca0eb4fbf83d9))

---

### 2026-02-28 -- Stabilization: quiescence, generational handles, MPSC hardening

This batch focused on correctness hardening after the initial feature buildout.

#### Added
- Conformance vignettes, CI workflow improvements, API ergonomics validation
  ([db5d647](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/db5d64744390a7b91fbe541933d33d3a2f29d6c4))
- Automatic header dependency tracking (`-MMD -MP`)
  ([15dcbc0](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/15dcbc0f62b165febe4e73bafef515c33bd6f694))
- Generational/validation test coverage
  ([22fea84](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/22fea846525969b34609d74071b42ced0dda29bc))

#### Fixed
- Bounds checks, ODR violations, and quiescence obligation gate
  ([e4dbf19](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/e4dbf19bf88e78d55acfa68bbb3e2f5c5b029019))
- Quiescence drain lifecycle, reusable cleanup stacks, timer edge cases
  ([56b084c](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/56b084ca01949cc25b4be0189870aee5e8f83bc7))
- MPSC channel permits, cross-platform ledger digest, duplicate codec key rejection
  ([da8a097](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/da8a0976b3807ee476121d80900606879615f957))
- Timer generation widening, trace event count cap, equivalence.c linked into libasx
  ([9ea2883](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/9ea288391792ae2c023a121300292876af0c4f59))
- Generational handle safety for cleanup stack and timer wheel
  ([b154d7b](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/b154d7baaba5d5a7ae4c4f7617c3c7fe1375df3f))
- Fault containment, captured-state lifecycle, parallel scheduler hardening
  ([f6e9c42](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/f6e9c4293134fc306e27469d4114ad638fc691c5))
- Input validation: MPSC, JSON codec, replay loader
  ([c017237](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/c0172373da51274bcc3b778a74fdd72c7165b0bd))
- Adaptive decision state_count validation, coroutine error expectation
  ([e066f9e](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/e066f9e829dca7ed28b038fbc70d3b0be834e382))
- uint32_t multiplication overflow in adapter router and HFT gate
  ([c65eb5e](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/c65eb5e01dc77218f68beef872cbe89da6cda18a))

---

### 2026-02-27 -- Core runtime buildout: scheduler, codec, trace, cancellation, profiles

The largest phase of initial implementation, standing up the complete runtime
from walking skeleton to full subsystem coverage.

#### Added -- Runtime core
- Walking skeleton end-to-end lifecycle
  ([52da192](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/52da192db1d80147281b8a287d889f3b6d34349d))
- Cleanup-stack and generation-safe handle validation
  ([090ff50](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/090ff506406943b6549a8419d8bde266f55b9a91))
- Must-use enforcement and task-local error ledger
  ([c435ef7](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/c435ef79e25f6c9f1d00cb764d40ecf90c3d3795))
- Obligation arena and lifecycle API
  ([d3370cd](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/d3370cd71164f989b3d16a97ffb5ff7548d14e8a))
- Hooks subsystem, scheduler extensions, protothread tests, conformance suite
  ([6efe64d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/6efe64dd06c058688764d8d878ca489e5d1ea129))

#### Added -- Safety and monitoring
- Affinity guards, ghost safety monitors, resource contracts, channel primitives
  ([bb24661](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/bb246619baac1ce16ad7ee4043fc11a1a3b022e6))
- Fault injection, safety profiles, scheduler event sequencing, ghost integration, region poisoning
  ([7dd3d32](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/7dd3d32f32101103bbe555973975c9d60212e1c8))
- Region poison and is_poisoned lifecycle functions
  ([5b143f1](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/5b143f1250218dda6cb277bdd4e94de4d0317f3b))
- Fault containment policy enum and budget query/constructor helpers
  ([f56b6a3](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/f56b6a3441e9f29e7f240ac16851ae71c9b7b921))

#### Added -- Codec and trace
- BIN codec transport, cross-codec equivalence engine, and buffer API
  ([84b2421](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/84b242189285633531daf76decd623bfd23003d3))
- Codec abstraction headers wired into umbrella include and hooks
  ([02129f8](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/02129f8905c1f39f625c4787f47274b8ec0eeab9))
- Event/snapshot/trace headers and safety posture test
  ([c93ea1d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/c93ea1d9513ed9bf428ba4b7d425feebfcc99621))
- Deterministic event trace, replay verification, and snapshot export
  ([bd670e4](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/bd670e4fea0c60e6a12bfd7331e56b8a839d7494))

#### Added -- Scheduler and cancellation
- Timer wheel, cancellation protocol, trace persistence, and full test infrastructure
  ([0a95fff](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/0a95fff50f3edfa0abae3f608473069dcfcfe34a))
- Cancellation API and region fault containment in public header
  ([4e00adb](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/4e00adbcf0527fb2b8983fb0f165775c55c3106b))

#### Added -- Profiles and deployment
- Vertical acceleration adapters, parallel profile, domain instrumentation
  ([3c3f879](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/3c3f879b26d857a82039512d83b081b3f6d5d9d2))
- Deployment hardening scenario packs and OpenWrt packaging
  ([d7532d9](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/d7532d94698b6ca2b62075e0f4ad99de6fa3d43d))
- MPSC channel implementation, status codes, safety profiles in build system
  ([f814e18](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/f814e18650dc8da55c1a230f6d6f7e60dabb8c6b))

#### Added -- CI and conformance
- Conformance, codec equivalence, and profile parity CI scripts
  ([d13e916](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/d13e9166edf46e246f25becb04d63abe9f8c2d7e))
- Fixture replay runner, diff artifact generation, enriched conformance reporting
  ([c36cfa5](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/c36cfa5b8db6aecd6f9533d3c2d4cceae50629c7))
- Comprehensive quality gate enforcement with semantic delta budget and model checking
  ([7347f88](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/7347f88c2271056bf94f863c6e8daa59a3fa12fe))
- Endian-assumption checker and extended matrix build scripts
  ([ec2feb0](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/ec2feb091a02ecaf6142bd3b1cb84be398f1199b))
- JSON schemas for fixture capture and family manifests
  ([99d2711](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/99d27113bc211aa166db049688211b031d28d6ab))

#### Added -- Tests
- Comprehensive test suites for affinity, ghost, resource, channel, codec, and scheduler
  ([568b81d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/568b81dff5e4630cf52ad70c2700793f50ad3633))
- Cleanup-stack integration tests and obligation unit tests
  ([f906b9d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/f906b9dca63759aa118decc82f9e0ea3b2d545f7))
- Coverage gaps filled: transition, cancel, outcome, budget tests
  ([52d8152](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/52d8152b37dadeef6cded27fd9083560721287da))

#### Fixed
- Walking skeleton: region recycling, hook defaults, Makefile wiring
  ([8bea15d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/8bea15d9a5ed2a02ae3aa99d9f196c6e4255a447))
- Trace continuity, checkpoint waivers, scheduler robustness
  ([75d1819](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/75d18191af70f5461caaa58197932d744a2f03d4))

---

### 2026-02-26 -- Project bootstrap: spec extraction, build system, scaffold

The initial commit date: project plan, specification extraction from the Rust
reference implementation, build system, core scaffolding, and CI harnesses.

#### Added -- Planning and specification
- Bootstrap porting plan and README
  ([4240d9d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/4240d9d696615d89a8d3f93c8dcc68ba2153178a))
- Cross-vertical fidelity program for HFT, automotive, and routers
  ([eef180e](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/eef180e95099a42c3dad7cd13b4e697767c24a47))
- UB elimination, API stability, and phase gate strengthening
  ([1e7d715](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/1e7d715f25af4eb502015e78ea34273578480cc9))
- Phase 1 spec-review gate completion
  ([f91155b](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/f91155ba058d3c7690d0e9af7991cdd9b01eff2f))

#### Added -- Spec extraction from Rust reference
- Lifecycle transition tables corrected against Rust source semantics
  ([65d0f44](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/65d0f44a18068bceaca5661e5ea50188c815004f))
- Deterministic channel/timer/scheduler semantics
  ([98b8899](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/98b8899301f76e0677419d4fa073eefe5bcbf898))
- Machine-readable invariant schema and generation pipeline
  ([fda7eac](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/fda7eac59ea126931bf4b1fef91e383bed3b323a))
- Quiescence/close/leak-detection invariants
  ([0f1e1de](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/61543680628d2c94fd90ff68816f6b409841de44))

#### Added -- Build system and scaffold
- Makefile + CMake build system
  ([76258dc](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/76258dc578acee90972d63f4dbf58c8a5d96e945))
- Core module sources, tests, and platform stubs
  ([461906d](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/461906db3496a24110d36b12bbe419a079bc89ee))
- Compiler/platform matrix build scripts
  ([1245d4b](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/1245d4b3a80ced1a657cb9ed283097dddcddbf0e))
- Runtime hook contract: allocator, reactor, clock, entropy, log sinks
  ([9cdb44f](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/9cdb44f342b0ba6213f6fe131a0eb39a5899f00a))
- QEMU user-mode execution harness for router-class targets
  ([0883ef6](https://github.com/Dicklesworthstone/asupersync_ansi_c/commit/0883ef6e1540c5123368cbe793c771af91459b5a))

---

## Architecture summary

The `asx` runtime is organized into 28+ subsystems under `src/` and
`include/asx/`, with corresponding test suites under `tests/`:

| Subsystem family | Modules |
|---|---|
| **Core** | types, cancel, combinator, budget, transition, outcome, status |
| **Runtime** | scheduler, hooks, lifecycle, builder, blocking, IO driver, replay, trace, snapshot, diagnostic, lab, regression_localize, cancellation |
| **Channel** | MPSC (with generational handles) |
| **Codec** | JSON codec, BIN codec, cross-codec equivalence |
| **Time** | Timer wheel (generational, deterministic) |
| **Sync** | Mutex, barrier, semaphore, once |
| **Net** | TCP, TLS, WebSocket, QUIC, HTTP, gRPC, web framework, server, distributed, db, messaging |
| **App** | CLI, doctor, report, status |
| **Observability** | Evidence, evidence_sink, monitor, console, tracing_compat |
| **Cx** | Context, scope, resource management |
| **Lifecycle** | Link, obligation, record, session |
| **Service** | Retry, circuit-breaker middleware |
| **Transport** | Connection abstraction |
| **Remote** | Remote execution, saga |
| **Spork** | Orchestration |
| **Encoding/Decoding** | Pipeline codecs |
| **Migration** | Schema migration |
| **Security** | Audit module |
| **Platform** | POSIX, Win32, freestanding, embedded_router, HFT, automotive, parallel, browser stubs |
| **ABI** | Stability contract, consumer shim |
| **Actor** | Supervision, mailbox |
| **Bytes** | Buffer API |
| **Signal/Stream/Plan/Process/FS** | OS interface subsystems |
| **RaptorQ** | Forward error correction stub |

**Deployment profiles:** CORE, POSIX, WIN32, FREESTANDING, EMBEDDED_ROUTER,
HFT, AUTOMOTIVE, PARALLEL, BROWSER.

**Test categories:** unit, invariant, E2E, vignette, conformance, fuzz,
formal (CBMC), bench, embedded.
