#pragma once

#include "duckdb.hpp"
#include "dynamodb_extension.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/auth/SSOCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>

namespace duckdb {
static void ResolveCredentialChain(CreateSecretInput &input, KeyValueSecret &result) {
	if (input.options.find("access_key_id") != input.options.end()) {
		return;
	}

	string profile;
	auto profile_it = input.options.find("profile");

	if (profile_it != input.options.end()) {
		profile = profile_it->second.ToString();
	}

	setenv("AWS_SDK_LOAD_CONFIG", "1", 1);

	Aws::Client::ClientConfiguration client_config;
	client_config.profileName = profile;

	auto &cred_config = client_config.credentialProviderConfig;

	Aws::Auth::DefaultAWSCredentialsProviderChain chain(cred_config);

	auto creds = chain.GetAWSCredentials();

	if (creds.GetAWSAccessKeyId().empty()) {
		throw std::runtime_error("credential_chain: could not resolve AWS credentials");
	}

	result.secret_map["access_key_id"] = creds.GetAWSAccessKeyId();
	result.secret_map["secret_access_key"] = creds.GetAWSSecretKey();

	if (!creds.GetSessionToken().empty()) {
		result.secret_map["session_token"] = creds.GetSessionToken();
	}
}

static unique_ptr<BaseSecret> CreateDynamoSecret(ClientContext &ctx, CreateSecretInput &input) {
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);

	for (const auto &[key, value] : input.options) {
		result->secret_map[StringUtil::Lower(key)] = value.ToString();
	}

	if (input.provider == "credential_chain") {
		ResolveCredentialChain(input, *result);
	}

	result->redact_keys = {"secret_access_key", "session_token"};
	return std::move(result);
}

static DynamoDBSecretConfig LoadDynamoSecret(ClientContext &ctx, const std::string &secret_name) {
	auto &manager = SecretManager::Get(ctx);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(ctx);
	unique_ptr<const BaseSecret> secret;

	if (!secret_name.empty()) {
		auto entry = manager.GetSecretByName(transaction, secret_name, "");
		secret = std::move(entry->secret);
	} else {
		auto match = manager.LookupSecret(transaction, "", "dynamodb");
		if (!match.HasMatch()) {
			throw std::runtime_error("No DynamoDB secret found and no default available");
		}
		secret = match.GetSecret().Clone();
	}

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
		if (val.IsNull())
			return "";
		return val.ToString();
	};

	cfg.access_key_id = get("access_key_id");
	cfg.secret_access_key = get("secret_access_key");
	cfg.session_token = get("session_token");
	cfg.region = get("region");
	cfg.endpoint = get("endpoint_url");
	cfg.provider = get("provider");

	return cfg;
}
} // namespace duckdb
