#include "svi_surface.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <iomanip>
#include <stdexcept>

// -- Total variance from SVI ---------------------------------------------------
double SVISurface::total_variance(double k, const SVIParams& p) const {
    double diff = k - p.m;
    return p.a + p.b * (p.rho * diff + std::sqrt(diff*diff + p.sigma*p.sigma));
}

// -- RMSE of SVI fit in bps ----------------------------------------------------
double SVISurface::svi_rmse(double F, double T, const SVIParams& p,
                             const std::vector<SurfacePoint>& pts) const {
    if (pts.empty() || T <= 0.0) return 0.0;
    double sum = 0.0;
    for (const auto& pt : pts) {
        double k   = std::log(pt.strike / F);
        double w   = total_variance(k, p);
        if (w <= 0.0) { sum += 1e6; continue; }
        double iv  = std::sqrt(w / T);
        double d   = (iv - pt.mid_iv) * 10000.0;
        sum += d * d;
    }
    return std::sqrt(sum / pts.size());
}

// -- Gradient-descent SVI fit with multi-start ---------------------------------
SVIParams SVISurface::fit_single(double F, double T,
                                  const std::vector<SurfacePoint>& pts) const {
    if (pts.empty()) throw std::invalid_argument("No surface points");

    // Sort by strike
    auto sorted = pts;
    std::sort(sorted.begin(), sorted.end(),
              [](const SurfacePoint& a, const SurfacePoint& b){
                  return a.strike < b.strike; });

    // ATM total variance
    double atm_iv  = sorted[sorted.size()/2].mid_iv;
    double atm_var = atm_iv * atm_iv * T;

    auto clamp_params = [](SVIParams q) {
        q.b     = std::clamp(q.b,     1e-5, 3.99);
        q.sigma = std::max(q.sigma,   1e-5);
        q.rho   = std::clamp(q.rho,   -0.999, 0.999);
        q.a     = std::max(q.a,       1e-10);
        // Butterfly no-arb
        double max_b = 4.0 / (1.0 + std::abs(q.rho)) - 1e-6;
        q.b = std::min(q.b, max_b);
        return q;
    };

    SVIParams best_p;
    double    best_err = 1e18;

    // Multiple starting points covering different regimes
    std::vector<SVIParams> starts;
    for (double b_s    : {0.05, 0.15, 0.30}) {
    for (double rho_s  : {-0.5, -0.2, 0.0}) {
    for (double sig_s  : {0.10, 0.20}) {
        SVIParams s;
        s.a     = std::max(atm_var * (1.0 - b_s * 0.5), 1e-10);
        s.b     = b_s;
        s.rho   = rho_s;
        s.m     = 0.0;
        s.sigma = sig_s;
        starts.push_back(clamp_params(s));
    }}}

    for (const auto& start : starts) {
        SVIParams p   = start;
        double    err = svi_rmse(F, T, p, pts);

        auto try_update = [&](SVIParams q) {
            q = clamp_params(q);
            double e = svi_rmse(F, T, q, pts);
            if (e < err) { err = e; p = q; return true; }
            return false;
        };

        // Scales: a~atm_var, b~0.1, rho~0.1, m~0.1, sigma~0.1
        double scales[5] = { atm_var, 0.1, 0.1, 0.1, 0.1 };

        for (double sf : {1.0, 0.1, 0.01, 0.001, 0.0001, 0.00001}) {
            bool any = true;
            while (any) {
                any = false;
                for (int i = 0; i < 5; i++) {
                    double step = scales[i] * sf;
                    SVIParams q = p;
                    double* params[] = {&q.a, &q.b, &q.rho, &q.m, &q.sigma};
                    *params[i] += step;
                    if (try_update(q)) { any = true; continue; }
                    q = p;
                    *params[i] -= step;
                    if (try_update(q)) { any = true; }
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

// -- Fit a single slice --------------------------------------------------------
SVISlice SVISurface::fit_slice(double F, double T,
                                const std::vector<SurfacePoint>& pts) const {
    SVISlice sl;
    sl.expiry    = T;
    sl.params    = fit_single(F, T, pts);
    sl.fit_error = svi_rmse(F, T, sl.params, pts);
    return sl;
}

// -- Build full surface from market data ---------------------------------------
void SVISurface::build(double spot, double rate, double div_yield,
                        const std::vector<SurfacePoint>& market_pts) {
    spot_ = spot; rate_ = rate; div_ = div_yield;

    // Group by expiry
    std::map<double, std::vector<SurfacePoint>> by_expiry;
    for (const auto& pt : market_pts)
        by_expiry[pt.expiry].push_back(pt);

    slices_.clear();
    for (const auto& [T, pts] : by_expiry) {
        double F = spot * std::exp((rate - div_yield) * T);
        slices_.push_back(fit_slice(F, T, pts));
    }
    // Sort by expiry
    std::sort(slices_.begin(), slices_.end(),
              [](const SVISlice& a, const SVISlice& b){ return a.expiry < b.expiry; });
}

// -- Query surface at (K, T) ---------------------------------------------------
double SVISurface::iv(double K, double T) const {
    if (slices_.empty()) return 0.0;

    double F = spot_ * std::exp((rate_ - div_) * T);
    double k = std::log(K / F);

    // Extrapolate flat outside range
    if (T <= slices_.front().expiry) {
        double w = total_variance(k, slices_.front().params);
        return w > 0 ? std::sqrt(w / T) : 0.0;
    }
    if (T >= slices_.back().expiry) {
        double w = total_variance(k, slices_.back().params);
        return w > 0 ? std::sqrt(w / T) : 0.0;
    }

    // Linear interpolation in total variance (calendar-spread safe)
    for (size_t i = 1; i < slices_.size(); i++) {
        if (T <= slices_[i].expiry) {
            double T1 = slices_[i-1].expiry;
            double T2 = slices_[i].expiry;
            double w1 = total_variance(k, slices_[i-1].params);
            double w2 = total_variance(k, slices_[i].params);
            // Calendar interpolation: w is linear in T by construction
            double alpha = (T - T1) / (T2 - T1);
            double w = (1.0 - alpha) * w1 * T1/T + alpha * w2 * T2/T;
            return w > 0 ? std::sqrt(w) : 0.0;
        }
    }
    return 0.0;
}

// -- Print surface summary -----------------------------------------------------
void SVISurface::print_summary() const {
    std::cout << "\n  SVI Vol Surface -- " << slices_.size() << " expiry slices\n\n"
              << std::left
              << "  " << std::setw(10) << "Expiry"
              << std::setw(8)  << "Days"
              << std::setw(10) << "a"
              << std::setw(10) << "b"
              << std::setw(10) << "rho"
              << std::setw(10) << "m"
              << std::setw(10) << "sigma"
              << std::setw(10) << "Fit(bps)"
              << "\n";

    for (const auto& sl : slices_) {
        const auto& p = sl.params;
        std::cout << std::fixed << std::setprecision(4)
                  << "  " << std::setw(10) << sl.expiry
                  << std::setw(8)  << (int)(sl.expiry * 365)
                  << std::setw(10) << p.a
                  << std::setw(10) << p.b
                  << std::setw(10) << p.rho
                  << std::setw(10) << p.m
                  << std::setw(10) << p.sigma
                  << std::setw(10) << sl.fit_error
                  << "\n";
    }
    std::cout << "\n";
}
