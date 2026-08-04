#include <GLES2/gl2.h>

#include "utils/strings.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
#include "render/meshCamera.h"
#include "victoria/renderInterface.h"

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
    "uniform mat4 modelViewProjection;\n"
    "varying vec3 interpolatedNormal;\n"
    "void main()\n"
    "{\n"
    "    interpolatedNormal = vertexNormal;\n"
    "    gl_Position = modelViewProjection * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *meshFragmentShaderSource =
    "precision mediump float;\n"
    "varying vec3 interpolatedNormal;\n"
    "uniform vec3 lightDirection;\n"
    "void main()\n"
    "{\n"
    "    vec3 normal = normalize(interpolatedNormal);\n"
    /* Absolute rather than clamped: nothing here has a back face material, and
       an unlit interior reads as a hole in the model rather than as shadow. */
    "    float lambert = abs(dot(normal, lightDirection));\n"
    "    float shade = 0.28 + (0.72 * lambert);\n"
    "    gl_FragColor = vec4(shade, shade * 0.93, shade * 0.84, 1.0);\n"
    "}\n";

static GLuint meshProgram = 0;
static GLuint meshVertexBuffer = 0;
static GLuint meshIndexBuffer = 0;
static GLint meshPositionLocation = -1;
static GLint meshNormalLocation = -1;
static GLint meshMatrixLocation = -1;
static GLint meshLightLocation = -1;
static GLsizei meshIndexCount = 0;
static MemorySize meshChargedBytes = 0UL;
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
    Unsigned32 index;

    if (mesh == NULL_POINTER || mesh->vertexCount == 0U || mesh->indexCount < 3U ||
        mesh->normals == NULL_POINTER)
    {
        meshIndexCount = 0;
        return;
    }

    vertexBytes = (MemorySize)mesh->vertexCount * 6UL * sizeof(GLfloat);
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

    meshPositionLocation = glGetAttribLocation(meshProgram, "vertexPosition");
    meshNormalLocation = glGetAttribLocation(meshProgram, "vertexNormal");
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
    for (index = 0U; index < mesh->vertexCount; index++)
    {
        const Real32 *position = &mesh->positions[(MemorySize)index * 3UL];
        const Real32 *normal = &mesh->normals[(MemorySize)index * 3UL];
        GLfloat *target = &interleaved[(MemorySize)index * 6UL];

        target[0] = (GLfloat)position[0];
        target[1] = (GLfloat)position[1];
        target[2] = (GLfloat)position[2];
        target[3] = (GLfloat)normal[0];
        target[4] = (GLfloat)normal[1];
        target[5] = (GLfloat)normal[2];
    }

    glGenBuffers(1, &meshVertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, meshVertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertexBytes, interleaved, GL_STATIC_DRAW);

    glGenBuffers(1, &meshIndexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIndexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)indexBytes, mesh->indices, GL_STATIC_DRAW);

    memoryArenaRewindToMarker(arena, marker);

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
    Real32 angle = elapsedSeconds * 0.6f;
    const GLsizei vertexStride = (GLsizei)(6 * sizeof(GLfloat));

    meshCameraBuildMatrix(&meshCamera, angle, viewportAspect, matrix);
    meshCameraGetLightDirection(angle, light);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    glUseProgram(meshProgram);
    glUniformMatrix4fv(meshMatrixLocation, 1, GL_FALSE, matrix);
    glUniform3f(meshLightLocation, light[0], light[1], light[2]);

    glBindBuffer(GL_ARRAY_BUFFER, meshVertexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIndexBuffer);
    glEnableVertexAttribArray((GLuint)meshPositionLocation);
    glVertexAttribPointer((GLuint)meshPositionLocation, 3, GL_FLOAT, GL_FALSE, vertexStride,
                          (const void *)0);
    glEnableVertexAttribArray((GLuint)meshNormalLocation);
    glVertexAttribPointer((GLuint)meshNormalLocation, 3, GL_FLOAT, GL_FALSE, vertexStride,
                          (const void *)(3 * sizeof(GLfloat)));

    glDrawElements(GL_TRIANGLES, meshIndexCount, GL_UNSIGNED_SHORT, (const void *)0);

    glDisableVertexAttribArray((GLuint)meshPositionLocation);
    glDisableVertexAttribArray((GLuint)meshNormalLocation);
    glDisable(GL_DEPTH_TEST);
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
    if (shaderProgram != 0)
    {
        glDeleteProgram(shaderProgram);
        shaderProgram = 0;
    }
    shaderProgramCount = 0U;
}
