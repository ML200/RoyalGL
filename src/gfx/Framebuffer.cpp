#include "gfx/Framebuffer.h"
#include "gfx/GLCheck.h"

namespace RoyalGL
{
    Framebuffer::Framebuffer() { GL_CALL(glCreateFramebuffers(1, &m_fbo)); }

    Framebuffer::~Framebuffer()
    {
        if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
    }

    void Framebuffer::AttachColor(const Texture& tex) const
    {
        GL_CALL(glNamedFramebufferTexture(m_fbo, GL_COLOR_ATTACHMENT0, tex.Id(), 0));
        GL_CALL(glNamedFramebufferDrawBuffer(m_fbo, GL_COLOR_ATTACHMENT0));
    }

    void Framebuffer::Bind() const { GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, m_fbo)); }

    void Framebuffer::BindDefault() { GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0)); }
}
