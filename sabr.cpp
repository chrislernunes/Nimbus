#define _USE_MATH_DEFINES
#include "sabr.hpp"
#include <cmath>
#include <limits>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <iomanip>

SABRModel::SABRModel(double beta) : beta_(beta) {}

// -- Hagan et al. (2002) SABR implied vol -------------------------------------
// For beta=0.5: sigma_ATM ~ alpha / F^(1-beta) = alpha / sqrt(F)
// So alpha ~ sigma_ATM * sqrt(F)  (for BTC with F~67k, alpha ~ 0.65*259 ~ 168)
double SABRModel::implied_vol(double F, double K, double T,
                               const SABRParams& p) const {
    const double alpha = p.alpha;
    const double beta  = p.beta;
    const double rho   = p.rho;
    const double nu    = p.nu;

    if (T <= 0.0 || F <= 0.0 || K <= 0.0) return 0.3;
    if (alpha <= 0.0) return 1e-6;

    // ATM special case (avoids 0/0)
    if (std::abs(F - K) < 1e-8 * F) {
        double FK_mid = std::pow(F, 1.0 - beta);
        double A = alpha / FK_mid;
        double T1 = ((1.0-beta)*(1.0-beta)/24.0) * (alpha*alpha) / (FK_mid*FK_mid);
        double T2 = (rho*beta*nu*alpha) / (4.0 * FK_mid);
        double T3 = ((2.0 - 3.0*rho*rho) / 24.0) * nu * nu;
        return A * (1.0 + (T1 + T2 + T3) * T);
    }

    double log_FK  = std::log(F / K);
    double FK_beta = std::pow(F * K, (1.0 - beta) / 2.0);
    double log_FK2 = log_FK * log_FK;

    // z = (nu/alpha) * FK^((1-beta)/2) * log(F/K)
    double z = (nu / alpha) * FK_beta * log_FK;

    // x(z) = log[(sqrt(1-2rho*z+z^2) + z - rho) / (1-rho)]
    double disc = 1.0 - 2.0*rho*z + z*z;
    if (disc < 1e-14) disc = 1e-14;
    double x_z = std::log((std::sqrt(disc) + z - rho) / (1.0 - rho));

    double zx = (std::abs(x_z) < 1e-10) ? 1.0 : z / x_z;

    // Denominator: FK_beta * [1 + (1-beta)^2/24 * log^2(F/K) + (1-beta)^4/1920 * log^4(F/K)]
    double b1  = (1.0 - beta);
    double den = FK_beta * (1.0 + b1*b1/24.0 * log_FK2
                                + b1*b1*b1*b1/1920.0 * log_FK2*log_FK2);

    // T-correction
    double Tc1 = b1*b1/24.0 * alpha*alpha / (FK_beta*FK_beta);
    double Tc2 = rho*beta*nu*alpha / (4.0 * FK_beta);
    double Tc3 = (2.0 - 3.0*rho*rho) / 24.0 * nu*nu;
    double T_corr = 1.0 + (Tc1 + Tc2 + Tc3) * T;

    return (alpha / den) * zx * T_corr;
}

// -- RMSE in bps ---------------------------------------------------------------
double SABRModel::rmse_bps(double F, double T, const SABRParams& p,
                            const std::vector<CalibrationPoint>& pts) const {
    double sum = 0.0;
    for (const auto& pt : pts) {
        double model = implied_vol(F, pt.strike, T, p);
        double diff  = (model - pt.market_iv) * 10000.0;
        sum += pt.weight * diff * diff;
    }
    return pts.empty() ? 0.0 : std::sqrt(sum / pts.size());
}

