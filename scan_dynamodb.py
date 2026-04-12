import boto3
from decimal import Decimal


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

total_amount = 0.0
total_count = 0
response = table.scan()

while True:
    for item in response.get("Items", []):
        # Convert amount to float and add to total
        total_count += 1
        if "amount" in item:
            amount = item["amount"]

            # Handle DynamoDB Decimal type
            if isinstance(amount, Decimal):
                amount = float(amount)
            else:
                amount = float(amount)

            total_amount += amount

    # Handle pagination (scan can return partial results)
    if "LastEvaluatedKey" in response:
        response = table.scan(ExclusiveStartKey=response["LastEvaluatedKey"])
    else:
        break

print(f"Total amount: {total_amount}")
print(f"Total count: {total_count}")

