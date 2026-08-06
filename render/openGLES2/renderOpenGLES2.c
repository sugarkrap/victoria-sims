#include <GLES2/gl2.h>

#include "utils/strings.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "render/meshCamera.h"
#include "victoria/renderInterface.h"

/* Radians a second the camera goes round the model. Settable so a frame
   being compared against another can be taken from the same angle — see the
   note on renderSetCameraOrbitRate for why that is worth a knob. */
static Real32 cameraOrbitRate = RENDER_CAMERA_ORBIT_DEFAULT;
/* Where the orbit starts from, so holding the camera still can hold it
   somewhere worth looking at rather than wherever nought happens to be. */
static Real32 cameraStartAngle = 0.0f;

void renderSetCameraOrbitRate(Real32 radiansPerSecond)
{
    cameraOrbitRate = radiansPerSecond;
}

void renderSetCameraAngle(Real32 radians)
{
    cameraStartAngle = radians;
}


/* Vendor queries for total graphics memory. Neither is core in OpenGL ES 2.0,
   so both are guarded by an extension string check before use. */
#ifndef GL_NVX_GPU_MEMORY_INFO_DEDICATED_VIDMEM
#define GL_NVX_GPU_MEMORY_INFO_DEDICATED_VIDMEM 0x9047
#endif
#ifndef GL_ATI_MEMINFO_TEXTURE_FREE_MEMORY
#define GL_ATI_MEMINFO_TEXTURE_FREE_MEMORY 0x87FC
#endif

static const char *vertexShaderSource =
    "attribute vec2 vertexPosition;\n"
    "attribute vec3 vertexColor;\n"
    "varying vec3 interpolatedColor;\n"
    "void main()\n"
    "{\n"
    "    interpolatedColor = vertexColor;\n"
    "    gl_Position = vec4(vertexPosition, 0.0, 1.0);\n"
    "}\n";

static const char *fragmentShaderSource =
    "precision mediump float;\n"
    "varying vec3 interpolatedColor;\n"
    "uniform float colorPulse;\n"
    "void main()\n"
    "{\n"
    "    gl_FragColor = vec4(interpolatedColor * colorPulse, 1.0);\n"
    "}\n";

/* The overlay program: one quad, one texture, no depth, no lighting, no camera.
 *
 * The blend GL_ONE, GL_ONE_MINUS_SRC_ALPHA gives out = source + scene *
 * (1 - sourceAlpha), which is exactly compositing premultiplied pixels over
 * what is already there — so a panel, a button on it and a letter on that all
 * come out right in one pass, which straight alpha would not manage.
 *
 * The engine hands over pixels that are already premultiplied, so the fragment
 * shader is a plain fetch and the blend does the rest. */
static const char *overlayVertexShaderSource =
    "attribute vec2 overlayPosition;\n"
    "attribute vec2 overlayCoordinate;\n"
    "varying vec2 interpolatedCoordinate;\n"
    "void main()\n"
    "{\n"
    "    interpolatedCoordinate = overlayCoordinate;\n"
    "    gl_Position = vec4(overlayPosition, 0.0, 1.0);\n"
    "}\n";

static const char *overlayFragmentShaderSource =
    "precision mediump float;\n"
    "varying vec2 interpolatedCoordinate;\n"
    "uniform sampler2D overlaySampler;\n"
    "void main()\n"
    "{\n"
    "    gl_FragColor = texture2D(overlaySampler, interpolatedCoordinate);\n"
    "}\n";

static GLuint overlayProgram = 0;
static GLuint overlayTexture = 0;
static GLint overlayPositionLocation = -1;
static GLint overlayCoordinateLocation = -1;
static GLint overlaySamplerLocation = -1;
static Unsigned32 overlayWidth = 0U;
static Unsigned32 overlayHeight = 0U;
static MemorySize overlayChargedBytes = 0UL;
static Unsigned32 viewportWidth = 1U;
static Unsigned32 viewportHeight = 1U;

static const GLfloat triangleVertices[] = {
    /* position */ 0.0f, 0.6f, /* color */ 1.0f, 0.35f, 0.55f,
    /* position */ -0.6f, -0.5f, /* color */ 0.35f, 0.75f, 1.0f,
    /* position */ 0.6f, -0.5f, /* color */ 1.0f, 0.9f, 0.4f
};

/* The mesh program. Lighting is done here rather than baked per face as the
   software backend does, because a shader can interpolate a normal across a
   triangle and get smooth shading for the same cost as flat.

   The light arrives already rotated into model space, so a vertex can be lit
   from its own normal without a normal matrix — one less uniform, and one less
   thing to get wrong when the model turns. */
