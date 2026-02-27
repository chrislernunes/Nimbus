#pragma once
#include "types.hpp"
#include "svi_surface.hpp"
#include "sabr.hpp"
#include "market_maker.hpp"
#include "binance_options.hpp"
#include <functional>
#include <mutex>
#include <atomic>
#include <chrono>
#include <deque>

// ── Surface update event ──────────────────────────────────────────────────────
struct SurfaceUpdate {
    long long    timestamp_ms;
    double       spot;
    SVISurface   surface;
    SABRParams   sabr_params;        // calibrated to nearest expiry
    double       sabr_fit_bps;
    int          n_contracts;        // number of market points used
    int          n_slices;           // number of expiry slices fitted
    double       atm_iv;             // ATM IV of nearest expiry
    double       skew_25d;           // 25d put IV - 25d call IV
    double       term_spread;        // back-month ATM IV - front-month ATM IV
    std::string  status;             // "OK" | "STALE" | "ERROR"
};

// ── Surface health metrics ────────────────────────────────────────────────────
struct SurfaceHealth {
    double avg_fit_error_bps = 0.0;
    double max_fit_error_bps = 0.0;
    int    n_updates         = 0;
    int    n_failures        = 0;
    long long last_update_ms = 0;
    bool   is_stale          = true;
    double staleness_ms      = 0.0;
};

// ── LiveSurfaceEngine ─────────────────────────────────────────────────────────
//
// Sits between BinanceOptionsClient and OptionsMarketMaker.
// Responsibilities:
//   1. Receive raw OptionsChain from client (live or replay)
//   2. Filter contracts by quality (OI, bid-ask, moneyness range)
//   3. Fit SVI surface slices per expiry
//   4. Calibrate SABR to front-month slice
//   5. Publish SurfaceUpdate to registered callbacks
//   6. Update OptionsMarketMaker surface atomically
//   7. Track surface health + staleness
//
class LiveSurfaceEngine {
public:
    using UpdateCallback = std::function<void(const SurfaceUpdate&)>;

    LiveSurfaceEngine();

    // Register a callback to be invoked on every surface update
    void on_update(UpdateCallback cb);

    // Connect to a market maker (surface pushed on every update)
    void attach_market_maker(OptionsMarketMaker* mm);

    // Process one OptionsChain (called by BinanceOptionsClient callback)
    // Thread-safe: can be called from any thread
    void process(const OptionsChain& chain);

    // Latest surface (thread-safe read)
    SurfaceUpdate latest() const;

    // Health metrics
    SurfaceHealth health() const;

    // Print latest surface state to console
    void print_live_surface() const;

    // ── Filtering config ──────────────────────────────────────────────────────
    struct FilterConfig {
        double min_open_interest    = 5.0;      // contracts
        double min_moneyness        = 0.70;     // K/S lower bound
        double max_moneyness        = 1.40;     // K/S upper bound
        double min_expiry_days      = 1.0;
        double max_expiry_days      = 180.0;
        double max_iv               = 5.0;      // 500% IV cap
        double min_iv               = 0.05;     // 5% IV floor
        int    min_strikes_per_slice = 5;       // minimum strikes to fit a slice
    };
    void set_filter(const FilterConfig& f) { filter_ = f; }

private:
    mutable std::mutex       mutex_;
    SurfaceUpdate            latest_;
    SurfaceHealth            health_;
    std::vector<UpdateCallback> callbacks_;
    OptionsMarketMaker*      mm_ = nullptr;
    FilterConfig             filter_;

    // Calibration state
    SABRModel                sabr_{0.5};

    // Surface history for term structure analysis
    std::deque<SurfaceUpdate> history_;
    static constexpr int     MAX_HISTORY = 100;

    // Internal helpers
    std::vector<SurfacePoint> filter_chain(const OptionsChain& chain) const;

    double compute_atm_iv(const SVISurface& surface,
                          double spot, double T_front) const;

    double compute_skew_25d(const SVISurface& surface,
                             double spot, double T_front,
                             double rate) const;

    double compute_term_spread(const SVISurface& surface,
                                double spot) const;
};
