# Nimbus

A production-grade options pricing and volatility surface engine in C++20. Implements the complete derivatives pricing stack from first principles: Black-Scholes with full second-order Greeks, SABR calibration via Levenberg-Marquardt, SVI surface construction with no-arbitrage constraints, a delta-hedging options market maker — and a **live data layer** that streams the real Binance options chain, recalibrates the surface on every update, and records/replays historical sessions.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                    Binance EAPI (live)                           │
│   eapi.binance.com/eapi/v1/mark   — mark prices + Greeks        │
│   eapi.binance.com/eapi/v1/ticker — live bid/ask                │
│   eapi.binance.com/eapi/v1/index  — spot index                  │
└────────────────────────┬─────────────────────────────────────────┘
                         │  HTTP/REST  (libcurl, 30s poll)
                         ▼
              ┌───────────────────────┐
              │  BinanceOptionsClient │  ← also handles CSV record/replay
              │  fetch_chain()        │
              │  start_live(cb)       │
              │  record_snapshot()    │
              │  load_snapshots()     │
              │  replay()             │
              └──────────┬────────────┘
                         │  OptionsChain
                         ▼
              ┌───────────────────────┐
              │  LiveSurfaceEngine    │
              │                       │
              │  1. Quality filter    │  min OI, moneyness range,
              │     (per tick)        │  expiry window, IV bounds
              │  2. SVI fit           │  per-expiry slice
              │     (per tick)        │  no-arb enforced
              │  3. SABR calibrate    │  front-month α, ρ, ν
              │     (per tick)        │  Levenberg-Marquardt
              │  4. Surface analytics │  ATM IV, 25d skew,
              │                       │  term spread
              │  5. Health tracking   │  staleness, fit errors
              └──────────┬────────────┘
                         │  SurfaceUpdate (thread-safe)
               ┌─────────┴──────────┐
               ▼                    ▼
   ┌─────────────────┐   ┌────────────────────┐
   │  User callbacks │   │ OptionsMarketMaker │
   │  (print, log,   │   │                    │
   │   analyse)      │   │  • Fair value from │
   └─────────────────┘   │    SVI surface     │
                         │  • Spread: base +  │
                         │    inventory skew  │
                         │    + gamma addon   │
                         │  • Delta hedge     │
                         │  • PnL attribution │
                         └────────────────────┘

          ┌──────────────────────────────────────┐
          │           Pricing Core               │
          │  BlackScholes  SABR  SVISurface      │
          │  (no external deps — pure C++20)     │
          └──────────────────────────────────────┘