static const char *meshVertexShaderSource =
    "attribute vec3 vertexPosition;\n"
    "attribute vec3 vertexNormal;\n"
    "attribute vec2 vertexTextureCoordinate;\n"
    "uniform mat4 modelViewProjection;\n"
    "varying vec3 interpolatedNormal;\n"
    "varying vec2 interpolatedTextureCoordinate;\n"
    "void main()\n"
    "{\n"
    "    interpolatedNormal = vertexNormal;\n"
    "    interpolatedTextureCoordinate = vertexTextureCoordinate;\n"
    "    gl_Position = modelViewProjection * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *meshFragmentShaderSource =
    "precision mediump float;\n"
    "varying vec3 interpolatedNormal;\n"
    "varying vec2 interpolatedTextureCoordinate;\n"
    "uniform vec3 lightDirection;\n"
    "uniform sampler2D meshTexture;\n"
    "void main()\n"
    "{\n"
    "    vec3 normal = normalize(interpolatedNormal);\n"
    /* Absolute rather than clamped: nothing here has a back face material, and
       an unlit interior reads as a hole in the model rather than as shadow. */
    "    float lambert = abs(dot(normal, lightDirection));\n"
    "    float shade = 0.28 + (0.72 * lambert);\n"
    /* White when there is no image, so an untextured mesh is lit exactly as it
       was before any of this existed. */
    "    vec4 painted = texture2D(meshTexture, interpolatedTextureCoordinate);\n"
    "    gl_FragColor = vec4(painted.rgb * vec3(shade, shade * 0.93, shade * 0.84), 1.0);\n"
    "}\n";

static GLuint meshProgram = 0;
static GLuint meshVertexBuffer = 0;
static GLuint meshIndexBuffer = 0;
static GLuint meshTextureName = 0;
static MemorySize textureChargedBytes = 0UL;
static GLint meshTextureCoordinateLocation = -1;
static GLint meshSamplerLocation = -1;
static GLint meshPositionLocation = -1;
static GLint meshNormalLocation = -1;
static GLint meshMatrixLocation = -1;
static GLint meshLightLocation = -1;
static GLsizei meshIndexCount = 0;
static MemorySize meshChargedBytes = 0UL;
/* The vertex buffer's size, so an update can refuse a mesh of a different
   shape rather than writing past the end of what was uploaded. */
static MemorySize meshVertexBytes = 0UL;
/* One texture and one index range per part the mesh declared. The ranges are
   copied out of the mesh at upload rather than kept by pointer: the mesh is the
   caller's arena, and a backend holding a pointer into it across frames would
   be trusting storage it does not own. */
static GLuint meshPartTextures[RENDER_PART_LIMIT];
static MemorySize meshPartTextureBytes[RENDER_PART_LIMIT];
static GLsizei meshPartFirstIndex[RENDER_PART_LIMIT];
static GLsizei meshPartIndexCount[RENDER_PART_LIMIT];
static Unsigned32 meshPartCount = 0U;
static MeshCamera meshCamera;

static Real32 viewportAspect = 1.0f;
static GLuint shaderProgram = 0;
static GLuint vertexBuffer = 0;
static GLint vertexPositionLocation = -1;
static GLint vertexColorLocation = -1;
static GLint colorPulseLocation = -1;
static Unsigned32 shaderProgramCount = 0;
static Boolean vertexBufferIsCharged = BOOLEAN_FALSE;

static Boolean extensionIsSupported(const char *extensionName)
{
    const char *extensionList = (const char *)glGetString(GL_EXTENSIONS);
    MemorySize nameLength = stringLength(extensionName);
    MemorySize index = 0UL;

    if (extensionList == NULL_POINTER)
    {
        return BOOLEAN_FALSE;
    }

    /* Substring matching alone would accept a shorter name inside a longer
       one, so each candidate has to sit on a token boundary. */
    while (extensionList[index] != '\0')
    {
        MemorySize tokenLength = 0UL;

        while (extensionList[index] == ' ')
        {
            index += 1UL;
        }
        while (extensionList[index + tokenLength] != ' ' && extensionList[index + tokenLength] != '\0')
        {
            tokenLength += 1UL;
        }

        if (tokenLength == nameLength &&
            memoryCompare(extensionList + index, extensionName, nameLength) == 0)
        {
            return BOOLEAN_TRUE;
        }

        index += tokenLength;
    }

    return BOOLEAN_FALSE;
}

MemorySize renderQueryGraphicsMemoryBytes(void)
{
    GLint reportedKibibytes = 0;

    /* Only meaningful once a context is current; called after that by the
       engine, and harmless before it because the extension check fails. */
    if (extensionIsSupported("GL_NVX_gpu_memory_info") == BOOLEAN_TRUE)
    {
        glGetIntegerv(GL_NVX_GPU_MEMORY_INFO_DEDICATED_VIDMEM, &reportedKibibytes);
    }
    else if (extensionIsSupported("GL_ATI_meminfo") == BOOLEAN_TRUE)
    {
        GLint memoryInformation[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_ATI_MEMINFO_TEXTURE_FREE_MEMORY, memoryInformation);
        reportedKibibytes = memoryInformation[0];
    }

    /* Clear whatever error a driver raised for an enum it does not know. */
    while (glGetError() != GL_NO_ERROR)
    {
        /* Intentionally empty. */
    }

    if (reportedKibibytes <= 0)
    {
        return 0UL;
    }
    return (MemorySize)reportedKibibytes * 1024UL;
}

