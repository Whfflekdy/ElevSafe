// sp_tests.cpp
// 신호처리 단위 테스트. 데이터 파일 없이 코드 안에서 점을 만들어 확인한다.
#include <cstdio>
#include <string>

#include "clustering/peak_splitter.h"
#include "preprocessing/door_state_estimator.h"
#include "preprocessing/roi_filter.h"

using namespace elevsafe::sp;

namespace {

int g_failed = 0;

#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("  실패 %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                  \
        }                                                                \
    } while (0)

// 사람 하나를 대신하는 점 기둥. 머리 위치에 z_top 점이 찍힌다
void addPerson(Frame& f, float x, float y, float z_top, int n = 30) {
    for (int i = 0; i < n; ++i) {
        RadarPoint p;
        p.x = x + 0.02f * (i % 3);
        p.y = y;
        p.z = 0.2f + (z_top - 0.2f) * i / (n - 1);
        f.push_back(p);
    }
}

// 두 사람 사이의 낮은 점. 빈 칸은 골짜기로 보지 않으므로 꼭 필요하다
void addValley(Frame& f) {
    for (int i = 0; i < 5; ++i) {
        RadarPoint p;
        p.x = 0.0f;  p.y = 1.0f;  p.z = 1.2f;
        f.push_back(p);
    }
}

ClusteringResult oneCluster(const Frame& f) {
    ClusteringResult cr;
    cr.labels.assign(f.size(), 0);
    cr.num_clusters = 1;
    return cr;
}

PeakSplitConfig splitOn() {
    PeakSplitConfig c;
    c.enabled = true;
    return c;
}

void testSplitOffKeepsLabels() {
    Frame f;
    addPerson(f, -0.25f, 1.0f, 1.75f);
    addPerson(f, 0.25f, 1.0f, 1.70f);
    const SplitResult r = PeakSplitter(PeakSplitConfig{}).split(f, oneCluster(f));
    EXPECT(r.num_clusters == 1);
    EXPECT(r.split_count == 0);
    EXPECT(r.labels == oneCluster(f).labels);
}

void testSplitTwoPeople() {
    Frame f;
    addPerson(f, -0.25f, 1.0f, 1.75f);
    addPerson(f, 0.25f, 1.0f, 1.70f);
    addValley(f);
    const SplitResult r = PeakSplitter(splitOn()).split(f, oneCluster(f));
    EXPECT(r.num_clusters == 2);
    EXPECT(r.split_count == 1);
    EXPECT(r.labels.front() != r.labels[30]);   // 첫 사람과 둘째 사람의 번호가 다름
    EXPECT(r.maybe_merged[0] == 1 && r.maybe_merged[1] == 1);
}

void testSplitOnePersonStays() {
    Frame f;
    addPerson(f, 0.0f, 1.0f, 1.70f);
    const SplitResult r = PeakSplitter(splitOn()).split(f, oneCluster(f));
    EXPECT(r.num_clusters == 1);
    EXPECT(r.maybe_merged[0] == 0);
}

void testSplitNoValleyStays() {
    // 사이가 머리 높이와 비슷하면 골짜기가 없으니 나누지 않는다
    Frame f;
    addPerson(f, -0.25f, 1.0f, 1.75f);
    addPerson(f, 0.25f, 1.0f, 1.70f);
    addPerson(f, 0.0f, 1.0f, 1.68f);
    const SplitResult r = PeakSplitter(splitOn()).split(f, oneCluster(f));
    EXPECT(r.num_clusters == 1);
}

void testSplitSmallPieceFlagged() {
    // 둘째 봉우리 쪽 점이 너무 적으면 나누지 않고 표시만 한다
    Frame f;
    addPerson(f, -0.25f, 1.0f, 1.75f, 40);
    addPerson(f, 0.25f, 1.0f, 1.70f, 5);
    addValley(f);
    const SplitResult r = PeakSplitter(splitOn()).split(f, oneCluster(f));
    EXPECT(r.num_clusters == 1);
    EXPECT(r.maybe_merged[0] == 1);
}

