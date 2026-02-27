#define _USE_MATH_DEFINES
#include "black_scholes.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

// -- Normal distribution -------------------------------------------------------
namespace norm {

double pdf(double x) {
    return std::exp(-0.5 * x * x) / constants::SQRT_2PI;
}

// Abramowitz & Stegun 26.2.17 rational approximation
double cdf(double x) {
    if (x >  8.0) return 1.0;
    if (x < -8.0) return 0.0;
    bool neg = x < 0.0;
    if (neg) x = -x;
    double t  = 1.0 / (1.0 + 0.2316419 * x);
    double t2 = t * t, t3 = t2 * t, t4 = t3 * t, t5 = t4 * t;
    double poly = 0.319381530 * t
                - 0.356563782 * t2
                + 1.781477937 * t3
                - 1.821255978 * t4
                + 1.330274429 * t5;
    double val = 1.0 - pdf(x) * poly;
    return neg ? 1.0 - val : val;
}

} // namespace norm

// -- d1, d2 --------------------------------------------------------------------
void BlackScholes::d1d2(double S, double K, double T, double r, double q,
                         double sigma, double& d1, double& d2) {
    double vol_sqrt_T = sigma * std::sqrt(T);
    d1 = (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) / vol_sqrt_T;
    d2 = d1 - vol_sqrt_T;
}

// -- Internal pricer -----------------------------------------------------------
double BlackScholes::_price(double S, double K, double T, double r,
                             double q, double sigma, OptionType type) {
    if (T <= 0.0) {
        // Intrinsic at expiry
        double intr = type == OptionType::CALL ? std::max(S - K, 0.0)
                                               : std::max(K - S, 0.0);
        return intr;
    }
    double d1, d2;
    d1d2(S, K, T, r, q, sigma, d1, d2);
    double disc_r = std::exp(-r * T);
    double disc_q = std::exp(-q * T);

    if (type == OptionType::CALL)
        return S * disc_q * norm::cdf(d1) - K * disc_r * norm::cdf(d2);
    else
        return K * disc_r * norm::cdf(-d2) - S * disc_q * norm::cdf(-d1);
}

// -- Raw vega (not scaled) -----------------------------------------------------
double BlackScholes::_vega_raw(double S, double K, double T, double r,
                                double q, double sigma) {
    if (T <= 0.0) return 0.0;
    double d1, d2;
    d1d2(S, K, T, r, q, sigma, d1, d2);
    return S * std::exp(-q * T) * norm::pdf(d1) * std::sqrt(T);
}

// -- Public price --------------------------------------------------------------
double BlackScholes::price(const OptionContract& c, double sigma) {
    return _price(c.spot, c.strike, c.expiry, c.rate, c.div_yield, sigma, c.type);
}

