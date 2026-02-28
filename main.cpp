#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <cmath>
#include <string>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <windows.h>

#include "types.hpp"
#include "black_scholes.hpp"
#include "sabr.hpp"
#include "svi_surface.hpp"
#include "market_maker.hpp"
#include "binance_options.hpp"
#include "live_surface.hpp"

std::atomic<bool> g_running(true);
void sig_handler(int) { g_running = false; }

// Plain output -- MSYS2 bash intercepts ANSI before Windows console mode applies.
// Colour can be re-enabled by defining NIMBUS_COLOUR at compile time:
//   g++ ... -DNIMBUS_COLOUR ...
#ifdef NIMBUS_COLOUR
#define RESET  "\033[0m"
#define GREEN  "\033[32m"
#define RED    "\033[31m"
#define CYAN   "\033[36m"
#define YELLOW "\033[33m"
#define BLUE   "\033[34m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"
#else
#define RESET  ""
#define GREEN  ""
#define RED    ""
#define CYAN   ""
#define YELLOW ""
#define BLUE   ""
#define BOLD   ""
#define DIM    ""
#endif

static long long now_ms() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

// -- Section header ------------------------------------------------------------
void section(const std::string& title) {
    std::cout << "\n" << BOLD << CYAN
              << "==================================================\n"
              << "  " << title << "\n"
              << "==================================================\n"
              << RESET;
}

// -- Helper: build a standard FilterConfig for live/replay modes ---------------
//
// Key setting: min_open_interest = 0.0
//
// The Binance /eapi/v1/openInterest endpoint returns HTTP 400 {"code":-6010}
// for every expiry, so all tickers arrive with open_interest == 0.0. Setting
// the floor to 0.0 disables that gate entirely. All other thresholds are set
// to reasonable values for a clean BTC option surface.
//
static LiveSurfaceEngine::FilterConfig make_live_filter() {
    LiveSurfaceEngine::FilterConfig f;
    f.min_open_interest     = 0.0;   // OI unavailable from Binance EAPI
    f.min_moneyness         = 0.70;  // K/S lower bound
    f.max_moneyness         = 1.40;  // K/S upper bound
    f.min_expiry_days       = 1.0;
    f.max_expiry_days       = 180.0;
    f.min_iv                = 0.05;  // 5% IV floor
    f.max_iv                = 5.0;   // 500% IV cap
    f.min_strikes_per_slice = 5;     // minimum for a well-determined SVI fit
    return f;
}

// -- Demo 1: Black-Scholes pricing & Greeks ------------------------------------
void demo_black_scholes() {
    section("1. Black-Scholes Pricing & Full Greeks");

    double S = 67000.0;
    double r = 0.05;
    double q = 0.0;
    double T = 30.0 / 365.0;
    double sigma = 0.65;

    std::vector<std::pair<double,OptionType>> contracts = {
        {60000, OptionType::PUT},
        {65000, OptionType::PUT},
        {67000, OptionType::CALL},  // ATM
        {70000, OptionType::CALL},
        {75000, OptionType::CALL},
    };

    std::cout << std::fixed
              << "\n  Underlying: BTCUSDT  S=" << S << "  sigma="
              << sigma * 100 << "%  T=30d  r=" << r * 100 << "%\n\n"
              << "  " << std::left
              << std::setw(7)  << "Type"
              << std::setw(10) << "Strike"
              << std::setw(10) << "Price"
              << std::setw(9)  << "Delta"
              << std::setw(11) << "Gamma"
              << std::setw(9)  << "Vega"
              << std::setw(10) << "Theta/d"
              << std::setw(8)  << "Vanna"
              << "Volga\n"
              << "  " << std::string(80, '-') << "\n";

    for (auto& [K, type] : contracts) {
        OptionContract c{};
        c.underlying = "BTCUSDT";
        c.type       = type;
        c.strike     = K;
        c.expiry     = T;
        c.spot       = S;
        c.rate       = r;
        c.div_yield  = q;

        Greeks g = BlackScholes::greeks(c, sigma);

        std::string color = type == OptionType::CALL ? GREEN : RED;
        std::cout << std::setprecision(2)
                  << "  " << color << std::setw(7) << to_string(type) << RESET
                  << std::setprecision(0) << std::setw(10) << K
                  << std::setprecision(2) << std::setw(10) << g.price
                  << std::setprecision(4)
                  << std::setw(9)  << g.delta
                  << std::setprecision(6)
                  << std::setw(11) << g.gamma
                  << std::setprecision(4)
                  << std::setw(9)  << g.vega
                  << std::setprecision(2)
                  << std::setw(10) << g.theta
                  << std::setprecision(4)
                  << std::setw(8)  << g.vanna
                  << std::setw(8)  << g.volga << "\n";
    }

    // IV round-trip check
    section("  IV Round-Trip Check (price -> IV -> price)");
    OptionContract atm{};
    atm.underlying = "BTCUSDT";
    atm.type       = OptionType::CALL;
    atm.strike     = S;
    atm.expiry     = T;
    atm.spot       = S;
    atm.rate       = r;
    atm.div_yield  = q;

    for (double iv_in : {0.40, 0.55, 0.65, 0.80, 1.00}) {
        double px     = BlackScholes::price(atm, iv_in);
        double iv_out = BlackScholes::implied_vol(atm, px);
        double error  = std::abs(iv_out - iv_in) * 10000.0;
        std::string ok = error < 0.01
            ? (GREEN + std::string("OK")   + RESET)
            : (RED   + std::string("FAIL") + RESET);
        std::cout << std::fixed << std::setprecision(4)
                  << "  IV_in=" << iv_in * 100 << "%"
                  << "  px=" << std::setprecision(2) << px
                  << "  IV_out=" << std::setprecision(4) << iv_out * 100 << "%"
                  << "  err=" << std::setprecision(4) << error << "bps  " << ok << "\n";
    }
}

