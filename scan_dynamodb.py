import boto3

dynamodb = boto3.resource(
    "dynamodb",
    region_name="us-east-1",
    aws_access_key_id="dummy",
    aws_secret_access_key="dummy",
    endpoint_url="http://localhost:8000"
)
for table in dynamodb.tables.all():
    print(table.name)

table = dynamodb.Table("orders")

response = table.scan()
for item in response["Items"]:
    print(item)

response = dynamodb.describe_table(TableName="orders")

table = response["Table"]

key_schema = table["KeySchema"]
print("PRIMARY KEY:")
for key in key_schema:
    print(f"{key['KeyType']}: {key['AttributeName']}")