#!/usr/bin/env python3
import requests
import base64
import json
import csv
import subprocess
import os
import argparse

# Parse command-line arguments
parser = argparse.ArgumentParser(description="Run API tests for the ESP image system.")
parser.add_argument("--esp-ip", type=str, help="IP address of the ESP device (overrides $ESP_IP)")
args = parser.parse_args()

# Resolve ESP IP from command line, environment, or default
ESP_IP = args.esp_ip or os.environ.get("ESP_IP", "192.168.1.123")
base_url = f"http://{ESP_IP}"
spiral_image = "spiral.bmp"

# 1. Get recommended image specs from the device
print(f"Fetching image specs from {base_url}/api/imgspec ...")
try:
    specs_response = requests.get(f"{base_url}/api/imgspec")
    specs_response.raise_for_status()
    specs = specs_response.json()
    width = specs.get("width", 480)
    height = specs.get("height", 320)
    print(f"Using recommended dimensions: {width}x{height}")
except Exception as e:
    print(f"Failed to get image specs. Using default 480x320. Error: {e}")
    width, height = 480, 320

# 2. Generate the spiral BMP image with dynamic resolution
print("Generating spiral image...")
subprocess.run(["python3", "spiral_generator.py",
                "--width", str(width),
                "--height", str(height),
                "--output", spiral_image], check=True)

# 3. Read and encode image
with open(spiral_image, "rb") as img_file:
    bmp_base64 = base64.b64encode(img_file.read()).decode()

# 4. Define test cases
tests = [
    {
        "name": "Upload Spiral Image",
        "method": "POST",
        "url": f"{base_url}/api/img",
        "json": {
            "file": spiral_image,
            "img": bmp_base64
        },
        "expected_status": 200
    },
    {
        "name": "List Images",
        "method": "GET",
        "url": f"{base_url}/api/listimg",
        "expected_status": 200
    },
    {
        "name": "Get Spiral Image",
        "method": "GET",
        "url": f"{base_url}/api/img?file={spiral_image}",
        "expected_status": 200
    },
    {
        "name": "Create Image Chain",
        "method": "POST",
        "url": f"{base_url}/api/imgchain",
        "json": {
            "chain": [spiral_image, spiral_image],
            "fps": 12
        },
        "expected_status": 200
    },
    {
        "name": "Get Index Page",
        "method": "GET",
        "url": f"{base_url}/index.html",
        "expected_status": 200
    },
    {
        "name": "Get Image Specs",
        "method": "GET",
        "url": f"{base_url}/api/imgspec",
        "expected_status": 200
    }
]

# 5. Run tests
results = []

for test in tests:
    try:
        print(f"Running test: {test['name']} ...")
        if test["method"] == "POST":
            response = requests.post(test["url"], json=test.get("json", {}))
        else:
            response = requests.get(test["url"])

        # Try to decode JSON response
        try:
            response_body = response.json()
        except Exception:
            response_body = response.text

        passed = response.status_code == test["expected_status"]

        results.append({
            "Test": test["name"],
            "URL": test["url"],
            "Expected Status": test["expected_status"],
            "Actual Status": response.status_code,
            "Passed": passed,
            "Sent Payload": json.dumps(test.get("json", {})),
            "Response Body": json.dumps(response_body),
            "Error": ""
        })
    except Exception as e:
        results.append({
            "Test": test["name"],
            "URL": test["url"],
            "Expected Status": test["expected_status"],
            "Actual Status": "ERROR",
            "Passed": False,
            "Sent Payload": json.dumps(test.get("json", {})) if "json" in test else "",
            "Response Body": "",
            "Error": str(e)
        })

# 6. Save results
csv_file = "api_test_results.csv"
with open(csv_file, "w", newline='') as file:
    fieldnames = [
        "Test", "URL", "Expected Status", "Actual Status",
        "Passed", "Sent Payload", "Response Body", "Error"
    ]
    writer = csv.DictWriter(file, fieldnames=fieldnames)
    writer.writeheader()
    for row in results:
        writer.writerow(row)

print(f"\n✅ Test results saved to {csv_file}")
