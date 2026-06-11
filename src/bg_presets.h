#pragma once
#include <imgui.h>
#include <cmath>
#include <cstring>
#include <cstdint>

// ── Background preset descriptor ──────────────────────────────────────────────
struct BgPreset {
    const char* id;
    const char* label;
    const char* category;
    float       default_speed;
    int         n_colors;            // 0=fixed palette, 1-3=user colors used
    float       dc1[4], dc2[4], dc3[4];  // default colors
};

static const BgPreset g_bg_presets[] = {
    // id              label              category      spd  nc  c1                   c2                   c3
    {"solid",       "Solid Color",     "Solid",      1.f, 1, {1.0f,1.0f,1.0f,1},{0,0,0,0},{0,0,0,0}},
    {"blob",        "Blob Field",      "Organic",    1.f, 3, {0.4f,0.0f,0.8f,1},{0.0f,0.8f,1.0f,1},{1.0f,0.1f,0.5f,1}},
    {"lava",        "Lava Lamp",       "Organic",    0.4f,2, {0.9f,0.2f,0.0f,1},{0.1f,0.0f,0.0f,1},{0,0,0,0}},
    {"aurora",      "Aurora",          "Organic",    0.5f,3, {0.0f,0.8f,0.5f,1},{0.0f,0.3f,0.9f,1},{0.5f,0.0f,0.8f,1}},
    {"plasma",      "Plasma",          "Organic",    1.f, 3, {0.8f,0.0f,0.5f,1},{0.0f,0.5f,1.0f,1},{1.0f,0.8f,0.0f,1}},
    {"oilslick",    "Oil Slick",       "Organic",    0.3f,0, {0,0,0,0},{0,0,0,0},{0,0,0,0}},
    {"inkdrop",     "Ink Drop",        "Organic",    0.6f,3, {0.1f,0.0f,0.3f,1},{0.0f,0.4f,0.8f,1},{0.8f,0.0f,0.4f,1}},
    {"gradsweep",   "Gradient Sweep",  "Gradient",   0.4f,2, {0.0f,0.0f,0.0f,1},{0.2f,0.0f,0.8f,1},{0,0,0,0}},
    {"sunset",      "Sunset",          "Gradient",   0.2f,0, {0,0,0,0},{0,0,0,0},{0,0,0,0}},
    {"neonflood",   "Neon Flood",      "Gradient",   0.5f,3, {0.0f,0.0f,0.0f,1},{1.0f,0.0f,0.8f,1},{0.0f,1.0f,0.8f,1}},
    {"colorpulse",  "Color Pulse",     "Gradient",   0.5f,3, {0.1f,0.0f,0.2f,1},{0.0f,0.1f,0.4f,1},{0.2f,0.0f,0.1f,1}},
    {"spotlight",   "Spotlight",       "Gradient",   0.3f,2, {0.0f,0.0f,0.0f,1},{1.0f,0.9f,0.7f,1},{0,0,0,0}},
    {"prism",       "Prism Split",     "Gradient",   0.2f,3, {1.0f,0.1f,0.1f,1},{0.1f,0.5f,1.0f,1},{0.1f,1.0f,0.4f,1}},
    {"gridpulse",   "Grid Pulse",      "Geometric",  1.f, 2, {0.08f,0.08f,0.08f,1},{0.3f,0.3f,0.3f,1},{0,0,0,0}},
    {"stripes",     "Stripes",         "Geometric",  1.f, 2, {0.05f,0.05f,0.05f,1},{0.2f,0.2f,0.2f,1},{0,0,0,0}},
    {"rings",       "Concentric Rings","Geometric",  1.f, 2, {0.0f,0.0f,0.0f,1},{0.8f,0.8f,0.8f,1},{0,0,0,0}},
    {"tunnel",      "Tunnel",          "Geometric",  1.f, 2, {0.0f,0.0f,0.0f,1},{0.9f,0.9f,0.9f,1},{0,0,0,0}},
    {"vwgrid",      "Vaporwave Grid",  "Geometric",  0.5f,3, {0.05f,0.0f,0.15f,1},{0.9f,0.1f,0.6f,1},{0.1f,0.8f,1.0f,1}},
    {"checker",     "Checkerboard",    "Geometric",  0.8f,2, {0.08f,0.08f,0.08f,1},{0.18f,0.18f,0.18f,1},{0,0,0,0}},
    {"hex",         "Hex Grid",        "Geometric",  0.7f,2, {0.05f,0.05f,0.05f,1},{0.2f,0.8f,0.6f,1},{0,0,0,0}},
    {"halftone",    "Halftone",        "Geometric",  1.f, 2, {0.05f,0.05f,0.05f,1},{0.9f,0.9f,0.9f,1},{0,0,0,0}},
    {"filmgrain",   "Film Grain",      "Retro",      2.f, 0, {0,0,0,0},{0,0,0,0},{0,0,0,0}},
    {"tvstatic",    "TV Static",       "Retro",      4.f, 0, {0,0,0,0},{0,0,0,0},{0,0,0,0}},
    {"crt",         "CRT Scanlines",   "Retro",      1.f, 2, {0.05f,0.05f,0.1f,1},{0.1f,0.1f,0.2f,1},{0,0,0,0}},
    {"vhstracking", "VHS Tracking",    "Retro",      0.6f,2, {0.0f,0.0f,0.0f,1},{0.6f,0.6f,0.7f,1},{0,0,0,0}},
    {"glitchblk",   "Glitch Blocks",   "Retro",      2.f, 3, {0.0f,0.0f,0.0f,1},{0.0f,1.0f,0.5f,1},{1.0f,0.0f,0.5f,1}},
    {"matrix",      "Matrix Rain",     "Retro",      1.5f,0, {0,0,0,0},{0,0,0,0},{0,0,0,0}},
    {"starfield",   "Starfield",       "Particles",  0.5f,2, {0.0f,0.0f,0.0f,1},{1.0f,1.0f,1.0f,1},{0,0,0,0}},
    {"rain",        "Rain",            "Particles",  2.f, 2, {0.02f,0.04f,0.06f,1},{0.6f,0.8f,1.0f,1},{0,0,0,0}},
    {"snow",        "Snow",            "Particles",  0.8f,2, {0.05f,0.07f,0.12f,1},{1.0f,1.0f,1.0f,1},{0,0,0,0}},
    {"confetti",    "Confetti",        "Particles",  1.f, 3, {1.0f,0.2f,0.3f,1},{0.2f,0.6f,1.0f,1},{1.0f,0.9f,0.0f,1}},
    {"bubbles",     "Bubbles",         "Particles",  0.5f,2, {0.02f,0.04f,0.08f,1},{0.3f,0.7f,1.0f,1},{0,0,0,0}},
    {"fire",        "Fire",            "Particles",  2.f, 0, {0,0,0,0},{0,0,0,0},{0,0,0,0}},
    {"waveform",    "Waveform",        "Abstract",   1.f, 2, {0.04f,0.04f,0.04f,1},{0.0f,1.0f,0.6f,1},{0,0,0,0}},
    {"ripple",      "Ripple",          "Abstract",   0.8f,2, {0.02f,0.02f,0.05f,1},{0.3f,0.6f,1.0f,1},{0,0,0,0}},
    {"breathing",   "Breathing",       "Abstract",   0.4f,2, {0.02f,0.02f,0.05f,1},{0.2f,0.0f,0.6f,1},{0,0,0,0}},
    {"clocksweep",  "Clock Sweep",     "Abstract",   0.5f,3, {0.8f,0.0f,0.3f,1},{0.0f,0.5f,1.0f,1},{1.0f,0.8f,0.0f,1}},
    {"noisedrift",  "Noise Drift",     "Abstract",   0.5f,3, {0.1f,0.0f,0.2f,1},{0.0f,0.2f,0.5f,1},{0.3f,0.0f,0.2f,1}},
    {"lightning",   "Lightning",       "Abstract",   1.f, 2, {0.02f,0.02f,0.05f,1},{0.7f,0.8f,1.0f,1},{0,0,0,0}},
};
static const int g_n_bg_presets = (int)(sizeof(g_bg_presets)/sizeof(g_bg_presets[0]));

