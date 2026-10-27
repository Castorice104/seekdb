#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
using namespace oceanbase::common;

class AIProviderTest : public ::testing::Test {
protected:
  ObArenaAllocator allocator;
  ObJsonObject *object(const char *text) {
    ObString value(text);
    ObJsonObject *result = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, value, result));
    return result;
  }
};

TEST_F(AIProviderTest, openai_reorders_indices)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator,
      object(R"({"data":[{"index":1,"embedding":[3,4]},{"index":0,"embedding":[1,2]}]})"), result));
  auto *vectors = static_cast<ObJsonArray *>(result);
  auto *first = static_cast<ObJsonArray *>(vectors->get_value(0));
  EXPECT_EQ(1, first->get_value(0)->get_int());
}

TEST_F(AIProviderTest, rejects_malformed_embeddings)
{
  const char *responses[] = {
    R"({"data":{}})", R"({"data":[]})",
    R"({"data":[{"index":0,"embedding":"bad"}]})",
    R"({"data":[{"index":0,"embedding":[true]}]})",
    R"({"data":[{"index":0,"embedding":["1"]}]})",
    R"({"data":[{"index":0,"embedding":[]}]})",
    R"({"data":[{"index":1,"embedding":[1]}]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})"
  };
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (auto response : responses) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, object(response), result)) << response;
    EXPECT_EQ(nullptr, result);
  }
}

TEST_F(AIProviderTest, rejects_empty_or_nonstring_completion)
{
  ObOpenAIUtils::ObOpenAIComplete openai;
  ObOllamaUtils::ObOllamaComplete ollama;
  ObIJsonBase *result = nullptr;
  EXPECT_NE(OB_SUCCESS, openai.parse_output(allocator,
      object(R"({"choices":[{"message":{"content":""}}]})"), result));
  EXPECT_NE(OB_SUCCESS, ollama.parse_output(allocator, object(R"({"response":null})"), result));
  EXPECT_NE(OB_SUCCESS, ollama.parse_output(allocator, object(R"({"response":""})"), result));
}

TEST_F(AIProviderTest, ollama_routing_headers_and_stream)
{
  ObAIFuncIComplete *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator, ObString("oLlAmA"), provider));
  ObString key;
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, provider->get_header(allocator, key, headers));
  EXPECT_GT(headers.count(), 0);
  ObString model("configured-model"), prompt, content("input");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator, model, prompt, content,
      object(R"({"temperature":0.2,"max_tokens":16,"stream":true})"), body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  ASSERT_NE(nullptr, body->get_value("options"));
  auto *options = static_cast<ObJsonObject *>(body->get_value("options"));
  ASSERT_NE(nullptr, options->get_value("num_predict"));
  EXPECT_EQ(16, options->get_value("num_predict")->get_int());
}


TEST_F(AIProviderTest, ollama_native_embed)
{
  ObAIFuncIEmbed *provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator, ObString("OlLaMa"), provider));
  ObString model("configured-model"), key;
  ObArray<ObString> headers, inputs;
  ASSERT_EQ(OB_SUCCESS, provider->get_header(allocator, key, headers));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("one")));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("two")));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator, model, inputs, nullptr, body));
  EXPECT_EQ(2, body->get_value("input")->element_count());
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->parse_output(allocator, object(R"({"embeddings":[[1,2],[3,4]]})"), result));
  EXPECT_EQ(2, result->element_count());
  for (auto text : {R"({"embeddings":[[1],[2,3]]})", R"({"embeddings":[[null]]})", R"({"embeddings":[]})"}) {
    EXPECT_NE(OB_SUCCESS, provider->parse_output(allocator, object(text), result));
    EXPECT_EQ(nullptr, result);
  }
}

TEST_F(AIProviderTest, rejects_bad_indices_dimensions_and_nonfinite)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  const char *texts[] = {
    R"({"data":[{"embedding":[1]}]})",
    R"({"data":[{"index":-1,"embedding":[1]}]})",
    R"({"data":[{"index":0.5,"embedding":[1]}]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":1,"embedding":[2,3]}]})",
    R"({"data":[{"index":0,"embedding":[3.5e38]}]})"
  };
  for (auto text : texts) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, object(text), result)) << text;
    EXPECT_EQ(nullptr, result);
  }
}

TEST_F(AIProviderTest, ollama_endpoint_create_and_update_without_key)
{
  oceanbase::share::ObAiModelEndpointInfo endpoint;
  ASSERT_EQ(OB_SUCCESS, endpoint.parse_from_json_base(allocator, ObString("local"),
      *object(R"({"ai_model_name":"model","url":"http://localhost/api/generate","provider":"oLlAmA"})")));
  EXPECT_TRUE(endpoint.get_encrypted_access_key().empty());
  EXPECT_EQ(OB_SUCCESS, endpoint.merge_delta_endpoint(allocator,
      *object(R"({"url":"http://localhost/api/embed","access_key":"","provider":"OLLAMA"})")));
  EXPECT_NE(OB_SUCCESS, endpoint.merge_delta_endpoint(allocator,
      *object(R"({"provider":"OPENAI","access_key":""})")));
}

TEST_F(AIProviderTest, ollama_merges_native_options_and_sets_system)
{
  ObOllamaUtils::ObOllamaComplete provider;
  ObString model("configured-model"), system("system instructions"), content("input");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, content,
      object(R"({"options":{"top_k":5},"max_completion_tokens":8,"format":"json"})"), body));
  auto *options = static_cast<ObJsonObject *>(body->get_value("options"));
  ASSERT_NE(nullptr, options);
  EXPECT_EQ(5, options->get_value("top_k")->get_int());
  EXPECT_EQ(8, options->get_value("num_predict")->get_int());
  EXPECT_EQ(system, static_cast<ObJsonString *>(body->get_value("system"))->get_str());
  EXPECT_NE(OB_SUCCESS, provider.get_body(allocator, model, system, content,
      object(R"({"options":[]})"), body));
}

TEST_F(AIProviderTest, dashscope_reorders_text_indices)
{
  ObDashscopeUtils::ObDashscopeEmbed provider;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator,
      object(R"({"output":{"embeddings":[{"text_index":1,"embedding":[2]},{"text_index":0,"embedding":[1]}]}})"), result));
  auto *first = static_cast<ObJsonArray *>(static_cast<ObJsonArray *>(result)->get_value(0));
  EXPECT_EQ(1, first->get_value(0)->get_int());
}

TEST_F(AIProviderTest, dashscope_rejects_duplicate_text_indices)
{
  ObDashscopeUtils::ObDashscopeEmbed provider;
  ObIJsonBase *result = nullptr;
  EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator,
      object(R"({"output":{"embeddings":[{"text_index":0,"embedding":[1]},{"text_index":0,"embedding":[2]}]}})"), result));
}