// -- Demo 2: SABR calibration --------------------------------------------------
void demo_sabr() {
    section("2. SABR Model Calibration");

    double S = 67000.0;
    double T = 30.0 / 365.0;
    double r = 0.05;
    double F = S * std::exp(r * T);

    std::vector<CalibrationPoint> market = {
        {60000, T, 0.700, 1.0},
        {63000, T, 0.672, 1.0},
        {65000, T, 0.658, 1.0},
        {66000, T, 0.652, 1.0},
        {67000, T, 0.650, 1.0},  // ATM
        {68000, T, 0.651, 1.0},
        {69000, T, 0.654, 1.0},
        {71000, T, 0.663, 1.0},
        {74000, T, 0.678, 1.0},
    };

    SABRModel sabr(0.5);
    double calib_err;
    auto params = sabr.calibrate(F, T, market, calib_err);

    std::cout << "\n  Forward F = " << std::fixed << std::setprecision(2) << F << "\n"
              << "  Calibrated SABR params:\n"
              << std::setprecision(4)
              << "    alpha = " << params.alpha << "  (vol*F^(1-beta))\n"
              << "    beta  = " << params.beta  << "  (backbone, fixed)\n"
              << "    rho   = " << params.rho   << "  (spot-vol corr)\n"
              << "    nu    = " << params.nu    << "  (vol of vol)\n"
              << "    ATM IV check = "
              << sabr.implied_vol(F, F, T, params) * 100.0 << "%  (target: 65.0%)\n"
              << "    fit RMSE = " << calib_err << " bps\n\n";

    std::cout << "  " << std::left
              << std::setw(10) << "Strike"
              << std::setw(12) << "Market IV"
              << std::setw(12) << "SABR IV"
              << "Error(bps)\n"
              << "  " << std::string(44, '-') << "\n";

    for (const auto& pt : market) {
        double model_iv = sabr.implied_vol(F, pt.strike, T, params);
        double err_bps  = (model_iv - pt.market_iv) * 10000.0;
        auto fmtpct = [](double v, int dp) {
            std::ostringstream os;
            os << std::fixed << std::setprecision(dp) << v << "%";
            return os.str();
        };
        std::cout << std::setprecision(0)
                  << "  " << std::setw(10) << pt.strike
                  << std::setw(12) << fmtpct(pt.market_iv * 100, 2)
                  << std::setw(12) << fmtpct(model_iv * 100, 2)
                  << std::setprecision(1) << err_bps << "\n";
    }
}

