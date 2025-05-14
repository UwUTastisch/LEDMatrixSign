#!/usr/bin/env python3

from flask import Flask, request, jsonify, send_from_directory, abort
import os
from werkzeug.utils import secure_filename
import base64

app = Flask(__name__)

# Configuration
IMAGES_DIR = '/tmp/images'
STATIC_DIR = '../webserver'
os.makedirs(IMAGES_DIR, exist_ok=True)
os.makedirs(STATIC_DIR, exist_ok=True)

# In-memory storage for image chain
current_img_chain = {'chain': [], 'fps': 0}

# Image specifications
IMG_SPECS = {
    "format": "BMP",
    "colorspace": "sRGB",
    "width": 8,
    "height": 8,
    "bitDepth": 24,
    "compression": "none",
    "maxSizeKB": 450
}

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
    except Exception as e:
        return jsonify({"error": "Invalid Base64 data"}), 400
    
    safe_filename = secure_filename(data['file'])
    file_path = os.path.join(IMAGES_DIR, safe_filename)
    
    try:
        with open(file_path, 'wb') as f:
            f.write(img_data)
    except Exception as e:
        return jsonify({"error": f"File write error: {str(e)}"}), 500
    
    return jsonify({"file": data['file']})

@app.route('/api/listimg', methods=['GET'])
def list_images():
    page = request.args.get('page', default=1, type=int)
    files = [f for f in os.listdir(IMAGES_DIR) 
            if os.path.isfile(os.path.join(IMAGES_DIR, f))]
    return jsonify({"list": files})

@app.route('/api/imgchain', methods=['POST'])
def set_image_chain():
    data = request.get_json()
    if not data or 'chain' not in data or 'fps' not in data:
        return jsonify({"error": "Invalid payload"}), 400
    
    current_img_chain.update({
        "chain": data['chain'],
        "fps": data['fps']
    })
    
    return jsonify({"status": "ok"})

@app.route('/index.html')
@app.route('/')
def serve_frontend():
    return send_from_directory(STATIC_DIR, 'index.html')

@app.route('/api/imgspec', methods=['GET'])
def get_specs():
    return jsonify(IMG_SPECS)

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=True)