// -- Multi-start coordinate descent calibration --------------------------------
// Uses parameter-scaled steps; tries multiple starting points to avoid
// local minima. No matrix inversion -- robust to ill-conditioned surfaces.
SABRParams SABRModel::lm_calibrate(double F, double T,
                                    const std::vector<CalibrationPoint>& pts,
                                    const SABRParams& init) const {
    // For beta=0.5: alpha_scale ~ sigma_ATM * F^(1-beta)
    double atm_iv = init.alpha / std::pow(F, 1.0 - beta_);
    if (atm_iv <= 0.0 || atm_iv > 5.0) atm_iv = 0.5;

    double alpha_scale = atm_iv * std::pow(F, 1.0 - beta_);
    double alpha_max   = alpha_scale * 20.0;  // generous upper bound

    SABRParams best_p  = init;
    double     best_err = rmse_bps(F, T, init, pts);

    auto clamp_params = [&](SABRParams q) {
        q.alpha = std::clamp(q.alpha, 1e-6, alpha_max);
        q.rho   = std::clamp(q.rho,   -0.999, 0.999);
        q.nu    = std::clamp(q.nu,    1e-6, 5.0);
        q.beta  = beta_;
        return q;
    };

    // Try multiple starting points
    std::vector<SABRParams> starts;
    for (double rho_s : {-0.5, -0.3, 0.0}) {
        for (double nu_s : {0.3, 0.6, 1.0}) {
            SABRParams s;
            s.alpha = alpha_scale;
            s.beta  = beta_;
            s.rho   = rho_s;
            s.nu    = nu_s;
            starts.push_back(s);
        }
    }

    for (const auto& start : starts) {
        SABRParams p = clamp_params(start);
        double     err = rmse_bps(F, T, p, pts);

        // Parameter natural scales
        double scales[3] = { alpha_scale * 0.1, 0.05, 0.1 };

        for (double sf : {1.0, 0.1, 0.01, 0.001, 0.0001}) {
            bool any = true;
            while (any) {
                any = false;
                // alpha
                for (int sign : {1, -1}) {
                    SABRParams q = p;
                    q.alpha += sign * scales[0] * sf;
                    q = clamp_params(q);
                    double e = rmse_bps(F, T, q, pts);
                    if (e < err) { err = e; p = q; any = true; break; }
                }
                // rho
                for (int sign : {1, -1}) {
                    SABRParams q = p;
                    q.rho += sign * scales[1] * sf;
                    q = clamp_params(q);
                    double e = rmse_bps(F, T, q, pts);
                    if (e < err) { err = e; p = q; any = true; break; }
                }
                // nu
                for (int sign : {1, -1}) {
                    SABRParams q = p;
                    q.nu += sign * scales[2] * sf;
                    q = clamp_params(q);
                    double e = rmse_bps(F, T, q, pts);
                    if (e < err) { err = e; p = q; any = true; break; }
                }
            }
        }

        if (err < best_err) {
            best_err = err;
            best_p   = p;
        }
    }

    return best_p;
}

// -- Public calibrate ----------------------------------------------------------
SABRParams SABRModel::calibrate(double F, double T,
                                 const std::vector<CalibrationPoint>& points,
                                 double& calibration_error_bps) const {
    if (points.empty()) throw std::invalid_argument("No calibration points");

    // Find ATM IV
    double atm_iv = 0.5;
    double best_dist = 1e18;
    for (const auto& pt : points) {
        double d = std::abs(pt.strike - F);
        if (d < best_dist) { best_dist = d; atm_iv = pt.market_iv; }
    }

    // Correct initial alpha for SABR scale
    SABRParams init;
    init.alpha = atm_iv * std::pow(F, 1.0 - beta_);
    init.beta  = beta_;
    init.rho   = -0.3;
    init.nu    = 0.5;

    SABRParams result = lm_calibrate(F, T, points, init);
    calibration_error_bps = rmse_bps(F, T, result, points);
    return result;
}

// -- Smile curve ---------------------------------------------------------------
std::vector<std::pair<double,double>> SABRModel::smile(
    double F, double T, const SABRParams& p,
    double strike_lo, double strike_hi, int n_points) const
{
    std::vector<std::pair<double,double>> out;
    out.reserve(n_points);
    double step = (strike_hi - strike_lo) / (n_points - 1);
    for (int i = 0; i < n_points; i++) {
        double K = strike_lo + i * step;
        out.emplace_back(K, implied_vol(F, K, T, p));
    }
    return out;
}
