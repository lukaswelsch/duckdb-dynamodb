import boto3
import random
import uuid
from datetime import datetime, timedelta

dynamo = boto3.resource(
    "dynamodb",
    endpoint_url="http://localhost:8000",
    region_name="us-east-1",
    aws_access_key_id="dummy",       # DynamoDB Local ignores credentials
    aws_secret_access_key="dummy",
)
client = boto3.client(
    "dynamodb",
    endpoint_url="http://localhost:8000",
    region_name="us-east-1",
    aws_access_key_id="dummy",
    aws_secret_access_key="dummy",
)

print("Creating 'orders' table...")
try:
    table = dynamo.create_table(
        TableName="orders",
        KeySchema=[
            {"AttributeName": "customerId", "KeyType": "HASH"},
            {"AttributeName": "orderId",    "KeyType": "RANGE"},
        ],
        AttributeDefinitions=[
            {"AttributeName": "customerId", "AttributeType": "S"},
            {"AttributeName": "orderId",    "AttributeType": "S"},
            # GSI key attributes must be declared here too
            {"AttributeName": "status",     "AttributeType": "S"},
        ],
        GlobalSecondaryIndexes=[
            {
                "IndexName": "status-index",
                "KeySchema": [
                    {"AttributeName": "status", "KeyType": "HASH"},
                ],
                "Projection": {"ProjectionType": "ALL"},
                "ProvisionedThroughput": {"ReadCapacityUnits": 5, "WriteCapacityUnits": 5},
            }
        ],
        BillingMode="PAY_PER_REQUEST",
    )
    table.wait_until_exists()
except client.exceptions.ResourceInUseException:
    print("Table already exists, skipping creation")
    table = dynamo.Table("orders")

# Intentionally heterogeneous: some rows have extra rare attributes
# to exercise the hybrid schema / _extra column logic.

STATUSES    = ["PENDING", "SHIPPED", "DELIVERED", "CANCELLED"]
CUSTOMERS   = [f"CUST-{i:03d}" for i in range(1, 11)]

print("Seeding 200 rows (heterogeneous schema)...")
with table.batch_writer() as batch:
    for i in range(2000):
        customer_id = random.choice(CUSTOMERS)
        order_id    = str(uuid.uuid4())
        status      = random.choice(STATUSES)
        amount      = round(random.uniform(10, 500), 2)
        created_at  = (datetime.now() - timedelta(days=random.randint(0, 365))).isoformat()

        item = {
            "customerId": customer_id,
            "orderId":    order_id,
            "status":     status,
            "amount":     str(amount),   # DynamoDB stores numbers as strings in SDK
            "createdAt":  created_at,
        }

        # ~20% of rows have a 'tags' attribute (rare → should land in _extra)
        if random.random() < 0.2:
            item["tags"] = ["promo", "vip"][random.randint(0, 1)]

        # ~10% have a 'notes' attribute (rarer still)
        if random.random() < 0.1:
            item["notes"] = f"Note for order {i}"

        # ~5% have a nested 'address' map (exercises JSON serialisation in _extra)
        if random.random() < 0.05:
            item["shippingAddress"] = {
                "street": f"{random.randint(1,999)} Main St",
                "city":   "Berlin",
                "zip":    str(random.randint(10000, 99999)),
            }

        batch.put_item(Item=item)

print("rows seeded")

count = table.scan(Select="COUNT")["Count"]
