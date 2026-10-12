// Original 822478C0 chooses TexEnvMode02 for two contiguous texture IDs.
// after-02 event3 expands UV1's lower-right atlas region over the whole output,
// while UV0 spans its entire texture. Both stages in the .fp use c0, not c1.
static void darknessFixedCompositeContract(WorldRendererD3D11& renderer) {
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=28;
    geometry->formats[0]=3;geometry->formats[1]=geometry->formats[2]=2;
    geometry->vertices.resize(4*28);geometry->indices={0,1,2,0,2,3};
    const float positions[4][3]{{-1,-1,.5f},{-1,1,.5f},{1,1,.5f},{1,-1,.5f}};
    const float uvs[4][2]{{0,1},{0,0},{1,0},{1,1}};
    // Captured minima: half an original 1720x720 texel beyond the atlas midpoint.
    const float atlasMin[2]{std::bit_cast<float>(0x3f00130du),std::bit_cast<float>(0x3f002d83u)};
    for(unsigned v=0;v<4;++v) {
        auto* p=geometry->vertices.data()+v*28;
        for(unsigned c=0;c<3;++c)put(p+c*4,std::bit_cast<uint32_t>(positions[v][c]));
        for(unsigned c=0;c<2;++c) {
            put(p+12+c*4,std::bit_cast<uint32_t>(uvs[v][c]));
            put(p+20+c*4,std::bit_cast<uint32_t>(atlasMin[c]+(1-atlasMin[c])*uvs[v][c]));
        }
    }
    using Rgba=std::array<uint8_t,4>;
    const Rgba sceneColors[4]{{208,96,144,255},{48,224,160,160},{112,64,240,80},{240,176,32,0}};
    const Rgba atlasColors[4]{{64,192,224,64},{224,112,80,255},{160,240,48,0},{96,48,192,192}};
    auto scene=std::make_shared<ColorImage>();scene->width=scene->height=2;
    for(const auto& color:sceneColors)scene->pixels.insert(scene->pixels.end(),color.begin(),color.end());
    auto atlas=std::make_shared<ColorImage>();atlas->width=atlas->height=4;
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
        const Rgba color=x>=2 && y>=2?atlasColors[(y-2)*2+x-2]:Rgba{7,251,13,17};
        atlas->pixels.insert(atlas->pixels.end(),color.begin(),color.end());
    }
    WorldDraw draw;draw.geometry={geometry,geometry,0,6};
    draw.material=WorldMaterial::fixed;draw.fragmentName="MRenderXenon_Attrib_TexEnvMode02";
    draw.textureMask=worldFragmentTextureMask(draw.fragmentName,0);
    require(draw.textureMask==3,"Fixed composite omitted an original texture binding");
    draw.options.modes.fill(4);draw.options.modes[0]=draw.options.modes[1]=0;
    draw.options.coordinates[1]=1;
    for(unsigned c=0;c<4;++c)draw.constants.vectors[c][c]=1;
    draw.constants.references[0][2]=10;
    draw.textures[0]=scene;draw.textures[1]=atlas;
    for(unsigned slot=0;slot<2;++slot) {
        draw.samplers[slot].valid=true;draw.samplers[slot].address.fill(2);
    }
    // Event3 writes RGBA, with alpha ALWAYS and framebuffer blending disabled.
    put(draw.attributes.data()+92,0x01100210);draw.attributes[97]=8;
    draw.attributes[144]=2;draw.attributes[145]=1;
    struct Case {EngineVector c0,color;};
    const Case cases[]{
        {{0,0,0,0},{1,1,1,1}}, // Exact captured modulation, white vertex constant.
        {{0,0,0,0},{.75f,.5f,.875f,.625f}},
        {{1,1,1,1},{.75f,.5f,.875f,.625f}},
        {{.25f,.5f,.75f,.375f},{.75f,.5f,.875f,.625f}}
    };
    auto half=[](uint16_t h) {unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    unsigned checks=0;
    for(auto extent:{std::array<uint32_t,2>{64,64},{128,48},{256,72}}) {
        draw.viewport={0,0,extent[0],extent[1]};draw.targets={2301+extent[0],0,0,0,0};
        WorldClear clear;clear.viewport=draw.viewport;clear.targets=draw.targets;clear.flags=1;
        clear.color={.125f,.25f,.375f,.875f};
        for(unsigned test=0;test<std::size(cases);++test) {
            draw.constants.vectors[10]=cases[test].color;draw.fragmentConstants[0]=cases[test].c0;
            // Deliberately disagree: the original second lrp still reads c0.
            for(unsigned c=0;c<4;++c)draw.fragmentConstants[1][c]=1-cases[test].c0[c];
            renderer.clear(clear);
            require(renderer.draw(draw),"Original two-stage fixed composite rejected");
            const auto pixels=renderer.readSurface(draw.targets[0],false);
            require(pixels.size()==size_t(extent[0])*extent[1]*8,"Fixed composite output extent changed");
            // Check every pixel, including all four corners: a quarter-screen
            // quad, shared UVs, reversed V or reading outside the atlas must fail.
            for(unsigned y=0;y<extent[1];++y)for(unsigned x=0;x<extent[0];++x) {
                const double u=(x+.5)/extent[0],v=(y+.5)/extent[1];
                const unsigned sceneIndex=(v>=.5?2:0)+(u>=.5?1:0);
                const unsigned ax=unsigned((atlasMin[0]+(1-atlasMin[0])*u)*4);
                const unsigned ay=unsigned((atlasMin[1]+(1-atlasMin[1])*v)*4);
                require(ax>=2 && ax<4 && ay>=2 && ay<4,"Fixed composite oracle left the atlas region");
                const Rgba samples[]{sceneColors[sceneIndex],atlasColors[(ay-2)*2+ax-2]};
                std::array<double,4> expected{};
                for(unsigned c=0;c<4;++c)expected[c]=cases[test].color[c];
                // .fp lines22..32: lrp(D.a,R,D) in RGB, preserve R.a;
                // then lrp(c0,that,R*D), independently for both samples.
                for(const auto& sample:samples)for(unsigned c=0;c<4;++c) {
                    const double texel=sample[c]/255.0,alpha=sample[3]/255.0;
                    const double mixed=c==3?expected[c]:alpha*expected[c]+(1-alpha)*texel;
                    const double weight=cases[test].c0[c];
                    expected[c]=weight*mixed+(1-weight)*expected[c]*texel;
                }
                uint16_t pixel[4];std::memcpy(pixel,pixels.data()+(size_t(y)*extent[0]+x)*8,8);
                for(unsigned c=0;c<4;++c) {
                    const double actual=half(pixel[c]);
                    if(!std::isfinite(actual) || std::abs(actual-expected[c])>=.002) {
                        std::fprintf(stderr,"DarknessFixedComposite extent=%ux%u case=%u pixel=%u,%u lane=%u actual=%g expected=%g\n",
                            extent[0],extent[1],test,x,y,c,actual,expected[c]);
                        throw std::runtime_error("Two-stage atlas composite differs from original TexEnvMode02");
                    }
                    ++checks;
                }
            }
        }
    }
    for(unsigned slot=0;slot<2;++slot) {
        auto missing=draw;missing.textures[slot].reset();
        require(!renderer.draw(missing),"Fixed composite accepted a missing required texture");
    }
    std::printf("DarknessFixedComposite: %u full-output RGBA checks, distinct atlas UVs, modulation and mixed c0 passed.\n",checks);
}