// -- Demo 3: SVI Surface -------------------------------------------------------
void demo_svi_surface() {
    section("3. SVI Vol Surface Construction");

    double S = 67000.0;

    std::vector<SurfacePoint> mkt;
    std::vector<double> expiries = {7.0/365, 30.0/365, 90.0/365};
    std::vector<double> strikes  = {50000, 55000, 60000, 63000, 65000,
                                    67000, 69000, 72000, 75000, 80000};
    std::vector<double> atm_vols = {0.80, 0.65, 0.58};
    std::vector<double> skews    = {0.20, 0.14, 0.10};

    for (size_t e = 0; e < expiries.size(); e++) {
        double T   = expiries[e];
        double atm = atm_vols[e];
        double skw = skews[e];
        double F   = S * std::exp(0.05 * T);
        for (double K : strikes) {
            double lm = std::log(K / F);
            double iv = std::max(atm + skw * (-lm) + 0.3 * lm * lm, 0.10);
            mkt.push_back({K, T, iv, iv * 0.99, iv * 1.01});
        }
    }

    SVISurface surface;
    surface.build(S, 0.05, 0.0, mkt);
    surface.print_summary();

    std::cout << "  Surface queries (interpolated IVs):\n\n"
              << "  " << std::left
              << std::setw(10) << "Strike"
              << std::setw(12) << "7d IV"
              << std::setw(12) << "30d IV"
              << std::setw(12) << "60d IV"
              << "90d IV\n"
              << "  " << std::string(46, '-') << "\n";

    for (double K : {55000.0, 60000.0, 65000.0, 67000.0, 70000.0, 75000.0}) {
        std::cout << std::fixed << std::setprecision(1)
                  << "  " << std::setw(10) << K;
        for (double T : {7.0/365, 30.0/365, 60.0/365, 90.0/365}) {
            double iv = surface.iv(K, T);
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << iv * 100 << "%";
            std::cout << std::setw(12) << oss.str();
        }
        std::cout << "\n";
    }
}

// -- Demo 4: Options Market Maker ----------------------------------------------
void demo_market_maker() {
    section("4. Options Market Maker Simulation");

    double S = 67000.0;

    std::vector<SurfacePoint> mkt;
    std::vector<double> expiries = {7.0/365, 30.0/365, 90.0/365};
    std::vector<double> strikes  = {55000, 60000, 63000, 65000, 67000,
                                    69000, 72000, 75000, 80000};

    for (size_t e = 0; e < expiries.size(); e++) {
        double T   = expiries[e];
        double atm = (e == 0 ? 0.80 : e == 1 ? 0.65 : 0.58);
        double skw = (e == 0 ? 0.20 : e == 1 ? 0.14 : 0.10);
        double F   = S * std::exp(0.05 * T);
        for (double K : strikes) {
            double lm = std::log(K / F);
            double iv = std::max(atm + skw * (-lm) + 0.3 * lm * lm, 0.10);
            mkt.push_back({K, T, iv, iv * 0.99, iv * 1.01});
        }
    }

    SVISurface surface;
    surface.build(S, 0.05, 0.0, mkt);

    MMConfig cfg;
    cfg.base_spread_vol = 0.005;
    cfg.inventory_skew  = 0.002;
    cfg.taker_fee_bps   = 0.5;

    OptionsMarketMaker mm(cfg);
    mm.update_surface(surface);

    std::cout << "\n  Two-sided quotes (S=" << S << "):\n\n"
              << "  " << std::left
              << std::setw(6)  << "Type"
              << std::setw(8)  << "Strike"
              << std::setw(8)  << "Days"
              << std::setw(12) << "Bid IV"
              << std::setw(12) << "Ask IV"
              << std::setw(10) << "Bid $"
              << std::setw(10) << "Ask $"
              << std::setw(10) << "Sprd bps"
              << "Delta\n"
              << "  " << std::string(80, '-') << "\n";

    std::vector<std::tuple<double,double,OptionType>> to_quote = {
        {60000, 30.0/365, OptionType::PUT},
        {65000, 30.0/365, OptionType::PUT},
        {67000, 30.0/365, OptionType::CALL},
        {67000, 30.0/365, OptionType::PUT},
        {70000, 30.0/365, OptionType::CALL},
        {75000, 30.0/365, OptionType::CALL},
        {67000,  7.0/365, OptionType::CALL},
        {67000, 90.0/365, OptionType::CALL},
    };

    for (auto& [K, T, type] : to_quote) {
        OptionContract c{};
        c.underlying = "BTCUSDT";
        c.type       = type;
        c.strike     = K;
        c.expiry     = T;
        c.spot       = S;
        c.rate       = 0.05;
        c.div_yield  = 0.0;

        MMQuote q = mm.quote(c, S);

        std::string color = type == OptionType::CALL ? GREEN : RED;
        std::cout << std::fixed << std::setprecision(1)
                  << "  " << color << std::setw(6) << to_string(type) << RESET
                  << std::setw(8)  << K
                  << std::setw(8)  << (int)(T * 365)
                  << std::setw(12) << [&]{ std::ostringstream o; o << std::fixed << std::setprecision(2) << q.bid_iv*100 << "%"; return o.str(); }()
                  << std::setw(12) << [&]{ std::ostringstream o; o << std::fixed << std::setprecision(2) << q.ask_iv*100 << "%"; return o.str(); }()
                  << std::setprecision(0)
                  << std::setw(10) << q.bid_price
                  << std::setw(10) << q.ask_price
                  << std::setprecision(1)
                  << std::setw(10) << q.spread_bps
                  << std::setprecision(3) << q.greeks.delta << "\n";
    }

    section("  Simulating fills + delta hedges");
    long long ts = now_ms();

    OptionContract atm_call{};
    atm_call.underlying = "BTCUSDT";
    atm_call.type       = OptionType::CALL;
    atm_call.strike     = 67000;
    atm_call.expiry     = 30.0 / 365.0;
    atm_call.spot       = S;
    atm_call.rate       = 0.05;
    atm_call.div_yield  = 0.0;

    OptionContract otm_put = atm_call;
    otm_put.type   = OptionType::PUT;
    otm_put.strike = 60000;

    MMQuote q1 = mm.quote(atm_call, S);
    MMQuote q2 = mm.quote(otm_put,  S);

    std::cout << "\n";
    mm.fill(atm_call, q1, "BID", 2.0, ts);
    mm.fill(otm_put,  q2, "ASK", 3.0, ts + 1000);
    mm.hedge_delta(S, S - 50.0, S + 50.0, ts + 2000);
    mm.mark_to_market(S * 1.01);

    mm.print_greeks();
    mm.print_inventory();
    mm.print_pnl();
}