void testSplitKeepsNoise() {
    Frame f;
    addPerson(f, 0.0f, 1.0f, 1.70f);
    ClusteringResult cr = oneCluster(f);
    cr.labels[0] = -1;
    const SplitResult r = PeakSplitter(splitOn()).split(f, cr);
    EXPECT(r.labels[0] == -1);
}

DoorStateConfig doorConfig() {
    DoorStateConfig c;
    c.view_zone = {-0.44f, 0.44f, -3.0f, -0.3f, 0.1f, 2.1f};
    c.window = 3;
    c.open_threshold = 10.0f;
    c.close_threshold = 0.5f;
    return c;
}

Frame hallPoints(int n) {
    Frame f;
    for (int i = 0; i < n; ++i) {
        RadarPoint p;
        p.x = 0.0f;  p.y = -1.0f;  p.z = 1.0f;
        f.push_back(p);
    }
    return f;
}

void testDoorUnknownUntilWindowFull() {
    DoorStateEstimator d(doorConfig());
    EXPECT(d.update(hallPoints(20)) == DoorState::UNKNOWN);
    EXPECT(d.update(hallPoints(20)) == DoorState::UNKNOWN);
    EXPECT(d.update(hallPoints(20)) == DoorState::OPEN);
}

void testDoorHysteresis() {
    DoorStateEstimator d(doorConfig());
    for (int i = 0; i < 3; ++i) d.update(hallPoints(20));
    // 평균이 두 기준 사이면 이전 상태 유지
    for (int i = 0; i < 3; ++i) d.update(hallPoints(3));
    EXPECT(d.state() == DoorState::OPEN);
    // 평균이 닫힘 기준 아래로 내려가면 닫힘
    for (int i = 0; i < 3; ++i) d.update(hallPoints(0));
    EXPECT(d.state() == DoorState::CLOSED);
}

void testRoiDoorWins() {
    ROIConfig c;
    c.door_zone = {-0.44f, 0.44f, -0.5f, 0.3f, 0.1f, 2.1f};
    c.cabin_zone = {-0.75f, 0.75f, 0.3f, 1.35f, 0.1f, 2.2f};
    ROIFilter roi(c);
    RadarPoint door, cabin, edge, out;
    door.y = 0.0f;   door.z = 1.0f;
    cabin.y = 1.0f;  cabin.z = 1.0f;
    edge.y = 0.3f;   edge.z = 1.0f;    // 경계는 DOOR 우선
    out.y = -1.0f;   out.z = 1.0f;
    EXPECT(roi.classify(door) == Zone::DOOR);
    EXPECT(roi.classify(cabin) == Zone::CABIN);
    EXPECT(roi.classify(edge) == Zone::DOOR);
    EXPECT(roi.classify(out) == Zone::NONE);
}

void run(const char* name, void (*fn)()) {
    const int before = g_failed;
    fn();
    std::printf("%s %s\n", g_failed == before ? "통과" : "실패", name);
}

}  // namespace

int main() {
    run("분할 꺼짐: 번호 유지", testSplitOffKeepsLabels);
    run("분할: 2명 나누기", testSplitTwoPeople);
    run("분할: 1명 유지", testSplitOnePersonStays);
    run("분할: 골짜기 없으면 유지", testSplitNoValleyStays);
    run("분할: 작은 조각은 표시만", testSplitSmallPieceFlagged);
    run("분할: 노이즈 유지", testSplitKeepsNoise);
    run("문 상태: 창이 찰 때까지 UNKNOWN", testDoorUnknownUntilWindowFull);
    run("문 상태: 히스테리시스", testDoorHysteresis);
    run("ROI: DOOR 우선", testRoiDoorWins);
    return g_failed == 0 ? 0 : 1;
}