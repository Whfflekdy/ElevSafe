#include "elevsafe/backend_pipeline.h"
#include "elevsafe/retina_client.h"
#include "elevsafe/signal_adapter.h"
#include "io/csv_io.h"
#include "pipeline/signal_pipeline.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace
{
    namespace sp = elevsafe::sp;
    namespace fs = std::filesystem;
    constexpr const char* inputHeader = "frame,timestamp_us,x,y,z,doppler,power,target_id";
    constexpr const char* resultHeader = "frame_id,num_clusters,door_state";

    void require(bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    std::string trim(const std::string& text)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    std::vector<std::string> cells(const std::string& line)
    {
        std::vector<std::string> result;
        std::size_t begin = 0;
        for (;;)
        {
            const auto end = line.find(',', begin);
            result.push_back(trim(line.substr(begin, end == std::string::npos ? end : end - begin)));
            if (end == std::string::npos) return result;
            begin = end + 1;
        }
    }

    template<class T> T integer(const std::string& text)
    {
        T value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "Invalid integer: " + text);
        return value;
    }

    struct InputMeta { int id; std::int64_t timestamp; std::size_t points; };

    // Audit the raw converter schema before B's reader sorts/merges frame IDs.
    // Actual point deserialization remains readFramesCSV(), not a replacement reader.
    std::vector<InputMeta> auditInput(std::istream& input)
    {
        std::string line;
        require(static_cast<bool>(std::getline(input, line)) && trim(line) == inputHeader,
            "Expected raw PCR converter header with integer timestamp_us");
        std::vector<InputMeta> frames;
        std::map<int, bool> seen;
        std::size_t row = 1;
        while (std::getline(input, line))
        {
            ++row;
            if (trim(line).empty()) continue;
            try
            {
                const auto values = cells(line);
                require(values.size() == 8, "Expected exactly eight columns");
                const int id = integer<int>(values[0]);
                const auto timestamp = integer<std::int64_t>(values[1]);
                require(id >= 0 && timestamp >= 0, "Negative frame ID or timestamp is unsupported");
                const auto target = integer<std::int64_t>(values[7]);
                require(std::in_range<std::int32_t>(target) && std::in_range<int>(target), "Target ID out of range");
                for (std::size_t i = 2; i < 7; ++i)
                {
                    float value{};
                    const auto parsed = std::from_chars(values[i].data(), values[i].data() + values[i].size(), value);
                    require(parsed.ec == std::errc{} && parsed.ptr == values[i].data() + values[i].size()
                        && std::isfinite(value), "Invalid/non-finite point value");
                }
                if (frames.empty() || frames.back().id != id)
                {
                    require(!seen.contains(id), "Repeated frame ID group: " + std::to_string(id));
                    require(frames.empty() || (id > frames.back().id && timestamp >= frames.back().timestamp),
                        "Frame order or timestamp reversed");
                    seen.emplace(id, true);
                    frames.push_back({id, timestamp, 0});
                }
                require(frames.back().timestamp == timestamp, "Inconsistent timestamps within one frame");
                ++frames.back().points;
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error("Input row " + std::to_string(row) + ": " + error.what());
            }
        }
        require(input.eof(), "Input read failed");
        require(!frames.empty(), "Input has no point frames (CSV cannot represent empty frames)");
        return frames;
    }

    elevsafe::RadarFrame toRadarFrame(const sp::FrameData& input)
    {
        require(std::in_range<std::uint32_t>(input.frame_id)
            && std::in_range<std::uint64_t>(input.timestamp_us), "Input metadata cannot fit RadarFrame");
        elevsafe::RadarFrame frame;
        frame.frameCount = static_cast<std::uint32_t>(input.frame_id);
        frame.timestampUs = static_cast<std::uint64_t>(input.timestamp_us);
        frame.points.reserve(input.points.size());
        for (const auto& point : input.points)
        {
            require(std::in_range<std::int32_t>(point.target_id), "Target ID cannot fit RadarPoint");
            require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)
                && std::isfinite(point.doppler) && std::isfinite(point.power), "Non-finite input point");
            require(point.zone == sp::Zone::NONE && point.cluster_id == -1 && !point.has_bins
                && point.range_bin == 0 && point.u_bin == 0 && point.v_bin == 0, "Processed B points are not raw input");
            frame.points.push_back({point.x, point.y, point.z, point.doppler, point.power, point.target_id});
        }
        return frame;
    }

    bool sameFloat(float a, float b)
    {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    void checkInput(const sp::FrameData& expected, const sp::FrameData& actual)
    {
        const auto where = "Input preservation frame=" + std::to_string(expected.frame_id);
        require(expected.frame_id == actual.frame_id && expected.timestamp_us == actual.timestamp_us
            && expected.points.size() == actual.points.size(), where + " metadata/count mismatch");
        for (std::size_t i = 0; i < expected.points.size(); ++i)
        {
            const auto& a = expected.points[i]; const auto& b = actual.points[i];
            require(sameFloat(a.x,b.x) && sameFloat(a.y,b.y) && sameFloat(a.z,b.z)
                && sameFloat(a.doppler,b.doppler) && sameFloat(a.power,b.power) && a.target_id == b.target_id
                && a.zone == b.zone && a.cluster_id == b.cluster_id && a.has_bins == b.has_bins
                && a.range_bin == b.range_bin && a.u_bin == b.u_bin && a.v_bin == b.v_bin,
                where + " point/order mismatch at " + std::to_string(i));
        }
    }

    struct Record
    {
        int id;
        std::optional<std::int64_t> timestamp;
        std::size_t clusters;
        sp::DoorState state;
    };
    Record record(const sp::PipelineResult& r) { return {r.frame_id, r.timestamp_us, r.clusters.size(), r.estimated_door_state}; }

    std::vector<Record> readBaseline(std::istream& input)
    {
        std::string line;
        require(static_cast<bool>(std::getline(input,line)) && trim(line) == resultHeader, "Invalid baseline header");
        std::vector<Record> records;
        while (std::getline(input,line))
        {
            if (trim(line).empty()) continue;
            const auto v = cells(line);
            require(v.size() == 3, "Invalid baseline row");
            auto state = sp::DoorState::UNKNOWN;
            if (v[2] == "OPEN") state = sp::DoorState::OPEN;
            else if (v[2] == "CLOSED") state = sp::DoorState::CLOSED;
            else require(v[2] == "UNKNOWN", "Invalid baseline door state");
            const auto id = integer<int>(v[0]);
            require(id >= 0, "Negative baseline frame ID");
            records.push_back({id, std::nullopt, integer<std::size_t>(v[1]), state});
        }
        require(input.eof() && !records.empty(), "Empty/unreadable baseline");
        return records;
    }

    struct Diff { std::string comparison; int id; std::string field, expected, actual; };
    using Diffs = std::vector<Diff>;
    template<class T> std::string show(T value) { std::ostringstream s; s << std::setprecision(17) << value; return s.str(); }

    void compareRecords(const std::vector<Record>& expected, const std::vector<Record>& actual,
        const std::string& label, Diffs& diffs)
    {
        std::map<int,const Record*> left, right;
        const auto index = [&](const auto& records, auto& map, const char* side)
        {
            for (const auto& r : records)
                if (!map.emplace(r.id, &r).second) diffs.push_back({label,r.id,"duplicate_id",side,"duplicate"});
        };
        index(expected,left,"expected"); index(actual,right,"actual");
        if (expected.size() != actual.size()) diffs.push_back({label,-1,"frame_count",show(expected.size()),show(actual.size())});
        for (std::size_t i = 0; i < std::min(expected.size(),actual.size()); ++i)
            if (expected[i].id != actual[i].id) diffs.push_back({label,expected[i].id,"order["+show(i)+"]",show(expected[i].id),show(actual[i].id)});
        for (const auto& [id,a] : left)
        {
            const auto found = right.find(id);
            if (found == right.end()) { diffs.push_back({label,id,"missing_id","present","missing"}); continue; }
            const auto& b = *found->second;
            if (a->timestamp && a->timestamp != b.timestamp) diffs.push_back({label,id,"timestamp_us",show(*a->timestamp),b.timestamp ? show(*b.timestamp) : "missing"});
            if (a->clusters != b.clusters) diffs.push_back({label,id,"num_clusters",show(a->clusters),show(b.clusters)});
            if (a->state != b.state) diffs.push_back({label,id,"door_state",sp::doorStateToString(a->state),sp::doorStateToString(b.state)});
        }
        for (const auto& [id,b] : right)
            if (!left.contains(id)) diffs.push_back({label,id,"extra_id","missing","present"});
    }

    void compareClusters(const sp::PipelineResult& a, const sp::PipelineResult& b, Diffs& diffs)
    {
        for (std::size_t i = 0; i < std::min(a.clusters.size(),b.clusters.size()); ++i)
        {
            const auto& x = a.clusters[i]; const auto& y = b.clusters[i];
            const auto check = [&](const char* name, auto left, auto right)
            {
                if (left != right) diffs.push_back({"direct-adapter",a.frame_id,"cluster["+show(i)+"]."+name,show(left),show(right)});
            };
            const auto checkFloat = [&](const char* name, float left, float right)
            {
                if (!sameFloat(left,right)) diffs.push_back({"direct-adapter",a.frame_id,"cluster["+show(i)+"]."+name,
                    show(left)+" bits="+show(std::bit_cast<std::uint32_t>(left)),show(right)+" bits="+show(std::bit_cast<std::uint32_t>(right))});
            };
            check("cluster_id",x.cluster_id,y.cluster_id);
            check("num_points",x.num_points,y.num_points);
            check("zone",static_cast<int>(x.zone),static_cast<int>(y.zone));
            check("maybe_merged",x.maybe_merged,y.maybe_merged);
            checkFloat("cx",x.cx,y.cx); checkFloat("cy",x.cy,y.cy); checkFloat("cz",x.cz,y.cz);
            checkFloat("x_min",x.x_min,y.x_min); checkFloat("x_max",x.x_max,y.x_max);
            checkFloat("y_min",x.y_min,y.y_min); checkFloat("y_max",x.y_max,y.y_max);
            checkFloat("z_min",x.z_min,y.z_min); checkFloat("z_max",x.z_max,y.z_max);
            checkFloat("z_top",x.z_top,y.z_top); checkFloat("mean_doppler",x.mean_doppler,y.mean_doppler);
        }
    }

    struct Totals
    {
        std::size_t frames=0, clusters=0, open=0, closed=0, unknown=0, transitions=0;
    };
    Totals totals(const std::vector<Record>& records)
    {
        Totals t; auto previous=sp::DoorState::UNKNOWN;
        for (const auto& r : records)
        {
            ++t.frames; t.clusters+=r.clusters;
            if (r.state==sp::DoorState::OPEN) ++t.open;
            else if (r.state==sp::DoorState::CLOSED) ++t.closed;
            else ++t.unknown;
            if (previous!=sp::DoorState::UNKNOWN && previous!=r.state) ++t.transitions;
            previous=r.state;
        }
        return t;
    }
    void writeTotals(std::ostream& out, const Totals& t)
    {
        out << "{\"frames\":"<<t.frames<<",\"clusters\":"<<t.clusters<<",\"open\":"<<t.open
            <<",\"closed\":"<<t.closed<<",\"unknown\":"<<t.unknown<<",\"transitions\":"<<t.transitions<<'}';
    }
    std::ofstream output(const fs::path& path)
    {
        std::ofstream file(path);
        require(file.is_open(), "Cannot open output: "+path.string());
        file.exceptions(std::ios::badbit | std::ios::failbit);
        return file;
    }
    void writeRecords(const fs::path& path, const std::vector<Record>& records)
    {
        auto file=output(path); file<<resultHeader<<'\n';
        for (const auto& r : records) file<<r.id<<','<<r.clusters<<','<<sp::doorStateToString(r.state)<<'\n';
        file.close();
    }
    std::string quoted(const std::string& text)
    {
        std::string result="\"";
        for (char c : text) { if(c=='"') result+='"'; result+=c; }
        return result+'"';
    }

    std::vector<sp::FrameData> readInput(const std::string& path)
    {
        std::ifstream input(path);
        require(input.is_open(),"Cannot open input CSV");
        const auto metadata=auditInput(input);
        const auto frames=sp::readFramesCSV(path);
        require(frames.size()==metadata.size(),"B CSV reader lost frames");
        for (std::size_t i=0;i<frames.size();++i)
            require(frames[i].frame_id==metadata[i].id && frames[i].timestamp_us==metadata[i].timestamp
                && frames[i].points.size()==metadata[i].points,"B CSV reader changed metadata/order/point count");
        return frames;
    }

    int compare(const std::string& cfgPath, const std::string& inputPath, const std::string& baselinePath,
        const fs::path& directory, bool s03)
    {
        require(!fs::exists(directory) || (fs::is_directory(directory) && fs::is_empty(directory)),
            "Output directory must be new or empty; baseline files will not be overwritten");
        fs::create_directories(directory);
        sp::Config cfg; require(cfg.load(cfgPath),"Config load failed");
        sp::SignalPipeline direct(cfg), adapter(cfg);
        require(direct.ok() && adapter.ok(),"SignalPipeline initialization failed");
        require(direct.usesBackground()==adapter.usesBackground(),"Background activation differs");
        require(!direct.usesBackground(),"This comparison requires background disabled");
        if(s03) require(cfg.getFloat("door_state.close_threshold",1.0f)==0.5f,"S0-3 requires close_threshold=0.5");
        const auto frames=readInput(inputPath);
        std::ifstream baseline(baselinePath); require(baseline.is_open(),"Cannot open baseline");
        const auto reference=readBaseline(baseline);
        std::vector<Record> expected, actual; Diffs diffs;
        std::size_t points=0, detailed=0;
        elevsafe::BackendPipeline backend(1); // Enqueue -> immediate drain, as in main.cpp.
        const sp::FrameContext context;
        for(const auto& source : frames)
        {
            const auto radar=toRadarFrame(source);
            const auto left=direct.process(source,context);
            const auto leftRecord=record(left);
            require(leftRecord.id==source.frame_id && leftRecord.timestamp==source.timestamp_us,"B direct metadata changed");
            expected.push_back(leftRecord);
            backend.onFrame(radar);
            require(backend.pendingSize()==1,"Unexpected pending size before drain");
            const auto drained=backend.drain([&](const elevsafe::RadarFrame& pending)
            {
                const auto converted=elevsafe::toSignalFrame(pending);
                checkInput(source,converted);
                const auto right=adapter.process(converted,context);
                actual.push_back(record(right));
                compareClusters(left,right,diffs);
                detailed+=std::min(left.clusters.size(),right.clusters.size());
            });
            require(drained==1 && backend.pendingSize()==0,"Frame lost or repeated during drain");
            points+=source.points.size();
        }
        compareRecords(expected,actual,"direct-adapter",diffs);
        compareRecords(reference,expected,"baseline-direct",diffs);
        const auto left=totals(expected), right=totals(actual);
        const bool official=!s03 || (left.frames==1673 && points==381730 && left.clusters==333
            && left.transitions==7 && left.open==1175 && left.closed==489 && left.unknown==9);
        if(!official) diffs.push_back({"official",-1,"totals","1673 frames/381730 points/333 clusters/7 transitions/1175 OPEN/489 CLOSED/9 UNKNOWN","see direct totals"});
        writeRecords(directory/"direct_frames.csv",expected);
        writeRecords(directory/"adapter_frames.csv",actual);
        auto diffFile=output(directory/"frame_diff.csv");
        diffFile<<"comparison,frame_id,field,expected,actual\n";
        for(const auto& d : diffs) diffFile<<quoted(d.comparison)<<','<<d.id<<','<<quoted(d.field)<<','<<quoted(d.expected)<<','<<quoted(d.actual)<<'\n';
        diffFile.close();
        auto summary=output(directory/"comparison_summary.json");
        std::set<int> mismatchedFrames;
        std::size_t baselineDifferences=0, detailedDifferences=0;
        for(const auto& d : diffs)
        {
            if(d.id>=0) mismatchedFrames.insert(d.id);
            if(d.comparison=="baseline-direct") ++baselineDifferences;
            if(d.field.starts_with("cluster[")) ++detailedDifferences;
        }
        summary<<"{\n  \"passed\":"<<(diffs.empty()?"true":"false")<<",\n  \"input_points\":"<<points
            <<",\n  \"background_direct\":false,\n  \"background_adapter\":false,\n  \"direct\":";
        writeTotals(summary,left); summary<<",\n  \"adapter\":"; writeTotals(summary,right);
        summary<<",\n  \"baseline_frames\":"<<reference.size()<<",\n  \"difference_count\":"<<diffs.size()
            <<",\n  \"mismatched_frame_count\":"<<mismatchedFrames.size()
            <<",\n  \"baseline_difference_count\":"<<baselineDifferences
            <<",\n  \"cluster_detail_difference_count\":"<<detailedDifferences
            <<",\n  \"detailed_clusters_compared\":"<<detailed
            <<",\n  \"float_comparison\":\"bit-exact float32\",\n  \"official_s03_checked\":"<<(s03?"true":"false")<<"\n}\n";
        summary.close();
        std::cout<<"direct="; writeTotals(std::cout,left); std::cout<<"\nadapter="; writeTotals(std::cout,right);
        std::cout<<"\npoints="<<points<<" detailedClusters="<<detailed<<" differences="<<diffs.size()<<" background=disabled\n";
        if(!diffs.empty())
        {
            const auto& d=diffs.front();
            std::cerr<<"First mismatch: "<<d.comparison<<" frame_id="<<d.id<<" field="<<d.field
                <<" expected="<<d.expected<<" actual="<<d.actual<<'\n';
        }
        return diffs.empty()?0:1;
    }

    using Clock = std::chrono::steady_clock;

    std::optional<Diff> pointDifference(const elevsafe::RadarFrame& expected,
        const elevsafe::RadarFrame& actual, int originalId)
    {
        if(expected.points.size()!=actual.points.size())
            return Diff{"tcp-points",originalId,"point_count",show(expected.points.size()),show(actual.points.size())};
        for(std::size_t i=0;i<expected.points.size();++i)
        {
            const auto& a=expected.points[i]; const auto& b=actual.points[i];
            const auto floats=std::initializer_list<std::pair<const char*,std::pair<float,float>>>{
                {"x",{a.x,b.x}},{"y",{a.y,b.y}},{"z",{a.z,b.z}},
                {"doppler",{a.doppler,b.doppler}},{"power",{a.power,b.power}}};
            for(const auto& [field,values] : floats)
                if(!sameFloat(values.first,values.second))
                    return Diff{"tcp-points",originalId,"point["+show(i)+"]."+field,
                        show(values.first)+" bits="+show(std::bit_cast<std::uint32_t>(values.first)),
                        show(values.second)+" bits="+show(std::bit_cast<std::uint32_t>(values.second))};
            if(a.targetId!=b.targetId)
                return Diff{"tcp-points",originalId,"point["+show(i)+"].target_id",show(a.targetId),show(b.targetId)};
        }
        return std::nullopt;
    }

    // Stable FNV-1a over explicit little-endian words. Only a lookup hint:
    // pointDifference() must still confirm every float32/int32 bit.
    std::uint64_t pointFingerprint(const elevsafe::RadarFrame& frame)
    {
        std::uint64_t hash=14695981039346656037ull;
        const auto word=[&](std::uint32_t value)
        {
            for(unsigned shift=0;shift<32;shift+=8)
            { hash^=(value>>shift)&255u; hash*=1099511628211ull; }
        };
        require(std::in_range<std::uint32_t>(frame.points.size()),"Point count cannot fit fingerprint");
        word(static_cast<std::uint32_t>(frame.points.size()));
        for(const auto& p : frame.points)
        {
            word(std::bit_cast<std::uint32_t>(p.x)); word(std::bit_cast<std::uint32_t>(p.y));
            word(std::bit_cast<std::uint32_t>(p.z)); word(std::bit_cast<std::uint32_t>(p.doppler));
            word(std::bit_cast<std::uint32_t>(p.power)); word(std::bit_cast<std::uint32_t>(p.targetId));
        }
        return hash;
    }

    struct Mapping
    {
        std::size_t index;
        int originalId;
        std::uint32_t tcpId;
        std::int64_t offlineTime;
        std::uint64_t hostTime;
        std::size_t points;
        bool matched;
    };

    class TcpComparison
    {
        const std::vector<sp::FrameData>& source;
        sp::Config config;
        std::vector<elevsafe::RadarFrame> originals;
        std::unordered_map<std::uint64_t,std::size_t> lookup;
        std::vector<sp::PipelineResult> expected;
        std::unique_ptr<sp::SignalPipeline> live;
        elevsafe::BackendPipeline backend{1};
        std::function<std::uint64_t(const elevsafe::RadarFrame&)> fingerprint;
        Clock::time_point startTime{};
        std::chrono::seconds syncLimit, totalLimit;
        std::optional<std::uint32_t> previousTcp;
        std::optional<std::uint64_t> previousTime;
        bool synchronized=false;
        std::vector<Record> results;

        void fail(const std::string& why, std::size_t index)
        {
            if(finished) return;
            reason=why; firstFailure=index; finished=true; passed=false;
        }

    public:
        bool finished=false, passed=false;
        std::string reason="not_started";
        std::optional<std::size_t> firstFailure;
        int errorCode=0;
        std::size_t received=0, excluded=0, verifiedFrames=0, verifiedPoints=0, detailedClusters=0;
        std::vector<Mapping> mapping;
        Diffs frameDiffs, pointDiffs;

        TcpComparison(const std::vector<sp::FrameData>& frames, const sp::Config& cfg,
            const std::vector<Record>& baseline, std::chrono::seconds syncSeconds,
            std::chrono::seconds totalSeconds, bool official,
            std::function<std::uint64_t(const elevsafe::RadarFrame&)> hash=pointFingerprint) :
            source(frames), config(cfg), fingerprint(std::move(hash)), syncLimit(syncSeconds), totalLimit(totalSeconds)
        {
            require(!source.empty(),"TCP reference is empty");
            sp::SignalPipeline direct(config);
            require(direct.ok() && !direct.usesBackground(),"TCP reference requires valid config and background disabled");
            std::vector<Record> records;
            std::size_t points=0;
            for(std::size_t i=0;i<source.size();++i)
            {
                originals.push_back(toRadarFrame(source[i]));
                const auto key=fingerprint(originals.back());
                require(lookup.emplace(key,i).second,"Ambiguous/colliding reference point fingerprint at index "+show(i));
                expected.push_back(direct.process(source[i],{}));
                require(expected.back().frame_id==source[i].frame_id && expected.back().timestamp_us==source[i].timestamp_us,
                    "Offline B metadata changed");
                records.push_back(record(expected.back())); points+=source[i].points.size();
            }
            compareRecords(baseline,records,"baseline-direct",frameDiffs);
            if(!frameDiffs.empty()) fail("offline_baseline_mismatch",0);
            if(official)
            {
                const auto t=totals(records);
                require(config.getFloat("door_state.close_threshold",1.0f)==0.5f && t.frames==1673 && points==381730
                    && t.clusters==333 && t.transitions==7 && t.open==1175 && t.closed==489 && t.unknown==9,
                    "Offline S0-3 totals/config do not match the established baseline");
            }
        }

        void start(Clock::time_point now)
        {
            startTime=now;
            if(!finished) reason="waiting_for_index_0";
        }
        Clock::time_point deadline() const
        { return synchronized ? startTime+totalLimit : std::min(startTime+syncLimit,startTime+totalLimit); }
        void checkTime(Clock::time_point now)
        {
            if(!finished && now>=deadline()) fail(synchronized ? "total_timeout" : "sync_timeout",results.size());
        }

        void accept(const elevsafe::RadarFrame& frame, Clock::time_point now)
        {
            if(finished) return; // Freeze even if feed() emits more callbacks after stop().
            const auto processingStart=Clock::now();
            ++received;
            checkTime(now); if(finished) return;
            const std::size_t next=results.size();
            const auto found=lookup.find(fingerprint(frame));
            if(found==lookup.end())
            {
                const auto difference=pointDifference(originals[next],frame,source[next].frame_id);
                if(difference) pointDiffs.push_back(*difference);
                else frameDiffs.push_back({"tcp-mapping",source[next].frame_id,"fingerprint","known reference","no matching fingerprint"});
                mapping.push_back({next,source[next].frame_id,frame.frameCount,source[next].timestamp_us,frame.timestampUs,frame.points.size(),false});
                fail("unexpected_or_modified_frame",next); return;
            }
            const auto index=found->second;
            if(const auto difference=pointDifference(originals[index],frame,source[index].frame_id))
            {
                pointDiffs.push_back(*difference);
                mapping.push_back({index,source[index].frame_id,frame.frameCount,source[index].timestamp_us,frame.timestampUs,frame.points.size(),false});
                fail("fingerprint_collision_or_modified_points",next); return;
            }
            if(!synchronized && index!=0) { ++excluded; return; }
            if(!synchronized)
            {
                live=std::make_unique<sp::SignalPipeline>(config);
                require(live->ok() && !live->usesBackground(),"TCP B initialization/background differs at synchronization");
                synchronized=true; reason="validating";
            }
            mapping.push_back({index,source[index].frame_id,frame.frameCount,source[index].timestamp_us,frame.timestampUs,frame.points.size(),true});
            if(index!=next)
            {
                frameDiffs.push_back({"tcp-order",source[next].frame_id,"logical_index",show(next),show(index)});
                fail(index+1==next ? "duplicate_frame" : "missing_or_out_of_order_frame",next); return;
            }
            if(previousTcp && static_cast<std::uint64_t>(frame.frameCount)!=static_cast<std::uint64_t>(*previousTcp)+1)
            {
                frameDiffs.push_back({"tcp-order",source[index].frame_id,"tcp_frame_id",show(static_cast<std::uint64_t>(*previousTcp)+1),show(frame.frameCount)});
                fail("tcp_frame_id_gap_or_duplicate",index); return;
            }
            if(previousTime && frame.timestampUs<*previousTime)
            {
                frameDiffs.push_back({"tcp-metadata",source[index].frame_id,"host_timestamp_us",">="+show(*previousTime),show(frame.timestampUs)});
                fail("host_timestamp_reversed",index); return;
            }
            previousTcp=frame.frameCount; previousTime=frame.timestampUs;
            backend.onFrame(frame);
            require(backend.pendingSize()==1,"TCP pending size before drain is not one");
            const auto before=frameDiffs.size();
            const auto drained=backend.drain([&](const elevsafe::RadarFrame& pending)
            {
                const auto converted=elevsafe::toSignalFrame(pending);
                auto preserved=source[index];
                require(std::in_range<int>(frame.frameCount) && std::in_range<std::int64_t>(frame.timestampUs),"TCP metadata out of B range");
                preserved.frame_id=static_cast<int>(frame.frameCount);
                preserved.timestamp_us=static_cast<std::int64_t>(frame.timestampUs);
                checkInput(preserved,converted);
                const auto result=live->process(converted,{});
                if(result.frame_id!=converted.frame_id || result.timestamp_us!=converted.timestamp_us)
                    frameDiffs.push_back({"tcp-metadata",source[index].frame_id,"B metadata",show(converted.frame_id)+":"+show(converted.timestamp_us),
                        show(result.frame_id)+":"+show(result.timestamp_us)});
                // Normalize only the comparison record; never rewrite the received RadarFrame/B input.
                Record normalized{source[index].frame_id,source[index].timestamp_us,result.clusters.size(),result.estimated_door_state};
                compareRecords({record(expected[index])},{normalized},"offline-tcp",frameDiffs);
                compareClusters(expected[index],result,frameDiffs);
                detailedClusters+=std::min(expected[index].clusters.size(),result.clusters.size());
                results.push_back(normalized);
            });
            require(drained==1 && backend.pendingSize()==0,"TCP frame dropped/repeated in BackendPipeline");
            verifiedPoints+=frame.points.size();
            if(frameDiffs.size()!=before) { fail("B_result_mismatch",index); return; }
            if(now+(Clock::now()-processingStart)>=deadline()) { fail("total_timeout",index); return; }
            ++verifiedFrames;
            if(results.size()==source.size()) { finished=true; passed=true; reason="complete"; }
        }

        void processingError(const std::string& message)
        {
            if(finished) return;
            frameDiffs.push_back({"tcp-processing",source[std::min(results.size(),source.size()-1)].frame_id,"exception","none",message});
            fail("processing_exception",results.size());
        }
        void transportEnded(const elevsafe::RetinaClient::RunDiagnostics& diagnostic, Clock::time_point now)
        {
            if(finished) return;
            errorCode=diagnostic.errorCode;
            checkTime(now); if(finished) return;
            fail(errorCode==10014 ? "winsock_10014" : diagnostic.timedOut ? "transport_timeout"
                : diagnostic.stage=="eof" ? "early_eof" : diagnostic.stage=="socket_runtime" ? "socket_runtime_error"
                : diagnostic.stage=="connect" || diagnostic.stage=="resolve"
                ? "connection_error" : "receive_error",results.size());
        }
        std::size_t completed() const { return verifiedFrames; }
        bool success(const elevsafe::RetinaClient::RunDiagnostics& diagnostic) const
        { return passed && diagnostic.errorCode==0 && diagnostic.parser.packetsRejected==0; }
        std::string terminationReason(const elevsafe::RetinaClient::RunDiagnostics& diagnostic) const
        {
            if(passed && diagnostic.parser.packetsRejected!=0) return "parser_rejections";
            if(passed && diagnostic.errorCode!=0) return "transport_error_after_completion";
            return reason;
        }

        void save(const fs::path& directory, const elevsafe::RetinaClient::RunDiagnostics& diagnostic) const
        {
            auto mapFile=output(directory/"tcp_frame_mapping.csv");
            mapFile<<"logical_index,original_frame_id,tcp_frame_id,offline_timestamp_us,host_timestamp_us,point_count,points_match\n";
            for(const auto& m : mapping) mapFile<<m.index<<','<<m.originalId<<','<<m.tcpId<<','<<m.offlineTime<<','<<m.hostTime<<','<<m.points<<','<<(m.matched?1:0)<<'\n';
            mapFile.close();
            auto framesFile=output(directory/"tcp_frames.csv");
            framesFile<<"logical_index,original_frame_id,tcp_frame_id,host_timestamp_us,num_clusters,door_state\n";
            for(std::size_t i=0;i<results.size();++i)
            {
                const auto& r=results[i]; const auto& m=mapping[i];
                framesFile<<i<<','<<r.id<<','<<m.tcpId<<','<<m.hostTime<<','<<r.clusters<<','<<sp::doorStateToString(r.state)<<'\n';
            }
            framesFile.close();
            const auto writeDiffs=[&](const fs::path& path,const Diffs& diffs)
            {
                auto f=output(path); f<<"comparison,frame_id,field,expected,actual\n";
                for(const auto& d : diffs) f<<quoted(d.comparison)<<','<<d.id<<','<<quoted(d.field)<<','<<quoted(d.expected)<<','<<quoted(d.actual)<<'\n';
                f.close();
            };
            writeDiffs(directory/"tcp_frame_diff.csv",frameDiffs); writeDiffs(directory/"tcp_point_diff.csv",pointDiffs);
            std::size_t clusterDiff=0,doorDiff=0;
            for(const auto& d : frameDiffs)
            { if(d.field.starts_with("cluster[") || d.field=="num_clusters") ++clusterDiff; if(d.field=="door_state") ++doorDiff; }
            auto summary=output(directory/"tcp_comparison_summary.json");
            summary<<"{\n\"complete\":"<<(passed?"true":"false")<<",\n\"passed\":"<<(success(diagnostic)?"true":"false")
                <<",\n\"termination_reason\":\""<<terminationReason(diagnostic)<<"\",\n\"error_code\":"<<(errorCode!=0?errorCode:diagnostic.errorCode)
                <<",\n\"received_tcp_frames\":"<<received<<",\n\"excluded_before_sync\":"<<excluded
                <<",\n\"verified_frames\":"<<verifiedFrames<<",\n\"processed_frames\":"<<results.size()
                <<",\n\"reference_frames\":"<<source.size()<<",\n\"verified_points\":"<<verifiedPoints
                <<",\n\"point_difference_count\":"<<pointDiffs.size()<<",\n\"frame_difference_count\":"<<frameDiffs.size()
                <<",\n\"cluster_difference_count\":"<<clusterDiff<<",\n\"door_state_difference_count\":"<<doorDiff
                <<",\n\"detailed_clusters_compared\":"<<detailedClusters<<",\n\"first_failure_logical_index\":"<<(firstFailure?show(*firstFailure):"null")
                <<",\n\"background_enabled\":false,\n\"parser_packets_parsed\":"<<diagnostic.parser.packetsParsed
                <<",\n\"parser_packets_rejected\":"<<diagnostic.parser.packetsRejected<<",\n\"parser_buffered_bytes\":"<<diagnostic.bufferedBytes
                <<",\n\"totals\":"; writeTotals(summary,totals(results)); summary<<"\n}\n"; summary.close();
        }
    };

    class StreamTee : public std::streambuf
    {
        std::ostream& stream;
        std::streambuf* original;
        std::streambuf* copy;
        std::streamsize xsputn(const char* data,std::streamsize size) override
        { return std::min(original->sputn(data,size),copy->sputn(data,size)); }
        int_type overflow(int_type value) override
        {
            if(traits_type::eq_int_type(value,traits_type::eof())) return traits_type::not_eof(value);
            return traits_type::eq_int_type(original->sputc(traits_type::to_char_type(value)),traits_type::eof())
                || traits_type::eq_int_type(copy->sputc(traits_type::to_char_type(value)),traits_type::eof())
                ? traits_type::eof() : value;
        }
        int sync() override { return original->pubsync()==0 && copy->pubsync()==0 ? 0 : -1; }
    public:
        StreamTee(std::ostream& target,std::ostream& duplicate) : stream(target),original(target.rdbuf()),copy(duplicate.rdbuf())
        { stream.rdbuf(this); }
        ~StreamTee() override { stream.flush(); stream.rdbuf(original); }
    };

    int tcpCompare(int argc,char** argv)
    {
        require(argc>=4,"--tcp requires HOST PORT");
        const std::string host=argv[2];
        const auto port=integer<unsigned>(argv[3]); require(port>0 && port<=65535,"Invalid TCP port");
        std::map<std::string,std::string> options;
        bool official=false;
        for(int i=4;i<argc;++i)
        {
            const std::string key=argv[i];
            if(key=="--s0-3") { require(!official,"Duplicate --s0-3"); official=true; continue; }
            require(key=="--config" || key=="--input" || key=="--baseline" || key=="--output"
                || key=="--sync-timeout" || key=="--total-timeout","Unknown TCP option: "+key);
            require(i+1<argc && !std::string(argv[i+1]).starts_with("--"),"Missing value for "+key);
            require(options.emplace(key,argv[++i]).second,"Duplicate TCP option: "+key);
        }
        for(const auto* key : {"--config","--input","--baseline","--output"}) require(options.contains(key) && !options[key].empty(),"Missing option: "+std::string(key));
        const auto seconds=[&](const char* key,unsigned fallback)
        {
            const auto value=options.contains(key)?integer<unsigned>(options.at(key)):fallback;
            require(value>0 && value<=86400,"Timeout must be 1..86400 seconds");
            return std::chrono::seconds(value);
        };
        const auto sync=seconds("--sync-timeout",120), total=seconds("--total-timeout",240);
        const fs::path directory=options.at("--output");
        require(!fs::exists(directory) || (fs::is_directory(directory) && fs::is_empty(directory)),"TCP output directory must be new or empty");
        fs::create_directories(directory);
        auto stdoutFile=output(directory/"tcp_stdout.log"), stderrFile=output(directory/"tcp_stderr.log");
        StreamTee stdoutTee(std::cout,stdoutFile), stderrTee(std::cerr,stderrFile);
        try
        {
        sp::Config config; require(config.load(options.at("--config")),"TCP config load failed");
        const auto frames=readInput(options.at("--input"));
        std::ifstream baselineFile(options.at("--baseline")); require(baselineFile.is_open(),"Cannot open TCP baseline");
        const auto baseline=readBaseline(baselineFile);
        TcpComparison comparison(frames,config,baseline,sync,total,official);
        elevsafe::RetinaClient client(host,static_cast<std::uint16_t>(port));
        elevsafe::RetinaClient::RunDiagnostics diagnostic;
        comparison.start(Clock::now());
        client.setFrameCallback([&](const elevsafe::RadarFrame& frame)
        {
            if(comparison.finished) return;
            try { comparison.accept(frame,Clock::now()); }
            catch(const std::exception& error) { comparison.processingError(error.what()); }
            if(comparison.completed()==1 && comparison.mapping.size()==1)
                std::cout<<"TCP synchronized: original_frame_id="<<comparison.mapping.front().originalId
                    <<" tcp_frame_id="<<comparison.mapping.front().tcpId<<'\n'<<std::flush;
            if(comparison.finished) client.stop(); // Same run/callback thread; no joins or racing watchdog.
        });
        if(!comparison.finished)
        {
            std::cout<<"TCP validation ready: "<<host<<':'<<port<<"; waiting for logical index 0\n"<<std::flush;
            client.run(std::cerr,[&]{return comparison.deadline();},diagnostic);
            comparison.transportEnded(diagnostic,Clock::now());
        }
        comparison.save(directory,diagnostic);
        std::ostringstream failure;
        if(!comparison.success(diagnostic)) failure<<"Validation failed: "<<comparison.terminationReason(diagnostic)<<" first_failure_logical_index="
            <<(comparison.firstFailure?show(*comparison.firstFailure):"null")<<'\n';
        const auto& differences=!comparison.pointDiffs.empty()?comparison.pointDiffs:comparison.frameDiffs;
        if(!differences.empty())
        {
            const auto& d=differences.front();
            failure<<"First mismatch: original_frame_id="<<d.id<<" field="<<d.field<<" expected="<<d.expected<<" actual="<<d.actual<<'\n';
        }
        std::cout<<"TCP validation "<<(comparison.success(diagnostic)?"PASS":"FAIL")<<" reason="<<comparison.terminationReason(diagnostic)
            <<" received="<<comparison.received<<" excluded="<<comparison.excluded<<" verified="<<comparison.completed()
            <<" points="<<comparison.verifiedPoints<<" error_code="<<comparison.errorCode<<'\n';
        std::cerr<<failure.str();
        return comparison.success(diagnostic)?0:1;
        }
        catch(const std::exception& error)
        {
            std::cerr<<"Initialization/output error: "<<error.what()<<'\n';
            auto summary=output(directory/"tcp_comparison_summary.json");
            summary<<"{\"complete\":false,\"passed\":false,\"termination_reason\":\"initialization_or_output_error\",\"error_code\":0,"
                <<"\"received_tcp_frames\":0,\"excluded_before_sync\":0,\"verified_frames\":0,\"verified_points\":0,\"first_failure_logical_index\":null}\n";
            summary.close();
            return 1;
        }
    }

    template<class Action> void rejects(Action action)
    {
        bool rejected=false;
        try { action(); } catch(const std::exception&) { rejected=true; }
        require(rejected,"Self-test expected rejection");
    }

    void tcpSelfTest(const char* cfgPath)
    {
        sp::Config cfg; require(cfg.load(cfgPath),"TCP self-test config failed");
        std::vector<sp::FrameData> frames(3);
        sp::SignalPipeline direct(cfg);
        std::vector<Record> baseline;
        for(std::size_t i=0;i<frames.size();++i)
        {
            auto& f=frames[i]; f.frame_id=10+static_cast<int>(i)*10; f.timestamp_us=static_cast<std::int64_t>(i)*50000;
            for(int j=0;j<12;++j) f.points.push_back({static_cast<float>(i)*0.4f+static_cast<float>(j)*0.01f,0.6f,1.0f,-0.5f,100.0f,255});
            baseline.push_back(record(direct.process(f,{})));
        }
        const auto origin=Clock::now();
        const auto make=[&]{return std::make_unique<TcpComparison>(frames,cfg,baseline,std::chrono::seconds(5),std::chrono::seconds(10),false);};
        const auto send=[&](TcpComparison& c,std::size_t index,unsigned counter)
        { auto f=toRadarFrame(frames[index]); f.frameCount=500+counter; f.timestampUs=counter*50000ull; c.accept(f,origin+std::chrono::milliseconds(counter)); };
        auto complete=make(); complete->start(origin);
        send(*complete,1,0); send(*complete,2,1); send(*complete,0,2); send(*complete,1,3); send(*complete,2,4);
        require(complete->passed && complete->completed()==3 && complete->excluded==2 && complete->received==5
            && complete->verifiedPoints==36 && complete->frameDiffs.empty() && complete->pointDiffs.empty(),"Tail/synchronization/full cycle failed");
        send(*complete,0,5);
        require(complete->passed && complete->received==5 && complete->mapping.size()==3 && complete->completed()==3,"Completed metrics changed");
        elevsafe::RetinaClient::RunDiagnostics rejected; rejected.parser.packetsRejected=1;
        require(!complete->success(rejected) && complete->terminationReason(rejected)=="parser_rejections","Parser rejection accepted");
        auto missing=make(); missing->start(origin); send(*missing,0,0); send(*missing,2,1);
        require(missing->finished && !missing->passed && missing->reason=="missing_or_out_of_order_frame","Missing frame not detected");
        auto duplicate=make(); duplicate->start(origin); send(*duplicate,0,0); send(*duplicate,0,1);
        require(duplicate->reason=="duplicate_frame","Duplicate not detected");
        auto order=make(); order->start(origin); send(*order,0,0); send(*order,1,1); send(*order,0,2);
        require(order->reason=="missing_or_out_of_order_frame","Reversed order not detected");
        for(int kind=0;kind<3;++kind)
        {
            auto changed=make(); changed->start(origin); send(*changed,0,0);
            auto f=toRadarFrame(frames[1]); f.frameCount=501; f.timestampUs=50000;
            if(kind==0) f.points[0].x=std::bit_cast<float>(std::bit_cast<std::uint32_t>(f.points[0].x)^1u);
            if(kind==1) std::swap(f.points[0],f.points[1]);
            if(kind==2) f.points.pop_back();
            changed->accept(f,origin+std::chrono::milliseconds(1));
            require(changed->finished && !changed->passed && !changed->pointDiffs.empty(),"Point bit/order/count change not detected");
        }
        rejects([&]{TcpComparison collision(frames,cfg,baseline,std::chrono::seconds(5),std::chrono::seconds(10),false,
            [](const auto&){return std::uint64_t{1};});});
        auto broken=std::make_shared<bool>(false);
        TcpComparison badHash(frames,cfg,baseline,std::chrono::seconds(5),std::chrono::seconds(10),false,
            [broken](const auto& f){return pointFingerprint(f)^(*broken?1ull:0ull);});
        badHash.start(origin); *broken=true; send(badHash,0,0);
        require(badHash.finished && !badHash.passed,"Incorrect fingerprint not detected");
        auto repeated=frames; repeated[1].points=repeated[0].points;
        rejects([&]{TcpComparison ambiguous(repeated,cfg,baseline,std::chrono::seconds(5),std::chrono::seconds(10),false);});
        elevsafe::RetinaClient::RunDiagnostics eof; eof.stage="eof";
        auto partial=make(); partial->start(origin); send(*partial,0,0); partial->transportEnded(eof,origin+std::chrono::seconds(1));
        require(partial->reason=="early_eof" && partial->completed()==1 && !partial->passed,"Partial EOF accepted");
        auto syncTimeout=make(); syncTimeout->start(origin); syncTimeout->checkTime(origin+std::chrono::seconds(5));
        require(syncTimeout->reason=="sync_timeout","Sync timeout not detected");
        auto totalTimeout=make(); totalTimeout->start(origin); send(*totalTimeout,0,0); totalTimeout->checkTime(origin+std::chrono::seconds(10));
        require(totalTimeout->reason=="total_timeout" && !totalTimeout->passed,"Total timeout not detected");
        auto gap=make(); gap->start(origin); send(*gap,0,0); send(*gap,1,2);
        require(gap->reason=="tcp_frame_id_gap_or_duplicate","TCP stream gap not detected");

        // Deterministic wire-byte test, not a TCP server/emulator. Feed multiple
        // packets and one-byte chunks through the real RetinaStreamParser.
        const auto packet=[](const elevsafe::RadarFrame& f)
        {
            std::vector<std::uint8_t> bytes;
            const auto word=[&](std::uint32_t value){for(unsigned s=0;s<32;s+=8) bytes.push_back(static_cast<std::uint8_t>(value>>s));};
            for(int i=0;i<9;++i) word(i==1?elevsafe::kPacketMagic:i==4?static_cast<std::uint32_t>(16+f.points.size()*24):0);
            word(static_cast<std::uint32_t>(elevsafe::kFrameMagic)); word(static_cast<std::uint32_t>(elevsafe::kFrameMagic>>32));
            word(f.frameCount); word(static_cast<std::uint32_t>(f.points.size()));
            for(const auto& p : f.points) for(float v : {p.x,p.y,p.z,p.doppler,p.power}) word(std::bit_cast<std::uint32_t>(v));
            for(const auto& p : f.points) word(std::bit_cast<std::uint32_t>(p.targetId));
            return bytes;
        };
        for(bool byteChunks : {false,true})
        {
            auto parsed=make(); parsed->start(origin);
            std::vector<std::uint8_t> wire;
            for(unsigned i=0;i<4;++i)
            { auto f=toRadarFrame(frames[i%3]); f.frameCount=100+i; const auto p=packet(f); wire.insert(wire.end(),p.begin(),p.end()); }
            unsigned callbacks=0;
            elevsafe::RetinaStreamParser parser([&](elevsafe::RadarFrame& f)
            {f.timestampUs=callbacks++*50000ull; parsed->accept(f,origin+std::chrono::milliseconds(callbacks));});
            if(byteChunks) for(const auto byte : wire) parser.feed(std::span<const std::uint8_t>(&byte,1));
            else parser.feed(wire);
            require(parsed->passed && parsed->received==3 && callbacks==4 && parser.statistics().packetsParsed==4
                && parser.statistics().packetsRejected==0,"Parser/mapping/drain integration or completion freeze failed");
        }
        elevsafe::RetinaClient expired("127.0.0.1",1);
        elevsafe::RetinaClient::RunDiagnostics status; std::ostringstream log;
        const auto result=expired.run(log,[]{return Clock::time_point{};},status);
        require(result==1 && status.timedOut && status.stage=="start","Expired client deadline not bounded");
        std::cout<<"TCP validation self-test passed (mapping, Parser/drain, expired-client deadline; no live TCP)\n";
    }
    void selfTest(const char* cfgPath)
    {
        const auto audit=[](const std::string& rows) { std::istringstream s(std::string(inputHeader)+"\n"+rows); return auditInput(s); };
        require(audit("1,0,0,0,0,0,0,255\n1,0,1,0,0,0,0,-1\n2,1,0,0,0,0,0,-1\n").size()==2,"Raw audit failed");
        for(const auto* rows : {"-1,0,0,0,0,0,0,0\n","1,-1,0,0,0,0,0,0\n","2147483648,0,0,0,0,0,0,0\n",
            "1,9223372036854775808,0,0,0,0,0,0\n","1,0,nan,0,0,0,0,0\n","1,0,1oops,0,0,0,0,0\n",
            "1,0,0,0,0,0,0,2147483648\n","1,0,0,0,0,0,0,0\n1,1,0,0,0,0,0,0\n",
            "2,0,0,0,0,0,0,0\n1,1,0,0,0,0,0,0\n","1,0,0,0,0,0,0,0\n2,1,0,0,0,0,0,0\n1,2,0,0,0,0,0,0\n"})
            rejects([&]{audit(rows);});
        sp::FrameData frame; frame.frame_id=0; frame.points.push_back({-0.0f,1,2,-3,4,255});
        checkInput(frame,elevsafe::toSignalFrame(toRadarFrame(frame)));
        frame.points.clear(); checkInput(frame,elevsafe::toSignalFrame(toRadarFrame(frame)));
        frame.frame_id=std::numeric_limits<int>::max(); frame.timestamp_us=std::numeric_limits<std::int64_t>::max();
        checkInput(frame,elevsafe::toSignalFrame(toRadarFrame(frame)));
        frame.frame_id=-1; rejects([&]{toRadarFrame(frame);});
        frame.frame_id=0; frame.timestamp_us=-1; rejects([&]{toRadarFrame(frame);});
        frame.timestamp_us=0; frame.points.push_back({}); frame.points[0].zone=sp::Zone::CABIN;
        rejects([&]{toRadarFrame(frame);});
        const std::vector<Record> reference={{1,0,0,sp::DoorState::UNKNOWN},{2,1,1,sp::DoorState::OPEN}};
        Diffs diffs; compareRecords(reference,reference,"self",diffs); require(diffs.empty(),"Identical records differ");
        auto changed=reference; changed.pop_back(); compareRecords(reference,changed,"self",diffs);
        require(std::any_of(diffs.begin(),diffs.end(),[](const auto& d){return d.field=="missing_id";}),"Missing ID not detected");
        diffs.clear(); changed=reference; changed.push_back(reference[0]); compareRecords(reference,changed,"self",diffs);
        require(std::any_of(diffs.begin(),diffs.end(),[](const auto& d){return d.field=="duplicate_id";}),"Duplicate ID not detected");
        diffs.clear(); changed=reference; std::reverse(changed.begin(),changed.end()); compareRecords(reference,changed,"self",diffs);
        require(!diffs.empty(),"Order mismatch not detected");
        diffs.clear(); changed=reference; changed[0].timestamp=9; changed[0].clusters=2; changed[0].state=sp::DoorState::CLOSED;
        compareRecords(reference,changed,"self",diffs); require(diffs.size()==3,"Metadata/result differences not detected");
        sp::PipelineResult a,b; a.clusters.resize(1); b=a; b.clusters[0].z_top=1.0f;
        diffs.clear(); compareClusters(a,b,diffs); require(diffs.size()==1,"Detailed difference not detected");
        b=a; b.clusters[0].cx=-0.0f; diffs.clear(); compareClusters(a,b,diffs); require(diffs.size()==1,"Float bits not checked");
        std::istringstream goodBaseline(std::string(resultHeader)+"\n1,0,UNKNOWN\n");
        require(readBaseline(goodBaseline).size()==1,"Baseline read failed");
        rejects([]{std::istringstream bad(std::string(resultHeader)+"\n1,0,INVALID\n");readBaseline(bad);});
        sp::Config cfg; require(cfg.load(cfgPath),"Self-test config failed");
        sp::SignalPipeline direct(cfg),adapter(cfg); require(direct.ok() && adapter.ok(),"Self-test B initialization failed");
        elevsafe::BackendPipeline backend(1); sp::FrameData empty; empty.frame_id=3;
        const auto expected=direct.process(empty,{}); backend.onFrame(toRadarFrame(empty));
        std::size_t called=0;
        const auto drained=backend.drain([&](const auto& pending){const auto actual=adapter.process(elevsafe::toSignalFrame(pending),{});
            require(actual.frame_id==expected.frame_id && actual.clusters.empty()
                && actual.estimated_door_state==expected.estimated_door_state,"Empty frame changed"); ++called;});
        require(drained==1 && called==1 && backend.pendingSize()==0,"Empty drain boundary failed");
        std::cout<<"signal_replay_compare self-test passed\n";
    }
}

int main(int argc,char** argv)
{
    try
    {
        if(argc==3 && std::string(argv[1])=="--self-test") { selfTest(argv[2]); return 0; }
        if(argc==3 && std::string(argv[1])=="--tcp-self-test") { tcpSelfTest(argv[2]); return 0; }
        if(argc>=2 && std::string(argv[1])=="--tcp") return tcpCompare(argc,argv);
        if(argc!=5 && argc!=6) { std::cerr<<"Usage: signal_replay_compare CONFIG INPUT_CSV BASELINE_CSV NEW_OUTPUT_DIR [--s0-3]\n"
            <<"       signal_replay_compare --tcp IP PORT --config CFG --input CSV --baseline CSV --output NEW_DIR"
            <<" [--sync-timeout SECONDS] [--total-timeout SECONDS] [--s0-3]\n"; return 2; }
        const bool s03=argc==6;
        require(!s03 || std::string(argv[5])=="--s0-3","Unknown option");
        return compare(argv[1],argv[2],argv[3],argv[4],s03);
    }
    catch(const std::exception& error) { std::cerr<<"signal_replay_compare failed: "<<error.what()<<'\n'; return 1; }
}
