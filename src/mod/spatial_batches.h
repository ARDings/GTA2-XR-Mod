#pragma once
#include "world_mesh.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace gta2ceiling {
// Partition index lists without modifying a single vertex, material, UV or
// winding. Triangle bounds, rather than cell bounds, preserve crossing faces.
inline bool PartitionRasterWorld(gta2::WorldMesh& mesh,float cellSize=32) {
    if (!std::isfinite(cellSize) || cellSize<1) return false;
    for(const auto& batch:mesh.batches) {
        if(batch.indexCount%3 || batch.indexStart>mesh.indices.size() ||
           batch.indexCount>mesh.indices.size()-batch.indexStart) return false;
        for(size_t i=batch.indexStart;i<static_cast<size_t>(batch.indexStart)+batch.indexCount;++i) {
            if(mesh.indices[i]>=mesh.vertices.size()) return false;
            const auto& v=mesh.vertices[mesh.indices[i]];
            if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)||
               std::abs(v.x)>100000 || std::abs(v.z)>100000) return false;
        }
    }
    std::vector<uint32_t> indices; indices.reserve(mesh.indices.size());
    std::vector<gta2::TileBatch> batches;
    for(const auto& source:mesh.batches) {
        std::map<std::pair<int,int>,std::vector<uint32_t>> cells;
        for(size_t i=source.indexStart;i<static_cast<size_t>(source.indexStart)+source.indexCount;i+=3) {
            const auto& a=mesh.vertices[mesh.indices[i]];
            const auto& b=mesh.vertices[mesh.indices[i+1]];
            const auto& c=mesh.vertices[mesh.indices[i+2]];
            const auto key=std::make_pair(static_cast<int>(std::floor((a.x+b.x+c.x)/(3*cellSize))),
                                         static_cast<int>(std::floor((a.z+b.z+c.z)/(3*cellSize))));
            auto& out=cells[key]; out.insert(out.end(),mesh.indices.begin()+i,mesh.indices.begin()+i+3);
        }
        for(const auto& cell:cells) {
            gta2::TileBatch batch=source;
            batch.indexStart=static_cast<uint32_t>(indices.size());
            batch.indexCount=static_cast<uint32_t>(cell.second.size());
            batch.vertexStart=std::numeric_limits<uint32_t>::max();
            uint32_t last=0; batch.bounded=true;
            for(int axis=0;axis<3;++axis) { batch.minimum[axis]=1e30f; batch.maximum[axis]=-1e30f; }
            for(auto index:cell.second) {
                batch.vertexStart=std::min(batch.vertexStart,index); last=std::max(last,index);
                const auto& v=mesh.vertices[index]; const float p[]{v.x,v.y,v.z};
                for(int axis=0;axis<3;++axis) {
                    batch.minimum[axis]=std::min(batch.minimum[axis],p[axis]);
                    batch.maximum[axis]=std::max(batch.maximum[axis],p[axis]);
                }
            }
            batch.vertexCount=last-batch.vertexStart+1;
            indices.insert(indices.end(),cell.second.begin(),cell.second.end()); batches.push_back(batch);
        }
    }
    mesh.indices.swap(indices); mesh.batches.swap(batches); return true;
}
inline bool RasterBatchIntersects(const gta2::TileBatch& batch,float minX,float minZ,float maxX,float maxZ) {
    return !batch.bounded || !(batch.maximum[0]<minX || batch.minimum[0]>maxX ||
                              batch.maximum[2]<minZ || batch.minimum[2]>maxZ);
}
// Conservative D3D clip volume test (row vectors, Z in [0,W]).
inline bool RasterBatchInFrustum(const gta2::TileBatch& batch,const float* m) {
    if(!batch.bounded) return true;
    for(int plane=0;plane<6;++plane) {
        float p[4];
        for(int row=0;row<4;++row) {
            if(plane<4) p[row]=m[row*4+3]+(plane%2?-1.0f:1.0f)*m[row*4+plane/2];
            else p[row]=plane==4?m[row*4+2]:m[row*4+3]-m[row*4+2];
        }
        float distance=p[3];
        for(int axis=0;axis<3;++axis) distance+=p[axis]*(p[axis]>=0?batch.maximum[axis]:batch.minimum[axis]);
        if(distance<-0.0001f) return false;
    }
    return true;
}
}
