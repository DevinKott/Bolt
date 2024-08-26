#include "plugin/plugin.h"
#include "gl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// comment or uncomment this to enable verbose logging of hooks in this file
//#define VERBOSE

// don't change this part, change the line above this instead
#if defined(VERBOSE)
#define LOG printf
#else
#define LOG(...)
#endif

static unsigned int gl_width;
static unsigned int gl_height;

static size_t egl_init_count = 0;
static uintptr_t egl_main_context = 0;
static uint8_t egl_main_context_destroy_pending = 0;
static uint8_t egl_main_context_makecurrent_pending = 0;

static struct GLProcFunctions gl = {0};
static const struct GLLibFunctions* lgl = NULL;
static GLuint program_direct_screen;
static GLint program_direct_screen_sampler;
static GLint program_direct_screen_d_xywh;
static GLint program_direct_screen_s_xywh;
static GLint program_direct_screen_src_wh_dest_wh;
static GLuint program_direct_surface;
static GLint program_direct_surface_sampler;
static GLint program_direct_surface_d_xywh;
static GLint program_direct_surface_s_xywh;
static GLint program_direct_surface_src_wh_dest_wh;
static GLuint program_direct_vao;
static GLuint buffer_vertices_square;

// "direct" program is basically a blit but with transparency.
// there are different vertex shaders for targeting the screen vs targeting a surface.
static const GLchar program_direct_screen_vs[] = "#version 330 core\n"
"layout (location = 0) in vec2 aPos;"
"out vec2 vPos;"
"uniform ivec4 d_xywh;"
"uniform ivec4 src_wh_dest_wh;"
"void main() {"
  "vPos = aPos;"
  "gl_Position = vec4(((((aPos * d_xywh.pq) + d_xywh.st) * vec2(2.0, 2.0) / src_wh_dest_wh.pq) - vec2(1.0, 1.0)) * vec2(1.0, -1.0), 0.0, 1.0);"
"}";
static const GLchar program_direct_surface_vs[] = "#version 330 core\n"
"layout (location = 0) in vec2 aPos;"
"out vec2 vPos;"
"uniform ivec4 d_xywh;"
"uniform ivec4 src_wh_dest_wh;"
"void main() {"
  "vPos = aPos;"
  "gl_Position = vec4(((((aPos * d_xywh.pq) + d_xywh.st) * vec2(2.0, 2.0) / src_wh_dest_wh.pq) - vec2(1.0, 1.0)), 0.0, 1.0);"
"}";
static const GLchar program_direct_fs[] = "#version 330 core\n"
"in vec2 vPos;"
"layout (location = 0) out vec4 col;"
"uniform sampler2D tex;"
"uniform ivec4 s_xywh;"
"uniform ivec4 src_wh_dest_wh;"
"void main() {"
  "col = texture(tex, ((vPos * s_xywh.pq) + s_xywh.st) / src_wh_dest_wh.st);"
"}";

static struct GLProgram* _bolt_context_get_program(struct GLContext*, GLuint);
static struct GLArrayBuffer* _bolt_context_get_buffer(struct GLContext*, GLuint);
static struct GLTexture2D* _bolt_context_get_texture(struct GLContext*, GLuint);
static struct GLVertexArray* _bolt_context_get_vao(struct GLContext*, GLuint);
static void _bolt_glcontext_init(struct GLContext*, void*, void*);
static void _bolt_glcontext_free(struct GLContext*);

static void _bolt_gl_plugin_drawelements_vertex2d_xy(size_t index, void* userdata, int32_t* out);
static void _bolt_gl_plugin_drawelements_vertex2d_atlas_xy(size_t index, void* userdata, int32_t* out);
static void _bolt_gl_plugin_drawelements_vertex2d_atlas_wh(size_t index, void* userdata, int32_t* out);
static void _bolt_gl_plugin_drawelements_vertex2d_uv(size_t index, void* userdata, double* out);
static void _bolt_gl_plugin_drawelements_vertex2d_colour(size_t index, void* userdata, double* out);
static void _bolt_gl_plugin_drawelements_vertex3d_xyz(size_t index, void* userdata, int32_t* out);
static size_t _bolt_gl_plugin_drawelements_vertex3d_atlas_meta(size_t index, void* userdata);
static void _bolt_gl_plugin_drawelements_vertex3d_meta_xywh(size_t meta, void* userdata, int32_t* out);
static void _bolt_gl_plugin_drawelements_vertex3d_uv(size_t index, void* userdata, double* out);
static void _bolt_gl_plugin_drawelements_vertex3d_colour(size_t index, void* userdata, double* out);
static uint8_t _bolt_gl_plugin_drawelements_vertex3d_boneid(size_t index, void* userdata);
static void _bolt_gl_plugin_bone_transform(uint8_t bone_id, void* userdata, double* out);
static void _bolt_gl_plugin_matrix3d_toworldspace(int x, int y, int z, void* userdata, double* out);
static void _bolt_gl_plugin_matrix3d_toscreenspace(int x, int y, int z, void* userdata, double* out);
static void _bolt_gl_plugin_matrix3d_worldpos(void* userdata, double* out);
static size_t _bolt_gl_plugin_texture_id(void* userdata);
static void _bolt_gl_plugin_texture_size(void* userdata, size_t* out);
static uint8_t _bolt_gl_plugin_texture_compare(void* userdata, size_t x, size_t y, size_t len, const unsigned char* data);
static uint8_t* _bolt_gl_plugin_texture_data(void* userdata, size_t x, size_t y);
static void _bolt_gl_plugin_surface_init(struct SurfaceFunctions* out, unsigned int width, unsigned int height, const void* data);
static void _bolt_gl_plugin_surface_destroy(void* userdata);
static void _bolt_gl_plugin_surface_resize(void* userdata, unsigned int width, unsigned int height);
static void _bolt_gl_plugin_surface_clear(void* userdata, double r, double g, double b, double a);
static void _bolt_gl_plugin_surface_subimage(void* userdata, int x, int y, int w, int h, const void* pixels, uint8_t is_bgra);
static void _bolt_gl_plugin_surface_drawtoscreen(void* userdata, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh);
static void _bolt_gl_plugin_surface_drawtosurface(void* userdata, void* target, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh);

#define MAX_TEXTURE_UNITS 4096 // would be nice if there was a way to query this at runtime, but it would be awkward to set up
#define BUFFER_LIST_CAPACITY 256 * 256
#define TEXTURE_LIST_CAPACITY 256
#define PROGRAM_LIST_CAPACITY 256 * 8
#define VAO_LIST_CAPACITY 256 * 256
#define CONTEXTS_CAPACITY 64 // not growable so we just have to hard-code a number and hope it's enough forever
#define GAME_MINIMAP_BIG_SIZE 2048
static struct GLContext contexts[CONTEXTS_CAPACITY];

// since GL contexts are bound only to the thread that binds them, we use thread-local storage (TLS)
// to keep track of which context struct is relevant to each thread. One TLS slot can store exactly
// one pointer, which happens to be exactly what we need to store.
// Win32 and POSIX both define that the initial value will be 0 on all threads, current and future.
#if defined(_WIN32)
#include <Windows.h>
static DWORD current_context_tls;
#else
static pthread_key_t current_context_tls;
#endif

struct PluginSurfaceUserdata {
    unsigned int width;
    unsigned int height;
    unsigned int framebuffer;
    unsigned int renderbuffer;
};

struct GLContext* _bolt_context() {
#if defined(_WIN32)
    return (struct GLContext*)TlsGetValue(current_context_tls);
#else
    return (struct GLContext*)pthread_getspecific(current_context_tls);
#endif
}

static void _bolt_set_context(struct GLContext* context) {
#if defined(_WIN32)
    TlsSetValue(current_context_tls, (LPVOID)context);
#else
    pthread_setspecific(current_context_tls, context);
#endif
}

size_t _bolt_context_count() {
    size_t ret = 0;
    for (size_t i = 0; i < CONTEXTS_CAPACITY; i += 1) {
        if (contexts[i].id != 0) {
            ret += 1;
        }
    }
    return ret;
}

void _bolt_create_context(void* egl_context, void* shared) {
    for (size_t i = 0; i < CONTEXTS_CAPACITY; i += 1) {
        struct GLContext* ptr = &contexts[i];
        if (ptr->id == 0) {
            _bolt_glcontext_init(ptr, egl_context, shared);
            ptr->is_attached = 1;
            return;
        }
    }
}

void _bolt_destroy_context(void* egl_context) {
    for (size_t i = 0; i < CONTEXTS_CAPACITY; i += 1) {
        struct GLContext* ptr = &contexts[i];
        if (ptr->id == (uintptr_t)egl_context) {
            if (ptr->is_attached) {
                ptr->deferred_destroy = 1;
            } else {
                _bolt_glcontext_free(ptr);
                ptr->id = 0;
            }
            break;
        }
    }
}

void _bolt_set_attr_binding(struct GLContext* c, struct GLAttrBinding* binding, unsigned int buffer, int size, const void* offset, unsigned int stride, uint32_t type, uint8_t normalise) {
    binding->buffer = _bolt_context_get_buffer(c, buffer);
    binding->offset = (uintptr_t)offset;
    binding->size = size;
    binding->stride = stride;
    binding->normalise = normalise;
    binding->type = type;
}

float _bolt_f16_to_f32(uint16_t bits) {
    const uint16_t bits_exp_component = (bits & 0b0111110000000000);
    if (bits_exp_component == 0) return 0.0f; // truncate subnormals to 0
    const uint32_t sign_component = (bits & 0b1000000000000000) << 16;
    const uint32_t exponent = (bits_exp_component >> 10) + (127 - 15); // adjust exp bias
    const uint32_t mantissa = bits & 0b0000001111111111;
    const union { uint32_t b; float f; } u = {.b = sign_component | (exponent << 23) | (mantissa << 13)};
    return u.f;
}

uint8_t _bolt_get_attr_binding(struct GLContext* c, const struct GLAttrBinding* binding, size_t index, size_t num_out, float* out) {
    struct GLArrayBuffer* buffer = binding->buffer;
    if (!buffer || !buffer->data) return 0;
    uintptr_t buf_offset = binding->offset + (binding->stride * index);

    const uint8_t* ptr = (uint8_t*)buffer->data + buf_offset;
    if (!binding->normalise) {
        switch (binding->type) {
            case GL_FLOAT:
                memcpy(out, ptr, num_out * sizeof(float));
                break;
            case GL_HALF_FLOAT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = _bolt_f16_to_f32(*(uint16_t*)(ptr + (i * 2)));
                break;
            case GL_UNSIGNED_BYTE:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (float)*(uint8_t*)(ptr + i);
                break;
            case GL_UNSIGNED_SHORT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (float)*(uint16_t*)(ptr + (i * 2));
                break;
            case GL_UNSIGNED_INT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (float)*(uint32_t*)(ptr + (i * 4));
                break;
            case GL_BYTE:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (float)*(int8_t*)(ptr + i);
                break;
            case GL_SHORT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (float)*(int16_t*)(ptr + (i * 2));
                break;
            case GL_INT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (float)*(int32_t*)(ptr + (i * 4));
                break;
            default:
                printf("warning: unsupported non-normalise type %u\n", binding->type);
                memset(out, 0, num_out * sizeof(float));
                break;
        }
    } else {
        switch (binding->type) {
            case GL_FLOAT:
                memcpy(out, ptr, num_out * sizeof(float));
                break;
            case GL_UNSIGNED_BYTE:
                for (size_t i = 0; i < num_out; i += 1) out[i] = ((float)*(uint8_t*)(ptr + i)) / 255.0;
                break;
            case GL_UNSIGNED_SHORT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = ((float)*(uint16_t*)(ptr + (i * 2))) / 65535.0;
                break;
            case GL_UNSIGNED_INT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = ((float)*(uint32_t*)(ptr + (i * 4))) / 4294967295.0;
                break;
            case GL_BYTE:
                for (size_t i = 0; i < num_out; i += 1) out[i] = ((((float)*(int8_t*)(ptr + i)) + 128.0) * 2.0 / 255.0) - 1.0;
                break;
            default:
                printf("warning: unsupported normalise type %u\n", binding->type);
                memset(out, 0, num_out * sizeof(float));
                break;
        }
    }
    return 1;
}

