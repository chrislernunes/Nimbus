#include "binance_options.hpp"
#include "black_scholes.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>
#include <stdexcept>
#include <windows.h>

using json = nlohmann::json;

// -- Timestamps ----------------------------------------------------------------
static long long now_ms_opt() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

// -- libcurl write callback ----------------------------------------------------
static size_t curl_write(void* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

BinanceOptionsClient::BinanceOptionsClient(const std::string& underlying)
    : underlying_(underlying) {}

// -- HTTP GET ------------------------------------------------------------------
std::string BinanceOptionsClient::http_get(const std::string& url) const {
    CURL* curl = curl_easy_init();
    std::string buf;
    if (!curl) return buf;

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    // Binance requires a User-Agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Nimbus/1.0 (github.com/chrislernunes/Nimbus)");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "[HTTP] curl error: " << curl_easy_strerror(res) << "\n";
        buf.clear();
    } else {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (code != 200) {
            std::cerr << "[HTTP] " << url << " -> " << code << "  body: " << buf << "\n";
            buf.clear();
        }
    }
    curl_easy_cleanup(curl);
    return buf;
}

// -- Parse symbol "BTC-250530-65000-C" ----------------------------------------
bool BinanceOptionsClient::parse_symbol(const std::string& sym,
                                         std::string& und,
                                         std::string& exp_str,
                                         double& strike,
                                         OptionType& type) const {
    // Format: UNDERLYING-YYMMDD-STRIKE-C/P
    std::vector<std::string> parts;
    std::string token;
    for (char c : sym) {
        if (c == '-') { parts.push_back(token); token.clear(); }
        else token += c;
    }
    parts.push_back(token);
    if (parts.size() < 4) return false;

    und     = parts[0];
    exp_str = parts[1];
    try { strike = std::stod(parts[2]); }
    catch (...) { return false; }
    type = (parts[3] == "C") ? OptionType::CALL : OptionType::PUT;
    return true;
}

// -- "YYMMDD" -> fractional years from now -------------------------------------
double BinanceOptionsClient::expiry_to_years(const std::string& exp_str,
                                              long long now) const {
    // Parse YYMMDD
    if (exp_str.size() != 6) return 0.0;
    int yy = std::stoi(exp_str.substr(0, 2));
    int mm = std::stoi(exp_str.substr(2, 2));
    int dd = std::stoi(exp_str.substr(4, 2));

    std::tm tm_exp{};
    tm_exp.tm_year = 100 + yy;  // years since 1900
    tm_exp.tm_mon  = mm - 1;
    tm_exp.tm_mday = dd;
    tm_exp.tm_hour = 8;  // Binance options expire 08:00 UTC

    // Convert to UTC timestamp
    // mktime uses local time -- adjust for UTC
    time_t t_exp = _mkgmtime(&tm_exp);
    long long exp_ms = static_cast<long long>(t_exp) * 1000LL;
    double diff_ms = static_cast<double>(exp_ms - now);
    return std::max(diff_ms / (1000.0 * 3600.0 * 24.0 * 365.0), 1.0/365.0);
}

// -- Fetch spot index ----------------------------------------------------------
double BinanceOptionsClient::fetch_spot() const {
    std::string url = std::string(BASE_URL) + "/eapi/v1/index?underlying=" + underlying_ + "USDT";
    std::string resp = http_get(url);
    if (resp.empty()) {
        // Fallback: use Binance spot API
        url = "https://api.binance.com/api/v3/ticker/price?symbol=" + underlying_ + "USDT";
        resp = http_get(url);
        if (resp.empty()) return 0.0;
        auto j = json::parse(resp, nullptr, false);
        if (j.is_discarded()) return 0.0;
        return std::stod(j.value("price", "0"));
    }
    auto j = json::parse(resp, nullptr, false);
    if (j.is_discarded()) return 0.0;
    return std::stod(j.value("indexPrice", "0"));
}

