#include "worldfetch.h"

#include <chrono>
#include <cstdio>
#include <ctime>

#include "http.h"
#include "inflate.h"
#include "worldmetar.h"

namespace wx {

void WorldMetars::start() {
    stop();
    stop_ = false;
    th_ = std::thread([this]() { run(); });
}

void WorldMetars::stop() {
    stop_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void WorldMetars::want(bool on) {
    std::lock_guard<std::mutex> lk(m_);
    if (on == want_) return;
    want_ = on;
    if (!on) data_.reset();
    else next_ = std::chrono::steady_clock::now();   // wanted again: fetch at once
    cv_.notify_all();
}

std::shared_ptr<const std::vector<Station>> WorldMetars::stations() const {
    std::lock_guard<std::mutex> lk(m_);
    return data_;
}

std::vector<std::string> WorldMetars::takeLog() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> v;
    v.swap(log_);
    return v;
}

void WorldMetars::log(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_);
    if (log_.size() < 50) log_.push_back(s);
}

void WorldMetars::run() {
    using Clock = std::chrono::steady_clock;
    while (!stop_) {
        {
            std::unique_lock<std::mutex> lk(m_);
            while (!stop_ && !(want_ && Clock::now() >= next_)) {
                if (want_) cv_.wait_until(lk, next_); else cv_.wait(lk);
            }
            if (stop_) return;
        }
        std::string body, err;
        long status = 0;
        const auto t0 = Clock::now();
        bool ok = httpGetCancellable(kWorldMetarUrl, "", body, status, err, 30, 16u * 1024u * 1024u, &stop_) && status == 200;
        if (stop_) return;
        const double kb = body.size() / 1024.0;
        std::vector<Station> st;
        if (ok) {
            std::string csv;
            if (body.size() >= 2 && (unsigned char)body[0] == 0x1f && (unsigned char)body[1] == 0x8b) {   // gzip (else already unpacked in transit)
                std::vector<uint8_t> raw;
                if (gunzip(body, raw, 96u * 1024u * 1024u)) csv.assign(raw.begin(), raw.end());
                else { ok = false; err = "could not unpack the file"; }
            } else csv.swap(body);
            if (ok && !parseWorldMetarCsv(csv, std::time(nullptr), st)) { ok = false; err = "no METAR table in the file"; }
        }
        const double s = std::chrono::duration<double>(Clock::now() - t0).count();
        char b[256];
        if (ok) {
            std::snprintf(b, sizeof b, "world METARs (map only, not sent to X-Plane): %zu stations, %.0f kB in %.1f s", st.size(), kb, s);
            log(b);
            std::lock_guard<std::mutex> lk(m_);
            if (want_) data_ = std::make_shared<const std::vector<Station>>(std::move(st));
            next_ = Clock::now() + std::chrono::minutes(10);
        } else {
            std::snprintf(b, sizeof b, "world METARs: download failed (%s) - next try in 2 min",
                          err.empty() ? ("HTTP " + std::to_string(status)).c_str() : err.c_str());
            log(b);
            std::lock_guard<std::mutex> lk(m_);
            next_ = Clock::now() + std::chrono::minutes(2);
        }
    }
}

} // namespace wx