uint8_t _bolt_get_attr_binding_int(struct GLContext* c, const struct GLAttrBinding* binding, size_t index, size_t num_out, int32_t* out) {
    struct GLArrayBuffer* buffer = binding->buffer;
    if (!buffer || !buffer->data) return 0;
    uintptr_t buf_offset = binding->offset + (binding->stride * index);

    const uint8_t* ptr = (uint8_t*)buffer->data + buf_offset;
    if (!binding->normalise) {
        switch (binding->type) {
            case GL_UNSIGNED_BYTE:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (int32_t)*(uint8_t*)(ptr + i);
                break;
            case GL_UNSIGNED_SHORT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (int32_t)*(uint16_t*)(ptr + (i * 2));
                break;
            case GL_UNSIGNED_INT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (int32_t)*(uint32_t*)(ptr + (i * 4));
                break;
            case GL_BYTE:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (int32_t)*(int8_t*)(ptr + i);
                break;
            case GL_SHORT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (int32_t)*(int16_t*)(ptr + (i * 2));
                break;
            case GL_INT:
                for (size_t i = 0; i < num_out; i += 1) out[i] = (int32_t)*(int32_t*)(ptr + (i * 4));
                break;
            default:
                return 0;
        }
    } else {
        return 0;
    }
    return 1;
}

static int _bolt_hashmap_compare(const void* a, const void* b, void* udata) {
    return (**(GLuint**)a) - (**(GLuint**)b);
}

static uint64_t _bolt_hashmap_hash(const void* item, uint64_t seed0, uint64_t seed1) {
    const GLuint* const* const id = item;
    return hashmap_sip(*id, sizeof(GLuint), seed0, seed1);
}

static void _bolt_hashmap_init(struct HashMap* map, size_t cap) {
    _bolt_rwlock_init(&map->rwlock);
    map->map = hashmap_new(sizeof(void*), cap, 0, 0, _bolt_hashmap_hash, _bolt_hashmap_compare, NULL, NULL);
}

static void _bolt_hashmap_destroy(struct HashMap* map) {
    _bolt_rwlock_destroy(&map->rwlock);
    hashmap_free(map->map);
}

static void _bolt_glcontext_init(struct GLContext* context, void* egl_context, void* egl_shared) {
    struct GLContext* shared = NULL;
    if (egl_shared) {
        for (size_t i = 0; i < CONTEXTS_CAPACITY; i += 1) {
            struct GLContext* ptr = &contexts[i];
            if (ptr->id == (uintptr_t)egl_shared) {
                shared = ptr;
                break;
            }
        }
    }
    memset(context, 0, sizeof(*context));
    context->id = (uintptr_t)egl_context;
    context->texture_units = calloc(MAX_TEXTURE_UNITS, sizeof(unsigned int));
    context->game_view_tex = -1;
    context->target_3d_tex = -1;
    context->game_view_framebuffer = -1;
    context->need_3d_tex = 0;
    if (shared) {
        context->programs = shared->programs;
        context->buffers = shared->buffers;
        context->textures = shared->textures;
        context->vaos = shared->vaos;
    } else {
        context->is_shared_owner = 1;
        context->programs = malloc(sizeof(struct HashMap));
        _bolt_hashmap_init(context->programs, PROGRAM_LIST_CAPACITY);
        context->buffers = malloc(sizeof(struct HashMap));
        _bolt_hashmap_init(context->buffers, BUFFER_LIST_CAPACITY);
        context->textures = malloc(sizeof(struct HashMap));
        _bolt_hashmap_init(context->textures, TEXTURE_LIST_CAPACITY);
        context->vaos = malloc(sizeof(struct HashMap));
        _bolt_hashmap_init(context->vaos, VAO_LIST_CAPACITY);
    }
}

static void _bolt_glcontext_free(struct GLContext* context) {
    free(context->texture_units);
    if (context->is_shared_owner) {
        _bolt_hashmap_destroy(context->programs);
        free(context->programs);
        _bolt_hashmap_destroy(context->buffers);
        free(context->buffers);
        _bolt_hashmap_destroy(context->textures);
        free(context->textures);
        _bolt_hashmap_destroy(context->vaos);
        free(context->vaos);
    }
}

static GLenum _bolt_binding_for_buffer(GLuint target) {
    switch (target) {
        case GL_ARRAY_BUFFER:
            return GL_ARRAY_BUFFER_BINDING;
        case GL_ELEMENT_ARRAY_BUFFER:
            return GL_ELEMENT_ARRAY_BUFFER_BINDING;
        case GL_UNIFORM_BUFFER:
            return GL_UNIFORM_BUFFER_BINDING;
        default:
            // we don't care about other types of buffer
            return -1;
    }
}

static struct GLProgram* _bolt_context_get_program(struct GLContext* c, GLuint index) {
    struct HashMap* map = c->programs;
    const GLuint* index_ptr = &index;
    _bolt_rwlock_lock_read(&map->rwlock);
    struct GLProgram** program = (struct GLProgram**)hashmap_get(map->map, &index_ptr);
    struct GLProgram* ret = program ? *program : NULL;
    _bolt_rwlock_unlock_read(&map->rwlock);
    return ret;
}

static struct GLArrayBuffer* _bolt_context_get_buffer(struct GLContext* c, GLuint index) {
    struct HashMap* map = c->buffers;
    const GLuint* index_ptr = &index;
    _bolt_rwlock_lock_read(&map->rwlock);
    struct GLArrayBuffer** buffer = (struct GLArrayBuffer**)hashmap_get(map->map, &index_ptr);
    struct GLArrayBuffer* ret = buffer ? *buffer : NULL;
    _bolt_rwlock_unlock_read(&map->rwlock);
    return ret;
}

static struct GLTexture2D* _bolt_context_get_texture(struct GLContext* c, GLuint index) {
    struct HashMap* map = c->textures;
    const GLuint* index_ptr = &index;
    _bolt_rwlock_lock_read(&map->rwlock);
    struct GLTexture2D** tex = (struct GLTexture2D**)hashmap_get(map->map, &index_ptr);
    struct GLTexture2D* ret = tex ? *tex : NULL;
    _bolt_rwlock_unlock_read(&map->rwlock);
    return ret;
}

static struct GLVertexArray* _bolt_context_get_vao(struct GLContext* c, GLuint index) {
    struct HashMap* map = c->vaos;
    const GLuint* index_ptr = &index;
    _bolt_rwlock_lock_read(&map->rwlock);
    struct GLVertexArray** vao = (struct GLVertexArray**)hashmap_get(map->map, &index_ptr);
    struct GLVertexArray* ret = vao ? *vao : NULL;
    _bolt_rwlock_unlock_read(&map->rwlock);
    return ret;
}

static void _bolt_unpack_rgb565(uint16_t packed, uint8_t out[3]) {
    out[0] = (packed >> 11) & 0b00011111;
    out[0] = (out[0] << 3) | (out[0] >> 2);
    out[1] = (packed >> 5) & 0b00111111;
    out[1] = (out[1] << 2) | (out[1] >> 4);
    out[2] = packed & 0b00011111;
    out[2] = (out[2] << 3) | (out[2] >> 2);
}

// use a lookup table for each of the possible 5-bit and 6-bit values for consistency across platforms
// `lut5 = [round(pow(((x << 3) | (x >> 2)) / 255.0, 2.2) * 255.0) for x in range(32)]`
// `lut6 = [round(pow(((x << 2) | (x >> 4)) / 255.0, 2.2) * 255.0) for x in range(64)]`
//const uint8_t lut5[] = {0, 0, 1, 1, 3, 5, 7, 9, 13, 17, 21, 26, 32, 38, 44, 51, 60, 68, 77, 87, 98, 109, 120, 132, 146, 159, 173, 188, 205, 221, 238, 255};
//const uint8_t lut6[] = {0, 0, 0, 0, 1, 1, 1, 2, 3, 3, 4, 5, 6, 8, 9, 11, 13, 14, 16, 18, 20, 23, 25, 28, 30, 33, 36, 39, 43, 46, 49, 53, 58, 62, 66, 70, 75, 79, 84, 89, 94, 99, 105, 110, 116, 121, 127, 133, 141, 148, 154, 161, 168, 175, 182, 190, 197, 205, 213, 221, 229, 238, 246, 255};
static void _bolt_unpack_srgb565(uint16_t packed, uint8_t out[3]) {
    // game seems to be giving us RGBA and telling us it's SRGB, so for now, just don't convert it
    _bolt_unpack_rgb565(packed, out);
    //out[0] = lut5[(packed >> 11) & 0b00011111];
    //out[1] = lut6[(packed >> 5) & 0b00111111];
    //out[2] = lut5[packed & 0b00011111];
}

// note this function binds GL_DRAW_FRAMEBUFFER and GL_TEXTURE_2D (for the current active texture unit)
// so you'll have to restore the prior values yourself if you need to leave the opengl state unchanged
static void _bolt_gl_surface_init_buffers(struct PluginSurfaceUserdata* userdata) {
    gl.GenFramebuffers(1, &userdata->framebuffer);
    lgl->GenTextures(1, &userdata->renderbuffer);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, userdata->framebuffer);
    lgl->BindTexture(GL_TEXTURE_2D, userdata->renderbuffer);
    gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, userdata->width, userdata->height);
    lgl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    lgl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    lgl->TexParameteri(GL_TEXTURE_2D,  GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    lgl->TexParameteri(GL_TEXTURE_2D,  GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.FramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, userdata->renderbuffer, 0);
}

static void _bolt_gl_surface_destroy_buffers(struct PluginSurfaceUserdata* userdata) {
    gl.DeleteFramebuffers(1, &userdata->framebuffer);
    lgl->DeleteTextures(1, &userdata->renderbuffer);
}