// -- Full Greeks ---------------------------------------------------------------
Greeks BlackScholes::greeks(const OptionContract& c, double sigma) {
    Greeks g;
    double S = c.spot, K = c.strike, T = c.expiry;
    double r = c.rate, q = c.div_yield;

    if (T <= 0.0 || sigma <= 0.0) {
        g.price = price(c, sigma);
        return g;
    }

    double d1, d2;
    d1d2(S, K, T, r, q, sigma, d1, d2);

    double Nd1  = norm::cdf(d1);
    double Nd2  = norm::cdf(d2);
    double nd1  = norm::pdf(d1);
    double sqT  = std::sqrt(T);
    double disc_r = std::exp(-r * T);
    double disc_q = std::exp(-q * T);

    // Price
    if (c.type == OptionType::CALL)
        g.price = S * disc_q * Nd1 - K * disc_r * Nd2;
    else
        g.price = K * disc_r * (1.0 - Nd2) - S * disc_q * (1.0 - Nd1);

    // Delta
    if (c.type == OptionType::CALL)
        g.delta = disc_q * Nd1;
    else
        g.delta = disc_q * (Nd1 - 1.0);

    // Gamma (same for call & put)
    g.gamma = disc_q * nd1 / (S * sigma * sqT);

    // Vega: per 1% change in vol
    g.vega = S * disc_q * nd1 * sqT * 0.01;

    // Theta: per calendar day
    double theta_raw = -(S * disc_q * nd1 * sigma / (2.0 * sqT));
    if (c.type == OptionType::CALL)
        theta_raw += q * S * disc_q * Nd1 - r * K * disc_r * Nd2;
    else
        theta_raw += -q * S * disc_q * (1.0 - Nd1) + r * K * disc_r * (1.0 - Nd2);
    g.theta = theta_raw / constants::DAYS_YEAR;

    // Rho: per 1% change in rate
    if (c.type == OptionType::CALL)
        g.rho = K * T * disc_r * Nd2 * 0.01;
    else
        g.rho = -K * T * disc_r * (1.0 - Nd2) * 0.01;

    // Vanna: d²V / dS dsigma
    g.vanna = -disc_q * nd1 * d2 / sigma;

    // Volga (Vomma): d²V / dsigma²  -- per 1% vol, per 1% vol
    g.volga = g.vega * d1 * d2 / sigma * 0.01;

    // Charm: dΔ/dt per day
    double charm_raw;
    if (c.type == OptionType::CALL)
        charm_raw = -disc_q * (nd1 * (2.0*(r-q)*T - d2*sigma*sqT) /
                                (2.0*T*sigma*sqT) + q * Nd1);
    else
        charm_raw = -disc_q * (nd1 * (2.0*(r-q)*T - d2*sigma*sqT) /
                                (2.0*T*sigma*sqT) - q * (1.0-Nd1));
    g.charm = charm_raw / constants::DAYS_YEAR;

    // Speed: d³V / dS³
    g.speed = -g.gamma / S * (d1 / (sigma * sqT) + 1.0);

    return g;
}

// -- Implied Volatility (Newton-Raphson + Halley fallback) ---------------------
double BlackScholes::implied_vol(const OptionContract& c, double market_price) {
    const double S = c.spot, K = c.strike, T = c.expiry;
    if (T <= 0.0) return std::numeric_limits<double>::quiet_NaN();

    // Intrinsic value check
    double intrinsic = c.type == OptionType::CALL ? std::max(S - K * std::exp(-c.rate * T), 0.0)
                                                  : std::max(K * std::exp(-c.rate * T) - S, 0.0);
    if (market_price < intrinsic - 1e-6)
        return std::numeric_limits<double>::quiet_NaN();

    // Initial guess: Brenner-Subrahmanyam approximation
    double sigma = std::sqrt(2.0 * M_PI / T) * market_price / S;
    if (sigma < constants::MIN_VOL) sigma = 0.2;
    if (sigma > constants::MAX_VOL) sigma = constants::MAX_VOL;

    for (int i = 0; i < constants::IV_MAX_IT; i++) {
        double p    = _price(S, K, T, c.rate, c.div_yield, sigma, c.type);
        double diff = p - market_price;

        if (std::abs(diff) < constants::IV_TOL) return sigma;

        double v = _vega_raw(S, K, T, c.rate, c.div_yield, sigma);
        if (v < 1e-12) break;  // vega too small, Newton diverges

        // Halley's method: faster convergence near boundaries
        double d1, d2;
        d1d2(S, K, T, c.rate, c.div_yield, sigma, d1, d2);
        double d2_vega = v * d1 * d2 / sigma;   // d(vega)/d(sigma)
        double step    = diff / (v - 0.5 * diff * d2_vega / v);
        sigma -= step;

        if (sigma < constants::MIN_VOL) sigma = constants::MIN_VOL;
        if (sigma > constants::MAX_VOL) sigma = constants::MAX_VOL;
    }

    // Final check
    double p = _price(S, K, T, c.rate, c.div_yield, sigma, c.type);
    if (std::abs(p - market_price) < 1e-4) return sigma;
    return std::numeric_limits<double>::quiet_NaN();
}
