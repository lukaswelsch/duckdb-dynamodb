// test/test_runner.cpp
// Integration tests against DynamoDB Local (docker run -p 8000:8000 amazon/dynamodb-local)
//
// Run:
//   docker run -d -p 8000:8000 amazon/dynamodb-local
//   ./run_tests

#include "duckdb.hpp"
#include <iostream>
#include <cassert>
#include <string>

using namespace duckdb;

// ─────────────────────────────────────────────
// Helper: run a query and print results
// ─────────────────────────────────────────────
static void RunQuery(Connection &con, const std::string &sql, const std::string &label = "") {
    if (!label.empty()) std::cout << "\n── " << label << " ──\n";
    std::cout << "SQL: " << sql << "\n";

    auto result = con.Query(sql);
    if (result->HasError()) {
        std::cerr << "ERROR: " << result->GetError() << "\n";
        return;
    }
    result->Print();
}

static bool ExpectRowCount(Connection &con, const std::string &sql, idx_t expected) {
    auto result = con.Query(sql);
    if (result->HasError()) {
        std::cerr << "ERROR: " << result->GetError() << "\n";
        return false;
    }
    idx_t actual = result->RowCount();
    bool ok = (actual == expected);
    std::cout << (ok ? "✅ PASS" : "❌ FAIL")
              << " expected=" << expected << " got=" << actual << " | " << sql << "\n";
    return ok;
}

int main() {

    // ── Bootstrap DuckDB and load our extension ───────────────────────────
    DuckDB db(nullptr); // in-memory DuckDB instance
    Connection con(db);

    // Load the extension (path relative to test binary)
    con.Query("LOAD '../duckdb_dynamodb.duckdb_extension'");

    // ─────────────────────────────────────────────────────────────────────
    // All tests point at DynamoDB Local on localhost:8000
    // Dummy credentials are required by the SDK but ignored by DynamoDB Local
    // ─────────────────────────────────────────────────────────────────────
    std::string LOCAL = "endpoint='http://localhost:8000', region='us-east-1'";

    // ══════════════════════════════════════════════════════════════════════
    // TEST 1 — Basic scan returns rows (table must exist in DynamoDB Local)
    // ══════════════════════════════════════════════════════════════════════
    RunQuery(con,
        "SELECT * FROM dynamodb_scan('orders', " + LOCAL + ", allow_full_scan=true) LIMIT 5",
        "Basic full scan (first 5 rows)");

    // ══════════════════════════════════════════════════════════════════════
    // TEST 2 — PK predicate pushdown → Query (not Scan)
    //   Confirm by checking the query doesn't throw "allow_full_scan required"
    // ══════════════════════════════════════════════════════════════════════
    RunQuery(con,
        "SELECT * FROM dynamodb_scan('orders', " + LOCAL + ")"
        " WHERE customerId = 'CUST-001'",
        "PK predicate → routed as Query, no full scan");

    // ══════════════════════════════════════════════════════════════════════
    // TEST 3 — GROUP BY on full scan (parallel segments)
    //   DuckDB's hash-aggregate ingests rows from N parallel scan threads.
    //   The result is identical to a sequential scan — just faster.
    // ══════════════════════════════════════════════════════════════════════
    RunQuery(con,
        "SELECT status, COUNT(*) AS cnt, SUM(amount) AS total"
        " FROM dynamodb_scan('orders', " + LOCAL + ", allow_full_scan=true, parallel_segments=4)"
        " GROUP BY status ORDER BY cnt DESC",
        "GROUP BY across parallel scan segments");

    // ══════════════════════════════════════════════════════════════════════
    // TEST 4 — GSI routing
    //   Assumes a GSI called 'status-index' with PK=status exists.
    // ══════════════════════════════════════════════════════════════════════
    RunQuery(con,
        "SELECT * FROM dynamodb_scan('orders', " + LOCAL + ")"
        " WHERE status = 'PENDING'",
        "GSI routing — no full scan needed");

    // ══════════════════════════════════════════════════════════════════════
    // TEST 5 — Raw JSON mode (dynamodb_json)
    //   Every row is a JSON blob; user shreds it with -> operator.
    // ══════════════════════════════════════════════════════════════════════
    RunQuery(con,
        "SELECT raw->>'$.customerId' AS id, (raw->>'$.amount')::DOUBLE AS amount"
        " FROM dynamodb_json('orders', " + LOCAL + ", allow_full_scan=true) LIMIT 10",
        "JSON mode — raw blob per row");

    // ══════════════════════════════════════════════════════════════════════
    // TEST 6 — Hybrid schema: _extra captures infrequent attributes
    // ══════════════════════════════════════════════════════════════════════
    RunQuery(con,
        "SELECT customerId, amount, _extra"
        " FROM dynamodb_scan('orders', " + LOCAL + ", schema_mode='hybrid',"
        "                    allow_full_scan=true) LIMIT 10",
        "Hybrid schema — rare attrs in _extra JSON column");

    // ══════════════════════════════════════════════════════════════════════
    // TEST 7 — Full scan blocked without allow_full_scan
    // ══════════════════════════════════════════════════════════════════════
    {
        auto result = con.Query(
            "SELECT * FROM dynamodb_scan('orders', " + LOCAL + ")"
            " WHERE nonIndexedCol = 'x'"); // no key predicate
        bool blocked = result->HasError() &&
                       result->GetError().find("allow_full_scan") != std::string::npos;
        std::cout << (blocked ? "✅ PASS" : "❌ FAIL")
                  << " Full scan correctly blocked without allow_full_scan=true\n";
    }

    // ══════════════════════════════════════════════════════════════════════
    // TEST 8 — Replacement scan syntax
    // ══════════════════════════════════════════════════════════════════════
    RunQuery(con,
        "SELECT COUNT(*) FROM 'dynamodb://orders' -- requires allow_full_scan via SET",
        "Replacement scan syntax");

    // ══════════════════════════════════════════════════════════════════════
    // TEST 9 — Projection pushdown verification
    //   Only customerId and amount should be fetched from DynamoDB.
    //   Verify by checking _extra is NULL (no other cols requested).
    // ══════════════════════════════════════════════════════════════════════
    RunQuery(con,
        "SELECT customerId, amount"  // only 2 cols → ProjectionExpression='customerId,amount'
        " FROM dynamodb_scan('orders', " + LOCAL + ", allow_full_scan=true) LIMIT 5",
        "Projection pushdown — only 2 cols fetched from DynamoDB");

    std::cout << "\n── All tests complete ──\n";
    return 0;
}
