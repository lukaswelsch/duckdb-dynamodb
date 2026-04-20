# DuckDB DynamoDB Extension
This extension adds the possibility to directly query DynamoDB tables from DuckDB. Currently this is limited to read-only. 
If you want to do Analytics on your DynamoDB you need to export the content and perform analytics there. 
With this extension you can directly query your table, do fast GROUP BYs or JOINs with existing DuckDB tables. 
The extension optimizes calls by performing filter pushdowns: 

If a primary key is available this one is used to fetch only the needed records.
If a GSI is available and queried, the GSI is used. 
The results of these queries are then handed back to DuckDB to handle the final filtering. 

If the user does not filter by a GSI or Primary Key, a full table scan is needed. 
Because this is an expensive operation in DynamoDB, the user is warned and it is only executed, if the user passes allow_full_scan=true.

## Authentication

The extension uses DuckDB Secrets to Authenticate against AWS services. 


## Quickstart
```SQL
INSTALL 'duckdb' FROM community;
LOAD 'duckdb';
```

Create a secret:
```SQL
CREATE SECRET dev(
  type dynamodb
)
```

Query the table:
```SQL
SELECT * FROM dynamodb(table='sample_table', secret_name='dev') WHERE pk = 'sample-123'
```