// this function is called at the earliest possible opportunity, and is never undone
static void _bolt_gl_load(void* (*GetProcAddress)(const char*)) {
#if defined(_WIN32)
    current_context_tls = TlsAlloc();
#else
    pthread_key_create(&current_context_tls, NULL);
#endif
#define INIT_GL_FUNC(NAME) gl.NAME = GetProcAddress("gl"#NAME);
    INIT_GL_FUNC(ActiveTexture)
    INIT_GL_FUNC(AttachShader)
    INIT_GL_FUNC(BindAttribLocation)
    INIT_GL_FUNC(BindBuffer)
    INIT_GL_FUNC(BindFramebuffer)
    INIT_GL_FUNC(BindVertexArray)
    INIT_GL_FUNC(BlitFramebuffer)
    INIT_GL_FUNC(BufferData)
    INIT_GL_FUNC(BufferStorage)
    INIT_GL_FUNC(BufferSubData)
    INIT_GL_FUNC(CompileShader)
    INIT_GL_FUNC(CompressedTexSubImage2D)
    INIT_GL_FUNC(CopyImageSubData)
    INIT_GL_FUNC(CreateProgram)
    INIT_GL_FUNC(CreateShader)
    INIT_GL_FUNC(DeleteBuffers)
    INIT_GL_FUNC(DeleteFramebuffers)
    INIT_GL_FUNC(DeleteProgram)
    INIT_GL_FUNC(DeleteShader)
    INIT_GL_FUNC(DeleteVertexArrays)
    INIT_GL_FUNC(DisableVertexAttribArray)
    INIT_GL_FUNC(DrawElements)
    INIT_GL_FUNC(EnableVertexAttribArray)
    INIT_GL_FUNC(FlushMappedBufferRange)
    INIT_GL_FUNC(FramebufferTexture)
    INIT_GL_FUNC(FramebufferTextureLayer)
    INIT_GL_FUNC(GenBuffers)
    INIT_GL_FUNC(GenFramebuffers)
    INIT_GL_FUNC(GenVertexArrays)
    INIT_GL_FUNC(GetActiveUniformBlockiv)
    INIT_GL_FUNC(GetActiveUniformsiv)
    INIT_GL_FUNC(GetFramebufferAttachmentParameteriv)
    INIT_GL_FUNC(GetIntegeri_v)
    INIT_GL_FUNC(GetIntegerv)
    INIT_GL_FUNC(GetUniformBlockIndex)
    INIT_GL_FUNC(GetUniformfv)
    INIT_GL_FUNC(GetUniformIndices)
    INIT_GL_FUNC(GetUniformiv)
    INIT_GL_FUNC(GetUniformLocation)
    INIT_GL_FUNC(LinkProgram)
    INIT_GL_FUNC(MapBufferRange)
    INIT_GL_FUNC(MultiDrawElements)
    INIT_GL_FUNC(ShaderSource)
    INIT_GL_FUNC(TexStorage2D)
    INIT_GL_FUNC(Uniform1i)
    INIT_GL_FUNC(Uniform4i)
    INIT_GL_FUNC(UniformMatrix4fv)
    INIT_GL_FUNC(UnmapBuffer)
    INIT_GL_FUNC(UseProgram)
    INIT_GL_FUNC(VertexAttribPointer)
#undef INIT_GL_FUNC
}

// this function is called when the "main" gl context gets created, and is undone by _bolt_gl_close()
// when all the contexts are destroyed.
static void _bolt_gl_init() {
    GLint size;
    const GLchar* source;
    GLuint direct_screen_vs = gl.CreateShader(GL_VERTEX_SHADER);
    source = &program_direct_screen_vs[0];
    size = sizeof(program_direct_screen_vs) - sizeof(*program_direct_screen_vs);
    gl.ShaderSource(direct_screen_vs, 1, &source, &size);
    gl.CompileShader(direct_screen_vs);

    GLuint direct_surface_vs = gl.CreateShader(GL_VERTEX_SHADER);
    source = &program_direct_surface_vs[0];
    size = sizeof(program_direct_surface_vs) - sizeof(*program_direct_surface_vs);
    gl.ShaderSource(direct_surface_vs, 1, &source, &size);
    gl.CompileShader(direct_surface_vs);

    GLuint direct_fs = gl.CreateShader(GL_FRAGMENT_SHADER);
    source = &program_direct_fs[0];
    size = sizeof(program_direct_fs) - sizeof(*program_direct_fs);
    gl.ShaderSource(direct_fs, 1, &source, &size);
    gl.CompileShader(direct_fs);

    program_direct_screen = gl.CreateProgram();
    gl.AttachShader(program_direct_screen, direct_screen_vs);
    gl.AttachShader(program_direct_screen, direct_fs);
    gl.LinkProgram(program_direct_screen);
    program_direct_screen_sampler = gl.GetUniformLocation(program_direct_screen, "tex");
    program_direct_screen_d_xywh = gl.GetUniformLocation(program_direct_screen, "d_xywh");
    program_direct_screen_s_xywh = gl.GetUniformLocation(program_direct_screen, "s_xywh");
    program_direct_screen_src_wh_dest_wh = gl.GetUniformLocation(program_direct_screen, "src_wh_dest_wh");

    program_direct_surface = gl.CreateProgram();
    gl.AttachShader(program_direct_surface, direct_surface_vs);
    gl.AttachShader(program_direct_surface, direct_fs);
    gl.LinkProgram(program_direct_surface);
    program_direct_surface_sampler = gl.GetUniformLocation(program_direct_surface, "tex");
    program_direct_surface_d_xywh = gl.GetUniformLocation(program_direct_surface, "d_xywh");
    program_direct_surface_s_xywh = gl.GetUniformLocation(program_direct_surface, "s_xywh");
    program_direct_surface_src_wh_dest_wh = gl.GetUniformLocation(program_direct_surface, "src_wh_dest_wh");

    gl.DeleteShader(direct_screen_vs);
    gl.DeleteShader(direct_surface_vs);
    gl.DeleteShader(direct_fs);

    gl.GenVertexArrays(1, &program_direct_vao);
    gl.BindVertexArray(program_direct_vao);
    gl.GenBuffers(1, &buffer_vertices_square);
    gl.BindBuffer(GL_ARRAY_BUFFER, buffer_vertices_square);
    const GLfloat square[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 8, square, GL_STATIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, 0, 2 * sizeof(float), NULL);
    gl.BindVertexArray(0);
}

void _bolt_gl_close() {
    gl.DeleteBuffers(1, &buffer_vertices_square);
    gl.DeleteProgram(program_direct_screen);
    gl.DeleteProgram(program_direct_surface);
    gl.DeleteVertexArrays(1, &program_direct_vao);
    _bolt_destroy_context((void*)egl_main_context);
}

static GLuint _bolt_glCreateProgram() {
    LOG("glCreateProgram\n");
    GLuint id = gl.CreateProgram();
    struct GLContext* c = _bolt_context();
    struct GLProgram* program = malloc(sizeof(struct GLProgram));
    program->id = id;
    program->loc_aVertexPosition2D = -1;
    program->loc_aVertexColour = -1;
    program->loc_aTextureUV = -1;
    program->loc_aTextureUVAtlasMin = -1;
    program->loc_aTextureUVAtlasExtents = -1;
    program->loc_aMaterialSettingsSlotXY_TilePositionXZ = -1;
    program->loc_aVertexPosition_BoneLabel = -1;
    program->loc_uProjectionMatrix = -1;
    program->loc_uDiffuseMap = -1;
    program->loc_uTextureAtlas = -1;
    program->loc_uTextureAtlasSettings = -1;
    program->loc_uAtlasMeta = -1;
    program->loc_uModelMatrix = -1;
    program->loc_uBoneTransforms = -1;
    program->loc_uGridSize = -1;
    program->loc_uVertexScale = -1;
    program->loc_sSceneHDRTex = -1;
    program->block_index_ViewTransforms = -1;
    program->offset_uCameraPosition = -1;
    program->offset_uViewProjMatrix = -1;
    program->is_2d = 0;
    program->is_3d = 0;
    program->is_minimap = 0;
    _bolt_rwlock_lock_write(&c->programs->rwlock);
    hashmap_set(c->programs->map, &program);
    _bolt_rwlock_unlock_write(&c->programs->rwlock);
    LOG("glCreateProgram end\n");
    return id;
}

static void _bolt_glDeleteProgram(GLuint program) {
    LOG("glDeleteProgram\n");
    gl.DeleteProgram(program);
    struct GLContext* c = _bolt_context();
    unsigned int* ptr = &program;
    _bolt_rwlock_lock_write(&c->programs->rwlock);
    struct GLProgram* const* p = hashmap_delete(c->programs->map, &ptr);
    free(*p);
    _bolt_rwlock_unlock_write(&c->programs->rwlock);
    LOG("glDeleteProgram end\n");
}

static void _bolt_glBindAttribLocation(GLuint program, GLuint index, const GLchar* name) {
    LOG("glBindAttribLocation\n");
    gl.BindAttribLocation(program, index, name);
    struct GLContext* c = _bolt_context();
    struct GLProgram* p = _bolt_context_get_program(c, program);
#define ATTRIB_MAP(NAME) if (!strcmp(name, #NAME)) p->loc_##NAME = index;
    ATTRIB_MAP(aVertexPosition2D)
    ATTRIB_MAP(aVertexColour)
    ATTRIB_MAP(aTextureUV)
    ATTRIB_MAP(aTextureUVAtlasMin)
    ATTRIB_MAP(aTextureUVAtlasExtents)
    ATTRIB_MAP(aMaterialSettingsSlotXY_TilePositionXZ)
    ATTRIB_MAP(aVertexPosition_BoneLabel)
#undef ATTRIB_MAP
    LOG("glBindAttribLocation end\n");
}

static void _bolt_glLinkProgram(GLuint program) {
    LOG("glLinkProgram\n");
    gl.LinkProgram(program);
    struct GLContext* c = _bolt_context();
    struct GLProgram* p = _bolt_context_get_program(c, program);
    const GLint uDiffuseMap = gl.GetUniformLocation(program, "uDiffuseMap");
    const GLint uProjectionMatrix = gl.GetUniformLocation(program, "uProjectionMatrix");
    const GLint uTextureAtlas = gl.GetUniformLocation(program, "uTextureAtlas");
    const GLint uTextureAtlasSettings = gl.GetUniformLocation(program, "uTextureAtlasSettings");
    const GLint uAtlasMeta = gl.GetUniformLocation(program, "uAtlasMeta");
    const GLint loc_uModelMatrix = gl.GetUniformLocation(program, "uModelMatrix");
    const GLint loc_uGridSize = gl.GetUniformLocation(program, "uGridSize");
    const GLint loc_uVertexScale = gl.GetUniformLocation(program, "uVertexScale");
    p->loc_sSceneHDRTex = gl.GetUniformLocation(program, "sSceneHDRTex");
    p->loc_sSourceTex = gl.GetUniformLocation(program, "sSourceTex");
    p->loc_uBoneTransforms = gl.GetUniformLocation(program, "uBoneTransforms");

    const GLchar* view_var_names[] = {"uCameraPosition", "uViewProjMatrix"};
    const GLuint block_index_ViewTransforms = gl.GetUniformBlockIndex(program, "ViewTransforms");
    GLuint ubo_indices[2];
    GLint view_offsets[2];
    GLint viewport_offset;
    if ((GLint)block_index_ViewTransforms != -1) {
        gl.GetUniformIndices(program, 2, view_var_names, ubo_indices);
        gl.GetActiveUniformsiv(program, 2, ubo_indices, GL_UNIFORM_OFFSET, view_offsets);
    }

    if (loc_uModelMatrix != -1 && loc_uGridSize != -1 && block_index_ViewTransforms != -1) {
        p->loc_uModelMatrix = loc_uModelMatrix;
        p->loc_uGridSize = loc_uGridSize;
        p->block_index_ViewTransforms = block_index_ViewTransforms;
        p->offset_uCameraPosition = view_offsets[0];
        p->offset_uViewProjMatrix = view_offsets[1];
        p->is_minimap = 1;
    }
    if (p && p->loc_aVertexPosition2D != -1 && p->loc_aVertexColour != -1 && p->loc_aTextureUV != -1 && p->loc_aTextureUVAtlasMin != -1 && p->loc_aTextureUVAtlasExtents != -1 && uDiffuseMap != -1 && uProjectionMatrix != -1) {
        p->loc_uDiffuseMap = uDiffuseMap;
        p->loc_uProjectionMatrix = uProjectionMatrix;
        p->is_2d = 1;
    }
    if (p && p->loc_aTextureUV != -1 && p->loc_aVertexColour != -1 && p->loc_aVertexPosition_BoneLabel != -1 && p->loc_aMaterialSettingsSlotXY_TilePositionXZ != -1 && uTextureAtlas != -1 && uAtlasMeta != -1 && uTextureAtlasSettings != -1 && loc_uModelMatrix != -1 && loc_uVertexScale != -1 && block_index_ViewTransforms != -1) {
        p->loc_uTextureAtlas = uTextureAtlas;
        p->loc_uTextureAtlasSettings = uTextureAtlasSettings;
        p->loc_uAtlasMeta = uAtlasMeta;
        p->loc_uModelMatrix = loc_uModelMatrix;
        p->loc_uVertexScale = loc_uVertexScale;
        p->block_index_ViewTransforms = block_index_ViewTransforms;
        p->offset_uCameraPosition = view_offsets[0];
        p->offset_uViewProjMatrix = view_offsets[1];
        p->is_3d = 1;
    }
    LOG("glLinkProgram end\n");
}

