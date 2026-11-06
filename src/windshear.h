#pragma once
// Windshear model for WXPlane (2026-11-06). Independent of X-Plane: the plugin feeds it the stations and radar cells it already has,
// steps it with simulation time, and sends the low-level wind records it asks for. See docs/windshear.md for what is modelled, what is
// applied and what is not (downdrafts: X-Plane's weather records have no vertical-wind field).
#include <cstdint>
#include <string>
#include <vector>

#include "radar.h"
#include "wxmodel.h"

namespace wx {
namespace shear {

enum FieldType { kGustFront = 1, kMicroburst = 2 };
enum Confidence { kInsufficient = 0, kLow = 1, kMedium = 2, kHigh = 3 };

// A place where the observations support windshear formation.
struct Source {
    std::string key;          // stable while the evidence stays in the same 10 nm square: no duplicate fields per square
    double lat = 0, lon = 0;
    float dbz = 0;            // strongest radar reflectivity at the source (0 if none)
    bool tsObserved = false;  // a station within 15 nm reports TS / VCTS / SQ
    float gustKt = 0;         // strongest observed gust there
    bool dry = false;         // dry-microburst environment: convective echo, a deep dry sub-cloud layer and a high cloud base
    int confidence = kInsufficient;
    double pHourGustFront = 0, pHourMicroburst = 0;   // calibration targets (engineering assumptions, not measured rates)
    std::string why;
};

struct Env {
    const std::vector<Station>* stations = nullptr;
    const std::vector<RadarCell>* cells = nullptr;   // the radar's measured cells (synthetic ones are ignored)
    double steerDirDeg = 270, steerKt = 20;          // environmental (steering) wind the storms move with, FROM direction
};

std::vector<Source> assess(const Env& env);

struct Field {
    uint32_t id = 0;
    int type = kGustFront;
    std::string source;       // source key
    std::string cause;
    int confidence = kLow;
    double lat0 = 0, lon0 = 0;    // position at t0
    double t0 = 0;                // simulation time of formation (s)
    double moveToDeg = 90, moveKt = 15;   // advection (toward), from the environmental wind
    float peakKt = 15;            // peak horizontal outflow
    float downFpm = 0;            // microburst downdraft (modelled and logged only, never applied)
    double lifeS = 900;
    double outDirDeg = 90;        // gust front: direction the outflow blows toward
    bool forced = false;          // test event
};

// One low-level wind record X-Plane should hold. Winds from the surface to topAglFt are replaced by (ambient + this vector).
struct ShearRecord {
    std::string key;
    double lat = 0, lon = 0;
    float radiusNm = 1;
    float towardDeg = 0, kt = 0;  // the added outflow vector (direction it blows TOWARD)
    float topAglFt = 2500;
    uint32_t fieldId = 0;
};

struct Vec3 { double east = 0, north = 0, up = 0; };   // kt (horizontal), fpm (up)

class Engine {
public:
    explicit Engine(uint32_t seed = 1) : seed_(seed) {}
    void setSeed(uint32_t s) { seed_ = s; }
    // Every ~30 s of simulation time. Forms new fields from the sources (hazard rate per 30 s bucket, deterministic), retires dead ones.
    // Returns log lines for what happened.
    std::vector<std::string> step(double simT, const std::vector<Source>& sources, const Env& env);
    // A test event, mature at once: type, centre, the direction the aircraft flies (gust fronts are put across it).
    const Field& force(int type, double lat, double lon, double simT, double trackDeg, const Env& env);
    void clear() { fields_.clear(); }
    // Drop fields more than maxNm from the aircraft (teleport, long jump): their records are no longer needed.
    int dropFar(double lat, double lon, double maxNm, double simT);
    const std::vector<Field>& fields() const { return fields_; }
    // Records for all fields at time t (pass simT + lead so X-Plane's fade-in finishes in time). `gain` compensates X-Plane's blending.
    std::vector<ShearRecord> records(double t, double gain) const;
    // The model's own wind perturbation at a point (kt east/north, fpm up), for tests and the encounter log.
    Vec3 perturbation(double lat, double lon, double aglFt, double t) const;
    static double intensity(const Field& f, double t);   // 0..1 lifecycle
    static void position(const Field& f, double t, double& lat, double& lon);
    static double outflowRadiusNm(const Field& f, double t);   // microburst ring radius / gust front distance from its parent

private:
    uint32_t seed_;
    uint32_t nextId_ = 1;
    std::vector<Field> fields_;
    double lastT_ = -1;
};

// Deterministic hash in [0,1) of (seed, text, a, b)
double hash01(uint32_t seed, const std::string& s, long long a, long long b);
// P per interval from an hourly probability: lambda = -ln(1-P)/3600, P = 1 - exp(-lambda dt)
double intervalProbability(double pHour, double dtS);
std::string fieldName(int type);

}  // namespace shear
}  // namespace wx