static GLuint compileShader(GLenum shaderStage, const char *shaderSource)
{
    GLuint shader;
    GLint compileStatus = GL_FALSE;

    shader = glCreateShader(shaderStage);
    if (shader == 0)
    {
        return 0;
    }

    glShaderSource(shader, 1, &shaderSource, NULL_POINTER);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);

    if (compileStatus != GL_TRUE)
    {
        char shaderLog[512];
        glGetShaderInfoLog(shader, (GLsizei)sizeof(shaderLog), NULL_POINTER, shaderLog);
        platformLogMessage("renderer: shader compilation failed");
        platformLogMessage(shaderLog);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static void bindTriangleAttributes(void)
{
    const GLsizei vertexStride = (GLsizei)(5 * sizeof(GLfloat));

    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glEnableVertexAttribArray((GLuint)vertexPositionLocation);
    glVertexAttribPointer((GLuint)vertexPositionLocation, 2, GL_FLOAT, GL_FALSE, vertexStride,
                          (const void *)0);
    glEnableVertexAttribArray((GLuint)vertexColorLocation);
    glVertexAttribPointer((GLuint)vertexColorLocation, 3, GL_FLOAT, GL_FALSE, vertexStride,
                          (const void *)(2 * sizeof(GLfloat)));
}

/* Linking is not the whole cost. Drivers routinely defer real code generation
   until a program is first used to draw, which is why compiling at startup
   alone leaves the stall in place — it just moves it to frame one. Drawing
   once here, with colour writes masked off and a one-pixel viewport, forces
   that work to happen now. */
static void warmUpProgram(GLuint programToWarm)
{
    GLint previousViewport[4];

    VICTORIA_PROFILE_ZONE_BEGIN("renderWarmUpShaders");

    glGetIntegerv(GL_VIEWPORT, previousViewport);

    glUseProgram(programToWarm);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glViewport(0, 0, 1, 1);

    bindTriangleAttributes();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray((GLuint)vertexPositionLocation);
    glDisableVertexAttribArray((GLuint)vertexColorLocation);

    /* The draw is only guaranteed to have been executed once the pipeline has
       drained, and waiting is the entire point of doing it here. */
    glFinish();

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    glUseProgram(0);

    VICTORIA_PROFILE_ZONE_END();
}

Boolean renderInitialize(MemoryArena *arena, Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    GLuint vertexShader;
    GLuint fragmentShader;
    GLint linkStatus = GL_FALSE;

    (void)arena;

    VICTORIA_PROFILE_ZONE_BEGIN("renderCompileShaders");

    vertexShader = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    if (vertexShader == 0)
    {
        VICTORIA_PROFILE_ZONE_END();
        return BOOLEAN_FALSE;
    }

    fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    if (fragmentShader == 0)
    {
        glDeleteShader(vertexShader);
        VICTORIA_PROFILE_ZONE_END();
        return BOOLEAN_FALSE;
    }

    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &linkStatus);

    /* The program holds its own reference once linked. */
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    if (linkStatus != GL_TRUE)
    {
        char programLog[512];
        glGetProgramInfoLog(shaderProgram, (GLsizei)sizeof(programLog), NULL_POINTER, programLog);
        platformLogMessage("renderer: program link failed");
        platformLogMessage(programLog);
        glDeleteProgram(shaderProgram);
        shaderProgram = 0;
        VICTORIA_PROFILE_ZONE_END();
        return BOOLEAN_FALSE;
    }

    shaderProgramCount = 1U;

    vertexPositionLocation = glGetAttribLocation(shaderProgram, "vertexPosition");
    vertexColorLocation = glGetAttribLocation(shaderProgram, "vertexColor");
    colorPulseLocation = glGetUniformLocation(shaderProgram, "colorPulse");

    VICTORIA_PROFILE_ZONE_END();

    if (graphicsMemoryBudgetRequest(GRAPHICS_MEMORY_CATEGORY_BUFFER, sizeof(triangleVertices)) ==
        BOOLEAN_FALSE)
    {
        platformLogMessage("renderer: vertex buffer refused by the graphics memory ceiling");
        glDeleteProgram(shaderProgram);
        shaderProgram = 0;
        shaderProgramCount = 0U;
        return BOOLEAN_FALSE;
    }
    vertexBufferIsCharged = BOOLEAN_TRUE;

    glGenBuffers(1, &vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(triangleVertices), triangleVertices, GL_STATIC_DRAW);

    warmUpProgram(shaderProgram);

    renderResize(widthInPixels, heightInPixels);
    platformLogMessage("renderer: OpenGL ES 2.0 backend ready");
    return BOOLEAN_TRUE;
}

Unsigned32 renderGetShaderProgramCount(void)
{
    return shaderProgramCount;
}

void renderResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    glViewport(0, 0, (GLsizei)widthInPixels, (GLsizei)heightInPixels);
    viewportAspect = (heightInPixels > 0U) ? ((Real32)widthInPixels / (Real32)heightInPixels) : 1.0f;
    /* Kept because the overlay is drawn in pixels rather than in the clip space
       everything else here works in, and there is no way to ask GL what the
       viewport is without a round trip to the driver every frame. */
    viewportWidth = (widthInPixels > 0U) ? widthInPixels : 1U;
    viewportHeight = (heightInPixels > 0U) ? heightInPixels : 1U;
}

void renderSetOverlay(const Unsigned8 *pixels, Unsigned32 widthInPixels,
                      Unsigned32 heightInPixels)
{
    MemorySize wantedBytes;

    if (pixels == NULL_POINTER || widthInPixels == 0U || heightInPixels == 0U)
    {
        overlayWidth = 0U;
        overlayHeight = 0U;
        return;
    }

    if (overlayProgram == 0)
    {
        GLuint vertexShader = compileShader(GL_VERTEX_SHADER, overlayVertexShaderSource);
        GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, overlayFragmentShaderSource);
        GLint linkStatus = GL_FALSE;

        /* Built the first time there is something to draw rather than at
           start-up. A run that never opens the menu never compiles it, and a
           shader compile is the most expensive thing a driver does. */
        if (vertexShader == 0 || fragmentShader == 0)
        {
            return;
        }
        overlayProgram = glCreateProgram();
        glAttachShader(overlayProgram, vertexShader);
        glAttachShader(overlayProgram, fragmentShader);
        glLinkProgram(overlayProgram);
        glGetProgramiv(overlayProgram, GL_LINK_STATUS, &linkStatus);
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        if (linkStatus != GL_TRUE)
        {
            platformLogMessage("renderer: the overlay program would not link, so no text");
            glDeleteProgram(overlayProgram);
            overlayProgram = 0;
            return;
        }
        shaderProgramCount++;
        overlayPositionLocation = glGetAttribLocation(overlayProgram, "overlayPosition");
        overlayCoordinateLocation = glGetAttribLocation(overlayProgram, "overlayCoordinate");
        overlaySamplerLocation = glGetUniformLocation(overlayProgram, "overlaySampler");
    }

    wantedBytes = (MemorySize)widthInPixels * (MemorySize)heightInPixels * 4UL;
    if (wantedBytes > overlayChargedBytes)
    {
        if (graphicsMemoryBudgetRequest(GRAPHICS_MEMORY_CATEGORY_TEXTURE,
                                        wantedBytes - overlayChargedBytes) == BOOLEAN_FALSE)
        {
            platformLogMessage("render: the graphics ceiling will not hold the text overlay");
            return;
        }
    }
    else if (wantedBytes < overlayChargedBytes)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE,
                                    overlayChargedBytes - wantedBytes);
    }
    overlayChargedBytes = wantedBytes;

    if (overlayTexture == 0U)
    {
        glGenTextures(1, &overlayTexture);
    }
    glBindTexture(GL_TEXTURE_2D, overlayTexture);
    /* Four bytes a pixel is always a multiple of four, so the default unpack
       alignment would do — set anyway, because the day this becomes three bytes
       is the day every row after the first is read from the wrong place, and
       that reads as a font problem rather than as this line. */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)widthInPixels, (GLsizei)heightInPixels, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    /* Nearest, deliberately. The overlay is drawn at exactly one texel a pixel
       and linear sampling of that is a blur with no upside. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    overlayWidth = widthInPixels;
    overlayHeight = heightInPixels;
}

/* One quad in the top left corner, a texel to a pixel.
 *
 * Client-side arrays rather than a buffer: the quad changes whenever the window
 * is resized or the menu grows a line, and a buffer respecified every time it
 * moves is a buffer that exists only to be rewritten. */