static void _bolt_glUseProgram(GLuint program) {
    LOG("glUseProgram\n");
    gl.UseProgram(program);
    struct GLContext* c = _bolt_context();
    c->bound_program = _bolt_context_get_program(c, program);
    LOG("glUseProgram end\n");
}

static void _bolt_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
    LOG("glTexStorage2D\n");
    gl.TexStorage2D(target, levels, internalformat, width, height);
    struct GLContext* c = _bolt_context();
    if (target == GL_TEXTURE_2D) {
        struct GLTexture2D* tex = c->texture_units[c->active_texture];
        free(tex->data);
        tex->data = malloc(width * height * 4);
        tex->width = width;
        tex->height = height;
    }
    LOG("glTexStorage2D end\n");
}

static void _bolt_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalised, GLsizei stride, const void* pointer) {
    LOG("glVertexAttribPointer\n");
    gl.VertexAttribPointer(index, size, type, normalised, stride, pointer);
    struct GLContext* c = _bolt_context();
    GLint array_binding;
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_binding);
    _bolt_set_attr_binding(c, &c->bound_vao->attributes[index], array_binding, size, pointer, stride, type, normalised);
    LOG("glVertexAttribPointer end\n");
}

static void _bolt_glGenBuffers(GLsizei n, GLuint* buffers) {
    LOG("glGenBuffers\n");
    gl.GenBuffers(n, buffers);
    struct GLContext* c = _bolt_context();
    _bolt_rwlock_lock_write(&c->buffers->rwlock);
    for (GLsizei i = 0; i < n; i += 1) {
        struct GLArrayBuffer* buffer = calloc(1, sizeof(struct GLArrayBuffer));
        buffer->id = buffers[i];
        hashmap_set(c->buffers->map, &buffer);
    }
    _bolt_rwlock_unlock_write(&c->buffers->rwlock);
    LOG("glGenBuffers end\n");
}

static void _bolt_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    LOG("glBufferData\n");
    gl.BufferData(target, size, data, usage);
    struct GLContext* c = _bolt_context();
    GLenum binding_type = _bolt_binding_for_buffer(target);
    if (binding_type != -1) {
        GLint buffer_id;
        gl.GetIntegerv(binding_type, &buffer_id);
        void* buffer_content = malloc(size);
        if (data) memcpy(buffer_content, data, size);
        struct GLArrayBuffer* buffer = _bolt_context_get_buffer(c, buffer_id);
        free(buffer->data);
        buffer->data = buffer_content;
    }
    LOG("glBufferData end\n");
}

static void _bolt_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    LOG("glDeleteBuffers\n");
    gl.DeleteBuffers(n, buffers);
    struct GLContext* c = _bolt_context();
    _bolt_rwlock_lock_write(&c->buffers->rwlock);
    for (GLsizei i = 0; i < n; i += 1) {
        const GLuint* ptr = &buffers[i];
        struct GLArrayBuffer* const* buffer = hashmap_delete(c->buffers->map, &ptr);
        free((*buffer)->data);
        free((*buffer)->mapping);
        free(*buffer);
    }
    _bolt_rwlock_unlock_write(&c->buffers->rwlock);
    LOG("glDeleteBuffers end\n");
}

static void _bolt_glBindFramebuffer(GLenum target, GLuint framebuffer) {
    LOG("glBindFramebuffer\n");
    gl.BindFramebuffer(target, framebuffer);
    struct GLContext* c = _bolt_context();
    switch (target) {
        case GL_READ_FRAMEBUFFER:
            c->current_read_framebuffer = framebuffer;
            break;
        case GL_DRAW_FRAMEBUFFER:
            c->current_draw_framebuffer = framebuffer;
            break;
        case GL_FRAMEBUFFER:
            c->current_read_framebuffer = framebuffer;
            c->current_draw_framebuffer = framebuffer;
            break;
    }
    LOG("glBindFramebuffer end\n");
}

// https://www.khronos.org/opengl/wiki/S3_Texture_Compression
static void _bolt_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data) {
    LOG("glCompressedTexSubImage2D\n");
    gl.CompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
    if (target != GL_TEXTURE_2D || level != 0 || width <= 0 || height <= 0) return;
    const uint8_t is_dxt1 = (format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || format == GL_COMPRESSED_SRGB_S3TC_DXT1_EXT || format == GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT);
    const uint8_t is_srgb = (format == GL_COMPRESSED_SRGB_S3TC_DXT1_EXT || format == GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT || format == GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT || format == GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT);
    const size_t input_stride = is_dxt1 ? 8 : 16;
    void (*const unpack565)(uint16_t, uint8_t*) = is_srgb ? _bolt_unpack_srgb565 : _bolt_unpack_rgb565;
    struct GLContext* c = _bolt_context();
    struct GLTexture2D* tex = c->texture_units[c->active_texture];
    GLint out_xoffset = xoffset;
    GLint out_yoffset = yoffset;
    for (size_t ii = 0; ii < ((size_t)width * (size_t)height); ii += input_stride) {
        const uint8_t* ptr = (uint8_t*)data + ii;
        const uint8_t* cptr = is_dxt1 ? ptr : (ptr + 8);
        uint8_t* out_ptr = tex->data + (out_yoffset * tex->width * 4) + (out_xoffset * 4);
        uint16_t c0 = cptr[0] + (cptr[1] << 8);
        uint16_t c1 = cptr[2] + (cptr[3] << 8);
        uint8_t c0_rgb[3];
        uint8_t c1_rgb[3];
        unpack565(c0, c0_rgb);
        unpack565(c1, c1_rgb);
        const uint8_t c0_greater = c0 > c1;
        const uint32_t ctable = cptr[4] + (cptr[5] << 8) + (cptr[6] << 16) + (cptr[7] << 24);
        const uint8_t alpha0 = ptr[0];
        const uint8_t alpha1 = ptr[1];
        const uint8_t a0_greater = alpha0 > alpha1;
        const uint64_t atable = ptr[2] + ((uint64_t)ptr[3] << 8) + ((uint64_t)ptr[4] << 16) + ((uint64_t)ptr[5] << 24) + ((uint64_t)ptr[6] << 32) + ((uint64_t)ptr[7] << 40);

        for (size_t j = 0; j < 4; j += 1) {
            for (size_t i = 0; i < 4; i += 1) {
                if (out_xoffset + i >= 0 && out_yoffset + j >= 0 && out_xoffset + i < tex->width && out_yoffset + j < tex->height) {
                    uint8_t* pixel_ptr = out_ptr + (tex->width * j * 4) + (i * 4);
                    uint8_t pixel_index = (4 * j) + i;
                    const uint8_t code = (ctable >> (pixel_index * 2)) & 0b11;

                    // parse colour table and set R, G and B bytes for this pixel
                    switch(code) {
                        case 0:
                            memcpy(pixel_ptr, c0_rgb, 3);
                            break;
                        case 1:
                            memcpy(pixel_ptr, c1_rgb, 3);
                            break;
                        case 2:
                            if (c0_greater) {
                                pixel_ptr[0] = (2*c0_rgb[0]+c1_rgb[0])/3;
                                pixel_ptr[1] = (2*c0_rgb[1]+c1_rgb[1])/3;
                                pixel_ptr[2] = (2*c0_rgb[2]+c1_rgb[2])/3;
                            } else {
                                pixel_ptr[0] = (c0_rgb[0]+c1_rgb[0])/2;
                                pixel_ptr[1] = (c0_rgb[1]+c1_rgb[1])/2;
                                pixel_ptr[2] = (c0_rgb[2]+c1_rgb[2])/2;
                            }
                            break;
                        case 3:
                            if (c0_greater) {
                                pixel_ptr[0] = (2*c1_rgb[0]+c0_rgb[0])/3;
                                pixel_ptr[1] = (2*c1_rgb[1]+c0_rgb[1])/3;
                                pixel_ptr[2] = (2*c1_rgb[2]+c0_rgb[2])/3;
                            } else {
                                memset(pixel_ptr, 0, 3);
                            }
                            break;
                    }

                    // parse alpha table (if any) and set alpha byte for this pixel
                    switch (format) {
                        case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
                        case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT: {
                            // no alpha channel at all
                            pixel_ptr[3] = 0xFF;
                            break;
                        }
                        case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
                        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT: {
                            // black means zero alpha
                            pixel_ptr[3] = (code == 3 && !c0_greater) ? 0 : 0xFF;
                            break;
                        }
                        case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
                        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT: {
                            // 4-bit alpha value from the alpha chunk
                            pixel_ptr[3] = ((ptr[pixel_index / 2] >> ((i & 1) ? 4 : 0)) & 0b1111) * 17;
                            break;
                        }
                        case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT: {
                            // 3-bit instruction value from atable
                            const uint8_t code = (atable >> (pixel_index * 3)) & 0b111;
                            switch (code) {
                                case 0:
                                    pixel_ptr[3] = alpha0;
                                    break;
                                case 1:
                                    pixel_ptr[3] = alpha1;
                                    break;
                                case 6:
                                case 7:
                                    if (!a0_greater) {
                                        pixel_ptr[3] = code == 6 ? 0 : 0xFF;
                                        break;
                                    }
                                    // fall through is intentional
                                default:
                                    if (a0_greater) pixel_ptr[3] = (((8 - code) * (uint16_t)alpha0) + ((code - 1) * (uint16_t)alpha1)) / 7;
                                    else pixel_ptr[3] = (((6 - code) * (uint16_t)alpha0) + ((code - 1) * (uint16_t)alpha1)) / 5;
                                    break;
                            }
                            break;
                        }
                    }
                }
            }
        }

        out_xoffset += 4;
        if (out_xoffset >= xoffset + width) {
            out_xoffset = xoffset;
            out_yoffset += 4;
        }
    }
    LOG("glCompressedTexSubImage2D end\n");
}