```

---

## Modes

```bash
nimbus.exe                    # Static demos — no network, runs offline
nimbus.exe live               # Live Binance surface, 5 min (Ctrl+C to stop)
nimbus.exe record 120         # Record 2 min of live data, then replay at 10x
nimbus.exe replay surface.csv # Replay previously saved CSV
```

---

## Components

### `BinanceOptionsClient` — `binance_options.cpp/hpp`

Fetches real options data from Binance European Options API (`eapi.binance.com`).

**Endpoints used:**

| Endpoint | Purpose | Rate weight |
|---|---|---|
| `GET /eapi/v1/mark?underlying=BTCUSDT` | Mark prices + Greeks for all listed contracts | 5 |
| `GET /eapi/v1/ticker?underlying=BTCUSDT` | Live bid/ask for all contracts | 5 |
| `GET /eapi/v1/index?underlying=BTCUSDT` | Spot index price | 1 |
| `GET /eapi/v1/exchangeInfo` | Available expiry dates | 1 |

At a 30-second poll interval: ~12 req/min, well under the 400 req/min limit.

**Symbol parsing**: `BTC-250530-65000-C` → underlying=BTC, expiry=2025-05-30, strike=65000, type=CALL

**Expiry calculation**: YYMMDD → fractional years from now, anchored to 08:00 UTC (Binance expiry time)

**Historical recording**: Every chain fetch can be appended to a CSV with full contract data. The CSV schema is:
```
timestamp_ms, spot, symbol, type, strike, expiry_years,
mark_iv, bid, ask, delta, gamma, vega, theta, open_interest, volume_24h
```

**Replay**: Load CSV → reconstruct `OptionsChain` sequence → feed through `LiveSurfaceEngine` at configurable speed (1.0x = real-time, 0.0 = instant).

---

### `LiveSurfaceEngine` — `live_surface.cpp/hpp`

Sits between the data client and the pricing/MM layer. Runs the full recalibration pipeline on every chain update.

**Pipeline per tick:**

1. **Quality filter** — configurable per symbol:
   - Expiry window: 1d – 180d
   - Moneyness: 0.70× – 1.40× spot (removes very deep OTM junk)
   - IV bounds: 5% – 500%
   - Min open interest: 5 contracts
   - Min 5 strikes per expiry slice (otherwise SVI is under-determined)

2. **SVI fit** — one slice per expiry, coordinate-descent minimiser with butterfly no-arb constraint enforced at every step

3. **SABR calibration** — Levenberg-Marquardt fitting α, ρ, ν to the front-month slice; β fixed at 0.5

4. **Surface analytics computed per update:**
   - ATM IV (front-month)
   - 25-delta skew: `IV(K_put25) − IV(K_call25)` — measures the demand premium for downside protection
   - Term spread: `ATM_IV(back-month) − ATM_IV(front-month)` — measures the vol term structure slope

5. **Health monitoring** — tracks staleness, update/failure counts, avg/max SVI fit errors

6. **Thread-safe surface publish** — mutex-protected; market maker surface updated atomically

**Staleness**: Surface marked STALE after 60 seconds without a successful update.

---

### `BlackScholes` — `black_scholes.cpp/hpp`

Full closed-form BSM with continuous dividend yield `q` (directly models crypto funding rates).

**9 Greeks computed analytically:**

| Greek | Formula | Quant use |
|---|---|---|
| Delta | ∂V/∂S | Hedge ratio |
| Gamma | ∂²V/∂S² | Hedge frequency / P&L convexity |
| Vega | ∂V/∂σ (per 1%) | Vol exposure |
| Theta | ∂V/∂t (per day) | Time decay cost |
| Rho | ∂V/∂r (per 1%) | Rate sensitivity |
| Vanna | ∂²V/∂S∂σ | Delta changes with vol (spot-vol risk) |
| Volga | ∂²V/∂σ² | Vega convexity (smile P&L) |
| Charm | ∂²V/∂S∂t | Delta decay per day |
| Speed | ∂³V/∂S³ | Gamma sensitivity to spot |

**IV solver**: Newton-Raphson with Halley's method correction. Brenner-Subrahmanyam initial guess. Convergence in 3–6 iterations. Round-trip error < 0.01 bps.

---

### `SABRModel` — `sabr.cpp/hpp`

SABR stochastic volatility model (Hagan, Kumar, Lesniewski, Woodward 2002).

**Hagan closed-form** maps `(F, K, T, α, β, ρ, ν)` → implied vol directly — no simulation.

**Calibration** (`lm_calibrate`): Levenberg-Marquardt minimising weighted RMSE between model IV and market IV. Calibrates α, ρ, ν simultaneously with numerical Jacobian. β held fixed (standard practice). Constrained: α > 0, ν > 0, |ρ| < 1.

**Why SABR?** SABR is the standard model for rates and equity vol desks. It parametrises skew (ρ) and curvature (ν) separately, which maps cleanly to risk sensitivities used in hedging.

---

### `SVISurface` — `svi_surface.cpp/hpp`

SVI (Stochastic Volatility Inspired) parametrisation of Gatheral (2004). Parametrises **total implied variance** `w(k) = σ²T`:

```
w(k) = a + b [ ρ(k−m) + √((k−m)² + σ²) ]
```

**No-arbitrage enforced:**
- Butterfly: `b(1 + |ρ|) ≤ 4` — prevents negative local vol density
- Calendar: interpolation in `w/T` space — prevents calendar spread arbitrage

**Multi-expiry**: One SVI slice per expiry, interpolated in total-variance space between slices.

---

### `OptionsMarketMaker` — `market_maker.cpp/hpp`

Delta-hedging market maker using the live SVI surface as fair value.

**Spread construction:**
```
bid_iv = surface_iv − half_spread − gamma_addon − uncertainty + inventory_skew
ask_iv = surface_iv + half_spread + gamma_addon + uncertainty + inventory_skew
```

| Component | Effect | Rationale |
|---|---|---|
| `base_spread_vol` | Fixed half-spread | Compensation for adverse selection |
| `gamma_addon` | Wider near-money | High-gamma options are expensive to delta hedge |
| `vol_uncertainty` | Flat add-on | Model error premium |
| `inventory_skew` | Shifts mid | Reduces net vega without widening spread |

**On every fill**: updates net delta/gamma/vega/theta/vanna/volga, deducts fee, records to trade log.

**Delta hedge**: flattens net delta via spot market after fills or on demand. Uses bid for short hedge, ask for long hedge.

**Mark-to-market**: unrealized PnL computed against current SVI surface on every spot update.

---

## Build

### Dependencies

```bash
# MSYS2 / Windows (run install dependencies task in VS Code)
pacman -S mingw-w64-ucrt-x86_64-curl
pacman -S mingw-w64-ucrt-x86_64-nlohmann-json

