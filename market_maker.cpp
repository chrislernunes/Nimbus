#include "market_maker.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>

OptionsMarketMaker::OptionsMarketMaker(const MMConfig& cfg) : cfg_(cfg) {}

void OptionsMarketMaker::update_surface(const SVISurface& surface) {
    surface_ = surface;
}

// -- Inventory vol skew --------------------------------------------------------
// If we're long vega, we want to sell -> lower ask vol, higher bid vol
// If we're short vega, we want to buy -> higher ask vol, lower bid vol
double OptionsMarketMaker::inventory_vol_skew(OptionType /*type*/) const {
    double vega_ratio = inventory_.net_vega / cfg_.max_inventory_vega;
    return std::clamp(vega_ratio, -1.0, 1.0) * cfg_.inventory_skew;
}

// -- Gamma addon ---------------------------------------------------------------
// Near-money options have high gamma -> wider spread to compensate for
// hedging cost (gamma scalping risk borne by market maker)
double OptionsMarketMaker::gamma_vol_addon(const Greeks& g) const {
    // Scale addon by normalized gamma exposure
    double gamma_notional = std::abs(g.gamma * 100.0);  // per $100 move
    return std::min(gamma_notional * 0.0001, 0.005);    // cap at 0.5% vol
}

// -- Quote generation ----------------------------------------------------------
MMQuote OptionsMarketMaker::quote(const OptionContract& c,
                                   double spot_price) const {
    MMQuote q{};

    // Get fair IV from surface
    double fair_iv = surface_.iv(c.strike, c.expiry);
    if (fair_iv <= 0.0) fair_iv = 0.30;  // fallback ATM vol

    // Compute mid Greeks at fair IV
    OptionContract mc = c;
    mc.spot = spot_price;
    q.greeks  = BlackScholes::greeks(mc, fair_iv);
    q.mid_iv  = fair_iv;

    // -- Spread construction ------------------------------------------------
    // Half-spread = base + inventory skew + gamma addon + uncertainty
    double skew    = inventory_vol_skew(c.type);
    double g_addon = gamma_vol_addon(q.greeks);

    double half_spread_vol = cfg_.base_spread_vol
                           + g_addon
                           + cfg_.vol_uncertainty;

    // Inventory skew: shifts mid, doesn't widen spread
    double bid_iv = fair_iv - half_spread_vol - skew;
    double ask_iv = fair_iv + half_spread_vol - skew;

    bid_iv = std::max(bid_iv, 0.01);
    ask_iv = std::max(ask_iv, bid_iv + 0.001);

    q.bid_iv = bid_iv;
    q.ask_iv = ask_iv;

    // -- Convert IV to prices -----------------------------------------------
    q.bid_price = BlackScholes::price(mc, bid_iv);
    q.ask_price = BlackScholes::price(mc, ask_iv);

    // Spread in bps of underlying
    double spread_dollar = q.ask_price - q.bid_price;
    q.spread_bps = spot_price > 0 ? (spread_dollar / spot_price) * 10000.0 : 0.0;

    return q;
}