static void _bolt_glCopyImageSubData(
    GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
    GLuint dstName, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
    GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth
) {
    LOG("glCopyImageSubData\n");
    gl.CopyImageSubData(srcName, srcTarget, srcLevel, srcX, srcY, srcZ, dstName, dstTarget, dstLevel, dstX, dstY, dstZ, srcWidth, srcHeight, srcDepth);
    struct GLContext* c = _bolt_context();
    if (srcTarget == GL_TEXTURE_2D && dstTarget == GL_TEXTURE_2D && srcLevel == 0 && dstLevel == 0) {
        struct GLTexture2D* src = _bolt_context_get_texture(c, srcName);
        struct GLTexture2D* dst = _bolt_context_get_texture(c, dstName);
        for (GLsizei i = 0; i < srcHeight; i += 1) {
            memcpy(dst->data + (dstY * dst->width * 4) + (dstX * 4), src->data + (srcY * src->width * 4) + (srcX * 4), srcWidth * 4);
        }
    }
    LOG("glCopyImageSubData end\n");
}

static void _bolt_glEnableVertexAttribArray(GLuint index) {
    LOG("glEnableVertexAttribArray\n");
    gl.EnableVertexAttribArray(index);
    struct GLContext* c = _bolt_context();
    c->bound_vao->attributes[index].enabled = 1;
    LOG("glEnableVertexAttribArray end\n");
}

static void _bolt_glDisableVertexAttribArray(GLuint index) {
    LOG("glDisableVertexAttribArray\n");
    gl.DisableVertexAttribArray(index);
    struct GLContext* c = _bolt_context();
    c->bound_vao->attributes[index].enabled = 0;
    LOG("glDisableVertexAttribArray end\n");
}

static void* _bolt_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    LOG("glMapBufferRange\n");
    struct GLContext* c = _bolt_context();
    GLenum binding_type = _bolt_binding_for_buffer(target);
    if (binding_type != -1) {
        GLint buffer_id;
        gl.GetIntegerv(binding_type, &buffer_id);
        struct GLArrayBuffer* buffer = _bolt_context_get_buffer(c, buffer_id);
        buffer->mapping = malloc(length);
        buffer->mapping_offset = offset;
        buffer->mapping_len = length;
        buffer->mapping_access_type = access;
        LOG("glMapBufferRange end (intercepted)\n");
        return buffer->mapping;
    } else {
        void* ret = gl.MapBufferRange(target, offset, length, access);
        LOG("glMapBufferRange end (not intercepted)\n");
        return ret;
    }
}

static GLboolean _bolt_glUnmapBuffer(GLuint target) {
    LOG("glUnmapBuffer\n");
    struct GLContext* c = _bolt_context();
    GLenum binding_type = _bolt_binding_for_buffer(target);
    if (binding_type != -1) {
        GLint buffer_id;
        gl.GetIntegerv(binding_type, &buffer_id);
        struct GLArrayBuffer* buffer = _bolt_context_get_buffer(c, buffer_id);
        free(buffer->mapping);
        buffer->mapping = NULL;
        LOG("glUnmapBuffer end (intercepted)\n");
        return 1;
    } else {
        GLboolean ret = gl.UnmapBuffer(target);
        LOG("glUnmapBuffer end (not intercepted)\n");
        return ret;
    }
}

static void _bolt_glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    LOG("glBufferStorage\n");
    gl.BufferStorage(target, size, data, flags);
    struct GLContext* c = _bolt_context();
    GLenum binding_type = _bolt_binding_for_buffer(target);
    if (binding_type != -1) {
        GLint buffer_id;
        gl.GetIntegerv(binding_type, &buffer_id);
        void* buffer_content = malloc(size);
        if (data) memcpy(buffer_content, data, size);
        struct GLArrayBuffer* buffer = _bolt_context_get_buffer(c, buffer_id);
        free(buffer->data);
        buffer->data = buffer_content;
    }
    LOG("glBufferStorage end (%s)\n", binding_type == -1 ? "not intercepted" : "intercepted");
}

static void _bolt_glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    LOG("glFlushMappedBufferRange\n");
    struct GLContext* c = _bolt_context();
    GLenum binding_type = _bolt_binding_for_buffer(target);
    if (binding_type != -1) {
        GLint buffer_id;
        gl.GetIntegerv(binding_type, &buffer_id);
        struct GLArrayBuffer* buffer = _bolt_context_get_buffer(c, buffer_id);
        gl.BufferSubData(target, buffer->mapping_offset + offset, length, buffer->mapping + offset);
        memcpy((uint8_t*)buffer->data + buffer->mapping_offset + offset, buffer->mapping + offset, length);
    } else {
        gl.FlushMappedBufferRange(target, offset, length);
    }
    LOG("glFlushMappedBufferRange end (%s)\n", binding_type == -1 ? "not intercepted" : "intercepted");
}

static void _bolt_glActiveTexture(GLenum texture) {
    LOG("glActiveTexture\n");
    gl.ActiveTexture(texture);
    struct GLContext* c = _bolt_context();
    c->active_texture = texture - GL_TEXTURE0;
    LOG("glActiveTexture end\n");
}

static void _bolt_glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices, GLsizei drawcount) {
    LOG("glMultiDrawElements\n");
    gl.MultiDrawElements(mode, count, type, indices, drawcount);
    for (GLsizei i = 0; i < drawcount; i += 1) {
        _bolt_gl_onDrawElements(mode, count[i], type, indices[i]);
    }
    LOG("glMultiDrawElements end\n");
}

static void _bolt_glGenVertexArrays(GLsizei n, GLuint* arrays) {
    LOG("glGenVertexArrays\n");
    gl.GenVertexArrays(n, arrays);
    struct GLContext* c = _bolt_context();
    GLint attrib_count;
    gl.GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attrib_count);
    _bolt_rwlock_lock_write(&c->vaos->rwlock);
    for (GLsizei i = 0; i < n; i += 1) {
        struct GLVertexArray* array = malloc(sizeof(struct GLVertexArray));
        array->id = arrays[i];
        array->attributes = calloc(attrib_count, sizeof(struct GLAttrBinding));
        hashmap_set(c->vaos->map, &array);
    }
    _bolt_rwlock_unlock_write(&c->vaos->rwlock);
    LOG("glGenVertexArrays end\n");
}

static void _bolt_glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
    LOG("glDeleteVertexArrays\n");
    gl.DeleteVertexArrays(n, arrays);
    struct GLContext* c = _bolt_context();
    _bolt_rwlock_lock_write(&c->vaos->rwlock);
    for (GLsizei i = 0; i < n; i += 1) {
        const GLuint* ptr = &arrays[i];
        struct GLVertexArray* const* vao = hashmap_delete(c->vaos->map, &ptr);
        free((*vao)->attributes);
        free(*vao);
    }
    _bolt_rwlock_unlock_write(&c->vaos->rwlock);
    LOG("glDeleteVertexArrays end\n");
}

static void _bolt_glBindVertexArray(GLuint array) {
    LOG("glBindVertexArray\n");
    gl.BindVertexArray(array);
    struct GLContext* c = _bolt_context();
    c->bound_vao = _bolt_context_get_vao(c, array);
    LOG("glBindVertexArray end\n");
}

static void _bolt_glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    LOG("glBlitFramebuffer\n");
    gl.BlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    struct GLContext* c = _bolt_context();
    if (c->current_draw_framebuffer == 0 && c->game_view_framebuffer != c->current_read_framebuffer) {
        c->game_view_framebuffer = c->current_read_framebuffer;
        c->game_view_x = dstX0;
        c->game_view_y = dstY0;
        c->need_3d_tex = 1;
        printf("new game_view_framebuffer %u...\n", c->current_read_framebuffer);
    } else if (c->need_3d_tex) {
        int draw_tex;
        gl.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &draw_tex);
        if (draw_tex == c->game_view_tex) {
            c->need_3d_tex = 0;
            c->game_view_w = dstX1 - dstX0;
            c->game_view_h = dstY1 - dstY0;
            gl.GetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &c->target_3d_tex);
            printf("new target_3d_tex %i\n", c->target_3d_tex);
        }
    }
    LOG("glBlitFramebuffer end\n");
}

void* _bolt_gl_GetProcAddress(const char* name) {
#define PROC_ADDRESS_MAP(FUNC) if (!strcmp(name, "gl"#FUNC)) { return gl.FUNC ? _bolt_gl##FUNC : NULL; }
    PROC_ADDRESS_MAP(CreateProgram)
    PROC_ADDRESS_MAP(DeleteProgram)
    PROC_ADDRESS_MAP(BindAttribLocation)
    PROC_ADDRESS_MAP(LinkProgram)
    PROC_ADDRESS_MAP(UseProgram)
    PROC_ADDRESS_MAP(TexStorage2D)
    PROC_ADDRESS_MAP(VertexAttribPointer)
    PROC_ADDRESS_MAP(GenBuffers)
    PROC_ADDRESS_MAP(BufferData)
    PROC_ADDRESS_MAP(DeleteBuffers)
    PROC_ADDRESS_MAP(BindFramebuffer)
    PROC_ADDRESS_MAP(CompressedTexSubImage2D)
    PROC_ADDRESS_MAP(CopyImageSubData)
    PROC_ADDRESS_MAP(EnableVertexAttribArray)
    PROC_ADDRESS_MAP(DisableVertexAttribArray)
    PROC_ADDRESS_MAP(MapBufferRange)
    PROC_ADDRESS_MAP(UnmapBuffer)
    PROC_ADDRESS_MAP(BufferStorage)
    PROC_ADDRESS_MAP(FlushMappedBufferRange)
    PROC_ADDRESS_MAP(ActiveTexture)
    PROC_ADDRESS_MAP(MultiDrawElements)
    PROC_ADDRESS_MAP(GenVertexArrays)
    PROC_ADDRESS_MAP(DeleteVertexArrays)
    PROC_ADDRESS_MAP(BindVertexArray)
    PROC_ADDRESS_MAP(BlitFramebuffer)
#undef PROC_ADDRESS_MAP
    return NULL;
}

void _bolt_gl_onSwapBuffers(uint32_t window_width, uint32_t window_height) {
    gl_width = window_width;
    gl_height = window_height;
    if (_bolt_plugin_is_inited()) _bolt_plugin_process_windows(window_width, window_height);
}

void _bolt_gl_onCreateContext(void* context, void* shared_context, const struct GLLibFunctions* libgl, void* (*GetProcAddress)(const char*), bool is_important) {
    if (!shared_context && is_important) {
        lgl = libgl;
        if (egl_init_count == 0) {
            _bolt_gl_load(GetProcAddress);
        } else {
            egl_main_context = (uintptr_t)context;
            egl_main_context_makecurrent_pending = 1;
        }
        egl_init_count += 1;
    }
    _bolt_create_context(context, shared_context);
}

void _bolt_gl_onMakeCurrent(void* context) {
    struct GLContext* const current_context = _bolt_context();
    if (current_context) {
        current_context->is_attached = 0;
        if (current_context->deferred_destroy) _bolt_destroy_context(current_context);
    }
    if (!context) {
        _bolt_set_context(NULL);
        return;
    }
    for (size_t i = 0; i < CONTEXTS_CAPACITY; i += 1) {
        struct GLContext* const ptr = &contexts[i];
        if (ptr->id == (uintptr_t)context) {
            _bolt_set_context(ptr);
            break;
        }
    }
    if (egl_main_context_makecurrent_pending && (uintptr_t)context == egl_main_context) {
        egl_main_context_makecurrent_pending = 0;
        _bolt_gl_init();
        const struct PluginManagedFunctions functions = {
            .surface_init = _bolt_gl_plugin_surface_init,
            .surface_destroy = _bolt_gl_plugin_surface_destroy,
            .surface_resize_and_clear = _bolt_gl_plugin_surface_resize,
        };
        _bolt_plugin_init(&functions);
    }
}