# Ubuntu / Debian
sudo apt install libcurl4-openssl-dev nlohmann-json3-dev
```

### Compile

```bash
# Direct — Windows MSYS2
g++ -std=c++20 -O2 \
    main.cpp black_scholes.cpp sabr.cpp svi_surface.cpp \
    market_maker.cpp binance_options.cpp live_surface.cpp \
    -IC:/msys64/ucrt64/include -LC:/msys64/ucrt64/lib \
    -lcurl -lws2_32 \
    -o nimbus.exe

# Direct — Linux / macOS
g++ -std=c++20 -O2 \
    main.cpp black_scholes.cpp sabr.cpp svi_surface.cpp \
    market_maker.cpp binance_options.cpp live_surface.cpp \
    -lcurl -o nimbus

# CMake (cross-platform)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

---

## Output Examples

### Live mode (`nimbus.exe live`)

```
[Chain] Fetched 312 contracts  spot=67241.30  ts=1772206464047

── Live Vol Surface ─────────────────────────────
  spot       $67241.30
  status     OK
  contracts  184          ← after quality filtering
  slices     6            ← expiry buckets fitted
  ATM IV     68.4%
  25d skew   +14.2%       ← put IV > call IV (downside demand)
  term sprd  -6.1%        ← front > back (vol term structure inverted)
  SABR fit   22.8 bps
    α=0.4821  ρ=-0.341  ν=0.612

  updates    3
  avg fit    18.4 bps
  max fit    31.2 bps
  staleness  0s

  Expiry   Days  ATM IV    Fit(bps)  Arb-free
  0.0192   7     78.1%     31.2      ✓
  0.0822   30    68.4%     18.4      ✓
  0.2466   90    62.3%     14.7      ✓

  ATM CALL K=67000  bid=3041  ask=3187  bid_iv=67.2%  ask_iv=69.8%  delta=0.491
```

### Record + Replay

```
nimbus.exe record 120

  Phase 1: Recording 120s of live data → btc_options_surface.csv
  [REC #1] contracts=312  spot=$67241.30
  [REC #2] contracts=314  spot=$67318.80
  [REC #3] contracts=311  spot=$67189.60
  [REC #4] contracts=312  spot=$67204.10
  Recorded 4 snapshots to btc_options_surface.csv

  Phase 2: Historical Replay (10x speed)
  [REPLAY #1] spot=$67241.30  ATM=68.4%  skew=14.2%  status=OK
  [REPLAY #2] spot=$67318.80  ATM=68.7%  skew=14.0%  status=OK
  ...
```

---

## Configuration

**Filter** (`live_surface.hpp` → `FilterConfig`):

| Parameter | Default | Description |
|---|---|---|
| `min_open_interest` | 5 | Skip illiquid contracts |
| `min_moneyness` | 0.70 | Lower strike bound (K/S) |
| `max_moneyness` | 1.40 | Upper strike bound |
| `min_expiry_days` | 1 | Skip expiring today |
| `max_expiry_days` | 180 | Skip very long-dated |
| `min_iv` | 5% | IV floor |
| `max_iv` | 500% | IV cap |
| `min_strikes_per_slice` | 5 | Minimum strikes for SVI fit |

**Market Maker** (`market_maker.hpp` → `MMConfig`):

| Parameter | Default | Description |
|---|---|---|
| `base_spread_vol` | 0.005 | Base half-spread in vol |
| `inventory_skew` | 0.002 | Skew per unit vega |
| `max_inventory_vega` | $50,000 | Vega limit |
| `taker_fee_bps` | 0.5 | Binance taker fee |
| `maker_fee_bps` | 0.2 | Binance maker fee |

**Poll interval** (`main.cpp`): 30 seconds (configurable). Binance mark prices update every few seconds but REST weight limits suggest 30s as the practical minimum for full chain fetches.

---

## Mathematical References

- Black, F. & Scholes, M. (1973). *The Pricing of Options and Corporate Liabilities*. Journal of Political Economy.
- Hagan, P., Kumar, D., Lesniewski, A., Woodward, D. (2002). *Managing Smile Risk*. Wilmott Magazine.
- Gatheral, J. (2004). *A Parsimonious Arbitrage-Free Implied Volatility Parametrization*. Global Derivatives.
- Gatheral, J. & Jacquier, A. (2014). *Arbitrage-Free SVI Volatility Surfaces*. Quantitative Finance.
