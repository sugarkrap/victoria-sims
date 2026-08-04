#include <GLES2/gl2.h>

#include "utils/strings.h"
#include "victoria/freestandingRuntime.h"
#include "victoria/graphicsMemoryBudget.h"
#include "victoria/platformInterface.h"
#include "victoria/profiler.h"
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
}

void renderDrawFrame(Real32 elapsedSeconds)
{
    Real32 colorPulse = 0.65f + (0.35f * mathSine(elapsedSeconds * 1.5f));

    VICTORIA_PROFILE_ZONE_BEGIN("renderDrawFrame");

    glClearColor(0.06f, 0.07f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(shaderProgram);
    glUniform1f(colorPulseLocation, colorPulse);

    bindTriangleAttributes();
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glDisableVertexAttribArray((GLuint)vertexPositionLocation);
    glDisableVertexAttribArray((GLuint)vertexColorLocation);

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
    if (shaderProgram != 0)
    {
        glDeleteProgram(shaderProgram);
        shaderProgram = 0;
    }
    shaderProgramCount = 0U;
}
