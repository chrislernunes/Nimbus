#pragma once
#include "types.hpp"
#include <vector>
#include <map>

// ── SVI (Stochastic Volatility Inspired) Parametrisation ─────────────────────
//
// Gatheral (2004): raw SVI parametrises total implied variance w = σ²T as:
//
//   w(k) = a + b[ ρ(k-m) + √((k-m)² + σ²) ]
//
// where k = log(K/F) is log-moneyness.
//
// Parameters:
//   a — vertical translation (overall variance level)
//   b — slope/wing factor  (> 0)
//   rho — skew parameter   (-1 < ρ < 1)
//   m — horizontal shift
//   sigma — smile width    (> 0)
//
// No-arbitrage (butterfly): b(1+|ρ|) ≤ 4
// No-arbitrage (calendar):  w(k,T₁) ≤ w(k,T₂)  for T₁ < T₂
//
struct SVIParams {
    double a     = 0.04;
    double b     = 0.10;
    double rho   = -0.2;
    double m     = 0.0;
    double sigma = 0.15;

    bool is_arbitrage_free() const {
        return b > 0 && sigma > 0 && std::abs(rho) < 1.0
            && b * (1.0 + std::abs(rho)) <= 4.0;
    }
};

// One SVI slice per expiry
struct SVISlice {
    double    expiry;     // years
    SVIParams params;
    double    fit_error;  // RMSE bps
};

class SVISurface {
public:
    // Fit SVI to a single expiry slice
    SVISlice fit_slice(double F, double T,
                       const std::vector<SurfacePoint>& pts) const;

    // Fit SVI across multiple expiries and store slices
    void build(double spot, double rate, double div_yield,
               const std::vector<SurfacePoint>& market_pts);

    // Query: IV at arbitrary (K, T) via linear interpolation between slices
    double iv(double K, double T) const;

    // Total implied variance at log-moneyness k, expiry T
    double total_variance(double k, const SVIParams& p) const;

    // Print surface summary
    void print_summary() const;

    const std::vector<SVISlice>& slices() const { return slices_; }

private:
    std::vector<SVISlice> slices_;
    double spot_   = 0.0;
    double rate_   = 0.0;
    double div_    = 0.0;

    SVIParams fit_single(double F, double T,
                          const std::vector<SurfacePoint>& pts) const;
    double svi_rmse(double F, double T, const SVIParams& p,
                    const std::vector<SurfacePoint>& pts) const;
};
