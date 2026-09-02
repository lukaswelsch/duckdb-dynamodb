# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most its tests in this format as SQL statements, so for the quack extension, this should probably be the goal too.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```

# Running the Tests

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
