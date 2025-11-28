#!/usr/bin/env python3

from flask import Flask, request, jsonify, send_from_directory, Response
import os
from werkzeug.utils import secure_filename
import base64
import json
import time

app = Flask(__name__)

# -------------------------------------------------------------------
# Configuration
# -------------------------------------------------------------------
# Base temp folder (one place to change)
TMP_BASE_DIR = "/tmp"

IMAGES_DIR = os.path.join(TMP_BASE_DIR, "images")
IMGCHAIN_DIR = os.path.join(TMP_BASE_DIR, "imgchain")
STATIC_DIR = "../webserver"

os.makedirs(IMAGES_DIR, exist_ok=True)
os.makedirs(IMGCHAIN_DIR, exist_ok=True)
os.makedirs(STATIC_DIR, exist_ok=True)

# In-memory storage for image chain (current "playing" chain)
current_img_chain = {
    "chain": [],   # list of filenames like ["1.bmp", "2.bmp", ...]
    "fps": 0.0
}

# Image specifications
IMG_SPECS = {
    "format": "BMP",
    "colorspace": "sRGB",
    "width": 48,
    "height": 48,
    "bitDepth": 24,
    "compression": "none",
    "maxSizeKB": 450
}

# -------------------------------------------------------------------
# /api/img – get / upload single images
# -------------------------------------------------------------------
@app.route('/api/img', methods=['GET'])
def get_image():
    filename = request.args.get('file')
    if not filename:
        return jsonify({"error": "Missing 'file' parameter"}), 400

    safe_filename = secure_filename(filename)
    file_path = os.path.join(IMAGES_DIR, safe_filename)

    if not os.path.isfile(file_path):
        return jsonify({"error": "File not found"}), 404

    with open(file_path, 'rb') as f:
        img_data = f.read()

    return jsonify({
        "img": base64.b64encode(img_data).decode('utf-8'),
        "file": filename
    })


@app.route('/api/img', methods=['POST'])
def upload_image():
    data = request.get_json()
    if not data or 'img' not in data or 'file' not in data:
        return jsonify({"error": "Invalid payload"}), 400

    try:
        img_data = base64.b64decode(data['img'])
    except Exception:
        return jsonify({"error": "Invalid Base64 data"}), 400

    safe_filename = secure_filename(data['file'])
    file_path = os.path.join(IMAGES_DIR, safe_filename)

    try:
        with open(file_path, 'wb') as f:
            f.write(img_data)
    except Exception as e:
        return jsonify({"error": f"File write error: {str(e)}"}), 500

    return jsonify({"file": data['file']})


# -------------------------------------------------------------------
# /api/listimg – list available images
# -------------------------------------------------------------------
@app.route('/api/listimg', methods=['GET'])
def list_images():
    # keeping "page" param to match your original signature, but not using it
    _page = request.args.get('page', default=1, type=int)

    files = [
        f for f in os.listdir(IMAGES_DIR)
        if os.path.isfile(os.path.join(IMAGES_DIR, f))
    ]
    return jsonify({"list": files})


# -------------------------------------------------------------------
# /api/imgchain – set current image chain (POST)
# Mimics ESP POST /api/imgchain { "chain":["1.bmp","2.bmp"], "fps":12.5, ?"num":1 }
# plus a simple GET /api/imgchain/state to inspect the current chain.
# -------------------------------------------------------------------
@app.route('/api/imgchain', methods=['POST'])
def set_image_chain():
    data = request.get_json()
    if not data or 'chain' not in data or 'fps' not in data:
        return jsonify({"error": "Invalid payload"}), 400

    chain = data.get('chain', [])
    fps = data.get('fps', 0)

    if not isinstance(chain, list) or len(chain) == 0:
        return jsonify({"error": "Invalid or empty chain"}), 400

    try:
        fps = float(fps)
    except (TypeError, ValueError):
        return jsonify({"error": "Invalid fps"}), 400

    if fps <= 0:
        return jsonify({"error": "Invalid fps"}), 400

    # Optionally persist chain to a .chain file (just for debugging / similarity)
    # This loosely mimics the ESP behaviour: frameDuration + filenames
    frame_duration = int(1000.0 / fps)
    chain_num = data.get("num")

    if chain_num is not None:
        try:
            chain_num = int(chain_num)
        except (TypeError, ValueError):
            return jsonify({"error": "Invalid num"}), 400
    else:
        # auto-increment: find max existing N in *.chain
        max_num = 0
        for fname in os.listdir(IMGCHAIN_DIR):
            if fname.lower().endswith(".chain"):
                base = fname[:fname.lower().rfind(".chain")]
                try:
                    n = int(base)
                    max_num = max(max_num, n)
                except ValueError:
                    continue
        chain_num = max_num + 1

    chain_path = os.path.join(IMGCHAIN_DIR, f"{chain_num}.chain")

    # If already exists, back it up like ESP code does
    if os.path.exists(chain_path):
        os.replace(chain_path, chain_path + ".bak")

    try:
        with open(chain_path, "w", encoding="utf-8") as f:
            f.write(f"{frame_duration}\n")
            for fn in chain:
                # Store filenames with ".bmp" extension (always)
                base = fn
                if not base.lower().endswith(".bmp"):
                    base += ".bmp"
                f.write(f"{base}\n")
    except Exception as e:
        return jsonify({"error": f"fs write chain: {e}"}), 500

    # Update in-memory "currently playing" chain with full filenames
    current_img_chain["chain"] = chain[:]  # copy list
    current_img_chain["fps"] = fps

    return jsonify({"status": "ok", "chainNum": str(chain_num)})


