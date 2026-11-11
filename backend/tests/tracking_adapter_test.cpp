#include "elevsafe/tracking_adapter.h"
#include "elevsafe/signal_adapter.h"
#include "elevsafe/websocket_message.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    void expect(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
}
int main(int argc, char** argv)
{
    try
    {
        using namespace std::chrono_literals;
        using Json = nlohmann::json;
        using Demo = elevsafe::TrackingDemo;
        const Demo::Clock::time_point zero{};
        const std::chrono::system_clock::time_point utc{};
        elevsafe::sp::PipelineResult b;
        b.frame_id = 19470; b.timestamp_us = 30052013;
        elevsafe::sp::Cluster c;
        c.cluster_id = 37; c.cx = .25f; c.cy = -.5f; c.cz = 1.25f;
        c.x_min = 0; c.x_max = .5f; c.y_min = -1; c.y_max = 0;
        c.z_min = .25f; c.z_max = 2.25f; c.num_points = 40; c.mean_doppler = .5f;
        auto small = c; small.num_points = 39;
        b.clusters = {small, c};
        const auto fd = elevsafe::toTrackingDetections(b);
        expect(fd.frame == b.frame_id && fd.timestamp_us == b.timestamp_us, "frame/time changed");
        expect(fd.detections.size() == 1, "39/40 filter boundary");
        const auto& d = fd.detections.front();
        expect(d.cluster == 37 && d.cx == .25 && d.cy == -.5 && d.cz == 1.25,
            "centroid or cluster mapping");
        expect(d.sx == .5 && d.sy == 1 && d.sz == 2 && d.zmin == .25 && d.zmax == 2.25,
            "extent mapping");
        expect(d.n_points == 40 && d.mean_doppler == .5 && d.mean_power == 0, "metadata mapping");
        b.clusters = {c};
        Demo demo;
        auto first = demo.process(b, zero, utc);
        expect(first && Json::parse(*first)["passengers"].empty(), "tentative exposed");
        b.timestamp_us += 50000; ++b.frame_id;
        expect(!demo.process(b, zero + 50ms, utc), "50ms should not publish");
        b.timestamp_us += 50000; ++b.frame_id;
        auto confirmed = demo.process(b, zero + 100ms, utc + 1s);
        expect(confirmed && elevsafe::isSnapshot(*confirmed), "confirmed snapshot missing");
        auto json = Json::parse(*confirmed);
        expect(json["timestamp"] == "1970-01-01T00:00:01Z", "wall UTC not used");
        expect(json["timestamp_us"] == b.timestamp_us && json["door_state"] == "UNKNOWN"
            && json["door_command"].is_null(), "snapshot contract");
        expect(json["passengers"].size() == 1 && json["passengers"][0]["id"] == 1
            && json["passengers"][0]["state"] == "OUTSIDE", "tracking ID/classification");
        b.clusters[0].cluster_id = 99; b.timestamp_us += 100000;
        json = Json::parse(*demo.process(b, zero + 200ms, utc + 2s));
        expect(json["passengers"][0]["id"] == 1, "B cluster ID became tracking ID");
        expect(demo.processedFrames() == 4, "C skipped rate-limited input");
        b.clusters.clear(); b.timestamp_us += 100000;
        auto empty = demo.process(b, zero + 300ms, utc + 3s);
        expect(empty && Json::parse(*empty)["passengers"].empty(), "empty input did not remove LOST");
        b.clusters = {c}; b.timestamp_us = 0;
        Demo newSession;
        expect(Json::parse(*newSession.process(b, zero, utc))["passengers"].empty(), "session history leaked");
        TrackOutput inside; inside.state = TrackState::CONFIRMED; inside.id = 8; inside.y = 0;
        TrackOutput lost = inside; lost.state = TrackState::LOST;
        TrackOutput tentative = inside; tentative.state = TrackState::TENTATIVE;
        for (auto state : {elevsafe::sp::DoorState::OPEN, elevsafe::sp::DoorState::CLOSED,
                elevsafe::sp::DoorState::UNKNOWN})
        {
            b.estimated_door_state = state;
            json = Json::parse(elevsafe::trackingSnapshot(b, {inside, lost, tentative}, utc));
            expect(json["passengers"].size() == 1 && json["passengers"][0]["state"] == "INSIDE"
                && json["passengers"][0]["safety_state"].is_null()
                && json["door_state"] == elevsafe::sp::doorStateToString(state), "state/filter contract");
        }
        expect(argc == 2, "signal fixture config required");
        elevsafe::sp::Config config; expect(config.load(argv[1]), "fixture config load");
        elevsafe::sp::SignalPipeline signal(config); expect(signal.ok(), "B init");
        Demo realB;
        for (int i = 0; i < 3; ++i)
        {
            elevsafe::RadarFrame frame; frame.frameCount = i; frame.timestampUs = i * 100000;
            for (int j = 0; j < 40; ++j)
            {
                elevsafe::RadarPoint point; point.x = .25f; point.y = .8f; point.z = 1;
                point.power = 10; point.targetId = 255; frame.points.push_back(point);
            }
            auto result = signal.process(elevsafe::toSignalFrame(frame), {});
            expect(result.clusters.size() == 1 && result.clusters[0].num_points == 40, "B fixture cluster");
            auto snapshot = realB.process(result, zero + i * 100ms, utc);
            if (i == 2) expect(Json::parse(*snapshot)["passengers"].size() == 1, "A-B-C path failed");
        }
        std::cout << "tracking_adapter_test PASS: mapping, 39/40, all-frame C, IDs, empty, UTC, v7, session reset, real A-B-C\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
