#pragma once
#include "types.hpp"
#include <vector>

// ── SABR Model ────────────────────────────────────────────────────────────────
//
// The SABR model (Hagan et al. 2002) captures the volatility smile via:
//
//   dF  =  α F^β dW₁
//   dα  =  ν α  dW₂
//   dW₁ · dW₂ = ρ dt
//
// Parameters:
//   α (alpha) — initial vol level (ATM vol proxy)
//   β (beta)  — backbone: 0 = normal, 0.5 = CIR-like, 1 = log-normal
//   ρ (rho)   — spot-vol correlation  (negative = downward skew)
//   ν (nu)    — vol-of-vol (controls smile curvature)
//
// We use the Hagan closed-form approximation for implied vol.
// β is fixed (caller sets it); α, ρ, ν are calibrated to market.
//
struct SABRParams {
    double alpha = 0.3;  // initial vol level
    double beta  = 0.5;  // fixed backbone (user configurable)
    double rho   = -0.3; // spot-vol correlation
    double nu    = 0.5;  // vol of vol
};

struct CalibrationPoint {
    double strike;
    double expiry;     // years
    double market_iv;  // annualised
    double weight;     // fitting weight (e.g. 1/vega²)
};

class SABRModel {
public:
    explicit SABRModel(double beta = 0.5);

    // Hagan et al. implied vol formula
    // F = forward price = S * exp((r-q)*T)
    double implied_vol(double F, double K, double T,
                       const SABRParams& p) const;

    // Calibrate α, ρ, ν to a strip of market IVs at fixed expiry
    // beta is kept fixed throughout calibration
    // Returns calibrated params; sets calibration_error (RMSE bps)
    SABRParams calibrate(double F, double T,
                         const std::vector<CalibrationPoint>& points,
                         double& calibration_error_bps) const;

    // Smile: compute IV across a range of strikes given calibrated params
    std::vector<std::pair<double,double>> smile(
        double F, double T, const SABRParams& p,
        double strike_lo, double strike_hi, int n_points = 50) const;

private:
    double beta_;

    // Levenberg-Marquardt style least-squares minimiser
    // Minimises sum of (model_iv - market_iv)² * weight
    SABRParams lm_calibrate(double F, double T,
                             const std::vector<CalibrationPoint>& pts,
                             const SABRParams& init) const;

    double rmse_bps(double F, double T, const SABRParams& p,
                    const std::vector<CalibrationPoint>& pts) const;
};
