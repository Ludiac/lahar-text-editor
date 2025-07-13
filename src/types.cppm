module;

#include "primitive_types.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

export module vulkan_app:Types;

import vulkan_hpp;
import std;

/**
 * @brief A UBO to control shader features via boolean-like integer toggles.
 */
export struct ShaderTogglesUBO {
  i32 useNormalMapping{1};
  i32 useOcclusion{1};
  i32 useEmission{1};
  i32 useLights{1};
  i32 useAmbient{1};
};

/**
 * @brief Defines the layout of a single vertex for 3D models.
 */
export struct Vertex {
  glm::vec3 pos;
  glm::vec3 normal;
  glm::vec2 uv;
  glm::vec4 tangent; // w-component stores handedness
};

/**
 * @brief The uniform buffer object for model-view-projection matrices.
 */
export struct UniformBufferObject {
  glm::mat4 model;
  glm::mat4 view;
  glm::mat4 projection;
  glm::mat4 inverseView;
  glm::mat4 normalMatrix;
};

/**
 * @brief Represents a 3D transformation with translation, rotation, and scale.
 */
export struct Transform {
  glm::vec3 translation{0.0};
  glm::vec3 scale{1.0};
  glm::quat rotation{glm::identity<glm::quat>()};
  glm::vec3 rotation_speed_euler_dps{0.0};

  void update(float delta_time) {
    if (glm::length(rotation_speed_euler_dps) > 0.0) {
      glm::vec3 angular_change_rad = glm::radians(rotation_speed_euler_dps) * delta_time;
      auto delta_rotation = glm::quat(angular_change_rad);
      rotation = glm::normalize(delta_rotation * rotation);
    }
  }

  [[nodiscard]] glm::mat4 getMatrix() const {
    glm::mat4 trans_matrix = glm::translate(glm::mat4(1.0), translation);
    glm::mat4 rot_matrix = glm::mat4_cast(rotation);
    glm::mat4 scale_matrix = glm::scale(glm::mat4(1.0), scale);
    return trans_matrix * rot_matrix * scale_matrix;
  }
};

/**
 * @brief Decomposes a 4x4 matrix into its translation, rotation, and scale components.
 */
export [[nodiscard]] Transform decomposeFromMatrix(const glm::mat4 &matrix) {
  Transform t;
  glm::vec3 skew;
  glm::vec4 perspective;
  if (!glm::decompose(matrix, t.scale, t.rotation, t.translation, skew, perspective)) {
    std::println("Warning: Matrix decomposition failed. Returning default transform.");
    return Transform{};
  }
  return t;
}

const int MAX_LIGHTS = 16;

/**
 * @brief Represents a single point light in the scene.
 */
export struct PointLight {
  alignas(16) glm::vec4 position;
  alignas(16) glm::vec4 color; // w is intensity
};

/**
 * @brief The uniform buffer object for scene-wide light data.
 */
export struct SceneLightsUBO {
  PointLight lights[MAX_LIGHTS];
  int lightCount;
};

/**
 * @brief Represents PBR material properties, matching shader layout.
 */
struct alignas(16) Material {
  alignas(16) glm::vec4 baseColorFactor{1.0, 1.0, 1.0, 1.0};
  float metallicFactor{1.0};
  float roughnessFactor{1.0};
  float occlusionStrength{1.0};

private:
  float _pad0;

public:
  alignas(16) glm::vec3 emissiveFactor{0.0, 0.0, 0.0};

private:
  float _pad1;

public:
  float normalScale{1.0};
  float heightScale{0.0};

private:
  float _pad2[2];

public:
  float transmissionFactor{0.0};

private:
  float _pad3[3];

public:
  float clearcoatFactor{0.0};
  float clearcoatRoughness{0.0};

private:
  glm::vec2 _pad4{0.0, 0.0};

public:
  glm::vec3 sheenColorFactor{0.0, 0.0, 0.0};
  float sheenRoughness{0.0};

private:
  float _pad5;
};

/**
 * @brief A simple first-person camera implementation.
 */
export class Camera {
public:
  glm::vec3 Position{-10.0, -10.0, 60.0};
  float Yaw = -75.0;
  float Pitch = 10.0;
  float Roll = 180.0;
  float Zoom = 45.0;
  float Near = 0.1;
  float Far = 10000.0;

  float MovementSpeed = 10.;
  float MouseSensitivity = 0.1;
  bool MouseCaptured = false;

  glm::vec3 Front{0.0, 0.0, -1.0};
  glm::vec3 Right{1.0, 0.0, 0.0};
  glm::vec3 Up{0.0, 1.0, 0.0};
  glm::vec3 WorldUp{0.0, 1.0, 0.0};

  [[nodiscard]] glm::mat4 GetViewMatrix() const {
    return glm::lookAt(Position, Position + Front, Up);
  }

  [[nodiscard]] glm::mat4 GetProjectionMatrix(float aspectRatio) const {
    return glm::perspective(glm::radians(Zoom), aspectRatio, Near, Far);
  }

  void updateVectors() {
    glm::vec3 front;
    front.x = cos(glm::radians(Yaw)) * cos(glm::radians(Pitch));
    front.y = sin(glm::radians(Pitch));
    front.z = sin(glm::radians(Yaw)) * cos(glm::radians(Pitch));
    Front = glm::normalize(front);

    glm::quat rollQuat = glm::angleAxis(glm::radians(Roll), Front);
    glm::vec3 rolledUp = rollQuat * WorldUp;

    Right = glm::normalize(glm::cross(Front, rolledUp));
    Up = glm::normalize(glm::cross(Right, Front));
  }
};
