// metal_render_test.mm — offscreen verification of the Metal RenderSurface.
// Renders one frame into a BGRA8 texture and writes a PNG. macOS host test;
// no simulator/device needed (metal_render.mm is Apple-portable).
#import <Metal/Metal.h>
#include "../src/metal_render.h"
#include <vector>
#include <cstdio>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../vendor/stb_image_write.h"

int main(int argc, char** argv) {
    @autoreleasepool {
        int W = 270, H = 480;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 2; }

        metal_render_init((__bridge void*)dev);

        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

        int rc = metal_render_frame((__bridge void*)tex, W, H, 1.6);
        if (rc != 0) { fprintf(stderr, "render rc=%d\n", rc); return 3; }

        // blit-sync managed texture, then read back
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl synchronizeResource:tex]; [bl endEncoding];
        [cb commit]; [cb waitUntilCompleted];

        std::vector<uint8_t> bgra((size_t)W*H*4), rgb((size_t)W*H*3);
        [tex getBytes:bgra.data() bytesPerRow:W*4
           fromRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0];
        for (size_t i=0;i<(size_t)W*H;i++){ rgb[i*3+0]=bgra[i*4+2]; rgb[i*3+1]=bgra[i*4+1]; rgb[i*3+2]=bgra[i*4+0]; }
        const char* out = argc>1 ? argv[1] : "/tmp/metal_test.png";
        stbi_write_png(out, W, H, 3, rgb.data(), W*3);
        printf("wrote %s (%dx%d)\n", out, W, H);
    }
    return 0;
}