void* _bolt_gl_onDestroyContext(void* context) {
    uint8_t do_destroy_main = 0;
    if ((uintptr_t)context != egl_main_context) {
        _bolt_destroy_context(context);
    } else {
        egl_main_context_destroy_pending = 1;
    }
    if (_bolt_context_count() == 1 && egl_main_context_destroy_pending) {
        do_destroy_main = 1;
        if (egl_init_count > 1) _bolt_plugin_close();
    }
    return do_destroy_main ? (void*)egl_main_context : NULL;
}

void _bolt_gl_onGenTextures(GLsizei n, GLuint* textures) {
    struct GLContext* c = _bolt_context();
    _bolt_rwlock_lock_write(&c->textures->rwlock);
    for (GLsizei i = 0; i < n; i += 1) {
        struct GLTexture2D* tex = calloc(1, sizeof(struct GLTexture2D));
        tex->id = textures[i];
        tex->is_minimap_tex_big = 0;
        tex->is_minimap_tex_small = 0;
        hashmap_set(c->textures->map, &tex);
    }
    _bolt_rwlock_unlock_write(&c->textures->rwlock);
}

void _bolt_gl_onDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices_offset) {
    struct GLContext* c = _bolt_context();
    struct GLAttrBinding* attributes = c->bound_vao->attributes;
    GLint element_binding;
    gl.GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_binding);
    struct GLArrayBuffer* element_buffer = _bolt_context_get_buffer(c, element_binding);
    const unsigned short* indices = (unsigned short*)((uint8_t*)element_buffer->data + (uintptr_t)indices_offset);
    if (type == GL_UNSIGNED_SHORT && mode == GL_TRIANGLES && count > 0 && c->bound_program->is_2d && !c->bound_program->is_minimap) {
        GLint diffuse_map;
        gl.GetUniformiv(c->bound_program->id, c->bound_program->loc_uDiffuseMap, &diffuse_map);
        GLfloat projection_matrix[16];
        gl.GetUniformfv(c->bound_program->id, c->bound_program->loc_uProjectionMatrix, projection_matrix);
        GLint draw_tex;
        gl.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &draw_tex);
        struct GLTexture2D* tex = c->texture_units[diffuse_map];
        struct GLTexture2D* tex_target = _bolt_context_get_texture(c, draw_tex);

        if (tex->is_minimap_tex_big) {
            tex_target->is_minimap_tex_small = 1;
            if (count == 6) {
                // get XY and UV of first two vertices
                const struct GLAttrBinding* tex_uv = &attributes[c->bound_program->loc_aTextureUV];
                const struct GLAttrBinding* position_2d = &attributes[c->bound_program->loc_aVertexPosition2D];
                int32_t pos0[2];
                int32_t pos1[2];
                float uv0[2];
                float uv1[2];
                _bolt_get_attr_binding_int(c, position_2d, indices[0], 2, pos0);
                _bolt_get_attr_binding_int(c, position_2d, indices[1], 2, pos1);
                _bolt_get_attr_binding(c, tex_uv, indices[0], 2, uv0);
                _bolt_get_attr_binding(c, tex_uv, indices[1], 2, uv1);
                const double x0 = (double)pos0[0];
                const double y0 = (double)pos0[1];
                const double x1 = (double)pos1[0];
                const double y1 = (double)pos1[1];
                const double u0 = (double)uv0[0];
                const double v0 = (double)uv0[1];
                const double u1 = (double)uv1[0];
                const double v1 = (double)uv1[1];

                // find out angle of the map, by comparing atan2 of XYs to atan2 of UVs
                double pos_angle_rads = atan2(y1 - y0, x1 - x0);
                double uv_angle_rads = atan2(v1 - v0, u1 - u0);
                if (pos_angle_rads < uv_angle_rads) pos_angle_rads += M_PI * 2.0;
                const double map_angle_rads = pos_angle_rads - uv_angle_rads;

                // find out scaling of the map, by comparing distance between XYs to distance between UVs
                const double pos_a = x0 - x1;
                const double pos_b = y0 - y1;
                const double uv_a = u0 - u1;
                const double uv_b = v0 - v1;
                const double pos_dist = sqrt(fabs((pos_a * pos_a) + (pos_b * pos_b)));
                const double uv_dist = sqrt(fabs((uv_a * uv_a) + (uv_b * uv_b))) * tex->width;
                // sometimes it renders the same UV coordinate for the first two vertices, we can't
                // work with that. seems to happen only once immediately after logging into a world.
                if (uv_dist >= 1.0) {
                    const double dist_ratio = pos_dist / uv_dist;

                    // by unrotating the big tex's vertices around any point, and rotating small-tex-relative
                    // points by the same method, we can estimate what section of the big tex will be drawn.
                    // note: `>> 1` is the same as `/ 2` except that it doesn't cause an erroneous gcc warning
                    const double scaled_x = tex->width * dist_ratio;
                    const double scaled_y = tex->height * dist_ratio;
                    const double angle_sin = sin(-map_angle_rads);
                    const double angle_cos = cos(-map_angle_rads);
                    const double unrotated_x0 = (x0 * angle_cos) - (y0 * angle_sin);
                    const double unrotated_y0 = (x0 * angle_sin) + (y0 * angle_cos);
                    const double scaled_xoffset = (u0 * scaled_x) - unrotated_x0;
                    const double scaled_yoffset = (v0 * scaled_y) - unrotated_y0;
                    const double small_tex_cx = 1.0 / projection_matrix[0];
                    const double small_tex_cy = 1.0 / projection_matrix[5];
                    const double cx = (scaled_xoffset + (small_tex_cx * angle_cos) - (small_tex_cy * angle_sin)) / dist_ratio;
                    const double cy = (scaled_yoffset + (small_tex_cx * angle_sin) + (small_tex_cy * angle_cos)) / dist_ratio;

                    struct RenderMinimapEvent render;
                    render.angle = map_angle_rads;
                    render.scale = dist_ratio;
                    render.x = tex->minimap_center_x + (64.0 * (cx - (double)(tex->width >> 1)));
                    render.y = tex->minimap_center_y + (64.0 * (cy - (double)(tex->height >> 1)));
                    _bolt_plugin_handle_minimap(&render);
                }
            }
        } else {
            struct GLPluginDrawElementsVertex2DUserData vertex_userdata;
            vertex_userdata.c = c;
            vertex_userdata.indices = (unsigned short*)((uint8_t*)element_buffer->data + (uintptr_t)indices_offset);
            vertex_userdata.atlas = c->texture_units[diffuse_map];
            vertex_userdata.position = &attributes[c->bound_program->loc_aVertexPosition2D];
            vertex_userdata.atlas_min = &attributes[c->bound_program->loc_aTextureUVAtlasMin];
            vertex_userdata.atlas_size = &attributes[c->bound_program->loc_aTextureUVAtlasExtents];
            vertex_userdata.tex_uv = &attributes[c->bound_program->loc_aTextureUV];
            vertex_userdata.colour = &attributes[c->bound_program->loc_aVertexColour];

            struct GLPluginTextureUserData tex_userdata;
            tex_userdata.tex = tex;

            struct RenderBatch2D batch;
            batch.screen_width = roundf(2.0 / projection_matrix[0]);
            batch.screen_height = roundf(2.0 / projection_matrix[5]);
            batch.index_count = count;
            batch.vertices_per_icon = 6;
            batch.is_minimap = tex_target && tex_target->is_minimap_tex_small;
            batch.vertex_functions.userdata = &vertex_userdata;
            batch.vertex_functions.xy = _bolt_gl_plugin_drawelements_vertex2d_xy;
            batch.vertex_functions.atlas_xy = _bolt_gl_plugin_drawelements_vertex2d_atlas_xy;
            batch.vertex_functions.atlas_wh = _bolt_gl_plugin_drawelements_vertex2d_atlas_wh;
            batch.vertex_functions.uv = _bolt_gl_plugin_drawelements_vertex2d_uv;
            batch.vertex_functions.colour = _bolt_gl_plugin_drawelements_vertex2d_colour;
            batch.texture_functions.userdata = &tex_userdata;
            batch.texture_functions.id = _bolt_gl_plugin_texture_id;
            batch.texture_functions.size = _bolt_gl_plugin_texture_size;
            batch.texture_functions.compare = _bolt_gl_plugin_texture_compare;
            batch.texture_functions.data = _bolt_gl_plugin_texture_data;

            _bolt_plugin_handle_render2d(&batch);
        }
    }
    if (type == GL_UNSIGNED_SHORT && mode == GL_TRIANGLES && c->bound_program->is_3d) {
        GLint draw_tex;
        gl.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &draw_tex);
        if (draw_tex == c->target_3d_tex) {
            GLint atlas, settings_atlas, ubo_binding, ubo_view_index, ubo_viewport_index;
            GLfloat atlas_meta[4];
            gl.GetUniformiv(c->bound_program->id, c->bound_program->loc_uTextureAtlas, &atlas);
            gl.GetUniformiv(c->bound_program->id, c->bound_program->loc_uTextureAtlasSettings, &settings_atlas);
            gl.GetUniformfv(c->bound_program->id, c->bound_program->loc_uAtlasMeta, atlas_meta);
            struct GLTexture2D* tex = c->texture_units[atlas];
            struct GLTexture2D* tex_settings = c->texture_units[settings_atlas];
            gl.GetActiveUniformBlockiv(c->bound_program->id, c->bound_program->block_index_ViewTransforms, GL_UNIFORM_BLOCK_BINDING, &ubo_binding);
            gl.GetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, ubo_binding, &ubo_view_index);
            const float* view_proj_matrix = (float*)((uint8_t*)(_bolt_context_get_buffer(c, ubo_view_index)->data) + c->bound_program->offset_uViewProjMatrix);

            struct GLPluginDrawElementsVertex3DUserData vertex_userdata;
            vertex_userdata.c = c;
            vertex_userdata.indices = (unsigned short*)((uint8_t*)element_buffer->data + (uintptr_t)indices_offset);
            vertex_userdata.atlas_scale = roundf(atlas_meta[1]);
            vertex_userdata.atlas = tex;
            vertex_userdata.settings_atlas = tex_settings;
            vertex_userdata.xy_xz = &attributes[c->bound_program->loc_aMaterialSettingsSlotXY_TilePositionXZ];
            vertex_userdata.xyz_bone = &attributes[c->bound_program->loc_aVertexPosition_BoneLabel];
            vertex_userdata.tex_uv = &attributes[c->bound_program->loc_aTextureUV];
            vertex_userdata.colour = &attributes[c->bound_program->loc_aVertexColour];

            struct GLPluginTextureUserData tex_userdata;
            tex_userdata.tex = tex;

            struct GLPlugin3DMatrixUserData matrix_userdata;
            gl.GetUniformfv(c->bound_program->id, c->bound_program->loc_uModelMatrix, matrix_userdata.model_matrix);
            memcpy(matrix_userdata.viewproj_matrix, view_proj_matrix, 16 * sizeof(float));

            struct Render3D render;
            render.vertex_count = count;
            render.is_animated = c->bound_program->loc_uBoneTransforms != -1;
            render.vertex_functions.userdata = &vertex_userdata;
            render.vertex_functions.xyz = _bolt_gl_plugin_drawelements_vertex3d_xyz;
            render.vertex_functions.atlas_meta = _bolt_gl_plugin_drawelements_vertex3d_atlas_meta;
            render.vertex_functions.atlas_xywh = _bolt_gl_plugin_drawelements_vertex3d_meta_xywh;
            render.vertex_functions.uv = _bolt_gl_plugin_drawelements_vertex3d_uv;
            render.vertex_functions.colour = _bolt_gl_plugin_drawelements_vertex3d_colour;
            render.vertex_functions.bone_id = _bolt_gl_plugin_drawelements_vertex3d_boneid;
            render.vertex_functions.bone_transform = _bolt_gl_plugin_bone_transform;
            render.texture_functions.userdata = &tex_userdata;
            render.texture_functions.id = _bolt_gl_plugin_texture_id;
            render.texture_functions.size = _bolt_gl_plugin_texture_size;
            render.texture_functions.compare = _bolt_gl_plugin_texture_compare;
            render.texture_functions.data = _bolt_gl_plugin_texture_data;
            render.matrix_functions.userdata = &matrix_userdata;
            render.matrix_functions.to_world_space = _bolt_gl_plugin_matrix3d_toworldspace;
            render.matrix_functions.to_screen_space = _bolt_gl_plugin_matrix3d_toscreenspace;
            render.matrix_functions.world_pos = _bolt_gl_plugin_matrix3d_worldpos;

            _bolt_plugin_handle_render3d(&render);
        }
    }
}