// -- Fetch available expiries --------------------------------------------------
std::vector<std::string> BinanceOptionsClient::fetch_expiry_dates() const {
    std::string url = std::string(BASE_URL) + "/eapi/v1/exchangeInfo";
    std::string resp = http_get(url);
    if (resp.empty()) return {};

    auto j = json::parse(resp, nullptr, false);
    if (j.is_discarded()) return {};

    std::vector<std::string> dates;
    if (!j.contains("optionSymbols")) return {};

    for (const auto& sym : j["optionSymbols"]) {
        std::string name = sym.value("symbol", "");
        std::string und, exp_str;
        double strike;
        OptionType type;
        if (parse_symbol(name, und, exp_str, strike, type)) {
            if (und == underlying_ &&
                std::find(dates.begin(), dates.end(), exp_str) == dates.end())
                dates.push_back(exp_str);
        }
    }
    std::sort(dates.begin(), dates.end());
    return dates;
}

// -- Fetch mark prices for one expiry -----------------------------------------
std::vector<BinanceOptionTicker> BinanceOptionsClient::fetch_expiry(
    const std::string& expiry_date) const
{
    long long ts = now_ms_opt();

    // Mark prices
    std::string url = std::string(BASE_URL) + "/eapi/v1/mark?underlying="
                    + underlying_ + "USDT&expiration=" + expiry_date;
    std::string resp = http_get(url);
    if (resp.empty()) return {};

    auto j = json::parse(resp, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return {};

    std::vector<BinanceOptionTicker> tickers;
    for (const auto& item : j) {
        std::string sym = item.value("symbol", "");
        std::string und, exp_str;
        double strike;
        OptionType type;
        if (!parse_symbol(sym, und, exp_str, strike, type)) continue;

        BinanceOptionTicker t;
        t.symbol       = sym;
        t.underlying   = und + "USDT";
        t.type         = type;
        t.strike       = strike;
        t.expiry_years = expiry_to_years(exp_str, ts);
        t.mark_price   = std::stod(item.value("markPrice",    "0"));
        t.mark_iv      = std::stod(item.value("markIV",       "0"));
        t.delta        = std::stod(item.value("delta",        "0"));
        t.gamma        = std::stod(item.value("gamma",        "0"));
        t.vega         = std::stod(item.value("vega",         "0"));
        t.theta        = std::stod(item.value("theta",        "0"));
        t.timestamp_ms = ts;

        // Bid/ask from ticker endpoint
        t.bid = 0.0;
        t.ask = 0.0;

        tickers.push_back(t);
    }
    return tickers;
}

// -- Fetch full chain ----------------------------------------------------------
OptionsChain BinanceOptionsClient::fetch_chain() const {
    long long ts = now_ms_opt();
    OptionsChain chain;
    chain.underlying   = underlying_;
    chain.timestamp_ms = ts;

    // 1. Spot
    chain.spot = fetch_spot();
    if (chain.spot <= 0.0) {
        std::cerr << "[Chain] Failed to fetch spot for " << underlying_ << "\n";
        return chain;
    }

    // 2. Get all mark prices in one shot via /eapi/v1/mark (no expiry filter)
    std::string url = std::string(BASE_URL) + "/eapi/v1/mark?underlying="
                    + underlying_ + "USDT";
    std::string resp = http_get(url);

    if (resp.empty()) {
        std::cerr << "[Chain] Empty response from mark endpoint\n";
        return chain;
    }

    auto j = json::parse(resp, nullptr, false);
    if (j.is_discarded() || !j.is_array()) {
        std::cerr << "[Chain] JSON parse failed\n";
        return chain;
    }

    for (const auto& item : j) {
        std::string sym = item.value("symbol", "");
        std::string und, exp_str;
        double strike;
        OptionType type;
        if (!parse_symbol(sym, und, exp_str, strike, type)) continue;
        if (und != underlying_) continue;

        double exp_years = expiry_to_years(exp_str, ts);
        if (exp_years <= 0.0) continue;  // already expired

        BinanceOptionTicker t;
        t.symbol       = sym;
        t.underlying   = und + "USDT";
        t.type         = type;
        t.strike       = strike;
        t.expiry_years = exp_years;
        t.expiry_ts_ms = ts + static_cast<long long>(exp_years * 365.0 * 86400000.0);
        t.mark_price   = std::stod(item.value("markPrice",    "0"));
        t.mark_iv      = std::stod(item.value("markIV",       "0"));
        t.delta        = std::stod(item.value("delta",        "0"));
        t.gamma        = std::stod(item.value("gamma",        "0"));
        t.vega         = std::stod(item.value("vega",         "0"));
        t.theta        = std::stod(item.value("theta",        "0"));
        t.bid          = 0.0;
        t.ask          = 0.0;
        t.open_interest = std::stod(item.value("openInterest", "0"));
        t.volume_24h   = std::stod(item.value("amount",        "0"));
        t.timestamp_ms = ts;

        chain.tickers.push_back(t);
    }

    // 3. Enrich with bid/ask from ticker endpoint
    std::string tick_url = std::string(BASE_URL) + "/eapi/v1/ticker?underlying="
                         + underlying_ + "USDT";
    std::string tick_resp = http_get(tick_url);
    if (!tick_resp.empty()) {
        auto tj = json::parse(tick_resp, nullptr, false);
        if (!tj.is_discarded() && tj.is_array()) {
            std::map<std::string, std::pair<double,double>> bids_asks;
            for (const auto& ti : tj) {
                std::string sym = ti.value("symbol", "");
                double bid = std::stod(ti.value("bidPrice", "0"));
                double ask = std::stod(ti.value("askPrice", "0"));
                bids_asks[sym] = {bid, ask};
            }
            for (auto& t : chain.tickers) {
                auto it = bids_asks.find(t.symbol);
                if (it != bids_asks.end()) {
                    t.bid = it->second.first;
                    t.ask = it->second.second;
                }
            }
        }
    }

    std::cout << "[Chain] Fetched " << chain.tickers.size()
              << " contracts  spot=" << std::fixed << std::setprecision(2) << chain.spot
              << "  ts=" << ts << "\n";
    return chain;
}

// -- Convert chain to SurfacePoints --------------------------------------------
std::vector<SurfacePoint> OptionsChain::to_surface_points(
    double min_oi, double min_bid, double max_spread_iv) const
{
    std::vector<SurfacePoint> pts;
    for (const auto& t : tickers) {
        if (t.open_interest < min_oi)   continue;
        if (t.mark_iv <= 0.0)           continue;
        if (t.mark_price <= 0.0)        continue;

        // Filter by bid/ask spread if available
        if (t.bid > 0 && t.ask > 0) {
            if (t.bid < min_bid) continue;
            // Estimate IV bid/ask spread -- skip very wide markets
            // (crude: use mark_price ± half bid-ask as proxy)
        }

        SurfacePoint p;
        p.strike = t.strike;
        p.expiry = t.expiry_years;
        p.mid_iv = t.mark_iv;
        p.bid_iv = t.mark_iv * 0.99;
        p.ask_iv = t.mark_iv * 1.01;
        pts.push_back(p);
    }
    return pts;
}

// -- Live polling --------------------------------------------------------------
void BinanceOptionsClient::start_live(ChainCallback cb, int interval_ms) {
    running_ = true;
    poll_thread_ = std::thread([this, cb, interval_ms]() {
        while (running_) {
            auto t0 = std::chrono::steady_clock::now();
            try {
                OptionsChain chain = fetch_chain();
                if (!chain.tickers.empty()) cb(chain);
            } catch (const std::exception& e) {
                std::cerr << "[Live] Exception: " << e.what() << "\n";
            }
            auto elapsed = std::chrono::steady_clock::now() - t0;
            auto sleep_ms = std::chrono::milliseconds(interval_ms) - elapsed;
            if (sleep_ms.count() > 0)
                std::this_thread::sleep_for(sleep_ms);
        }
    });
}

void BinanceOptionsClient::stop() {
    running_ = false;
    if (poll_thread_.joinable()) poll_thread_.join();
}

// -- Record snapshot to CSV ----------------------------------------------------
// CSV format:
// timestamp_ms,spot,symbol,type,strike,expiry_years,mark_iv,bid,ask,delta,gamma,vega,theta,oi,vol
void BinanceOptionsClient::record_snapshot(const OptionsChain& chain,
                                            const std::string& filepath) const {
    bool write_header = false;
    {
        std::ifstream f(filepath);
        write_header = !f.good();
    }

    std::ofstream f(filepath, std::ios::app);
    if (!f.is_open()) {
        std::cerr << "[Record] Cannot open " << filepath << "\n";
        return;
    }

    if (write_header)
        f << "timestamp_ms,spot,symbol,type,strike,expiry_years,"
             "mark_iv,bid,ask,delta,gamma,vega,theta,open_interest,volume_24h\n";

    for (const auto& t : chain.tickers) {
        f << chain.timestamp_ms << ","
          << std::fixed << std::setprecision(4) << chain.spot << ","
          << t.symbol << ","
          << (t.type == OptionType::CALL ? "C" : "P") << ","
          << t.strike << ","
          << t.expiry_years << ","
          << t.mark_iv << ","
          << t.bid << ","
          << t.ask << ","
          << t.delta << ","
          << t.gamma << ","
          << t.vega << ","
          << t.theta << ","
          << t.open_interest << ","
          << t.volume_24h << "\n";
    }
}

// -- Load snapshots from CSV ---------------------------------------------------
std::vector<HistoricalSnapshot> BinanceOptionsClient::load_snapshots(
    const std::string& filepath) const
{
    std::ifstream f(filepath);
    if (!f.is_open()) {
        std::cerr << "[Load] Cannot open " << filepath << "\n";
        return {};
    }

    std::map<long long, HistoricalSnapshot> by_ts;
    std::string line;
    std::getline(f, line); // skip header

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(ss, field, ',')) fields.push_back(field);
        if (fields.size() < 15) continue;

        long long ts  = std::stoll(fields[0]);
        double spot   = std::stod(fields[1]);
        // std::string sym = fields[2];
        OptionType type = (fields[3] == "C") ? OptionType::CALL : OptionType::PUT;
        double strike   = std::stod(fields[4]);
        double exp_yrs  = std::stod(fields[5]);
        double mark_iv  = std::stod(fields[6]);
        double bid      = std::stod(fields[7]);
        double ask_val  = std::stod(fields[8]);
        double delta    = std::stod(fields[9]);
        double gamma    = std::stod(fields[10]);
        double vega_v   = std::stod(fields[11]);
        double theta    = std::stod(fields[12]);
        double oi       = std::stod(fields[13]);
        double vol24    = std::stod(fields[14]);

        auto& snap = by_ts[ts];
        snap.timestamp_ms = ts;
        snap.spot = spot;
        snap.chain.spot = spot;
        snap.chain.timestamp_ms = ts;

        BinanceOptionTicker t;
        t.symbol       = fields[2];
        t.type         = type;
        t.strike       = strike;
        t.expiry_years = exp_yrs;
        t.mark_iv      = mark_iv;
        t.bid          = bid;
        t.ask          = ask_val;
        t.delta        = delta;
        t.gamma        = gamma;
        t.vega         = vega_v;
        t.theta        = theta;
        t.open_interest = oi;
        t.volume_24h   = vol24;
        t.timestamp_ms = ts;
        snap.chain.tickers.push_back(t);
    }

    std::vector<HistoricalSnapshot> result;
    result.reserve(by_ts.size());
    for (auto& [ts, snap] : by_ts) {
        snap.surface_pts = snap.chain.to_surface_points();
        result.push_back(std::move(snap));
    }

    std::cout << "[Load] Loaded " << result.size() << " snapshots from " << filepath << "\n";
    return result;
}

// -- Replay historical snapshots -----------------------------------------------
void BinanceOptionsClient::replay(const std::vector<HistoricalSnapshot>& snaps,
                                   ChainCallback cb,
                                   double speed_factor) const {
    if (snaps.empty()) return;

    std::cout << "[Replay] " << snaps.size() << " snapshots"
              << "  speed=" << speed_factor << "x\n";

    for (size_t i = 0; i < snaps.size(); i++) {
        cb(snaps[i].chain);

        if (i + 1 < snaps.size() && speed_factor > 0.0) {
            long long gap_ms = snaps[i+1].timestamp_ms - snaps[i].timestamp_ms;
            long long sleep_ms = static_cast<long long>(
                static_cast<double>(gap_ms) / speed_factor);
            if (sleep_ms > 0 && sleep_ms < 300000) // cap at 5 min
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
    std::cout << "[Replay] Complete\n";
}
