from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.responses import JSONResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
from typing import Optional, Dict, Any
from PIL import Image, ImageDraw, ImageFont
import importlib.util
from pathlib import Path

# load font5x7.py from the api-tests folder (hyphen in folder name prevents normal import)
spec = importlib.util.spec_from_file_location(
    "font5x7",
    str(Path(__file__).resolve().parent / "font5x7.py"),
)
font5x7 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(font5x7)
import io
import base64
import asyncio

app = FastAPI(title="LEDMatrixSign Test Backend")

# Mount simple static frontend
app.mount("/frontend", StaticFiles(directory="./api-tests/frontend"), name="frontend")


class DisplayBrightnessRequest(BaseModel):
    brightness: int


class TextDrawable(BaseModel):
    x: int = 0
    y: int = 0
    text: str
    font: Optional[str] = None
    color: Optional[str] = "#FFFFFF"
    objname: Optional[str] = None


class ClearDirective(BaseModel):
    all: Optional[bool] = False
    obj: Optional[list] = None


class Frame(BaseModel):
    duration: Optional[int] = None
    clear: Optional[ClearDirective] = None
    text: Optional[TextDrawable] = None


class FramebufferServer:
    def __init__(self, width=48, height=48):
        self.width = width
        self.height = height
        # RGBA image for ease of drawing in Pillow
        self.image = Image.new("RGBA", (self.width, self.height), (0, 0, 0, 255))
        self._ws_clients = set()
        self._changed = asyncio.Event()

    def set_size(self, w, h):
        self.width = w
        self.height = h
        self.image = Image.new("RGBA", (w, h), (0, 0, 0, 255))

    def clear(self):
        draw = ImageDraw.Draw(self.image)
        draw.rectangle([(0, 0), (self.width, self.height)], fill=(0, 0, 0, 255))
        self._changed.set()

    def draw_text(self, text: TextDrawable):
        # If font is the 5x bitmap font (name starts with "5x/"), render using bitmap
        color = text.color or "#FFFFFF"
        def parse_color(c):
            if not c:
                return (255, 255, 255, 255)
            c = c.lstrip('#')
            if len(c) == 6:
                r = int(c[0:2], 16)
                g = int(c[2:4], 16)
                b = int(c[4:6], 16)
                return (r, g, b, 255)
            if len(c) == 8:
                r = int(c[0:2], 16)
                g = int(c[2:4], 16)
                b = int(c[4:6], 16)
                a = int(c[6:8], 16)
                return (r, g, b, a)
            return (255, 255, 255, 255)

        col = parse_color(color)
        if text.font and text.font.startswith("5x"):
            # render using pixel font
            glyphs = font5x7.FONT5x7
            fw = glyphs['w']
            fh = glyphs['h']
            x = text.x
            y = text.y
            pixels = self.image.load()
            for ch in text.text:
                glyph = glyphs['glyphs'].get(ch)
                if glyph is None:
                    # advance by fw+1 for unknown glyph
                    x += fw + 1
                    continue
                # glyph is list of column bytes
                for cx, colbyte in enumerate(glyph):
                    for row in range(fh):
                        if (colbyte >> row) & 1:
                            px = x + cx
                            py = y + row
                            if 0 <= px < self.width and 0 <= py < self.height:
                                pixels[px, py] = col
                x += fw + 1
            self._changed.set()
            return

        # fallback to PIL text
        draw = ImageDraw.Draw(self.image)
        try:
            font = ImageFont.load_default()
        except Exception:
            font = None
        draw.text((text.x, text.y), text.text, fill=color, font=font)
        self._changed.set()

    def get_png_base64(self) -> str:
        buf = io.BytesIO()
        self.image.save(buf, format="PNG")
        return base64.b64encode(buf.getvalue()).decode("ascii")

    def get_bgra_base64(self) -> str:
        # Convert RGBA -> BGRA bytes
        rgba = self.image.convert("RGBA")
        raw = rgba.tobytes()
        # reorder bytes
        bgra = bytearray()
        for i in range(0, len(raw), 4):
            r = raw[i]
            g = raw[i + 1]
            b = raw[i + 2]
            a = raw[i + 3]
            bgra.extend([b, g, r, a])
        return base64.b64encode(bytes(bgra)).decode("ascii")

    async def broadcast_loop(self):
        # send updates to connected websockets when changed
        while True:
            await self._changed.wait()
            self._changed.clear()
            png = self.get_png_base64()
            payload = {"png": png, "width": self.width, "height": self.height}
            to_remove = []
            for ws in list(self._ws_clients):
                try:
                    await ws.send_json(payload)
                except Exception:
                    to_remove.append(ws)
            for ws in to_remove:
                self._ws_clients.discard(ws)
            await asyncio.sleep(0)


FB = FramebufferServer()


@app.on_event("startup")
async def startup_event():
    # start broadcaster
    asyncio.create_task(FB.broadcast_loop())


@app.get("/framebuffer/size")
def framebuffer_size():
    return {"width": FB.width, "height": FB.height}


@app.post("/framebuffer/draw")
async def framebuffer_draw(frame: Frame):
    if frame.clear and frame.clear.all:
        FB.clear()
    if frame.text:
        FB.draw_text(frame.text)
    return JSONResponse({"status": "ok"})


@app.get("/framebuffer/get")
def framebuffer_get():
    return {
        "width": FB.width,
        "height": FB.height,
        "format": "BGRA8888",
        "data": FB.get_bgra_base64(),
        "png": FB.get_png_base64(),
    }


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    FB._ws_clients.add(websocket)
    try:
        while True:
            # keep connection alive; client may send pings
            data = await websocket.receive_text()
            # simple protocol: client can request a full update
            if data == "get":
                await websocket.send_json({"png": FB.get_png_base64(), "width": FB.width, "height": FB.height})
    except WebSocketDisconnect:
        FB._ws_clients.discard(websocket)


@app.get("/")
def root():
    html = ("<html><body>Test backend running. Visit "
            "<a href=\"/frontend/index.html\">frontend</a></body></html>")
    return HTMLResponse(html)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8000)