void _bolt_gl_onDrawArrays(GLenum mode, GLint first, GLsizei count) {
    struct GLContext* c = _bolt_context();
    GLint draw_tex;
    gl.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &draw_tex);
    struct GLTexture2D* tex = _bolt_context_get_texture(c, draw_tex);
    if (c->bound_program->is_minimap && tex->width == GAME_MINIMAP_BIG_SIZE && tex->height == GAME_MINIMAP_BIG_SIZE) {
        GLint ubo_binding, ubo_view_index;
        gl.GetActiveUniformBlockiv(c->bound_program->id, c->bound_program->block_index_ViewTransforms, GL_UNIFORM_BLOCK_BINDING, &ubo_binding);
        gl.GetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, (GLuint)ubo_binding, &ubo_view_index);
        const float* camera_position = (float*)((uint8_t*)(_bolt_context_get_buffer(c, ubo_view_index)->data) + c->bound_program->offset_uCameraPosition);
        tex->is_minimap_tex_big = 1;
        tex->minimap_center_x = camera_position[0];
        tex->minimap_center_y = camera_position[2];
    } else if (mode == GL_TRIANGLE_STRIP && count == 4) {
        if (c->bound_program->loc_sSceneHDRTex != -1) {
            GLint game_view_tex;
            gl.GetUniformiv(c->bound_program->id, c->bound_program->loc_sSceneHDRTex, &game_view_tex);
            if (c->current_draw_framebuffer == 0 && c->game_view_tex_front != c->texture_units[game_view_tex]->id) {
                c->game_view_tex = c->texture_units[game_view_tex]->id;
                c->game_view_tex_front = c->game_view_tex;
                c->current_draw_framebuffer = -1;
                c->game_view_x = 0;
                c->game_view_y = 0;
                c->need_3d_tex = 1;
                printf("new direct game_view_tex %u...\n", c->game_view_tex);
            } else if (c->need_3d_tex == 1 && c->game_view_framebuffer == c->current_draw_framebuffer) {
                c->game_view_tex = c->texture_units[game_view_tex]->id;
                c->game_view_tex_front = c->game_view_tex;
                printf("new game_view_tex %u...\n", c->game_view_tex);
            }
        } else if (c->bound_program->loc_sSourceTex != -1) {
            if (c->need_3d_tex == 1 && draw_tex == c->game_view_tex) {
                GLint game_view_tex;
                gl.GetUniformiv(c->bound_program->id, c->bound_program->loc_sSourceTex, &game_view_tex);
                c->game_view_tex = c->texture_units[game_view_tex]->id;
                printf("updated direct game_view_tex to %u...\n", c->game_view_tex);
                c->need_3d_tex += 1;
            }

        }
    }
}

void _bolt_gl_onBindTexture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D) {
        struct GLContext* c = _bolt_context();
        c->texture_units[c->active_texture] = _bolt_context_get_texture(c, texture);
    }
}

void _bolt_gl_onTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels) {
    struct GLContext* c = _bolt_context();
    if (target == GL_TEXTURE_2D && level == 0 && format == GL_RGBA) {
        struct GLTexture2D* tex = c->texture_units[c->active_texture];
        if (tex && !(xoffset < 0 || yoffset < 0 || xoffset + width > tex->width || yoffset + height > tex->height)) {
            for (GLsizei y = 0; y < height; y += 1) {
                uint8_t* dest_ptr = tex->data + ((tex->width * (y + yoffset)) + xoffset) * 4;
                const uint8_t* src_ptr = (uint8_t*)pixels + (width * y * 4);
                memcpy(dest_ptr, src_ptr, width * 4);
            }
        }
    }
}

void _bolt_gl_onDeleteTextures(GLsizei n, const GLuint* textures) {
    struct GLContext* c = _bolt_context();
    _bolt_rwlock_lock_write(&c->textures->rwlock);
    for (GLsizei i = 0; i < n; i += 1) {
        const GLuint* ptr = &textures[i];
        struct GLTexture2D* const* texture = hashmap_delete(c->textures->map, &ptr);
        free((*texture)->data);
        free(*texture);
    }
    _bolt_rwlock_unlock_write(&c->textures->rwlock);
}

void _bolt_gl_onClear(GLbitfield mask) {
    struct GLContext* c = _bolt_context();
    if (mask & GL_COLOR_BUFFER_BIT) {
        GLint draw_tex;
        gl.GetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &draw_tex);
        struct GLTexture2D* tex = _bolt_context_get_texture(c, draw_tex);
        if (tex) {
            tex->is_minimap_tex_big = 0;
        }
    }
}

void _bolt_gl_onViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    struct GLContext* c = _bolt_context();
    c->viewport_x = x;
    c->viewport_y = y;
    c->viewport_w = width;
    c->viewport_h = height;
}

static void _bolt_gl_plugin_drawelements_vertex2d_xy(size_t index, void* userdata, int32_t* out) {
    struct GLPluginDrawElementsVertex2DUserData* data = userdata;
    if (!_bolt_get_attr_binding_int(data->c, data->position, data->indices[index], 2, out)) {
        float pos[2];
        _bolt_get_attr_binding(data->c, data->position, data->indices[index], 2, pos);
        out[0] = (int32_t)roundf(pos[0]);
        out[1] = (int32_t)roundf(pos[1]);
    }
}

static void _bolt_gl_plugin_drawelements_vertex2d_atlas_xy(size_t index, void* userdata, int32_t* out) {
    struct GLPluginDrawElementsVertex2DUserData* data = userdata;
    float xy[2];
    _bolt_get_attr_binding(data->c, data->atlas_min, data->indices[index], 2, xy);
    out[0] = (int32_t)roundf(xy[0] * data->atlas->width);
    out[1] = (int32_t)roundf(xy[1] * data->atlas->height);
}

static void _bolt_gl_plugin_drawelements_vertex2d_atlas_wh(size_t index, void* userdata, int32_t* out) {
    struct GLPluginDrawElementsVertex2DUserData* data = userdata;
    float wh[2];
    _bolt_get_attr_binding(data->c, data->atlas_size, data->indices[index], 2, wh);
    // these are negative for some reason
    out[0] = -(int32_t)roundf(wh[0] * data->atlas->width);
    out[1] = -(int32_t)roundf(wh[1] * data->atlas->height);
}

static void _bolt_gl_plugin_drawelements_vertex2d_uv(size_t index, void* userdata, double* out) {
    struct GLPluginDrawElementsVertex2DUserData* data = userdata;
    float uv[2];
    _bolt_get_attr_binding(data->c, data->tex_uv, data->indices[index], 2, uv);
    out[0] = (double)uv[0];
    out[1] = (double)uv[1];
}

static void _bolt_gl_plugin_drawelements_vertex2d_colour(size_t index, void* userdata, double* out) {
    struct GLPluginDrawElementsVertex2DUserData* data = userdata;
    float colour[4];
    _bolt_get_attr_binding(data->c, data->colour, data->indices[index], 4, colour);
    // these are ABGR for some reason
    out[0] = (double)colour[3];
    out[1] = (double)colour[2];
    out[2] = (double)colour[1];
    out[3] = (double)colour[0];
}

static void _bolt_gl_plugin_drawelements_vertex3d_xyz(size_t index, void* userdata, int32_t* out) {
    struct GLPluginDrawElementsVertex3DUserData* data = userdata;
    if (!_bolt_get_attr_binding_int(data->c, data->xyz_bone, data->indices[index], 3, out)) {
        float pos[3];
        _bolt_get_attr_binding(data->c, data->xyz_bone, data->indices[index], 3, pos);
        out[0] = (int32_t)roundf(pos[0]);
        out[1] = (int32_t)roundf(pos[1]);
        out[2] = (int32_t)roundf(pos[2]);
    }
}

static size_t _bolt_gl_plugin_drawelements_vertex3d_atlas_meta(size_t index, void* userdata) {
    struct GLPluginDrawElementsVertex3DUserData* data = userdata;
    int material_xy[2];
    _bolt_get_attr_binding_int(data->c, data->xy_xz, data->indices[index], 2, material_xy);
    return ((size_t)material_xy[1] << 16) | (size_t)material_xy[0];
}

static void _bolt_gl_plugin_drawelements_vertex3d_meta_xywh(size_t meta, void* userdata, int32_t* out) {
    struct GLPluginDrawElementsVertex3DUserData* data = userdata;
    size_t slot_x = meta & 0xFF;
    size_t slot_y = meta >> 16;
    // this is pretty wild
    const uint8_t* settings_ptr = data->settings_atlas->data + (slot_y * data->settings_atlas->width * 4 * 4) + (slot_x * 3 * 4);
    const uint8_t bitmask = *(settings_ptr + (data->settings_atlas->width * 2 * 4) + 7);
    out[0] = ((int32_t)(*settings_ptr) + (bitmask & 1 ? 256 : 0)) * data->atlas_scale;
    out[1] = ((int32_t)*(settings_ptr + 1) + (bitmask & 2 ? 256 : 0)) * data->atlas_scale;
    out[2] = (int32_t)*(settings_ptr + 8) * data->atlas_scale;
    out[3] = out[2];
}

static void _bolt_gl_plugin_drawelements_vertex3d_uv(size_t index, void* userdata, double* out) {
    struct GLPluginDrawElementsVertex3DUserData* data = userdata;
    float uv[2];
    _bolt_get_attr_binding(data->c, data->tex_uv, data->indices[index], 2, uv);
    out[0] = (double)uv[0];
    out[1] = (double)uv[1];
}

static void _bolt_gl_plugin_drawelements_vertex3d_colour(size_t index, void* userdata, double* out) {
    struct GLPluginDrawElementsVertex3DUserData* data = userdata;
    float colour[4];
    _bolt_get_attr_binding(data->c, data->colour, data->indices[index], 4, colour);
    // these are ABGR for some reason
    out[0] = (double)colour[3];
    out[1] = (double)colour[2];
    out[2] = (double)colour[1];
    out[3] = (double)colour[0];
}