// Original Fulton Darkness-voice capture: constant bits,
// distinct UV streams and sampler addressing; normalized geometry and synthetic
// finite textures keep the independent ARB oracle distributable.
// Inputs: baseline-fulton-issue98-20261012-01/trace/owned-effect-1.jsonl draws0..3.
static void capturedVoiceShaderContract(WorldRendererD3D11& renderer) {
    constexpr const char* captured[]{
        "0000403f0000803e0000403f0000803ecdcc4c3b610bb63b000000003d860d3c0000803f000040400000a0400000e04000001041000030410000504100007041458c813e9c38003f458c813e0000803f5c49543d5c49543d5c49543d0000803ff09ea070000000000000c07f0000c0ff000020c000003041000000003d860d3c",
        "0000403f0000803e0000403f0000803ecdcc4c3b610bb63b00000000f4552a3d0000803f000040400000a0400000e04000001041000030410000504100007041c373873e8a10013fc373873e0000803fee807f3eee807f3eee807f3e0000803ff09ea07060dddbd60000c07f0000c0ff000020c00000304100000000f4552a3d",
        "0000403f0000803e0000403f0000803ecdcc4c3b610bb63b00000000dc55853d0000803f000040400000a0400000e04000001041000030410000504100007041b6aa8b3eacaa013fb6aa8b3e0000803fca00c83eca00c83eca00c83e0000803ff09ea070000000000000c07f0000c0ff000020c00000304100000000dc55853d",
        "0000403f0000803e0000403f0000803ecdcc4c3b610bb63b00000000966b813e0000803f000040400000a0400000e040000010410000304100005041000070410e4cad3e9478063f0e4cad3e0000803f6121c23f6121c23f6121c23f0000803ff09ea07060dddbd60000c07f0000c0ff000020c00000304100000000966b813e"
    };
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=28;
    geometry->formats[0]=3;geometry->formats[1]=geometry->formats[2]=2;
    geometry->vertices.resize(112);geometry->indices={0,1,3,3,1,2};
    constexpr float positions[4][3]{{1,-1,.5f},{-1,-1,.5f},{-1,1,.5f},{1,1,.5f}};
    constexpr float uv0[4][2]{{1,.5f},{.5f,.5f},{.5f,0},{1,0}};
    constexpr float uv1[4][2]{{1,1},{0,1},{0,0},{1,0}};
    for(unsigned v=0;v<4;++v) {
        auto* bytes=geometry->vertices.data()+v*28;
        for(unsigned c=0;c<3;++c)put(bytes+c*4,std::bit_cast<uint32_t>(positions[v][c]));
        for(unsigned c=0;c<2;++c) {
            put(bytes+12+c*4,std::bit_cast<uint32_t>(uv0[v][c]));
            put(bytes+20+c*4,std::bit_cast<uint32_t>(uv1[v][c]));
        }
    }
    auto image=[](const std::array<uint8_t,16>& red) {
        auto result=std::make_shared<ColorImage>();result->width=result->height=4;
        for(auto value:red)result->pixels.insert(result->pixels.end(),{value,value,value,255});
        return result;
    };
    constexpr std::array<uint8_t,16> mask0{32,64,96,128, 64,96,128,160, 96,128,160,192, 128,160,192,224};
    constexpr std::array<uint8_t,16> mask1{224,192,160,128, 192,160,128,96, 160,128,96,64, 128,96,64,32};
    constexpr std::array<uint8_t,16> noise{16,80,144,240, 48,112,208,176, 96,224,64,128, 232,24,160,56};
    auto scene=std::make_shared<ColorImage>();scene->width=scene->height=4;
    constexpr std::array<uint8_t,4> sceneColor{48,96,160,192};
    for(unsigned i=0;i<16;++i)scene->pixels.insert(scene->pixels.end(),sceneColor.begin(),sceneColor.end());
    auto sample=[](const auto& pixels,double u,double v,bool wrap) {
        const double x=u*4-.5,y=v*4-.5;
        const int ix=int(std::floor(x)),iy=int(std::floor(y));
        const double fx=x-ix,fy=y-iy;
        const auto address=[&](int i) {return wrap?unsigned((i%4+4)%4):unsigned(std::clamp(i,0,3));};
        const auto red=[&](int px,int py) {return pixels[address(py)*4+address(px)]/255.0;};
        const double a=red(ix,iy)*(1-fx)+red(ix+1,iy)*fx;
        const double b=red(ix,iy+1)*(1-fx)+red(ix+1,iy+1)*fx;
        return a*(1-fy)+b*fy;
    };
    auto half=[](uint16_t h) {const unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    WorldDraw draw;draw.geometry={geometry,geometry,0,6};
    draw.material=WorldMaterial::post;draw.fragmentName="XREngine_RadialBlurInvert";
    draw.textureMask=worldFragmentTextureMask(draw.fragmentName,0);
    draw.textureIds[0]=12650;draw.textureIds[1]=734;draw.textureIds[2]=735;draw.textureIds[3]=11572;
    draw.textures[0]=scene;draw.textures[1]=image(mask0);draw.textures[2]=image(mask1);draw.textures[3]=image(noise);
    draw.options.modes.fill(4);draw.options.modes[0]=draw.options.modes[1]=0;draw.options.coordinates[1]=1;
    for(unsigned c=0;c<4;++c)draw.constants.vectors[c][c]=1;
    draw.constants.references[0][2]=10;draw.constants.vectors[10]={1,1,1,1};
    draw.attributes[97]=8;put(draw.attributes.data()+92,0x01100000);
    for(unsigned slot=0;slot<4;++slot) {
        auto& s=draw.samplers[slot];s.valid=true;s.minLinear=s.magLinear=s.mipLinear=true;
        s.lodValid=true;s.minLevel=s.maxLevel=0;s.address.fill(slot==3?0:2);s.anisotropy=(slot==1 || slot==2)?16:1;
    }
    unsigned checks=0;
    for(auto extent:{std::array<uint32_t,2>{128,72},{256,144}}) {
        draw.viewport={0,0,extent[0],extent[1]};draw.targets={3301+extent[0],0,0,0,0};
        WorldClear clear;clear.viewport=draw.viewport;clear.targets=draw.targets;clear.flags=1;
        for(unsigned which=0;which<4;++which) {
            auto nibble=[](char c) {return unsigned(c>='a'?c-'a'+10:c-'0');};
            for(unsigned row=0;row<8;++row)for(unsigned lane=0;lane<4;++lane) {
                const auto* hex=captured[which]+(row*4+lane)*8;
                uint32_t bits=0;for(unsigned b=0;b<4;++b)bits|=(nibble(hex[b*2])*16+nibble(hex[b*2+1]))<<(b*8);
                draw.fragmentConstants[row][lane]=std::bit_cast<float>(bits);
            }
            require(std::isnan(draw.fragmentConstants[6][2]) && std::isnan(draw.fragmentConstants[6][3]),
                "Captured unused env6 NaNs lost");
            renderer.clear(clear);require(renderer.draw(draw),"Captured voice shader fixture rejected");
            const auto pixels=renderer.readSurface(draw.targets[0],false);
            require(pixels.size()==size_t(extent[0])*extent[1]*8,"Captured voice shader extent changed");
            const auto& c1=draw.fragmentConstants[1];const auto& c4=draw.fragmentConstants[4];
            const auto& c5=draw.fragmentConstants[5];const auto& c7=draw.fragmentConstants[7];
            std::array<double,4> effect{};
            double invertedGray=0,totalAffection=0;
            for(unsigned c=0;c<3;++c) {
                const double value=sceneColor[c]/255.0;
                invertedGray+=(1-value*value)*c4[c];totalAffection+=c4[c];
            }
            for(unsigned c=0;c<3;++c) {
                const double value=sceneColor[c]/255.0;
                const double gray=c4[3]*invertedGray+(1-c4[3])*(1-value*value);
                effect[c]=(totalAffection-gray)*c5[c]+1-c1[3];
            }
            const double alpha=sceneColor[3]/255.0;
            // DP3 r1.rgb leaves r1.w from the second scene fetch intact.
            effect[3]=(c4[3]*alpha+(1-c4[3])*(1-alpha))*c5[3]-c1[2];
            for(unsigned y=0;y<extent[1];++y)for(unsigned x=0;x<extent[0];++x) {
                const double u=(x+.5)/extent[0],v=(y+.5)/extent[1];
                const double t1=sample(mask0,u,v,false),t2=sample(mask1,u,v,false);
                const double t3=sample(noise,(u-.5)*c7[3],(v-.5)*c7[3],true);
                const double mask=t3*t1+(1-t3)*t2;
                uint16_t rgba[4];std::memcpy(rgba,pixels.data()+(size_t(y)*extent[0]+x)*8,8);
                for(unsigned c=0;c<4;++c) {
                    const double expected=mask*effect[c]+1-mask,actual=half(rgba[c]);
                    if(!std::isfinite(actual) || std::abs(actual-expected)>=.004) {
                        std::fprintf(stderr,"CapturedVoice case=%u pixel=%u,%u lane=%u actual=%g expected=%g mask=%g\n",
                            which,x,y,c,actual,expected,mask);
                        throw std::runtime_error("Captured voice differs from original ARB formula");
                    }
                    if(c<3)require(actual>=1-c1[3]-.004,"Captured voice atlas became black");
                    ++checks;
                }
            }
        }
    }
    std::printf("CapturedVoice: %u ARB-formula checks with captured strengths, UV streams, clamp/wrap and unused NaNs.\n",checks);
}

// The voice effect composites eight scene samples and three independent masks.
// Check actual GPU pixels across the whole output, including wide viewports.
static void darknessEffectContract(WorldRendererD3D11& renderer) {
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=28;
    geometry->formats[0]=3;geometry->formats[1]=geometry->formats[2]=2;
    geometry->vertices.resize(4*28);geometry->indices={0,1,2,0,2,3};
    const float positions[4][3]{{-1,-1,.5f},{-1,1,.5f},{1,1,.5f},{1,-1,.5f}};
    const float uvs[4][2]{{0,1},{0,0},{1,0},{1,1}};
    for(unsigned v=0;v<4;++v) {
        auto* p=geometry->vertices.data()+v*28;
        for(unsigned c=0;c<3;++c)put(p+c*4,std::bit_cast<uint32_t>(positions[v][c]));
        for(unsigned stream=0;stream<2;++stream)for(unsigned c=0;c<2;++c)
            put(p+12+stream*8+c*4,std::bit_cast<uint32_t>(uvs[v][c]));
    }
    auto solid=[](uint8_t value) {
        auto image=std::make_shared<ColorImage>();image->width=image->height=1;
        image->pixels={value,value,value,255};return image;
    };
    auto scene=std::make_shared<ColorImage>();scene->width=scene->height=2;
    scene->pixels={32,64,96,255, 128,160,192,255, 64,128,224,255, 224,96,32,255};
    WorldDraw draw;draw.geometry={geometry,geometry,0,6};
    draw.material=WorldMaterial::post;draw.fragmentName="XREngine_RadialBlurInvert";
    draw.textureMask=worldFragmentTextureMask(draw.fragmentName,0);
    require(draw.textureMask==15,"Darkness effect capture omitted a scene/mask binding");
    draw.options.modes.fill(4);draw.options.modes[0]=draw.options.modes[1]=0;
    draw.options.coordinates[1]=1;
    for(unsigned c=0;c<4;++c)draw.constants.vectors[c][c]=1;
    draw.constants.references[0][2]=10;draw.constants.vectors[10]={1,1,1,1};
    draw.attributes[97]=8;put(draw.attributes.data()+92,0x01100000);
    draw.fragmentConstants[0]={.5f,.5f,0,0};
    draw.fragmentConstants[1]={0,0,0,1}; // zero streak, full strength
    draw.fragmentConstants[4]={1,0,0,0}; // preserves each squared RGB lane
    draw.fragmentConstants[5]={1,1,1,1};
    draw.textures[0]=scene;draw.textures[1]=solid(255);draw.textures[2]=solid(0);
    // These are authored bindings, unlike RenderDeathScene0's explicitly
    // unbound mask stages. Losing their pixels must still reject the draw.
    for(unsigned slot=0;slot<4;++slot)draw.textureIds[slot]=uint16_t(0x710+slot);
    // Point sampling makes each quadrant's analytic source color independent
    // of pixel-center offsets and the linear filter's cross-quadrant blend.
    for(unsigned slot=0;slot<4;++slot) {
        draw.samplers[slot].valid=true;draw.samplers[slot].address.fill(2);
    }
    auto half=[](uint16_t h) {unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    unsigned checks=0;
    for(auto extent:{std::array<uint32_t,2>{64,64},{128,48},{256,72}}) {
        draw.viewport={0,0,extent[0],extent[1]};draw.targets={1901+extent[0],0,0,0,0};
        WorldClear clear;clear.viewport=draw.viewport;clear.targets=draw.targets;clear.flags=1;
        for(uint8_t mask:{uint8_t(0),uint8_t(128),uint8_t(255)}) {
            draw.textures[3]=solid(mask);renderer.clear(clear);
            require(renderer.draw(draw),"Original Darkness voice fragment rejected");
            const auto pixels=renderer.readSurface(draw.targets[0],false);
            require(pixels.size()==size_t(extent[0])*extent[1]*8,"Darkness output extent changed");
            for(unsigned quadrant=0;quadrant<4;++quadrant) {
                const unsigned x=extent[0]*(quadrant%2?3:1)/4,y=extent[1]*(quadrant/2?3:1)/4;
                uint16_t pixel[4];std::memcpy(pixel,pixels.data()+(size_t(y)*extent[0]+x)*8,8);
                const double weight=mask/255.0;
                for(unsigned c=0;c<4;++c) {
                    const double color=scene->pixels[quadrant*4+c]/255.0;
                    const double expected=weight*(c==3?0:color*color)+(1-weight);
                    if(std::abs(half(pixel[c])-expected)>=.002) {
                        std::fprintf(stderr,"DarknessEffect extent=%ux%u mask=%u quadrant=%u lane=%u actual=%g expected=%g\n",
                            extent[0],extent[1],unsigned(mask),quadrant,c,half(pixel[c]),expected);
                        throw std::runtime_error("Darkness scene/mask pixel differs from source formula");
                    }
                    ++checks;
                }
            }
        }
    }
    for(unsigned slot=0;slot<4;++slot) {
        auto missing=draw;missing.textures[slot].reset();
        require(!renderer.draw(missing),"Darkness effect accepted a missing required mask");
    }
    std::printf("DarknessEffect: %u quadrant scene/mask RGBA checks at square, ultrawide and 32:9 extents passed.\n",checks);
    darknessFixedCompositeContract(renderer);
    capturedVoiceShaderContract(renderer);
}

// Retail RenderDeathScene0 (8235F390) clears descriptor+184/+186/+188,
// then supplies XREngine_RadialBlurInvert by name pointer0x82064488.
// 822968A8 binds those fields as mask slots1/2/3. Requested ID0
// takes 822569E0 -> 82256EE4 -> 82864F20, whose 82865020 stores object0.
// These are original null bindings, not failed uploads of authored masks.
// The captured original draw declares position3 + UV0float2 only, despite
// stage1 referencing UV1. That absent UV1 drives only the three null masks.
// The original .fp's final LRP chooses white when both masks sample zero;
// the following TexEnvMode02 therefore preserves the resolved scene. Check
// that complete chain at the user's actual 1720x720 guest extent, rather
// than substituting CPU images for either of its GPU-resolved sources.
static void deathSceneContract(WorldRendererD3D11& renderer,unsigned scale) {
    constexpr uint32_t width=1720,height=720,sceneTarget=3901,atlasTarget=3902,outputTarget=3903;
    using Rgba=std::array<uint8_t,4>;
    const Rgba colors[4]{{208,96,144,255},{48,224,160,160},{112,64,240,80},{240,176,32,0}};
    WorldClear clear;clear.viewport={0,0,width,height};clear.flags=1;clear.targets={sceneTarget,0,0,0,0};
    for(unsigned quadrant=0;quadrant<4;++quadrant) {
        const int32_t x=int32_t((quadrant%2)*width/2),y=int32_t((quadrant/2)*height/2);
        clear.rectangle=std::array<int32_t,4>{x,y,x+int32_t(width/2),y+int32_t(height/2)};
        for(unsigned lane=0;lane<4;++lane)clear.color[lane]=colors[quadrant][lane]/255.0f;
        renderer.clear(clear);
    }
    WorldResolve sceneCopy;sceneCopy.targets=clear.targets;sceneCopy.viewport={0,0,width,height};
    sceneCopy.rectangle={0,0,width,height};sceneCopy.destination={3904,0x3904000,width,height,54,0};
    require(renderer.resolve(sceneCopy),"Death scene's original GPU source resolve failed");

    const float positions[4][3]{{-1,-1,.5f},{-1,1,.5f},{1,1,.5f},{1,-1,.5f}};
    const float uvs[4][2]{{0,1},{0,0},{1,0},{1,1}};
    const float deathUvs[4][2]{{.5f,.5f},{0,.5f},{0,0},{.5f,0}};
    // Normalize positions to the fixture's viewport; retain the captured
    // death UV0 values and declaration. Seed/final passes separately supply
    // their two live UV streams, rather than inventing UV1 for the death draw.
    auto makeGeometry=[&](bool secondUv,bool capturedDeathUv) {
        auto result=std::make_shared<StoredGeometry>();
        result->vertexCount=4;result->stride=secondUv?28:20;result->formats[0]=3;
        result->formats[1]=2;if(secondUv)result->formats[2]=2;
        result->vertices.resize(4*result->stride);result->indices={0,1,3,3,1,2};
        for(unsigned vertex=0;vertex<4;++vertex) {
            auto* bytes=result->vertices.data()+vertex*result->stride;
            for(unsigned lane=0;lane<3;++lane)put(bytes+lane*4,std::bit_cast<uint32_t>(positions[vertex][lane]));
            for(unsigned stream=0;stream<(secondUv?2u:1u);++stream)for(unsigned lane=0;lane<2;++lane)
                put(bytes+12+stream*8+lane*4,std::bit_cast<uint32_t>(capturedDeathUv?deathUvs[vertex][lane]:uvs[vertex][lane]));
        }
        return result;
    };
    auto geometry=makeGeometry(false,true);
    WorldDraw death;death.geometry={geometry,geometry,0,6};
    death.targets={atlasTarget,0,0,0,0};death.viewport={width/2,height/2,width/2,height/2};
    death.material=WorldMaterial::post;death.fragmentName="XREngine_RadialBlurInvert";
    death.textureMask=worldFragmentTextureMask(death.fragmentName,0);
    require(death.textureMask==15,"Death pass lost the original four-stage shader usage mask");
    death.options.modes.fill(4);death.options.modes[0]=death.options.modes[1]=0;
    for(unsigned stage=0;stage<8;++stage)death.options.coordinates[stage]=uint8_t(stage);
    for(unsigned lane=0;lane<4;++lane)death.constants.vectors[lane][lane]=1;
    death.constants.references[0][2]=10;death.constants.vectors[10]={1,1,1,1};
    death.attributes[97]=8;put(death.attributes.data()+92,0x01100000);
    // Match the first effect-boundaries death capture's live fragment
    // constants. Unused env6 stays zero instead of copying its garbage bits.
    death.fragmentConstants[0]={.25f,.25f,.25f,.25f};
    death.fragmentConstants[1]={std::bit_cast<float>(0x3b1868c8u),std::bit_cast<float>(0x3bb60b61u),0,std::bit_cast<float>(0x3f3b71ebu)};
    death.fragmentConstants[2]={1,3,5,7};death.fragmentConstants[3]={9,11,13,15};
    death.fragmentConstants[4]={.3f,.3f,.3f,1};
    const float affection=std::bit_cast<float>(0x408c9570u);
    death.fragmentConstants[5]={affection,affection,affection,1};
    death.textureIds[0]=12650;death.textureObjects[0]=sceneCopy.destination;
    for(unsigned slot=0;slot<4;++slot) {
        death.samplers[slot].valid=true;death.samplers[slot].address.fill(2);
    }
    auto white=std::make_shared<ColorImage>();white->width=white->height=1;white->pixels={255,255,255,255};
    auto prior=death;
    auto priorGeometry=makeGeometry(true,false);prior.geometry={priorGeometry,priorGeometry,0,6};
    for(unsigned slot=1;slot<4;++slot) {prior.textureIds[slot]=uint16_t(0x720+slot);prior.textures[slot]=white;}
    require(renderer.draw(prior),"Prior authored mask bindings could not seed the death transition");
    clear.targets=death.targets;clear.rectangle.reset();clear.color={1,0,1,1};renderer.clear(clear);
    for(unsigned slot=1;slot<4;++slot)
        require(!death.textureIds[slot] && !death.textureObjects[slot].key() && !death.textures[slot],
                "Death fixture accidentally supplied a mask or stale resolve key");
    require(renderer.draw(death),"Original death pass rejected its deliberately unbound masks");
    auto half=[](uint16_t h) {const unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    {
        const auto pixels=renderer.readSurface(atlasTarget,false);
        const unsigned physicalWidth=width*scale,physicalHeight=height*scale;
        require(pixels.size()==size_t(physicalWidth)*physicalHeight*8,"Death atlas lost its physical extent");
        for(unsigned y:{physicalHeight/2,3*physicalHeight/4,physicalHeight-1})
            for(unsigned x:{physicalWidth/2,3*physicalWidth/4,physicalWidth-1}) {
                uint16_t rgba[4];std::memcpy(rgba,pixels.data()+(size_t(y)*physicalWidth+x)*8,8);
                for(auto lane:rgba)require(std::isfinite(half(lane)) && std::abs(half(lane)-1)<.002,
                    "Original null masks sampled stale assets or left the death atlas black");
            }
    }
    // A nonzero requested ID/object/storage with no resident image is still a
    // failed binding, never the authored null-stage case above.
    for(unsigned slot=1;slot<4;++slot)for(unsigned kind=0;kind<3;++kind) {
        auto missing=death;
        // Keep every declared live coordinate valid so a UV rejection cannot
        // conceal a missing-resource validation regression.
        missing.geometry={priorGeometry,priorGeometry,0,6};
        if(kind==0)missing.textureIds[slot]=uint16_t(0x730+slot);
        if(kind==1)missing.textureObjects[slot]={3906+slot,0,width,height,54,0};
        if(kind==2)missing.textureObjects[slot]={0,0x3910000+slot*0x1000,width,height,54,0};
        require(!renderer.draw(missing),"Death null-mask exception concealed a missing authored mask binding");
    }
    {
        auto missing=death;missing.textureIds[0]=0;missing.textureObjects[0]={};
        require(!renderer.draw(missing),"Death null-mask exception accepted an absent primary scene");
        missing=death;missing.textureObjects[0].storage+=0x100000;
        require(!renderer.draw(missing),"Death pass concealed an unavailable primary scene resolve");
    }
    {
        auto missing=death;auto missingUv0=std::make_shared<StoredGeometry>(*geometry);
        // A valid positions-only layout isolates the required-UV0 check;
        // retaining the removed stream's stride would fail vertex decoding.
        missingUv0->formats[1]=0;missingUv0->stride=12;missingUv0->vertices.resize(4*12);
        for(unsigned vertex=0;vertex<4;++vertex)for(unsigned lane=0;lane<3;++lane)
            put(missingUv0->vertices.data()+vertex*12+lane*4,std::bit_cast<uint32_t>(positions[vertex][lane]));
        missing.geometry={missingUv0,missingUv0,0,6};
        require(!renderer.draw(missing),"Death null-mask UV1 exception accepted an absent primary UV0");
        // All authored masks are resident, so this exercises the UV1 boundary
        // rather than failing earlier for a missing texture upload.
        auto liveMasks=prior;liveMasks.geometry=death.geometry;
        require(!renderer.draw(liveMasks),"Death absent UV1 was accepted with live authored mask textures");
    }
    WorldResolve atlasCopy;atlasCopy.targets=death.targets;atlasCopy.viewport={0,0,width,height};
    atlasCopy.rectangle={0,0,width,height};atlasCopy.destination={3905,0x3905000,width,height,54,0};
    require(renderer.resolve(atlasCopy),"Death blur atlas resolve failed");
    auto compositeGeometry=makeGeometry(true,false);
    const float atlasMin[2]{std::bit_cast<float>(0x3f00130du),std::bit_cast<float>(0x3f002d83u)};
    for(unsigned vertex=0;vertex<4;++vertex)for(unsigned lane=0;lane<2;++lane)
        put(compositeGeometry->vertices.data()+vertex*28+20+lane*4,
            std::bit_cast<uint32_t>(atlasMin[lane]+(1-atlasMin[lane])*uvs[vertex][lane]));
    auto composite=death;composite.geometry={compositeGeometry,compositeGeometry,0,6};
    composite.targets={outputTarget,0,0,0,0};composite.viewport={0,0,width,height};
    composite.material=WorldMaterial::fixed;composite.fragmentName="MRenderXenon_Attrib_TexEnvMode02";
    composite.textureMask=worldFragmentTextureMask(composite.fragmentName,0);
    composite.textureIds[0]=1;composite.textureIds[1]=6;
    composite.textureObjects[0]=sceneCopy.destination;composite.textureObjects[1]=atlasCopy.destination;
    composite.fragmentConstants={};put(composite.attributes.data()+92,0x01100210);
    composite.attributes[144]=2;composite.attributes[145]=1;
    clear.targets=composite.targets;clear.color={0,0,0,0};renderer.clear(clear);
    require(renderer.draw(composite),"Death scene's final resolved two-texture composite rejected");
    const auto pixels=renderer.readSurface(outputTarget,false);
    const unsigned physicalWidth=width*scale,physicalHeight=height*scale;
    require(pixels.size()==size_t(physicalWidth)*physicalHeight*8,"Death composite lost its native output extent");
    uint64_t checks=0;
    for(unsigned y=0;y<physicalHeight;++y)for(unsigned x=0;x<physicalWidth;++x) {
        const auto& expected=colors[(y>=physicalHeight/2?2:0)+(x>=physicalWidth/2?1:0)];
        uint16_t rgba[4];std::memcpy(rgba,pixels.data()+(size_t(y)*physicalWidth+x)*8,8);
        for(unsigned lane=0;lane<4;++lane) {
            const double actual=half(rgba[lane]);
            // Original format54 stores ten RGB bits and two alpha bits. The
            // final white-atlas modulation preserves those resolved values;
            // expecting the source's eight-bit alpha would misread the console
            // frontbuffer contract (160/255, for example, resolves to 2/3).
            const double maximum=lane==3?3.0:1023.0;
            const double resolved=std::floor(expected[lane]*maximum/255.0+.5)/maximum;
            if(!std::isfinite(actual) || std::abs(actual-resolved)>=.002) {
                std::fprintf(stderr,"DeathScene scale=%u pixel=%u,%u lane=%u actual=%g expected=%g\n",
                    scale,x,y,lane,actual,resolved);
                throw std::runtime_error("Original death chain lost scene brightness, alpha or full-output coverage");
            }
            ++checks;
        }
    }
    std::printf("DeathScene%u: %llu RGBA checks at %ux%u preserve GPU-resolved scene through original null masks and final atlas composite; missing authored bindings stay rejected.\n",
        scale,static_cast<unsigned long long>(checks),physicalWidth,physicalHeight);
}
