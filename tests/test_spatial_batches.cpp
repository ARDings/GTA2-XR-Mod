#include "../src/mod/spatial_batches.h"
#include "math3d.h"
#include <array>
#include <cassert>
#include <cstring>
#include <set>
#include <iostream>

int main() {
    gta2::WorldMesh mesh;
    gta2::TileBatch source; source.tile=7; source.needsAlphaTest=true; source.doubleSided=true;
    for(int x=0;x<256;x+=16) for(int z=0;z<256;z+=16) {
        for(auto p:std::array<std::array<float,3>,3>{{{{float(x),0,float(z)}},
            {{float(x+2),1,float(z)}},{{float(x),0,float(z+2)}}}}) {
            mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
            mesh.vertices.push_back({p[0],p[1],p[2],0,1,0,0.25f,0.75f});
        }
    }
    // A face spans several chunks; it must survive even when its centroid is
    // outside the visible rectangle but a corner enters it.
    for(auto p:std::array<std::array<float,3>,3>{{{{-100,0,-100}},{{200,0,-100}},{{0,0,200}}}}) {
        mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
        mesh.vertices.push_back({p[0],p[1],p[2],0,1,0,0,0});
    }
    source.indexCount=source.vertexCount=static_cast<uint32_t>(mesh.indices.size());
    mesh.batches.push_back(source);
    const auto originalVertices=mesh.vertices;
    std::multiset<std::array<uint32_t,3>> originalTriangles,partitionedTriangles;
    for(size_t i=0;i<mesh.indices.size();i+=3)
        originalTriangles.insert({mesh.indices[i],mesh.indices[i+1],mesh.indices[i+2]});
    assert(gta2ceiling::PartitionRasterWorld(mesh,32));
    assert(mesh.vertices.size()==originalVertices.size());
    assert(std::memcmp(mesh.vertices.data(),originalVertices.data(),mesh.vertices.size()*sizeof(gta2::Vertex))==0);
    unsigned visibleTriangles=0;
    bool retainedCrossing=false;
    for(const auto& batch:mesh.batches) {
        assert(batch.tile==7 && batch.needsAlphaTest && batch.doubleSided && batch.bounded);
        assert(batch.indexCount%3==0);
        const bool visible=gta2ceiling::RasterBatchIntersects(batch,120,120,150,150);
        if(visible) visibleTriangles+=batch.indexCount/3;
        for(size_t i=batch.indexStart;i<static_cast<size_t>(batch.indexStart)+batch.indexCount;i+=3) {
            partitionedTriangles.insert({mesh.indices[i],mesh.indices[i+1],mesh.indices[i+2]});
            for(size_t j=i;j<i+3;++j) {
                const auto index=mesh.indices[j]; assert(index>=batch.vertexStart && index<batch.vertexStart+batch.vertexCount);
                const auto& v=mesh.vertices[index]; const float xyz[]{v.x,v.y,v.z};
                for(int axis=0;axis<3;++axis) assert(xyz[axis]>=batch.minimum[axis] && xyz[axis]<=batch.maximum[axis]);
            }
            if(mesh.indices[i]==768) retainedCrossing=visible;
        }
    }
    assert(originalTriangles==partitionedTriangles && retainedCrossing);
    assert(visibleTriangles<40); // Most of this synthetic district is outside the ceiling field.
    gta2::TileBatch box; box.bounded=true;
    box.minimum[0]=box.minimum[1]=-.1f; box.minimum[2]=1;
    box.maximum[0]=box.maximum[1]=.1f; box.maximum[2]=2;
    const auto projection=gta2::PerspectiveFovLH(1.57079632679f,1,0.1f,10);
    assert(gta2ceiling::RasterBatchInFrustum(box,&projection.m[0][0]));
    for(int axis=0;axis<3;++axis) for(float direction:{-1.0f,1.0f}) {
        auto outside=box;
        outside.minimum[axis]+=20*direction; outside.maximum[axis]+=20*direction;
        assert(!gta2ceiling::RasterBatchInFrustum(outside,&projection.m[0][0]));
    }
    auto crossing=box; crossing.minimum[0]=-20; crossing.maximum[0]=20;
    assert(gta2ceiling::RasterBatchInFrustum(crossing,&projection.m[0][0]));
    const auto oldIndices=mesh.indices;
    mesh.batches[0].indexCount=1;
    assert(!gta2ceiling::PartitionRasterWorld(mesh)); assert(mesh.indices==oldIndices);
    std::cout<<"Raster partitions preserve exact triangles/materials and conservatively cull ceiling/frustum bounds.\n";
}
