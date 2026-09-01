import boto3
import random
import uuid
from datetime import datetime, timedelta
from decimal import Decimal
import os

# Set endpoint_url to target DynamoDB Local, or leave as None to hit real AWS DynamoDB
ENDPOINT_URL = os.getenv("DYNAMODB_ENDPOINT_URL")  # Example: "http://localhost:8000"
REGION_NAME =  os.getenv("AWS_DEFAULT_REGION")

session = boto3.Session(region_name=REGION_NAME)

dynamo = session.resource(
    "dynamodb",
    region_name=REGION_NAME,
    endpoint_url=ENDPOINT_URL,
)
client = session.client(
    "dynamodb",
    region_name=REGION_NAME,
    endpoint_url=ENDPOINT_URL,
)

print("Creating 'orders' table...")
try:
    table = dynamo.create_table(
        TableName="orders",
        KeySchema=[
            {"AttributeName": "customerId", "KeyType": "HASH"},
            {"AttributeName": "orderId", "KeyType": "RANGE"},
        ],
        AttributeDefinitions=[
            {"AttributeName": "customerId", "AttributeType": "S"},
            {"AttributeName": "orderId", "AttributeType": "S"},
            # GSI key attributes must be declared here too
            {"AttributeName": "status", "AttributeType": "S"},
        ],
        GlobalSecondaryIndexes=[
            {
                "IndexName": "status-index",
                "KeySchema": [
                    {"AttributeName": "status", "KeyType": "HASH"},
                ],
                "Projection": {"ProjectionType": "ALL"},
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

STATUSES = ["PENDING", "SHIPPED", "DELIVERED", "CANCELLED"]
CUSTOMERS = [f"CUST-{i:03d}" for i in range(1, 11)]

print("Seeding 200 rows (heterogeneous schema)...")
with table.batch_writer() as batch:
    for i in range(2000):
        customer_id = random.choice(CUSTOMERS)
        order_id = str(uuid.uuid4())
        status = random.choice(STATUSES)
        amount = Decimal(str(round(random.uniform(10, 500), 2)))
        created_at = (datetime.now() - timedelta(days=random.randint(0, 365))).isoformat()

        item = {
            "customerId": customer_id,
            "orderId": order_id,
            "status": status,
            "amount": amount,
            "createdAt": created_at,
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
                "city": "Berlin",
                "zip": str(random.randint(10000, 99999)),
            }

        batch.put_item(Item=item)

    batch.put_item(
        {
            "customerId": "test_cust_id",
            "orderId": "1234",
            "status": "ACTIVE",
            "amount": Decimal("350.00"),
            "createdAt": "2026-04-01T23:40:53.371863",
            "tags": "extra_tag",
        }
    )

print("rows seeded")

count = table.scan(Select="COUNT")["Count"]
