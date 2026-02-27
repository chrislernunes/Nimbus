#pragma once
#include <cmath>
#include <string>
#include <stdexcept>

// ── Option type ────────────────────────────────────────────────────────────────
enum class OptionType { CALL, PUT };

inline std::string to_string(OptionType t) {
    return t == OptionType::CALL ? "CALL" : "PUT";
}

// ── Option contract descriptor ────────────────────────────────────────────────
struct OptionContract {
    std::string underlying;   // e.g. "BTCUSDT"
    OptionType  type;
    double      strike;       // K
    double      expiry;       // T in years (e.g. 30/365.0)
    double      spot;         // S at quote time
    double      rate;         // risk-free rate (annualised, e.g. 0.05)
    double      div_yield;    // continuous dividend / funding rate
};

// ── Full Greeks bundle ────────────────────────────────────────────────────────
struct Greeks {
    double price    = 0.0;
    double delta    = 0.0;   // dV/dS
    double gamma    = 0.0;   // d²V/dS²
    double vega     = 0.0;   // dV/dσ   (per 1% move in vol)
    double theta    = 0.0;   // dV/dT   (per calendar day)
    double rho      = 0.0;   // dV/dr   (per 1% move in rate)
    double vanna    = 0.0;   // d²V/dS dσ
    double volga    = 0.0;   // d²V/dσ² (vomma)
    double charm    = 0.0;   // d²V/dS dT (delta decay)
    double speed    = 0.0;   // d³V/dS³
};

// ── Vol surface point ─────────────────────────────────────────────────────────
struct SurfacePoint {
    double strike;
    double expiry;   // years
    double mid_iv;
    double bid_iv;
    double ask_iv;
};

// ── Market maker quote ────────────────────────────────────────────────────────
struct MMQuote {
    double bid_price;
    double ask_price;
    double bid_iv;
    double ask_iv;
    double mid_iv;
    double spread_bps;
    Greeks greeks;
};

// ── Constants ─────────────────────────────────────────────────────────────────
namespace constants {
    constexpr double SQRT_2PI  = 2.506628274631000;
    constexpr double INV_SQRT2 = 0.707106781186548;
    constexpr double DAYS_YEAR = 365.0;
    constexpr double MIN_VOL   = 1e-6;
    constexpr double MAX_VOL   = 20.0;
    constexpr double IV_TOL    = 1e-8;
    constexpr int    IV_MAX_IT = 200;
}
