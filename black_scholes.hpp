#pragma once
#include "types.hpp"

// ── Normal distribution helpers ───────────────────────────────────────────────
namespace norm {
    // Standard normal PDF
    double pdf(double x);
    // Standard normal CDF (Hart approximation, error < 1.5e-7)
    double cdf(double x);
}

// ── Black-Scholes pricing & full Greeks ──────────────────────────────────────
//
// Model:  dS = (r - q) S dt + σ S dW
//
// S  = spot price
// K  = strike
// T  = time to expiry (years)
// r  = risk-free rate (continuous, annualised)
// q  = dividend / funding yield (continuous, annualised)
// σ  = implied volatility (annualised)
//
class BlackScholes {
public:
    // Price only
    static double price(const OptionContract& c, double sigma);

    // Full Greeks bundle (uses central finite differences for higher-order)
    static Greeks greeks(const OptionContract& c, double sigma);

    // Implied volatility via Newton-Raphson with Halley's method fallback
    // Returns NaN if no solution found within tolerance
    static double implied_vol(const OptionContract& c, double market_price);

    // d1, d2 for external use
    static void d1d2(double S, double K, double T, double r, double q,
                     double sigma, double& d1, double& d2);

private:
    static double _price(double S, double K, double T, double r,
                         double q, double sigma, OptionType type);
    static double _vega_raw(double S, double K, double T, double r,
                             double q, double sigma);
};
