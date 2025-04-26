#!/usr/bin/env python3
import sqlite3
import subprocess
import time
import argparse
import os
import json

class DBIROptimizer:
    def __init__(self, db_path, dbir_binary="./dbir"):
        self.db_path = os.path.abspath(db_path)
        self.dbir_binary = dbir_binary
        
        # Check if binary exists
        if not os.path.exists(dbir_binary):
            raise FileNotFoundError(f"DBIROptimizer binary not found at {dbir_binary}")
        
        # Check if database exists
        if not os.path.exists(db_path):
            raise FileNotFoundError(f"Database file not found at {db_path}")
    
    def run_query(self, sql, use_optimizer=True):
        """Run a SQL query through DBIROptimizer or SQLite"""
        results = {}
        start_time = time.time()
        
        if use_optimizer:
            # Call the DBIROptimizer binary
            cmd = [self.dbir_binary, self.db_path, sql]
            try:
                process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                stdout, stderr = process.communicate()
                output = stdout.decode('utf-8')
                
                # Parse execution time from output
                for line in output.split('\n'):
                    if "execution time:" in line:
                        parts = line.split()
                        time_idx = parts.index("time:") + 1
                        if time_idx < len(parts):
                            exec_time = float(parts[time_idx])
                            results['execution_time_ms'] = exec_time
                
                if stderr:
                    results['error'] = stderr.decode('utf-8')
                    
                results['output'] = output
                
            except Exception as e:
                results['error'] = str(e)
        else:
            # Use SQLite directly
            try:
                conn = sqlite3.connect(self.db_path)
                cursor = conn.cursor()
                
                rows = cursor.execute(sql).fetchall()
                column_names = [description[0] for description in cursor.description] if cursor.description else []
                
                results['column_names'] = column_names
                results['rows'] = rows
                results['row_count'] = len(rows)
                
                conn.close()
            except Exception as e:
                results['error'] = str(e)
        
        results['total_time_ms'] = (time.time() - start_time) * 1000
        return results
    
    def benchmark(self, sql, runs=3):
        """Benchmark a query with both DBIROptimizer and SQLite"""
        print(f"Benchmarking query: {sql}")
        
        sqlite_times = []
        optimizer_times = []
        
        # Run with SQLite
        print("\nRunning with SQLite...")
        for i in range(runs):
            print(f"  Run {i+1}/{runs}", end="... ")
            results = self.run_query(sql, use_optimizer=False)
            if 'error' in results:
                print(f"ERROR: {results['error']}")
                return
            
            time_ms = results['total_time_ms']
            sqlite_times.append(time_ms)
            print(f"{time_ms:.2f}ms, {results['row_count']} rows")
        
        # Run with DBIROptimizer
        print("\nRunning with DBIROptimizer...")
        for i in range(runs):
            print(f"  Run {i+1}/{runs}", end="... ")
            results = self.run_query(sql, use_optimizer=True)
            if 'error' in results:
                print(f"ERROR: {results['error']}")
                return
            
            if 'execution_time_ms' in results:
                time_ms = results['execution_time_ms']
                optimizer_times.append(time_ms)
                print(f"{time_ms:.2f}ms")
            else:
                print("Could not determine execution time")
        
        # Calculate averages
        sqlite_avg = sum(sqlite_times) / len(sqlite_times) if sqlite_times else 0
        optimizer_avg = sum(optimizer_times) / len(optimizer_times) if optimizer_times else 0
        
        # Print summary
        print("\nBenchmark Summary:")
        print(f"  SQLite average: {sqlite_avg:.2f}ms")
        print(f"  DBIROptimizer average: {optimizer_avg:.2f}ms")
        
        if optimizer_avg > 0 and sqlite_avg > 0:
            speedup = sqlite_avg / optimizer_avg
            print(f"  Speedup: {speedup:.2f}x")

def generate_test_database(db_path):
    """Create a test database with sample tables and data"""
    print(f"Creating test database at {db_path}...")
    
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    
    # Create tables
    cursor.execute('''
    CREATE TABLE customers (
        id INTEGER PRIMARY KEY,
        name TEXT,
        city TEXT,
        state TEXT
    )
    ''')
    
    cursor.execute('''
    CREATE TABLE orders (
        order_id INTEGER PRIMARY KEY,
        customer_id INTEGER,
        order_date TEXT,
        status TEXT,
        FOREIGN KEY (customer_id) REFERENCES customers (id)
    )
    ''')
    
    # Insert sample data
    cities = ['NY', 'LA', 'Chicago', 'Houston', 'Phoenix']
    states = ['New York', 'California', 'Illinois', 'Texas', 'Arizona']
    
    # Insert customers
    for i in range(1, 101):
        city_idx = i % len(cities)
        cursor.execute(
            "INSERT INTO customers VALUES (?, ?, ?, ?)",
            (i, f"Customer {i}", cities[city_idx], states[city_idx])
        )
    
    # Insert orders
    statuses = ['Pending', 'Shipped', 'Delivered', 'Cancelled']
    for i in range(1, 1001):
        customer_id = (i % 100) + 1
        status_idx = i % len(statuses)
        cursor.execute(
            "INSERT INTO orders VALUES (?, ?, date('now', '-' || ? || ' days'), ?)",
            (i, customer_id, i % 60, statuses[status_idx])
        )
    
    conn.commit()
    conn.close()
    print("Database created with sample data.")

def load_tpch_queries(queries_file):
    """Load TPC-H queries from a JSON file"""
    with open(queries_file, 'r') as f:
        return json.load(f)

def main():
    parser = argparse.ArgumentParser(description="DBIROptimizer Python Interface")
    parser.add_argument("--db", required=True, help="Path to SQLite database file")
    parser.add_argument("--binary", default="./dbir", help="Path to DBIROptimizer binary")
    parser.add_argument("--create-test-db", action="store_true", help="Create a test database")
    parser.add_argument("--query", help="SQL query to execute")
    parser.add_argument("--benchmark", action="store_true", help="Run benchmarks")
    parser.add_argument("--runs", type=int, default=3, help="Number of benchmark runs")
    parser.add_argument("--tpch-queries", help="Path to TPC-H queries JSON file")
    
    args = parser.parse_args()
    
    if args.create_test_db:
        generate_test_database(args.db)
    
    optimizer = DBIROptimizer(args.db, args.binary)
    
    if args.query:
        if args.benchmark:
            optimizer.benchmark(args.query, args.runs)
        else:
            print("Running query with DBIROptimizer...")
            results = optimizer.run_query(args.query)
            print(results['output'] if 'output' in results else results)
    
    if args.tpch_queries:
        queries = load_tpch_queries(args.tpch_queries)
        for name, sql in queries.items():
            print(f"\n{'='*50}")
            print(f"Running TPC-H query: {name}")
            optimizer.benchmark(sql, args.runs)

if __name__ == "__main__":
    main()