// -- Fill simulation -----------------------------------------------------------
void OptionsMarketMaker::fill(const OptionContract& c, const MMQuote& q,
                               const std::string& side, double quantity,
                               long long ts) {
    double fill_price, fill_iv;
    if (side == "BID") {
        fill_price = q.bid_price;
        fill_iv    = q.bid_iv;
        quantity   = -std::abs(quantity);  // we sell at our bid
    } else {
        fill_price = q.ask_price;
        fill_iv    = q.ask_iv;
        quantity   = +std::abs(quantity);  // we buy at our ask
    }

    double fee = std::abs(quantity) * fill_price * cfg_.taker_fee_bps * 1e-4;

    OptionTrade trade;
    trade.contract      = c;
    trade.fill_price    = fill_price;
    trade.fill_iv       = fill_iv;
    trade.quantity      = quantity;
    trade.delta_at_fill = q.greeks.delta;
    trade.vega_at_fill  = q.greeks.vega;
    trade.fee           = fee;
    trade.timestamp_ms  = ts;
    trade.side          = side == "BID" ? "BID_FILL" : "ASK_FILL";
    option_trades_.push_back(trade);

    // Update inventory Greeks
    double notional = std::abs(quantity);
    double sign     = quantity > 0 ? 1.0 : -1.0;
    inventory_.net_delta += sign * notional * q.greeks.delta;
    inventory_.net_gamma += sign * notional * q.greeks.gamma;
    inventory_.net_vega  += sign * notional * q.greeks.vega * c.spot;
    inventory_.net_theta += sign * notional * q.greeks.theta;
    inventory_.net_vanna += sign * notional * q.greeks.vanna;
    inventory_.net_volga += sign * notional * q.greeks.volga;
    inventory_.trade_count++;
    inventory_.realized_pnl -= fee;

    // Open position tracking
    bool found = false;
    for (auto& pos : positions_) {
        if (pos.contract.strike  == c.strike &&
            pos.contract.expiry  == c.expiry &&
            pos.contract.type    == c.type) {
            double new_qty = pos.quantity + quantity;
            if (std::abs(new_qty) < 1e-10) {
                // Closed: realise PnL
                double pnl = quantity * (pos.avg_price - fill_price) - fee;
                inventory_.realized_pnl += pnl;
                pos.quantity = 0.0;
            } else {
                if ((pos.quantity > 0) == (quantity > 0)) {
                    // Same direction: average in
                    pos.avg_price = (pos.avg_price * pos.quantity +
                                     fill_price * quantity) / new_qty;
                } else {
                    // Partial close
                    double closed = std::min(std::abs(quantity), std::abs(pos.quantity));
                    double sign_close = quantity > 0 ? 1.0 : -1.0;
                    double pnl = sign_close * closed * (fill_price - pos.avg_price) - fee;
                    inventory_.realized_pnl += pnl;
                }
                pos.quantity = new_qty;
            }
            found = true;
            break;
        }
    }
    if (!found) {
        positions_.push_back({c, quantity, fill_price, fill_iv});
    }

    std::cout << std::fixed << std::setprecision(2)
              << "[FILL] " << to_string(c.type)
              << " K=" << std::setprecision(0) << c.strike
              << " T=" << (int)(c.expiry*365) << "d"
              << " iv=" << std::setprecision(2) << fill_iv*100 << "%"
              << " px=" << fill_price
              << " qty=" << std::setprecision(1) << quantity
              << " fee=$" << std::setprecision(4) << fee
              << " " << trade.side << "\n";
}

// -- Delta hedge ---------------------------------------------------------------
std::vector<HedgeTrade> OptionsMarketMaker::hedge_delta(double spot_price,
                                                          double spot_bid,
                                                          double spot_ask,
                                                          long long ts) {
    std::vector<HedgeTrade> hedges;
    double delta = inventory_.net_delta;

    if (std::abs(delta) < 1e-8) return hedges;

    // Flatten: sell delta if positive, buy if negative
    double hedge_qty   = -delta;
    double fill_price  = hedge_qty > 0 ? spot_ask : spot_bid;
    double fee         = std::abs(hedge_qty) * fill_price * cfg_.taker_fee_bps * 1e-4;

    HedgeTrade h;
    h.underlying    = "SPOT";
    h.quantity      = hedge_qty;
    h.price         = fill_price;
    h.fee           = fee;
    h.timestamp_ms  = ts;
    hedges.push_back(h);
    hedge_trades_.push_back(h);

    inventory_.net_delta   += hedge_qty;
    inventory_.realized_pnl -= fee;

    std::cout << std::fixed << std::setprecision(4)
              << "[HEDGE]  SPOT"
              << "  qty=" << hedge_qty
              << "  @ "   << fill_price
              << "  fee=$" << fee
              << "  delta_after=" << inventory_.net_delta << "\n";

    return hedges;
}