static uint8_t _bolt_gl_plugin_drawelements_vertex3d_boneid(size_t index, void* userdata) {
    struct GLPluginDrawElementsVertex3DUserData* data = userdata;
    uint32_t ret[4];
    if (!_bolt_get_attr_binding_int(data->c, data->xyz_bone, data->indices[index], 4, (int32_t*)&ret)) {
        float retf[4];
        _bolt_get_attr_binding(data->c, data->xyz_bone, data->indices[index], 4, retf);
        return (uint8_t)(uint32_t)(int32_t)roundf(retf[3]);
    }
    return (uint8_t)(ret[3]);
}

static void _bolt_gl_plugin_bone_transform(uint8_t bone_id, void* userdata, double* out) {
    struct GLContext* c = _bolt_context();
    const GLint uniform_loc = c->bound_program->loc_uBoneTransforms + (bone_id * 3);
    GLfloat f[4];
    gl.GetUniformfv(c->bound_program->id, uniform_loc, f);
    out[0] = (double)f[0];
    out[1] = (double)f[1];
    out[2] = (double)f[2];
    out[3] = 0.0;
    out[4] = (double)f[3];
    gl.GetUniformfv(c->bound_program->id, uniform_loc + 1, f);
    out[5] = (double)f[0];
    out[6] = (double)f[1];
    out[7] = 0.0;
    out[8] = (double)f[2];
    out[9] = (double)f[3];
    gl.GetUniformfv(c->bound_program->id, uniform_loc + 2, f);
    out[10] = (double)f[0];
    out[11] = 0.0;
    out[12] = (double)f[1];
    out[13] = (double)f[2];
    out[14] = (double)f[3];
    out[15] = 1.0;
}

static void _bolt_gl_plugin_matrix3d_toworldspace(int x, int y, int z, void* userdata, double* out) {
    const struct GLPlugin3DMatrixUserData* data = userdata;
    const double dx = (double)x;
    const double dy = (double)y;
    const double dz = (double)z;
    const float* mmx = data->model_matrix;
    const double outx = (dx * (double)mmx[0]) + (dy * mmx[4]) + (dz * mmx[8]) + mmx[12];
    const double outy = (dx * (double)mmx[1]) + (dy * mmx[5]) + (dz * mmx[9]) + mmx[13];
    const double outz = (dx * (double)mmx[2]) + (dy * mmx[6]) + (dz * mmx[10]) + mmx[14];
    const double homogenous = (dx * (double)mmx[3]) + (dy * mmx[7]) + (dz * mmx[11]) + mmx[15];
    out[0] = outx / homogenous;
    out[1] = outy / homogenous;
    out[2] = outz / homogenous;
}

static void _bolt_gl_plugin_matrix3d_toscreenspace(int x, int y, int z, void* userdata, double* out) {
    const struct GLContext* c = _bolt_context();
    struct GLPlugin3DMatrixUserData* data = userdata;
    const double dx = (double)x;
    const double dy = (double)y;
    const double dz = (double)z;
    const float* mmx = data->model_matrix;
    const float* vpmx = data->viewproj_matrix;
    const double mx = (dx * (double)mmx[0]) + (dy * (double)mmx[4]) + (dz * (double)mmx[8]) + (double)mmx[12];
    const double my = (dx * (double)mmx[1]) + (dy * (double)mmx[5]) + (dz * (double)mmx[9]) + (double)mmx[13];
    const double mz = (dx * (double)mmx[2]) + (dy * (double)mmx[6]) + (dz * (double)mmx[10]) + (double)mmx[14];
    const double mh = (dx * (double)mmx[3]) + (dy * (double)mmx[7]) + (dz * (double)mmx[11]) + (double)mmx[15];
    const double outx = (mx * (double)vpmx[0]) + (my * (double)vpmx[4]) + (mz * (double)vpmx[8]) + (mh * (double)vpmx[12]);
    const double outy = (mx * (double)vpmx[1]) + (my * (double)vpmx[5]) + (mz * (double)vpmx[9]) + (mh * (double)vpmx[13]);
    const double homogenous = (mx * (double)vpmx[3]) + (my * (double)vpmx[7]) + (mz * (double)vpmx[11]) + (mh * (double)vpmx[15]);
    out[0] = (((outx  / homogenous) + 1.0) * c->game_view_w / 2.0) + (double)c->game_view_x;
    out[1] = (((-outy / homogenous) + 1.0) * c->game_view_h / 2.0) + (double)c->game_view_y;
}

static void _bolt_gl_plugin_matrix3d_worldpos(void* userdata, double* out) {
    struct GLPlugin3DMatrixUserData* data = userdata;
    const float* mmx = data->model_matrix;
    out[0] = (double)data->model_matrix[12];
    out[1] = (double)data->model_matrix[13];
    out[2] = (double)data->model_matrix[14];
}

static size_t _bolt_gl_plugin_texture_id(void* userdata) {
    const struct GLPluginTextureUserData* data = userdata;
    return data->tex->id;
}

static void _bolt_gl_plugin_texture_size(void* userdata, size_t* out) {
    const struct GLPluginTextureUserData* data = userdata;
    out[0] = data->tex->width;
    out[1] = data->tex->height;
}

static uint8_t _bolt_gl_plugin_texture_compare(void* userdata, size_t x, size_t y, size_t len, const unsigned char* data) {
    const struct GLPluginTextureUserData* data_ = userdata;
    const struct GLTexture2D* tex = data_->tex;
    const size_t start_offset = (tex->width * y * 4) + (x * 4);
    if (start_offset + len > tex->width * tex->height * 4) {
        printf(
            "warning: out-of-bounds texture compare attempt: tried to read %zu bytes at %zu,%zu of texture id=%u w,h=%u,%u\n",
            len, x, y, tex->id, tex->width, tex->height
        );
        return 0;
    }
    return !memcmp(tex->data + start_offset, data, len);
}

static uint8_t* _bolt_gl_plugin_texture_data(void* userdata, size_t x, size_t y) {
    const struct GLPluginTextureUserData* data = userdata;
    const struct GLTexture2D* tex = data->tex;
    return tex->data + (tex->width * y * 4) + (x * 4);
}

static void _bolt_gl_plugin_surface_init(struct SurfaceFunctions* functions, unsigned int width, unsigned int height, const void* data) {
    struct PluginSurfaceUserdata* userdata = malloc(sizeof(struct PluginSurfaceUserdata));
    struct GLContext* c = _bolt_context();
    userdata->width = width;
    userdata->height = height;
    _bolt_gl_surface_init_buffers(userdata);
    if (data) {
        lgl->TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);
    } else {
        lgl->ClearColor(0.0, 0.0, 0.0, 0.0);
        lgl->Clear(GL_COLOR_BUFFER_BIT);
    }
    functions->userdata = userdata;
    functions->clear = _bolt_gl_plugin_surface_clear;
    functions->subimage = _bolt_gl_plugin_surface_subimage;
    functions->draw_to_screen = _bolt_gl_plugin_surface_drawtoscreen;
    functions->draw_to_surface = _bolt_gl_plugin_surface_drawtosurface;

    const struct GLTexture2D* original_tex = c->texture_units[c->active_texture];
    lgl->BindTexture(GL_TEXTURE_2D, original_tex ? original_tex->id : 0);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->current_draw_framebuffer);
}

static void _bolt_gl_plugin_surface_destroy(void* _userdata) {
    struct PluginSurfaceUserdata* userdata = _userdata;
    _bolt_gl_surface_destroy_buffers(userdata);
    free(userdata);
}

static void _bolt_gl_plugin_surface_resize(void* _userdata, unsigned int width, unsigned int height) {
    struct PluginSurfaceUserdata* userdata = _userdata;
    _bolt_gl_surface_destroy_buffers(userdata);
    userdata->width = width;
    userdata->height = height;
    _bolt_gl_surface_init_buffers(userdata);
    lgl->ClearColor(0.0, 0.0, 0.0, 0.0);
    lgl->Clear(GL_COLOR_BUFFER_BIT);
}

static void _bolt_gl_plugin_surface_clear(void* _userdata, double r, double g, double b, double a) {
    struct PluginSurfaceUserdata* userdata = _userdata;
    struct GLContext* c = _bolt_context();
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, userdata->framebuffer);
    lgl->ClearColor(r, g, b, a);
    lgl->Clear(GL_COLOR_BUFFER_BIT);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->current_draw_framebuffer);
}

static void _bolt_gl_plugin_surface_subimage(void* _userdata, int x, int y, int w, int h, const void* pixels, uint8_t is_bgra) {
    struct PluginSurfaceUserdata* userdata = _userdata;
    struct GLContext* c = _bolt_context();
    lgl->BindTexture(GL_TEXTURE_2D, userdata->renderbuffer);
    lgl->TexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, is_bgra ? GL_BGRA : GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    const struct GLTexture2D* original_tex = c->texture_units[c->active_texture];
    lgl->BindTexture(GL_TEXTURE_2D, original_tex ? original_tex->id : 0);
}

static void _bolt_gl_plugin_surface_drawtoscreen(void* _userdata, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh) {
    struct PluginSurfaceUserdata* userdata = _userdata;
    struct GLContext* c = _bolt_context();

    gl.UseProgram(program_direct_screen);
    lgl->BindTexture(GL_TEXTURE_2D, userdata->renderbuffer);
    gl.BindVertexArray(program_direct_vao);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl.Uniform1i(program_direct_screen_sampler, c->active_texture);
    gl.Uniform4i(program_direct_screen_d_xywh, dx, dy, dw, dh);
    gl.Uniform4i(program_direct_screen_s_xywh, sx, sy, sw, sh);
    gl.Uniform4i(program_direct_screen_src_wh_dest_wh, userdata->width, userdata->height, gl_width, gl_height);
    lgl->Viewport(0, 0, gl_width, gl_height);
    lgl->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    lgl->Viewport(c->viewport_x, c->viewport_y, c->viewport_w, c->viewport_h);
    const struct GLTexture2D* original_tex = c->texture_units[c->active_texture];
    lgl->BindTexture(GL_TEXTURE_2D, original_tex ? original_tex->id : 0);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->current_draw_framebuffer);
    gl.BindVertexArray(c->bound_vao->id);
    gl.UseProgram(c->bound_program ? c->bound_program->id : 0);
}

static void _bolt_gl_plugin_surface_drawtosurface(void* _userdata, void* _target, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh) {
    struct PluginSurfaceUserdata* userdata = _userdata;
    struct PluginSurfaceUserdata* target = _target;
    struct GLContext* c = _bolt_context();

    gl.UseProgram(program_direct_surface);
    lgl->BindTexture(GL_TEXTURE_2D, userdata->renderbuffer);
    gl.BindVertexArray(program_direct_vao);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, target->framebuffer);
    gl.Uniform1i(program_direct_surface_sampler, c->active_texture);
    gl.Uniform4i(program_direct_surface_d_xywh, dx, dy, dw, dh);
    gl.Uniform4i(program_direct_surface_s_xywh, sx, sy, sw, sh);
    gl.Uniform4i(program_direct_surface_src_wh_dest_wh, userdata->width, userdata->height, target->width, target->height);
    lgl->Viewport(0, 0, target->width, target->height);
    lgl->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    lgl->Viewport(c->viewport_x, c->viewport_y, c->viewport_w, c->viewport_h);
    const struct GLTexture2D* original_tex = c->texture_units[c->active_texture];
    lgl->BindTexture(GL_TEXTURE_2D, original_tex ? original_tex->id : 0);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, c->current_draw_framebuffer);
    gl.BindVertexArray(c->bound_vao->id);
    gl.UseProgram(c->bound_program ? c->bound_program->id : 0);
}
