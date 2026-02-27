#include "live_surface.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <map>
#include <windows.h>

static long long now_ms_surf() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

#ifdef NIMBUS_COLOUR
#define RESET  "\033[0m"
#define GREEN  "\033[32m"
#define RED    "\033[31m"
#define YELLOW "\033[33m"
#define CYAN   "\033[36m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"
#else
#define RESET  ""
#define GREEN  ""
#define RED    ""
#define YELLOW ""
#define CYAN   ""
#define BOLD   ""
#define DIM    ""
#endif

LiveSurfaceEngine::LiveSurfaceEngine() {
    latest_.status = "INIT";
}

void LiveSurfaceEngine::on_update(UpdateCallback cb) {
    std::lock_guard<std::mutex> lk(mutex_);
    callbacks_.push_back(std::move(cb));
}

void LiveSurfaceEngine::attach_market_maker(OptionsMarketMaker* mm) {
    std::lock_guard<std::mutex> lk(mutex_);
    mm_ = mm;
}

// -- Quality filter ------------------------------------------------------------
std::vector<SurfacePoint> LiveSurfaceEngine::filter_chain(
    const OptionsChain& chain) const
{
    std::vector<SurfacePoint> pts;
    if (chain.spot <= 0.0) return pts;

    for (const auto& t : chain.tickers) {
        // Expiry filter
        double days = t.expiry_years * 365.0;
        if (days < filter_.min_expiry_days)  continue;
        if (days > filter_.max_expiry_days)  continue;

        // IV filter
        if (t.mark_iv < filter_.min_iv)      continue;
        if (t.mark_iv > filter_.max_iv)      continue;
        if (t.mark_price <= 0.0)             continue;

        // Moneyness filter
        double moneyness = t.strike / chain.spot;
        if (moneyness < filter_.min_moneyness) continue;
        if (moneyness > filter_.max_moneyness) continue;

        // Open interest filter
        if (t.open_interest < filter_.min_open_interest) continue;

        SurfacePoint p;
        p.strike = t.strike;
        p.expiry = t.expiry_years;
        p.mid_iv = t.mark_iv;
        // Use bid/ask if available, else ±1%
        p.bid_iv = t.bid > 0 ? t.mark_iv * 0.995 : t.mark_iv * 0.99;
        p.ask_iv = t.ask > 0 ? t.mark_iv * 1.005 : t.mark_iv * 1.01;
        pts.push_back(p);
    }
    return pts;
}

// -- ATM IV from surface -------------------------------------------------------
double LiveSurfaceEngine::compute_atm_iv(const SVISurface& surface,
                                          double spot, double T) const {
    return surface.iv(spot, T);
}

// -- 25-delta skew: put IV (25d) - call IV (25d) -------------------------------
// Approximation: 25d put ≈ K where |delta| = 0.25
// Using BSM inversion: K_25d ≈ F * exp(±sigma√T * N⁻¹(0.25))
double LiveSurfaceEngine::compute_skew_25d(const SVISurface& surface,
                                            double spot, double T,
                                            double rate) const {
    if (T <= 0.0) return 0.0;
    double F    = spot * std::exp(rate * T);
    double atm  = surface.iv(F, T);
    if (atm <= 0.0) return 0.0;

    // N⁻¹(0.25) ≈ -0.6745
    double sqT     = std::sqrt(T);
    double K_put25 = F * std::exp(-atm * sqT * 0.6745 - 0.5 * atm * atm * T);
    double K_cal25 = F * std::exp(+atm * sqT * 0.6745 - 0.5 * atm * atm * T);

    double iv_put25 = surface.iv(K_put25, T);
    double iv_cal25 = surface.iv(K_cal25, T);
    return iv_put25 - iv_cal25;
}

