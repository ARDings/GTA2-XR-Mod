// GTA2Ceiling: per-frame geometry without DrawPrimitiveUP.
//
// Every DrawPrimitiveUP / DrawIndexedPrimitiveUP makes DXVK copy the data into
// an upload buffer of its own, and those are created and freed all the time.
// Under Winlator on the Quest each of them is graphics memory (a dmabuf) of the
// 32-bit game process, and the freed ones were never given back: gta2.exe held
// 20 MB more every second until the headset ran out of memory after a few
// minutes (measured: 0.9 GB to 5.7 GB of dmabufs in four minutes while the D3D9
// resources stayed the same).
//
// So the sprites, the HUD and the field's mask go through two rings created
// once - a vertex and an index buffer - written with D3DLOCK_NOOVERWRITE only:
// nothing is renamed, nothing is allocated after the first frame. A ring holds
// many frames of data, far more than the GPU can be behind, so a region is long
// finished with when it comes round again.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>

namespace gta2 {

class DynamicDraw {
public:
    static DynamicDraw& Instance() {
        static DynamicDraw ring;
        return ring;
    }

    // Before IDirect3DDevice9::Reset and before the device goes away: the
    // rings live in D3DPOOL_DEFAULT.
    void Release() {
        if (vertices_) vertices_->Release();
        if (indices_) indices_->Release();
        vertices_ = nullptr;
        indices_ = nullptr;
        device_ = nullptr;
        vertexAt_ = indexAt_ = 0;
    }

    // Drop-in for DrawPrimitiveUP.
    HRESULT Draw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT primitives, const void* data,
                 UINT stride) {
        const UINT count = VertexCount(type, primitives);
        UINT first = 0;
        if (!device || !count || !PutVertices(device, data, count, stride, &first))
            return device ? device->DrawPrimitiveUP(type, primitives, data, stride) : D3DERR_INVALIDCALL;
        device->SetStreamSource(0, vertices_, 0, stride);
        return device->DrawPrimitive(type, first, primitives);
    }

    // Drop-in for DrawIndexedPrimitiveUP with 16-bit indices.
    HRESULT DrawIndexed(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex, UINT vertexCount,
                        UINT primitives, const uint16_t* indexData, const void* data, UINT stride) {
        const UINT indexCount = VertexCount(type, primitives);
        UINT firstVertex = 0, firstIndex = 0;
        // The vertex data starts at index 0, as with the UP call.
        if (!device || !indexCount || !PutVertices(device, data, minIndex + vertexCount, stride, &firstVertex) ||
            !PutIndices(device, indexData, indexCount, &firstIndex)) {
            return device ? device->DrawIndexedPrimitiveUP(type, minIndex, vertexCount, primitives, indexData,
                                                           D3DFMT_INDEX16, data, stride)
                          : D3DERR_INVALIDCALL;
        }
        device->SetStreamSource(0, vertices_, 0, stride);
        device->SetIndices(indices_);
        return device->DrawIndexedPrimitive(type, static_cast<INT>(firstVertex), minIndex, vertexCount, firstIndex,
                                            primitives);
    }

private:
    static constexpr UINT kVertexBytes = 16u << 20;  // about forty frames of sprites
    static constexpr UINT kIndexBytes = 4u << 20;

    static UINT VertexCount(D3DPRIMITIVETYPE type, UINT primitives) {
        switch (type) {
            case D3DPT_POINTLIST: return primitives;
            case D3DPT_LINELIST: return primitives * 2;
            case D3DPT_LINESTRIP: return primitives + 1;
            case D3DPT_TRIANGLELIST: return primitives * 3;
            case D3DPT_TRIANGLESTRIP:
            case D3DPT_TRIANGLEFAN: return primitives + 2;
            default: return 0;
        }
    }

    bool Ensure(IDirect3DDevice9* device) {
        if (device != device_) {
            Release();
            device_ = device;
        }
        if (failed_) return false;
        if (!vertices_ && FAILED(device->CreateVertexBuffer(kVertexBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                                            D3DPOOL_DEFAULT, &vertices_, nullptr))) {
            vertices_ = nullptr;
            failed_ = true;  // the UP calls keep working, just as before
            return false;
        }
        if (!indices_ && FAILED(device->CreateIndexBuffer(kIndexBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                                          D3DFMT_INDEX16, D3DPOOL_DEFAULT, &indices_, nullptr))) {
            indices_ = nullptr;
            failed_ = true;
            return false;
        }
        return true;
    }

    bool PutVertices(IDirect3DDevice9* device, const void* data, UINT count, UINT stride, UINT* first) {
        const UINT bytes = count * stride;
        if (!data || !stride || bytes == 0 || bytes > kVertexBytes / 8 || !Ensure(device)) return false;
        UINT at = (vertexAt_ + stride - 1) / stride * stride;  // a whole number of vertices in
        if (at + bytes > kVertexBytes) at = 0;
        void* target = nullptr;
        if (FAILED(vertices_->Lock(at, bytes, &target, D3DLOCK_NOOVERWRITE)) || !target) return false;
        std::memcpy(target, data, bytes);
        vertices_->Unlock();
        vertexAt_ = at + bytes;
        *first = at / stride;
        return true;
    }

    bool PutIndices(IDirect3DDevice9* device, const uint16_t* data, UINT count, UINT* first) {
        const UINT bytes = count * 2;
        if (!data || bytes == 0 || bytes > kIndexBytes / 8 || !Ensure(device)) return false;
        UINT at = indexAt_;
        if (at + bytes > kIndexBytes) at = 0;
        void* target = nullptr;
        if (FAILED(indices_->Lock(at, bytes, &target, D3DLOCK_NOOVERWRITE)) || !target) return false;
        std::memcpy(target, data, bytes);
        indices_->Unlock();
        indexAt_ = at + bytes;
        *first = at / 2;
        return true;
    }

    IDirect3DDevice9* device_ = nullptr;
    IDirect3DVertexBuffer9* vertices_ = nullptr;
    IDirect3DIndexBuffer9* indices_ = nullptr;
    UINT vertexAt_ = 0, indexAt_ = 0;
    bool failed_ = false;
};

}  // namespace gta2
