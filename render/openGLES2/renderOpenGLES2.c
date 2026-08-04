#include <GLES2/gl2.h>

#include "victoria/freestandingRuntime.h"
#include "victoria/platformInterface.h"
#include "victoria/renderInterface.h"

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

Boolean renderInitialize(MemoryArena *arena, Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    GLuint vertexShader;
    GLuint fragmentShader;
    GLint linkStatus = GL_FALSE;

    (void)arena;

    vertexShader = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    if (vertexShader == 0)
    {
        return BOOLEAN_FALSE;
    }

    fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    if (fragmentShader == 0)
    {
        glDeleteShader(vertexShader);
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
        return BOOLEAN_FALSE;
    }

    vertexPositionLocation = glGetAttribLocation(shaderProgram, "vertexPosition");
    vertexColorLocation = glGetAttribLocation(shaderProgram, "vertexColor");
    colorPulseLocation = glGetUniformLocation(shaderProgram, "colorPulse");

    glGenBuffers(1, &vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(triangleVertices), triangleVertices, GL_STATIC_DRAW);

    renderResize(widthInPixels, heightInPixels);
    platformLogMessage("renderer: OpenGL ES 2.0 backend ready");
    return BOOLEAN_TRUE;
}

void renderResize(Unsigned32 widthInPixels, Unsigned32 heightInPixels)
{
    glViewport(0, 0, (GLsizei)widthInPixels, (GLsizei)heightInPixels);
}

void renderDrawFrame(Real32 elapsedSeconds)
{
    const GLsizei vertexStride = (GLsizei)(5 * sizeof(GLfloat));
    Real32 colorPulse = 0.65f + (0.35f * mathSine(elapsedSeconds * 1.5f));

    glClearColor(0.06f, 0.07f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(shaderProgram);
    glUniform1f(colorPulseLocation, colorPulse);

    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glEnableVertexAttribArray((GLuint)vertexPositionLocation);
    glVertexAttribPointer((GLuint)vertexPositionLocation, 2, GL_FLOAT, GL_FALSE, vertexStride, (const void *)0);
    glEnableVertexAttribArray((GLuint)vertexColorLocation);
    glVertexAttribPointer((GLuint)vertexColorLocation, 3, GL_FLOAT, GL_FALSE, vertexStride,
                          (const void *)(2 * sizeof(GLfloat)));

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glDisableVertexAttribArray((GLuint)vertexPositionLocation);
    glDisableVertexAttribArray((GLuint)vertexColorLocation);
}

void renderShutdown(void)
{
    if (vertexBuffer != 0)
    {
        glDeleteBuffers(1, &vertexBuffer);
        vertexBuffer = 0;
    }
    if (shaderProgram != 0)
    {
        glDeleteProgram(shaderProgram);
        shaderProgram = 0;
    }
}
