#pragma once

#include "duckdb.hpp"
#include "dynamodb_extension.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
static unique_ptr<BaseSecret> CreateDynamoSecret(ClientContext &ctx,
												  CreateSecretInput &input) {
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);

	for (const auto &[key, value] : input.options) {
		result->secret_map[StringUtil::Lower(key)] = value.ToString();
	}

	result->redact_keys = {"secret_access_key", "session_token"};
	return std::move(result);
}

static DynamoDBSecretConfig LoadDynamoSecret(ClientContext &ctx, const std::string &secret_name) {
	auto &manager = SecretManager::Get(ctx);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(ctx);

	auto entry = manager.GetSecretByName(transaction, secret_name, "");
	auto &secret = entry->secret;

	if (!secret) {
		throw std::runtime_error("DynamoDB secret not found: " + secret_name);
	}

	auto kv = dynamic_cast<const KeyValueSecret *>(secret.get());
	if (!kv) {
		throw std::runtime_error("Invalid DynamoDB secret format");
	}

	DynamoDBSecretConfig cfg;

	auto get = [&](const std::string &key) -> std::string {
		auto val = kv->TryGetValue(key);
		if (val.IsNull()) return "";
		return val.ToString();
	};

	cfg.access_key_id = get("ACCESS_KEY_ID");
	cfg.secret_access_key = get("SECRET_ACCESS_KEY");
	cfg.region = get("REGION");
	cfg.endpoint = get("ENDPOINT_URL");
	cfg.provider = get(" PROVIDER");

	return cfg;
}
}