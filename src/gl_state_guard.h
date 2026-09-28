#pragma once
// GLStateGuard — snapshot/restore of the GL state a foreign renderer sharing
// the app's context may change (Skia for Script clips, src/script_gpu.cpp).
// Construct before handing the context over, destroy after it flushed; the
// app's renderers then see exactly the state they left. GL thread only.
#include "gl_compat.h"

class GLStateGuard {
public:
    static constexpr int kUnits = 8;  // texture units restored (app uses < 8)

    GLStateGuard() {
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex_);
        for (int i = 0; i < kUnits; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d_[i]);
            glGetIntegerv(GL_SAMPLER_BINDING, &sampler_[i]);
        }
        glActiveTexture((GLenum)active_tex_);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buf_);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buf_);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buf_);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo_);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo_);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &rbo_);
        glGetIntegerv(GL_VIEWPORT, viewport_);
        glGetIntegerv(GL_SCISSOR_BOX, scissor_);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color_);
        glGetBooleanv(GL_COLOR_WRITEMASK, color_mask_);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask_);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb_);
        glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb_);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a_);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a_);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb_);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_eq_a_);
        glGetFloatv(GL_BLEND_COLOR, blend_color_);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &stencil_mask_);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_align_);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_len_);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_px_);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows_);
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align_);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_len_);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_skip_px_);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_skip_rows_);
        for (int i = 0; i < kCaps; ++i) caps_on_[i] = glIsEnabled(kCapList[i]);
    }

    ~GLStateGuard() {
        for (int i = 0; i < kCaps; ++i) {
            if (caps_on_[i]) glEnable(kCapList[i]);
            else glDisable(kCapList[i]);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_align_);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_len_);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_px_);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows_);
        glPixelStorei(GL_PACK_ALIGNMENT, pack_align_);
        glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_len_);
        glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_px_);
        glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows_);
        glStencilMask((GLuint)stencil_mask_);
        glBlendColor(blend_color_[0], blend_color_[1], blend_color_[2], blend_color_[3]);
        glBlendEquationSeparate((GLenum)blend_eq_rgb_, (GLenum)blend_eq_a_);
        glBlendFuncSeparate((GLenum)blend_src_rgb_, (GLenum)blend_dst_rgb_,
                            (GLenum)blend_src_a_, (GLenum)blend_dst_a_);
        glDepthMask(depth_mask_);
        glColorMask(color_mask_[0], color_mask_[1], color_mask_[2], color_mask_[3]);
        glClearColor(clear_color_[0], clear_color_[1], clear_color_[2], clear_color_[3]);
        glScissor(scissor_[0], scissor_[1], scissor_[2], scissor_[3]);
        glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
        glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)rbo_);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo_);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpack_buf_);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buf_);
        glBindVertexArray((GLuint)vao_);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)array_buf_);
        glUseProgram((GLuint)program_);
        for (int i = 0; i < kUnits; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, (GLuint)tex2d_[i]);
            glBindSampler((GLuint)i, (GLuint)sampler_[i]);
        }
        glActiveTexture((GLenum)active_tex_);
    }

    GLStateGuard(const GLStateGuard&) = delete;
    GLStateGuard& operator=(const GLStateGuard&) = delete;

private:
    static constexpr int kCaps = 10;
    static constexpr GLenum kCapList[kCaps] = {
        GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
        GL_FRAMEBUFFER_SRGB, GL_MULTISAMPLE, GL_DITHER, GL_PRIMITIVE_RESTART,
        GL_POLYGON_OFFSET_FILL,
    };
    GLint active_tex_ = 0, tex2d_[kUnits] = {}, sampler_[kUnits] = {};
    GLint program_ = 0, vao_ = 0, array_buf_ = 0, pack_buf_ = 0, unpack_buf_ = 0;
    GLint draw_fbo_ = 0, read_fbo_ = 0, rbo_ = 0;
    GLint viewport_[4] = {}, scissor_[4] = {};
    GLfloat clear_color_[4] = {}, blend_color_[4] = {};
    GLboolean color_mask_[4] = {}, depth_mask_ = GL_TRUE;
    GLint blend_src_rgb_ = 0, blend_dst_rgb_ = 0, blend_src_a_ = 0, blend_dst_a_ = 0;
    GLint blend_eq_rgb_ = 0, blend_eq_a_ = 0, stencil_mask_ = 0;
    GLint unpack_align_ = 4, unpack_row_len_ = 0, unpack_skip_px_ = 0, unpack_skip_rows_ = 0;
    GLint pack_align_ = 4, pack_row_len_ = 0, pack_skip_px_ = 0, pack_skip_rows_ = 0;
    GLboolean caps_on_[kCaps] = {};
};
