#pragma once

#include "duckdb.hpp"
#include "dynamodb_extension.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/auth/STSCredentialsProvider.h>
#include <aws/core/auth/SSOCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/auth/LoginCredentialsProvider.h>

namespace duckdb {
static Aws::Auth::AWSCredentials ResolveViaChain(const std::string &chain_spec, const std::string &profile) {
	std::vector<std::string> providers;
	std::stringstream ss(chain_spec.empty() ? "config;sso;sts;env;instance;process" : chain_spec);
	std::string item;
	while (std::getline(ss, item, ';')) providers.push_back(item);

	for (auto &p : providers) {
		std::shared_ptr<Aws::Auth::AWSCredentialsProvider> provider;
		if (p == "env") {
			provider = Aws::MakeShared<Aws::Auth::EnvironmentAWSCredentialsProvider>("dynamodb");
		} else if (p == "config") {
			provider = Aws::MakeShared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(
				"dynamodb", profile.empty() ? "default" : profile.c_str());
		} else if (p == "sso") {
			provider = Aws::MakeShared<Aws::Auth::SSOCredentialsProvider>(
				"dynamodb", profile.empty() ? "default" : profile.c_str());
		} else if (p == "sts") {
			provider = Aws::MakeShared<Aws::Auth::STSAssumeRoleWebIdentityCredentialsProvider>("dynamodb");
		} else if (p == "instance") {
			provider = Aws::MakeShared<Aws::Auth::InstanceProfileCredentialsProvider>("dynamodb");
		} else if (p == "process") {
			provider = Aws::MakeShared<Aws::Auth::ProcessCredentialsProvider>("dynamodb");
		} else {
			continue;
		}
		auto creds = provider->GetAWSCredentials();
		if (!creds.GetAWSAccessKeyId().empty()) return creds;
	}

	throw std::runtime_error("credential_chain: no provider in chain [" + chain_spec + "] resolved credentials");
}
static void ResolveCredentialChain(CreateSecretInput &input, KeyValueSecret &result) {
	if (input.options.find("access_key_id") != input.options.end()) {
		return;
	}

	string profile; auto profile_it = input.options.find("profile");

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
