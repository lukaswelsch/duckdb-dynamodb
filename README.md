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

Only predicates on indexed attributes reduce RCU consumption. Predicates on non-indexed attributes do not affect what DynamoDB returns — DuckDB filters those locally.

## Full Scans

By default, queries that cannot push down any predicate are rejected to prevent unintended full table scans and unexpected RCU charges. To permit a full scan, set `allow_full_scan = true`:

```sql
SELECT status, COUNT(*) AS n
FROM dynamodb_scan('orders', allow_full_scan = true)
GROUP BY status;
```

> **Warning:** Full scans consume RCUs proportional to the total table size. Use with caution on large tables.

## Schema inference
This extensions scans a sample of DynamoDB items, counts how frequently each attribute appears, and infers a DuckDB type from its DynamoDB AttributeValue
Conflicting types are widened to VARCHAR. 

On default the extension uses the hybrid mode. 
In hybrid mode, attributes appearing in at least hybrid_threshold (default 80%) become regular columns.
Rarer attributes are collected into an _extra JSON column.

## Configuration and Authentication

Authentication uses [DuckDB Secrets Manager](https://duckdb.org/docs/current/configuration/secrets_manager.html). The extension supports two providers.

### `config` Provider

Provide credentials explicitly:

```sql
CREATE OR REPLACE SECRET dynamo_secret (
    TYPE dynamodb,
    PROVIDER config,
    KEY_ID 'EXAMPLE',
    SECRET 'EXAMPLESECRET',
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


## Function Reference

This extension provides one function that scans a DynamoDB table and pushes PK/SK/GSI predicates to DynamoDB.    

- **table_name ('orders')**: Target DynamoDB table to scan.
- **endpoint**: Custom connection URL (useful for local development).
- **allow_full_scan (false / true)**: Safety toggle to prevent accidental unindexed full-table reads.
- **parallel_segments**: Number of threads to divide and speed up the scan.
- **schema_mode ('hybrid'|'infer')**: Controls how DynamoDB attributes are mapped to data types.
    - hybrid (default): activates the hybrid mode, in which rare attributes are stored in an _extra JSON column.
    - infer: same as hybrid_threshold=0; all attributes are converted to columns
- **hybrid_threshold (0.8)**: Attributes must appear in this percent of samples to be a column
- **sample_size (200)**: How many rows should be checked to infer the schema from.

dynamodb_scan('orders',
       endpoint='http://localhost:8000',
       allow_full_scan=false,
       schema_mode='hybrid',
       parallel_segments=8)




## Limitations

- Write operations (`INSERT`, `UPDATE`, `DELETE`) are not supported.
- Currently range pushdowns on secondary key is not optimized.

This SQL is not optimized:
```sql
-- SK range pushed to DynamoDB; aggregation done by DuckDB
SELECT DATE_TRUNC('day', created_at) AS day, SUM(amount) AS revenue
FROM dynamodb_scan('orders')
WHERE pk = 'tenant'
  AND sk BETWEEN '2024-01-01' AND '2024-01-31'
GROUP BY day
ORDER BY day;
```


- IN Queries are also not optimized.
```sql
-- IN on PK pushed down via BatchGetItem
SELECT *
FROM dynamodb_scan('users')
WHERE pk IN ('user#1', 'user#2', 'user#3');
```

# Testing

The extension requires a DynamoDB instance for testing. 
Set the env var DYNAMODB_TEST_ENDPOINT to 1 to run the tests.

```bash
pip install boto3
docker run -d -p 8000:8000 amazon/dynamodb-local  
export AWS_ACCESS_KEY_ID='DUMMY'
export AWS_SECRET_ACCESS_KEY='DUMMY'
export AWS_DEFAULT_REGION='us-east-1'
export DYNAMODB_ENDPOINT_URL='http://localhost:8000'
python test/seed_local_dynamodb.py
```

Then you can run the tests with:
```bash
DYNAMODB_TEST_ENDPOINT=1 ./build/release/test/unittest
```

The CI has an additional step where it starts a dynamodb container and runs the seed script.


## Important Notes on Using DynamoDB
 
> ⚠️ Disclaimer: This community-driven open-source project is independently maintained and is not affiliated with, sponsored by, or endorsed by Amazon Web Services (AWS) or any related entities.
> The extension is distributed "as is," without warranties of any kind.
> All trademarks—including "DuckDB" and "DynamoDB"—belong to their respective owners.
> You assume full responsibility for complying with relevant service terms and for any charges generated by using this tool.

### Billing and Costs

Connecting to DynamoDB via this extension directly interacts with your AWS account and may generate service charges.
AWS bills DynamoDB usage based on read/write capacity, data storage, and network transfer rates.
You are solely responsible for tracking and paying any incurred expenses.
To prevent surprise charges, we strongly advise configuring budget tracking and threshold alerts inside your AWS Billing & Cost Management Dashboard.
