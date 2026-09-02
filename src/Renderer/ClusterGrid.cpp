#include "ClusterGrid.h"
#include "Shader.h"
#include "gl.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

ClusterGrid::~ClusterGrid() {
    if (m_AABBs) glDeleteBuffers(1, &m_AABBs);
    if (m_Counts) glDeleteBuffers(1, &m_Counts);
    if (m_Indices) glDeleteBuffers(1, &m_Indices);
}

void ClusterGrid::EnsureCreated() {
    if (m_AABBs) return;
    glCreateBuffers(1, &m_AABBs);
    glNamedBufferStorage(m_AABBs, (GLsizeiptr)CLUSTERS * 2 * (GLsizeiptr)sizeof(glm::vec4), nullptr, 0);
    glCreateBuffers(1, &m_Counts);
    glNamedBufferStorage(m_Counts, (GLsizeiptr)CLUSTERS * (GLsizeiptr)sizeof(unsigned int), nullptr, 0);
    glCreateBuffers(1, &m_Indices);
    glNamedBufferStorage(m_Indices,
        (GLsizeiptr)CLUSTERS * MAX_LIGHTS_PER_CLUSTER * (GLsizeiptr)sizeof(unsigned int), nullptr, 0);
}

void ClusterGrid::Cull(Shader& buildShader, Shader& cullShader, const glm::mat4& view, const glm::mat4& proj,
                       float nearZ, float farZ, int screenW, int screenH) {
    EnsureCreated();
    const unsigned int groups = (CLUSTERS + 63) / 64;

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_AABBs);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_Counts);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_Indices);

    buildShader.Bind();
    buildShader.SetMat4("uInvProj", glm::inverse(proj));
    buildShader.SetVec2("uScreenSize", glm::vec2((float)screenW, (float)screenH));
    buildShader.SetFloat("uZNear", nearZ);
    buildShader.SetFloat("uZFar", farZ);
    buildShader.DispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    cullShader.Bind();
    cullShader.SetMat4("uView", view);
    cullShader.DispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void ClusterGrid::BindForShading() const {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_Counts);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_Indices);
}

glm::vec4 ClusterGrid::ZParams(float nearZ, float farZ) {
    float lg = std::log(farZ / nearZ);
    return glm::vec4(nearZ, farZ,
                     (float)GRID_Z / lg,
                     -(float)GRID_Z * std::log(nearZ) / lg);
}
