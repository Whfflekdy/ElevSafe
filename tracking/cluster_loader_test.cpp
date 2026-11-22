#include "cluster_loader.hpp"
#include <iostream>
#include <cassert>
using namespace elevsafe;
int main(int argc,char**argv){
    std::string base = argc>1?argv[1]:"S0-1_t01_out";
    auto frames = loadClusterCsv(base+"_clusters.csv");
    auto doors  = loadDoorCsv(base+"_door.csv");
    size_t withC=0,total=0; int unk=0;
    for(auto&f:frames){ if(!f.clusters.empty())++withC; total+=f.clusters.size();
        if(doorAt(doors,f.frame_id)==DoorState::UNKNOWN)++unk; }
    std::cout<<"frames="<<frames.size()<<" frames_with_clusters="<<withC<<" total_clusters="<<total
             <<" door_rows="<<doors.size()<<" door_unknown_frames="<<unk<<"\n";
    for(auto&f:frames) if(!f.clusters.empty()){ auto&c=f.clusters[0];
        std::cout<<"first cluster: frame="<<f.frame_id<<" t_ms="<<f.timestamp_ms()<<" c=("<<c.cx<<","<<c.cy<<","<<c.cz
                 <<") z_top="<<c.z_top<<" n="<<c.num_points<<" zone="<<c.zone<<" merged="<<c.maybe_merged<<"\n"; break;}
    auto info = loadDoorInfoCsv(base+"_door.csv");
    size_t hallOn=0, noZone=0, hallCl=0;
    for(auto&kv:info) hallOn += kv.second.hall_enabled;
    for(auto&f:frames) for(auto&c:f.clusters){ if(c.det_zone==DetZone::UNKNOWN)++noZone; if(c.det_zone==DetZone::HALL)++hallCl; }
    std::cout<<"hall_enabled_frames="<<hallOn<<" clusters_zone_unknown="<<noZone<<" clusters_in_HALL="<<hallCl<<"\n";
    if(base.find("S0-1_t01")!=std::string::npos) assert(frames.size()==3646 && withC==28);
    if(base.find("S0-6_t01")!=std::string::npos) assert(frames.size()==2406 && withC==34 && hallOn==2406 && noZone==0);
    std::cout<<"OK\n";
}
