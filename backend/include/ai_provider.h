// AI Provider 抽象接口 + 配置结构
// 支持多种 AI 大模型，通过配置文件选择具体实现
#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace eng {

// AI 配置（各 Provider 共用）
struct AiConfig {
  std::string base_url="http://127.0.0.1:5340/v1";
  std::string api_key="local-offline-no-key";
  std::string chat_model="qwen2.5-0.5b";
  std::string provider_type="openai_compatible"; // openai_compatible | agnes
  int request_timeout_sec=60;
  int max_tokens_default=1024;
  double temperature_default=0.7;
  int retry_times=2;
  int retry_interval_sec=2;
};

// AI 请求/响应结构（各 Provider 共用）
struct AiRequest {
  std::vector<std::map<std::string, std::string>> messages; // [{"role":"user","content":"..."}]
  bool wantJson = false;
  int maxTokens = 0;   // 0 = 使用默认值
  double temperature = -1.0; // <0 = 使用默认值
};

struct AiResponse {
  int status = 0;           // HTTP 状态码，0 表示网络错误
  std::string body;         // 原始响应体
  std::string extractedJson; // 从响应中提取的 JSON 字符串
  bool ok = false;          // 是否成功
};

// Provider 类型（需在使用前定义）
enum class AiProviderType {
  OPENAI_COMPATIBLE, // OpenAI 兼容格式（默认）
  AGNES              // Agnes AI（预留，后续扩展）
};

// 抽象 Provider 接口
class IAiProvider {
public:
  virtual ~IAiProvider() = default;
  virtual void setConfig(const AiConfig& c) = 0;
  virtual bool isDegraded() const { return false; }
  // 发送聊天请求，返回响应
  virtual AiResponse postChat(const AiRequest& req) = 0;
  // 健康检查（可选实现，默认返回 true）
  virtual bool ping() { return true; }
  // 返回自身类型（用于 AiClient 检测是否需要重建 Provider）
  virtual AiProviderType getProviderType() const = 0;
};

// 工厂：根据 provider_type 创建对应 Provider 实例
std::unique_ptr<IAiProvider> createAiProvider(AiProviderType type);
AiProviderType parseProviderType(const std::string& name);
std::string providerTypeName(AiProviderType type);

} // namespace eng
