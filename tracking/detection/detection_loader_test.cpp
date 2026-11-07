#include <cstdio>
#include "detection/detection_loader.hpp"
int main(int argc, char** argv) {
    auto frames = loadDetectionsFromPointCloudCsv(argv[1]);
    size_t boxes = 0;
    for (auto& f : frames) boxes += f.detections.size();
    std::printf("frames=%zu boxes=%zu\n", frames.size(), boxes);
    for (size_t i = 0; i < 3 && i < frames.size(); ++i)
        for (auto& d : frames[i].detections)
            std::printf("frame=%lld ts=%lld cluster=%d c=(%.4f,%.4f,%.4f) size=(%.3f,%.3f,%.3f) n=%d\n",
                (long long)frames[i].frame, (long long)frames[i].timestamp_us, d.cluster,
                d.cx, d.cy, d.cz, d.sx, d.sy, d.sz, d.n_points);
}