@app.route('/api/imgchain/state', methods=['GET'])
def get_current_imgchain_state():
    """Small helper to inspect the currently active chain from the browser."""
    return jsonify(current_img_chain)


# -------------------------------------------------------------------
# /api/imgchain?num=<NUMBER> – return raw .chain file content (ESP-like)
# -------------------------------------------------------------------
@app.route('/api/imgchain', methods=['GET'])
def get_imgchain_by_num():
    num = request.args.get("num")
    if not num:
        return jsonify({"error": "missing num"}), 400

    try:
        n = int(num)
    except ValueError:
        return jsonify({"error": "invalid num"}), 400

    chain_path = os.path.join(IMGCHAIN_DIR, f"{n}.chain")
    if not os.path.exists(chain_path):
        return jsonify({"error": "not found"}), 404

    try:
        with open(chain_path, "r", encoding="utf-8") as f:
            content = f.read()
    except Exception as e:
        return jsonify({"error": f"fs read: {e}"}), 500

    lines = content.strip().split('\n')
    # ESP returns JSON like { "chain":["1.bmp","2.bmp",…], "fps":12.5, ?"num":1 }
    # We reconstruct the chain filenames (without .bmp) and parse fps from frame_duration
    if not lines or len(lines) < 2:
        return jsonify({"error": "invalid chain file"}), 500

    try:
        frame_duration = int(lines[0])
        fps = 1000.0 / frame_duration if frame_duration > 0 else 0
    except Exception:
        return jsonify({"error": "invalid frame duration"}), 500

    chain = [line.strip() for line in lines[1:] if line.strip()]
    num = n

    res_json = {"chain": chain, "fps": fps, "num": num}
    res = json.dumps(res_json)
    # text/plain, just like the ESP version
    return Response(res, mimetype="text/plain")


# -------------------------------------------------------------------
# /api/imgspec – image specs
# -------------------------------------------------------------------
@app.route('/api/imgspec', methods=['GET'])
def get_specs():
    return jsonify(IMG_SPECS)


# -------------------------------------------------------------------
# Frontend: index.html / root
# -------------------------------------------------------------------
@app.route('/index.html')
@app.route('/favicon.ico')
@app.route('/')
def serve_frontend():
    # Serve whatever index.html you have in STATIC_DIR
    return send_from_directory(STATIC_DIR, 'index.html')


# -------------------------------------------------------------------
# Livestream endpoint – simple HTML page that shows the animation
# based on the current_img_chain and /api/img.
#
# Open http://<host>:5000/livestream in a browser to see the animation.
# -------------------------------------------------------------------
@app.route('/livestream')
def livestream_page():
    html = """<!doctype html>
        <html lang="en">
        <head>
        <meta charset="utf-8">
        <title>Matrix Livestream</title>
        <style>
            body {
            font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
            background: #111;
            color: #eee;
            text-align: center;
            padding: 2rem;
            }
            #frame {
            image-rendering: pixelated;
            border: 2px solid #444;
            background: #000;
            margin-top: 1rem;
            height: 200px;
            width: 200px;
            }
            #status {
            margin-top: 0.5rem;
            font-size: 0.9rem;
            color: #aaa;
            }
            #error {
            margin-top: 1rem;
            color: #ff6b6b;
            font-weight: 600;
            }
            button {
            margin-top: 1rem;
            padding: 0.4rem 0.8rem;
            border-radius: 4px;
            border: none;
            cursor: pointer;
            }
        </style>
        </head>
        <body>
        <h1>Matrix Livestream</h1>
        <p>Shows frames from the currently configured image chain.</p>
        <div>
            <img id="frame" alt="Matrix Stream" />
        </div>
        <div id="status">Loading chain info…</div>
        <div id="error"></div>
        <button id="reloadChain">Reload chain</button>

        <script>
            let chain = [];
            let fps = 0;
            let idx = 0;
            let timerId = null;

            const statusEl = document.getElementById("status");
            const errorEl = document.getElementById("error");
            const imgEl = document.getElementById("frame");
            const reloadBtn = document.getElementById("reloadChain");

            async function loadChainState() {
            clearInterval(timerId);
            timerId = null;
            statusEl.textContent = "Loading chain info…";
            errorEl.textContent = "";
            try {
                const res = await fetch("/api/imgchain/state");
                if (!res.ok) {
                throw new Error("HTTP " + res.status);
                }
                const data = await res.json();
                chain = data.chain || [];
                fps = Number(data.fps || 0);
                if (!chain.length) {
                statusEl.textContent = "No frames in chain. POST /api/imgchain first.";
                return;
                }
                if (!fps || fps <= 0) {
                fps = 5; // fallback
                }
                statusEl.textContent = "Frames: " + chain.length + " @ " + fps.toFixed(2) + " FPS";
                idx = 0;
                const interval = 1000 / fps;
                timerId = setInterval(showNextFrame, interval);
            } catch (err) {
                console.error(err);
                errorEl.textContent = "Failed to load chain: " + err.message;
            }
            }

            async function showNextFrame() {
            if (!chain.length) return;
            const file = chain[idx];
            idx = (idx + 1) % chain.length;
            try {
                const res = await fetch("/api/img?file=" + encodeURIComponent(file));
                if (!res.ok) throw new Error("HTTP " + res.status);
                const data = await res.json();
                imgEl.src = "data:image/bmp;base64," + data.img;
            } catch (err) {
                console.error(err);
                errorEl.textContent = "Error fetching frame " + file + ": " + err.message;
            }
            }

            reloadBtn.addEventListener("click", loadChainState);

            // Initial load
            loadChainState();
        </script>
        </body>
        </html>
        """
    return Response(html, mimetype="text/html")


# -------------------------------------------------------------------
# Main
# -------------------------------------------------------------------
if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)