// -- Term spread: back-month ATM - front-month ATM -----------------------------
double LiveSurfaceEngine::compute_term_spread(const SVISurface& surface,
                                               double spot) const {
    const auto& slices = surface.slices();
    if (slices.size() < 2) return 0.0;
    double T1 = slices.front().expiry;
    double T2 = slices.back().expiry;
    double iv1 = surface.iv(spot, T1);
    double iv2 = surface.iv(spot, T2);
    return iv2 - iv1;
}

// -- Main process --------------------------------------------------------------
void LiveSurfaceEngine::process(const OptionsChain& chain) {
    SurfaceUpdate upd;
    upd.timestamp_ms = chain.timestamp_ms;
    upd.spot         = chain.spot;

    try {
        // 1. Filter
        auto pts = filter_chain(chain);
        upd.n_contracts = static_cast<int>(pts.size());

        if (pts.empty()) {
            upd.status = "ERROR: no usable contracts after filtering";
            std::cerr << "[Surface] " << upd.status << "\n";
            std::lock_guard<std::mutex> lk(mutex_);
            health_.n_failures++;
            return;
        }

        // 2. Group by expiry, check min_strikes_per_slice
        std::map<double, std::vector<SurfacePoint>> by_expiry;
        for (const auto& p : pts)
            by_expiry[p.expiry].push_back(p);

        std::vector<SurfacePoint> filtered_pts;
        for (auto& [T, slice_pts] : by_expiry) {
            if ((int)slice_pts.size() >= filter_.min_strikes_per_slice)
                for (const auto& p : slice_pts) filtered_pts.push_back(p);
        }

        if (filtered_pts.empty()) {
            upd.status = "ERROR: no expiry slice has enough strikes";
            std::lock_guard<std::mutex> lk(mutex_);
            health_.n_failures++;
            return;
        }

        // 3. Build SVI surface
        SVISurface surface;
        surface.build(chain.spot, 0.05, 0.0, filtered_pts);
        upd.surface   = surface;
        upd.n_slices  = static_cast<int>(surface.slices().size());

        if (upd.n_slices == 0) {
            upd.status = "ERROR: SVI fit produced no slices";
            std::lock_guard<std::mutex> lk(mutex_);
            health_.n_failures++;
            return;
        }

        // 4. SABR calibration on front-month slice
        double T_front = surface.slices().front().expiry;
        double F_front = chain.spot * std::exp(0.05 * T_front);

        // Build CalibrationPoints from front-month filtered pts
        std::vector<CalibrationPoint> calib_pts;
        for (const auto& p : filtered_pts) {
            if (std::abs(p.expiry - T_front) < 1e-6) {
                CalibrationPoint cp;
                cp.strike    = p.strike;
                cp.expiry    = p.expiry;
                cp.market_iv = p.mid_iv;
                cp.weight    = 1.0;
                calib_pts.push_back(cp);
            }
        }

        if (calib_pts.size() >= 3) {
            double err_bps;
            upd.sabr_params  = sabr_.calibrate(F_front, T_front, calib_pts, err_bps);
            upd.sabr_fit_bps = err_bps;
        }

        // 5. Surface analytics
        upd.atm_iv      = compute_atm_iv(surface, chain.spot, T_front);
        upd.skew_25d    = compute_skew_25d(surface, chain.spot, T_front, 0.05);
        upd.term_spread = compute_term_spread(surface, chain.spot);
        upd.status      = "OK";

        // 6. Update market maker if attached
        if (mm_) mm_->update_surface(surface);

        // 7. Store history
        {
            std::lock_guard<std::mutex> lk(mutex_);
            latest_ = upd;
            history_.push_back(upd);
            if ((int)history_.size() > MAX_HISTORY) history_.pop_front();

            health_.n_updates++;
            health_.last_update_ms = upd.timestamp_ms;
            health_.is_stale       = false;
            health_.staleness_ms   = 0.0;

            // Running avg fit error
            double total_err = 0.0;
            double max_err   = 0.0;
            for (const auto& sl : surface.slices()) {
                total_err += sl.fit_error;
                max_err    = std::max(max_err, sl.fit_error);
            }
            int ns = upd.n_slices;
            health_.avg_fit_error_bps = ns > 0 ? total_err / ns : 0.0;
            health_.max_fit_error_bps = max_err;
        }

        // 8. Fire callbacks
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto& cb : callbacks_) cb(upd);
        }

    } catch (const std::exception& e) {
        upd.status = std::string("EXCEPTION: ") + e.what();
        std::cerr << "[Surface] " << upd.status << "\n";
        std::lock_guard<std::mutex> lk(mutex_);
        health_.n_failures++;
    }
}