static void drawOverlay(void)
{
    GLfloat right = -1.0f + (2.0f * (GLfloat)overlayWidth / (GLfloat)viewportWidth);
    GLfloat bottom = 1.0f - (2.0f * (GLfloat)overlayHeight / (GLfloat)viewportHeight);
    const GLfloat quad[] = {
        /* position */ -1.0f, 1.0f,   /* coordinate */ 0.0f, 0.0f,
        /* position */ -1.0f, bottom, /* coordinate */ 0.0f, 1.0f,
        /* position */ right, 1.0f,   /* coordinate */ 1.0f, 0.0f,
        /* position */ right, bottom, /* coordinate */ 1.0f, 1.0f
    };
    const GLsizei stride = (GLsizei)(4 * sizeof(GLfloat));

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    /* Premultiplied alpha, which is what the fragment shader writes and is what
       makes the panel and the letters one pass instead of two. */
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(overlayProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, overlayTexture);
    if (overlaySamplerLocation >= 0)
    {
        glUniform1i(overlaySamplerLocation, 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray((GLuint)overlayPositionLocation);
    glVertexAttribPointer((GLuint)overlayPositionLocation, 2, GL_FLOAT, GL_FALSE, stride, quad);
    glEnableVertexAttribArray((GLuint)overlayCoordinateLocation);
    glVertexAttribPointer((GLuint)overlayCoordinateLocation, 2, GL_FLOAT, GL_FALSE, stride,
                          &quad[2]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint)overlayPositionLocation);
    glDisableVertexAttribArray((GLuint)overlayCoordinateLocation);

    glDisable(GL_BLEND);
}


/* Position, normal and texture coordinate, eight floats a vertex, into storage
   the caller owns. Shared by the upload and the per-frame update so the two
   cannot disagree about the layout the shader was written against. */
static void interleaveMeshVertices(const GeometryMesh *mesh, GLfloat *interleaved)
{
    Unsigned32 index;

    for (index = 0U; index < mesh->vertexCount; index++)
    {
        const Real32 *position = &mesh->positions[(MemorySize)index * 3UL];
        const Real32 *normal = &mesh->normals[(MemorySize)index * 3UL];
        GLfloat *target = &interleaved[(MemorySize)index * 8UL];

        target[0] = (GLfloat)position[0];
        target[1] = (GLfloat)position[1];
        target[2] = (GLfloat)position[2];
        target[3] = (GLfloat)normal[0];
        target[4] = (GLfloat)normal[1];
        target[5] = (GLfloat)normal[2];
        if (mesh->textureCoordinates != NULL_POINTER)
        {
            target[6] = (GLfloat)mesh->textureCoordinates[(MemorySize)index * 2UL];
            target[7] = (GLfloat)mesh->textureCoordinates[(MemorySize)index * 2UL + 1UL];
        }
        else
        {
            target[6] = 0.0f;
            target[7] = 0.0f;
        }
    }
}

/* Hands back the ledger charge and the driver objects the previous mesh took.
   Safe to call when there was no previous mesh: every handle is zero then, and
   the ledger release is clamped rather than trusted. */
static void releaseWhateverTheLastMeshTook(void)
{
    if (meshChargedBytes > 0UL)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, meshChargedBytes);
        meshChargedBytes = 0UL;
    }
    if (meshVertexBuffer != 0U)
    {
        glDeleteBuffers(1, &meshVertexBuffer);
        meshVertexBuffer = 0U;
    }
    if (meshIndexBuffer != 0U)
    {
        glDeleteBuffers(1, &meshIndexBuffer);
        meshIndexBuffer = 0U;
    }
    if (meshProgram != 0U)
    {
        glDeleteProgram(meshProgram);
        meshProgram = 0U;
    }
    {
        Unsigned32 part;

        for (part = 0U; part < RENDER_PART_LIMIT; part++)
        {
            if (meshPartTextures[part] != 0U)
            {
                glDeleteTextures(1, &meshPartTextures[part]);
                meshPartTextures[part] = 0U;
            }
            if (meshPartTextureBytes[part] > 0UL)
            {
                graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE,
                                            meshPartTextureBytes[part]);
                meshPartTextureBytes[part] = 0UL;
            }
        }
        meshPartCount = 0U;
    }
}

/* Re-sends the vertices of a mesh already uploaded, for a model whose vertices
 * moved but whose shape did not — one skinned on the processor each frame.
 *
 * Charges the ledger nothing and compiles nothing, because it reuses the buffer
 * and the program that are already there. renderSetMesh would rebuild both, and
 * rebuilding a program every frame cost twenty-two milliseconds on the frame it
 * happened to land on.
 *
 * Does nothing when no mesh is set or the vertex count has changed, which is
 * not this function's job to handle: that is a different mesh, and the caller
 * wants renderSetMesh for it. */
void renderUpdateMeshVertices(const GeometryMesh *mesh, MemoryArena *arena)
{
    MemorySize marker;
    MemorySize vertexBytes;
    GLfloat *interleaved;

    if (mesh == NULL_POINTER || meshVertexBuffer == 0U || meshIndexCount == 0 ||
        mesh->positions == NULL_POINTER || mesh->normals == NULL_POINTER ||
        (MemorySize)mesh->vertexCount * 8UL * sizeof(GLfloat) != meshVertexBytes)
    {
        return;
    }

    vertexBytes = meshVertexBytes;
    marker = memoryArenaGetMarker(arena);
    interleaved = (GLfloat *)memoryArenaAllocate(arena, vertexBytes, sizeof(GLfloat));
    if (interleaved == NULL_POINTER)
    {
        memoryArenaRewindToMarker(arena, marker);
        return;
    }
    interleaveMeshVertices(mesh, interleaved);

    glBindBuffer(GL_ARRAY_BUFFER, meshVertexBuffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)vertexBytes, interleaved);
    memoryArenaRewindToMarker(arena, marker);
}

