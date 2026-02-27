#pragma once
#include "types.hpp"
#include "black_scholes.hpp"
#include "svi_surface.hpp"
#include <vector>
#include <map>
#include <string>

// ── Market Maker Configuration ────────────────────────────────────────────────
struct MMConfig {
    double base_spread_vol    = 0.005;  // base half-spread in vol units (0.5%)
    double inventory_skew     = 0.002;  // per-unit inventory skew in vol
    double max_inventory_vega = 50000;  // USD vega limit before widening
    double max_inventory_delta= 0.5;    // delta limit before hedging
    double vol_uncertainty    = 0.001;  // model uncertainty add-on
    double min_edge_bps       = 2.0;    // minimum required edge in bps
    double taker_fee_bps      = 0.5;    // exchange taker fee
    double maker_fee_bps      = 0.2;    // exchange maker fee
};

// ── Inventory state ───────────────────────────────────────────────────────────
struct InventoryState {
    double net_delta   = 0.0;   // total portfolio delta
    double net_gamma   = 0.0;
    double net_vega    = 0.0;   // USD vega
    double net_theta   = 0.0;
    double net_vanna   = 0.0;
    double net_volga   = 0.0;
    double realized_pnl= 0.0;
    double unrealized_pnl = 0.0;
    int    trade_count = 0;
};

// ── Trade record ──────────────────────────────────────────────────────────────
struct OptionTrade {
    OptionContract contract;
    double         fill_price;
    double         fill_iv;
    double         quantity;    // + = long, - = short
    double         delta_at_fill;
    double         vega_at_fill;
    double         fee;
    long long      timestamp_ms;
    std::string    side;        // "BID_FILL" or "ASK_FILL"
};

// ── Delta hedge record ────────────────────────────────────────────────────────
struct HedgeTrade {
    std::string underlying;
    double      quantity;
    double      price;
    double      fee;
    long long   timestamp_ms;
};

// ── Options Market Maker ──────────────────────────────────────────────────────
//
// Quotes options using:
//   1. Fair value from SVI surface IV + Black-Scholes pricing
//   2. Bid/ask spread widened for:
//      - Inventory risk (skew quotes toward reducing exposure)
//      - Vol uncertainty
//      - Gamma / vega concentration
//   3. Delta hedge on every fill (if auto_hedge = true)
//   4. Edge filter: only quote if bid-ask > min_edge_bps
//
class OptionsMarketMaker {
public:
    explicit OptionsMarketMaker(const MMConfig& cfg = MMConfig{});

    // Update the underlying vol surface
    void update_surface(const SVISurface& surface);

    // Generate a two-sided quote for a given option
    // spot_price: current underlying price
    MMQuote quote(const OptionContract& c, double spot_price) const;

    // Simulate a fill (either side)
    // side: "BID" (we sell) or "ASK" (we buy)
    void fill(const OptionContract& c, const MMQuote& q,
              const std::string& side, double quantity, long long ts);

    // Delta hedge: flatten delta exposure using underlying
    // Returns hedge trade(s) needed
    std::vector<HedgeTrade> hedge_delta(double spot_price,
                                         double spot_bid, double spot_ask,
                                         long long ts);

    // Mark-to-market: update unrealized PnL
    void mark_to_market(double spot_price);

    // Reporting
    void print_inventory()  const;
    void print_greeks()     const;
    void print_trades()     const;
    void print_pnl()        const;

    const InventoryState& inventory()  const { return inventory_; }
    const MMConfig&       config()     const { return cfg_; }

private:
    MMConfig       cfg_;
    SVISurface     surface_;
    InventoryState inventory_;

    std::vector<OptionTrade> option_trades_;
    std::vector<HedgeTrade>  hedge_trades_;

    // Per-position: open option positions
    struct OpenPosition {
        OptionContract contract;
        double         quantity;
        double         avg_price;
        double         avg_iv;
    };
    std::vector<OpenPosition> positions_;

    // Spread adjustments
    double inventory_vol_skew(OptionType type) const;
    double gamma_vol_addon(const Greeks& g) const;
};
