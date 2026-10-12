#include "renderer/engine/world_mesh.h"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace DarkRecomp::Native;
using DarkRecomp::ColorImage;
static void require(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
static void put(uint8_t* base,uint32_t address,uint32_t value) {
    for(unsigned i=0;i<4;++i)base[address+i]=uint8_t(value>>(24-i*8));
}
struct HeldDraw {
    std::shared_ptr<WorldDraw> draw;
    std::weak_ptr<const StoredGeometry> geometry;
    std::weak_ptr<const ColorImage> image;
    unsigned id=0;
};
// Release a captured draw after main and TLS cleanup, while static objects
// are being destroyed. The recycler must still own its synchronization/storage.
struct ExitCheck {
    std::shared_ptr<WorldDraw> draw;
    std::weak_ptr<const StoredGeometry> geometry;
    ~ExitCheck() {
        draw.reset();
        if(!geometry.expired()) {std::fputs("Draw pool pinned geometry at static teardown\n",stderr);std::abort();}
    }
} exitCheck;
int main() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    try {
        require(base!=nullptr,"Reserve private guest space");
        require(VirtualAlloc(base+0x82A60000,0x10000,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"Commit context");
        require(VirtualAlloc(base+0x01000000,0x10000,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"Commit device");
        constexpr uint32_t context=0x82A69B00;
        base[context+16896+97]=8;
        put(base,context+17160,1280);put(base,context+17164,720);put(base,context+17172,0x3F800000);
        EngineVertexBindingSnapshot binding;
        binding.deviceAddress=0x01000000;binding.descriptor.flags=0x01000000;binding.descriptor.modes.fill(4);
        const auto encoded=encodeEngineVertexDescriptor(binding.descriptor);
        for(unsigned i=0;i<5;++i)binding.key[i+1]=uint32_t(encoded[i*4])<<24|uint32_t(encoded[i*4+1])<<16|
            uint32_t(encoded[i*4+2])<<8|encoded[i*4+3];
        {
            auto vertices=std::make_shared<StoredGeometry>();
            auto indices=std::make_shared<StoredGeometry>();
            vertices->id=41;indices->id=42;
            StoredDraw input{vertices,indices,1,2};input.vertexBindings=binding;
            auto copied=captureWorldDraw(base,input);
            require(copied && input.vertices==vertices && input.indices==indices &&
                    copied->geometry.vertices==vertices && copied->geometry.indices==indices &&
                    input.firstIndex==1 && input.indexCount==2 && input.vertexBindings->key==binding.key,
                    "Const capture consumed or changed its input");
            // Reject after vertex preparation and guest state capture, before
            // publishing geometry. Even a late failure must preserve ownership.
            put(base,context+17160,0);
            auto rejected=captureWorldDraw(base,std::move(input));
            put(base,context+17160,1280);
            require(!rejected && input.vertices==vertices && input.indices==indices &&
                    input.firstIndex==1 && input.indexCount==2 && input.vertexBindings->key==binding.key,
                    "Rejected owned capture consumed or changed its input");
            const std::weak_ptr<const StoredGeometry> weakVertices=vertices,weakIndices=indices;
            auto transferred=captureWorldDraw(base,std::move(input));
            require(transferred && !input.vertices && !input.indices &&
                    transferred->geometry.vertices==vertices && transferred->geometry.indices==indices &&
                    transferred->geometry.firstIndex==1 && transferred->geometry.indexCount==2 &&
                    input.firstIndex==1 && input.indexCount==2 && input.vertexBindings->key==binding.key,
                    "Successful owned capture did not transfer exactly its geometry references");
            copied.reset();vertices.reset();indices.reset();
            require(!weakVertices.expired() && !weakIndices.expired(),"Transferred draw lost its geometry");
            transferred.reset();
            require(weakVertices.expired() && weakIndices.expired(),"Released transferred draw pinned its geometry");
        }
        {
            enableEnginePreview();
            auto geometry=std::make_shared<StoredGeometry>();
            geometry->vertexCount=3;geometry->vertices.resize(36);geometry->indices={0,1,2};
            StoredDraw input{geometry,geometry,0,3};input.vertexBindings=binding;
            put(base,context+17160,0);
            previewObserveWorld(base,std::move(input));
            put(base,context+17160,1280);
            require(input.vertices==geometry && input.indices==geometry,"Rejected owned preview consumed its input");
            const std::weak_ptr<const StoredGeometry> weak=geometry;
            previewObserveWorld(base,std::move(input));
            require(!input.vertices && !input.indices,"Owned preview did not transfer its geometry");
            geometry.reset();previewEndFrame();
            std::vector<DarkRecomp::SimpleMesh> frame;
            require(takePreviewFrame(frame) && frame.size()==1 && frame[0].world &&
                    frame[0].world->geometry.indexCount==3 && !weak.expired(),
                    "Queued transferred draw lost its geometry");
            frame.clear();
            require(weak.expired(),"Preview accounting pinned transferred geometry after release");
        }
        for(unsigned round=0;round<3;++round) {
            std::array<std::vector<HeldDraw>,8> held;
            std::array<std::exception_ptr,8> errors{};
            std::vector<std::jthread> producers;
            for(unsigned thread=0;thread<held.size();++thread)producers.emplace_back([&,thread] {
                try {
                    // More live snapshots than the pool can retain, then
                    // reuse them with new threads after the producers exit.
                    for(unsigned i=0;i<600;++i) {
                        auto geometry=std::make_shared<StoredGeometry>();
                        geometry->id=1+round*4800+thread*600+i;geometry->vertexCount=3;geometry->stride=12;
                        geometry->formats[0]=3;geometry->vertices.resize(36);geometry->indices={0,1,2};
                        StoredDraw input{geometry,geometry,0,3};input.vertexBindings=binding;
                        if(i%31==0) {
                            input.vertexBindings->deviceAddress=0;
                            require(!captureWorldDraw(base,std::move(input)) && input.vertices==geometry &&
                                    input.indices==geometry,"Invalid owned capture consumed its input or was published");
                            input.vertexBindings=binding;
                        }
                        auto draw=captureWorldDraw(base,std::move(input));
                        require(draw && !input.vertices && !input.indices &&
                                draw->geometry.vertices==geometry && draw->geometry.indices==geometry,
                                "Capture changed geometry ownership");
                        require(draw->constants.vectors[255][0]==0 && draw->fragmentConstants[15][0]==0 &&
                                !draw->textureObjects[15].object && !draw->textureIds[15] && !draw->samplers[15].lodValid &&
                                !draw->textures[15] && !draw->fragmentFlags && draw->fragmentName.empty(),
                                "Recycled draw inherited an unused bank or texture");
                        auto image=std::make_shared<ColorImage>();image->width=image->height=1;
                        image->pixels={uint8_t(geometry->id),0,0,255};draw->textures[15]=image;
                        // Poison banks that this depth fixture will not fill.
                        draw->constants.vectors[255][0]=91;draw->fragmentConstants[15][0]=92;
                        draw->textureObjects[15].object=93;draw->textureIds[15]=94;draw->samplers[15].lodValid=true;
                        draw->fragmentFlags=95;draw->fragmentName="retained draw must not be recycled early";
                        held[thread].push_back({std::move(draw),geometry,image,geometry->id});
                    }
                } catch(...) {errors[thread]=std::current_exception();}
            });
            producers.clear(); // Join all producers before validating their retained results.
            for(const auto& error:errors)if(error)std::rethrow_exception(error);
            for(auto& batch:held)for(auto& item:batch) {
                require(item.draw->geometry.vertices->id==item.id && item.draw->textures[15]->pixels[0]==uint8_t(item.id) &&
                        item.draw->constants.vectors[255][0]==91 && item.draw->fragmentConstants[15][0]==92 &&
                        item.draw->textureObjects[15].object==93 && item.draw->fragmentFlags==95 &&
                        item.draw->fragmentName=="retained draw must not be recycled early",
                        "Retained draw changed after its producing thread exited");
                item.draw.reset();
                require(item.geometry.expired() && item.image.expired(),"Pool pinned an image or geometry after draw release");
            }
        }
        {
            auto geometry=std::make_shared<StoredGeometry>();
            StoredDraw input{geometry,geometry,0,3};input.vertexBindings=binding;
            exitCheck.draw=captureWorldDraw(base,input);exitCheck.geometry=geometry;
            require(bool(exitCheck.draw),"Capture retained for static teardown");
        }
        VirtualFree(base,0,MEM_RELEASE);
        std::puts("DrawPoolContract passed: 14,400 retained draws, 24 producer lifetimes, overflow, failed captures, unused bank reset and prompt resource release.");
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"DrawPoolContract failed: %s\n",error.what());
        if(base)VirtualFree(base,0,MEM_RELEASE);return 1;
    }
}