SurfaceUpdate LiveSurfaceEngine::latest() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return latest_;
}

SurfaceHealth LiveSurfaceEngine::health() const {
    std::lock_guard<std::mutex> lk(mutex_);
    SurfaceHealth h = health_;
    if (!h.is_stale && h.last_update_ms > 0) {
        h.staleness_ms = static_cast<double>(now_ms_surf() - h.last_update_ms);
        h.is_stale     = h.staleness_ms > 60000.0; // stale after 60s
    }
    return h;
}

// -- Pretty printer ------------------------------------------------------------
void LiveSurfaceEngine::print_live_surface() const {
    std::lock_guard<std::mutex> lk(mutex_);
    const auto& u = latest_;
    const auto& h = health_;

    std::string status_color = (u.status == "OK") ? GREEN : RED;
    std::string stale_color  = h.is_stale ? RED : GREEN;

    std::cout << "\n" << BOLD << CYAN
              << "-- Live Vol Surface --\n" << RESET
              << std::fixed << std::setprecision(2)
              << "  spot       $" << u.spot << "\n"
              << "  status     " << status_color << u.status << RESET << "\n"
              << "  contracts  " << u.n_contracts << "\n"
              << "  slices     " << u.n_slices << "\n"
              << std::setprecision(1)
              << "  ATM IV     " << u.atm_iv * 100.0 << "%\n"
              << "  25d skew   " << u.skew_25d * 100.0 << "% (put - call)\n"
              << "  term sprd  " << u.term_spread * 100.0 << "% (back - front)\n"
              << "  SABR fit   " << u.sabr_fit_bps << " bps\n"
              << "    alpha=" << std::setprecision(4) << u.sabr_params.alpha
              << "  rho=" << u.sabr_params.rho
              << "  nu=" << u.sabr_params.nu << "\n"
              << "\n"
              << "  updates    " << h.n_updates << "\n"
              << "  failures   " << h.n_failures << "\n"
              << "  avg fit    " << std::setprecision(1) << h.avg_fit_error_bps << " bps\n"
              << "  max fit    " << h.max_fit_error_bps << " bps\n"
              << "  staleness  " << stale_color
              << std::setprecision(0) << h.staleness_ms / 1000.0 << "s" << RESET << "\n\n";

    // Slice table
    if (!u.surface.slices().empty()) {
        std::cout << "  " << std::left
                  << std::setw(8)  << "Expiry"
                  << std::setw(7)  << "Days"
                  << std::setw(10) << "ATM IV"
                  << std::setw(10) << "25d Skew"
                  << std::setw(10) << "Fit(bps)"
                  << "Arb-free\n"
                  << "  " << std::string(50, '-') << "\n";

        for (const auto& sl : u.surface.slices()) {
            double atm_iv = u.surface.iv(u.spot, sl.expiry);
            bool arb_free = sl.params.is_arbitrage_free();
            std::string af_color = arb_free ? GREEN : RED;

            std::cout << std::fixed << std::setprecision(4)
                      << "  " << std::setw(8)  << sl.expiry
                      << std::setw(7)  << (int)(sl.expiry * 365)
                      << std::setw(10) << atm_iv * 100.0
                      << std::setw(10) << "n/a"   // per-slice skew
                      << std::setprecision(1)
                      << std::setw(10) << sl.fit_error
                      << af_color << (arb_free ? "OK" : "FAIL") << RESET << "\n";
        }
    }
    std::cout << "\n";
}
