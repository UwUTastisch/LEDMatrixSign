LEDMatrixSign Test Backend

Run the FastAPI test backend which implements a subset of the OpenAPI v2 endpoints and provides a small frontend showing a live LED framebuffer.

Setup

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r api-tests/requirements.txt
```

Run (development)

```bash
# Option A: run directly with python
python3 api-tests/test_backend.py

# Option B: use uvicorn (may require module import path adjustments)
uvicorn api-tests.test_backend:app --reload
```

Open the frontend at: http://127.0.0.1:8000/

Notes
- The server implements `/framebuffer/draw`, `/framebuffer/get`, `/framebuffer/size` and a websocket `/ws`.
- The frontend connects to `/ws` and displays a scaled-up canvas representing the framebuffer.
- The backend is intentionally minimal for testing and can be extended to more closely match every OpenAPI endpoint.
# Script Usage

```
# Using environment variable
export ESP_IP=192.168.1.42
python3 esp_api_test.py
```
Or
```
# Using CLI argument (overrides env)
python3 esp_api_test.py --esp-ip 192.168.1.42
```

View the file 

api_test_results.csv

to check if everything works fine

```
python spiral_generator.py --width 800 --height 600 --output my_spiral.bmp
```