void renderSetMesh(const GeometryMesh *mesh, MemoryArena *arena)
{
    MemorySize marker;
    GLfloat *interleaved;
    MemorySize vertexBytes;
    MemorySize indexBytes;
    GLuint vertexShader;
    GLuint fragmentShader;
    GLint linkStatus = GL_FALSE;

    if (mesh == NULL_POINTER || mesh->vertexCount == 0U || mesh->indexCount < 3U ||
        mesh->normals == NULL_POINTER)
    {
        meshIndexCount = 0;
        return;
    }

    /* Whatever the last mesh charged, given back before this one asks.
     *
     * Without this every call charges the ledger afresh and nothing is ever
     * released, which is invisible while a mesh is set once per load and fatal
     * the moment one is re-sent every frame: an animated face ran the buffer
     * category to fifteen megabytes in twelve seconds, and would have reached
     * the ceiling and been refused shortly after. The GL objects went the same
     * way — a program and two buffers a frame, none deleted. */
    releaseWhateverTheLastMeshTook();

    /* Position, normal and texture coordinate: eight floats a vertex. Carried
       even when the mesh has no coordinates, because a layout that changes
       shape per mesh means a program per mesh. */
    vertexBytes = (MemorySize)mesh->vertexCount * 8UL * sizeof(GLfloat);
    indexBytes = (MemorySize)mesh->indexCount * sizeof(GLushort);

    /* Charged before anything is created, so a device that cannot afford the
       mesh keeps its triangle rather than failing part way through. */
    if (graphicsMemoryBudgetRequest(GRAPHICS_MEMORY_CATEGORY_BUFFER, vertexBytes + indexBytes) ==
        BOOLEAN_FALSE)
    {
        platformLogMessage("render: the graphics ceiling will not hold this mesh");
        return;
    }
    meshChargedBytes = vertexBytes + indexBytes;

    vertexShader = compileShader(GL_VERTEX_SHADER, meshVertexShaderSource);
    fragmentShader = compileShader(GL_FRAGMENT_SHADER, meshFragmentShaderSource);
    if (vertexShader == 0U || fragmentShader == 0U)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, meshChargedBytes);
        meshChargedBytes = 0UL;
        return;
    }

    meshProgram = glCreateProgram();
    glAttachShader(meshProgram, vertexShader);
    glAttachShader(meshProgram, fragmentShader);
    glLinkProgram(meshProgram);
    glGetProgramiv(meshProgram, GL_LINK_STATUS, &linkStatus);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    if (linkStatus == GL_FALSE)
    {
        platformLogMessage("render: the mesh program would not link");
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, meshChargedBytes);
        meshChargedBytes = 0UL;
        return;
    }
    shaderProgramCount += 1U;

    if (meshTextureName == 0U)
    {
        /* One white pixel. A sampler with nothing bound reads as black on some
           drivers and as undefined on others, so an untextured mesh would go
           dark rather than staying as it was. */
        static const GLubyte whitePixel[4] = { 255U, 255U, 255U, 255U };

        glGenTextures(1, &meshTextureName);
        glBindTexture(GL_TEXTURE_2D, meshTextureName);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    meshPositionLocation = glGetAttribLocation(meshProgram, "vertexPosition");
    meshNormalLocation = glGetAttribLocation(meshProgram, "vertexNormal");
    meshTextureCoordinateLocation = glGetAttribLocation(meshProgram, "vertexTextureCoordinate");
    meshSamplerLocation = glGetUniformLocation(meshProgram, "meshTexture");
    meshMatrixLocation = glGetUniformLocation(meshProgram, "modelViewProjection");
    meshLightLocation = glGetUniformLocation(meshProgram, "lightDirection");

    /* Interleaved into scratch the arena gives back straight afterwards. The
       driver copies it, so holding it any longer would be two of everything. */
    marker = memoryArenaGetMarker(arena);
    interleaved = (GLfloat *)memoryArenaAllocate(arena, vertexBytes, sizeof(GLfloat));
    if (interleaved == NULL_POINTER)
    {
        platformLogMessage("render: not enough arena to stage the mesh");
        memoryArenaRewindToMarker(arena, marker);
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, meshChargedBytes);
        meshChargedBytes = 0UL;
        return;
    }
    interleaveMeshVertices(mesh, interleaved);

    glGenBuffers(1, &meshVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, meshVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertexBytes, interleaved, GL_STATIC_DRAW);

    glGenBuffers(1, &meshIndexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIndexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)indexBytes, mesh->indices, GL_STATIC_DRAW);

    memoryArenaRewindToMarker(arena, marker);

    meshVertexBytes = vertexBytes;
    {
        Unsigned32 part;

        meshPartCount = (mesh->storedPrimitiveCount < RENDER_PART_LIMIT)
                            ? mesh->storedPrimitiveCount
                            : RENDER_PART_LIMIT;
        for (part = 0U; part < meshPartCount; part++)
        {
            meshPartFirstIndex[part] = (GLsizei)mesh->primitives[part].firstIndex;
            meshPartIndexCount[part] = (GLsizei)mesh->primitives[part].indexCount;
        }
        /* A model with more parts than can be held draws the rest under the
           last range, so nothing goes missing and the join is visible. */
        if (mesh->storedPrimitiveCount > RENDER_PART_LIMIT && meshPartCount > 0U)
        {
            meshPartIndexCount[meshPartCount - 1U] =
                (GLsizei)mesh->indexCount - meshPartFirstIndex[meshPartCount - 1U];
        }
    }
    meshCameraFrame(&meshCamera, mesh);
    meshIndexCount = (GLsizei)mesh->indexCount;
    platformLogMessage("render: mesh uploaded to the OpenGL ES 2.0 backend");
}