// -- Demo 5: Live real-time surface -------------------------------------------
void demo_live(const std::string& underlying, int duration_seconds) {
    section("5. Live Surface -- Real-time Binance Options Chain");

    std::cout << "  Underlying : " << underlying << "\n"
              << "  Duration   : " << duration_seconds << "s\n"
              << "  Interval   : 30s (Binance EAPI rate limit safe)\n"
              << "  Ctrl+C to stop early\n\n";

    MMConfig cfg;
    cfg.base_spread_vol = 0.005;
    OptionsMarketMaker mm(cfg);

    LiveSurfaceEngine engine;
    engine.set_filter(make_live_filter());
    engine.attach_market_maker(&mm);

    int update_count = 0;

    engine.on_update([&](const SurfaceUpdate& upd) {
        update_count++;
        std::cout << "\n" << BOLD << YELLOW
                  << "[UPDATE #" << update_count << "]  "
                  << "spot=$" << std::fixed << std::setprecision(2) << upd.spot
                  << "  ATM_IV=" << std::setprecision(1) << upd.atm_iv * 100 << "%"
                  << "  skew=" << upd.skew_25d * 100 << "%"
                  << "  slices=" << upd.n_slices
                  << "  contracts=" << upd.n_contracts
                  << RESET << "\n";

        engine.print_live_surface();

        // Quote ATM call against live surface
        if (upd.atm_iv > 0 && upd.spot > 0 && !upd.surface.slices().empty()) {
            double T_front = upd.surface.slices().front().expiry;
            OptionContract atm{};
            atm.underlying = underlying + "USDT";
            atm.type       = OptionType::CALL;
            atm.strike     = std::round(upd.spot / 1000.0) * 1000.0;
            atm.expiry     = T_front;
            atm.spot       = upd.spot;
            atm.rate       = 0.05;
            atm.div_yield  = 0.0;

            MMQuote q = mm.quote(atm, upd.spot);
            std::cout << "  ATM CALL K=" << atm.strike
                      << "  bid=" << std::setprecision(0) << q.bid_price
                      << "  ask=" << q.ask_price
                      << "  bid_iv=" << std::setprecision(2) << q.bid_iv * 100 << "%"
                      << "  ask_iv=" << q.ask_iv * 100 << "%"
                      << "  delta=" << std::setprecision(3) << q.greeks.delta << "\n";
        }
    });

    BinanceOptionsClient client(underlying);
    client.start_live([&](const OptionsChain& chain) {
        engine.process(chain);
    }, 30000);

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(duration_seconds);
    while (g_running && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

    client.stop();

    auto h = engine.health();
    std::cout << "\n-- Live Session Summary --\n"
              << "  Updates   : " << h.n_updates  << "\n"
              << "  Failures  : " << h.n_failures << "\n"
              << "  Avg fit   : " << std::setprecision(1)
              << h.avg_fit_error_bps << " bps\n\n";
}

// -- Demo 6: Record + replay historical session --------------------------------
void demo_record_and_replay(const std::string& underlying,
                             const std::string& csv_path,
                             int record_seconds) {
    section("6. Record + Historical Replay");

    // -- Phase 1: Record -------------------------------------------------------
    std::cout << "  Phase 1: Recording " << record_seconds
              << "s of live data -> " << csv_path << "\n\n";

    BinanceOptionsClient client(underlying);
    int snapshots_recorded = 0;

    client.start_live([&](const OptionsChain& chain) {
        client.record_snapshot(chain, csv_path);
        snapshots_recorded++;
        std::cout << "  [REC #" << snapshots_recorded << "]  "
                  << "contracts=" << chain.tickers.size()
                  << "  spot=$" << std::fixed << std::setprecision(2)
                  << chain.spot << "\n";
    }, 30000);

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(record_seconds);
    while (g_running && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    client.stop();
    std::cout << "\n  Recorded " << snapshots_recorded
              << " snapshots to " << csv_path << "\n\n";

    if (snapshots_recorded == 0) {
        std::cout << "  No snapshots recorded -- skipping replay\n";
        return;
    }

    // -- Phase 2: Replay -------------------------------------------------------
    section("  Phase 2: Historical Replay (10x speed)");

    auto snapshots = client.load_snapshots(csv_path);
    if (snapshots.empty()) {
        std::cout << "  No snapshots to replay\n";
        return;
    }

    MMConfig cfg;
    OptionsMarketMaker mm(cfg);
    LiveSurfaceEngine engine;
    engine.set_filter(make_live_filter());
    engine.attach_market_maker(&mm);

    int replay_count = 0;
    engine.on_update([&](const SurfaceUpdate& upd) {
        replay_count++;
        std::cout << "[REPLAY #" << replay_count << "]"
                  << "  spot=$" << std::fixed << std::setprecision(2) << upd.spot
                  << "  ATM=" << std::setprecision(1) << upd.atm_iv * 100 << "%"
                  << "  skew=" << upd.skew_25d * 100 << "%"
                  << "  status=" << upd.status << "\n";
    });

    client.replay(snapshots, [&](const OptionsChain& chain) {
        engine.process(chain);
    }, 10.0);

    std::cout << "\n  Replayed " << replay_count << " snapshots\n";
    engine.print_live_surface();
}

// -- Main ----------------------------------------------------------------------
// Usage:
//   nimbus.exe               -- static demos, no network
//   nimbus.exe live          -- live Binance surface for 5 min (Ctrl+C to stop)
//   nimbus.exe record 120    -- record 2 min of live data, then replay at 10x
//   nimbus.exe replay <csv>  -- replay a previously saved CSV
int main(int argc, char* argv[]) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    std::signal(SIGINT, sig_handler);

    std::cout << BOLD << CYAN
              << "\n  Nimbus  --  Options Pricing & Vol Surface Engine\n"
              << "  C++20  |  Black-Scholes . SABR . SVI . Market Maker . Live Feed\n"
              << RESET << "\n";

    std::string mode = (argc > 1) ? argv[1] : "demo";

    if (mode == "live") {
        demo_live("BTC", 300);

    } else if (mode == "record") {
        int duration = (argc > 2) ? std::stoi(argv[2]) : 120;
        demo_record_and_replay("BTC", "btc_options_surface.csv", duration);

    } else if (mode == "replay") {
        std::string csv = (argc > 2) ? argv[2] : "btc_options_surface.csv";
        BinanceOptionsClient client("BTC");
        auto snaps = client.load_snapshots(csv);
        if (snaps.empty()) {
            std::cerr << "  No data in " << csv << " -- run 'record' first\n";
            return 1;
        }
        LiveSurfaceEngine engine;
        engine.set_filter(make_live_filter());

        int n = 0;
        engine.on_update([&](const SurfaceUpdate& upd) {
            n++;
            std::cout << "[" << n << "] spot=$"
                      << std::fixed << std::setprecision(2) << upd.spot
                      << "  ATM=" << std::setprecision(1) << upd.atm_iv * 100 << "%"
                      << "  skew=" << upd.skew_25d * 100 << "%\n";
        });
        client.replay(snaps, [&](const OptionsChain& c){ engine.process(c); }, 0.0);
        engine.print_live_surface();

    } else {
        demo_black_scholes();
        demo_sabr();
        demo_svi_surface();
        demo_market_maker();

        std::cout << BOLD << CYAN
                  << "\n  -- Run with arguments for live modes --\n"
                  << "  nimbus.exe live           -- live Binance surface (Ctrl+C to stop)\n"
                  << "  nimbus.exe record 120     -- record 2min, then replay\n"
                  << "  nimbus.exe replay out.csv -- replay saved CSV\n"
                  << RESET << "\n";
    }

    std::cout << BOLD << GREEN << "\n  Done.\n" << RESET << "\n";
    return 0;
}