// -- Mark to market ------------------------------------------------------------
void OptionsMarketMaker::mark_to_market(double spot_price) {
    double unrealized = 0.0;
    for (const auto& pos : positions_) {
        if (std::abs(pos.quantity) < 1e-10) continue;
        OptionContract mc = pos.contract;
        mc.spot = spot_price;
        double fair_iv  = surface_.iv(mc.strike, mc.expiry);
        if (fair_iv <= 0.0) fair_iv = pos.avg_iv;
        double mkt_price = BlackScholes::price(mc, fair_iv);
        unrealized += pos.quantity * (mkt_price - pos.avg_price);
    }
    inventory_.unrealized_pnl = unrealized;
}

// -- Reporting -----------------------------------------------------------------
void OptionsMarketMaker::print_greeks() const {
    const auto& I = inventory_;
    std::cout << std::fixed << std::setprecision(4)
              << "\n  Net Greeks\n"
              << "  delta    " << I.net_delta << "\n"
              << "  gamma    " << I.net_gamma << "\n"
              << "  vega $   " << I.net_vega  << "\n"
              << "  theta $  " << I.net_theta << "\n"
              << "  vanna    " << I.net_vanna << "\n"
              << "  volga    " << I.net_volga << "\n";
}

void OptionsMarketMaker::print_pnl() const {
    double total = inventory_.realized_pnl + inventory_.unrealized_pnl;
    std::cout << std::fixed << std::setprecision(2)
              << "\n  PnL Summary\n"
              << "  realized    $" << inventory_.realized_pnl   << "\n"
              << "  unrealized  $" << inventory_.unrealized_pnl << "\n"
              << "  total       $" << total                     << "\n"
              << "  trades      "  << inventory_.trade_count    << "\n\n";
}

void OptionsMarketMaker::print_inventory() const {
    std::cout << "\n  Open Positions\n"
              << "  " << std::left
              << std::setw(6) << "Type"
              << std::setw(10) << "Strike"
              << std::setw(8)  << "Days"
              << std::setw(10) << "Qty"
              << std::setw(10) << "AvgPx"
              << std::setw(10) << "AvgIV" << "\n";

    for (const auto& pos : positions_) {
        if (std::abs(pos.quantity) < 1e-10) continue;
        std::ostringstream iv_s;
        iv_s << std::fixed << std::setprecision(4) << pos.avg_iv * 100 << "%";
        std::cout << std::fixed
                  << "  " << std::setw(6) << to_string(pos.contract.type)
                  << std::setprecision(0) << std::setw(12) << pos.contract.strike
                  << std::setw(8)  << (int)(pos.contract.expiry * 365)
                  << std::setprecision(4)
                  << std::setw(10) << pos.quantity
                  << std::setw(10) << pos.avg_price
                  << std::setw(10) << iv_s.str() << "\n";
    }
}

void OptionsMarketMaker::print_trades() const {
    std::cout << "\n  Trade Log\n"
              << "  " << std::left
              << std::setw(10) << "Side"
              << std::setw(6)  << "Type"
              << std::setw(10) << "Strike"
              << std::setw(8)  << "Days"
              << std::setw(10) << "IV"
              << std::setw(10) << "Price"
              << std::setw(8)  << "Qty"
              << "Fee\n";

    for (const auto& t : option_trades_) {
        std::ostringstream fiv_s;
        fiv_s << std::fixed << std::setprecision(4) << t.fill_iv * 100 << "%";
        std::cout << std::fixed
                  << "  " << std::setw(10) << t.side
                  << std::setw(6)  << to_string(t.contract.type)
                  << std::setprecision(0) << std::setw(12) << t.contract.strike
                  << std::setw(8)  << (int)(t.contract.expiry * 365)
                  << std::setprecision(4)
                  << std::setw(10) << fiv_s.str()
                  << std::setw(10) << t.fill_price
                  << std::setw(8)  << t.quantity
                  << "$" << t.fee << "\n";
    }
}