inline const BgPreset* bg_preset_by_id(const char* id) {
    for (int i = 0; i < g_n_bg_presets; ++i)
        if (strcmp(g_bg_presets[i].id, id) == 0) return &g_bg_presets[i];
    return nullptr;
}

// ── Drawing ───────────────────────────────────────────────────────────────────

inline void draw_bg_preset(const char* id,
                            ImDrawList* dl, ImVec2 p, float w, float h,
                            float t, float speed, float intensity,
                            const float c1[4], const float c2[4], const float c3[4])
{
    if (!id || !id[0]) return;
    t *= speed;

    float x0 = p.x, y0 = p.y, x1 = p.x+w, y1 = p.y+h;

    // helpers
    auto rgba = [](float r, float g, float b, float a) -> ImU32 {
        r = r<0?0:(r>1?1:r); g = g<0?0:(g>1?1:g);
        b = b<0?0:(b>1?1:b); a = a<0?0:(a>1?1:a);
        return IM_COL32((int)(r*255),(int)(g*255),(int)(b*255),(int)(a*255));
    };
    auto from4 = [&](const float c[4], float am=1.f) -> ImU32 {
        return rgba(c[0],c[1],c[2],c[3]*am*intensity);
    };
    auto lerp4col = [&](const float a[4], const float b[4], float f, float am=1.f) -> ImU32 {
        return rgba(a[0]+(b[0]-a[0])*f, a[1]+(b[1]-a[1])*f,
                    a[2]+(b[2]-a[2])*f, (a[3]+(b[3]-a[3])*f)*am*intensity);
    };
    auto hf = [](int s) -> float {          // deterministic float from seed
        uint32_t x = (uint32_t)s * 2654435761u;
        x ^= x>>16; x *= 0x45d9f3b; x ^= x>>16;
        return (float)(x & 0xFFFFFF) / 16777216.f;
    };

    // ── Solid ─────────────────────────────────────────────────────────────────
    if (!strcmp(id,"solid")) {
        // Flat fill of c1 — alpha straight from the picker (not scaled by
        // intensity) so a translucent wash over lower tracks works too.
        // speed/intensity intentionally unused: nothing animates.
        dl->AddRectFilled({x0,y0},{x1,y1}, rgba(c1[0],c1[1],c1[2],c1[3]));
    }
    // ── Organic ───────────────────────────────────────────────────────────────
    else if (!strcmp(id,"blob")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.9f));
        for (int i = 0; i < 5; ++i) {
            float bx = x0 + w*(0.2f+0.6f*hf(i*7))   + sinf(t*0.37f+i*1.3f)*w*0.25f;
            float by = y0 + h*(0.2f+0.6f*hf(i*7+1))  + cosf(t*0.29f+i*2.1f)*h*0.25f;
            float br = w*(0.25f+0.15f*hf(i*7+2));
            const float* col = (i%3==0)?c1:(i%3==1)?c2:c3;
            for (int r = (int)br; r > 0; r -= (int)(br*0.08f)+1) {
                float a = (1.f-(float)r/br)*0.18f*intensity;
                dl->AddCircleFilled({bx,by},(float)r, rgba(col[0],col[1],col[2],a));
            }
        }
    }
    else if (!strcmp(id,"lava")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c2,0.95f));
        for (int i = 0; i < 4; ++i) {
            float phase = hf(i)*6.28f;
            float bx = x0 + w*(0.2f+0.6f*hf(i*3+11));
            float by = y0 + h*fmodf(0.15f+hf(i*3+12) + t*(0.04f+hf(i*3+13)*0.03f) + 100.f, 1.f);
            float br = h*(0.18f+0.1f*sinf(t*0.5f+phase));
            for (int r = (int)br; r > 0; r -= (int)(br*0.08f)+1) {
                float a = (1.f-(float)r/br)*0.35f*intensity;
                float f = (float)r/br;
                dl->AddCircleFilled({bx,by},(float)r,
                    rgba(c1[0]*f+0.05f*(1-f), c1[1]*f*0.3f, c1[2]*f*0.1f, a));
            }
        }
    }
    else if (!strcmp(id,"aurora")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, rgba(0,0,0,intensity));
        int bands = 6;
        for (int b = 0; b < bands; ++b) {
            float yc = y0 + h*(0.1f+0.8f*(float)b/(bands-1)) + sinf(t*0.4f+b*1.1f)*h*0.06f;
            float bh = h*0.12f*(0.5f+0.5f*sinf(t*0.6f+b*0.7f));
            float f  = (float)b/(bands-1);
            const float* col = (f<0.5f) ? (b%2==0?c1:c2) : (b%2==0?c2:c3);
            float a = 0.18f*intensity*(0.4f+0.6f*sinf(t*0.9f+b));
            for (float dy = -bh; dy < bh; dy += 1.5f) {
                float aa = a*(1.f-fabsf(dy/bh));
                float sx = sinf(t*0.3f+b*0.5f+(y0+yc+dy)*0.003f)*w*0.15f;
                dl->AddLine({x0+sx,yc+dy},{x1+sx,yc+dy},
                    rgba(col[0],col[1],col[2],aa));
            }
        }
    }
    else if (!strcmp(id,"plasma")) {
        // Bilinear interpolation via AddRectFilledMultiColor — smooth even at low grid res.
        int gx = 32, gy = (int)(gx*h/w+0.5f);
        float cw = w/gx, ch = h/gy;
        auto plasma_col = [&](int ix, int iy) -> ImU32 {
            float fx=(float)ix/gx, fy=(float)iy/gy;
            float v = sinf(fx*8+t)*0.25f + sinf(fy*6+t*1.3f)*0.25f
                    + sinf((fx+fy)*5+t*0.7f)*0.25f
                    + sinf(sqrtf((fx-0.5f)*(fx-0.5f)+(fy-0.5f)*(fy-0.5f))*9+t)*0.25f;
            v = v*0.5f+0.5f;
            float r,g,b;
            if (v<0.333f){float f=v/0.333f;r=c1[0]+(c2[0]-c1[0])*f;g=c1[1]+(c2[1]-c1[1])*f;b=c1[2]+(c2[2]-c1[2])*f;}
            else if(v<0.667f){float f=(v-0.333f)/0.333f;r=c2[0]+(c3[0]-c2[0])*f;g=c2[1]+(c3[1]-c2[1])*f;b=c2[2]+(c3[2]-c2[2])*f;}
            else{float f=(v-0.667f)/0.333f;r=c3[0]+(c1[0]-c3[0])*f;g=c3[1]+(c1[1]-c3[1])*f;b=c3[2]+(c1[2]-c3[2])*f;}
            return rgba(r,g,b,intensity);
        };
        for (int iy=0;iy<gy;++iy) for (int ix=0;ix<gx;++ix)
            dl->AddRectFilledMultiColor({x0+ix*cw,y0+iy*ch},{x0+(ix+1)*cw,y0+(iy+1)*ch},
                plasma_col(ix,iy), plasma_col(ix+1,iy), plasma_col(ix+1,iy+1), plasma_col(ix,iy+1));
    }
    else if (!strcmp(id,"oilslick")) {
        int gx=40, gy=(int)(40.f*h/w+1);
        float cw=w/gx, ch=h/gy;
        auto oil_col = [&](int ix, int iy) -> ImU32 {
            float fx=(float)ix/gx, fy=(float)iy/gy;
            float angle=atan2f(fy-0.5f,fx-0.5f);
            float hue=fmodf((angle/(2*3.14159f)+0.5f+t*0.12f+(fx+fy)*0.3f),1.f);
            float r,g,b; int hi=(int)(hue*6); float f=hue*6-hi;
            switch(hi%6){
                case 0:r=1;g=f;b=0;break; case 1:r=1-f;g=1;b=0;break;
                case 2:r=0;g=1;b=f;break; case 3:r=0;g=1-f;b=1;break;
                case 4:r=f;g=0;b=1;break; default:r=1;g=0;b=1-f;break;
            }
            return rgba(r*0.6f,g*0.6f,b*0.6f,intensity);
        };
        for (int iy=0;iy<gy;++iy) for (int ix=0;ix<gx;++ix)
            dl->AddRectFilledMultiColor({x0+ix*cw,y0+iy*ch},{x0+(ix+1)*cw,y0+(iy+1)*ch},
                oil_col(ix,iy), oil_col(ix+1,iy), oil_col(ix+1,iy+1), oil_col(ix,iy+1));
    }
    else if (!strcmp(id,"inkdrop")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        for (int i = 0; i < 8; ++i) {
            float phase = hf(i)*20.f;
            float cycle = fmodf(t*0.3f + phase, 4.f);
            if (cycle > 2.5f) continue;
            float prog = cycle/2.5f;
            float bx = x0 + w*hf(i*5+1);
            float by = y0 + h*hf(i*5+2);
            float br = prog * w * 0.5f;
            const float* col = (i%3==0)?c1:(i%3==1)?c2:c3;
            float a = (1.f-prog)*0.4f*intensity;
            for (int r=(int)br; r>0; r-=(int)(br*0.06f)+1)
                dl->AddCircleFilled({bx,by},(float)r, rgba(col[0],col[1],col[2],a*(float)r/br));
        }
    }
    // ── Gradient ──────────────────────────────────────────────────────────────
    else if (!strcmp(id,"gradsweep")) {
        float ang = t*0.4f;
        float ca = cosf(ang), sa = sinf(ang);
        float cx = (x0+x1)*0.5f, cy = (y0+y1)*0.5f;
        // four corners
        auto grad_col = [&](float fx, float fy) -> ImU32 {
            float proj = (fx-0.5f)*ca + (fy-0.5f)*sa;
            float f = proj*1.5f+0.5f; f=f<0?0:(f>1?1:f);
            return lerp4col(c1,c2,f);
        };
        dl->AddRectFilledMultiColor({x0,y0},{x1,y1},
            grad_col(0,0),grad_col(1,0),grad_col(1,1),grad_col(0,1));
        (void)cx;(void)cy;
    }
    else if (!strcmp(id,"sunset")) {
        float phase = fmodf(t*0.08f,1.f);
        // warm palette cycles
        float r1=0.05f,g1=0.01f,b1=0.15f;
        float r2=0.9f+0.1f*sinf(phase*6.28f), g2=0.3f*sinf(phase*6.28f+0.5f), b2=0.05f;
        float r3=1.f, g3=0.5f+0.2f*sinf(phase*6.28f), b3=0.05f;
        dl->AddRectFilledMultiColor({x0,y0},{x1,y1*0.5f},
            rgba(r1,g1,b1,intensity),rgba(r1,g1,b1,intensity),
            rgba(r2,g2,b2,intensity),rgba(r2,g2,b2,intensity));
        dl->AddRectFilledMultiColor({x0,y1*0.5f},{x1,y1},
            rgba(r2,g2,b2,intensity),rgba(r2,g2,b2,intensity),
            rgba(r3,g3,b3,intensity),rgba(r3,g3,b3,intensity));
    }
    else if (!strcmp(id,"neonflood")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float beat = 0.5f+0.5f*sinf(t*1.5f);
        float glow = w*0.4f*beat*intensity;
        // edges radiating inward
        for (float r = glow; r > 0; r -= w*0.025f) {
            float a = (1.f-r/glow)*0.12f;
            ImU32 col2 = rgba(c2[0],c2[1],c2[2],a);
            ImU32 col3 = rgba(c3[0],c3[1],c3[2],a);
            dl->AddRectFilledMultiColor({x0,y0},{x0+r,y1},col2,rgba(0,0,0,0),rgba(0,0,0,0),col2);
            dl->AddRectFilledMultiColor({x1-r,y0},{x1,y1},rgba(0,0,0,0),col3,col3,rgba(0,0,0,0));
            dl->AddRectFilledMultiColor({x0,y0},{x1,y0+r},col2,col2,rgba(0,0,0,0),rgba(0,0,0,0));
            dl->AddRectFilledMultiColor({x0,y1-r},{x1,y1},rgba(0,0,0,0),rgba(0,0,0,0),col3,col3);
        }
    }
    else if (!strcmp(id,"colorpulse")) {
        float f = fmodf(t*0.4f,3.f);
        ImU32 col;
        if (f<1.f)      col = lerp4col(c1,c2,f);
        else if (f<2.f) col = lerp4col(c2,c3,f-1.f);
        else            col = lerp4col(c3,c1,f-2.f);
        dl->AddRectFilled({x0,y0},{x1,y1},col);
    }
    else if (!strcmp(id,"spotlight")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float scx = x0 + w*(0.5f+0.35f*sinf(t*0.4f));
        float scy = y0 + h*(0.5f+0.3f*cosf(t*0.31f));
        float sr = w*0.55f;
        for (float r = sr; r > 0; r -= w*0.02f) {
            float a = (1.f-r/sr)*(1.f-r/sr)*0.25f*intensity;
            dl->AddCircleFilled({scx,scy},r, rgba(c2[0],c2[1],c2[2],a));
        }
    }
    else if (!strcmp(id,"prism")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, rgba(0.02f,0.02f,0.02f,intensity));
        float cx2 = (x0+x1)*0.5f, cy2 = (y0+y1)*0.5f;
        float off = w*0.18f;
        float bx[3]={cx2-off+sinf(t*0.3f)*off,cx2+off+cosf(t*0.25f)*off,cx2+sinf(t*0.22f+1.f)*off*0.5f};
        float by2[3]={cy2+cosf(t*0.28f)*off,cy2+sinf(t*0.2f+1.f)*off,cy2-off+cosf(t*0.33f)*off};
        float br2 = w*0.38f;
        const float* cols[3]={c1,c2,c3};
        for (int i=0;i<3;++i) for (float r=br2;r>0;r-=w*0.025f) {
            float a=(1.f-r/br2)*0.14f*intensity;
            dl->AddCircleFilled({bx[i],by2[i]},r,rgba(cols[i][0],cols[i][1],cols[i][2],a));
        }
    }
    // ── Geometric ─────────────────────────────────────────────────────────────
    else if (!strcmp(id,"gridpulse")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float scale = 1.f+0.15f*sinf(t*1.2f);
        float spacing = w*0.045f*scale;  // tighter grid — was 0.08f
        float ox = fmodf(t*20.f,spacing);
        for (float gx=x0-spacing+ox; gx<x1+spacing; gx+=spacing)
            dl->AddLine({gx,y0},{gx,y1}, from4(c2,0.35f));
        float oy = fmodf(t*20.f*h/w,spacing);
        for (float gy=y0-spacing+oy; gy<y1+spacing; gy+=spacing)
            dl->AddLine({x0,gy},{x1,gy}, from4(c2,0.35f));
    }
    else if (!strcmp(id,"stripes")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float sw = w*0.12f;
        float diag = w+h;
        float off = fmodf(t*60.f, sw*2);
        for (float d=-diag; d<diag*2; d+=sw*2) {
            float dd = d+off;
            ImVec2 pts[4] = {{x0+dd,y0},{x0+dd+sw,y0},{x0+dd+sw-h,y1},{x0+dd-h,y1}};
            dl->AddConvexPolyFilled(pts,4,from4(c2,0.25f));
        }
    }
    else if (!strcmp(id,"rings")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float cx2=(x0+x1)*0.5f, cy2=(y0+y1)*0.5f;
        float maxr = sqrtf(w*w+h*h)*0.7f;
        float period = w*0.12f;
        float phase = fmodf(t*40.f,period);
        for (float r=phase; r<maxr; r+=period) {
            float a = (1.f-r/maxr)*0.7f*intensity;
            dl->AddCircle({cx2,cy2},r, from4(c2,a),64,1.5f);
        }
    }
    else if (!strcmp(id,"tunnel")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float cx2=(x0+x1)*0.5f, cy2=(y0+y1)*0.5f;
        float zoom = fmodf(t*0.5f,1.f);
        int steps = 18;
        for (int i=steps;i>=0;--i) {
            float f = ((float)i/steps+zoom);
            f = fmodf(f,1.f);
            float sz = f*(w*0.75f);
            float a = (i%2==0?0.6f:0.0f)*intensity;
            dl->AddRect({cx2-sz,cy2-sz*h/w},{cx2+sz,cy2+sz*h/w}, from4(c2,a),0,0,1.5f);
        }
    }
    else if (!strcmp(id,"vwgrid")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        // horizon line
        float hz = y0+h*0.48f;
        dl->AddRectFilled({x0,y0},{x1,hz}, rgba(c1[0]*0.5f,c1[1]*0.3f,c1[2]*0.8f,intensity));
        // vertical lines (perspective)
        float scroll = fmodf(t*0.4f,1.f);
        int nvlines = 12;
        for (int i=0; i<=nvlines; ++i) {
            float fx = (float)i/nvlines;
            float tx = x0 + w*fx;
            float bx2 = (x0+x1)*0.5f + (tx-(x0+x1)*0.5f)*0.05f;
            dl->AddLine({bx2,hz},{tx,y1}, from4(c2,0.6f),1.f);
        }
        // horizontal lines (foreshortened)
        int nhlines = 10;
        for (int i=0; i<nhlines; ++i) {
            float f = (float)(i+scroll)/nhlines;
            float ff = f*f;
            float gy = hz + ff*(y1-hz);
            float gw = w*(0.05f+0.95f*ff);
            float a = ff*0.7f*intensity;
            dl->AddLine({(x0+x1)*0.5f-gw*0.5f,gy},{(x0+x1)*0.5f+gw*0.5f,gy}, from4(c3,a));
        }
    }
    else if (!strcmp(id,"checker")) {
        float csz = w*0.046f;  // smaller tiles — less blocky
        float rnd  = csz*0.18f; // slight rounding so edges feel soft
        float ox = fmodf(t*20.f,csz*2), oy = fmodf(t*20.f*h/w,csz*2);
        int nx=(int)(w/csz)+3, ny=(int)(h/csz)+3;
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        for (int iy=0;iy<ny;++iy) for (int ix=0;ix<nx;++ix) {
            if ((ix+iy)%2==0) {
                float fx=x0-csz+ix*csz-ox, fy=y0-csz+iy*csz-oy;
                dl->AddRectFilled({fx,fy},{fx+csz,fy+csz}, from4(c2,0.75f), rnd);
            }
        }
    }
    else if (!strcmp(id,"hex")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float hr = w*0.030f;  // ~32px at 1080p — recognizable hex without bathroom-tile scale
        float hx_step = hr*1.732f, hy_step = hr*1.5f;
        float wave_t = t*2.f;
        int nx=(int)(w/hx_step)+3, ny=(int)(h/hy_step)+3;
        for (int iy=-1;iy<ny;++iy) for (int ix=-1;ix<nx;++ix) {
            float cx2 = x0+ix*hx_step+(iy%2?hx_step*0.5f:0);
            float cy2 = y0+iy*hy_step;
            float dist = sqrtf((cx2-(x0+x1)*0.5f)*(cx2-(x0+x1)*0.5f)+(cy2-(y0+y1)*0.5f)*(cy2-(y0+y1)*0.5f));
            float pulse = 0.4f+0.4f*sinf(wave_t-dist*0.015f);
            ImVec2 pts[6];
            for (int k=0;k<6;++k) {
                float a2=k*3.14159f/3.f+3.14159f/6.f;
                pts[k]={cx2+cosf(a2)*hr*0.88f, cy2+sinf(a2)*hr*0.88f};
            }
            dl->AddConvexPolyFilled(pts,6, from4(c2,pulse*0.5f));
            dl->AddPolyline(pts,6, from4(c2,pulse*0.3f),ImDrawFlags_Closed,0.8f);
        }
    }
    else if (!strcmp(id,"halftone")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float spacing = w*0.035f;  // ~38px at 1080p — actual halftone scale
        float maxr = spacing*0.45f;
        int nx=(int)(w/spacing)+2, ny=(int)(h/spacing)+2;
        for (int iy=0;iy<ny;++iy) for (int ix=0;ix<nx;++ix) {
            float cx2=x0+ix*spacing, cy2=y0+iy*spacing;
            float dist=sqrtf((cx2-(x0+x1)*0.5f)*(cx2-(x0+x1)*0.5f)+(cy2-(y0+y1)*0.5f)*(cy2-(y0+y1)*0.5f));
            float pulse=0.3f+0.7f*fabs(sinf(t*1.5f-dist*0.02f));
            dl->AddCircleFilled({cx2,cy2},maxr*pulse, from4(c2,0.8f));
        }
    }
    // ── Retro ─────────────────────────────────────────────────────────────────
    else if (!strcmp(id,"filmgrain")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, rgba(0.05f,0.04f,0.03f,intensity));
        int gw=180, gh=(int)(180.f*h/w+1);  // ~6px cells at 1080p — reads as grain not blocks
        float cw2=w/gw, ch2=h/gh;
        int frame=(int)(t*24)&0xFFF;
        for (int iy=0;iy<gh;++iy) for (int ix=0;ix<gw;++ix) {
            float v=hf(ix*100+iy*7+frame*3001)*0.5f+0.05f;
            dl->AddRectFilled({x0+ix*cw2,y0+iy*ch2},{x0+(ix+1)*cw2,y0+(iy+1)*ch2},
                rgba(v,v*0.96f,v*0.92f,intensity));
        }
    }
    else if (!strcmp(id,"tvstatic")) {
        // Horizontal scanlines — more authentic TV static look than square blocks.
        int frame=(int)(t*30)&0xFFFF;
        int nlines=(int)(h/2.f)+1;
        for (int il=0;il<nlines;++il) {
            float fy=y0+il*2.f;
            // Per-line brightness with coarser band modulation on top
            float fine=hf(il*13+frame*4999);
            float band=hf(il/6+frame*997)*0.4f;
            float v=fine*0.7f+band*0.3f;
            dl->AddLine({x0,fy+0.5f},{x1,fy+0.5f},rgba(v,v,v,intensity),2.f);
        }
        // Occasional rolling bright band
        float by=y0+fmodf(t*h*0.25f,h);
        dl->AddRectFilled({x0,by},{x1,fminf(y1,by+h*0.04f)},rgba(1.f,1.f,1.f,intensity*0.12f));
    }
    else if (!strcmp(id,"crt")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float scroll = fmodf(t*30.f,h);
        for (float sy=y0; sy<y1; sy+=3.f) {
            float d=fmodf(sy-y0+scroll,h);
            float a=(d<h*0.1f?0.3f:0.08f)*intensity;
            dl->AddLine({x0,sy},{x1,sy}, rgba(0,0,0,a));
        }
        // glow tint
        dl->AddRectFilledMultiColor({x0,y0},{x1,y1},
            from4(c2,0.1f),from4(c2,0.1f),from4(c2,0.05f),from4(c2,0.05f));
    }
    else if (!strcmp(id,"vhstracking")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        // occasional tracking bands
        int nbands=3;
        for (int i=0;i<nbands;++i) {
            float phase=hf(i*13+7)*6.28f;
            float yb = y0+h*fmodf(hf(i*7)*0.9f + t*(0.1f+hf(i*7+1)*0.15f) + 100.f, 1.f);
            float bh2=h*(0.015f+0.02f*hf(i*7+2));
            float a=(0.4f+0.4f*sinf(t*3.f+phase))*intensity;
            float shift=(hf(i*7+3)-0.5f)*w*0.06f;
            dl->AddRectFilled({x0+shift,yb},{x1+shift,yb+bh2}, from4(c2,a));
        }
    }
    else if (!strcmp(id,"glitchblk")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        int frame=(int)(t*20)&0xFFFF;
        int nblocks=12;
        for (int i=0;i<nblocks;++i) {
            float seed=(float)(i+frame*37+i*frame);
            if (hf((int)seed)>0.35f) continue;
            float gx2=x0+hf(i*7+frame%50)*w, gy2=y0+hf(i*7+1+frame%50)*h;
            float gw2=w*0.05f+hf(i*7+2)*w*0.25f, gh2=h*0.02f+hf(i*7+3)*h*0.08f;
            const float* col=(i%3==0)?c1:(i%3==1)?c2:c3;
            dl->AddRectFilled({gx2,gy2},{gx2+gw2,gy2+gh2}, rgba(col[0],col[1],col[2],intensity*0.8f));
        }
    }
    else if (!strcmp(id,"matrix")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, rgba(0.f,0.05f,0.f,intensity));
        int ncols=30;
        float colw=w/ncols;
        static const char* chars="01アイウエオカキクケコ";
        int nch=(int)strlen(chars);
        float fsz=colw*1.1f; if(fsz<8)fsz=8;
        ImFont* f=ImGui::GetFont();
        for (int col=0;col<ncols;++col) {
            float spd=0.5f+hf(col*7)*1.f;
            float head_y=fmodf(t*spd*h*0.4f+hf(col*3)*h, h+h*0.3f)-h*0.3f;
            int nshow=(int)(h*0.4f/fsz)+2;
            for (int row=0;row<nshow;++row) {
                float cy2=head_y-row*fsz;
                if (cy2<y0-fsz||cy2>y1) continue;
                float bright=(row==0)?1.f:(1.f-(float)row/nshow)*0.8f;
                float a=bright*intensity;
                char ch[2]={chars[(col*17+row*7+(int)(t*10+col))%nch],0};
                float cx2=x0+col*colw+colw*0.2f;
                if(row==0) dl->AddText(f,fsz,{cx2,cy2},rgba(0.8f,1.f,0.8f,a),ch);
                else dl->AddText(f,fsz,{cx2,cy2},rgba(0.f,bright*0.9f,0.f,a),ch);
            }
        }
    }
    // ── Particles ─────────────────────────────────────────────────────────────
    else if (!strcmp(id,"starfield")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        int nstars=120;
        float cx2=(x0+x1)*0.5f, cy2=(y0+y1)*0.5f;
        for (int i=0;i<nstars;++i) {
            float layer=hf(i*3+2);
            float angle=hf(i*3)*6.28318f;
            float dist_norm=hf(i*3+1);
            float speed2=(0.3f+layer*0.7f)*0.08f;
            float d=fmodf(dist_norm+t*speed2, 1.f);
            float sx=cx2+cosf(angle)*d*w*0.7f;
            float sy=cy2+sinf(angle)*d*h*0.7f;
            if (sx<x0||sx>x1||sy<y0||sy>y1) continue;
            float sr2=(1.f+layer)*1.5f*d*d;
            float a=(0.4f+0.6f*layer)*d*d*intensity;
            dl->AddCircleFilled({sx,sy},sr2, from4(c2,a));
        }
    }
    else if (!strcmp(id,"rain")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        int ndrops=60;
        for (int i=0;i<ndrops;++i) {
            float rx=x0+hf(i*5)*w;
            float speed2=0.4f+hf(i*5+1)*0.8f;
            float len=h*(0.04f+hf(i*5+2)*0.06f);
            float ry=y0+fmodf(hf(i*5+3)+t*speed2, 1.f)*h;
            float a=(0.3f+0.4f*hf(i*5+4))*intensity;
            dl->AddLine({rx,ry},{rx,ry+len}, from4(c2,a),0.8f);
        }
    }
    else if (!strcmp(id,"snow")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        int nflakes=80;
        for (int i=0;i<nflakes;++i) {
            float speed2=0.04f+hf(i*4)*0.08f;
            float sway=sinf(t*0.5f+hf(i*4)*6.28f)*0.015f;
            float fx=fmodf(hf(i*4+1)+sway, 1.f);
            float fy=fmodf(hf(i*4+2)+t*speed2, 1.f);
            float sr2=1.5f+hf(i*4+3)*2.5f;
            float a=(0.4f+0.4f*hf(i*4+3))*intensity;
            dl->AddCircleFilled({x0+fx*w,y0+fy*h},sr2, from4(c2,a));
        }
    }
    else if (!strcmp(id,"confetti")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, rgba(0.05f,0.05f,0.05f,intensity));
        int npieces=60;
        for (int i=0;i<npieces;++i) {
            float speed2=0.12f+hf(i*6)*0.2f;
            float sway=sinf(t*1.2f+hf(i*6)*6.28f)*0.03f;
            float fx=fmodf(hf(i*6+1)+sway, 1.f);
            float fy=fmodf(hf(i*6+2)+t*speed2, 1.f);
            float pw2=w*0.012f+hf(i*6+3)*w*0.018f;
            float ph2=pw2*0.5f;
            float rot=t*2.f*hf(i*6+4)*6.28f+hf(i*6+5)*6.28f;
            float ca=cosf(rot),sa=sinf(rot);
            float cx2=x0+fx*w, cy2=y0+fy*h;
            ImVec2 pts[4];
            float hs[4]={-1,-1,1,1}, vs[4]={-1,1,1,-1};
            for(int k=0;k<4;k++){
                float lx=hs[k]*pw2*0.5f, ly=vs[k]*ph2*0.5f;
                pts[k]={cx2+lx*ca-ly*sa, cy2+lx*sa+ly*ca};
            }
            const float* col=(i%3==0)?c1:(i%3==1)?c2:c3;
            dl->AddConvexPolyFilled(pts,4, rgba(col[0],col[1],col[2],intensity));
        }
    }
    else if (!strcmp(id,"bubbles")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        int nb=35;
        for (int i=0;i<nb;++i) {
            float speed2=0.04f+hf(i*5)*0.06f;
            float sway=sinf(t*0.7f+hf(i*5)*6.28f)*0.02f;
            float fx=fmodf(hf(i*5+1)+sway, 1.f);
            float fy=1.f-fmodf(hf(i*5+2)+t*speed2, 1.f);
            float br2=w*0.01f+hf(i*5+3)*w*0.025f;
            float a=(0.2f+0.3f*hf(i*5+4))*intensity;
            float bx2=x0+fx*w, by2=y0+fy*h;
            dl->AddCircle({bx2,by2},br2, from4(c2,a),24,1.f);
            dl->AddCircleFilled({bx2,by2},br2*0.3f, from4(c2,a*0.3f));
        }
    }
    else if (!strcmp(id,"fire")) {
        // Bilinear fire: compute heat at each corner, blend across cells smoothly.
        int gw=60, gh=75;
        float cw2=w/gw, ch2=h/gh;
        dl->AddRectFilled({x0,y0},{x1,y1}, rgba(0.f,0.f,0.f,intensity));
        int frame=(int)(t*30)&0xFFF;
        auto fire_col = [&](int ix, int iy) -> ImU32 {
            float fy=(float)(gh-iy)/gh;  // 1=top (cool), 0=bottom (hot)
            float noise=hf(ix+iy*gw+frame*777+ix*(frame&63))*0.5f+
                        hf((ix+1)%gw+iy*gw+frame*313)*0.3f+
                        hf(ix+((iy+1)%gh)*gw+frame*541)*0.2f;
            float heat=noise*(1.f-fy*fy)-fy*0.3f;
            heat=heat<0?0:(heat>1?1:heat);
            float r=fminf(1.f,heat*2.5f), g=fmaxf(0.f,fminf(1.f,heat*1.2f-0.2f));
            return rgba(r,g,0.f,intensity);
        };
        for (int iy=0;iy<gh;++iy) for (int ix=0;ix<gw;++ix)
            dl->AddRectFilledMultiColor({x0+ix*cw2,y0+iy*ch2},{x0+(ix+1)*cw2,y0+(iy+1)*ch2},
                fire_col(ix,iy), fire_col(ix+1,iy), fire_col(ix+1,iy+1), fire_col(ix,iy+1));
    }
    // ── Abstract ──────────────────────────────────────────────────────────────
    else if (!strcmp(id,"waveform")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        int nbars=48;
        float bw=w/nbars*0.7f;
        float gap=w/nbars*0.3f;
        float max_bh=h*0.6f;
        for (int i=0;i<nbars;++i) {
            float bh2=max_bh*(0.2f+0.8f*fabs(sinf(t*3.f*(1.f+hf(i*3)*0.5f)+i*0.4f)));
            float bx2=x0+i*(bw+gap)+gap*0.5f;
            float by2=(y0+y1)*0.5f;
            dl->AddRectFilled({bx2,by2-bh2*0.5f},{bx2+bw,by2+bh2*0.5f}, from4(c2,0.9f));
        }
    }
    else if (!strcmp(id,"ripple")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float cx2=(x0+x1)*0.5f, cy2=(y0+y1)*0.5f;
        float maxr=sqrtf(w*w+h*h)*0.6f;
        float period=w*0.14f;
        float phase=fmodf(t*50.f,period);
        for (float r=phase;r<maxr;r+=period) {
            float a=(1.f-r/maxr)*0.6f*intensity;
            dl->AddCircle({cx2,cy2},r, from4(c2,a),80,1.5f);
        }
        // second source
        float cx3=x0+w*0.3f,cy3=y0+h*0.35f;
        float phase2=fmodf(t*40.f+period*0.5f,period);
        for (float r=phase2;r<maxr*0.6f;r+=period) {
            float a=(1.f-r/(maxr*0.6f))*0.3f*intensity;
            dl->AddCircle({cx3,cy3},r, from4(c2,a),64,1.f);
        }
    }
    else if (!strcmp(id,"breathing")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float pulse=0.5f+0.5f*sinf(t*1.2f);
        float cx2=(x0+x1)*0.5f, cy2=(y0+y1)*0.5f;
        float maxr=w*0.55f;
        for (float r=maxr;r>0;r-=w*0.025f) {
            float norm=r/maxr;
            float f=norm*pulse;
            float a=f*(1.f-norm)*1.5f*intensity;
            dl->AddCircleFilled({cx2,cy2},r, from4(c2,a));
        }
    }
    else if (!strcmp(id,"clocksweep")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, rgba(0.02f,0.02f,0.02f,intensity));
        float cx2=(x0+x1)*0.5f, cy2=(y0+y1)*0.5f;
        float rad=fminf(w,h)*0.52f;
        int nseg=12;
        const float* cols3[3]={c1,c2,c3};
        for (int i=0;i<nseg;++i) {
            float a1=(float)i/nseg*6.28318f+t*0.7f;
            float a2=(float)(i+1)/nseg*6.28318f+t*0.7f;
            float pulse=0.5f+0.5f*sinf(t*2.f+i*0.8f);
            const float* col=cols3[i%3];
            int nv=16+2;
            ImVec2 fan[18];
            fan[0]={cx2,cy2};
            for (int k=1;k<nv;++k) {
                float a3=a1+(a2-a1)*(float)(k-1)/(nv-2);
                fan[k]={cx2+cosf(a3)*rad,cy2+sinf(a3)*rad};
            }
            dl->AddConvexPolyFilled(fan,nv, rgba(col[0],col[1],col[2],pulse*0.4f*intensity));
        }
    }
    else if (!strcmp(id,"noisedrift")) {
        int gw=24, gh=(int)(24.f*h/w+1);
        float cw2=w/gw, ch2=h/gh;
        for (int iy=0;iy<gh;++iy) for (int ix=0;ix<gw;++ix) {
            float fx=(float)ix/gw, fy=(float)iy/gh;
            float n=sinf(fx*5.f+t*0.7f)*0.25f+sinf(fy*4.f+t*0.5f)*0.25f
                   +sinf((fx*2.f+fy*3.f)*3.f+t*0.9f)*0.25f+sinf(sqrtf(fx*fx+fy*fy)*6+t*0.6f)*0.25f;
            n=n*0.5f+0.5f;
            float r,g,b2;
            if(n<0.5f){float f=n*2; r=c1[0]+(c2[0]-c1[0])*f; g=c1[1]+(c2[1]-c1[1])*f; b2=c1[2]+(c2[2]-c1[2])*f;}
            else{float f=(n-0.5f)*2; r=c2[0]+(c3[0]-c2[0])*f; g=c2[1]+(c3[1]-c2[1])*f; b2=c2[2]+(c3[2]-c2[2])*f;}
            dl->AddRectFilled({x0+ix*cw2,y0+iy*ch2},{x0+(ix+1)*cw2+1,y0+(iy+1)*ch2+1},
                rgba(r,g,b2,intensity));
        }
    }
    else if (!strcmp(id,"lightning")) {
        dl->AddRectFilled({x0,y0},{x1,y1}, from4(c1,0.95f));
        float cycle=fmodf(t*0.8f,3.f);
        if (cycle<0.12f) {
            // flash
            float flash=(0.12f-cycle)/0.12f;
            dl->AddRectFilled({x0,y0},{x1,y1}, from4(c2,flash*0.3f));
            // bolt
            int nbolt=12;
            float bx2=x0+w*(0.3f+hf((int)(t*10)*3)*0.4f);
            float by2=y0;
            ImVec2 prev={bx2,by2};
            for (int i=1;i<=nbolt;++i) {
                float f=(float)i/nbolt;
                float nx2=bx2+(hf(i*7+(int)(t*10)*100)-0.5f)*w*0.4f;
                float ny2=y0+f*h;
                dl->AddLine(prev,{nx2,ny2}, from4(c2,flash*intensity),2.f);
                // branch
                if (i%3==0 && hf(i*3+(int)(t*10)*50)>0.5f) {
                    float blen=h*0.12f;
                    float bangle=(hf(i*11+(int)(t*10)*77)-0.5f)*3.14159f*0.6f;
                    dl->AddLine({nx2,ny2},{nx2+cosf(bangle)*blen,ny2+sinf(bangle)*blen},
                        from4(c2,flash*0.5f*intensity),1.f);
                }
                prev={nx2,ny2};
            }
        }
    }
}