/* The mesh is a closed solid seen from outside, so the far side is hidden by
   the near one and the depth buffer sorts the rest. Enabled only while a mesh
   is being drawn, so the placeholder triangle behaves exactly as it always
   did. */
static void drawMesh(Real32 elapsedSeconds)
{
    Real32 matrix[16];
    Real32 light[3];
    Real32 angle = cameraStartAngle + (elapsedSeconds * cameraOrbitRate);
    const GLsizei vertexStride = (GLsizei)(8 * sizeof(GLfloat));

    meshCameraBuildMatrix(&meshCamera, angle, viewportAspect, matrix);
    meshCameraGetLightDirection(angle, light);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    glUseProgram(meshProgram);
    glUniformMatrix4fv(meshMatrixLocation, 1, GL_FALSE, matrix);
    glUniform3f(meshLightLocation, light[0], light[1], light[2]);
    if (meshSamplerLocation >= 0)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, meshTextureName);
        glUniform1i(meshSamplerLocation, 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, meshVertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIndexBuffer);
    glEnableVertexAttribArray((GLuint)meshPositionLocation);
    glVertexAttribPointer((GLuint)meshPositionLocation, 3, GL_FLOAT, GL_FALSE, vertexStride,
                          (const void *)0);
    glEnableVertexAttribArray((GLuint)meshNormalLocation);
    glVertexAttribPointer((GLuint)meshNormalLocation, 3, GL_FLOAT, GL_FALSE, vertexStride,
                          (const void *)(3 * sizeof(GLfloat)));
    if (meshTextureCoordinateLocation >= 0)
    {
        glEnableVertexAttribArray((GLuint)meshTextureCoordinateLocation);
        glVertexAttribPointer((GLuint)meshTextureCoordinateLocation, 2, GL_FLOAT, GL_FALSE,
                              vertexStride, (const void *)(6 * sizeof(GLfloat)));
    }

    /* A part at a time, so each can wear its own skin. A Sim painted in one
       call wears one texture, and with a face's texture on its body the arms
       come out banded with an eyebrow. */
    if (meshPartCount == 0U)
    {
        glDrawElements(GL_TRIANGLES, meshIndexCount, GL_UNSIGNED_SHORT, (const void *)0);
    }
    else
    {
        Unsigned32 part;

        for (part = 0U; part < meshPartCount; part++)
        {
            if (meshPartIndexCount[part] == 0)
            {
                continue;
            }
            if (meshSamplerLocation >= 0)
            {
                /* The part's own texture, or the single one the model was given
                   when it has none of its own. */
                GLuint name = (meshPartTextures[part] != 0U) ? meshPartTextures[part]
                                                             : meshTextureName;

                glBindTexture(GL_TEXTURE_2D, name);
            }
            glDrawElements(GL_TRIANGLES, meshPartIndexCount[part], GL_UNSIGNED_SHORT,
                           (const void *)((MemorySize)meshPartFirstIndex[part] *
                                          sizeof(GLushort)));
        }
    }

    glDisableVertexAttribArray((GLuint)meshPositionLocation);
    glDisableVertexAttribArray((GLuint)meshNormalLocation);
    if (meshTextureCoordinateLocation >= 0)
    {
        glDisableVertexAttribArray((GLuint)meshTextureCoordinateLocation);
    }
    glDisable(GL_DEPTH_TEST);
}

static Boolean isPowerOfTwo(Unsigned32 value)
{
    return (value != 0U && (value & (value - 1U)) == 0U) ? BOOLEAN_TRUE : BOOLEAN_FALSE;
}

void renderSetTexture(const Unsigned8 *rgbaPixels, Unsigned32 widthInPixels,
                      Unsigned32 heightInPixels)
{
    MemorySize wantedBytes;
    GLint wrapMode;

    if (rgbaPixels == NULL_POINTER || widthInPixels == 0U || heightInPixels == 0U)
    {
        return;
    }
    wantedBytes = (MemorySize)widthInPixels * (MemorySize)heightInPixels * 4UL;
    if (graphicsMemoryBudgetRequest(GRAPHICS_MEMORY_CATEGORY_TEXTURE, wantedBytes) == BOOLEAN_FALSE)
    {
        platformLogMessage("render: the graphics ceiling will not hold this texture");
        return;
    }

    if (meshTextureName == 0U)
    {
        glGenTextures(1, &meshTextureName);
    }
    glBindTexture(GL_TEXTURE_2D, meshTextureName);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)widthInPixels, (GLsizei)heightInPixels, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);

    /* OpenGL ES 2.0 will not repeat a texture whose sides are not powers of
       two, and a driver handed one anyway is entitled to sample black. Retail
       textures are almost always powers of two; the ones that are not get
       clamped rather than dropped. */
    wrapMode = (isPowerOfTwo(widthInPixels) && isPowerOfTwo(heightInPixels)) ? GL_REPEAT
                                                                            : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);

    if (textureChargedBytes > 0UL)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE, textureChargedBytes);
    }
    textureChargedBytes = wantedBytes;
    platformLogMessage("render: texture uploaded to the OpenGL ES backend");
}

