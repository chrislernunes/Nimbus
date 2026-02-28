#pragma once
#include "types.hpp"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>
#include <thread>

// ── Raw Binance EAPI response structs ─────────────────────────────────────────
// Binance European Options (eapi.binance.com)
// Docs: https://binance-docs.github.io/apidocs/voptions/en/

struct BinanceOptionTicker {
    std::string symbol;         // e.g. "BTC-250530-65000-C"
    std::string underlying;     // "BTCUSDT"
    OptionType  type;
    double      strike;
    double      expiry_years;
    long long   expiry_ts_ms;   // unix ms
    double      mark_price;
    double      bid;
    double      ask;
    double      mark_iv;        // Binance calculated IV (annualised)
    double      delta;
    double      gamma;
    double      vega;
    double      theta;
    // NOTE: open_interest is always 0.0 — /eapi/v1/openInterest returns
    // HTTP 400 {"code":-6010} for every expiry. FilterConfig::min_open_interest
    // must be set to 0.0 to avoid rejecting all contracts.
    double      open_interest;
    double      volume_24h;
    long long   timestamp_ms;
};

// Parsed result of a full chain fetch
struct OptionsChain {
    std::string              underlying;   // "BTC"
    double                   spot;
    long long                timestamp_ms;
    std::vector<BinanceOptionTicker> tickers;

    // Convenience: filter to surface-ready points.
    // Pass min_oi=0.0 since OI is unavailable from Binance EAPI.
    std::vector<SurfacePoint> to_surface_points(
        double min_oi        = 0.0,   // OI unavailable — keep at 0
        double min_bid       = 0.0,
        double max_spread_iv = 0.20
    ) const;
};

// ── Historical snapshot ────────────────────────────────────────────────────────
struct HistoricalSnapshot {
    long long      timestamp_ms;
    double         spot;
    OptionsChain   chain;
    std::vector<SurfacePoint> surface_pts;
};

// ── BinanceOptionsClient ───────────────────────────────────────────────────────
//
// Fetches live and historical options data from Binance EAPI.
//
// Live endpoints used per poll cycle:
//   GET /eapi/v1/index   — underlying spot index price       (weight: 1)
//   GET /eapi/v1/mark    — mark prices + Greeks, all expiries (weight: 5)
//   GET /eapi/v1/ticker  — live bid/ask, all contracts       (weight: 5)
//
// NOT used (endpoint broken — returns -6010 for all expiries):
//   GET /eapi/v1/openInterest
//
// Rate limits:
//   3 requests × 2 polls/min = 6 req/min — well within 400 req/min limit.
//
class BinanceOptionsClient {
public:
    explicit BinanceOptionsClient(const std::string& underlying = "BTC");

    // ── One-shot fetches ──────────────────────────────────────────────────────

    // Current spot index price
    double fetch_spot() const;

    // Full options chain (mark prices + Greeks, all expiries)
    OptionsChain fetch_chain() const;

    // Mark price + Greeks for a single expiry
    // expiry_date: "YYMMDD" e.g. "250530"
    std::vector<BinanceOptionTicker> fetch_expiry(const std::string& expiry_date) const;

    // Available expiry dates on Binance
    std::vector<std::string> fetch_expiry_dates() const;

    // ── Live streaming ────────────────────────────────────────────────────────

    using ChainCallback = std::function<void(const OptionsChain&)>;

    // Start polling loop: fetches full chain every `interval_ms`, calls cb.
    // Runs in background thread until stop() is called.
    void start_live(ChainCallback cb, int interval_ms = 30000);
    void stop();
    bool is_running() const { return running_; }

    // ── Historical replay ─────────────────────────────────────────────────────

    // Record a snapshot to a CSV file (append mode)
    void record_snapshot(const OptionsChain& chain, const std::string& filepath) const;

    // Load snapshots from a CSV file
    std::vector<HistoricalSnapshot> load_snapshots(const std::string& filepath) const;

    // Replay snapshots, calling cb for each one at the original cadence.
    // speed_factor: 1.0 = real-time, 10.0 = 10x faster, 0.0 = instant
    void replay(const std::vector<HistoricalSnapshot>& snapshots,
                ChainCallback cb,
                double speed_factor = 10.0) const;

private:
    std::string       underlying_;
    std::atomic<bool> running_{false};
    std::thread       poll_thread_;

    // HTTP GET helper (libcurl)
    std::string http_get(const std::string& url) const;

    // Parse a single option symbol: "BTC-250530-65000-C"
    bool parse_symbol(const std::string& symbol,
                      std::string& underlying,
                      std::string& expiry_str,
                      double& strike,
                      OptionType& type) const;

    // Expiry string "YYMMDD" → years from now
    double expiry_to_years(const std::string& expiry_str,
                           long long now_ms) const;

    // Base URL
    static constexpr const char* BASE_URL = "https://eapi.binance.com";
};
