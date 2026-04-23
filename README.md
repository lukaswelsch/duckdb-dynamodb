# DuckDB DynamoDB Extension

The `dynamodb` extension adds support for querying [Amazon DynamoDB](https://aws.amazon.com/dynamodb/) tables directly from DuckDB. 
It implements filter pushdown on partition keys, sort keys, and GSIs to minimize read capacity consumption, with remaining filtering and all aggregation handled by DuckDB's execution engine.

## Installing and Loading

```sql
INSTALL dynamodb FROM community;
LOAD dynamodb;
```

## Usage

The extension exposes a `dynamodb_scan` table function:

```sql
SELECT * FROM dynamodb_scan('my-table');
```

> **Note:** Without a pushdown-eligible `WHERE` clause, this performs a full DynamoDB table scan. Full scans must be explicitly permitted — see [Full Scans](#full-scans).

## Filter Pushdown

The extension pushes down `WHERE` clause predicates on the **partition key (PK)**, **sort key (SK)**, and any **Global Secondary Indexes (GSIs)** to DynamoDB at query time.
If multiple options are available, the PK takes precedence over GSIs.
Predicates on all other attributes are evaluated locally by DuckDB after the DynamoDB response is received.

```sql
-- Pushed to DynamoDB via GSI on `user_id`
-- `status` filter applied locally by DuckDB
SELECT user_id, COUNT(*) AS event_count
FROM dynamodb_scan('events')
WHERE user_id = 'u_42'
  AND status = 'active'
GROUP BY user_id;
```

@todo implement this
```sql
-- SK range pushed to DynamoDB; aggregation done by DuckDB
SELECT DATE_TRUNC('day', created_at) AS day, SUM(amount) AS revenue
FROM dynamodb_scan('orders')
WHERE pk = 'tenant'
  AND sk BETWEEN '2024-01-01' AND '2024-01-31'
GROUP BY day
ORDER BY day;
```

```sql
-- IN on PK pushed down via BatchGetItem
SELECT *
FROM dynamodb_scan('users')
WHERE pk IN ('user#1', 'user#2', 'user#3');
```

Only predicates on indexed attributes reduce RCU consumption. Predicates on non-indexed attributes do not affect what DynamoDB returns — DuckDB filters those locally.

## Full Scans

By default, queries that cannot push down any predicate are rejected to prevent unintended full table scans and unexpected RCU charges. To permit a full scan, set `allow_full_scan = true`:

```sql
SELECT status, COUNT(*) AS n
FROM dynamodb_scan('orders', allow_full_scan = true)
GROUP BY status;
```

> **Warning:** Full scans consume RCUs proportional to the total table size. Use with caution on large tables.

## Configuration and Authentication

Authentication uses [DuckDB Secrets Manager](https://duckdb.org/docs/current/configuration/secrets_manager.html). The extension supports two providers.

### `config` Provider

Provide credentials explicitly:

```sql
CREATE OR REPLACE SECRET dynamo_secret (
    TYPE dynamodb,
    PROVIDER config,
    KEY_ID 'AKIAIOSFODNN7EXAMPLE',
    SECRET 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY',
    REGION 'eu-west-1'
);
```

### `credential_chain` Provider

Resolve credentials automatically via the AWS SDK default provider chain (environment variables, `~/.aws/credentials`, IAM instance profiles, etc.):

```sql
CREATE OR REPLACE SECRET dynamo_secret (
    TYPE dynamodb,
    PROVIDER credential_chain
);
```

A specific chain order can be specified with the `CHAIN` keyword:

```sql
CREATE OR REPLACE SECRET dynamo_secret (
    TYPE dynamodb,
    PROVIDER credential_chain,
    REGION 'eu-west-1'
);
```

## Function Reference

There are 2 functions available both scan a DynamoDB table and push PK/SK/GSI predicates to DynamoDB.             

| Function                                              | Description                                                     |
|-------------------------------------------------------|-----------------------------------------------------------------|
| `dynamodb_scan(table, [allow_full_scan], [endpoint])`           | Infers schema to create a table.                                |
| `dynamodb_json(table, [allow_full_scan], [endpoint])` | Does not infer the schema. All data is returned in JSON format. |


## Limitations

- DynamoDB does not support aggregation natively. All `GROUP BY`, `COUNT`, `SUM`, etc. are executed by DuckDB after data is fetched.
- `IN` on a sort key is not natively supported by DynamoDB's `KeyConditionExpression` and is evaluated locally by DuckDB.
- `IN` on non-indexed attributes is evaluated locally by DuckDB.
- Write operations (`INSERT`, `UPDATE`, `DELETE`) are not supported.
 