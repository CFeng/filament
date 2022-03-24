/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "OpenGLProgram.h"

#include "OpenGLDriver.h"

#include <utils/Log.h>
#include <utils/compiler.h>
#include <utils/Panic.h>
#include <utils/debug.h>

#include <private/backend/BackendUtils.h>

#include <ctype.h>

namespace filament {

using namespace filament::math;
using namespace utils;
using namespace backend;

static void logCompilationError(utils::io::ostream& out,
        backend::Program::Shader shaderType, const char* name,
        GLuint shaderId) noexcept;

static void logProgramLinkError(utils::io::ostream& out,
        const char* name, GLuint program) noexcept;

OpenGLProgram::OpenGLProgram() noexcept = default;

OpenGLProgram::OpenGLProgram(OpenGLContext& context, Program&& programBuilder) noexcept
        :  HwProgram(programBuilder.getName()) {

    auto samplerGroupInfo = std::move(programBuilder.getSamplerGroupInfo());
    auto uniformBlockInfo = std::move(programBuilder.getUniformBlockInfo());

    // this cannot fail because we check compilation status after linking the program
    // shaders[] is filled with id of shader stages present.
    OpenGLProgram::compileShaders(context, programBuilder.getShadersSource(), gl.shaders);

    // link the program, this also cannot fail because status is checked later.
    // TODO: defer this until beginRenderPass()
    gl.program = OpenGLProgram::linkProgram(gl.shaders);

    // check status of program linking and shader compilation, logs error and free all resources
    // in case of error.
    // TODO: defer this until first use
    bool success = OpenGLProgram::checkProgramStatus(name.c_str_safe(),
            gl.program, gl.shaders);

    // Failing to compile a program can't be fatal, because this will happen a lot in
    // the material tools. We need to have a better way to handle these errors and
    // return to the editor.
    if (UTILS_UNLIKELY(!success)) {
        PANIC_LOG("Failed to compile GLSL program.");
    }

    if (success) {
        // TODO: defer this until first use
        initializeProgramState(context, gl.program,
                uniformBlockInfo, samplerGroupInfo);
    }
}

OpenGLProgram::~OpenGLProgram() noexcept {
    const GLuint program = gl.program;
    UTILS_NOUNROLL
    for (GLuint shader: gl.shaders) {
        if (shader) {
            if (program) {
                glDetachShader(program, shader);
            }
            glDeleteShader(shader);
        }
    }
    if (program) {
        glDeleteProgram(program);
    }
}

/*
 * Compile shaders in the ShaderSource. This cannot fail because compilation failures are not
 * checked until after the program is linked.
 * This always returns the GL shader IDs or zero a shader stage is not present.
 */
void OpenGLProgram::compileShaders(OpenGLContext& context,
        Program::ShaderSource const& shadersSource,
        GLuint shaderIds[Program::SHADER_TYPE_COUNT]) noexcept {

    // build all shaders
    UTILS_NOUNROLL
    for (size_t i = 0; i < Program::SHADER_TYPE_COUNT; i++) {
        Program::Shader type = static_cast<Program::Shader>(i);
        GLenum glShaderType;
        switch (type) {
            case Program::Shader::VERTEX:
                glShaderType = GL_VERTEX_SHADER;
                break;
            case Program::Shader::FRAGMENT:
                glShaderType = GL_FRAGMENT_SHADER;
                break;
        }

        if (UTILS_LIKELY(!shadersSource[i].empty())) {
            Program::ShaderBlob const& shader = shadersSource[i];
            std::string_view shaderView(reinterpret_cast<const char*>(shader.data()), shader.size());
            std::string temp;

            if (!context.ext.GOOGLE_cpp_style_line_directive) {
                // If usages of the Google-style line directive are present, remove them, as some
                // drivers don't allow the quotation marks.
                if (UTILS_UNLIKELY(requestsGoogleLineDirectivesExtension(
                        shaderView.data(), shaderView.size()))) {
                    temp = shaderView; // copy string
                    removeGoogleLineDirectives(temp.data(), temp.size()); // length is unaffected
                    shaderView = temp;
                }
            }

            if (UTILS_UNLIKELY(context.getShaderModel() == ShaderModel::GL_CORE_41 &&
                !context.ext.ARB_shading_language_packing)) {
                // Tragically, OpenGL 4.1 doesn't support unpackHalf2x16 and
                // MacOS doesn't support GL_ARB_shading_language_packing
                if (temp.empty()) {
                    temp = shaderView; // copy string
                }

                std::string unpackHalf2x16{ R"(

// these don't handle denormals, NaNs or inf
float u16tofp32(highp uint v) {
    v <<= 16u;
    highp uint s = v & 0x80000000u;
    highp uint n = v & 0x7FFFFFFFu;
    highp uint nz = n == 0u ? 0u : 0xFFFFFFFF;
    return uintBitsToFloat(s | ((((n >> 3u) + (0x70u << 23))) & nz));
}
vec2 unpackHalf2x16(highp uint v) {
    return vec2(u16tofp32(v&0xFFFFu), u16tofp32(v>>16u));
}
uint fp32tou16(float val) {
    uint f32 = floatBitsToUint(val);
    uint f16 = 0u;
    uint sign = (f32 >> 16) & 0x8000u;
    int exponent = int((f32 >> 23) & 0xFFu) - 127;
    uint mantissa = f32 & 0x007FFFFFu;
    if (exponent > 15) {
        f16 = sign | (0x1Fu << 10);
    } else if (exponent > -15) {
        exponent += 15;
        mantissa >>= 13;
        f16 = sign | uint(exponent << 10) | mantissa;
    } else {
        f16 = sign;
    }
    return f16;
}
highp uint packHalf2x16(vec2 v) {
    highp uint x = fp32tou16(v.x);
    highp uint y = fp32tou16(v.y);
    return (y << 16) | x;
}
)"};
                // a good point for insertion is just before the first occurrence of an uniform block
                auto pos = temp.find("layout(std140)");
                if (pos != std::string_view::npos) {
                    temp.insert(pos, unpackHalf2x16);
                }
                shaderView = temp;
            }

            GLuint shaderId = glCreateShader(glShaderType);
            { // scope for source/length (we don't want them to leak out)
                const char* const source = shaderView.data();
                const GLint length = (GLint)shaderView.length();
                glShaderSource(shaderId, 1, &source, &length);
                glCompileShader(shaderId);
            }

            shaderIds[i] = shaderId;
        }
    }
}

/*
 * Create a program from the given shader IDs and links it. This cannot fail because errors
 * are checked later. This always returns a valid GL program ID (which doesn't mean the
 * program itself is valid).
 */
GLuint OpenGLProgram::linkProgram(const GLuint shaderIds[Program::SHADER_TYPE_COUNT]) noexcept {
    GLuint program = glCreateProgram();
    for (size_t i = 0; i < Program::SHADER_TYPE_COUNT; i++) {
        if (shaderIds[i]) {
            glAttachShader(program, shaderIds[i]);
        }
    }
    glLinkProgram(program);
    return program;
}

/*
 * Checks a program link status and logs errors and frees resources on failure.
 * Returns true on success.
 */
bool OpenGLProgram::checkProgramStatus(const char* name,
        GLuint& program, GLuint shaderIds[backend::Program::SHADER_TYPE_COUNT]) noexcept {

    GLint status;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (UTILS_LIKELY(status == GL_TRUE)) {
        return true;
    }

    // only if the link fails, we check the compilation status
    UTILS_NOUNROLL
    for (size_t i = 0; i < Program::SHADER_TYPE_COUNT; i++) {
        const Program::Shader type = static_cast<Program::Shader>(i);
        const GLuint shader = shaderIds[i];
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (status != GL_TRUE) {
            logCompilationError(slog.e, type, name, shader);
        }
        glDetachShader(program, shader);
        glDeleteShader(shader);
        shaderIds[i] = 0;
    }
    // log the link error as well
    logProgramLinkError(slog.e, name, program);
    glDeleteProgram(program);
    program = 0;
    return false;
}

/*
 * Initializes our internal state from a valid program. This must only be called after
 * checkProgramStatus() has been successfully called.
 */
void OpenGLProgram::initializeProgramState(OpenGLContext& context, GLuint program,
        Program::UniformBlockInfo const& uniformBlockInfo,
        Program::SamplerGroupInfo const& samplerGroupInfo) noexcept {

    // Associate each UniformBlock in the program to a known binding.
    UTILS_NOUNROLL
    for (GLuint binding = 0, n = uniformBlockInfo.size(); binding < n; binding++) {
        auto const& name = uniformBlockInfo[binding];
        if (!name.empty()) {
            GLint index = glGetUniformBlockIndex(program, name.c_str());
            if (index >= 0) {
                glUniformBlockBinding(program, GLuint(index), binding);
            }
            CHECK_GL_ERROR(utils::slog.e)
        }
    }

    auto& indicesRun = mIndicesRuns;
    uint8_t numUsedBindings = 0;
    uint8_t tmu = 0;

    UTILS_NOUNROLL
    for (size_t i = 0, c = samplerGroupInfo.size(); i < c; i++) {
        auto const& groupInfo = samplerGroupInfo[i];
        auto const& samplers = groupInfo.samplers;
        if (!samplers.empty()) {

            // keep this in the loop so we skip it in the rare case a program doesn't have
            // sampler. The context cache will prevent repeated calls to GL.
            context.useProgram(program);

            // Cache the sampler uniform locations for each interface block
            BlockInfo& info = mBlockInfos[numUsedBindings];
            info.binding = uint8_t(i);
            uint8_t count = 0;
            for (uint8_t j = 0, m = uint8_t(samplers.size()); j < m; ++j) {
                // find its location and associate a TMU to it
                GLint loc = glGetUniformLocation(program, samplers[j].name.c_str());
                if (loc >= 0) {
                    glUniform1i(loc, tmu);
                    indicesRun[tmu] = j;
                    count++;
                    tmu++;
                } else {
                    // glGetUniformLocation could fail if the uniform is not used
                    // in the program. We should just ignore the error in that case.
                }
            }
            if (count > 0) {
                numUsedBindings++;
                info.count = uint8_t(count - 1);
            }
        }
    }
    mUsedBindingsCount = numUsedBindings;
}

void OpenGLProgram::updateSamplers(OpenGLDriver* gld) noexcept {
    using GLTexture = OpenGLDriver::GLTexture;

    // cache a few member variable locally, outside of the loop
    OpenGLContext& glc = gld->getContext();
    const bool anisotropyWorkaround = glc.ext.EXT_texture_filter_anisotropic &&
                                      glc.bugs.texture_filter_anisotropic_broken_on_sampler;
    auto const& UTILS_RESTRICT samplerBindings = gld->getSamplerBindings();
    auto const& UTILS_RESTRICT indicesRun = mIndicesRuns;
    auto const& UTILS_RESTRICT blockInfos = mBlockInfos;

    UTILS_ASSUME(mUsedBindingsCount > 0);
    for (uint8_t i = 0, tmu = 0, n = mUsedBindingsCount; i < n; i++) {
        BlockInfo blockInfo = blockInfos[i];
        HwSamplerGroup const * const UTILS_RESTRICT hwsb = samplerBindings[blockInfo.binding];
        if (UTILS_UNLIKELY(!hwsb)) {
            tmu += blockInfo.count + 1;
            continue;
        }
        SamplerGroup const& UTILS_RESTRICT sb = *(hwsb->sb);
        SamplerGroup::Sampler const* const UTILS_RESTRICT samplers = sb.getSamplers();
        for (uint8_t j = 0, m = blockInfo.count ; j <= m; ++j, ++tmu) { // "<=" on purpose here
            const uint8_t index = indicesRun[tmu];
            assert_invariant(index < sb.getSize());

            Handle<HwTexture> th = samplers[index].t;
            if (UTILS_UNLIKELY(!th)) {
#ifndef NDEBUG
                slog.w << "In material " << name.c_str()
                       << ": no texture bound to unit " << +index << io::endl;
#endif
                continue;
            }

            const GLTexture* const UTILS_RESTRICT t = gld->handle_cast<const GLTexture*>(th);
            if (UTILS_UNLIKELY(t->gl.fence)) {
                glWaitSync(t->gl.fence, 0, GL_TIMEOUT_IGNORED);
                glDeleteSync(t->gl.fence);
                t->gl.fence = nullptr;
            }

            SamplerParams params{ samplers[index].s };
            if (UTILS_UNLIKELY(t->target == SamplerType::SAMPLER_EXTERNAL)) {
                // From OES_EGL_image_external spec:
                // "The default s and t wrap modes are CLAMP_TO_EDGE and it is an INVALID_ENUM
                //  error to set the wrap mode to any other value."
                params.wrapS = SamplerWrapMode::CLAMP_TO_EDGE;
                params.wrapT = SamplerWrapMode::CLAMP_TO_EDGE;
                params.wrapR = SamplerWrapMode::CLAMP_TO_EDGE;
            }

#ifndef NDEBUG
            // GLES3.x specification forbids depth textures to be filtered.
            if (isDepthFormat(t->format)
                && params.compareMode == SamplerCompareMode::NONE
                && params.filterMag != SamplerMagFilter::NEAREST
                && params.filterMin != SamplerMinFilter::NEAREST
                && params.filterMin != SamplerMinFilter::NEAREST_MIPMAP_NEAREST) {
                slog.w << "In material " << name.c_str()
                       << ": depth texture used with filtering sampler, tmu = "
                       << +index << io::endl;
            }
#endif
            gld->bindTexture(tmu, t);
            gld->bindSampler(tmu, params);

#if defined(GL_EXT_texture_filter_anisotropic)
            if (UTILS_UNLIKELY(anisotropyWorkaround)) {
                // Driver claims to support anisotropic filtering, but it fails when set on
                // the sampler, we have to set it on the texture instead.
                // The texture is already bound here.
                GLfloat anisotropy = float(1u << params.anisotropyLog2);
                glTexParameterf(t->gl.target, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                        std::min(glc.gets.max_anisotropy, anisotropy));
            }
#endif
        }
    }
    CHECK_GL_ERROR(utils::slog.e)
}

UTILS_NOINLINE
void logCompilationError(io::ostream& out, Program::Shader shaderType,
        const char* name, GLuint shaderId) noexcept {

    auto to_string = [](Program::Shader type) -> const char* {
        switch (type) {
            case Program::Shader::VERTEX:       return "vertex";
            case Program::Shader::FRAGMENT:     return "fragment";
        }
    };

    char error[1024];
    glGetShaderInfoLog(shaderId, sizeof(error), nullptr, error);

    out << "Compilation error in " << to_string(shaderType) << " shader \"" << name << "\":\n"
        << "\"" << error << "\""
        << io::endl;
}

UTILS_NOINLINE
void logProgramLinkError(io::ostream& out, char const* name, GLuint program) noexcept {
    char error[1024];
    glGetProgramInfoLog(program, sizeof(error), nullptr, error);

    out << "Link error in \"" << name << "\":\n"
        << "\"" << error << "\""
        << io::endl;
}

} // namespace filament