void renderSetPartTexture(Unsigned32 partIndex, const Unsigned8 *rgbaPixels,
                          Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    MemorySize wantedBytes;
    GLint wrapMode;

    if (partIndex >= RENDER_PART_LIMIT)
    {
        return;
    }
    if (rgbaPixels == NULL_POINTER || widthInPixels == 0U || heightInPixels == 0U)
    {
        if (meshPartTextures[partIndex] != 0U)
        {
            glDeleteTextures(1, &meshPartTextures[partIndex]);
            meshPartTextures[partIndex] = 0U;
        }
        if (meshPartTextureBytes[partIndex] > 0UL)
        {
            graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE,
                                        meshPartTextureBytes[partIndex]);
            meshPartTextureBytes[partIndex] = 0UL;
        }
        return;
    }

    wantedBytes = (MemorySize)widthInPixels * (MemorySize)heightInPixels * 4UL;
    if (graphicsMemoryBudgetRequest(GRAPHICS_MEMORY_CATEGORY_TEXTURE, wantedBytes) == BOOLEAN_FALSE)
    {
        platformLogMessage("render: the graphics ceiling will not hold this part's texture");
        return;
    }

    if (meshPartTextures[partIndex] == 0U)
    {
        glGenTextures(1, &meshPartTextures[partIndex]);
    }
    glBindTexture(GL_TEXTURE_2D, meshPartTextures[partIndex]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)widthInPixels, (GLsizei)heightInPixels, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels);
    wrapMode = (isPowerOfTwo(widthInPixels) && isPowerOfTwo(heightInPixels)) ? GL_REPEAT
                                                                            : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);

    /* Whatever this part held before, given back only once the new one is
       there — a release before the request could let the ceiling admit a
       texture it then cannot hold. */
    if (meshPartTextureBytes[partIndex] > 0UL)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE,
                                    meshPartTextureBytes[partIndex]);
    }
    meshPartTextureBytes[partIndex] = wantedBytes;
}

void renderDrawFrame(Real32 elapsedSeconds)
{
    Real32 colorPulse = 0.65f + (0.35f * mathSine(elapsedSeconds * 1.5f));

    VICTORIA_PROFILE_ZONE_BEGIN("renderDrawFrame");

    glClearColor(0.06f, 0.07f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (meshIndexCount > 0)
    {
        VICTORIA_PROFILE_ZONE_BEGIN("drawMesh");
        drawMesh(elapsedSeconds);
        VICTORIA_PROFILE_ZONE_END();
    }
    else
    {
        glUseProgram(shaderProgram);
        glUniform1f(colorPulseLocation, colorPulse);

        bindTriangleAttributes();
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glDisableVertexAttribArray((GLuint)vertexPositionLocation);
        glDisableVertexAttribArray((GLuint)vertexColorLocation);
    }

    if (overlayWidth > 0U && overlayProgram != 0)
    {
        VICTORIA_PROFILE_ZONE_BEGIN("drawOverlay");
        drawOverlay();
        VICTORIA_PROFILE_ZONE_END();
    }

    VICTORIA_PROFILE_ZONE_END();
}

void renderShutdown(void)
{
    if (vertexBuffer != 0)
    {
        glDeleteBuffers(1, &vertexBuffer);
        vertexBuffer = 0;
    }
    if (vertexBufferIsCharged == BOOLEAN_TRUE)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, sizeof(triangleVertices));
        vertexBufferIsCharged = BOOLEAN_FALSE;
    }
    if (meshVertexBuffer != 0)
    {
        glDeleteBuffers(1, &meshVertexBuffer);
        meshVertexBuffer = 0;
    }
    if (meshIndexBuffer != 0)
    {
        glDeleteBuffers(1, &meshIndexBuffer);
        meshIndexBuffer = 0;
    }
    if (meshChargedBytes > 0UL)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_BUFFER, meshChargedBytes);
        meshChargedBytes = 0UL;
    }
    if (meshProgram != 0)
    {
        glDeleteProgram(meshProgram);
        meshProgram = 0;
    }
    meshIndexCount = 0;
    if (overlayTexture != 0U)
    {
        glDeleteTextures(1, &overlayTexture);
        overlayTexture = 0U;
    }
    if (overlayChargedBytes > 0UL)
    {
        graphicsMemoryBudgetRelease(GRAPHICS_MEMORY_CATEGORY_TEXTURE, overlayChargedBytes);
        overlayChargedBytes = 0UL;
    }
    if (overlayProgram != 0)
    {
        glDeleteProgram(overlayProgram);
        overlayProgram = 0;
    }
    overlayWidth = 0U;
    overlayHeight = 0U;
    if (shaderProgram != 0)
    {
        glDeleteProgram(shaderProgram);
        shaderProgram = 0;
    }
    shaderProgramCount = 0U;
}
