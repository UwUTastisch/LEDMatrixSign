import json

grid_size_x = 6
grid_size_y = 6
panel_size = 8

panels = []

for x_index in range(grid_size):
    for y_index in range(grid_size):
        panel = {
            "b": False,
            "r": False,
            "v": False,
            "s": False,
            "x": x_index * panel_size,
            "y": y_index * panel_size,
            "h": panel_size,
            "w": panel_size
        }
        panels.append(panel)

data = {"panels": panels}

# Pretty print JSON
print(json.dumps(data, indent=2))
